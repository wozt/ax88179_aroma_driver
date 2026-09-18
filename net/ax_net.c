#include "ax_net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <coreinit/messagequeue.h>
#include <whb/log.h>
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip_port_reset.h"
#include "netif/ethernet.h"
#include "../aroma_module/debug_progress.h"

/*
 * Threaded lwIP port: the tcpip thread owns the lwIP core, the caller of
 * ax_net_poll (the module's worker thread) exclusively owns UHS and the
 * adapter. tcpip_input processes RX synchronously under the core lock;
 * outgoing frames go through a slot queue to the worker, which alone
 * calls into the AX88179 driver.
 *
 * Anything that has to change lwIP state from the worker is done under
 * LOCK_TCPIP_CORE rather than through tcpip_callback. That is not a
 * style choice: tcpip_callback_with_block(f, ctx, 1) blocks only until
 * the message is *posted*, never until f has run. The first version of
 * this file posted setup_netif and read ctx.err on the next line, so it
 * read the untouched initial value every time and reported a failed
 * start -- while the tcpip thread wrote its result into a stack frame
 * that had already returned. The console showed it as a bring-up that
 * reached "dhcp started" and then never got an address.
 */

#define TX_SLOTS 32

static struct netif iface;
static uint8_t rx_frame[1600];
static uint8_t tx_pool[TX_SLOTS][1600];
static atomic_int tx_in_use[TX_SLOTS];
static OSMessageQueue tx_queue;
static OSMessage tx_storage[TX_SLOTS];
static int initialized, active, link_errors, last_link_up, netif_in_list;
static int session_lease_mode = 1;
static int session_lease_valid;
static uint32_t session_ip, session_netmask, session_gateway;
static int current_using_cached_lease;
/* Counters for the bring-up heartbeat: when DHCP does not complete, the
 * question is always the same -- are frames leaving, are frames coming
 * back, and does the driver report an error on either side. */
static uint32_t stat_rx_ok, stat_rx_err, stat_tx_q, stat_tx_drop, stat_tx_ok, stat_tx_err;
static uint32_t stat_in_ok, stat_in_drop, stat_beats, stat_rx_idle;

#define WIRETRACE_SLOTS 32

struct wiretrace_event {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t type;
    uint32_t word1;
    uint32_t word2;
    uint32_t word3;
};

static struct wiretrace_event wiretrace[WIRETRACE_SLOTS];
static unsigned wiretrace_write;
static unsigned wiretrace_read;

static uint16_t wire_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t wire_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           p[3];
}

static void wiretrace_udp16(const uint8_t *frame, int len)
{
    if (!frame || len < 14)
        return;

    int l2 = 14;
    uint16_t ethertype = wire_be16(frame + 12);

    /* Handle one 802.1Q VLAN tag too, just in case. */
    if (ethertype == 0x8100 && len >= 18) {
        ethertype = wire_be16(frame + 16);
        l2 = 18;
    }

    if (ethertype != 0x0800 || len < l2 + 20)
        return;

    const uint8_t *ip = frame + l2;

    if ((ip[0] >> 4) != 4)
        return;

    int ihl = (ip[0] & 0x0f) * 4;

    if (ihl < 20 || len < l2 + ihl + 8)
        return;

    if (ip[9] != 17)
        return;

    const uint8_t *udp = ip + ihl;
    uint16_t udp_len = wire_be16(udp + 4);

    /* UDP header 8 + NNCS message 16. */
    if (udp_len != 24 || len < l2 + ihl + udp_len)
        return;

    const uint8_t *data = udp + 8;

    /* Keep the newest events if the reader falls behind. */
    if (wiretrace_write - wiretrace_read >= WIRETRACE_SLOTS)
        wiretrace_read++;

    struct wiretrace_event *e =
        &wiretrace[wiretrace_write % WIRETRACE_SLOTS];

    e->src_ip   = wire_be32(ip + 12);
    e->src_port = wire_be16(udp + 0);
    e->dst_port = wire_be16(udp + 2);
    e->type     = wire_be32(data + 0);
    e->word1    = wire_be32(data + 4);
    e->word2    = wire_be32(data + 8);
    e->word3    = wire_be32(data + 12);

    wiretrace_write++;
}

void ax_net_wire_trace_drain(void)
{
    while (wiretrace_read != wiretrace_write) {
        struct wiretrace_event *e =
            &wiretrace[wiretrace_read % WIRETRACE_SLOTS];

        uint32_t ip = e->src_ip;

        WHBLogPrintf(
            "AXWIRE: UDP16 <- %u.%u.%u.%u:%u -> local:%u "
            "type=%u word1=%u word2=%08x word3=%08x",
            (ip >> 24) & 255,
            (ip >> 16) & 255,
            (ip >> 8) & 255,
            ip & 255,
            e->src_port,
            e->dst_port,
            e->type,
            e->word1,
            e->word2,
            e->word3);

        wiretrace_read++;
    }
}

