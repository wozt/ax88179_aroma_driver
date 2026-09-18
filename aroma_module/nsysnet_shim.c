/*
 * nsysnet shim: replaces nsysnet.rpl socket/DNS exports with our lwIP
 * stack, so a title's traffic goes through the USB adapter without any
 * change on its side.
 *
 * Two ABI gaps are bridged here:
 *  - sockaddr: nsysnet has no sa_len byte and a 16-bit family; lwIP uses
 *    the BSD layout (8-bit len + 8-bit family). Port/address offsets
 *    coincide, only the first two bytes are translated.
 *  - constants: MSG_* flags, SOL_SOCKET (-1), SO_NBIO/SO_BIO/SO_NONBLOCK,
 *    EAI_* and h_errno values all differ between nsysnet and lwIP.
 *
 * A call goes to lwIP only if this shim created that fd on lwIP; every
 * other fd is passed straight through to the original export. Routing on
 * ownership rather than on the fd number is not a detail: a process
 * already has sockets open when the patches go in -- the module's own
 * UDP log, and in the Aroma root process several of Aroma's own modules
 * -- and an earlier version that claimed every fd above a threshold
 * hijacked them all into a stack that did not exist yet. It took the log
 * out at the instant sendto was patched, and the console stopped booting.
 * Owning nothing by default is the only safe default.
 *
 * A title that asks for a socket before the adapter is up gets a real
 * console socket, so the worst case is the Wi-Fi behaviour we had before.
 *
 * Not covered (only usable with native sockets, not shim sockets):
 * sendto_multi_ex, recvfrom_multi, getaddrinfo_async(_rs),
 * gethostbyaddr, netconf_*, socket_lib_init/finish. NSSL itself still
 * requires a system fd; NSSLCreateConnection is bridged by promoting an
 * AX-backed socket to its reserved native placeholder at the TLS boundary.
 */
#include "nsysnet_shim.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <function_patcher/function_patching.h>
#include <whb/log.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "../net/ax_net.h"

/* ------------------------------------------------------------------ */
/* nsysnet ABI (from wut's headers, copied to avoid clashing with      */
/* lwIP's own socket headers)                                          */

struct nsn_fd_set { uint32_t fds_bits; };
struct nsn_timeval { long tv_sec, tv_usec; };
struct nsn_sockaddr { uint16_t sa_family; char sa_data[14]; };
struct nsn_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;   /* offsets 2/4 match lwIP's sockaddr_in */
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};
struct nsn_addrinfo {   /* wut order: canonname BEFORE addr (lwIP swaps) */
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    uint32_t ai_addrlen;
    char *ai_canonname;
    struct nsn_sockaddr *ai_addr;
    struct nsn_addrinfo *ai_next;
};

#define NSN_AF_INET      2
#define NSN_SOL_SOCKET   (-1)
#define NSN_SOL_TCP      6

/* Wii U SOL_SOCKET values. Never pass these numerically to lwIP without
 * explicit translation: identical numbers do not always mean identical
 * options (SO_TCPSACK=0x0200 is SO_REUSEPORT in lwIP). */
#define NSN_SO_REUSEADDR    0x0004
#define NSN_SO_KEEPALIVE    0x0008
#define NSN_SO_DONTROUTE    0x0010
#define NSN_SO_BROADCAST    0x0020
#define NSN_SO_LINGER       0x0080
#define NSN_SO_OOBINLINE    0x0100
#define NSN_SO_TCPSACK      0x0200
#define NSN_SO_WINSCALE     0x0400
#define NSN_SO_SNDBUF       0x1001
#define NSN_SO_RCVBUF       0x1002
#define NSN_SO_SNDLOWAT     0x1003
#define NSN_SO_RCVLOWAT     0x1004
#define NSN_SO_ERROR        0x1007
#define NSN_SO_TYPE         0x1008
#define NSN_SO_HOPCNT       0x1009
#define NSN_SO_MAXMSG       0x1010
#define NSN_SO_RXDATA       0x1011
#define NSN_SO_TXDATA       0x1012
#define NSN_SO_MYADDR       0x1013
#define NSN_SO_NBIO         0x1014
#define NSN_SO_BIO          0x1015
#define NSN_SO_NONBLOCK     0x1016
#define NSN_SO_UNKNOWN1019  0x1019
#define NSN_SO_UNKNOWN101A  0x101A
#define NSN_SO_UNKNOWN101B  0x101B
#define NSN_SO_NOSLOWSTART  0x4000
#define NSN_SO_RUSRBUF      0x10000

/* Wii U SOL_TCP values. */
#define NSN_TCP_ACKDELAYTIME 0x2001
#define NSN_TCP_NOACKDELAY   0x2002
#define NSN_TCP_MAXSEG       0x2003
#define NSN_TCP_NODELAY      0x2004
#define NSN_TCP_UNKNOWN      0x2005

#define NSN_MSG_OOB       0x0001
#define NSN_MSG_PEEK      0x0002
#define NSN_MSG_DONTWAIT  0x0020

#define NSN_IP_TOS             3
#define NSN_IP_TTL             4
#define NSN_IP_MULTICAST_IF    9
#define NSN_IP_MULTICAST_TTL   10
#define NSN_IP_MULTICAST_LOOP  11
#define NSN_IP_ADD_MEMBERSHIP  12
#define NSN_IP_DROP_MEMBERSHIP 13
#define NSN_IP_UNKNOWN         14

#define AX_NATIVE_PORT_FTP     21
#define AX_NATIVE_PORT_WIILOAD 4299

/* wut netdb values */
#define NSN_HOST_NOT_FOUND 1
#define NSN_TRY_AGAIN      2
#define NSN_NO_RECOVERY    3
#define NSN_NO_DATA        4
#define NSN_EAI_AGAIN      2
#define NSN_EAI_FAIL       4
#define NSN_EAI_FAMILY     5
#define NSN_EAI_MEMORY     6
#define NSN_EAI_NONAME     8
#define NSN_EAI_SERVICE    9
#define NSN_NI_NAMEREQD    0x0004

extern int h_errno;

static uint16_t nsn_ntohs(uint16_t v) { return lwip_ntohs(v); }

/* ------------------------------------------------------------------ */
/* helpers                                                             */

/* Reserve a native socket for every public fd. Native and lwIP allocators
 * may otherwise both return 16 and hijack an existing system socket. */
static atomic_uint open_mask;
static atomic_int mapped_fd[32];
static atomic_int accepting_sockets;
static atomic_uintptr_t probe_thread;
static atomic_int probe_owns_accepting;
static atomic_int shim_trace_level;
static atomic_int system_dns;
static atomic_int force_native;
static atomic_int ax_activity_pending;

/*
 * nsysnet exposes several socket options which lwIP either does not
 * implement per socket or exposes using a different API.
 *
 * Keep title-visible compatibility state indexed by the public nsysnet
 * descriptor. This state never changes packet ownership; it only preserves
 * the ABI behaviour observed from the native Wii U stack.
 */
struct compat_sock_state {
    atomic_uint sol_flags;

    atomic_int tcp_ackdelay;
    atomic_int tcp_noackdelay;
    atomic_int tcp_maxseg;
    atomic_int tcp_unknown;

    atomic_int linger_on;
    atomic_int linger_secs;

    atomic_int sndbuf;
    atomic_int rcvbuf;
};

static struct compat_sock_state compat_sock[32];

static void compat_state_reset(int fd)
{
    if (fd < 0 || fd >= 32)
        return;

    atomic_store(&compat_sock[fd].sol_flags, 0);

    atomic_store(&compat_sock[fd].tcp_ackdelay, 0);
    atomic_store(&compat_sock[fd].tcp_noackdelay, 0);
    atomic_store(&compat_sock[fd].tcp_maxseg, TCP_MSS);
    atomic_store(&compat_sock[fd].tcp_unknown, 0);

    atomic_store(&compat_sock[fd].linger_on, 0);
    atomic_store(&compat_sock[fd].linger_secs, 0);

    atomic_store(&compat_sock[fd].sndbuf, 8192);
    atomic_store(&compat_sock[fd].rcvbuf, 8192);
}

