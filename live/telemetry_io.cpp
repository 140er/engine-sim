#include "telemetry_io.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitCsv(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            out.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    out.push_back(trim(cur));
    return out;
}

const char *findKey(const std::string &body, const char *key) {
    const size_t keyLen = std::strlen(key);
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t found = body.find(key, pos);
        if (found == std::string::npos) return nullptr;
        if (found == 0) {
            pos = 1;
            continue;
        }
        const char before = body[found - 1];
        const size_t end = found + keyLen;
        if ((before == '"' || before == '.') && end < body.size() && body[end] == '"') {
            return body.c_str() + end + 1;
        }
        pos = found + 1;
    }
    return nullptr;
}

void skipValueLead(const char *&p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') ++p;
}

bool parseNumber(const char *p, double *out) {
    skipValueLead(p);
    if (*p == '"') ++p;
    char *end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

bool parseGear(const char *p, int *gear) {
    skipValueLead(p);
    if (*p == '"') {
        ++p;
        if (*p == 'R' || *p == 'r') {
            *gear = -1;
            return true;
        }
        if (*p == 'N' || *p == 'n') {
            *gear = 0;
            return true;
        }
        char *end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) return false;
        *gear = static_cast<int>(v);
        return true;
    }
    char *end = nullptr;
    const long v = std::strtol(p, &end, 10);
    if (end == p) return false;
    *gear = static_cast<int>(v);
    return true;
}

double lerp(double a, double b, double u) {
    return a + (b - a) * u;
}

}  // namespace

bool loadReplay(const std::string &path, Replay *replay, std::string *error) {
    std::ifstream in(path);
    if (!in) {
        *error = "could not read " + path;
        return false;
    }
    std::string header;
    if (!std::getline(in, header)) {
        *error = "replay is empty";
        return false;
    }
    const auto columns = splitCsv(header);
    int timeCol = -1, rpmCol = -1, throttleCol = -1, gearCol = -1, speedCol = -1, loadCol = -1;
    for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
        const std::string &c = columns[static_cast<size_t>(i)];
        if (c == "time_s") timeCol = i;
        else if (c == "rpm") rpmCol = i;
        else if (c == "throttle") throttleCol = i;
        else if (c == "gear") gearCol = i;
        else if (c == "speed_mps") speedCol = i;
        else if (c == "load") loadCol = i;
    }
    if (timeCol < 0 || rpmCol < 0 || throttleCol < 0) {
        *error = "replay needs columns time_s,rpm,throttle";
        return false;
    }

    std::string line;
    double lastT = -1;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        const auto fields = splitCsv(line);
        auto field = [&](int col) -> std::string {
            if (col < 0 || col >= static_cast<int>(fields.size())) return "";
            return fields[static_cast<size_t>(col)];
        };
        TelemetryFrame frame;
        const double t = std::strtod(field(timeCol).c_str(), nullptr);
        if (t < lastT) {
            *error = "replay times go backwards";
            return false;
        }
        lastT = t;
        frame.rpm = std::strtod(field(rpmCol).c_str(), nullptr);
        frame.throttle = std::strtod(field(throttleCol).c_str(), nullptr);
        if (!field(gearCol).empty()) frame.gear = std::atoi(field(gearCol).c_str());
        if (!field(speedCol).empty()) frame.speedMps = std::strtod(field(speedCol).c_str(), nullptr);
        if (!field(loadCol).empty()) {
            frame.load = std::strtod(field(loadCol).c_str(), nullptr);
            frame.loadValid = true;
        }
        frame.ignition = true;
        replay->times.push_back(t);
        replay->frames.push_back(frame);
    }
    if (replay->times.size() < 2) {
        *error = "replay needs at least two rows";
        return false;
    }
    return true;
}

TelemetryFrame replayAt(const Replay &replay, double t) {
    if (replay.frames.empty()) return {};
    if (t <= replay.times.front()) return replay.frames.front();
    if (t >= replay.times.back()) return replay.frames.back();
    size_t hi = 1;
    while (hi + 1 < replay.times.size() && replay.times[hi] < t) ++hi;
    const size_t lo = hi - 1;
    const double span = replay.times[hi] - replay.times[lo];
    const double u = span > 0 ? (t - replay.times[lo]) / span : 0;
    TelemetryFrame frame = replay.frames[lo];
    const TelemetryFrame &next = replay.frames[hi];
    frame.rpm = lerp(replay.frames[lo].rpm, next.rpm, u);
    frame.throttle = lerp(replay.frames[lo].throttle, next.throttle, u);
    frame.speedMps = lerp(replay.frames[lo].speedMps, next.speedMps, u);
    if (replay.frames[lo].loadValid && next.loadValid) {
        frame.load = lerp(replay.frames[lo].load, next.load, u);
        frame.loadValid = true;
    }
    frame.gear = replay.frames[lo].gear;
    frame.ignition = true;
    return frame;
}

double replayDuration(const Replay &replay) {
    if (replay.times.empty()) return 0;
    return replay.times.back();
}

