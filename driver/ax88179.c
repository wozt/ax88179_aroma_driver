#include "ax88179.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AX88179_HOST_TEST
#include "../tests/uhs_mock.h"
#else
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>
#include <whb/log.h>
#endif

/*
 * The direction argument is 1 or 2, and neither is zero.
 *
 * Read out of nsysuhs.rpl rather than assumed. UhsSubmitBulkRequest
 * builds an ioctlv and counts its vectors from the direction:
 *
 *     param_4 == 1  ->  2 in, 0 out   the buffer is SENT      (write)
 *     param_4 == 2  ->  1 in, 1 out   the buffer is FILLED    (read)
 *     anything else ->  neither branch runs, and the call goes out
 *                       malformed
 *
 * The obvious guess -- 0 for out, 1 for in -- is wrong twice over: it
 * asks to WRITE to an IN endpoint, and its "out" is a request with no
 * vectors at all. Control transfers were unaffected because they take
 * no direction argument; theirs comes from bmRequestType, which is why
 * they were the one thing that always worked.
 */
#define UHS_DIR_OUT 1
#define UHS_DIR_IN  2

/* --- the chip, as its registers -------------------------------------- */
/* Numbers from Linux's ax88179_178a.c. A number a chip answers to is a
 * fact about the chip. */
#define AX_ACCESS_MAC            0x01
#define AX_ACCESS_PHY            0x02

#define AX_NODE_ID               0x10
#define AX_RX_CTL                0x0b
#define AX_MEDIUM_STATUS_MODE    0x22
#define AX_MONITOR_MOD           0x24
#define AX_PHYPWR_RSTCTL         0x26
#define AX_RX_BULKIN_QCTRL       0x2e
#define AX_CLK_SELECT            0x33
#define AX_RXCOE_CTL             0x34
#define AX_TXCOE_CTL             0x35
#define AX_PAUSE_WATERLVL_HIGH   0x54
#define AX_PAUSE_WATERLVL_LOW    0x55

#define AX_PHYPWR_RSTCTL_IPRL    0x0020
#define AX_CLK_SELECT_BCS        0x01
#define AX_CLK_SELECT_ACS        0x02

#define AX_RX_CTL_DROPCRCERR     0x0100
#define AX_RX_CTL_IPE            0x0200
#define AX_RX_CTL_START          0x0080
#define AX_RX_CTL_AP             0x0020
#define AX_RX_CTL_AB             0x0008
#define AX_RX_CTL_AMALL          0x0002

#define AX_MONITOR_MODE_RWMP     0x04
#define AX_MONITOR_MODE_PMEPOL   0x20
#define AX_MONITOR_MODE_PMETYPE  0x40

#define AX_RXHDR_CRC_ERR         0x20000000u
#define AX_RXHDR_DROP_ERR        0x80000000u

/* PHY, through the same control endpoint */
#define AX88179_PHY_ID           0x03
#define MII_BMSR                 0x01
#define MII_BMSR_LINK            0x0004
#define AX_PHYSICAL_LINK_STATUS  0x02
#define AX_USB_SS                0x04   /* USB bus speed, not Ethernet speed */

#define ASIX_VID                 0x0b95
#define AX88179_PID              0x1790

/* Control transfer direction/type for this chip: vendor request to the
 * device itself. */
/* UhsSubmitBulkRequest's direction argument. The header names no
 * constants for it, so they are named here. */


#define REQ_IN   0xC0
#define REQ_OUT  0x40

#define UHS_WORK_SIZE   (128 * 1024)
#define MAX_IFACES      16
/* The bulk IN carries several frames at once, headers and all. */
#define RX_BUFFER_SIZE  (26 * 1024)
#define RX_CTL_DEFAULT (AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START | \
                        AX_RX_CTL_AP | AX_RX_CTL_AB | AX_RX_CTL_AMALL)

/* UHS's answer when a bulk IN had nothing to hand back. */
#define AX_UHS_NO_DATA (-2162747)

