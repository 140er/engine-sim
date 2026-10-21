#ifndef ENGINE_SIM_LIVE_PROCESS_CPU_H
#define ENGINE_SIM_LIVE_PROCESS_CPU_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

inline double processCpuSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) * 1e-7;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    return static_cast<double>(usage.ru_utime.tv_sec) + usage.ru_utime.tv_usec * 1e-6
        + static_cast<double>(usage.ru_stime.tv_sec) + usage.ru_stime.tv_usec * 1e-6;
#endif
}

#endif