bool parseSimHubJson(const std::string &body, TelemetryFrame *frame) {
    TelemetryFrame parsed;
    double rpm = 0, throttle = 0, speed = 0, load = 0;
    const char *rpmAt = findKey(body, "Rpms");
    if (rpmAt == nullptr || !parseNumber(rpmAt, &rpm)) return false;
    parsed.rpm = rpm;
    parsed.ignition = rpm > 1.0;

    if (const char *throttleAt = findKey(body, "Throttle")) {
        if (parseNumber(throttleAt, &throttle)) {
            if (throttle > 1.0) throttle /= 100.0;
            parsed.throttle = throttle;
        }
    }
    if (const char *gearAt = findKey(body, "Gear")) {
        int gear = 0;
        if (parseGear(gearAt, &gear)) parsed.gear = gear;
    }
    if (const char *speedAt = findKey(body, "SpeedKmh")) {
        if (parseNumber(speedAt, &speed)) parsed.speedMps = speed / 3.6;
    }
    if (const char *loadAt = findKey(body, "EngineLoad")) {
        if (parseNumber(loadAt, &load)) {
            if (load > 1.0) load /= 100.0;
            if (load < 0) load = 0;
            if (load > 1) load = 1;
            parsed.load = load;
            parsed.loadValid = true;
        }
    }
    *frame = parsed;
    return true;
}

bool udpEncode(const TelemetryFrame &frame, unsigned char out[24]) {
    out[0] = 'E';
    out[1] = 'S';
    out[2] = 'T';
    out[3] = '1';
    out[4] = 1;
    unsigned char flags = 0;
    if (frame.ignition) flags |= 1u;
    if (frame.loadValid) flags |= 2u;
    out[5] = flags;
    const int16_t gear = static_cast<int16_t>(frame.gear);
    const uint16_t gearBits = static_cast<uint16_t>(gear);
    out[6] = static_cast<unsigned char>(gearBits & 255u);
    out[7] = static_cast<unsigned char>((gearBits >> 8) & 255u);
    auto putF = [&](int offset, float v) {
        static_assert(sizeof(float) == 4, "udp packet assumes IEEE-754 binary32");
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        for (int i = 0; i < 4; ++i) out[offset + i] = b[i];
    };
    putF(8, static_cast<float>(frame.rpm));
    putF(12, static_cast<float>(frame.throttle));
    putF(16, static_cast<float>(frame.load));
    putF(20, static_cast<float>(frame.speedMps));
    return true;
}

bool udpDecode(const unsigned char *data, int size, TelemetryFrame *frame) {
    if (size < 24) return false;
    if (data[0] != 'E' || data[1] != 'S' || data[2] != 'T' || data[3] != '1') return false;
    if (data[4] != 1) return false;
    TelemetryFrame parsed;
    parsed.ignition = (data[5] & 1u) != 0;
    parsed.loadValid = (data[5] & 2u) != 0;
    const uint16_t gearBits = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
    parsed.gear = static_cast<int16_t>(gearBits);
    auto getF = [&](int offset) {
        float v = 0;
        std::memcpy(&v, data + offset, 4);
        return static_cast<double>(v);
    };
    parsed.rpm = getF(8);
    parsed.throttle = getF(12);
    parsed.load = getF(16);
    parsed.speedMps = getF(20);
    if (parsed.throttle < 0) parsed.throttle = 0;
    if (parsed.throttle > 1) parsed.throttle = 1;
    if (parsed.load < 0) parsed.load = 0;
    if (parsed.load > 1) parsed.load = 1;
    *frame = parsed;
    return true;
}

double demoDuration() {
    return 8.0;
}

TelemetryFrame demoFrame(double t) {
    struct Knot {
        double t;
        double rpm;
        double throttle;
        int gear;
        double speed;
    };
    const Knot knots[] = {
        {0.0, 950, 0.12, 1, 5},
        {1.0, 980, 0.15, 1, 8},
        {1.15, 1500, 1.0, 1, 12},
        {3.0, 6200, 1.0, 1, 28},
        {3.001, 3900, 1.0, 2, 28},
        {4.8, 6400, 1.0, 2, 46},
        {4.801, 4200, 1.0, 3, 46},
        {6.2, 6100, 1.0, 3, 62},
        {6.201, 6100, 0.05, 3, 62},
        {7.5, 1600, 0.08, 3, 40},
        {8.0, 1050, 0.12, 2, 22}
    };
    const int n = static_cast<int>(sizeof(knots) / sizeof(knots[0]));
    if (t <= knots[0].t) {
        TelemetryFrame frame;
        frame.rpm = knots[0].rpm;
        frame.throttle = knots[0].throttle;
        frame.gear = knots[0].gear;
        frame.speedMps = knots[0].speed;
        return frame;
    }
    int hi = 1;
    while (hi < n - 1 && knots[hi].t < t) ++hi;
    const Knot &a = knots[hi - 1];
    const Knot &b = knots[hi];
    const double span = b.t - a.t;
    const double u = span > 1e-6 ? (t - a.t) / span : 1;
    TelemetryFrame frame;
    frame.rpm = lerp(a.rpm, b.rpm, u);
    frame.throttle = lerp(a.throttle, b.throttle, u);
    frame.speedMps = lerp(a.speed, b.speed, u);
    frame.gear = a.gear;
    frame.ignition = true;
    return frame;
}
