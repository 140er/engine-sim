#include "player.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr float kSilenceRpm = 450.f;
constexpr double kRpmTau = 0.020;
constexpr double kDemandTau = 0.030;
constexpr double kGainTau = 0.015;

double slew(double current, double target, double tau, double dt) {
    if (tau <= 0) return target;
    const double k = 1.0 - std::exp(-dt / tau);
    return current + (target - current) * k;
}

void bracket(const std::vector<double> &axis, double x, int *i0, int *i1, double *u) {
    if (axis.size() == 1 || x <= axis.front()) {
        *i0 = 0;
        *i1 = 0;
        *u = 0;
        return;
    }
    if (x >= axis.back()) {
        *i0 = static_cast<int>(axis.size()) - 1;
        *i1 = *i0;
        *u = 0;
        return;
    }
    int hi = 1;
    while (hi < static_cast<int>(axis.size()) - 1 && axis[static_cast<size_t>(hi)] < x) ++hi;
    const int lo = hi - 1;
    const double span = axis[static_cast<size_t>(hi)] - axis[static_cast<size_t>(lo)];
    *i0 = lo;
    *i1 = hi;
    *u = span > 0 ? (x - axis[static_cast<size_t>(lo)]) / span : 0;
}

float readLoop(const BakedCell &cell, double phase) {
    const int n = static_cast<int>(cell.samples.size());
    if (n <= 0) return 0;
    if (phase < 0) phase = 0;
    while (phase >= n) phase -= n;
    const int i0 = static_cast<int>(phase);
    int i1 = i0 + 1;
    if (i1 >= n) i1 = 0;
    const float f = static_cast<float>(phase - i0);
    return cell.samples[static_cast<size_t>(i0)] * (1.f - f)
        + cell.samples[static_cast<size_t>(i1)] * f;
}

}  // namespace

LivePlayer::LivePlayer(const BakedProfile *profile)
    : m_profile(profile)
    , m_outputRate(profile != nullptr ? profile->sampleRate : 44100) {}

void LivePlayer::setVolume(float volume) {
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    m_volume = volume;
}

void LivePlayer::setOutputRate(int sampleRate) {
    if (sampleRate >= 8000 && sampleRate <= 192000) m_outputRate = sampleRate;
}

void LivePlayer::setTelemetry(const TelemetryFrame &frame) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_frame = frame;
}

void LivePlayer::updateVoices(double rpm, double demand) {
    struct Want {
        int cell;
        float gain;
    };
    Want wants[4];
    int wantCount = 0;

    const bool audible = rpm >= kSilenceRpm && m_profile != nullptr && !m_profile->cells.empty();
    if (audible) {
        int r0 = 0, r1 = 0, d0 = 0, d1 = 0;
        double ru = 0, du = 0;
        bracket(m_profile->rpmAxis, rpm, &r0, &r1, &ru);
        bracket(m_profile->demandAxis, demand, &d0, &d1, &du);
        const double corners[4][3] = {
            {static_cast<double>(r0), static_cast<double>(d0), (1.0 - ru) * (1.0 - du)},
            {static_cast<double>(r1), static_cast<double>(d0), ru * (1.0 - du)},
            {static_cast<double>(r0), static_cast<double>(d1), (1.0 - ru) * du},
            {static_cast<double>(r1), static_cast<double>(d1), ru * du}
        };
        for (const auto &corner : corners) {
            if (corner[2] < 1e-4) continue;
            const int index = static_cast<int>(corner[0]) * static_cast<int>(m_profile->demandAxis.size())
                + static_cast<int>(corner[1]);
            bool merged = false;
            for (int i = 0; i < wantCount; ++i) {
                if (wants[i].cell == index) {
                    wants[i].gain += static_cast<float>(corner[2]);
                    merged = true;
                    break;
                }
            }
            if (!merged && wantCount < 4) {
                wants[wantCount].cell = index;
                wants[wantCount].gain = static_cast<float>(corner[2]);
                ++wantCount;
            }
        }
    }

    for (Voice &voice : m_voices) {
        if (!voice.used) continue;
        bool still = false;
        for (int i = 0; i < wantCount; ++i) {
            if (wants[i].cell == voice.cell) {
                voice.target = wants[i].gain;
                still = true;
                break;
            }
        }
        if (!still) voice.target = 0;
    }

    for (int i = 0; i < wantCount; ++i) {
        bool found = false;
        for (const Voice &voice : m_voices) {
            if (voice.used && voice.cell == wants[i].cell) {
                found = true;
                break;
            }
        }
        if (found) continue;
        Voice *slot = nullptr;
        for (Voice &voice : m_voices) {
            if (!voice.used) {
                slot = &voice;
                break;
            }
        }
        if (slot == nullptr) {
            for (Voice &voice : m_voices) {
                if (voice.target == 0 && voice.gain < 0.01f) {
                    slot = &voice;
                    break;
                }
            }
        }
        if (slot == nullptr) continue;
        slot->used = true;
        slot->cell = wants[i].cell;
        slot->phase = 0;
        slot->gain = 0;
        slot->target = wants[i].gain;
    }
}

