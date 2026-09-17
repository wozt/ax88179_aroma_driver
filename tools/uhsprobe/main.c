/*
 * Does this console's USB host stack see the adapter at all?
 *
 * The cheapest decisive question in the whole Ethernet project. The
 * stock IOSU driver only binds AX88772, so the adapter will certainly
 * not appear as a network interface -- but that says nothing about
 * whether the hardware is enumerated underneath.
 *
 * /dev/uhs is the USB host stack and wut reaches it from PPC userspace,
 * so this asks it directly, with MATCH_ANY: show me every interface you
 * have. If the AX88179 (0b95:1790) is in that list, a userspace driver
 * is possible and the IOSU route is optional. If it is absent, both
 * routes are dead and the adapter is simply the wrong one -- which is
 * worth knowing in an afternoon rather than after a month of ARM.
 */
#include <stdio.h>
#include <string.h>

#include <nsysuhs/uhs.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include "proc.h"

/* UHS wants a scratch buffer to build its answers in. Generous: the
 * profile struct is 0x16C bytes and a console can have a lot of
 * interfaces once a hub is involved. */
#define WORK_SIZE (128 * 1024)
#define MAX_IFACES 32

static UhsHandle g_handle;
static uint8_t g_work[WORK_SIZE] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[MAX_IFACES];

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "(per interface)";
    case 0x01: return "audio";
    case 0x02: return "communications";
    case 0x03: return "HID";
    case 0x06: return "still imaging";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC data";
    case 0x0E: return "video";
    case 0xE0: return "wireless";
    case 0xFF: return "vendor specific";
    default:   return "?";
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogUdpInit();
    WHBLogPrintf("uhsprobe: starting");
    proc_init();

    UhsConfig config;
    memset(&config, 0, sizeof(config));
    config.controller_num = 0;
    config.buffer = g_work;
    config.buffer_size = sizeof(g_work);

    const int32_t opened = UhsClientOpen(&g_handle, &config);
    WHBLogPrintf("uhsprobe: UhsClientOpen -> %d", (int)opened);

    if (opened >= 0) {
        UhsInterfaceFilter filter;
        memset(&filter, 0, sizeof(filter));
        filter.match_params = MATCH_ANY;

        const int32_t found =
            UhsQueryInterfaces(&g_handle, &filter, g_profiles, MAX_IFACES);
        WHBLogPrintf("uhsprobe: UhsQueryInterfaces -> %d interface(s)", (int)found);

        int adapter_seen = 0;
        for (int32_t i = 0; i < found && i < MAX_IFACES; i++) {
            const UhsInterfaceProfile *p = &g_profiles[i];
            const uint16_t vid = p->dev_desc.idVendor;
            const uint16_t pid = p->dev_desc.idProduct;
            WHBLogPrintf("  [%2d] %04x:%04x  dev class %02x %s  if class %02x/%02x/%02x  handle %d",
                         (int)i, vid, pid, p->dev_desc.bDeviceClass,
                         class_name(p->dev_desc.bDeviceClass), p->if_desc.bInterfaceClass,
                         p->if_desc.bInterfaceSubClass, p->if_desc.bInterfaceProtocol,
                         (int)p->if_handle);
            for (int e = 0; e < 4; e++) {
                if (p->in_endpoints[e].bLength) {
                    WHBLogPrintf("        in  ep %02x attr %02x max %u",
                                 p->in_endpoints[e].bEndpointAddress,
                                 p->in_endpoints[e].bmAttributes,
                                 p->in_endpoints[e].wMaxPacketSize);
                }
                if (p->out_endpoints[e].bLength) {
                    WHBLogPrintf("        out ep %02x attr %02x max %u",
                                 p->out_endpoints[e].bEndpointAddress,
                                 p->out_endpoints[e].bmAttributes,
                                 p->out_endpoints[e].wMaxPacketSize);
                }
            }
            if (vid == 0x0b95) {
                adapter_seen = 1;
                WHBLogPrintf("        ^^ ASIX. 1790 = AX88179, 7720 = AX88772");
            }
        }

        WHBLogPrintf("uhsprobe: VERDICT -- ASIX adapter %s",
                     adapter_seen ? "IS enumerated by the host stack"
                                  : "is NOT in the list");
        UhsClientClose(&g_handle);
    }

    /* Held open a moment so the logs are certainly out before the
     * transition back to the menu. */
    for (int i = 0; i < 180 && proc_running(); i++) {
    }

    WHBLogPrintf("uhsprobe: done");
    WHBLogUdpDeinit();
    proc_shutdown();
    return 0;
}