static void compat_state_reset_all(void)
{
    for (int fd = 0; fd < 32; fd++)
        compat_state_reset(fd);
}

static void compat_flag_set(int fd, unsigned flag, int enabled)
{
    if (fd < 0 || fd >= 32)
        return;

    if (enabled)
        atomic_fetch_or(&compat_sock[fd].sol_flags, flag);
    else
        atomic_fetch_and(&compat_sock[fd].sol_flags, ~flag);
}

static int compat_flag_value(int fd, unsigned flag)
{
    if (fd < 0 || fd >= 32)
        return 0;

    return (atomic_load(&compat_sock[fd].sol_flags) & flag)
        ? (int)flag
        : 0;
}

void nsysnet_shim_set_trace_level(int level)
{
    atomic_store(&shim_trace_level, level);
}

void nsysnet_shim_set_system_dns(int enabled)
{
    atomic_store(&system_dns, enabled ? 1 : 0);
}

void nsysnet_shim_set_force_native(int enabled)
{
    atomic_store(&force_native, enabled ? 1 : 0);
}

/*
 * Logging must never alter the errno produced by the socket operation
 * being instrumented. NEX often calls socketlasterr() immediately after
 * a non-blocking operation.
 *
 * force_native is deliberately silent: WHBLogPrintf itself uses network
 * sockets and could alter nsysnet's native per-thread last-error state.
 */
#define SHIM_TRACE(level, fmt, ...)                                      \
    do {                                                                  \
        if (!atomic_load(&force_native) &&                                \
            atomic_load(&shim_trace_level) >= (level)) {                  \
            int _saved_errno = errno;                                     \
            WHBLogPrintf("AX88179 shim: " fmt, ##__VA_ARGS__);            \
            errno = _saved_errno;                                         \
        }                                                                 \
    } while (0)

static int shim_accepts(void) { return atomic_load(&accepting_sockets); }

int nsysnet_shim_take_ax_activity(void)
{
    int expected = 1;
    return atomic_compare_exchange_strong(&ax_activity_pending,
                                          &expected,
                                          2);
}

static int stack_fd(int fd) { return atomic_load(&mapped_fd[fd]); }

static void track_fd(int fd, int lwfd) {
    compat_state_reset(fd);
    atomic_store(&mapped_fd[fd], lwfd);
    atomic_fetch_or(&open_mask, 1u << fd);
}
static void untrack_fd(int fd) { atomic_fetch_and(&open_mask, ~(1u << fd)); }
static int is_foreign(int fd) {
    return !(fd >= 0 && fd < 32 && (atomic_load(&open_mask) & (1u << fd)));
}
/* errno is per-thread in newlib. -1 marks a native call, whose error is
 * retrieved from nsysnet. lwIP calls use ordinary positive errno values. */
static int errno_to_nsn(int e) {
    static const int errors[] = {0, ENOBUFS, ETIMEDOUT, EISCONN, EOPNOTSUPP,
        ECONNABORTED, EWOULDBLOCK, ECONNREFUSED, ECONNRESET, ENOTCONN,
        EALREADY, EINVAL, EMSGSIZE, EPIPE, EDESTADDRREQ, -999, /* nsysnet ESHUTDOWN: absent in newlib */
        ENOPROTOOPT, EBUSY, ENOMEM, EADDRNOTAVAIL, EADDRINUSE,
        EAFNOSUPPORT, EINPROGRESS, EIO, ENOTSOCK};
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i)
        if (errors[i] == e) return i;
    switch (e) {
    case EFAULT: return 29; case ENETUNREACH: return 30;
    case EPROTONOSUPPORT: return 31; case EPROTOTYPE: return 32;
    case ENODEV: return 42; case EBADF: return 49;
    case ECANCELED: return 50; case EMFILE: return 51;
    default: return 23; /* EIO */
    }
}

static int msg_flags_to_lwip(int f)
{
    int r = 0;
    if (f & NSN_MSG_PEEK)     r |= MSG_PEEK;
    if (f & NSN_MSG_DONTWAIT) r |= MSG_DONTWAIT;
    if (f & NSN_MSG_OOB)      r |= MSG_OOB;
    return r;
}

/* nsysnet sockaddr (no len byte, 16-bit family) -> lwIP sockaddr_in.
 * Returns bytes written, 0 if the family is unsupported. */
static socklen_t sockaddr_to_lwip(struct sockaddr_in *out, const struct nsn_sockaddr *in,
                                  socklen_t inlen)
{
    if (!in || inlen < 8 || in->sa_family != NSN_AF_INET) return 0;
    const struct nsn_sockaddr_in *n = (const struct nsn_sockaddr_in *)in;
    memset(out, 0, sizeof(*out));
    out->sin_len = sizeof(*out);
    out->sin_family = AF_INET;
    out->sin_port = n->sin_port;
    out->sin_addr.s_addr = n->sin_addr;
    return sizeof(*out);
}


static socklen_t sockaddr_to_nsn(struct nsn_sockaddr *out, socklen_t *outlen,
                                 const struct sockaddr *in, socklen_t cap)
{
    if (!out || !outlen || cap < 16) return 0;
    struct nsn_sockaddr_in *n = (struct nsn_sockaddr_in *)out;
    const struct sockaddr_in *l = (const struct sockaddr_in *)in;
    memset(n, 0, sizeof(*n));
    if (in->sa_family != AF_INET) return 0;
    n->sin_family = NSN_AF_INET;
    n->sin_port = l->sin_port;
    n->sin_addr = l->sin_addr.s_addr;
    *outlen = 16;
    return 16;
}

static int set_nonblocking(int s, int on)
{
    return lwip_ioctl(s, FIONBIO, &on);
}

static int native_port(uint16_t net_port)
{
    uint16_t port = nsn_ntohs(net_port);
    return port == AX_NATIVE_PORT_FTP || port == AX_NATIVE_PORT_WIILOAD;
}

/*
 * Never forward a Wii U option number directly to lwIP.
 *
 * Even where values currently happen to match, keeping the translation
 * explicit protects us from collisions such as Wii U SO_TCPSACK 0x0200
 * versus lwIP SO_REUSEPORT 0x0200.
 */
static int socket_opt_to_lwip(int opt)
{
    switch (opt) {
    case NSN_SO_REUSEADDR: return SO_REUSEADDR;
    case NSN_SO_KEEPALIVE: return SO_KEEPALIVE;
    case NSN_SO_BROADCAST: return SO_BROADCAST;
    case NSN_SO_ERROR:     return SO_ERROR;
    case NSN_SO_TYPE:      return SO_TYPE;
    default:               return -1;
    }
}

static int ip_opt_to_lwip(int opt)
{
    switch (opt) {
    case NSN_IP_TOS:
        return IP_TOS;

    case NSN_IP_TTL:
        return IP_TTL;

    case NSN_IP_MULTICAST_IF:
        return IP_MULTICAST_IF;

    case NSN_IP_MULTICAST_TTL:
        return IP_MULTICAST_TTL;

    case NSN_IP_MULTICAST_LOOP:
        return IP_MULTICAST_LOOP;

    case NSN_IP_ADD_MEMBERSHIP:
        return IP_ADD_MEMBERSHIP;

    case NSN_IP_DROP_MEMBERSHIP:
        return IP_DROP_MEMBERSHIP;

    default:
        return -1;
    }
}

static int tcp_opt_to_lwip(int opt)
{
    switch (opt) {
    case NSN_TCP_NODELAY: return TCP_NODELAY;
    default:              return -1;
    }
}

