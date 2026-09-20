#ifndef AX_NET_H
#define AX_NET_H
#include <stdint.h>
#include "../driver/ax88179.h"
/* Threaded lwIP port: start spawns the tcpip thread; poll moves frames
 * between the adapter and the stack and must be called regularly from
 * the owner thread. */
int ax_net_start(Ax88179 *ax);
/* -2: repeated control failures, close and reopen the adapter. */
int ax_net_poll(void);
const char *ax_net_address(void);
void ax_net_stop(void);

/*
 * APPLICATION_ENDS path: terminate tcpip_thread and abandon the outgoing
 * title's lwIP state. ax_net_forget() fully resets it in the next title.
 */
void ax_net_abandon_title(void);

/* Call once per title, before ax_net_start: the previous title's tcpip
 * thread died with its process and none of that state may be reused. */
void ax_net_forget(void);
/* One-line bring-up heartbeat: link, DHCP state and frame counters. */
void ax_net_status(char *out, unsigned size);
/* Check for HOME/MINUS display exit request (stub). */
int ax_display_check_exit(void);
/* Non-zero once the tcpip thread exists and lwip_* calls are safe. */
int ax_net_stack_ready(void);
/* Our IPv4 address, network byte order, 0 when down. */
uint32_t ax_net_ip4(void);
void ax_net_set_session_lease_mode(int enabled);
/* Non-zero when the current start restored the session DHCP lease. */
int ax_net_using_cached_lease(void);
#endif
