/*
 * The whole sequence raw, the way usb_mic.rpl does it.
 *
 * usb_mic drives a USB device every day and imports exactly one
 * library: coreinit, for IOS_Open/IOS_Ioctl/IOS_Ioctlv. It opens
 * /dev/uhs itself. It never uses nsysuhs.rpl -- which is the library
 * every probe here has gone through, and the one thing between us and
 * a fault that has survived twelve eliminations.
 *
 * So this takes it out of the way. Every request block below is the one
 * nsysuhs builds, read from its decompilation rather than guessed:
 *
 *   ioctl  0x11 query        filter in, profiles out
 *   ioctl  0x04 acquire      0x0C {if_handle, context, callback}
 *   ioctl  0x0B endpoints    0x18 {type, if_handle, mask, pending, size, 0}
 *   ioctlv 0x0E bulk         0xA1 block + the data buffer
 *
 * Nothing here writes to the console: opening a device node and issuing
 * ioctls changes nothing that survives a reboot.
 */
#include <stdio.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/ios.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID    0x0b95
#define AX88179_PID 0x1790

#define UHS_IOCTL_QUERY     0x11
#define UHS_IOCTL_ACQUIRE   0x04
#define UHS_IOCTL_ADMIN_EP  0x0B
#define UHS_IOCTLV_BULK     0x0E

#define UHS_DIR_OUT 1
#define UHS_DIR_IN  2

/* IOS reads and writes these directly, so they are aligned and static. */
static uint8_t g_filter[0x40] __attribute__((aligned(0x40)));
static uint8_t g_profiles[16 * 0x16C] __attribute__((aligned(0x40)));
static uint8_t g_req[0x100] __attribute__((aligned(0x40)));
static uint8_t g_data[4096] __attribute__((aligned(0x40)));
static IOSVec  g_vecs[2] __attribute__((aligned(0x40)));

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("UHS raw: /dev/uhs directly") != 0) {
        probe_shutdown();
        return 1;
    }

    const IOSHandle fd = IOS_Open("/dev/uhs/0", IOS_OPEN_READWRITE);
    probe_say("IOS_Open(\"/dev/uhs/0\") -> %d", (int)fd);
    if (fd < 0) {
        probe_wait(); probe_shutdown(); return 1;
    }

    /* --- query, to find the adapter and its handle ------------------- */
    memset(g_filter, 0, sizeof(g_filter));
    put32(g_filter + 0x00, MATCH_DEV_VID | MATCH_DEV_PID);
    g_filter[0x04] = (uint8_t)(ASIX_VID >> 8);     /* vid at +2 in the struct, */
    g_filter[0x05] = (uint8_t)ASIX_VID;            /* which is 16-bit fields   */
    g_filter[0x06] = (uint8_t)(AX88179_PID >> 8);
    g_filter[0x07] = (uint8_t)AX88179_PID;
    /* match_params is a u16 at +0, vid u16 at +2, pid u16 at +4 */
    memset(g_filter, 0, sizeof(g_filter));
    g_filter[0] = 0x00; g_filter[1] = 0x03;                    /* VID|PID */
    g_filter[2] = (uint8_t)(ASIX_VID >> 8);  g_filter[3] = (uint8_t)ASIX_VID;
    g_filter[4] = (uint8_t)(AX88179_PID >> 8); g_filter[5] = (uint8_t)AX88179_PID;

    memset(g_profiles, 0, sizeof(g_profiles));
    DCFlushRange(g_filter, sizeof(g_filter));
    DCFlushRange(g_profiles, sizeof(g_profiles));
    int32_t r = IOS_Ioctl(fd, UHS_IOCTL_QUERY, g_filter, 0x10, g_profiles, sizeof(g_profiles));
    DCInvalidateRange(g_profiles, sizeof(g_profiles));
    probe_say("ioctl 0x11 query -> %d interface(s)", (int)r);
    if (r <= 0) {
        IOS_Close(fd);
        probe_wait(); probe_shutdown(); return 1;
    }
    const uint32_t ifh = get32(g_profiles);
    probe_say("interface handle %u (slot %u)", (unsigned)ifh, (unsigned)(ifh & 0xFFFF));

    /* --- acquire ----------------------------------------------------- */
    memset(g_req, 0, sizeof(g_req));
    put32(g_req + 0, ifh);
    DCFlushRange(g_req, sizeof(g_req));
    r = IOS_Ioctl(fd, UHS_IOCTL_ACQUIRE, g_req, 0x0C, NULL, 0);
    probe_say("ioctl 0x04 acquire -> %d", (int)r);

    /* --- enable the endpoints ---------------------------------------- */
    memset(g_req, 0, sizeof(g_req));
    put32(g_req + 0x00, 1);        /* type: ENABLE */
    put32(g_req + 0x04, ifh);
    /*
     * The mask names endpoints by direction, not by number.
     *
     * The worker that consumes this request walks bits 0..31 and reads
     * each as: endpoint = bit % 16, direction = (bit < 16) ? IN : OUT.
     * It looks each one up and fails the WHOLE call the moment one does
     * not exist.
     *
     * So 0xFFFF -- the obvious "all of them" -- asks to enable IN
     * endpoints 0 through 15, and this adapter has two. That is why
     * every variation of pending count and size made no difference: the
     * mask was wrong in all of them.
     *
     * This adapter: interrupt IN 1, bulk IN 2, bulk OUT 3.
     */
    put32(g_req + 0x08, (1u << 1) | (1u << 2) | (1u << (16 + 3)));
    put32(g_req + 0x0C, 4);        /* max pending  (must be <= 0x100)   */
    put32(g_req + 0x10, 2048);     /* max size     (<= 0x10000000)      */
    DCFlushRange(g_req, sizeof(g_req));
    r = IOS_Ioctl(fd, UHS_IOCTL_ADMIN_EP, g_req, 0x18, NULL, 0);
    probe_say("ioctl 0x0B endpoints -> %d", (int)r);

    /* --- the bulk read, as an ioctlv --------------------------------- */
    memset(g_req, 0, sizeof(g_req));
    put32(g_req + 0x00, ifh);
    g_req[0x04] = 0x02;                 /* endpoint, one byte  */
    put32(g_req + 0x05, 300);           /* timeout, unaligned  */
    put32(g_req + 0x09, 3);             /* what nsysuhs always writes here */
    put32(g_req + 0x0D, UHS_DIR_IN);
    put32(g_req + 0x11, sizeof(g_data));
    memset(g_data, 0, sizeof(g_data));

    memset(g_vecs, 0, sizeof(g_vecs));
    g_vecs[0].vaddr = g_req;
    g_vecs[0].len = 0xA1;
    g_vecs[1].vaddr = g_data;
    g_vecs[1].len = sizeof(g_data);

    DCFlushRange(g_req, sizeof(g_req));
    DCFlushRange(g_data, sizeof(g_data));
    DCFlushRange(g_vecs, sizeof(g_vecs));
    /* read: one vector in (the request), one out (the data) */
    r = IOS_Ioctlv(fd, UHS_IOCTLV_BULK, 1, 1, g_vecs);
    DCInvalidateRange(g_data, sizeof(g_data));
    probe_say("ioctlv 0x0E bulk IN -> %d %s", (int)r, r >= 0 ? "*** ACCEPTED ***" : "");
    if (r > 0) {
        probe_say("  first bytes: %02x %02x %02x %02x %02x %02x %02x %02x", g_data[0], g_data[1],
                  g_data[2], g_data[3], g_data[4], g_data[5], g_data[6], g_data[7]);
    }

    IOS_Close(fd);
    probe_wait();
    probe_shutdown();
    return 0;
}
