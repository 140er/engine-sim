#include "profile.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

void putU32(std::ostream &out, uint32_t v) {
    unsigned char b[4] = {
        static_cast<unsigned char>(v & 255u),
        static_cast<unsigned char>((v >> 8) & 255u),
        static_cast<unsigned char>((v >> 16) & 255u),
        static_cast<unsigned char>((v >> 24) & 255u)
    };
    out.write(reinterpret_cast<const char *>(b), 4);
}

void putF64(std::ostream &out, double v) {
    static_assert(sizeof(double) == 8, "profile file assumes IEEE-754 binary64");
    unsigned char b[8];
    std::memcpy(b, &v, 8);
    out.write(reinterpret_cast<const char *>(b), 8);
}

bool getExact(std::istream &in, void *dst, std::streamsize n) {
    in.read(reinterpret_cast<char *>(dst), n);
    return in.good() && in.gcount() == n;
}

bool getU32(std::istream &in, uint32_t *v) {
    unsigned char b[4];
    if (!getExact(in, b, 4)) return false;
    *v = static_cast<uint32_t>(b[0])
        | (static_cast<uint32_t>(b[1]) << 8)
        | (static_cast<uint32_t>(b[2]) << 16)
        | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool getF64(std::istream &in, double *v) {
    unsigned char b[8];
    if (!getExact(in, b, 8)) return false;
    std::memcpy(v, b, 8);
    return true;
}

}  // namespace

bool saveProfile(const BakedProfile &profile, const std::string &path, std::string *error) {
    if (profile.rpmAxis.empty() || profile.demandAxis.empty()) {
        *error = "profile has an empty rpm or demand axis";
        return false;
    }
    if (profile.cells.size() != profile.rpmAxis.size() * profile.demandAxis.size()) {
        *error = "profile cell count does not match the axes";
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        *error = "could not write " + path;
        return false;
    }

    out.write("ESLB", 4);
    putU32(out, 1);
    putU32(out, static_cast<uint32_t>(profile.sampleRate));
    putU32(out, static_cast<uint32_t>(profile.rpmAxis.size()));
    putU32(out, static_cast<uint32_t>(profile.demandAxis.size()));
    putU32(out, static_cast<uint32_t>(profile.name.size()));
    out.write(profile.name.data(), static_cast<std::streamsize>(profile.name.size()));

    for (double rpm : profile.rpmAxis) putF64(out, rpm);
    for (double demand : profile.demandAxis) putF64(out, demand);

    for (const BakedCell &cell : profile.cells) {
        putF64(out, cell.rpm);
        putF64(out, cell.demand);
        putF64(out, cell.nativeRpm);
        putU32(out, static_cast<uint32_t>(cell.samples.size()));
        for (float sample : cell.samples) {
            float clamped = sample;
            if (clamped > 1.f) clamped = 1.f;
            if (clamped < -1.f) clamped = -1.f;
            const int rounded = static_cast<int>(clamped * 32767.f);
            const int16_t pcm = static_cast<int16_t>(rounded);
            unsigned char b[2] = {
                static_cast<unsigned char>(static_cast<uint16_t>(pcm) & 255u),
                static_cast<unsigned char>((static_cast<uint16_t>(pcm) >> 8) & 255u)
            };
            out.write(reinterpret_cast<const char *>(b), 2);
        }
    }

    if (!out.good()) {
        *error = "write failed for " + path;
        return false;
    }
    return true;
}

bool loadProfile(const std::string &path, BakedProfile *profile, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "could not read " + path;
        return false;
    }

    char magic[4];
    if (!getExact(in, magic, 4) || std::memcmp(magic, "ESLB", 4) != 0) {
        *error = path + " is not an engine-sim live profile";
        return false;
    }

    uint32_t version = 0;
    uint32_t sampleRate = 0;
    uint32_t rpmCount = 0;
    uint32_t demandCount = 0;
    uint32_t nameLen = 0;
    if (!getU32(in, &version) || !getU32(in, &sampleRate) || !getU32(in, &rpmCount)
        || !getU32(in, &demandCount) || !getU32(in, &nameLen)) {
        *error = "truncated profile header";
        return false;
    }
    if (version != 1 || sampleRate < 8000 || sampleRate > 192000 || rpmCount == 0
        || rpmCount > 256 || demandCount == 0 || demandCount > 64 || nameLen > 1024) {
        *error = "unsupported profile header";
        return false;
    }

    profile->sampleRate = static_cast<int>(sampleRate);
    profile->name.assign(nameLen, '\0');
    if (nameLen > 0 && !getExact(in, profile->name.data(), nameLen)) {
        *error = "truncated profile name";
        return false;
    }

    profile->rpmAxis.resize(rpmCount);
    profile->demandAxis.resize(demandCount);
    for (uint32_t i = 0; i < rpmCount; ++i) {
        if (!getF64(in, &profile->rpmAxis[i])) {
            *error = "truncated rpm axis";
            return false;
        }
    }
    for (uint32_t i = 0; i < demandCount; ++i) {
        if (!getF64(in, &profile->demandAxis[i])) {
            *error = "truncated demand axis";
            return false;
        }
    }

    const uint32_t cellCount = rpmCount * demandCount;
    profile->cells.resize(cellCount);
    for (uint32_t c = 0; c < cellCount; ++c) {
        BakedCell &cell = profile->cells[c];
        uint32_t sampleCount = 0;
        if (!getF64(in, &cell.rpm) || !getF64(in, &cell.demand) || !getF64(in, &cell.nativeRpm)
            || !getU32(in, &sampleCount)) {
            *error = "truncated cell header";
            return false;
        }
        if (sampleCount == 0 || sampleCount > 44100u * 5u) {
            *error = "cell length is out of range";
            return false;
        }
        cell.samples.resize(sampleCount);
        for (uint32_t s = 0; s < sampleCount; ++s) {
            unsigned char b[2];
            if (!getExact(in, b, 2)) {
                *error = "truncated cell audio";
                return false;
            }
            const uint16_t u = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
            const int16_t pcm = static_cast<int16_t>(u);
            cell.samples[s] = static_cast<float>(pcm) / 32768.f;
        }
    }
    return true;
}
