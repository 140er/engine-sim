#ifndef ENGINE_SIM_GCC_COMPAT_H
#define ENGINE_SIM_GCC_COMPAT_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER)
#define __forceinline inline
#define __int64 long long
#endif

#endif
