#include "wav_io.h"

#include <cstring>
#include <fstream>
#include <cstdint>

namespace {

bool readU32(std::istream &in, uint32_t *v) {
    unsigned char b[4];
    in.read(reinterpret_cast<char *>(b), 4);
    if (!in) return false;
    *v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
        | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool readU16(std::istream &in, uint16_t *v) {
    unsigned char b[2];
    in.read(reinterpret_cast<char *>(b), 2);
    if (!in) return false;
    *v = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
    return true;
}

void writeU32(std::ostream &out, uint32_t v) {
    unsigned char b[4] = {
        static_cast<unsigned char>(v & 255u),
        static_cast<unsigned char>((v >> 8) & 255u),
        static_cast<unsigned char>((v >> 16) & 255u),
        static_cast<unsigned char>((v >> 24) & 255u)
    };
    out.write(reinterpret_cast<const char *>(b), 4);
}

void writeU16(std::ostream &out, uint16_t v) {
    unsigned char b[2] = {
        static_cast<unsigned char>(v & 255u),
        static_cast<unsigned char>((v >> 8) & 255u)
    };
    out.write(reinterpret_cast<const char *>(b), 2);
}

}  // namespace

bool readWavMono16(const std::string &path, WavData *out, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "could not read " + path;
        return false;
    }

    char riff[4];
    uint32_t riffSize = 0;
    char wave[4];
    in.read(riff, 4);
    if (!readU32(in, &riffSize) || !in.read(wave, 4) || std::memcmp(riff, "RIFF", 4) != 0
        || std::memcmp(wave, "WAVE", 4) != 0) {
        *error = path + " is not a RIFF WAVE file";
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bits = 0;
    bool haveFmt = false;
    std::vector<unsigned char> data;

    while (in && !in.eof()) {
        char id[4];
        uint32_t size = 0;
        if (!in.read(id, 4)) break;
        if (!readU32(in, &size)) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            if (!readU16(in, &audioFormat) || !readU16(in, &channels) || !readU32(in, &sampleRate)
                || !readU32(in, &byteRate) || !readU16(in, &blockAlign) || !readU16(in, &bits)) {
                *error = "truncated fmt chunk in " + path;
                return false;
            }
            haveFmt = true;
            if (size > 16) in.seekg(size - 16, std::ios::cur);
        }
        else if (std::memcmp(id, "data", 4) == 0) {
            data.resize(size);
            if (size > 0) in.read(reinterpret_cast<char *>(data.data()), size);
            if (!in) {
                *error = "truncated data chunk in " + path;
                return false;
            }
        }
        else {
            in.seekg(size, std::ios::cur);
        }
        if (size & 1u) in.seekg(1, std::ios::cur);
    }

    if (!haveFmt || data.empty()) {
        *error = path + " is missing fmt or data";
        return false;
    }
    if (audioFormat != 1 || bits != 16 || (channels != 1 && channels != 2)) {
        *error = path + " must be 16-bit PCM, mono or stereo";
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(channels) * 2u;
    const size_t frames = data.size() / frameBytes;
    out->sampleRate = static_cast<int>(sampleRate);
    out->samples.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const unsigned char *p = data.data() + i * frameBytes;
        auto sampleAt = [](const unsigned char *q) {
            const uint16_t u = static_cast<uint16_t>(q[0]) | (static_cast<uint16_t>(q[1]) << 8);
            return static_cast<int>(static_cast<int16_t>(u));
        };
        int mixed = sampleAt(p);
        if (channels == 2) mixed = (mixed + sampleAt(p + 2)) / 2;
        out->samples[i] = static_cast<int16_t>(mixed);
    }
    return true;
}

bool writeWavMono16(const std::string &path, int sampleRate, const int16_t *samples, int count, std::string *error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        *error = "could not write " + path;
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(count) * 2u;
    const uint32_t riffSize = 36u + dataBytes;
    out.write("RIFF", 4);
    writeU32(out, riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, 1);
    writeU32(out, static_cast<uint32_t>(sampleRate));
    writeU32(out, static_cast<uint32_t>(sampleRate) * 2u);
    writeU16(out, 2);
    writeU16(out, 16);
    out.write("data", 4);
    writeU32(out, dataBytes);
    for (int i = 0; i < count; ++i) {
        const uint16_t u = static_cast<uint16_t>(samples[i]);
        writeU16(out, u);
    }
    if (!out.good()) {
        *error = "write failed for " + path;
        return false;
    }
    return true;
}

bool writeWavMonoFloat(const std::string &path, int sampleRate, const float *samples, int count, std::string *error) {
    std::vector<int16_t> pcm(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        float s = samples[i];
        if (s > 1.f) s = 1.f;
        if (s < -1.f) s = -1.f;
        pcm[static_cast<size_t>(i)] = static_cast<int16_t>(s * 32767.f);
    }
    return writeWavMono16(path, sampleRate, pcm.data(), count, error);
}