static int compat_set_int(const void *optval, socklen_t optlen)
{
    if (!optval || optlen < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int compat_get_int(void *optval, socklen_t *optlen, int value)
{
    if (!optval || !optlen || *optlen < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }

    *(int *)optval = value;
    *optlen = sizeof(int);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sockets                                                             */

extern int (*real_socketclose)(int sockfd);

DECL_FUNCTION(int, socket, int domain, int type, int protocol)
{
    if (atomic_load(&force_native) ||
        !shim_accepts() ||
        !ax_net_stack_ready() ||
        domain != NSN_AF_INET) {
        SHIM_TRACE(1, "socket(%d,%d,%d) -> NATIVE%s",
                   domain, type, protocol,
                   atomic_load(&force_native) ? " forced" : "");
        errno = -1;
        return real_socket(domain, type, protocol);
    }
    errno = 0;
    int s = lwip_socket(domain, type, protocol);
    if (s < 0 && (type & ~0xF) != 0) {
        /* Some titles OR flag bits into the socket type; retry clean. */
        s = lwip_socket(domain, type & 0xF, protocol);
    }
    if (s >= 0) {
        /*
         * Fresh native Wii U sockets report SO_RCVBUF=8192.
         * Match the real lwIP receive queue to that default.
         */
        int native_rcvbuf = 8192;
        lwip_setsockopt(s,
                        SOL_SOCKET,
                        SO_RCVBUF,
                        &native_rcvbuf,
                        sizeof(native_rcvbuf));

        /* lwIP refuses broadcast sends without SO_BROADCAST, nsysnet does
         * not -- and whb's UDP logger never sets it. Allow it upfront or
         * every broadcast sendto (logs, LAN discovery) silently fails. */
        if ((type & 0xF) == SOCK_DGRAM) {
            int one = 1;
            lwip_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        }
    }
    if (s < 0) return s;
    int fd = real_socket(NSN_AF_INET, type & 0xF, protocol);
    if (fd < 0 || fd >= 32) {
        if (fd >= 0) real_socketclose(fd);
        lwip_close(s);
        errno = EMFILE;
        return -1;
    }
    track_fd(fd, s);

    int expected_activity = 0;
    atomic_compare_exchange_strong(&ax_activity_pending,
                                   &expected_activity,
                                   1);
    SHIM_TRACE(1, "socket(%d,%d,%d) -> AX fd=%d lwfd=%d", domain, type, protocol, fd, s);
    return fd;
}

DECL_FUNCTION(int, socketclose, int sockfd)
{
    if (is_foreign(sockfd)) {
        SHIM_TRACE(1, "close(fd=%d) -> NATIVE", sockfd);
        errno = -1;
        return real_socketclose(sockfd);
    }

    SHIM_TRACE(1, "close(fd=%d/lwfd=%d) -> AX",
               sockfd, stack_fd(sockfd));

    errno = 0;
    int r = lwip_close(stack_fd(sockfd));

    if (r == 0) {
        untrack_fd(sockfd);
        real_socketclose(sockfd);
    }

    return r;
}

DECL_FUNCTION(int, socketclose_all, void)
{
    if (!shim_accepts()) {
        SHIM_TRACE(1, "close_all -> NATIVE");
        errno = -1;
        return real_socketclose_all();
    }
    uint32_t m = atomic_exchange(&open_mask, 0);
    SHIM_TRACE(1, "close_all AX mask=%08x", (unsigned)m);
    for (int s = 0; s < 32; s++) {
        if (m & (1u << s)) {
            SHIM_TRACE(1, "close_all fd=%d/lwfd=%d", s, stack_fd(s));
            lwip_close(stack_fd(s));
        }
    }
    errno = -1;
    int r = real_socketclose_all();
    SHIM_TRACE(1, "close_all native rc=%d", r);
    return r;
}

DECL_FUNCTION(int, bind, int sockfd, const struct nsn_sockaddr *addr, socklen_t addrlen)
{
    if (is_foreign(sockfd)) { errno = -1; return real_bind(sockfd, addr, addrlen); }
    errno = 0;
    struct sockaddr_in l;
    if (!sockaddr_to_lwip(&l, addr, addrlen)) { errno = EAFNOSUPPORT; return -1; }
    uint16_t port = nsn_ntohs(l.sin_port);
    if (native_port(l.sin_port)) {
        SHIM_TRACE(1, "bind(fd=%d,port=%u) -> NATIVE reserved", sockfd, port);
        lwip_close(stack_fd(sockfd));
        untrack_fd(sockfd);
        errno = -1;
        return real_bind(sockfd, addr, addrlen);
    }
    SHIM_TRACE(1, "bind(fd=%d/lwfd=%d,port=%u) -> AX",
               sockfd, stack_fd(sockfd), port);
    int r = lwip_bind(stack_fd(sockfd), (struct sockaddr *)&l, sizeof(l));
    SHIM_TRACE(1, "bind fd=%d rc=%d errno=%d", sockfd, r, errno);
    return r;
}

DECL_FUNCTION(int, connect, int sockfd, const struct nsn_sockaddr *addr, socklen_t addrlen)
{
    if (is_foreign(sockfd)) {
        SHIM_TRACE(1, "connect(fd=%d) -> NATIVE", sockfd);
        errno = -1;
        return real_connect(sockfd, addr, addrlen);
    }

    SHIM_TRACE(1, "connect(fd=%d/lwfd=%d) -> AX",
               sockfd, stack_fd(sockfd));

    errno = 0;

    struct sockaddr_in l;

    if (!sockaddr_to_lwip(&l, addr, addrlen)) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    return lwip_connect(stack_fd(sockfd),
                        (struct sockaddr *)&l,
                        sizeof(l));
}

DECL_FUNCTION(int, listen, int sockfd, int backlog)
{
    if (is_foreign(sockfd)) {
        SHIM_TRACE(1, "listen(fd=%d,backlog=%d) -> NATIVE", sockfd, backlog);
        errno = -1;
        return real_listen(sockfd, backlog);
    }
    errno = 0;
    SHIM_TRACE(1, "listen(fd=%d/lwfd=%d,backlog=%d) -> AX",
               sockfd, stack_fd(sockfd), backlog);
    int r = lwip_listen(stack_fd(sockfd), backlog);
    SHIM_TRACE(1, "listen fd=%d rc=%d errno=%d", sockfd, r, errno);
    return r;
}

DECL_FUNCTION(int, accept, int sockfd, struct nsn_sockaddr *addr, socklen_t *addrlen)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_accept(sockfd, addr, addrlen);
    }
    errno = 0;
    if (addr && !addrlen) { errno = EFAULT; return -1; }
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    int s = lwip_accept(stack_fd(sockfd), addr ? (struct sockaddr *)&l : NULL, addr ? &llen : NULL);
    if (s >= 0) {
        /*
         * accept() can only produce a connection-oriented socket here.
         * Keep the native placeholder the same type as the accepted
         * lwIP socket so a later AX->native promotion remains valid.
         */
        int fd = real_socket(NSN_AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || fd >= 32) {
            if (fd >= 0) real_socketclose(fd);
            lwip_close(s); errno = EMFILE; return -1;
        }
        track_fd(fd, s);
        s = fd;
        if (addr) sockaddr_to_nsn(addr, addrlen, (struct sockaddr *)&l, *addrlen);
    }
    return s;
}

DECL_FUNCTION(int, shutdown, int sockfd, int how)
{
    if (is_foreign(sockfd)) { errno = -1; return real_shutdown(sockfd, how); }
    errno = 0;
    return lwip_shutdown(stack_fd(sockfd), how);
}

DECL_FUNCTION(int, send, int sockfd, const void *buf, size_t len, int flags)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_send(sockfd, buf, len, flags);
    }

    errno = 0;
    return (int)lwip_send(stack_fd(sockfd), buf, len,
                          msg_flags_to_lwip(flags));
}

