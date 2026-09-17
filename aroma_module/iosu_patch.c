#include "iosu_patch.h"

#include <mocha/mocha.h>
#include <whb/log.h>

/*
 * The one instruction that stands between us and the adapter.
 *
 * IOSU's UHS endpoint-administration worker (ioctl 0x0B) guards
 * ownership with a conditional branch: an interface an IOSU driver has
 * probed will not enable its endpoints for a Cafe OS client. The whole
 * of this project's dead ends came down to that branch. Disassembled,
 * it is a single `beq` at 0x10114338 -- 0x0A000019 -- and turning it
 * into a NOP makes the check fall through and the enable succeed, which
 * the console's own log then confirms with "Enable endpoints".
 *
 * The write is to IOSU kernel memory through Mocha, so it does not
 * survive a reboot. That is exactly why it belongs in the module rather
 * than in a separate app run by hand: the module reapplies it every
 * boot, before it touches the adapter, so the thing "just works" from a
 * cold start.
 */
#define GUARD_ADDR   0x10114338u
#define GUARD_BEQ    0x0A000019u   /* the original branch */
#define GUARD_NOP    0xE1A00000u   /* mov r0, r0 */

int iosu_patch_apply(char *why, unsigned why_size)
{
    if (Mocha_InitLibrary() != MOCHA_RESULT_SUCCESS) {
        snprintf(why, why_size, "Mocha unavailable (is the environment's mocha loaded?)");
        return -1;
    }

    int rc = -1;
    uint32_t value = 0;
    if (Mocha_IOSUKernelRead32(GUARD_ADDR, &value) != MOCHA_RESULT_SUCCESS) {
        snprintf(why, why_size, "cannot read IOSU 0x%08X", GUARD_ADDR);
    } else if (value == GUARD_NOP) {
        /* Already patched this boot, or by the standalone app. A NOP
         * stays a NOP -- nothing to do, and reporting success keeps the
         * worker from treating a re-run as a failure. */
        snprintf(why, why_size, "already applied");
        rc = 0;
    } else if (value != GUARD_BEQ) {
        /* Not the instruction this was written for. A different IOSU
         * build, or the wrong address -- either way, writing here blind
         * could brick the running system, so it refuses. */
        snprintf(why, why_size, "unexpected instruction 0x%08X at 0x%08X -- refusing to write",
                 value, GUARD_ADDR);
    } else if (Mocha_IOSUKernelWrite32(GUARD_ADDR, GUARD_NOP) != MOCHA_RESULT_SUCCESS) {
        snprintf(why, why_size, "write to 0x%08X failed", GUARD_ADDR);
    } else if (Mocha_IOSUKernelRead32(GUARD_ADDR, &value) != MOCHA_RESULT_SUCCESS ||
               value != GUARD_NOP) {
        snprintf(why, why_size, "readback mismatch (0x%08X)", value);
    } else {
        snprintf(why, why_size, "applied");
        rc = 0;
    }

    Mocha_DeInitLibrary();
    return rc;
}