struct Ax88179 {
    UhsHandle handle;
    UhsConfig config;
    uint16_t tx_maxpacket;
    uint16_t medium;
    uint32_t enabled_eps;
    uint32_t ep_in_mask, ep_out_mask;
    int32_t last_control;
    uint8_t last_cmd;
    uint16_t last_value, last_index, last_size;
    uint8_t last_read;
    uint32_t  if_handle;
    uint8_t   ep_in;      /* bulk, frames from the wire */
    uint8_t   ep_out;     /* bulk, frames to it */
    uint8_t   ep_irq;     /* interrupt, link status */
    uint8_t   mac[AX88179_MAC_LEN];

    /* What is left of the last bulk transfer, still to be unwrapped. */
    int       rx_len;
    int       rx_frames;      /* how many frames its header promised */
    int       rx_next;        /* which one comes next */
    uint32_t  rx_hdr_offset;  /* where the descriptors live */
    int       rx_pos;         /* how far into the frames we have walked */
    int32_t   last_bulk;      /* what the last bulk request returned */
    /* How long the last failing bulk IN took. A refusal comes back at
     * once; a timeout takes the timeout. That is the whole difference
     * between "the adapter will not talk to us" and "nothing arrived",
     * and the error code alone does not say which. */
    uint32_t  last_bulk_us;
};

/* UHS DMAs into these, so they are aligned and static rather than on a
 * stack that moves. */
static uint8_t g_uhs_work[UHS_WORK_SIZE] __attribute__((aligned(0x40)));
static uint8_t g_ctrl[64] __attribute__((aligned(0x40)));
static uint8_t g_rx[RX_BUFFER_SIZE] __attribute__((aligned(0x40)));
static uint8_t g_tx[2048] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[MAX_IFACES] __attribute__((aligned(0x40)));
/* Static DMA buffers: one instance, called serially by its owner thread. */
static int g_open;
static unsigned phy_init_generation;
static int debug_last_link = -1;
static OSTime debug_link_start;

