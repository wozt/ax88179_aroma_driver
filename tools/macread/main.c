/*
 * Can this console actually talk to the adapter?
 *
 * The previous probe showed the USB host stack enumerates the AX88179.
 * That proves it is *there*. This asks the next thing: will UhsAcquire
 * Interface hand it over, and does it answer a control transfer.
 *
 * The transfer is the one Linux's ax88179_178a.c uses to read the MAC
 * address, taken from that driver rather than guessed:
 *
 *     ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, ETH_ALEN, ETH_ALEN, mac)
 *       -> bmRequestType USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_DEVICE = 0xC0
 *          bRequest      AX_ACCESS_MAC = 0x01
 *          wValue        AX_NODE_ID    = 0x10
 *          wIndex        6 (the size)
 *          wLength       6
 *
 * A plausible MAC coming back -- ASIX's own OUI is 00:0E:C6 -- means the
 * whole userspace path works: enumerate, acquire, control transfer,
 * read a register. Everything after that is work rather than doubt.
 */
#include <stdio.h>
#include <string.h>

#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID   0x0b95
#define AX88179_PID 0x1790

#define AX_ACCESS_MAC 0x01
#define AX_NODE_ID    0x10
#define ETH_ALEN      6
#define USB_IN_VENDOR_DEVICE 0xC0

#define WORK_SIZE (128 * 1024)
#define MAX_IFACES 32

static UhsHandle g_handle;
static uint8_t g_work[WORK_SIZE] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[MAX_IFACES];
/* The stack DMAs into this, so it is aligned and not on the stack. */
static uint8_t g_mac[64] __attribute__((aligned(0x40)));

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("AX88179: acquire and read MAC") != 0) {
        probe_shutdown();
        return 1;
    }

    UhsConfig config;
    memset(&config, 0, sizeof(config));
    config.controller_num = 0;
    config.buffer = g_work;
    config.buffer_size = sizeof(g_work);

    int32_t r = UhsClientOpen(&g_handle, &config);
    probe_say("UhsClientOpen -> %d", (int)r);
    if (r < 0) {
        probe_wait();
        probe_shutdown();
        return 1;
    }

    UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid = ASIX_VID;
    filter.pid = AX88179_PID;

    int32_t found = UhsQueryInterfaces(&g_handle, &filter, g_profiles, MAX_IFACES);
    probe_say("query for %04x:%04x -> %d interface(s)", ASIX_VID, AX88179_PID, (int)found);
    if (found <= 0) {
        probe_say("the adapter is not plugged in, or not enumerated");
        probe_wait();
        UhsClientClose(&g_handle);
        probe_shutdown();
        return 1;
    }

    const uint32_t if_handle = g_profiles[0].if_handle;
    probe_say("interface handle %u, endpoints in %02x/%02x out %02x", (unsigned)if_handle,
              g_profiles[0].in_endpoints[0].bEndpointAddress,
              g_profiles[0].in_endpoints[1].bEndpointAddress,
              g_profiles[0].out_endpoints[0].bEndpointAddress);

    r = UhsAcquireInterface(&g_handle, if_handle, NULL, NULL);
    probe_say("UhsAcquireInterface -> %d %s", (int)r, r >= 0 ? "(granted)" : "(REFUSED)");
    if (r < 0) {
        probe_say("nothing else should hold a vendor-specific class,");
        probe_say("so a refusal here is the answer we did not want.");
        probe_wait();
        UhsClientClose(&g_handle);
        probe_shutdown();
        return 1;
    }

    memset(g_mac, 0, sizeof(g_mac));
    r = UhsSubmitControlRequest(&g_handle, if_handle, g_mac, AX_ACCESS_MAC,
                                USB_IN_VENDOR_DEVICE, AX_NODE_ID, ETH_ALEN, ETH_ALEN, 1000);
    probe_say("control transfer (read MAC) -> %d", (int)r);

    if (r >= 0) {
        probe_say("MAC: %02x:%02x:%02x:%02x:%02x:%02x", g_mac[0], g_mac[1], g_mac[2], g_mac[3],
                  g_mac[4], g_mac[5]);
        int all_zero = 1, all_ff = 1;
        for (int i = 0; i < ETH_ALEN; i++) {
            if (g_mac[i] != 0x00) all_zero = 0;
            if (g_mac[i] != 0xFF) all_ff = 0;
        }
        if (all_zero || all_ff) {
            probe_say("that is not a real address -- the transfer went through");
            probe_say("but the adapter did not answer with anything.");
        } else {
            probe_say("a real address. ASIX's own OUI is 00:0e:c6.");
            probe_say("ENUMERATE, ACQUIRE, CONTROL TRANSFER: all work.");
        }
    }

    UhsReleaseInterface(&g_handle, if_handle, false);
    UhsClientClose(&g_handle);
    probe_wait();
    probe_shutdown();
    return 0;
}