extern uint32_t ax_fetch_calls, ax_fetch_timeouts, ax_fetch_infinite, ax_fetch_msgs;

/*
 * A 500 ms timer that only counts itself. If this stays at zero while
 * the rest of the stack looks alive, lwIP's timer wheel is not turning
 * -- which is exactly what a DHCP client stuck in SELECTING after a
 * single DISCOVER looks like from outside.
 */
static void beat_cb(void *arg)
{
    (void)arg;
    stat_beats++;
    sys_timeout(500, beat_cb, NULL);
}
static uint32_t last_link;
static char address[16];

static err_t send_frame(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len > sizeof(tx_pool[0])) return ERR_BUF;
    /* The core lock serializes producers (tcpip thread and RX worker). */
    int slot = -1;
    for (int i = 0; i < TX_SLOTS; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&tx_in_use[i], &expected, 1)) { slot = i; break; }
    }
    if (slot < 0) { stat_tx_drop++; return ERR_MEM; }
    if (pbuf_copy_partial(p, tx_pool[slot], p->tot_len, 0) != p->tot_len) {
        atomic_store(&tx_in_use[slot], 0);
        return ERR_BUF;
    }
    OSMessage m;
    memset(&m, 0, sizeof(m));
    m.message = (void *)(uintptr_t)slot;
    m.args[0] = p->tot_len;
    /* Never block here. The queue holds TX_SLOTS entries and at most
     * TX_SLOTS frames can be in flight, so it cannot actually fill up --
     * but this runs on the tcpip thread holding the core lock, and the
     * only thread that drains the queue may be waiting for that same
     * lock. Dropping a frame is what a full driver ring does anyway. */
    if (!OSSendMessage(&tx_queue, &m, OS_MESSAGE_FLAGS_NONE)) {
        atomic_store(&tx_in_use[slot], 0);
        stat_tx_drop++;
        return ERR_MEM;
    }
    stat_tx_q++;
    return ERR_OK;
}

static err_t init_interface(struct netif *n)
{
    n->name[0] = 'a'; n->name[1] = 'x';
    n->hostname = "wiiu-ax88179";
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, ax88179_mac(n->state), 6);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = send_frame;
    return ERR_OK;
}

struct setup_ctx {
    Ax88179 *ax;
    int link_up;
    int restore_lease;
    err_t err;
};

/* Runs on the worker thread with the core lock held. */
static void setup_netif(struct setup_ctx *c)
{
    ax_mark(AX_MARK_SETUP_CB);
    ip4_addr_t zero = {0};
    /* Re-entrant: a previous start whose stop never ran (callback
     * allocation failure, worker killed mid-path) leaves the netif in
     * lwIP's list, and re-adding it trips the "netif already added"
     * assert. Remove whatever is left before adding again. */
    if (netif_in_list) {
        if (netif_dhcp_data(&iface) != NULL) {
            dhcp_stop(&iface);
            dhcp_cleanup(&iface);
        }
        netif_remove(&iface);
        netif_in_list = 0;
    }
    memset(&iface, 0, sizeof(iface));
    if (!netif_add(&iface, &zero, &zero, &zero, c->ax, init_interface, tcpip_input)) {
        c->err = ERR_IF;
        return;
    }
    netif_in_list = 1;
    ax_mark(AX_MARK_NETIF_ADDED);
    netif_set_default(&iface);
    netif_set_up(&iface);
    if (c->link_up) netif_set_link_up(&iface);
    if (c->restore_lease) {
        ip4_addr_t ip={.addr=session_ip}, mask={.addr=session_netmask};
        ip4_addr_t gw={.addr=session_gateway};
        netif_set_addr(&iface, &ip, &mask, &gw);
    } else if (dhcp_start(&iface) != ERR_OK) {
        dhcp_cleanup(&iface);
        netif_remove(&iface);
        netif_in_list = 0;
        c->err = ERR_IF;
        return;
    }
    if (!c->restore_lease) ax_mark(AX_MARK_DHCP_STARTED);
    sys_timeout(500, beat_cb, NULL);
    c->err = ERR_OK;
}

