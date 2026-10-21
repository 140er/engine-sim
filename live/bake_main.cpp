#include "process_cpu.h"
#include "profile.h"
#include "wav_io.h"

#include "compiler.h"
#include "engine.h"
#include "exhaust_system.h"
#include "ignition_module.h"
#include "simulator.h"
#include "transmission.h"
#include "units.h"
#include "vehicle.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

void usage() {
    std::cerr
        << "engine-sim-bake --script engine.mr --out profile.eslb [--root repo]\n"
        << "  [--rpm 1000,2000,3000,4000,5000,6000] [--demand 0.15,0.55,1]\n"
        << "  [--settle 0.45] [--record 0.40]\n"
        << "  [--benchmark-realtime seconds]\n"
        << "The script must call set_engine, or define public node main that does.\n"
        << "A script that only defines main is wrapped and called.\n"
        << "Pass --root as the repo root so es/ and impulse responses resolve.\n";
}

std::vector<double> parseList(const std::string &text) {
    std::vector<double> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) values.push_back(std::strtod(item.c_str(), nullptr));
    }
    return values;
}

std::string readFile(const std::string &path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string parentPath(const std::string &path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

std::string fileName(const std::string &path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

struct CompiledScript {
    Engine *engine = nullptr;
    Vehicle *vehicle = nullptr;
    Transmission *transmission = nullptr;
};

bool compileScript(
    const std::string &script,
    const std::string &root,
    const std::string &scriptDir,
    CompiledScript *compiled)
{
    es_script::Compiler compiler;
    compiler.initialize();
    compiler.addSearchPath(root + "/es");
    compiler.addSearchPath(scriptDir);
    if (!compiler.compile(script)) {
        std::cerr << "script failed to compile\n" << readFile("error_log.log");
        compiler.destroy();
        return false;
    }
    const es_script::Compiler::Output output = compiler.execute();
    compiler.destroy();
    if (output.engine != nullptr) std::remove("error_log.log");
    compiled->engine = output.engine;
    compiled->vehicle = output.vehicle;
    compiled->transmission = output.transmission;
    return true;
}

void drain(Simulator *sim) {
    std::vector<int16_t> trash(4096);
    for (;;) {
        const int got = sim->readAudioOutput(static_cast<int>(trash.size()), trash.data());
        if (got < static_cast<int>(trash.size())) break;
    }
}

struct Chunk {
    std::vector<int16_t> audio;
    double meanRpm = 0;
};

Chunk simulate(
    Simulator *sim,
    Engine *engine,
    double rpm,
    double demand,
    double seconds,
    bool keep,
    double *physicsSeconds = nullptr,
    double *synthSeconds = nullptr)
{
    sim->m_dyno.m_rotationSpeed = units::rpm(rpm);
    engine->setSpeedControl(demand);
    Chunk chunk;
    std::vector<int16_t> buffer(2048);
    double rpmSum = 0;
    int rpmCount = 0;
    const int frames = std::max(1, static_cast<int>(std::ceil(seconds / 0.02)));
    for (int i = 0; i < frames; ++i) {
        engine->setSpeedControl(demand);
        sim->m_dyno.m_rotationSpeed = units::rpm(rpm);
        const auto physicsStart = std::chrono::steady_clock::now();
        sim->startFrame(0.02);
        while (sim->simulateStep()) {}
        sim->endFrame();
        if (physicsSeconds != nullptr) {
            *physicsSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - physicsStart).count();
        }
        const auto synthStart = std::chrono::steady_clock::now();
        sim->synthesizer().renderAudio();
        if (synthSeconds != nullptr) {
            *synthSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - synthStart).count();
        }
        for (;;) {
            const int got = sim->readAudioOutput(static_cast<int>(buffer.size()), buffer.data());
            if (keep && got > 0) {
                chunk.audio.insert(chunk.audio.end(), buffer.begin(), buffer.begin() + got);
            }
            if (got < static_cast<int>(buffer.size())) break;
        }
        rpmSum += engine->getRpm();
        ++rpmCount;
    }
    chunk.meanRpm = rpmCount > 0 ? rpmSum / rpmCount : rpm;
    return chunk;
}

std::vector<float> makeLoop(const std::vector<int16_t> &recorded, int sampleRate, double rpm) {
    if (recorded.size() < 256 || rpm < 1) return {};
    std::vector<float> samples(recorded.size());
    float peak = 0;
    for (size_t i = 0; i < recorded.size(); ++i) {
        samples[i] = static_cast<float>(recorded[i]) / 32768.f;
        peak = std::max(peak, std::fabs(samples[i]));
    }
    if (peak < 0.01f) return {};

    const int expected = std::max(32, static_cast<int>(std::lround(sampleRate * 120.0 / rpm)));
    int minLag = std::max(32, static_cast<int>(expected * 0.85));
    int maxLag = std::min(static_cast<int>(samples.size()) / 3, static_cast<int>(expected * 1.15));
    if (maxLag <= minLag) {
        minLag = std::max(32, expected / 2);
        maxLag = std::min(static_cast<int>(samples.size()) / 3, expected * 2);
    }
    int bestLag = expected;
    double best = -1e300;
    const int start = std::max(0, static_cast<int>(samples.size()) - expected * 6);
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double corr = 0;
        const int end = static_cast<int>(samples.size()) - lag;
        for (int i = start; i < end; i += 2) {
            corr += static_cast<double>(samples[static_cast<size_t>(i)])
                * samples[static_cast<size_t>(i + lag)];
        }
        if (corr > best) {
            best = corr;
            bestLag = lag;
        }
    }

    const int period = std::max(32, bestLag);
    int fade = std::min(period / 8, sampleRate / 200);
    if (fade < 16) fade = 16;
    int cycles = 4;
    while (cycles > 1 && cycles * period + fade > static_cast<int>(samples.size())) --cycles;
    int body = cycles * period;
    if (body + fade > static_cast<int>(samples.size())) return {};

    const int from = static_cast<int>(samples.size()) - (body + fade);
    std::vector<float> loop(static_cast<size_t>(body));
    for (int i = 0; i < body; ++i) loop[static_cast<size_t>(i)] = samples[static_cast<size_t>(from + i)];
    for (int i = 0; i < fade; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(fade);
        const double enter = std::sin(u * 1.57079632679);
        const double leave = std::cos(u * 1.57079632679);
        const double weight = enter + leave;
        loop[static_cast<size_t>(i)] = static_cast<float>(
            (loop[static_cast<size_t>(i)] * enter
            + samples[static_cast<size_t>(from + body + i)] * leave) / weight);
    }
    return loop;
}

}  // namespace

