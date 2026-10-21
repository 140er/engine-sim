#ifndef ENGINE_SIM_LIVE_WAV_IO_H
#define ENGINE_SIM_LIVE_WAV_IO_H

#include <cstdint>
#include <string>
#include <vector>

struct WavData {
    int sampleRate = 0;
    std::vector<int16_t> samples;
};

bool readWavMono16(const std::string &path, WavData *out, std::string *error);
bool writeWavMono16(const std::string &path, int sampleRate, const int16_t *samples, int count, std::string *error);
bool writeWavMonoFloat(const std::string &path, int sampleRate, const float *samples, int count, std::string *error);

#endif
