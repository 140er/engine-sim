#ifndef ENGINE_SIM_LIVE_AUDIO_OUT_H
#define ENGINE_SIM_LIVE_AUDIO_OUT_H

#include "player.h"

#include <string>

class AudioOut {
public:
    AudioOut();
    ~AudioOut();

    bool open(LivePlayer *player, std::string *error);
    void close();

private:
    struct Impl;
    Impl *m_impl;
};

#endif
