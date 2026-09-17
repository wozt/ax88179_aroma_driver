#ifndef AX88179_IOSU_PATCH_H
#define AX88179_IOSU_PATCH_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NOPs the UHS endpoint-ownership branch in IOSU so a Cafe OS client may
 * enable the adapter's endpoints. Idempotent: applying it twice is fine,
 * and it refuses to write if the target is not the expected instruction.
 *
 * Returns 0 when the patch is in place (freshly applied or already
 * there), -1 otherwise with a human-readable reason in `why`. The write
 * is volatile, so this must run each boot before the adapter is opened.
 */
int iosu_patch_apply(char *why, unsigned why_size);

#ifdef __cplusplus
}
#endif

#endif
