#ifndef ENGINE_SIM_LIVE_TELEMETRY_H
#define ENGINE_SIM_LIVE_TELEMETRY_H

struct TelemetryFrame {
    double rpm = 0;
    double throttle = 0;
    double load = 0;
    double speedMps = 0;
    int gear = 0;
    bool loadValid = false;
    bool ignition = true;

    double demand() const {
        double d = loadValid ? load : throttle;
        if (d < 0) return 0;
        if (d > 1) return 1;
        return d;
    }
};

#endif
