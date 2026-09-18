#ifndef AX_LWIP_OPTS_H
#define AX_LWIP_OPTS_H
/* Threaded stack (NO_SYS=0): a tcpip thread owns the core; the driver
 * worker only moves frames in/out and everything else goes through
 * tcpip callbacks or the socket/netconn API. */
#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (256 * 1024)
#define PBUF_POOL_SIZE 48
#define PBUF_POOL_BUFSIZE 1600
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_DHCP 1
/* The real name in 2.2.x. DHCP_DOES_ARP_CHECK, which this used to set,
 * no longer exists anywhere in lwIP and was being ignored in silence. */
#define LWIP_DHCP_DOES_ACD_CHECK 1
/*
 * Headroom on the timeout pool, and not an arbitrary number.
 *
 * The default is the exact count of the modules compiled in -- with TCP,
 * reassembly, ARP, DHCP, ACD and DNS that is seven, six of which are
 * taken the moment the stack initialises. That leaves one slot, and
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
#define TCP_WND (16 * TCP_MSS)
#define TCP_SND_BUF (16 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_TCP_SEG 64
/* Our fds run 16..31. The top stays at 31 because a title's fd_set is a
 * single 32-bit word, and starting at 16 keeps them clear of the numbers
 * nsysnet hands out. The shim routes on ownership, not on the number, so
 * this is only a second line of defence -- but it means nsysnet would
 * have to hold seventeen sockets at once before the two ranges could
 * even touch. */
/*
 * Each queued UDP datagram consumes one struct netbuf.
 * lwIP defaults MEMP_NUM_NETBUF to only 2, which caused bursts such as
 * NEX/NNCS to keep the first two packets and silently drop the rest.
 */
#define MEMP_NUM_NETBUF 32
#define MEMP_NUM_NETCONN 16
#define LWIP_SOCKET_OFFSET 16
#define MEMP_NUM_TCPIP_MSG_INPKT 16
#define LWIP_DNS 1
#define LWIP_NETCONN 1
#define LWIP_SOCKET 1
/* Enter the stack directly under the core mutex rather than posting a
 * message and hoping: tcpip_callback() returns as soon as the message is
 * queued, not when it has run, which made the netif setup below silently
 * asynchronous. */
#define LWIP_TCPIP_CORE_LOCKING 1
/* RX is called by the worker, never an interrupt. Process it synchronously
 * under the core lock, so ICMP/ARP replies are queued before input returns.
 * Timers and socket API messages still run in the tcpip thread. */
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1
#define TCPIP_MBOX_SIZE 16
#define TCPIP_THREAD_STACKSIZE (32 * 1024)
#define TCPIP_THREAD_PRIO 1
#define DEFAULT_THREAD_STACKSIZE (16 * 1024)
#define DEFAULT_THREAD_PRIO 2
#define DEFAULT_TCP_RECVMBOX_SIZE 16
#define DEFAULT_UDP_RECVMBOX_SIZE 32
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