int main(int argc, char **argv) {
    std::string root = ".";
    std::string script;
    std::string outPath;
    std::string rpmText = "1000,2000,3000,4000,5000,6000";
    std::string demandText = "0.15,0.55,1";
    double settle = 0.45;
    double record = 0.40;
    double benchmarkSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--root") root = need("--root");
        else if (arg == "--script") script = need("--script");
        else if (arg == "--out") outPath = need("--out");
        else if (arg == "--rpm") rpmText = need("--rpm");
        else if (arg == "--demand") demandText = need("--demand");
        else if (arg == "--settle") settle = std::strtod(need("--settle").c_str(), nullptr);
        else if (arg == "--record") record = std::strtod(need("--record").c_str(), nullptr);
        else if (arg == "--benchmark-realtime") {
            benchmarkSeconds = std::strtod(need("--benchmark-realtime").c_str(), nullptr);
        }
        else if (arg == "--help") {
            usage();
            return 0;
        }
        else {
            std::cerr << "unknown argument " << arg << "\n";
            usage();
            return 2;
        }
    }

    if (script.empty() || (benchmarkSeconds <= 0 && outPath.empty())) {
        usage();
        return 2;
    }

    std::srand(1);

    const std::string scriptDir = parentPath(script);
    CompiledScript compiled;
    if (!compileScript(script, root, scriptDir, &compiled)) return 1;
    if (compiled.engine == nullptr) {
        const std::string wrapper = scriptDir + "/.eslb_bake_entry.mr";
        {
            std::ofstream out(wrapper);
            if (!out) {
                std::cerr << "could not write " << wrapper << "\n";
                return 1;
            }
            out << "import \"engine_sim.mr\"\nimport \"" << fileName(script)
                << "\"\nmain()\n";
        }
        std::cerr << "script defines an engine but does not call it; compiling "
                  << wrapper << "\n";
        const bool ok = compileScript(wrapper, root, scriptDir, &compiled);
        std::remove(wrapper.c_str());
        if (!ok) return 1;
    }

    Engine *engine = compiled.engine;
    Vehicle *vehicle = compiled.vehicle;
    Transmission *transmission = compiled.transmission;
    if (engine == nullptr) {
        std::cerr << "script did not call set_engine\n" << readFile("error_log.log");
        return 1;
    }
    if (vehicle == nullptr) {
        Vehicle::Parameters params;
        params.mass = units::mass(1597, units::kg);
        params.diffRatio = 3.42;
        params.tireRadius = units::distance(10, units::inch);
        params.dragCoefficient = 0.25;
        params.crossSectionArea = units::distance(6.0, units::foot) * units::distance(6.0, units::foot);
        params.rollingResistance = 2000.0;
        vehicle = new Vehicle;
        vehicle->initialize(params);
    }
    if (transmission == nullptr) {
        const double gearRatios[] = {2.97, 2.07, 1.43, 1.00, 0.84, 0.56};
        Transmission::Parameters params;
        params.GearCount = 6;
        params.GearRatios = gearRatios;
        params.MaxClutchTorque = units::torque(1000.0, units::ft_lb);
        transmission = new Transmission;
        transmission->initialize(params);
    }

    Simulator *sim = engine->createSimulator(vehicle, transmission);
    sim->setSimulationFrequency(static_cast<int>(engine->getSimulationFrequency()));
    Synthesizer::AudioParameters audio = sim->synthesizer().getAudioParameters();
    audio.inputSampleNoise = static_cast<float>(engine->getInitialJitter());
    audio.airNoise = static_cast<float>(engine->getInitialNoise());
    audio.dF_F_mix = static_cast<float>(engine->getInitialHighFrequencyGain());
    sim->synthesizer().setAudioParameters(audio);

    const int exhaustCount = engine->getExhaustSystemCount();
    if (exhaustCount <= 0) {
        std::cerr << "engine has no exhaust\n";
        return 1;
    }
    for (int i = 0; i < exhaustCount; ++i) {
        ImpulseResponse *impulse = engine->getExhaustSystem(i)->getImpulseResponse();
        if (impulse == nullptr) {
            std::cerr << "exhaust " << i << " has no impulse response\n";
            return 1;
        }
        WavData wav;
        std::string wavError;
        std::string wavPath = impulse->getFilename();
        {
            std::ifstream probe(wavPath, std::ios::binary);
            if (!probe) {
                const std::string prefixed = root + "/" + wavPath;
                std::ifstream alt(prefixed, std::ios::binary);
                if (alt) wavPath = prefixed;
            }
        }
        std::cerr << "impulse " << wavPath << "\n";
        if (!readWavMono16(wavPath, &wav, &wavError)) {
            std::cerr << wavError << "\n";
            return 1;
        }
        sim->synthesizer().initializeImpulseResponse(
            wav.samples.data(),
            static_cast<unsigned int>(wav.samples.size()),
            static_cast<float>(impulse->getVolume()),
            i);
    }

    engine->getIgnitionModule()->m_enabled = true;
    sim->m_dyno.m_enabled = true;
    sim->m_dyno.m_hold = true;
    sim->m_starterMotor.m_enabled = false;
    drain(sim);

    if (benchmarkSeconds > 0) {
        const auto wallStart = std::chrono::steady_clock::now();
        const double cpuAtStart = processCpuSeconds();
        double audioSamples = 0;
        double physicsSeconds = 0;
        double synthSeconds = 0;
        while (true) {
            const double wall = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - wallStart).count();
            if (wall >= benchmarkSeconds) break;
            const auto busyStart = std::chrono::steady_clock::now();
            Chunk chunk = simulate(
                sim, engine, 4000, 0.8, 0.02, true, &physicsSeconds, &synthSeconds);
            audioSamples += static_cast<double>(chunk.audio.size());
            const double busy = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - busyStart).count();
            if (busy < 0.02) {
                std::this_thread::sleep_for(std::chrono::duration<double>(0.02 - busy));
            }
        }
        const double wall = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wallStart).count();
        const double audioSeconds = audioSamples / 44100.0;
        std::cout << "audio_seconds=" << audioSeconds << "\n";
        std::cout << "wall_seconds=" << wall << "\n";
        std::cout << "physics_cpu_seconds=" << physicsSeconds << "\n";
        std::cout << "synth_cpu_seconds=" << synthSeconds << "\n";
        std::cout << "process_cpu_seconds=" << (processCpuSeconds() - cpuAtStart) << "\n";
        std::cout << "mean_rpm=" << engine->getRpm() << "\n";
        if (audioSamples < 44100) {
            std::cerr << "benchmark produced almost no audio\n";
            return 1;
        }
        return 0;
    }

    const std::vector<double> rpms = parseList(rpmText);
    const std::vector<double> demands = parseList(demandText);
    if (rpms.empty() || demands.empty()) {
        std::cerr << "rpm and demand lists are empty\n";
        return 2;
    }

    BakedProfile profile;
    profile.sampleRate = 44100;
    profile.name = engine->getName();
    profile.rpmAxis = rpms;
    profile.demandAxis = demands;

    for (double rpm : rpms) {
        for (double demand : demands) {
            std::cerr << "bake rpm " << rpm << " demand " << demand << "\n";
            double settled = 0;
            Chunk warmup;
            while (settled < 3.0) {
                const double slice = std::min(settle, 3.0 - settled);
                warmup = simulate(sim, engine, rpm, demand, slice, false);
                settled += slice;
                const double miss = std::fabs(warmup.meanRpm - rpm) / rpm;
                if (miss <= 0.15) break;
                std::cerr << "  holding, measured " << warmup.meanRpm << " rpm\n";
            }
            Chunk taken = simulate(sim, engine, rpm, demand, record, true);
            std::vector<float> loop = makeLoop(taken.audio, profile.sampleRate, taken.meanRpm);
            if (loop.empty()) {
                std::cerr << "cell rpm " << rpm << " demand " << demand
                          << " produced no loop (measured " << taken.meanRpm << " rpm, "
                          << taken.audio.size() << " samples)\n";
                return 1;
            }
            BakedCell cell;
            cell.rpm = rpm;
            cell.demand = demand;
            cell.nativeRpm = taken.meanRpm > 1 ? taken.meanRpm : rpm;
            cell.samples = std::move(loop);
            float peak = 0;
            for (float sample : cell.samples) peak = std::max(peak, std::fabs(sample));
            std::cerr << "  loop " << cell.samples.size() << " samples, native rpm "
                      << cell.nativeRpm << ", peak " << peak << "\n";
            profile.cells.push_back(std::move(cell));
        }
    }

    std::string error;
    if (!saveProfile(profile, outPath, &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "wrote " << outPath << " name=" << profile.name
              << " cells=" << profile.cells.size() << "\n";
    return 0;
}
