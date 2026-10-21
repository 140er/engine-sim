#include "audio_out.h"
#include "player.h"
#include "profile.h"
#include "telemetry_io.h"
#include "wav_io.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "process_cpu.h"

namespace {

std::atomic<bool> g_run{true};

#ifdef _WIN32
BOOL WINAPI onCtrl(DWORD) {
    g_run = false;
    return TRUE;
}
#else
void onSignal(int) {
    g_run = false;
}
#endif

void usage() {
    std::cerr
        << "engine-sim-live --profile profile.eslb [mode]\n"
        << "  --demo                 built-in shift replay\n"
        << "  --replay file.csv      columns time_s,rpm,throttle,gear,speed_mps,load\n"
        << "  --source simhub|udp|ac|auto\n"
        << "  --simhub-url http://127.0.0.1:8888/api/getgamedata\n"
        << "  --udp-port 21377\n"
        << "  --wav out.wav          write audio instead of opening a device\n"
        << "  --play                 open the output device\n"
        << "  --realtime             pace to the clock, for a CPU measurement\n"
        << "  --seconds N            stop a live source after N seconds\n"
        << "  --volume 0.8\n"
        << "  --self-check\n";
}

struct Url {
    std::string host;
    int port = 80;
    std::string path;
};

bool parseUrl(const std::string &text, Url *url) {
    const std::string prefix = "http://";
    if (text.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string rest = text.substr(prefix.size());
    const size_t slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    url->path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        url->host = hostport;
        url->port = 80;
    }
    else {
        url->host = hostport.substr(0, colon);
        url->port = std::atoi(hostport.substr(colon + 1).c_str());
    }
    return !url->host.empty() && url->port > 0;
}

bool netStartup() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void netClose(int fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool httpGet(const std::string &urlText, std::string *body, std::string *error) {
    Url url;
    if (!parseUrl(urlText, &url)) {
        *error = "simhub url must start with http://";
        return false;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *info = nullptr;
    const std::string port = std::to_string(url.port);
    if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &info) != 0) {
        *error = "could not resolve " + url.host;
        return false;
    }
    int fd = -1;
    for (addrinfo *it = info; it != nullptr; it = it->ai_next) {
        fd = static_cast<int>(socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (fd < 0) continue;
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
        const int rc = connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen));
#ifdef _WIN32
        const bool inProgress = rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool inProgress = rc != 0 && (errno == EINPROGRESS || errno == EINTR);
#endif
        if (rc != 0 && !inProgress) {
            netClose(fd);
            fd = -1;
            continue;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(fd, &writeSet);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        if (select(fd + 1, nullptr, &writeSet, nullptr, &tv) <= 0) {
            netClose(fd);
            fd = -1;
            continue;
        }
        int soError = 0;
#ifdef _WIN32
        int soLen = sizeof(soError);
#else
        socklen_t soLen = sizeof(soError);
#endif
        getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError), &soLen);
        if (soError != 0) {
            netClose(fd);
            fd = -1;
            continue;
        }
#ifdef _WIN32
        mode = 0;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        fcntl(fd, F_SETFL, flags);
#endif
        break;
    }
    freeaddrinfo(info);
    if (fd < 0) {
        *error = "simhub http connect failed";
        return false;
    }

#ifdef _WIN32
    DWORD timeout = 300;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    timeval recvTv;
    recvTv.tv_sec = 0;
    recvTv.tv_usec = 300000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recvTv, sizeof(recvTv));
#endif

    const std::string request = "GET " + url.path + " HTTP/1.0\r\nHost: " + url.host
        + "\r\nConnection: close\r\n\r\n";
    const char *cursor = request.data();
    int left = static_cast<int>(request.size());
    while (left > 0) {
        const int sent = static_cast<int>(send(fd, cursor, left, 0));
        if (sent <= 0) {
            netClose(fd);
            *error = "simhub http send failed";
            return false;
        }
        cursor += sent;
        left -= sent;
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const int got = static_cast<int>(recv(fd, buffer, sizeof(buffer), 0));
        if (got == 0) break;
        if (got < 0) {
            netClose(fd);
            *error = "simhub http read failed";
            return false;
        }
        response.append(buffer, buffer + got);
        if (response.size() > 2 * 1024 * 1024) break;
    }
    netClose(fd);
    const size_t split = response.find("\r\n\r\n");
    if (split == std::string::npos) {
        *error = "simhub http response has no body";
        return false;
    }
    *body = response.substr(split + 4);
    return true;
}