#define PHY_LOG(fmt, ...) \
    WHBLogPrintf("[%llu] PHY: " fmt, \
        (unsigned long long)OSTicksToMilliseconds(OSGetTime()), ##__VA_ARGS__)

/* The chip speaks little-endian and this console does not. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void sleep_ms(int ms)
{
    OSSleepTicks(OSMillisecondsToTicks((uint32_t)ms));
}

/* --- register access -------------------------------------------------- */

static int control(Ax88179 *ax, int read, uint8_t cmd, uint16_t value,
                   uint16_t index, uint16_t size, void *data)
{
    if (!ax || !data || !size || size > sizeof(g_ctrl)) return -1;
    memset(g_ctrl, 0, sizeof(g_ctrl));
    if (!read) memcpy(g_ctrl, data, size);
    DCFlushRange(g_ctrl, sizeof(g_ctrl));
    ax->last_cmd = cmd;
    ax->last_value = value;
    ax->last_index = index;
    ax->last_size = size;
    ax->last_read = read;
    int32_t r = (int32_t)UhsSubmitControlRequest(&ax->handle, ax->if_handle,
        g_ctrl, cmd, read ? REQ_IN : REQ_OUT, value, index, size, 1000);
    ax->last_control = r;
    if (r < 0) return -1;
    /* UHS synchronous transfers return the transferred byte count. */
    if (r != size) return -1;
    if (read) {
        DCInvalidateRange(g_ctrl, sizeof(g_ctrl));
        memcpy(data, g_ctrl, size);
    }
    return 0;
}

static int reg_write(Ax88179 *ax, uint8_t cmd, uint16_t reg, uint16_t size,
                     const void *data)
{
    return control(ax, 0, cmd, reg, size, size, (void *)data);
}

static int reg_read(Ax88179 *ax, uint8_t cmd, uint16_t reg, uint16_t size, void *out)
{
    return control(ax, 1, cmd, reg, size, size, out);
}

int ax88179_read_mac(Ax88179 *ax, uint16_t reg, void *out, uint16_t size)
{
    return reg_read(ax, AX_ACCESS_MAC, reg, size, out);
}

int ax88179_read_phy(Ax88179 *ax, uint16_t reg, uint16_t *value)
{
    uint8_t bytes[2];
    if (!value || control(ax, 1, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, bytes))
        return -1;
    *value = bytes[0] | ((uint16_t)bytes[1] << 8);
    return 0;
}

static int phy_write(Ax88179 *ax, uint16_t reg, uint16_t value)
{
    uint8_t bytes[2] = { value, value >> 8 };
    return control(ax, 0, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, bytes);
}

int32_t ax88179_last_control(const Ax88179 *ax)
{
    return ax ? ax->last_control : -1;
}

static int mac_write8(Ax88179 *ax, uint16_t reg, uint8_t value)
{
    return reg_write(ax, AX_ACCESS_MAC, reg, 1, &value);
}

static int mac_write16(Ax88179 *ax, uint16_t reg, uint16_t value)
{
    /* The chip is little-endian and this console is not. */
    const uint8_t le[2] = { (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };
    return reg_write(ax, AX_ACCESS_MAC, reg, 2, le);
}

/* --- finding and opening ---------------------------------------------- */

/*
 * The endpoints, taken from the interface descriptor rather than
 * assumed.
 *
 * The first probes read in_endpoints[0] and [1] and got different
 * answers from a filtered query than from MATCH_ANY -- the array is
 * sparse, not packed. So this walks it and sorts by what each endpoint
 * IS: bulk in, bulk out, interrupt in. A bulk transfer to the wrong
 * endpoint fails silently, which is the worst way for this to be wrong.
 */
static int find_endpoints(Ax88179 *ax, const UhsInterfaceProfile *p)
{
    ax->ep_in = ax->ep_out = ax->ep_irq = 0;
    for (int i = 0; i < 16; i++) {
        const UhsEndpointDescriptor *in = &p->in_endpoints[i];
        if (in->bLength && (in->bEndpointAddress & 0x80)) {
            const uint8_t kind = in->bmAttributes & 0x03;
            if (kind == 0x02 && !ax->ep_in) {
                ax->ep_in = in->bEndpointAddress & 0x0F;
                ax->ep_in_mask = 1u << (16 + ax->ep_in);
            } else if (kind == 0x03 && !ax->ep_irq) {
                ax->ep_irq = in->bEndpointAddress & 0x0F;
            }
        }
        const UhsEndpointDescriptor *out = &p->out_endpoints[i];
        if (out->bLength && !(out->bEndpointAddress & 0x80)) {
            if ((out->bmAttributes & 0x03) == 0x02 && !ax->ep_out) {
                ax->ep_out = out->bEndpointAddress & 0x0F;
                ax->ep_out_mask = 1u << ax->ep_out;
                /* Accept native or raw USB byte order; legal bulk sizes
                 * for this device are unambiguous in either representation. */
                uint16_t mps = out->wMaxPacketSize;
                if (mps != 64 && mps != 512 && mps != 1024)
                    mps = (uint16_t)((mps >> 8) | (mps << 8));
                if (mps == 64 || mps == 512 || mps == 1024)
                    ax->tx_maxpacket = mps;
            }
        }
    }
    return (ax->ep_in && ax->ep_out && ax->tx_maxpacket) ? 0 : -1;
}

Ax88179 *ax88179_open(char *why, unsigned why_size)
{
    if (g_open) {
        snprintf(why, why_size, "driver already open (single instance)");
        return NULL;
    }
    Ax88179 *ax = calloc(1, sizeof(*ax));
    if (!ax) {
        snprintf(why, why_size, "out of memory");
        return NULL;
    }

    ax->config.controller_num = 0;
    ax->config.buffer = g_uhs_work;
    ax->config.buffer_size = sizeof(g_uhs_work);
    if ((int32_t)UhsClientOpen(&ax->handle, &ax->config) < 0) {
        snprintf(why, why_size, "cannot open /dev/uhs");
        free(ax);
        return NULL;
    }

    UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid = ASIX_VID;
    filter.pid = AX88179_PID;

    const int32_t found = UhsQueryInterfaces(&ax->handle, &filter, g_profiles, MAX_IFACES);
    if (found <= 0) {
        snprintf(why, why_size, "no AX88179 on the bus");
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    ax->if_handle = g_profiles[0].if_handle;
    if (find_endpoints(ax, &g_profiles[0]) != 0) {
        snprintf(why, why_size, "no bulk endpoint pair on the interface");
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    if ((int32_t)UhsAcquireInterface(&ax->handle, ax->if_handle, NULL, NULL) < 0) {
        snprintf(why, why_size, "the interface was refused");
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    g_open = 1;
    const char *stage = "PHY power reset";
#define CHECK(call) do { if ((call) != 0) goto fail; } while (0)
    int warm_phy = (phy_init_generation != 0);
    if (warm_phy) {
        uint16_t warm_bmcr = 0xffff;
        uint16_t warm_bmsr = 0xffff;

        if (ax88179_read_phy(ax, 0, &warm_bmcr) != 0 ||
            ax88179_read_phy(ax, 1, &warm_bmsr) != 0 ||
            warm_bmcr == 0xffff || warm_bmcr == 0x0000 ||
            warm_bmsr == 0xffff || warm_bmsr == 0x0000) {
            PHY_LOG("warm invalid -> cold");
            warm_phy = 0;
        } else {
            PHY_LOG("warm ok bmcr=%04x bmsr=%04x",
                         warm_bmcr, warm_bmsr);
        }
    }

    if (!warm_phy) {
        PHY_LOG("cold reset");
        CHECK(mac_write16(ax, AX_PHYPWR_RSTCTL, 0));
        CHECK(mac_write16(ax, AX_PHYPWR_RSTCTL, AX_PHYPWR_RSTCTL_IPRL));
        sleep_ms(500);
    } else {
        PHY_LOG("warm reopen");
    }
    stage = "clock select";
    CHECK(mac_write8(ax, AX_CLK_SELECT, 3));
    sleep_ms(200);
    stage = "clock readback";
    uint8_t clock;
    CHECK(reg_read(ax, AX_ACCESS_MAC, AX_CLK_SELECT, 1, &clock));
    if ((clock & 3) != 3) goto fail;
    stage = "MAC address";
    CHECK(reg_read(ax, AX_ACCESS_MAC, AX_NODE_ID, 6, ax->mac));
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 6; i++) nonzero |= ax->mac[i];
    if (!nonzero || (ax->mac[0] & 1)) goto fail;

    stage = "RX/TX configuration";
    CHECK(mac_write16(ax, AX_RX_CTL, 0));
    CHECK(mac_write16(ax, AX_MEDIUM_STATUS_MODE, 0));
    const uint8_t queue[5] = {7, 0x20, 3, 0x16, 0xff};
    CHECK(reg_write(ax, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, queue));
    CHECK(mac_write8(ax, AX_PAUSE_WATERLVL_LOW, 0x34));
    CHECK(mac_write8(ax, AX_PAUSE_WATERLVL_HIGH, 0x52));
    CHECK(mac_write8(ax, AX_RXCOE_CTL, 0));
    CHECK(mac_write8(ax, AX_TXCOE_CTL, 0));
    stage = "PHY page/autonegotiation";
    CHECK(phy_write(ax, 0x1f, 0));
    uint16_t bmcr;
    CHECK(ax88179_read_phy(ax, 0, &bmcr));
    if (bmcr == 0xffff) goto fail;
    /* Preserve speed defaults; clear reset, loopback, powerdown/isolate. */
    uint16_t new_bmcr = (bmcr & ~0xcc00u) | 0x1000;
    if (!warm_phy)
        new_bmcr |= 0x0200; /* cold/fallback path: restart autoneg */
    CHECK(phy_write(ax, 0, new_bmcr));
    debug_link_start = OSGetTime();
    debug_last_link = -1;
    PHY_LOG("autoneg %s (generation %u, BMCR %04x -> %04x)",
                 warm_phy ? "KEEP" : "RESTART",
                 phy_init_generation, bmcr, new_bmcr);
    phy_init_generation++;

    /* UHS endpoint mask: OUT in bits 0..15, IN in bits 16..31,
     * matching wut UHSEndpointGetMask. Bulk direction is a separate argument. */
    uint32_t ep_mask = ax->ep_in_mask;
    stage = "enable bulk IN";
    int32_t ep_result = (int32_t)UhsAdministerEndpoint(&ax->handle, ax->if_handle,
        UHS_ADMIN_EP_ENABLE, ep_mask, 1, sizeof(g_rx));
    if (ep_result < 0) goto ep_fail;
    ax->enabled_eps |= ep_mask;
    stage = "enable bulk OUT";
    ep_mask = ax->ep_out_mask;
    ep_result = (int32_t)UhsAdministerEndpoint(&ax->handle, ax->if_handle,
        UHS_ADMIN_EP_ENABLE, ep_mask, 1, sizeof(g_tx));
    if (ep_result < 0) goto ep_fail;
    ax->enabled_eps |= ep_mask;
    stage = "start RX";
    CHECK(mac_write16(ax, AX_RX_CTL, RX_CTL_DEFAULT));
#undef CHECK
    return ax;
ep_fail:
    snprintf(why, why_size, "%s mask=%08lx: UHS %ld", stage, (unsigned long)ep_mask, (long)ep_result);
    ax88179_close(ax);
    return NULL;
fail:
    snprintf(why, why_size, "%s: %s cmd=%02x value=%04x index=%04x len=%u UHS=%ld",
        stage, ax->last_read ? "IN" : "OUT", ax->last_cmd, ax->last_value,
        ax->last_index, ax->last_size, (long)ax->last_control);
    ax88179_close(ax);
    return NULL;
}

void ax88179_close(Ax88179 *ax)
{
    if (!ax) {
        return;
    }
    mac_write16(ax, AX_RX_CTL, 0);
    if (ax->enabled_eps)
        UhsAdministerEndpoint(&ax->handle, ax->if_handle, UHS_ADMIN_EP_DISABLE,
                              ax->enabled_eps, 0, 0);
    UhsReleaseInterface(&ax->handle, ax->if_handle, false);
    UhsClientClose(&ax->handle);
    free(ax);
    g_open = 0;
}

const uint8_t *ax88179_mac(const Ax88179 *ax)
{
    return ax ? ax->mac : NULL;
}

int ax88179_link(Ax88179 *ax, int *speed)
{
    if (speed) *speed = 0;
    uint16_t status, bmsr;
    if (!ax || ax88179_read_phy(ax, 1, &bmsr) ||
        ax88179_read_phy(ax, 1, &bmsr) || ax88179_read_phy(ax, 0x11, &status))
        return -1;
    if (bmsr == 0xffff || status == 0xffff) return -1;

    int debug_up = ((bmsr & 4) && (bmsr & 0x20) && (status & 0x400));
    if (debug_up != debug_last_link) {
        uint64_t elapsed = OSTicksToMilliseconds(OSGetTime() - debug_link_start);
        PHY_LOG("link=%s after %llu ms BMSR=%04x STATUS=%04x",
                     debug_up ? "UP" : "DOWN",
                     (unsigned long long)elapsed,
                     bmsr, status);
        debug_last_link = debug_up;
    }
    if (!(bmsr & 4) || !(bmsr & 0x20) || !(status & 0x400)) {
        if (ax->medium && mac_write16(ax, AX_MEDIUM_STATUS_MODE, 0)) return -1;
        ax->medium = 0;
        ax->rx_frames = 0;
        return 0;
    }
    int s = (status & 0xc000) == 0x8000 ? 1000 :
            (status & 0xc000) == 0x4000 ? 100 : 10;
    /* Flow control remains off until pause negotiation is implemented. */
    uint16_t medium = 0x100;
    if (status & 0x2000) medium |= 2;
    if (s == 1000) medium |= 9;
    if (s == 100) medium |= 0x200;
    if (ax->medium != medium) {
        uint8_t usb;
        if (reg_read(ax, AX_ACCESS_MAC, AX_PHYSICAL_LINK_STATUS, 1, &usb)) return -1;
        const uint8_t queues[4][5] = {
            {7, 0x4f, 0, 0x12, 0xff}, {7, 0x20, 3, 0x16, 0xff},
            {7, 0xae, 7, 0x18, 0xff}, {7, 0xcc, 0x4c, 0x18, 8}
        };
        unsigned q = s == 1000 && (usb & 4) ? 0 :
                     s == 1000 && (usb & 2) ? 1 :
                     s == 100 && (usb & 6) ? 2 : 3;
        if (mac_write16(ax, AX_MEDIUM_STATUS_MODE, 0) ||
            mac_write16(ax, AX_RX_CTL, 0) ||
            reg_write(ax, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, queues[q]) ||
            mac_write16(ax, AX_RX_CTL, RX_CTL_DEFAULT)) return -1;
        /* Wait for TX FIFO readiness, bounded to approximately 100 ms. */
        unsigned i;
        for (i = 0; i < 10; i++) {
            uint8_t fifo[4];
            if (control(ax, 1, 0x81, 0x8c, 0, 4, fifo)) return -1;
            if (!(le32(fifo) & 0x40000000u)) break;
            sleep_ms(10);
        }
        if (i == 10 || mac_write16(ax, AX_MEDIUM_STATUS_MODE, medium)) return -1;
        ax->rx_frames = 0;
        ax->medium = medium;
    }
    if (speed) *speed = s;
    return 1;
}

int ax88179_send(Ax88179 *ax, const void *frame, int length)
{
    if (!ax || !ax->medium || !frame || length < 14 || length > 1518) {
        return -1;
    }
    /*
     * Two little-endian words in front of the frame: the length, and a
     * TSO/padding control word (TSO disabled here). Without them the adapter accepts
     * the transfer and puts nothing on the wire.
     */
    const int wire_length = length < 60 ? 60 : length;
    const uint32_t hdr0 = (uint32_t)wire_length;
    const uint32_t hdr1 = (wire_length + 8) % ax->tx_maxpacket == 0 ? 0x80008000u : 0;
    g_tx[0] = (uint8_t)(hdr0 & 0xFF);
    g_tx[1] = (uint8_t)((hdr0 >> 8) & 0xFF);
    g_tx[2] = (uint8_t)((hdr0 >> 16) & 0xFF);
    g_tx[3] = (uint8_t)((hdr0 >> 24) & 0xFF);
    for (int i = 0; i < 4; i++) g_tx[4+i] = (uint8_t)(hdr1 >> (8*i));
    memset(g_tx + 8, 0, (size_t)wire_length);
    memcpy(g_tx + 8, frame, (size_t)length);
    /* Like Linux usbnet: terminate an exact-multiple transfer with a
     * short USB packet. Header padding flag alone leaves the stream open. */
    int transfer_length = wire_length + 8;
    if (hdr1) g_tx[transfer_length++] = 0;
    DCFlushRange(g_tx, sizeof(g_tx));

    const int32_t r = UhsSubmitBulkRequest(&ax->handle, ax->if_handle, ax->ep_out,
                                           UHS_DIR_OUT, g_tx, transfer_length, 1000);
    ax->last_bulk = r;
    return r == transfer_length ? 0 : -1;
}

static int
ax88179_receive_buffered_impl(Ax88179 *ax, void *frame, int max_length)
{
    if (!ax || !frame || max_length < 14) {
        return -1;
    }

    while (ax->rx_next < ax->rx_frames) {
        const uint8_t *desc =
            g_rx + ax->rx_hdr_offset + 4 * ax->rx_next;

        const uint32_t d = le32(desc);
        ax->rx_next++;

        const int length =
            (int)((d >> 16) & 0x1FFF);

        /*
         * AX88179 bulk metadata contains dummy descriptors with
         * zero length. They consume a descriptor slot but no frame data.
         */
        if (length == 0) {
            continue;
        }

        const int padded =
            (length + 7) & ~7;

        if (ax->rx_pos + padded >
            (int)ax->rx_hdr_offset) {
            ax->rx_frames = 0;
            return -1;
        }

        const uint8_t *packet =
            g_rx + ax->rx_pos + 2;

        /*
         * Two bytes of AX alignment precede the ethernet header and the
         * hardware delivers the four-byte FCS after the ethernet frame.
         */
        const int packet_len =
            length - 2 - 4;

        ax->rx_pos += padded;

        if ((d & (AX_RXHDR_CRC_ERR |
                  AX_RXHDR_DROP_ERR)) ||
            packet_len < 14) {
            continue;
        }

        if (packet_len > max_length) {
            continue;
        }

        memcpy(frame,
               packet,
               (size_t)packet_len);

        return packet_len;
    }

    return 0;
}

int
ax88179_receive_buffered(
    Ax88179 *ax,
    void *frame,
    int max_length)
{
    return ax88179_receive_buffered_impl(
        ax,
        frame,
        max_length);
}

int
ax88179_receive(
    Ax88179 *ax,
    void *frame,
    int max_length,
    int timeout_us)
{
    if (!ax || !frame ||
        max_length < 14 ||
        timeout_us < 0) {
        return -1;
    }

    if (!ax->medium) {
        return 0;
    }

    /*
     * Always consume anything remaining from the previous USB aggregate
     * before asking UHS for another bulk transfer.
     */
    int n =
        ax88179_receive_buffered_impl(
            ax,
            frame,
            max_length);

    if (n != 0) {
        return n;
    }

    DCFlushRange(g_rx, sizeof(g_rx));

    const OSTime t0 =
        OSGetTime();

    const int32_t got =
        UhsSubmitBulkRequest(
            &ax->handle,
            ax->if_handle,
            ax->ep_in,
            UHS_DIR_IN,
            g_rx,
            sizeof(g_rx),
            timeout_us);

    ax->last_bulk_us =
        (uint32_t)OSTicksToMicroseconds(
            OSGetTime() - t0);

    ax->last_bulk = got;

    if (got == AX_UHS_NO_DATA) {
        return 0;
    }

    if (got < 0) {
        return -1;
    }

    if (got == 0) {
        return 0;
    }

    if (got < 4 ||
        got > (int32_t)sizeof(g_rx)) {
        return -1;
    }

    DCInvalidateRange(
        g_rx,
        sizeof(g_rx));

    ax->rx_len = (int)got;

    const uint8_t *trailer =
        g_rx + ax->rx_len - 4;

    const uint32_t rx_hdr =
        le32(trailer);

    ax->rx_frames =
        (int)(rx_hdr & 0xFFFF);

    ax->rx_hdr_offset =
        (rx_hdr >> 16) & 0xFFFF;

    ax->rx_next = 0;
    ax->rx_pos = 0;

    if (ax->rx_frames == 0) {
        return 0;
    }

    if (ax->rx_hdr_offset +
            4u * (uint32_t)ax->rx_frames >
        (uint32_t)ax->rx_len - 4u) {
        ax->rx_frames = 0;
        return -1;
    }

    return ax88179_receive_buffered_impl(
        ax,
        frame,
        max_length);
}

int32_t ax88179_last_bulk(const Ax88179 *ax)
{
    return ax ? ax->last_bulk : 0;
}

uint32_t ax88179_last_bulk_us(const Ax88179 *ax)
{
    return ax ? ax->last_bulk_us : 0;
}

void ax88179_endpoints(const Ax88179 *ax, int *in, int *out, int *irq)
{
    if (!ax) {
        return;
    }
    if (in)  *in  = ax->ep_in;
    if (out) *out = ax->ep_out;
    if (irq) *irq = ax->ep_irq;
}
