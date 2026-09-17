#ifndef DEBUG_PROGRESS_H
#define DEBUG_PROGRESS_H

/* Boot-watchdog checkpoints: each stage of the network bring-up sets a
 * bit. A watchdog thread in main.c dumps the mask via OSFatal if DHCP
 * never binds, so a silent hang says exactly where it stopped. */
void ax_mark(unsigned bit);
unsigned ax_progress_snapshot(void);

#define AX_MARK_WORKER_STARTED   0
#define AX_MARK_UDP_LOG          1
#define AX_MARK_IOSU_PATCH       2
#define AX_MARK_ADAPTER_OPEN     3
#define AX_MARK_QUEUE_INIT       4
#define AX_MARK_TCPIP_INIT       5
#define AX_MARK_THREAD_CREATED   6
#define AX_MARK_THREAD_ENTERED   7
#define AX_MARK_SETUP_CB         8
#define AX_MARK_NETIF_ADDED      9
#define AX_MARK_DHCP_STARTED     10
#define AX_MARK_NET_STARTED      11
#define AX_MARK_POLL_LOOP        12
#define AX_MARK_LINK_UP          13
#define AX_MARK_DHCP_BOUND       14
#define AX_MARK_SHIM_INSTALLED   15
#define AX_MARK_NET_STOP         16
#define AX_MARK_ADAPTER_CLOSED   17

#endif
