#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include "ax88179.h"
#include "probe.h"
#include "proc.h"
#include "ax_net.h"

static void dump_registers(Ax88179 *ax)
{
    const uint16_t regs[] = {0x26, 0x33, 0x0b, 0x22};
    for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        uint8_t b[2] = {0};
        if (ax88179_read_mac(ax, regs[i], b, regs[i] == 0x33 ? 1 : 2))
            probe_say("MAC %02x: error UHS %ld", regs[i], (long)ax88179_last_control(ax));
        else probe_say("MAC %02x = %04x", regs[i], b[0] | (b[1] << 8));
    }
    const uint16_t phy[] = {0, 1, 2, 3, 0x11};
    for (unsigned i = 0; i < sizeof(phy)/sizeof(phy[0]); i++) {
        uint16_t v;
        if (ax88179_read_phy(ax, phy[i], &v))
            probe_say("PHY %02x: error UHS %ld", phy[i], (long)ax88179_last_control(ax));
        else probe_say("PHY %02x = %04x", phy[i], v);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (probe_init("AX88179 usermode Ethernet - DHCP 3") != 0) {
        probe_shutdown(); return 1;
    }
    char why[160];
    Ax88179 *ax = ax88179_open(why, sizeof(why));
    if (!ax) {
        probe_say("Initialization failed:");
        /* Keep the UHS return code visible even on a narrow display. */
        for (unsigned offset = 0; offset < strlen(why); offset += 60)
            probe_say("%.60s", why + offset);
        probe_wait(); probe_shutdown(); return 1;
    }
    const uint8_t *mac = ax88179_mac(ax);
    probe_say("MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    int in, out, irq;
    ax88179_endpoints(ax, &in, &out, &irq);
    probe_say("Endpoints: bulk IN %d, OUT %d; interrupt IN %d", in, out, irq);
    probe_say("Endpoint masks: IN %08lx, OUT %08lx",
        (unsigned long)(1u << (16 + in)), (unsigned long)(1u << out));
    dump_registers(ax);
    probe_say("Waiting for Ethernet negotiation (up to 15 seconds)...");
    int up = 0, speed = 0;
    for (int i = 0; i < 60 && probe_poll(); i++) {
        up = ax88179_link(ax, &speed);
        if (up != 0) break;
        OSSleepTicks(OSMillisecondsToTicks(250));
    }
    if (up < 0) probe_say("Link read/config failed; last control UHS=%ld", (long)ax88179_last_control(ax));
    else if (!up) probe_say("Link down/negotiating. Check cable and switch.");
    else if (argc > 1 && !strcmp(argv[1], "--wire")) {
        probe_say("Link UP: %d Mbit/s", speed);
        dump_registers(ax);
        /* A PC raw-Ethernet peer returns PING payloads as PONG.
         * 504 bytes exercises the USB 512-byte boundary including TX header. */
        const unsigned sizes[3] = {60, 1514, 504};
        uint8_t test[1514];
        unsigned verified = 0;
        unsigned seen = 0;
        uint8_t frame[1600];
        int32_t previous_error = 0;
        for (int i = 0; i < 70 && probe_poll(); i++) {
            if (i % 10 == 0 && ax88179_link(ax, &speed) != 1) {
                probe_say("Link lost or control error"); break;
            }
            if (i % 10 == 0) {
                unsigned seq = (unsigned)(i / 10) % 3;
                unsigned length = sizes[seq];
                memset(test, 0xff, 6);
                memcpy(test + 6, mac, 6);
                test[12] = 0x88; test[13] = 0xb5;
                memcpy(test + 14, "AX179PNG", 8);
                test[22] = (uint8_t)seq;
                for (unsigned j = 23; j < length; j++) test[j] = (uint8_t)(j + seq);
                int sent = ax88179_send(ax, test, (int)length);
                probe_say("TX probe seq=%u len=%u: %s UHS=%ld", seq, length,
                    sent ? "failed" : "submitted", (long)ax88179_last_bulk(ax));
            }
            int n = ax88179_receive(ax, frame, sizeof(frame), 100);
            if (n < 0) {
                int32_t r = ax88179_last_bulk(ax);
                if (!previous_error || r != previous_error)
                    probe_say("RX error: UHS %ld (positive = malformed aggregate)", (long)r);
                previous_error = r;
                /* Unknown negative UHS codes are not silently called timeouts. */
                OSSleepTicks(OSMillisecondsToTicks(100));
                continue;
            }
            if (!n) continue;
            seen++;
            if (n >= 23 && !memcmp(frame, mac, 6) && frame[12] == 0x88 &&
                frame[13] == 0xb5 && !memcmp(frame + 14, "AX179PON", 8) && frame[22] < 3) {
                unsigned seq = frame[22];
                unsigned expected = sizes[seq];
                int valid = (unsigned)n == expected;
                for (int j = 23; j < n; j++)
                    if (frame[j] != (uint8_t)(j + seq)) valid = 0;
                probe_say("RX peer echo seq=%u len=%d payload=%s", seq, n, valid ? "PASS" : "FAIL");
                if (valid) verified |= 1u << seq;
                /* Continue beyond one full round to detect a stuck TX
                 * stream after the exact USB packet boundary test. */
            }
            if (seen <= 8) probe_say("RX %d bytes: %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x", n,
                frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], frame[12], frame[13]);
        }
        probe_say("Received %u Ethernet frames", seen);
        probe_say("WIRE TEST: %s (verified mask=%x, expected 7)",
            verified == 7 ? "PASS" : "INCOMPLETE", verified);
    }
    if (up == 1 && ax_net_start(ax) == 0) {
        probe_say("DHCP started on AX88179 (separate from system Wi-Fi)");
        OSTime next_report = OSGetTime();
        unsigned duration = argc > 2 && !strcmp(argv[1], "--seconds") ?
                            (unsigned)strtoul(argv[2], NULL, 10) : 0;
        OSTime deadline = duration ? OSGetTime() + OSMillisecondsToTicks(duration * 1000u) : 0;
        unsigned polls = 0, frames = 0, errors = 0;
        char last_ip[16] = "";
        while (probe_poll()) {
            if (deadline && OSGetTime() >= deadline) {
                probe_say("Timed test complete, releasing interface and exiting");
                proc_stop();
                break;
            }
            int n = ax_net_poll();
            polls++; if (n > 0) frames++; if (n < 0) errors++;
            if (OSGetTime() >= next_report) {
                probe_say("IP active: polls=%u rx=%u errors=%u lastUHS=%ld", polls, frames, errors,
                    (long)ax88179_last_bulk(ax));
                next_report = OSGetTime() + OSMillisecondsToTicks(10000);
            }
            const char *ip = ax_net_address();
            if (ip && strcmp(ip, last_ip)) {
                snprintf(last_ip, sizeof(last_ip), "%s", ip);
                probe_say("DHCP BOUND: %s", ip);
                probe_say("ICMP echo responder active until HOME/MINUS");
            }
        }
        ax_net_stop();
    }
    ax88179_close(ax);
    probe_wait(); probe_shutdown(); return 0;
}