int bindUdp(int port, std::string *error) {
    const int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (fd < 0) {
        *error = "udp socket failed";
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        netClose(fd);
        *error = "udp bind failed on port " + std::to_string(port);
        return -1;
    }
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    return fd;
}

bool recvUdp(int fd, TelemetryFrame *frame) {
    unsigned char packet[64];
    bool got = false;
    for (;;) {
        const int n = static_cast<int>(recv(fd, reinterpret_cast<char *>(packet), sizeof(packet), 0));
        if (n < 0) break;
        TelemetryFrame parsed;
        if (udpDecode(packet, n, &parsed)) {
            *frame = parsed;
            got = true;
        }
        if (n == 0) break;
    }
    return got;
}

#ifdef _WIN32
struct AcPhysicsHead {
    int packetId;
    float gas;
    float brake;
    float fuel;
    int gear;
    int rpms;
    float steerAngle;
    float speedKmh;
};

bool readAc(TelemetryFrame *frame) {
    static HANDLE handle = nullptr;
    static AcPhysicsHead *view = nullptr;
    if (view == nullptr) {
        handle = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
        if (handle == nullptr) return false;
        view = static_cast<AcPhysicsHead *>(MapViewOfFile(handle, FILE_MAP_READ, 0, 0, sizeof(AcPhysicsHead)));
        if (view == nullptr) {
            CloseHandle(handle);
            handle = nullptr;
            return false;
        }
    }
    frame->throttle = view->gas;
    frame->rpm = view->rpms;
    frame->speedMps = view->speedKmh / 3.6;
    if (view->gear <= 0) frame->gear = -1;
    else if (view->gear == 1) frame->gear = 0;
    else frame->gear = view->gear - 1;
    frame->ignition = view->rpms > 0;
    frame->loadValid = false;
    return true;
}
#endif

struct Stats {
    double sum = 0;
    float peak = 0;
    int count = 0;
    void add(const float *samples, int n) {
        for (int i = 0; i < n; ++i) {
            const float s = samples[i];
            sum += static_cast<double>(s) * s;
            const float a = std::fabs(s);
            if (a > peak) peak = a;
            ++count;
        }
    }
    double rms() const { return count > 0 ? std::sqrt(sum / count) : 0; }
};

}  // namespace

