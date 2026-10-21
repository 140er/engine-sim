#include "audio_out.h"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <string>

struct AudioOut::Impl {
    ma_device device;
    bool open = false;
};

namespace {

void playback(ma_device *device, void *output, const void *, ma_uint32 frameCount) {
    auto *player = static_cast<LivePlayer *>(device->pUserData);
    player->render(static_cast<float *>(output), static_cast<int>(frameCount));
}

}  // namespace

AudioOut::AudioOut() : m_impl(new Impl) {}

AudioOut::~AudioOut() {
    close();
    delete m_impl;
}

bool AudioOut::open(LivePlayer *player, std::string *error) {
    close();
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate = static_cast<ma_uint32>(player->outputRate());
    config.dataCallback = playback;
    config.pUserData = player;

    ma_result result = ma_device_init(nullptr, &config, &m_impl->device);
    if (result != MA_SUCCESS) {
        config.sampleRate = 0;
        result = ma_device_init(nullptr, &config, &m_impl->device);
    }
    if (result != MA_SUCCESS) {
        *error = "audio device did not open";
        return false;
    }

    player->setOutputRate(static_cast<int>(m_impl->device.sampleRate));
    result = ma_device_start(&m_impl->device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&m_impl->device);
        *error = "audio device did not start";
        return false;
    }
    m_impl->open = true;
    return true;
}

void AudioOut::close() {
    if (m_impl != nullptr && m_impl->open) {
        ma_device_uninit(&m_impl->device);
        m_impl->open = false;
    }
}
