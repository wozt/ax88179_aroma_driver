/*
 * WUMS ships a weak abort() that reports the module name on the fatal
 * screen, but the *reason* is lost (and UDP logs are unreliable this
 * early in boot). This strong override wins the link: the lwIP assert
 * text (stashed by LWIP_PLATFORM_ASSERT) and the caller address go
 * straight onto the OSFatal screen, and the address can be symbolized
 * offline against the .elf.
 */
#include <stdio.h>
#include <stdatomic.h>
#include <coreinit/debug.h>

char ax_last_assert[192];
static atomic_uint progress_mask;

void ax_mark(unsigned bit)
{
    atomic_fetch_or(&progress_mask, 1u << bit);
}

unsigned ax_progress_snapshot(void)
{
    return atomic_load(&progress_mask);
}

void abort(void)
{
    void *ra = __builtin_return_address(0);
    char msg[256];
    if (ax_last_assert[0]) {
        snprintf(msg, sizeof(msg), "AX88179: %.160s\n(caller %p)", ax_last_assert, ra);
    } else {
        snprintf(msg, sizeof(msg), "AX88179 module: abort from %p", ra);
    }
    OSFatal(msg);
    __builtin_unreachable();
}