void LivePlayer::render(float *dst, int frames) {
    TelemetryFrame frame;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        frame = m_frame;
    }

    const double sr = static_cast<double>(m_outputRate > 0 ? m_outputRate : 44100);
    const double dt = 1.0 / sr;
    const double loopScale = (m_profile != nullptr && m_profile->sampleRate > 0)
        ? static_cast<double>(m_profile->sampleRate) / sr
        : 1.0;
    const float gainK = static_cast<float>(1.0 - std::exp(-dt / kGainTau));

    double targetRpm = frame.ignition ? frame.rpm : 0;
    if (targetRpm < 0) targetRpm = 0;
    double targetDemand = frame.demand();
    if (!m_primed) {
        m_rpm = targetRpm;
        m_demand = targetDemand;
        m_primed = true;
    }

    for (int i = 0; i < frames; ++i) {
        m_rpm = slew(m_rpm, targetRpm, kRpmTau, dt);
        m_demand = slew(m_demand, targetDemand, kDemandTau, dt);
        if ((i & 31) == 0) updateVoices(m_rpm, m_demand);

        float sum = 0;
        for (Voice &voice : m_voices) {
            if (!voice.used) continue;
            voice.gain += (voice.target - voice.gain) * gainK;
            if (voice.gain < 1e-4f && voice.target == 0) {
                voice.used = false;
                continue;
            }
            if (m_profile == nullptr || voice.cell < 0
                || voice.cell >= static_cast<int>(m_profile->cells.size())) {
                continue;
            }
            const BakedCell &cell = m_profile->cells[static_cast<size_t>(voice.cell)];
            if (cell.samples.empty() || cell.nativeRpm <= 1.0) continue;
            double rate = m_rpm / cell.nativeRpm;
            if (rate < 0.25) rate = 0.25;
            if (rate > 4.0) rate = 4.0;
            sum += voice.gain * readLoop(cell, voice.phase);
            voice.phase += rate * loopScale;
            const double n = static_cast<double>(cell.samples.size());
            while (voice.phase >= n) voice.phase -= n;
        }
        dst[i] = sum * m_volume;
    }
}

int playerSelfCheck() {
    BakedProfile profile;
    profile.sampleRate = 44100;
    profile.name = "self-check";
    profile.rpmAxis = {1000, 3000, 6000};
    profile.demandAxis = {0, 1};

    for (double rpm : profile.rpmAxis) {
        for (double demand : profile.demandAxis) {
            BakedCell cell;
            cell.rpm = rpm;
            cell.demand = demand;
            cell.nativeRpm = rpm;
            const int cycles = std::max(1, static_cast<int>(std::lround(rpm / 30.0)));
            const int n = profile.sampleRate;
            cell.samples.resize(static_cast<size_t>(n));
            const double fundamental = 0.40;
            const double harmonic = 0.18 * demand;
            for (int i = 0; i < n; ++i) {
                const double p = 2.0 * 3.141592653589793 * cycles * static_cast<double>(i) / n;
                cell.samples[static_cast<size_t>(i)] = static_cast<float>(
                    fundamental * std::sin(p) + harmonic * std::sin(2.0 * p));
            }
            profile.cells.push_back(std::move(cell));
        }
    }

    LivePlayer player(&profile);
    player.setVolume(1);
    const int buf = 256;
    const double total = 2.0;
    std::vector<float> audio;
    audio.reserve(static_cast<size_t>(total * profile.sampleRate));
    std::vector<float> block(static_cast<size_t>(buf));
    for (double t = 0; t < total; t += static_cast<double>(buf) / profile.sampleRate) {
        TelemetryFrame frame;
        frame.ignition = true;
        frame.gear = t < 1.0 ? 2 : 3;
        frame.rpm = t < 1.0 ? 3000 : 6000;
        frame.throttle = t < 1.0 ? 0.2 : 1;
        player.setTelemetry(frame);
        player.render(block.data(), buf);
        audio.insert(audio.end(), block.begin(), block.end());
    }

    auto stats = [&](int begin, int end, double *rms, int *crossings, float *maxStep) {
        double energy = 0;
        int n = 0;
        int crosses = 0;
        float step = 0;
        for (int i = begin; i < end; ++i) {
            const float s = audio[static_cast<size_t>(i)];
            energy += static_cast<double>(s) * s;
            ++n;
            if (i > begin) {
                const float prev = audio[static_cast<size_t>(i - 1)];
                const float d = std::fabs(s - prev);
                if (d > step) step = d;
                if ((prev <= 0 && s > 0) || (prev >= 0 && s < 0)) ++crosses;
            }
        }
        *rms = n > 0 ? std::sqrt(energy / n) : 0;
        *crossings = crosses;
        *maxStep = step;
    };

    const int sr = profile.sampleRate;
    double rmsLo = 0, rmsHi = 0;
    int crossLo = 0, crossHi = 0;
    float stepLo = 0, stepHi = 0;
    stats(static_cast<int>(0.25 * sr), static_cast<int>(0.95 * sr), &rmsLo, &crossLo, &stepLo);
    stats(static_cast<int>(1.25 * sr), static_cast<int>(1.95 * sr), &rmsHi, &crossHi, &stepHi);
    (void)stepLo;
    (void)stepHi;

    float maxStep = 0;
    for (size_t i = 1; i < audio.size(); ++i) {
        const float d = std::fabs(audio[i] - audio[i - 1]);
        if (d > maxStep) maxStep = d;
    }

    if (rmsLo < 0.05 || rmsHi < 0.05) {
        std::cerr << "self-check: output is silent\n";
        return 1;
    }
    if (crossHi < crossLo * 1.6) {
        std::cerr << "self-check: pitch did not follow the rpm step ("
                  << crossLo << " vs " << crossHi << " zero crossings)\n";
        return 1;
    }
    if (maxStep > 0.08f) {
        std::cerr << "self-check: sample step " << maxStep
                  << " is a click. A continuous sine at this level stays under 0.04.\n";
        return 1;
    }
    std::cout << "self-check ok rms=" << rmsLo << "," << rmsHi
              << " crossings=" << crossLo << "," << crossHi
              << " max_step=" << maxStep << "\n";
    return 0;
}