int ax_net_start(Ax88179 *ax)
{
    if (active || !ax) return -1;
    if (!initialized) {
        srand((unsigned)OSGetTime());
        for (int i = 0; i < TX_SLOTS; i++) atomic_store(&tx_in_use[i], 0);
        OSInitMessageQueue(&tx_queue, tx_storage, TX_SLOTS);
        ax_mark(AX_MARK_QUEUE_INIT);
        tcpip_init(NULL, NULL);
        ax_mark(AX_MARK_TCPIP_INIT);
        initialized = 1;
    }
    struct setup_ctx ctx = { .ax=ax, .link_up=0, .restore_lease=session_lease_mode && session_lease_valid, .err=ERR_ARG };
    current_using_cached_lease = ctx.restore_lease;
    int speed;
    if (ax88179_link(ax, &speed) == 1) ctx.link_up = 1;
    LOCK_TCPIP_CORE();
    setup_netif(&ctx);
    UNLOCK_TCPIP_CORE();
    if (ctx.err != ERR_OK) return -1;
    active = 1;
    address[0] = 0;
    last_link = sys_now();
    last_link_up = ctx.link_up;
    link_errors = 0;
    return 0;
}

static void set_link(int up)
{
    LOCK_TCPIP_CORE();
    if (up) netif_set_link_up(&iface);
    else netif_set_link_down(&iface);
    UNLOCK_TCPIP_CORE();
}

static void drain_tx(Ax88179 *ax, int send)
{
    OSMessage m;
    while (OSReceiveMessage(&tx_queue, &m, OS_MESSAGE_FLAGS_NONE)) {
        int slot = (int)(uintptr_t)m.message;
        if (send) {
            if (ax88179_send(ax, tx_pool[slot], m.args[0]) == 0) stat_tx_ok++;
            else stat_tx_err++;
        }
        atomic_store(&tx_in_use[slot], 0);
    }
}

int ax_net_poll(void)
{
    if (!active) return -1;
    uint32_t now = sys_now();
    if ((uint32_t)(now - last_link) >= 500) {
        int speed, up = ax88179_link(iface.state, &speed);
        last_link = now;
        if (up == 1) ax_mark(AX_MARK_LINK_UP);
        if (up >= 0 && up != last_link_up) {
            set_link(up);
            last_link_up = up;
        }
        if (up < 0) link_errors++;
        else link_errors = 0;
        if (link_errors >= 3) return -2;
    }
    drain_tx(iface.state, 1);
    /* One bounded receive; timers live in the tcpip thread now. */
    /*
     * The wait is in microseconds, not milliseconds, and it costs about
     * a millisecond of IPC on top: asking for 100 took 985us, asking for
     * 1000 took 1990us. So this is a 5 ms wait.
     *
     * A bulk IN completes the moment the adapter has a frame, so the
     * wait only ever runs to the end on an idle link -- the frame is
     * handed to the stack as it arrives rather than on the next turn of
     * a polling loop, which is where the latency was coming from. The
     * cost is the other direction: a frame this stack wants to send on
     * its own initiative waits for the current read to finish, so this
     * value is a ceiling on transmit latency as much as a floor on
     * idle CPU. Five milliseconds is the compromise; a transmit-heavy
     * load wants the sending moved off this thread entirely.
     */
    int n = ax88179_receive(iface.state, rx_frame, sizeof(rx_frame), 5000);
    if (n > 0) stat_rx_ok++;
    else if (n < 0) stat_rx_err++;
    else stat_rx_idle++;
    if (n > 0) {
        /* Observe NNCS UDP packets before lwIP gets a chance to accept
         * or discard them. No logging is done here. */
        wiretrace_udp16(rx_frame, n);

        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (p) {
            if (pbuf_take(p, rx_frame, (u16_t)n) != ERR_OK || iface.input(p, &iface) != ERR_OK) {
                stat_in_drop++;
                pbuf_free(p);
            } else {
                stat_in_ok++;
            }
        } else {
            stat_in_drop++;
        }
    }
    /* CORE_LOCKING_INPUT makes input synchronous: generated replies are
     * now queued. Drain after every receive, also when a timer/socket
     * queued TX during an idle USB wait. No UHS call holds the core lock. */
    drain_tx(iface.state, 1);
    /* The read paces the loop when it actually waits. Sleep only if it
     * came back far too fast to have waited, so a UHS that ignores the
     * timeout cannot turn this into a spin. */
    if (n <= 0 && ax88179_last_bulk_us(iface.state) < 1500)
        OSSleepTicks(OSMillisecondsToTicks(2));
    return n;
}

const char *ax_net_address(void)
{
    if (!active || !netif_is_link_up(&iface)) return NULL;
    const ip4_addr_t *ip=netif_ip4_addr(&iface);
    if (!ip || !ip->addr) return NULL;
    if (session_lease_mode && !session_lease_valid &&
        dhcp_supplied_address(&iface)) {
        session_ip=ip->addr;
        session_netmask=netif_ip4_netmask(&iface)->addr;
        session_gateway=netif_ip4_gw(&iface)->addr;
        session_lease_valid=1;
    }
    return ip4addr_ntoa_r(ip,address,sizeof(address));
}