DECL_FUNCTION(int, sendto, int sockfd, const void *buf, size_t len, int flags,
              const struct nsn_sockaddr *dest_addr, socklen_t addrlen)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
    }

    errno = 0;

    if (!dest_addr)
        return (int)lwip_sendto(stack_fd(sockfd), buf, len,
                                msg_flags_to_lwip(flags), NULL, 0);

    struct sockaddr_in l;

    if (!sockaddr_to_lwip(&l, dest_addr, addrlen)) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    return (int)lwip_sendto(stack_fd(sockfd), buf, len,
                            msg_flags_to_lwip(flags),
                            (struct sockaddr *)&l, sizeof(l));
}

DECL_FUNCTION(int, sendto_multi,
              int sockfd,
              const void *buf,
              int len,
              int flags,
              const struct nsn_sockaddr *destv,
              int dest_count)
{
    if (is_foreign(sockfd)) {
        SHIM_TRACE(1, "sendto_multi(fd=%d,count=%d) -> NATIVE",
                   sockfd, dest_count);
        errno = -1;
        return real_sendto_multi(sockfd, buf, len, flags,
                                 destv, dest_count);
    }

    errno = 0;

    for (int i = 0; i < dest_count; i++) {
        struct sockaddr_in l;

        if (!sockaddr_to_lwip(&l, &destv[i],
                              sizeof(struct nsn_sockaddr))) {
            errno = EAFNOSUPPORT;
            return -1;
        }

        int r = (int)lwip_sendto(stack_fd(sockfd),
                                 buf,
                                 len,
                                 msg_flags_to_lwip(flags),
                                 (struct sockaddr *)&l,
                                 sizeof(l));

        if (r < 0)
            return -1;

        if (r != len) {
            errno = EIO;
            return -1;
        }
    }

    return len;
}

DECL_FUNCTION(int, recv, int sockfd, void *buf, size_t len, int flags)
{
    if (is_foreign(sockfd)) { errno = -1; return real_recv(sockfd, buf, len, flags); }
    errno = 0;
    return (int)lwip_recv(stack_fd(sockfd), buf, len, msg_flags_to_lwip(flags));
}

DECL_FUNCTION(int, recvfrom, int sockfd, void *buf, size_t len, int flags,
              struct nsn_sockaddr *src_addr, socklen_t *addrlen)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
    }
    errno = 0;
    if (src_addr && !addrlen) { errno = EFAULT; return -1; }
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    int r = (int)lwip_recvfrom(stack_fd(sockfd), buf, len, msg_flags_to_lwip(flags),
                               src_addr ? (struct sockaddr *)&l : NULL,
                               src_addr ? &llen : NULL);
    if (r >= 0 && src_addr) sockaddr_to_nsn(src_addr, addrlen, (struct sockaddr *)&l, *addrlen);
    return r;
}

DECL_FUNCTION(int, recvfrom_ex,
              int sockfd,
              void *buf,
              int len,
              int flags,
              struct nsn_sockaddr *src_addr,
              socklen_t *addrlen,
              void *extra,
              int extra_len)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_recvfrom_ex(sockfd, buf, len, flags,
                                src_addr, addrlen, extra, extra_len);
    }

    errno = 0;

    if (src_addr && !addrlen) {
        errno = EFAULT;
        return -1;
    }

    /*
     * Wii U extension: flag 0x40 requests the received packet TTL.
     * We currently provide a compatibility placeholder rather than the
     * true received IP TTL.
     */
    if ((flags & 0x40) && extra && extra_len >= 1)
        ((uint8_t *)extra)[0] = 64;

    struct sockaddr_in l;
    socklen_t llen = sizeof(l);

    int r = (int)lwip_recvfrom(
        stack_fd(sockfd),
        buf,
        len,
        msg_flags_to_lwip(flags),
        src_addr ? (struct sockaddr *)&l : NULL,
        src_addr ? &llen : NULL);

    if (r >= 0 && src_addr) {
        sockaddr_to_nsn(src_addr, addrlen,
                        (struct sockaddr *)&l, *addrlen);
    }

    return r;
}

/* One pass of lwIP's select over the bits in `want`, with an immediate
 * or bounded timeout. Returns the count and rewrites the bit sets. */
static int lwip_select_bits(int nfds, uint32_t want,
                            uint32_t *r, uint32_t *w, uint32_t *e,
                            struct timeval *tv)
{
    fd_set lr, lw, le;
    FD_ZERO(&lr); FD_ZERO(&lw); FD_ZERO(&le);
    int ln = LWIP_SOCKET_OFFSET;
    for (int i = 0; i < nfds; i++) {
        if (!(want & (1u << i))) continue;
        int fd = stack_fd(i);
        if (fd >= ln) ln = fd + 1;
        if (r && (*r & (1u << i))) FD_SET(fd, &lr);
        if (w && (*w & (1u << i))) FD_SET(fd, &lw);
        if (e && (*e & (1u << i))) FD_SET(fd, &le);
    }
    int rc = lwip_select(ln, r ? &lr : NULL, w ? &lw : NULL, e ? &le : NULL, tv);
    if (rc >= 0) {
        if (r) *r = 0;
        if (w) *w = 0;
        if (e) *e = 0;
        for (int i = 0; i < nfds; i++) {
            if (!(want & (1u << i))) continue;
            int fd = stack_fd(i);
            if (r && FD_ISSET(fd, &lr)) *r |= 1u << i;
            if (w && FD_ISSET(fd, &lw)) *w |= 1u << i;
            if (e && FD_ISSET(fd, &le)) *e |= 1u << i;
        }
    }
    return rc;
}

/*
 * select() is the one call that can be handed fds from both stacks at
 * once -- a title that opened a socket while the adapter was still
 * coming up, then more once it was up. A pure set goes straight to the
 * stack that owns it. A mixed one cannot: waiting inside either stack
 * would ignore the other half and hang the title, so it is polled on
 * both until something fires or the caller's deadline passes.
 */
DECL_FUNCTION(int, select, int nfds, struct nsn_fd_set *readfds, struct nsn_fd_set *writefds,
              struct nsn_fd_set *exceptfds, struct nsn_timeval *timeout)
{
    /* nsysnet fds are 0..31; a larger nfds would just be EINVAL anyway. */
    if (nfds < 0 || nfds > 32 || (timeout &&
        (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000))) {
        errno = EINVAL; return -1;
    }
    uint32_t want = (readfds ? readfds->fds_bits : 0) |
                    (writefds ? writefds->fds_bits : 0) |
                    (exceptfds ? exceptfds->fds_bits : 0);
    want &= nfds == 32 ? UINT32_MAX : ((1u << nfds) - 1);
    uint32_t nat = want & ~atomic_load(&open_mask);

    if (nat == want) {
        errno = -1;
        return real_select(nfds, readfds, writefds, exceptfds, timeout);
    }
    errno = 0;

    uint32_t r = readfds ? readfds->fds_bits : 0;
    uint32_t w = writefds ? writefds->fds_bits : 0;
    uint32_t e = exceptfds ? exceptfds->fds_bits : 0;

    if (nat == 0) {
        struct timeval tv, *ptv = NULL;
        if (timeout) { tv.tv_sec = timeout->tv_sec; tv.tv_usec = timeout->tv_usec; ptv = &tv; }
        int rc = lwip_select_bits(nfds, want, readfds ? &r : NULL, writefds ? &w : NULL,
                                  exceptfds ? &e : NULL, ptv);
        if (rc >= 0) {
            if (readfds) readfds->fds_bits = r;
            if (writefds) writefds->fds_bits = w;
            if (exceptfds) exceptfds->fds_bits = e;
        }
        return rc;
    }

