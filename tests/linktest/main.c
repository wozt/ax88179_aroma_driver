/*
 * Bring the adapter up and see whether frames move.
 *
 * Everything before this proved the console can *address* the adapter.
 * This is the first thing that asks the adapter to do its job: power the
 * PHY, start the receiver, and listen. A LAN is never silent -- ARP,
 * broadcast, discovery -- so on a plugged-in cable frames should simply
 * start arriving, and their source addresses should be real.
 *
 * If frames arrive and look like ethernet, the hard question of this
 * whole project is answered and the rest is a TCP/IP stack.
 */
#include <stdio.h>
#include <string.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "ax88179.h"
#include "probe.h"

static uint8_t g_frame[2048];

static const char *ethertype_name(uint16_t t)
{
    switch (t) {
    case 0x0800: return "IPv4";
    case 0x0806: return "ARP";
    case 0x86DD: return "IPv6";
    case 0x8100: return "VLAN";
    default:     return "?";
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("AX88179: bring up and listen") != 0) {
        probe_shutdown();
        return 1;
    }

    char why[128];
    Ax88179 *ax = ax88179_open(why, sizeof(why));
    if (!ax) {
        probe_say("could not open the adapter: %s", why);
        probe_wait();
        probe_shutdown();
        return 1;
    }

    const uint8_t *mac = ax88179_mac(ax);
    probe_say("adapter up, MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
              mac[4], mac[5]);
    {
        int in = 0, out = 0, irq = 0;
        ax88179_endpoints(ax, &in, &out, &irq);
        probe_say("endpoints: bulk in %d, bulk out %d, interrupt %d", in, out, irq);
    }

    /* The PHY needs a moment to negotiate once it is powered. */
    int speed = 0, up = 0;
    for (int i = 0; i < 60 && !up; i++) {
        up = ax88179_link(ax, &speed);
        if (!up) {
            OSSleepTicks(OSMillisecondsToTicks(250));
        }
    }
    if (up < 0) {
        probe_say("PHY/link configuration failed: UHS %ld", (long)ax88179_last_control(ax));
        ax88179_close(ax); probe_wait(); probe_shutdown(); return 1;
    }
    probe_say(up ? "link UP at %d Mbit/s" : "link DOWN (%d) -- is the cable in?", speed);

    probe_say("listening for frames...");
    int seen = 0, shown = 0;
    for (int i = 0; i < 400 && shown < 8; i++) {
        const int n = ax88179_receive(ax, g_frame, sizeof(g_frame), 50);
        if (n <= 0) {
            continue;
        }
        seen++;
        const uint16_t type = (uint16_t)((g_frame[12] << 8) | g_frame[13]);
        probe_say("  %3d B  %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x  %04x %s", n,
                  g_frame[6], g_frame[7], g_frame[8], g_frame[9], g_frame[10], g_frame[11],
                  g_frame[0], g_frame[1], g_frame[2], type, ethertype_name(type));
        shown++;
    }

    if (seen) {
        probe_say("FRAMES ARE MOVING -- %d seen. The rest is a TCP/IP stack.", seen);
    } else {
        probe_say("no frames. link %s; last bulk request returned %d",
                  up ? "up" : "down", (int)ax88179_last_bulk(ax));
        probe_say("a negative there is the host stack refusing the transfer;");
        probe_say("a zero is it timing out with nothing to give.");
    }

    ax88179_close(ax);
    probe_wait();
    probe_shutdown();
    return 0;
}
