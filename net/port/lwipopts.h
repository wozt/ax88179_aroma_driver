#ifndef AX_LWIP_OPTS_H
#define AX_LWIP_OPTS_H
/* Threaded stack (NO_SYS=0): a tcpip thread owns the core; the driver
 * worker only moves frames in/out and everything else goes through
 * tcpip callbacks or the socket/netconn API. */
#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (256 * 1024)
/*
 * A native title may request SO_RCVBUF=65535. One full receive window
 * consumes roughly 45 Ethernet-sized pbufs, so the old pool of 48 left
 * effectively no headroom for ARP/DHCP/other traffic.
 */
#define PBUF_POOL_SIZE 512
#define PBUF_POOL_BUFSIZE 1600
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_DHCP 1
#define LWIP_IGMP 1
/* The real name in 2.2.x. DHCP_DOES_ARP_CHECK, which this used to set,
 * no longer exists anywhere in lwIP and was being ignored in silence. */
#define LWIP_DHCP_DOES_ACD_CHECK 1
/*
 * Headroom on the timeout pool, and not an arbitrary number.
 *
 * The default is the exact count of the modules compiled in -- with TCP,
 * reassembly, ARP, DHCP, ACD, IGMP and DNS that is eight. With only
 * the default-sized pool this leaves essentially no useful headroom, and
 * lwip_cyclic_timer reschedules each cyclic timer by allocating from
 * this same pool *and never checks whether it got one*. One failed
 * allocation and that timer is gone for the life of the stack. That is
 * what happened here: dhcp_fine_tmr died, so the DHCP client sent a
 * single DISCOVER, never retransmitted, and sat in SELECTING forever
 * while everything else looked healthy.
 */
#define MEMP_NUM_SYS_TIMEOUT 16
#define LWIP_AUTOIP 0
#define LWIP_UDP 1
#define LWIP_TCP 1
#define TCP_MSS 1460
/*
 * This is now only the global upper bound. nsysnet SO_RCVBUF controls
 * the actual per-PCB receive window through pcb->rcv_wnd_max.
 *
 * Native nsysnet accepts a maximum visible RCVBUF of 65535.
 */
#define TCP_WND 65535
#define TCP_SND_BUF (16 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define MEMP_NUM_TCP_PCB 32
#define MEMP_NUM_UDP_PCB 32
#define MEMP_NUM_TCP_SEG 64
/*
 * lwIP descriptors are private to the shim and are explicitly mapped from
 * the title-visible nsysnet descriptors through mapped_fd[].
 *
 * Keep the internal lwIP descriptor range starting at zero so the complete
 * 32-bit lwIP fd_set can be used. The title-visible descriptors remain real
 * nsysnet placeholders and therefore retain the native Wii U fd 4..31
 * exhaustion limit independently of the lwIP descriptor numbers.
 */
/*
 * Each queued UDP datagram consumes one struct netbuf.
 * lwIP defaults MEMP_NUM_NETBUF to only 2, which caused bursts such as
 * NEX/NNCS to keep the first two packets and silently drop the rest.
 */
#define MEMP_NUM_NETBUF 512
#define MEMP_NUM_NETCONN 32
#define LWIP_SOCKET_OFFSET 0
#define MEMP_NUM_TCPIP_MSG_INPKT 512
#define LWIP_DNS 1
#define LWIP_NETCONN 1
#define LWIP_SOCKET 1
/* Enter the stack directly under the core mutex rather than posting a
 * message and hoping: tcpip_callback() returns as soon as the message is
 * queued, not when it has run, which made the netif setup below silently
 * asynchronous. */
#define LWIP_TCPIP_CORE_LOCKING 1
/*
 * RX is produced by the AX/UHS worker, but protocol processing happens on
 * lwIP's tcpip thread.
 *
 * Keeping tcpip_input asynchronous is important for burst throughput:
 * the USB worker can immediately continue draining the AX88179 instead of
 * waiting for ethernet/IP/UDP processing under the core lock.
 *
 * The input message pool and mailbox are deliberately large enough for
 * the characterized burst tests.
 */
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0
#define TCPIP_MBOX_SIZE 512
#define TCPIP_THREAD_STACKSIZE (32 * 1024)
/*
 * Coreinit uses 0 as highest priority.
 *
 * sys_thread_new maps lwIP priority N to Coreinit 4+N.
 * Priority 1 therefore maps the tcpip thread to Coreinit priority 5.
 *
 * A characterization run with Coreinit priority 17 caused severe RX
 * regression, so keep the previously validated priority 5.
 */
#define TCPIP_THREAD_PRIO 1
#define DEFAULT_THREAD_STACKSIZE (16 * 1024)
#define DEFAULT_THREAD_PRIO 2
/*
 * A 65535-byte TCP receive window needs about 45 full-size TCP pbufs.
 * The old 16-entry mailbox independently capped application-visible
 * queued data near 16 * 1460 = 23360 bytes.
 */
#define DEFAULT_TCP_RECVMBOX_SIZE 64
#define DEFAULT_UDP_RECVMBOX_SIZE 64
#define DEFAULT_ACCEPTMBOX_SIZE 8
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_RCVBUF 1
#define LWIP_SO_SNDTIMEO 1
#define SO_REUSE 1
#define LWIP_TCP_KEEPALIVE 1
/* newlib (pulled in by wut headers) already defines struct timeval. */
#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define IP_REASSEMBLY 1
#define IP_FRAG 1
#define LWIP_STATS 1
#define LWIP_NETIF_LOOPBACK 0
#endif
