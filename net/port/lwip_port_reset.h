#ifndef AX_LWIP_PORT_RESET_H
#define AX_LWIP_PORT_RESET_H

/*
 * Put lwIP back to its power-on state.
 *
 * lwip_init() rebuilds the memory pools, but every module keeps its own
 * statics -- the pcb lists, the timeout list, the ARP table, the socket
 * table -- pointing into the pools it just rebuilt. That is harmless
 * when a stack is brought up once. Here it is not: the tcpip thread and
 * everything else the port allocated die with the title's process, so
 * the stack has to come up again in the next one, and the second
 * lwip_init() would leave those pointers aimed at recycled memory.
 *
 * Each of these sits in the vendored lwIP source next to the statics it
 * clears; see vendor/lwip/UPSTREAM.md. Call this before lwip_init().
 */
void lwip_port_reset_timeouts(void);
void lwip_port_reset_udp(void);
void lwip_port_reset_tcp(void);
void lwip_port_reset_raw(void);
void lwip_port_reset_netif(void);
void lwip_port_reset_etharp(void);
void lwip_port_reset_ip4_frag(void);
void lwip_port_reset_sockets(void);

static inline void lwip_port_reset_all(void)
{
    lwip_port_reset_timeouts();
    lwip_port_reset_udp();
    lwip_port_reset_tcp();
    lwip_port_reset_raw();
    lwip_port_reset_netif();
    lwip_port_reset_etharp();
    lwip_port_reset_ip4_frag();
    lwip_port_reset_sockets();
}

#endif