int main(int argc, char **argv) {
    std::string profilePath = "profiles/subaru_ej25.eslb";
    std::string replayPath;
    std::string wavPath;
    std::string simhubUrl = "http://127.0.0.1:8888/api/getgamedata";
    std::string source = "demo";
    int udpPort = 21377;
    double volume = 0.8;
    double seconds = -1;
    bool play = false;
    bool realtime = false;
    bool selfCheck = false;
    bool sourceSet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--profile") profilePath = need("--profile");
        else if (arg == "--demo") source = "demo";
        else if (arg == "--replay") {
            replayPath = need("--replay");
            source = "replay";
        }
        else if (arg == "--source") {
            source = need("--source");
            sourceSet = true;
        }
        else if (arg == "--simhub-url") simhubUrl = need("--simhub-url");
        else if (arg == "--udp-port") udpPort = std::atoi(need("--udp-port").c_str());
        else if (arg == "--wav") wavPath = need("--wav");
        else if (arg == "--play") play = true;
        else if (arg == "--realtime") realtime = true;
        else if (arg == "--seconds") seconds = std::strtod(need("--seconds").c_str(), nullptr);
        else if (arg == "--volume") volume = std::strtod(need("--volume").c_str(), nullptr);
        else if (arg == "--self-check") selfCheck = true;
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

    if (selfCheck) return playerSelfCheck();
    if (!sourceSet && replayPath.empty()) source = "demo";
    if (wavPath.empty() && !realtime) play = true;

    BakedProfile profile;
    std::string error;
    if (!loadProfile(profilePath, &profile, &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (profile.cells.empty()) {
        std::cerr << "profile has no cells\n";
        return 1;
    }

    Replay replay;
    if (source == "replay") {
        if (!loadReplay(replayPath, &replay, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    const bool liveSource = source == "simhub" || source == "udp" || source == "ac" || source == "auto";
    int udpFd = -1;
    if (liveSource && (source == "udp" || source == "auto")) {
        if (!netStartup()) {
            std::cerr << "socket startup failed\n";
            return 1;
        }
        udpFd = bindUdp(udpPort, &error);
        if (udpFd < 0 && source == "udp") {
            std::cerr << error << "\n";
            return 1;
        }
        if (udpFd >= 0) std::cerr << "listening for udp telemetry on " << udpPort << "\n";
    }
    else if (liveSource && source == "simhub") {
        if (!netStartup()) {
            std::cerr << "socket startup failed\n";
            return 1;
        }
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(onCtrl, TRUE);
#else
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);
#endif

    LivePlayer player(&profile);
    player.setVolume(static_cast<float>(volume));
    AudioOut device;
    if (play) {
        if (!device.open(&player, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cerr << "playing at " << player.outputRate() << " Hz\n";
    }

    double duration = demoDuration();
    if (source == "replay") duration = replayDuration(replay);
    if (liveSource) duration = seconds > 0 ? seconds : 1.0e9;
    if (seconds > 0 && !liveSource) duration = std::min(duration, seconds);

    const int block = 256;
    const int rate = player.outputRate();
    std::vector<float> buffer(static_cast<size_t>(block));
    std::vector<float> recorded;
    Stats stats;
    TelemetryFrame liveFrame;
    bool haveLive = false;
    auto lastHttp = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto lastUdp = lastHttp;
    auto lastHttpOk = lastHttp;
    double clock_s = 0;
    double renderCpu = 0;
    const auto wallStart = std::chrono::steady_clock::now();
    const double cpuAtStart = processCpuSeconds();

    while (g_run && clock_s < duration) {
        TelemetryFrame frame;
        if (source == "demo") frame = demoFrame(clock_s);
        else if (source == "replay") frame = replayAt(replay, clock_s);
        else {
            const auto now = std::chrono::steady_clock::now();
            if (udpFd >= 0 && recvUdp(udpFd, &liveFrame)) {
                lastUdp = now;
                haveLive = true;
            }
            const bool wantHttp = source == "simhub" || source == "auto";
            if (wantHttp && std::chrono::duration<double>(now - lastHttp).count() > 0.016) {
                lastHttp = now;
                std::string body;
                std::string httpError;
                TelemetryFrame parsed;
                if (httpGet(simhubUrl, &body, &httpError) && parseSimHubJson(body, &parsed)) {
                    if (source == "simhub"
                        || std::chrono::duration<double>(now - lastUdp).count() > 0.5) {
                        liveFrame = parsed;
                        haveLive = true;
                    }
                    lastHttpOk = now;
                }
            }
#ifdef _WIN32
            if ((source == "ac" || source == "auto") && !haveLive) {
                if (readAc(&liveFrame)) haveLive = true;
            }
            if (source == "ac") {
                TelemetryFrame ac;
                if (readAc(&ac)) {
                    liveFrame = ac;
                    haveLive = true;
                }
            }
#endif
            if (!haveLive) frame.rpm = 0;
            else frame = liveFrame;
            (void)lastHttpOk;
        }

        player.setTelemetry(frame);
        const auto renderStart = std::chrono::steady_clock::now();
        player.render(buffer.data(), block);
        renderCpu += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - renderStart).count();
        stats.add(buffer.data(), block);
        if (!wavPath.empty()) {
            recorded.insert(recorded.end(), buffer.begin(), buffer.end());
        }
        clock_s += static_cast<double>(block) / rate;

        if (realtime || play) {
            const double target = clock_s;
            const double wall = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - wallStart).count();
            if (wall < target) {
                std::this_thread::sleep_for(std::chrono::duration<double>(target - wall));
            }
        }
    }

    if (play) device.close();
    if (udpFd >= 0) netClose(udpFd);

    if (!wavPath.empty()) {
        if (!writeWavMonoFloat(wavPath, rate, recorded.data(), static_cast<int>(recorded.size()), &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cerr << "wrote " << wavPath << "\n";
    }

    const double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wallStart).count();
    std::cout << "audio_seconds=" << clock_s << "\n";
    std::cout << "wall_seconds=" << wall << "\n";
    std::cout << "render_cpu_seconds=" << renderCpu << "\n";
    std::cout << "process_cpu_seconds=" << (processCpuSeconds() - cpuAtStart) << "\n";
    std::cout << "rms=" << stats.rms() << "\n";
    std::cout << "peak=" << stats.peak << "\n";
    if (stats.rms() < 1e-4 && source == "demo") {
        std::cerr << "demo produced silence\n";
        return 1;
    }
    return 0;
}