    /* Mixed: poll both, 2 ms apart, until one answers or time runs out. */
    OSTime deadline = 0;
    int bounded = timeout != NULL;
    if (bounded)
        deadline = OSGetTime() + OSMillisecondsToTicks((uint64_t)timeout->tv_sec * 1000ULL +
                                                       (uint64_t)timeout->tv_usec / 1000ULL);
    for (;;) {
        struct nsn_fd_set nr = { r & nat }, nw = { w & nat }, ne = { e & nat };
        struct nsn_timeval zero_n = { 0, 0 };
        int rc_n = real_select(nfds, readfds ? &nr : NULL, writefds ? &nw : NULL,
                               exceptfds ? &ne : NULL, &zero_n);

        if (rc_n < 0) { errno = -1; return -1; }
        uint32_t lr = r & ~nat, lw = w & ~nat, le = e & ~nat;
        struct timeval zero_l = { 0, 0 };
        int rc_l = lwip_select_bits(nfds, want & ~nat, readfds ? &lr : NULL,
                                    writefds ? &lw : NULL, exceptfds ? &le : NULL, &zero_l);

        if (rc_l < 0) return -1;
        if (rc_n > 0 || rc_l > 0) {
            if (readfds)   readfds->fds_bits   = (rc_n > 0 ? nr.fds_bits : 0) | (rc_l > 0 ? lr : 0);
            if (writefds)  writefds->fds_bits  = (rc_n > 0 ? nw.fds_bits : 0) | (rc_l > 0 ? lw : 0);
            if (exceptfds) exceptfds->fds_bits = (rc_n > 0 ? ne.fds_bits : 0) | (rc_l > 0 ? le : 0);
            return (rc_n > 0 ? rc_n : 0) + (rc_l > 0 ? rc_l : 0);
        }

        if (bounded && OSGetTime() >= deadline) {
            if (readfds) readfds->fds_bits = 0;
            if (writefds) writefds->fds_bits = 0;
            if (exceptfds) exceptfds->fds_bits = 0;
            return 0;
        }
        OSSleepTicks(OSMillisecondsToTicks(2));
    }
}

DECL_FUNCTION(int, getsockname, int sockfd, struct nsn_sockaddr *addr, socklen_t *addrlen)
{
    if (is_foreign(sockfd)) { errno = -1; return real_getsockname(sockfd, addr, addrlen); }
    errno = 0;
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    if (!addr || !addrlen) { errno = EFAULT; return -1; }
    int r = lwip_getsockname(stack_fd(sockfd), (struct sockaddr *)&l, &llen);
    if (r == 0) sockaddr_to_nsn(addr, addrlen, (struct sockaddr *)&l, *addrlen);
    return r;
}

DECL_FUNCTION(int, getpeername, int sockfd, struct nsn_sockaddr *addr, socklen_t *addrlen)
{
    if (is_foreign(sockfd)) { errno = -1; return real_getpeername(sockfd, addr, addrlen); }
    errno = 0;
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    if (!addr || !addrlen) { errno = EFAULT; return -1; }
    int r = lwip_getpeername(stack_fd(sockfd), (struct sockaddr *)&l, &llen);
    if (r == 0) sockaddr_to_nsn(addr, addrlen, (struct sockaddr *)&l, *addrlen);
    return r;
}

DECL_FUNCTION(int, setsockopt, int sockfd, int level, int optname,
              const void *optval, socklen_t optlen)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_setsockopt(sockfd, level, optname, optval, optlen);
    }

    errno = 0;

    if (level == NSN_SOL_SOCKET) {
        switch (optname) {
        case NSN_SO_NBIO:
            return set_nonblocking(stack_fd(sockfd), 1);

        case NSN_SO_BIO:
            return set_nonblocking(stack_fd(sockfd), 0);

        case NSN_SO_NONBLOCK:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            return set_nonblocking(stack_fd(sockfd),
                                   *(const int *)optval != 0);

        /*
         * Native nsysnet accepts these and reports the corresponding
         * option bit when queried. lwIP has no compatible per-socket
         * implementation for them in our build, so preserve their
         * title-visible state without forwarding the numeric value.
         */
        case NSN_SO_DONTROUTE:
        case NSN_SO_OOBINLINE:
        case NSN_SO_TCPSACK:
        case NSN_SO_WINSCALE:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            compat_flag_set(sockfd,
                            (unsigned)optname,
                            *(const int *)optval != 0);
            return 0;

        /*
         * nsysnet accepts this tuning hint but does not expose it back
         * through getsockopt().
         */
        case NSN_SO_NOSLOWSTART:
            return compat_set_int(optval, optlen);

        /*
         * Native behaviour measured on Wii U:
         *   fresh socket -> 8192
         *   -1 and 0..65535 -> accepted
         *   >=65536 -> EINVAL
         *
         * lwIP has no true per-socket TCP send-buffer setter, so this is
         * title-visible ABI state. Real send backpressure is tested
         * separately.
         */
        case NSN_SO_SNDBUF: {
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            int value = *(const int *)optval;

            if (value > 65535) {
                errno = EINVAL;
                return -1;
            }

            atomic_store(&compat_sock[sockfd].sndbuf, value);
            return 0;
        }

        /*
         * lwIP does have a real receive-buffer limit. Enforce the Wii U
         * range first, then apply it to lwIP and mirror the visible value.
         */
        case NSN_SO_RCVBUF: {
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            int value = *(const int *)optval;

            if (value > 65535) {
                errno = EINVAL;
                return -1;
            }

            int r = lwip_setsockopt(stack_fd(sockfd),
                                    SOL_SOCKET,
                                    SO_RCVBUF,
                                    &value,
                                    sizeof(value));

            if (r == 0)
                atomic_store(&compat_sock[sockfd].rcvbuf, value);

            return r;
        }

        /*
         * Native Wii U rejects attempts to set these low-water marks,
         * while getters return zero.
         */
        case NSN_SO_SNDLOWAT:
        case NSN_SO_RCVLOWAT:
            errno = ENOPROTOOPT;
            return -1;

        /*
         * WUT documents SO_MAXMSG as equivalent to TCP_MAXSEG.
         * lwIP has no per-socket MSS setter, so retain the requested
         * title-visible value.
         */
        case NSN_SO_MAXMSG:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            if (*(const int *)optval <= 0) {
                errno = EINVAL;
                return -1;
            }

            atomic_store(&compat_sock[sockfd].tcp_maxseg,
                         *(const int *)optval);
            return 0;

        /*
         * Native get after set {1,2} produced {SO_LINGER,2}; the on/off
         * field is returned as the option bit rather than literal 1.
         * WUT also documents the option as effectively having no network
         * effect, so only ABI state is retained here.
         */
        case NSN_SO_LINGER: {
            if (!optval || optlen < sizeof(struct linger)) {
                errno = EINVAL;
                return -1;
            }

            const struct linger *l =
                (const struct linger *)optval;

            if (l->l_linger < 0) {
                errno = EINVAL;
                return -1;
            }

            atomic_store(&compat_sock[sockfd].linger_on,
                         l->l_onoff != 0);

            atomic_store(&compat_sock[sockfd].linger_secs,
                         l->l_linger);

            return 0;
        }

        case NSN_SO_RXDATA:
        case NSN_SO_TXDATA:
        case NSN_SO_MYADDR:
            errno = ENOPROTOOPT;
            return -1;

        default: {
            int lwopt = socket_opt_to_lwip(optname);

            if (lwopt < 0) {
                errno = ENOPROTOOPT;
                return -1;
            }

            return lwip_setsockopt(stack_fd(sockfd),
                                   SOL_SOCKET,
                                   lwopt,
                                   optval,
                                   optlen);
        }
        }
    }

    if (level == IPPROTO_IP) {
        int lwopt = ip_opt_to_lwip(optname);

        if (lwopt < 0) {
            errno = ENOPROTOOPT;
            return -1;
        }

        return lwip_setsockopt(stack_fd(sockfd),
                               IPPROTO_IP,
                               lwopt,
                               optval,
                               optlen);
    }

    if (level == NSN_SOL_TCP) {
        switch (optname) {
        case NSN_TCP_NODELAY: {
            int lwopt = tcp_opt_to_lwip(optname);

            return lwip_setsockopt(stack_fd(sockfd),
                                   IPPROTO_TCP,
                                   lwopt,
                                   optval,
                                   optlen);
        }

        case NSN_TCP_ACKDELAYTIME:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            atomic_store(&compat_sock[sockfd].tcp_ackdelay,
                         *(const int *)optval);
            return 0;

        case NSN_TCP_NOACKDELAY:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            atomic_store(&compat_sock[sockfd].tcp_noackdelay,
                         *(const int *)optval);
            return 0;

        case NSN_TCP_UNKNOWN:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            atomic_store(&compat_sock[sockfd].tcp_unknown,
                         *(const int *)optval);
            return 0;

        case NSN_TCP_MAXSEG:
            if (compat_set_int(optval, optlen) != 0)
                return -1;

            if (*(const int *)optval <= 0) {
                errno = EINVAL;
                return -1;
            }

            atomic_store(&compat_sock[sockfd].tcp_maxseg,
                         *(const int *)optval);
            return 0;

        default:
            errno = ENOPROTOOPT;
            return -1;
        }
    }

    errno = ENOPROTOOPT;
    return -1;
}

