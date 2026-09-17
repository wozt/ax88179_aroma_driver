lwIP 2.2.1, STABLE-2_2_1_RELEASE
Commit 77dcd25a72509eb83f72b033d219b1d40cd8eb95
https://github.com/lwip-tcpip/lwip
Subset: core, api, headers, netif/ethernet.c. BSD license: COPYING.

## Local patch: per-process re-initialisation

lwip_init() assumes a stack that comes up once. It rebuilds the memory
pools but leaves every module's own statics pointing into them. This
module has to bring the stack up again in each Wii U title's process,
because the tcpip thread and everything else the port allocated die with
the previous one -- and a second lwip_init() on top of those stale
pointers walks recycled memory. The timeout list did it first, and that
walk never returned: a console frozen on its boot logo.

Eight `lwip_port_reset_*()` functions were therefore added, each sitting
in the file that owns the statics it clears:

    core/timeouts.c      next_timeout, current_timeout_due_time,
                         tcpip_tcp_timer_active
    core/udp.c           udp_pcbs
    core/tcp.c           the four pcb lists, tcp_ticks
    core/raw.c           raw_pcbs
    core/netif.c         netif_list, netif_default, netif_num
    core/ipv4/etharp.c   arp_table
    core/ipv4/ip4_frag.c reassdatagrams, ip_reass_pbufcount
    api/sockets.c        sockets[], select_cb_list, select_cb_ctr

They are additive -- nothing upstream calls them. The port declares them
in `net/port/lwip_port_reset.h` and calls them from `ax_net_forget()`,
before lwip_init(). Clearing only some of them is worse than clearing
none: a first attempt reset netif_list alone and still hung.

