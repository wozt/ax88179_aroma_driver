#ifndef WIIU_AX88179_H
#define WIIU_AX88179_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An ASIX AX88179 driver for the Wii U, in PPC userspace over /dev/uhs.
 *
 * The console's own IOSU driver only binds the AX88772 -- checked, its
 * module even names its source file `usb_eth_asix.c` -- so an AX88179
 * is enumerated by the USB host stack and then ignored. Measured on the
 * console: the host stack sees it (0b95:1790, interrupt IN + bulk IN +
 * bulk OUT), hands the interface over on request, and answers a control
 * transfer with its real MAC address. Everything below is built on those
 * three facts and not on hope.
 *
 * The register numbers and the order of the bring-up come from reading
 * Linux's `ax88179_178a.c`, which is GPL-2.0. Numbers a chip responds to
 * are facts about the chip; this code is written from them rather than
 * copied, and that distinction is the reason this file can live in this
 * project at all.
 */

#define AX88179_MAC_LEN 6

typedef struct Ax88179 Ax88179;

/* Finds the adapter, takes the interface and brings the chip up.
 * Returns NULL and writes why into `why`. Single instance, serial calls only. */
Ax88179 *ax88179_open(char *why, unsigned why_size);
void     ax88179_close(Ax88179 *ax);

/* The adapter's own hardware address, read out of it. */
const uint8_t *ax88179_mac(const Ax88179 *ax);

/* Whether the PHY says a cable is in and a partner is talking, and at
 * what speed. `speed` is 10, 100 or 1000, or 0 when the link is down. */
/* Returns 1 up (MAC configured), 0 down/negotiating, -1 USB error.
 * Poll regularly to apply renegotiated speed/duplex before sending. */
int ax88179_link(Ax88179 *ax, int *speed);
int ax88179_read_mac(Ax88179 *ax, uint16_t reg, void *out, uint16_t size);
int ax88179_read_phy(Ax88179 *ax, uint16_t reg, uint16_t *value);
int32_t ax88179_last_control(const Ax88179 *ax);

/* One ethernet frame out. Returns 0, or -1. */
int ax88179_send(Ax88179 *ax, const void *frame, int length);

/*
 * One ethernet frame in, 0 when none is currently ready, -1 on
 * USB/malformed data.
 *
 * On Wii U the RX path keeps three asynchronous UHS bulk-IN requests
 * pending. Completed aggregates are copied into a parser-owned snapshot,
 * their DMA slot is immediately re-armed, and frames are then unwrapped.
 *
 * The adapter does not hand over bare frames: a bulk transfer carries
 * several, wrapped in the chip's own receive header, and this unwraps
 * them. Getting that wrong does not fail -- it silently corrupts every
 * inbound packet, which is what the AX88772B patcher had to fix on the
 * other chip and is the failure mode to watch for here.
 */
/* UHS timeout in microseconds, inferred from console timing measurements. */
int ax88179_receive(Ax88179 *ax, void *frame, int max_length, int timeout_us);

/*
 * Return the next ethernet frame already buffered inside the most recent
 * AX88179 bulk transfer.
 *
 * This never submits a new USB request. It returns:
 *   >0 frame length
 *    0 no buffered frame remains
 *   -1 malformed buffered data
 */
int ax88179_receive_buffered(Ax88179 *ax, void *frame, int max_length);

/* What the last bulk request returned, and which endpoints were
 * chosen. Both exist because the first version swallowed its errors and
 * "no frames" could have meant any of five things. */
int32_t ax88179_last_bulk(const Ax88179 *ax);
/* Microseconds the last bulk IN took: a refusal returns at once, a
 * timeout takes the timeout it was given. */
uint32_t ax88179_last_bulk_us(const Ax88179 *ax);
void    ax88179_endpoints(const Ax88179 *ax, int *in, int *out, int *irq);

#ifdef __cplusplus
}
#endif

#endif /* WIIU_AX88179_H */