DECL_FUNCTION(int, getsockopt, int sockfd, int level, int optname,
              void *optval, socklen_t *optlen)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_getsockopt(sockfd, level, optname, optval, optlen);
    }

    errno = 0;

    if (level == NSN_SOL_SOCKET) {
        switch (optname) {
        /*
         * Native nsysnet accepts SO_NBIO as a setter but does not expose
         * it through getsockopt. SO_NONBLOCK is the queryable variant.
         */
        case NSN_SO_NONBLOCK: {
            if (!optval || !optlen ||
                *optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }

            int fl =
                lwip_fcntl(stack_fd(sockfd), F_GETFL, 0);

            if (fl < 0)
                return -1;

            *(int *)optval =
                !!(fl & O_NONBLOCK);

            *optlen = sizeof(int);
            return 0;
        }

        case NSN_SO_NBIO:
        case NSN_SO_BIO:
        case NSN_SO_NOSLOWSTART:
            errno = ENOPROTOOPT;
            return -1;

        case NSN_SO_DONTROUTE:
        case NSN_SO_OOBINLINE:
        case NSN_SO_TCPSACK:
        case NSN_SO_WINSCALE:
            return compat_get_int(
                optval,
                optlen,
                compat_flag_value(sockfd,
                                  (unsigned)optname));

        case NSN_SO_RXDATA: {
            if (!optval || !optlen ||
                *optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }

            int v = 0;

            int r = lwip_ioctl(stack_fd(sockfd),
                               FIONREAD,
                               &v);

            if (r == 0) {
                *(int *)optval = v;
                *optlen = sizeof(int);
            }

            return r;
        }

        /*
         * Native returns zero for TXDATA on a fresh socket.
         * lwIP exposes no equivalent queued-send byte counter.
         */
        case NSN_SO_TXDATA:
            return compat_get_int(optval, optlen, 0);

        case NSN_SO_MYADDR:
            if (!optval || !optlen ||
                *optlen < sizeof(uint32_t)) {
                errno = EINVAL;
                return -1;
            }

            *(uint32_t *)optval = ax_net_ip4();
            *optlen = sizeof(uint32_t);
            return 0;

        case NSN_SO_SNDBUF:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(&compat_sock[sockfd].sndbuf));

        case NSN_SO_RCVBUF:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(&compat_sock[sockfd].rcvbuf));

        case NSN_SO_SNDLOWAT:
        case NSN_SO_RCVLOWAT:
        case NSN_SO_HOPCNT:
            return compat_get_int(optval,
                                  optlen,
                                  0);

        case NSN_SO_MAXMSG:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(
                    &compat_sock[sockfd].tcp_maxseg));

        case NSN_SO_LINGER: {
            if (!optval || !optlen ||
                *optlen < sizeof(struct linger)) {
                errno = EINVAL;
                return -1;
            }

            struct linger *l =
                (struct linger *)optval;

            int on =
                atomic_load(
                    &compat_sock[sockfd].linger_on);

            l->l_onoff =
                on ? NSN_SO_LINGER : 0;

            l->l_linger =
                atomic_load(
                    &compat_sock[sockfd].linger_secs);

            *optlen = sizeof(struct linger);
            return 0;
        }

        default: {
            int lwopt =
                socket_opt_to_lwip(optname);

            if (lwopt < 0) {
                errno = ENOPROTOOPT;
                return -1;
            }

            int rc =
                lwip_getsockopt(stack_fd(sockfd),
                                SOL_SOCKET,
                                lwopt,
                                optval,
                                optlen);

            if (rc == 0 &&
                optname == NSN_SO_ERROR &&
                optval &&
                optlen &&
                *optlen >= sizeof(int)) {
                *(int *)optval =
                    errno_to_nsn(*(int *)optval);
            }

            return rc;
        }
        }
    }

    if (level == IPPROTO_IP) {
        int lwopt = ip_opt_to_lwip(optname);

        if (lwopt < 0) {
            errno = ENOPROTOOPT;
            return -1;
        }

        return lwip_getsockopt(stack_fd(sockfd),
                               IPPROTO_IP,
                               lwopt,
                               optval,
                               optlen);
    }

    if (level == NSN_SOL_TCP) {
        switch (optname) {
        case NSN_TCP_NODELAY: {
            int lwopt = tcp_opt_to_lwip(optname);

            return lwip_getsockopt(stack_fd(sockfd),
                                   IPPROTO_TCP,
                                   lwopt,
                                   optval,
                                   optlen);
        }

        case NSN_TCP_ACKDELAYTIME:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(
                    &compat_sock[sockfd].tcp_ackdelay));

        case NSN_TCP_NOACKDELAY:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(
                    &compat_sock[sockfd].tcp_noackdelay));

        case NSN_TCP_MAXSEG:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(
                    &compat_sock[sockfd].tcp_maxseg));

        case NSN_TCP_UNKNOWN:
            return compat_get_int(
                optval,
                optlen,
                atomic_load(
                    &compat_sock[sockfd].tcp_unknown));

        default:
            errno = ENOPROTOOPT;
            return -1;
        }
    }

    errno = ENOPROTOOPT;
    return -1;
}