/*
 * One line saying whether anything is actually moving on the wire, and
 * what lwIP's DHCP client makes of it. dhcp= is lwIP's state number:
 * 6 REQUESTING, 10 BOUND, 12 BACKING_OFF, 0 OFF -- anything that is not
 * marching towards 10 says which half of the path is broken.
 */
void ax_net_status(char *out, unsigned size)
{
    struct dhcp *d = active ? netif_dhcp_data(&iface) : NULL;
    snprintf(out, size,
             "link=%d dhcp=%d/%u rx=%u/%u idle=%u in=%u drop=%u tx=%u/%u beats=%u "
             "fetch=%u fto=%u fmsg=%u finf=%u bulk=%ld/%uus",
             active ? netif_is_link_up(&iface) : -1,
             d ? (int)d->state : -1, d ? (unsigned)d->tries : 0u,
             stat_rx_ok, stat_rx_err, stat_rx_idle, stat_in_ok, stat_in_drop,
             stat_tx_ok, stat_tx_q, stat_beats,
             ax_fetch_calls, ax_fetch_timeouts, ax_fetch_msgs, ax_fetch_infinite,
             active ? (long)ax88179_last_bulk(iface.state) : 0L,
             active ? ax88179_last_bulk_us(iface.state) : 0u);
}

/* Check for HOME/MINUS display exit request — stub for now.
 * Can be extended with SDL2 display code later. */
int ax_display_check_exit(void)
{
    return 0;
}

/*
 * Drop everything the previous title left behind.
 *
 * A resident module's static data survives a title switch -- that is
 * the whole point of one -- but the process it ran in does not. The
 * tcpip thread, its mailbox, its mutex and every malloc made here die
 * with that address space. Keeping `initialized` set across the
 * boundary meant every title after the first came up with a netif and a
 * DHCP client and no stack thread at all: no timer ever fired, a single
 * DISCOVER went out, received frames piled up in a mailbox nobody was
 * draining, and the counters simply stopped where the previous title
 * had left them.
 *
 * lwIP's own state is static as well, so it comes back pointing into
 * pools that tcpip_init is about to reset. It is abandoned rather than
 * unwound: taking it apart politely would mean following exactly those
 * stale pointers. lwip_port_reset_all clears the heads -- the timeout
 * list, the pcb lists, netif_list, the ARP and socket tables -- and
 * mem_init/memp_init wipe the pools a moment later. Doing only some of
 * them is worse than doing none: the first version of this cleared
 * netif_list alone, and the second lwip_init walked a timeout list that
 * still pointed into the pool it had just rebuilt, which never returned
 * and left the console frozen on its boot logo.
 */
void ax_net_forget(void)
{
    lwip_port_reset_all();
    netif_in_list = 0;
    memset(&iface, 0, sizeof(iface));
    initialized = 0;
    active = 0;
    link_errors = last_link_up = 0;
    address[0] = 0;
    stat_rx_ok = stat_rx_err = stat_tx_q = stat_tx_drop = stat_tx_ok = stat_tx_err = 0;
    stat_in_ok = stat_in_drop = stat_beats = stat_rx_idle = 0;
    wiretrace_write = 0;
    wiretrace_read = 0;
    ax_fetch_calls = ax_fetch_timeouts = ax_fetch_infinite = ax_fetch_msgs = 0;
}

/* Non-zero once tcpip_init has run: the socket/DNS API is usable. */
void ax_net_set_session_lease_mode(int enabled)
{
    session_lease_mode = enabled;
    if (!enabled) session_lease_valid = 0;
}

int ax_net_stack_ready(void)
{
    return initialized;
}

int ax_net_using_cached_lease(void)
{
    return current_using_cached_lease;
}

/* Our IPv4 address in network byte order, 0 when none. */
uint32_t ax_net_ip4(void)
{
    if (!active) return 0;
    return netif_ip4_addr(&iface)->addr;
}

static void stop_netif(void)
{
    if (netif_dhcp_data(&iface)) {
        if (session_lease_mode && session_lease_valid) dhcp_stop(&iface);
        else dhcp_release_and_stop(&iface);
        dhcp_cleanup(&iface);
    }
    netif_set_down(&iface);
    netif_remove(&iface);
    netif_in_list = 0;
}

void ax_net_stop(void)
{
    if (!active) return;
    LOCK_TCPIP_CORE();
    stop_netif();
    UNLOCK_TCPIP_CORE();
    /* Drop whatever the tcpip thread queued for TX before the adapter
     * handle goes away. The stack is out of the netif by now, so nothing
     * new can arrive in the queue. */
    drain_tx(iface.state, 0);
    active = 0;
}
