#ifndef ENGINE_SIM_LIVE_PROFILE_H
#define ENGINE_SIM_LIVE_PROFILE_H

#include <string>
#include <vector>

struct BakedCell {
    double rpm = 0;
    double demand = 0;
    double nativeRpm = 0;
    std::vector<float> samples;
};

struct BakedProfile {
    int sampleRate = 44100;
    std::string name;
    std::vector<double> rpmAxis;
    std::vector<double> demandAxis;
    std::vector<BakedCell> cells;

    const BakedCell *find(int rpmIndex, int demandIndex) const {
        const int i = rpmIndex * static_cast<int>(demandAxis.size()) + demandIndex;
        if (i < 0 || i >= static_cast<int>(cells.size())) return nullptr;
        return &cells[static_cast<size_t>(i)];
    }
};

bool saveProfile(const BakedProfile &profile, const std::string &path, std::string *error);
bool loadProfile(const std::string &path, BakedProfile *profile, std::string *error);

#endif