DECL_FUNCTION(int, socketlasterr, void)
{
    int result;

    /*
     * In reference/native mode never infer anything from newlib errno.
     * Return exactly what nsysnet would have returned without the shim.
     */
    if (atomic_load(&force_native)) {
        result = real_socketlasterr();
    } else if (errno < 0) {
        result = real_socketlasterr();
    } else {
        result = errno_to_nsn(errno);
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* NSSL bridge                                                         */

/*
 * NSSL only operates on native nsysnet descriptors. AX sockets therefore
 * stay on lwIP until a title hands one to NSSLCreateConnection(), at which
 * point the public placeholder fd is connected natively to the same peer
 * and becomes a normal nsysnet socket.
 */

/*
 * Promote an AX/lwIP-backed public socket to its native nsysnet
 * placeholder.
 *
 * The placeholder was created with the same socket type/protocol as the
 * lwIP socket. For NSSL we connect that native socket to the same peer,
 * then retire the lwIP side and make the public fd native permanently.
 *
 * Returns:
 *   1    promoted successfully
 *  -1    lwIP peer unavailable
 *  -2    unsupported peer address
 *  -100-N  native connect failed, where N is nsysnet socketlasterr()
 */
static int promote_ax_socket_to_native(int sockfd)
{
    if (is_foreign(sockfd))
        return 0;

    int lwfd = stack_fd(sockfd);

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));

    if (lwip_getpeername(lwfd,
                         (struct sockaddr *)&peer,
                         &peer_len) != 0)
        return -1;

    if (peer.sin_family != AF_INET)
        return -2;

    struct nsn_sockaddr_in native_peer;
    memset(&native_peer, 0, sizeof(native_peer));

    native_peer.sin_family = NSN_AF_INET;
    native_peer.sin_port = peer.sin_port;
    native_peer.sin_addr = peer.sin_addr.s_addr;

    /*
     * The native placeholder has never been connected. Connect it now
     * while the AX socket is still alive, so failure leaves the old path
     * untouched.
     */
    errno = -1;

    int rc = real_connect(sockfd,
                          (const struct nsn_sockaddr *)&native_peer,
                          sizeof(native_peer));

    if (rc != 0) {
        int native_error = real_socketlasterr();
        return -100 - native_error;
    }

    /*
     * Native connection succeeded. From this point the public fd belongs
     * to nsysnet. Clear ownership before closing lwIP so any concurrent
     * socket operation sees the new native path.
     */
    untrack_fd(sockfd);
    atomic_store(&mapped_fd[sockfd], -1);

    lwip_close(lwfd);

    return 1;
}

DECL_FUNCTION(int32_t, NSSLCreateConnection,
              int32_t context,
              const char *host,
              int32_t hostLength,
              int32_t options,
              int32_t sockfd,
              int32_t block)
{
    if (!is_foreign(sockfd))
        (void)promote_ax_socket_to_native(sockfd);

    return real_NSSLCreateConnection(context,
                                     host,
                                     hostLength,
                                     options,
                                     sockfd,
                                     block);
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */

DECL_FUNCTION(struct hostent *, gethostbyname, const char *name)
{
    if (!shim_accepts() || !ax_net_stack_ready()) return real_gethostbyname(name);
    struct hostent *h = lwip_gethostbyname(name);
    if (!h) {
        switch (h_errno) { /* lwIP -> nsysnet h_errno values */
        case 210: h_errno = NSN_HOST_NOT_FOUND; break;
        case 213: h_errno = NSN_TRY_AGAIN;      break;
        case 212: h_errno = NSN_NO_RECOVERY;    break;
        case 211: h_errno = NSN_NO_DATA;        break;
        }
    }
    return h;
}

/*
 * getaddrinfo can answer from either stack, and the caller frees the
 * list through the patched freeaddrinfo. Freeing a list nsysnet
 * allocated with our free() would corrupt the heap, so remember the
 * heads we built ourselves; anything else goes back to nsysnet.
 */
#define AI_OWNED_MAX 8
static _Atomic(void *) ai_owned[AI_OWNED_MAX];

static int ai_remember(void *head)
{
    for (int i = 0; i < AI_OWNED_MAX; i++) {
        void *empty = NULL;
        if (atomic_compare_exchange_strong(&ai_owned[i], &empty, head)) return 1;
    }
    return 0;
}
static int ai_is_ours(void *head)
{
    if (!head) return 0;
    for (int i = 0; i < AI_OWNED_MAX; i++) {
        void *expected = head;
        if (atomic_compare_exchange_strong(&ai_owned[i], &expected, NULL)) return 1;
    }
    return 0;
}

static int eai_to_nsn(int e)
{
    switch (e) { /* lwIP -> nsysnet EAI values */
    case 200: return NSN_EAI_NONAME;
    case 201: return NSN_EAI_SERVICE;
    case 203: return NSN_EAI_MEMORY;
    case 204: return NSN_EAI_FAMILY;
    default:  return NSN_EAI_FAIL;
    }
}

static int ai_flags_to_lwip(int f)
{
    int r = 0;
    if (f & 0x01) r |= 0x01; /* PASSIVE */
    if (f & 0x02) r |= 0x02; /* CANONNAME */
    if (f & 0x04) r |= 0x04; /* NUMERICHOST */
    if (f & 0x08) r |= 0x10; /* V4MAPPED (value differs) */
    if (f & 0x10) r |= 0x20; /* ALL */
    if (f & 0x20) r |= 0x40; /* ADDRCONFIG */
    return r;
}

DECL_FUNCTION(int, getaddrinfo, const char *node, const char *service,
              const struct nsn_addrinfo *hints, struct nsn_addrinfo **res)
{
    if (!shim_accepts() || !ax_net_stack_ready()) return real_getaddrinfo(node, service, hints, res);
    if (!res) return NSN_EAI_FAIL;
    *res = NULL;

    struct addrinfo lh, *lph = NULL;
    if (hints) {
        memset(&lh, 0, sizeof(lh));
        lh.ai_flags = ai_flags_to_lwip(hints->ai_flags);
        lh.ai_family = hints->ai_family;
        lh.ai_socktype = hints->ai_socktype;
        lh.ai_protocol = hints->ai_protocol;
        lph = &lh;
    }
    struct addrinfo *lres = NULL;
    int err = lwip_getaddrinfo(node, service, lph, &lres);
    if (err != 0) return eai_to_nsn(err);

    /* Convert the list: wut's addrinfo swaps ai_addr/ai_canonname and its
     * sockaddr has no length byte. One allocation per node. */
    struct nsn_addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *la = lres; la; la = la->ai_next) {
        size_t canonlen = la->ai_canonname ? strlen(la->ai_canonname) + 1 : 0;
        struct nsn_addrinfo *na = malloc(sizeof(*na) + sizeof(struct nsn_sockaddr_in) + canonlen);
        if (!na) { err = NSN_EAI_MEMORY; goto fail; }
        na->ai_flags = la->ai_flags;
        na->ai_family = la->ai_family;
        na->ai_socktype = la->ai_socktype;
        na->ai_protocol = la->ai_protocol;
        na->ai_next = NULL;
        char *storage = (char *)(na + 1);
        na->ai_addr = (struct nsn_sockaddr *)storage;
        sockaddr_to_nsn(na->ai_addr, &na->ai_addrlen, la->ai_addr, 16);
        storage += sizeof(struct nsn_sockaddr_in);
        if (canonlen) {
            memcpy(storage, la->ai_canonname, canonlen);
            na->ai_canonname = storage;
        } else {
            na->ai_canonname = NULL;
        }
        *tail = na;
        tail = &na->ai_next;
    }
    if (!ai_remember(head)) { err = NSN_EAI_MEMORY; goto fail; }
    lwip_freeaddrinfo(lres);
    *res = head;
    return 0;

fail:
    lwip_freeaddrinfo(lres);
    while (head) {
        struct nsn_addrinfo *next = head->ai_next;
        free(head);
        head = next;
    }
    return err;
}

