#ifndef AX_LWIP_CC_H
#define AX_LWIP_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <whb/log.h>
/* lwIP asserts stash their text for debug_abort.c's abort() override so
 * the fatal screen names the failing assertion; diagnostics go to the
 * UDP log. */
extern char ax_last_assert[192];
#define LWIP_PLATFORM_DIAG(x)  do { WHBLogPrintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    snprintf(ax_last_assert, 192, "%s (%s:%d)", x, __FILE__, __LINE__); \
    abort(); \
} while (0)
/* Route lwIP errno through newlib's per-thread errno instead of lwIP's
 * own global. */
#define LWIP_ERRNO_STDINCLUDE
#undef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#define LWIP_RAND() ((uint32_t)rand())
#endif
