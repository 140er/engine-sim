#ifndef ENGINE_SIM_LIVE_TELEMETRY_IO_H
#define ENGINE_SIM_LIVE_TELEMETRY_IO_H

#include "telemetry.h"

#include <string>
#include <utility>
#include <vector>

struct Replay {
    std::vector<double> times;
    std::vector<TelemetryFrame> frames;
};

bool loadReplay(const std::string &path, Replay *replay, std::string *error);
TelemetryFrame replayAt(const Replay &replay, double t);
double replayDuration(const Replay &replay);

bool parseSimHubJson(const std::string &body, TelemetryFrame *frame);

bool udpEncode(const TelemetryFrame &frame, unsigned char out[24]);
bool udpDecode(const unsigned char *data, int size, TelemetryFrame *frame);

double demoDuration();
TelemetryFrame demoFrame(double t);

#endif