DECL_FUNCTION(void, freeaddrinfo, struct nsn_addrinfo *res)
{
    if (!ai_is_ours(res)) { real_freeaddrinfo(res); return; }
    while (res) {
        struct nsn_addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

DECL_FUNCTION(int, getnameinfo, const struct nsn_sockaddr *addr, socklen_t addrlen,
              char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
    if (!shim_accepts()) return real_getnameinfo(addr, addrlen, host, hostlen, serv, servlen, flags);
    if (addrlen < sizeof(struct nsn_sockaddr_in)) return NSN_EAI_FAMILY;
    if (!addr || addr->sa_family != NSN_AF_INET) return NSN_EAI_FAMILY;
    const struct nsn_sockaddr_in *in = (const struct nsn_sockaddr_in *)addr;
    /* Numeric only: lwIP has no reverse lookup. */
    if (flags & NSN_NI_NAMEREQD) return NSN_EAI_NONAME;
    if (host) {
        const uint8_t *b = (const uint8_t *)&in->sin_addr;
        int n = snprintf(host, hostlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if (n < 0 || (socklen_t)n >= hostlen) return 12 /* NSN_EAI_BADHINTS/overflow */;
    }
    if (serv) {
        int n = snprintf(serv, servlen, "%u", nsn_ntohs(in->sin_port));
        if (n < 0 || (socklen_t)n >= servlen) return 12;
    }
    return 0;
}

DECL_FUNCTION(int *, get_h_errno, void)
{
    if (!shim_accepts()) return real_get_h_errno();
    return &h_errno;
}

DECL_FUNCTION(const char *, gai_strerror, int ecode)
{
    if (!shim_accepts()) return real_gai_strerror(ecode);
    switch (ecode) {
    case 0:  return "success";
    case 1:  return "address family not supported";
    case 2:  return "temporary failure in name resolution";
    case 3:  return "invalid flags";
    case 4:  return "non-recoverable failure in name resolution";
    case 5:  return "address family not supported";
    case 6:  return "out of memory";
    case 7:  return "no address associated with name";
    case 8:  return "name or service not known";
    case 9:  return "service not supported for socket type";
    case 10: return "socket type not supported";
    case 11: return "system error";
    case 12: return "invalid hints";
    case 13: return "unsupported protocol";
    case 14: return "buffer overflow";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* registration                                                        */

static PatchedFunctionHandle handles[2 * 24];
int handle_count = 0;

static int add_patch(function_replacement_data_t *data, const char *name, int process)
{
    PatchedFunctionHandle h = 0;
    bool patched = false;
    FunctionPatcherStatus st = FunctionPatcher_AddFunctionPatch(data, &h, &patched);
    if (st == FUNCTION_PATCHER_RESULT_SUCCESS) {
        if (handle_count < (int)(sizeof(handles) / sizeof(handles[0])))
            handles[handle_count++] = h;
        SHIM_TRACE(2, "%s %s proc=%d (%s)",
                   patched ? "patched" : "registered", name, process,
                   patched ? "now" : "waiting");
    } else {
        WHBLogPrintf("AX88179 shim: FAILED to add patch %s (proc %d): %s",
                     name, process, FunctionPatcher_GetStatusStr(st));
    }
    return st == FUNCTION_PATCHER_RESULT_SUCCESS ? 0 : -1;
}

/*
 * The game process only.
 *
 * The Wii U Menu is excluded because it mixes patched exports with
 * nsysnet internals we cannot cover (async DNS on system fds, NSSL), for
 * no benefit -- menu traffic can stay on the console's own network.
 *
 * The Aroma root process is excluded because it is not ours to take
 * over. This module runs inside it during boot, and so do Aroma's own
 * modules with their sockets already open. Patching it once cost the
 * console its boot: the log died the instant sendto was replaced and the
 * environment never reached its menu. Homebrew launched from Aroma runs
 * as the game process anyway, which is the case this exists for.
 */
#define SHIM_PATCH(name)                                                          \
    do {                                                                          \
        function_replacement_data_t d = REPLACE_FUNCTION_FOR_PROCESS(             \
            name, LIBRARY_NSYSNET, name, FP_TARGET_PROCESS_GAME);                 \
        if (add_patch(&d, #name, FP_TARGET_PROCESS_GAME) < 0) goto fail;           \
    } while (0)

static int installed;

int nsysnet_shim_install(void)
{
    if (installed) {
        atomic_store(&accepting_sockets, 1);
        return 0;
    }
    atomic_store(&accepting_sockets, 0);
    atomic_store(&open_mask, 0);
    for (int i = 0; i < AI_OWNED_MAX; ++i) atomic_store(&ai_owned[i], NULL);
    FunctionPatcherStatus st = FunctionPatcher_InitLibrary();
    if (st != FUNCTION_PATCHER_RESULT_SUCCESS) {
        WHBLogPrintf("AX88179 shim: function patcher unavailable (%s) -- "
                     "titles keep using the console network", FunctionPatcher_GetStatusStr(st));
        return -1;
    }
    uint32_t version = 0;
    FunctionPatcher_GetVersion(&version);
    SHIM_TRACE(2, "FunctionPatcher v%u", version);

    SHIM_PATCH(socket);
    SHIM_PATCH(socketclose);
    SHIM_PATCH(socketclose_all);
    SHIM_PATCH(bind);
    SHIM_PATCH(connect);
    SHIM_PATCH(listen);
    SHIM_PATCH(accept);
    SHIM_PATCH(shutdown);
    SHIM_PATCH(send);
    SHIM_PATCH(sendto);
    SHIM_PATCH(sendto_multi);
    SHIM_PATCH(recv);
    SHIM_PATCH(recvfrom);
    SHIM_PATCH(recvfrom_ex);
    SHIM_PATCH(select);
    SHIM_PATCH(setsockopt);
    SHIM_PATCH(getsockopt);
    SHIM_PATCH(getsockname);
    SHIM_PATCH(getpeername);
    SHIM_PATCH(socketlasterr);

    /*
     * NSSL requires a native descriptor. Promote AX-backed sockets to
     * their reserved nsysnet placeholder at the TLS boundary.
     */
    SHIM_PATCH(NSSLCreateConnection);

    if (!atomic_load(&system_dns)) {
        SHIM_PATCH(gethostbyname);
        SHIM_PATCH(getaddrinfo);
        SHIM_PATCH(freeaddrinfo);
        SHIM_PATCH(getnameinfo);
        SHIM_PATCH(get_h_errno);
        SHIM_PATCH(gai_strerror);
    } else {
        SHIM_TRACE(1, "DNS hooks skipped -> SYSTEM/Pretendo");
    }

    installed = 1;
    atomic_store(&accepting_sockets, 1);
    return 0;
fail:
    while (handle_count) FunctionPatcher_RemoveFunctionPatch(handles[--handle_count]);
    FunctionPatcher_DeInitLibrary();
    return -1;
}

/* Registration persists, but fd/heap/thread ownership does not. Never
 * infer that a surviving patch makes per-title lwIP state valid. */
void nsysnet_shim_stop_accepting(void) {
    atomic_store(&accepting_sockets, 0);
    atomic_store(&probe_owns_accepting, 0);
}
void nsysnet_shim_begin_title(void) {
    nsysnet_shim_stop_accepting();
    atomic_store(&probe_thread, 0);
    atomic_store(&open_mask, 0);
    atomic_store(&ax_activity_pending, 0);
    compat_state_reset_all();

    for (int i=0; i<AI_OWNED_MAX; ++i) atomic_store(&ai_owned[i], NULL);
}
int nsysnet_shim_begin_probe(void) {
    if (atomic_load(&probe_thread)) return -3;
    if (!ax_net_stack_ready() || !ax_net_address()) return -4;
    int was_accepting = atomic_load(&accepting_sockets);
    if (nsysnet_shim_install() < 0) return -1;
    atomic_store(&probe_thread, (uintptr_t)OSGetCurrentThread());
    atomic_store(&accepting_sockets, 1);
    atomic_store(&probe_owns_accepting, was_accepting ? 0 : 1);
    return 0;
}
int nsysnet_shim_end_probe(void) {
    if (atomic_load(&probe_thread) != (uintptr_t)OSGetCurrentThread()) return -3;
    /* The probe closes its sockets and frees its DNS results first. */
    if (atomic_load(&open_mask)) return -5;
    for (int i=0; i<AI_OWNED_MAX; ++i) if (atomic_load(&ai_owned[i])) return -5;
    if (atomic_load(&probe_owns_accepting))
        nsysnet_shim_stop_accepting();
    atomic_store(&probe_owns_accepting, 0);
    atomic_store(&probe_thread, 0);
    return 0;
}
