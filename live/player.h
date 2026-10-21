#ifndef ENGINE_SIM_LIVE_PLAYER_H
#define ENGINE_SIM_LIVE_PLAYER_H

#include "profile.h"
#include "telemetry.h"

#include <mutex>

class LivePlayer {
public:
    explicit LivePlayer(const BakedProfile *profile);

    void setVolume(float volume);
    void setOutputRate(int sampleRate);
    void setTelemetry(const TelemetryFrame &frame);
    void render(float *dst, int frames);

    int outputRate() const { return m_outputRate; }

private:
    struct Voice {
        int cell = -1;
        double phase = 0;
        float gain = 0;
        float target = 0;
        bool used = false;
    };

    void updateVoices(double rpm, double demand);

    const BakedProfile *m_profile;
    float m_volume = 0.8f;
    int m_outputRate = 44100;
    bool m_primed = false;
    double m_rpm = 0;
    double m_demand = 0;
    TelemetryFrame m_frame;
    std::mutex m_mu;
    Voice m_voices[8];
};

int playerSelfCheck();

#endif
