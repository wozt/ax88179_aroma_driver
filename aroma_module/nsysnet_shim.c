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
 * netconf_* is not covered yet.
 *
 * socket_lib_init/finish deliberately remain native: the shim still
 * requires nsysnet /dev/socket for reserved public descriptors and the
 * NSSL localhost bridge. NSSL itself still
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
#include <coreinit/title.h>
#include <nn/result.h>
#include <function_patcher/function_patching.h>
#include <whb/log.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
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

struct nsn_recvfrom_multi_buffers {
    void *buffer;
    unsigned int bufferlen;
    struct nsn_sockaddr *froms;
    unsigned int fromslen;
    int *results;
    unsigned int resultslen;
};

struct nsn_sendto_multi_ex_buffers {
    void *buffer;
    unsigned int bufferlen;
    int *datagram_lens;
    unsigned int datagram_lens_len;
    struct nsn_sockaddr *dests;
    unsigned int destslen;
    int *results;
    unsigned int resultslen;
};

#define NSN_IPC_ALIGN 0x40u

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
#define NSN_MSG_IP_RECVTTL 0x0040

#define NSN_IP_TOS             3
#define NSN_IP_TTL             4
#define NSN_IP_MULTICAST_IF    9
#define NSN_IP_MULTICAST_TTL   10
#define NSN_IP_MULTICAST_LOOP  11
#define NSN_IP_ADD_MEMBERSHIP  12
#define NSN_IP_DROP_MEMBERSHIP 13
#define NSN_IP_UNKNOWN         14

#define AX_FTP_PORT            21
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
#define NSN_EAI_INPROGRESS 15
#define NSN_NI_NAMEREQD    0x0004

extern int h_errno;

static uint16_t nsn_ntohs(uint16_t v) { return lwip_ntohs(v); }


static int nsn_ipc_ptr_ok(const void *p)
{
    return p && (((uintptr_t)p & (NSN_IPC_ALIGN - 1u)) == 0);
}

static uint32_t nsn_ipc_pad(uint32_t size)
{
    return (size + (NSN_IPC_ALIGN - 1u)) &
           ~(NSN_IPC_ALIGN - 1u);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */

/* Reserve a native socket for every public fd. Native and lwIP allocators
 * may otherwise both return 16 and hijack an existing system socket. */
static atomic_uint open_mask;
static atomic_int mapped_fd[32];
static atomic_int accepting_sockets;
static atomic_uintptr_t probe_thread;
static atomic_int probe_owns_accepting;

/*
 * Probe mode may start after production AX sockets already exist
 * (notably the module UDP logger). Remember that baseline so EndProbe
 * only diagnoses resources created after BeginProbe.
 */
static atomic_uint probe_baseline_open_mask;
static atomic_uint probe_baseline_ai_mask;

static atomic_int shim_trace_level;
static atomic_int system_dns;
static atomic_int force_native;
static atomic_int ax_activity_pending;
static atomic_int nssl_bridge_enabled;

/*
 * FTPiiU obtains the console address from nn::ac, which currently reports
 * the native/Wi-Fi address. When its control listener is routed through
 * AX, remember that address so its subsequent PASV bind(address, 0) can
 * be translated to the AX address as well.
 */
static atomic_uint ftp_native_bind_ip;
static atomic_int ftp_ax_redirect_active;

/*
 * FTPiiU starts before AX becomes ready, so its first port-21 listener can
 * legitimately be native/Wi-Fi. Once AX is ready we mark only that listener
 * for a one-shot poll failure. FTPiiU then destroys/recreates it itself.
 */
static atomic_uint ftp_restart_mask;

/*
 * A relay can finish before Nintendo NSSL destroys the corresponding
 * connection. Track the public native transport independently from the
 * relay slot so title teardown can still wake every outstanding NSSL
 * connection.
 */
static atomic_uint nssl_public_mask;
static atomic_int nssl_connection_by_fd[32];

static void nssl_relay_stop_all(void);
static void nssl_relay_detach_public_fd(int fd);

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

void nsysnet_shim_set_nssl_bridge(int enabled)
{
    atomic_store(&nssl_bridge_enabled, enabled ? 1 : 0);
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
    /*
     * REQUESTS_EXIT first flips accepting_sockets to zero. From that exact
     * point every title-visible socket must fall back to its native nsysnet
     * placeholder, even while the CPU2 worker is still retiring lwIP.
     */
    if (!shim_accepts())
        return 1;

    return !(fd >= 0 && fd < 32 &&
             (atomic_load(&open_mask) & (1u << fd)));
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
    return port == AX_NATIVE_PORT_WIILOAD;
}

/*
 * FTPiiU currently learns its local IPv4 address from nn::ac.
 *
 * While Wi-Fi is still enabled that gives the native address, e.g.
 * 192.168.2.124, even though the socket itself is owned by lwIP/AX at
 * 192.168.2.190.
 *
 * Translate:
 *
 *   FTP control:
 *       192.168.2.124:21 -> 192.168.2.190:21
 *
 * and later:
 *
 *   FTP PASV:
 *       192.168.2.124:0  -> 192.168.2.190:0
 *
 * Port zero is important: FTPiiU asks the stack for an ephemeral passive
 * port and then advertises the address returned by getsockname().
 */
static int ftp_rewrite_bind_to_ax(
    int sockfd,
    struct sockaddr_in *addr)
{
    if (!addr ||
        addr->sin_family != AF_INET)
        return 0;

    int type = 0;
    socklen_t type_len = sizeof(type);

    if (lwip_getsockopt(
            stack_fd(sockfd),
            SOL_SOCKET,
            SO_TYPE,
            &type,
            &type_len) != 0 ||
        type != SOCK_STREAM)
        return 0;

    uint32_t ax_ip = ax_net_ip4();

    if (ax_ip == 0)
        return 0;

    uint16_t port =
        nsn_ntohs(addr->sin_port);

    /*
     * First identify the FTP control listener.
     */
    if (port == AX_FTP_PORT) {
        atomic_store(
            &ftp_native_bind_ip,
            addr->sin_addr.s_addr);

        atomic_store(
            &ftp_ax_redirect_active,
            1);

        if (addr->sin_addr.s_addr != ax_ip) {
            addr->sin_addr.s_addr = ax_ip;
            return 1;
        }

        return 0;
    }

    /*
     * FTPiiU creates its PASV listener from the command socket's stored
     * local address, then binds it with port 0.
     */
    if (port == 0 &&
        atomic_load(&ftp_ax_redirect_active)) {

        uint32_t native_ip =
            atomic_load(&ftp_native_bind_ip);

        if (addr->sin_addr.s_addr == native_ip &&
            addr->sin_addr.s_addr != ax_ip) {

            addr->sin_addr.s_addr = ax_ip;
            return 1;
        }
    }

    return 0;
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

/*
 * Persistent exit trace.
 *
 * This hook runs before WUMS_HOOK_FINI_WUT_DEVOPTAB, so the SD devoptab
 * should still be alive even if nsysnet itself is currently shutting down.
 */
static void shim_exit_trace(const char *msg)
{
    FILE *f =
        fopen("fs:/vol/external01/ax88179_exit.log", "a");

    if (!f)
        return;

    fprintf(
        f,
        "[%llu] title=%016llx %s\n",
        (unsigned long long)
            OSTicksToMilliseconds(OSGetTime()),
        (unsigned long long)
            OSGetTitleID(),
        msg);

    fflush(f);
    fclose(f);
}

/*
 * Diagnostic hook only.
 *
 * WUT's __fini_wut_socket() does:
 *
 *   ACClose()
 *   ACFinalize()
 *   __wut_socket_fini_devoptab()
 *   socket_lib_finish()
 *
 * If "begin" appears but "end" does not, socket_lib_finish itself hangs.
 * If neither appears, the hang is earlier in __fini_wut_socket().
 */
DECL_FUNCTION(void, socket_lib_finish, void)
{
    shim_exit_trace("socket_lib_finish begin");

    real_socket_lib_finish();

    shim_exit_trace("socket_lib_finish end");
}

DECL_FUNCTION(NNResult, ACClose, void)
{
    shim_exit_trace("ACClose begin");

    NNResult result =
        real_ACClose();

    char line[96];

    snprintf(
        line,
        sizeof(line),
        "ACClose end result=%d",
        result.value);

    shim_exit_trace(line);

    return result;
}

DECL_FUNCTION(void, ACFinalize, void)
{
    shim_exit_trace("ACFinalize begin");

    real_ACFinalize();

    shim_exit_trace("ACFinalize end");
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

        /*
         * Fresh native TCP sockets expose SO_SNDBUF=8192 and the native
         * backpressure probe confirms that this value is operational, not
         * merely metadata.
         */
        if ((type & 0xF) == SOCK_STREAM) {
            int native_sndbuf = 8192;
            lwip_setsockopt(s,
                            SOL_SOCKET,
                            SO_SNDBUF,
                            &native_sndbuf,
                            sizeof(native_sndbuf));
        }

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
        if (sockfd >= 0 && sockfd < 32) {
            atomic_fetch_and(&ftp_restart_mask, ~(1u << sockfd));

            atomic_fetch_and(
                &nssl_public_mask,
                ~(1u << sockfd));

            atomic_store(
                &nssl_connection_by_fd[sockfd],
                -1);
        }

        /*
         * It may be a public descriptor previously handed to NSSL.
         * Detach it before nsysnet can recycle the descriptor number.
         */
        nssl_relay_detach_public_fd(sockfd);

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
    atomic_store(&ftp_restart_mask, 0);
    atomic_store(&nssl_public_mask, 0);

    for (int fd = 0; fd < 32; ++fd)
        atomic_store(
            &nssl_connection_by_fd[fd],
            -1);

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
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_bind(sockfd, addr, addrlen);
    }

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

    int ftp_rewritten =
        ftp_rewrite_bind_to_ax(
            sockfd,
            &l);

    if (ftp_rewritten) {
        SHIM_TRACE(
            1,
            "FTP bind fd=%d port=%u -> AX ip=%08x",
            sockfd,
            port,
            (unsigned)l.sin_addr.s_addr);
    }

    SHIM_TRACE(1, "bind(fd=%d/lwfd=%d,port=%u) -> AX",
               sockfd, stack_fd(sockfd), port);

    int r = lwip_bind(
        stack_fd(sockfd),
        (struct sockaddr *)&l,
        sizeof(l));
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
        int native_sndbuf = 8192;
        int native_rcvbuf = 8192;

        lwip_setsockopt(s,
                        SOL_SOCKET,
                        SO_SNDBUF,
                        &native_sndbuf,
                        sizeof(native_sndbuf));

        lwip_setsockopt(s,
                        SOL_SOCKET,
                        SO_RCVBUF,
                        &native_rcvbuf,
                        sizeof(native_rcvbuf));

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


DECL_FUNCTION(int, sendto_multi_ex,
              int sockfd,
              int flags,
              struct nsn_sendto_multi_ex_buffers *b,
              int count)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_sendto_multi_ex(sockfd, flags, b, count);
    }

    errno = 0;

    if (!b || count < 0) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0)
        return 0;

    if (!nsn_ipc_ptr_ok(b) ||
        !nsn_ipc_ptr_ok(b->buffer) ||
        !nsn_ipc_ptr_ok(b->datagram_lens) ||
        !nsn_ipc_ptr_ok(b->dests) ||
        !nsn_ipc_ptr_ok(b->results)) {
        errno = EINVAL;
        return -1;
    }

    if ((b->bufferlen & 0x3f) ||
        (b->datagram_lens_len & 0x3f) ||
        (b->destslen & 0x3f) ||
        (b->resultslen & 0x3f)) {
        errno = EINVAL;
        return -1;
    }

    uint64_t total_required = 0;

    for (int i = 0; i < count; i++) {
        if (b->datagram_lens[i] < 0) {
            errno = EINVAL;
            return -1;
        }

        total_required += (uint32_t)b->datagram_lens[i];

        if (total_required > UINT32_MAX) {
            errno = EINVAL;
            return -1;
        }
    }

    uint32_t data_required =
        nsn_ipc_pad((uint32_t)total_required);

    uint32_t lens_required =
        nsn_ipc_pad((uint32_t)count * sizeof(int));

    uint32_t dests_required =
        nsn_ipc_pad((uint32_t)count *
                    sizeof(struct nsn_sockaddr));

    uint32_t results_required =
        nsn_ipc_pad((uint32_t)count * sizeof(int));

    if (b->bufferlen < data_required ||
        b->datagram_lens_len < lens_required ||
        b->destslen < dests_required ||
        b->resultslen < results_required) {
        errno = EINVAL;
        return -1;
    }

    for (int i = 0; i < count; i++)
        b->results[i] = 0;

    uint8_t *data = b->buffer;
    int total = 0;

    for (int i = 0; i < count; i++) {
        int len = b->datagram_lens[i];

        struct sockaddr_in dest;

        if (!sockaddr_to_lwip(
                &dest,
                &b->dests[i],
                sizeof(struct nsn_sockaddr))) {
            b->results[i] = -1;
            errno = EAFNOSUPPORT;
            return -1;
        }

        int rc = (int)lwip_sendto(
            stack_fd(sockfd),
            data,
            len,
            msg_flags_to_lwip(flags),
            (struct sockaddr *)&dest,
            sizeof(dest));

        b->results[i] = rc;

        if (rc < 0)
            return -1;

        total += rc;
        data += len;
    }

    return total;
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
     * Native recvfrom_ex ABI:
     *
     * An extra/message output length of zero is rejected with EINVAL,
     * independently of MSG_IP_RECVTTL, and the pending datagram is not
     * consumed.
     *
     * Native characterization:
     *   extra_len=0 -> rc=-1, socketlasterr=11 (EINVAL)
     */
    if (extra_len == 0) {
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    uint8_t recv_ttl = 0;

    int r = (int)lwip_recvfrom_with_ttl(
        stack_fd(sockfd),
        buf,
        len,
        msg_flags_to_lwip(flags),
        src_addr ? (struct sockaddr *)&l : NULL,
        src_addr ? &llen : NULL,
        &recv_ttl);

    /*
     * Native nsysnet behaviour established by recvfrom_ex_probe:
     *
     *   - on successful receive, the supplied output area is zeroed
     *   - MSG_IP_RECVTTL (0x40) stores the real received IPv4 TTL in
     *     the first byte
     *   - bytes 1..extra_len-1 remain zero
     *
     * Leave extra untouched on receive failure, matching the observed
     * native failure case.
     */
    if (r >= 0 && extra && extra_len > 0) {
        memset(extra, 0, (size_t)extra_len);

        if (flags & NSN_MSG_IP_RECVTTL)
            ((uint8_t *)extra)[0] = recv_ttl;
    }

    if (r >= 0 && src_addr) {
        sockaddr_to_nsn(src_addr, addrlen,
                        (struct sockaddr *)&l, *addrlen);
    }

    return r;
}


DECL_FUNCTION(int, recvfrom_multi,
              int sockfd,
              int flags,
              struct nsn_recvfrom_multi_buffers *b,
              int datagram_len,
              int count,
              struct nsn_timeval *timeout)
{
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_recvfrom_multi(
            sockfd,
            flags,
            b,
            datagram_len,
            count,
            timeout);
    }

    errno = 0;

    if (!b || datagram_len < 0 || count < 0) {
        errno = EINVAL;
        return -1;
    }

    if (timeout &&
        (timeout->tv_sec < 0 ||
         timeout->tv_usec < 0 ||
         timeout->tv_usec >= 1000000)) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0)
        return 0;

    if (!nsn_ipc_ptr_ok(b) ||
        !nsn_ipc_ptr_ok(b->buffer) ||
        !nsn_ipc_ptr_ok(b->froms) ||
        !nsn_ipc_ptr_ok(b->results)) {
        errno = EINVAL;
        return -1;
    }

    if ((b->bufferlen & 0x3f) ||
        (b->fromslen & 0x3f) ||
        (b->resultslen & 0x3f)) {
        errno = EINVAL;
        return -1;
    }

    uint64_t data_bytes =
        (uint64_t)(uint32_t)datagram_len *
        (uint64_t)(uint32_t)count;

    if (data_bytes > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    uint32_t data_required =
        nsn_ipc_pad((uint32_t)data_bytes);

    uint32_t froms_required =
        nsn_ipc_pad((uint32_t)count *
                    sizeof(struct nsn_sockaddr));

    uint32_t results_required =
        nsn_ipc_pad((uint32_t)count * sizeof(int));

    if (b->bufferlen < data_required ||
        b->fromslen < froms_required ||
        b->resultslen < results_required) {
        errno = EINVAL;
        return -1;
    }

    for (int i = 0; i < count; i++)
        b->results[i] = 0;

    OSTime deadline = 0;

    if (timeout) {
        uint64_t timeout_us =
            (uint64_t)timeout->tv_sec * 1000000ULL +
            (uint64_t)timeout->tv_usec;

        deadline =
            OSGetTime() +
            (OSTime)OSMicrosecondsToTicks(timeout_us);
    }

    int lwfd = stack_fd(sockfd);

    for (int i = 0; i < count; i++) {
        if (timeout) {
            OSTime now = OSGetTime();

            if (now >= deadline) {
                b->results[i] = -1;
                errno = 0;
                return i;
            }

            uint64_t remaining_us =
                OSTicksToMicroseconds(deadline - now);

            struct timeval tv;
            tv.tv_sec =
                (long)(remaining_us / 1000000ULL);
            tv.tv_usec =
                (long)(remaining_us % 1000000ULL);

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(lwfd, &rfds);

            int ready = lwip_select(
                lwfd + 1,
                &rfds,
                NULL,
                NULL,
                &tv);

            if (ready == 0) {
                b->results[i] = -1;
                errno = 0;
                return i;
            }

            if (ready < 0) {
                b->results[i] = -1;
                return i == 0 ? -1 : i;
            }
        }

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        uint8_t *slot =
            (uint8_t *)b->buffer +
            ((size_t)i * (size_t)datagram_len);

        int rc = (int)lwip_recvfrom(
            lwfd,
            slot,
            datagram_len,
            msg_flags_to_lwip(flags),
            (struct sockaddr *)&from,
            &fromlen);

        b->results[i] = rc;

        if (rc < 0) {
            if (errno == EWOULDBLOCK ||
                errno == EAGAIN) {
                errno = 0;
                return i;
            }

            return i == 0 ? -1 : i;
        }

        socklen_t cap = sizeof(struct nsn_sockaddr);

        sockaddr_to_nsn(
            &b->froms[i],
            &cap,
            (struct sockaddr *)&from,
            sizeof(struct nsn_sockaddr));
    }

    return count;
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

    /*
     * Safe FTP native -> AX handoff.
     *
     * Never close another thread's descriptor behind its back. Returning
     * one EBADF from the listener poll makes FTPiiU run handleNetworkLost(),
     * close its own listener, and recreate it on the next loop iteration.
     */
    uint32_t ftp_restart =
        want & atomic_load(&ftp_restart_mask);

    if (ftp_restart) {
        atomic_fetch_and(&ftp_restart_mask, ~ftp_restart);

        if (readfds)
            readfds->fds_bits = 0;

        if (writefds)
            writefds->fds_bits = 0;

        if (exceptfds)
            exceptfds->fds_bits = 0;

        errno = EBADF;
        return -1;
    }

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
    if (is_foreign(sockfd)) {
        errno = -1;
        return real_getsockname(sockfd, addr, addrlen);
    }

    errno = 0;

    if (!addr || !addrlen) {
        errno = EFAULT;
        return -1;
    }

    struct sockaddr_in l;
    socklen_t llen = sizeof(l);

    memset(&l, 0, sizeof(l));

    int r = lwip_getsockname(
        stack_fd(sockfd),
        (struct sockaddr *)&l,
        &llen);

    if (r != 0)
        return r;

    /*
     * Native nsysnet behaviour:
     *
     * A UDP socket explicitly bound to INADDR_ANY still reports
     * 0.0.0.0 while unconnected. Once connected, getsockname()
     * exposes the local address chosen for that route.
     *
     * lwIP leaves the PCB local_ip as ANY, so emulate nsysnet only
     * when the socket is demonstrably connected.
     */
    if (l.sin_family == AF_INET &&
        l.sin_addr.s_addr == INADDR_ANY) {

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        memset(&peer, 0, sizeof(peer));

        if (lwip_getpeername(
                stack_fd(sockfd),
                (struct sockaddr *)&peer,
                &peer_len) == 0) {

            uint32_t ax_ip = ax_net_ip4();

            if (ax_ip != 0)
                l.sin_addr.s_addr = ax_ip;
        }

        /*
         * getpeername() on an unconnected socket may set errno.
         * A successful getsockname() must not leak that internal probe
         * error to the title.
         */
        errno = 0;
    }

    sockaddr_to_nsn(
        addr,
        addrlen,
        (struct sockaddr *)&l,
        *addrlen);

    return 0;
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

            /*
             * Negative values are native-valid ABI values but their real
             * queue semantics have not been characterized. Preserve them
             * visibly without changing lwIP's backing queue.
             *
             * Zero and positive values have measured network effects and
             * therefore update the real per-PCB lwIP send capacity.
             */
            if (value >= 0) {
                int r = lwip_setsockopt(stack_fd(sockfd),
                                        SOL_SOCKET,
                                        SO_SNDBUF,
                                        &value,
                                        sizeof(value));
                if (r != 0)
                    return r;
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
         * Native SO_TXDATA reports real outstanding TCP transmit data.
         * The patched lwIP socket layer exposes snd_lbb-lastack for this.
         */
        case NSN_SO_TXDATA: {
            int value = 0;
            socklen_t len = sizeof(value);

            int r = lwip_getsockopt(stack_fd(sockfd),
                                    SOL_SOCKET,
                                    SO_TXDATA,
                                    &value,
                                    &len);

            if (r != 0)
                return r;

            return compat_get_int(optval, optlen, value);
        }

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
 * NSSL/IOS-NSEC needs a real nsysnet descriptor.  In legacy mode we
 * promote the reserved public descriptor to a normal native Internet
 * connection.
 *
 * Bridge mode keeps the already-connected lwIP socket alive instead:
 *
 *       IOS-NSEC / Nintendo NSSL
 *                  |
 *          native public fd
 *                  |
 *             127.0.0.1
 *                  |
 *          private native fd
 *                  |
 *              relay thread
 *                  |
 *              lwIP socket
 *                  |
 *               AX88179
 *
 * The relay only sees TLS records.  Encryption, certificates, handshake
 * state and NSSL semantics remain entirely inside Nintendo's NSSL.
 */

#define NSSL_RELAY_MAX       8
#define NSSL_RELAY_BUF       4096
#define NSN_ERR_WOULDBLOCK   6
#define NSN_ERR_CONNRESET    8
#define NSN_ERR_NOTCONN      9
#define NSN_ERR_PIPE         13
#define NSSL_INVALID_FD      (-0x280010)

struct nssl_relay {
    atomic_int allocated;
    atomic_int ready;
    atomic_int stop;
    atomic_int done;

    /*
     * Public nsysnet descriptor handed to Nintendo NSSL.
     * The relay owns only the transport behind it, but lifecycle teardown
     * must still be able to interrupt this endpoint.
     */
    atomic_int public_fd;

    int native_fd;
    int lwfd;
    int error;

    uint64_t native_to_ax;
    uint64_t ax_to_native;

    OSThread thread __attribute__((aligned(0x40)));
    uint8_t stack[16 * 1024] __attribute__((aligned(0x40)));

    uint8_t to_ax[NSSL_RELAY_BUF];
    uint8_t to_native[NSSL_RELAY_BUF];
};

static struct nssl_relay nssl_relays[NSSL_RELAY_MAX];


/*
 * Keep NSSL lifetime separate from relay lifetime.
 */
static void nssl_track_public_connection(
    int fd,
    int32_t connection)
{
    if (fd < 0 ||
        fd >= 32 ||
        connection < 0)
        return;

    atomic_store(
        &nssl_connection_by_fd[fd],
        connection);

    atomic_fetch_or(
        &nssl_public_mask,
        1u << fd);
}


static void nssl_untrack_public_fd(int fd)
{
    if (fd < 0 ||
        fd >= 32)
        return;

    atomic_fetch_and(
        &nssl_public_mask,
        ~(1u << fd));

    atomic_store(
        &nssl_connection_by_fd[fd],
        -1);
}


/*
 * Protect against a stale descriptor number.
 *
 * Every bridge public fd must still be connected to 127.0.0.1. If the fd
 * has already disappeared/recycled, never touch whatever now owns that
 * number.
 */
static int nssl_public_fd_is_loopback(int fd)
{
    struct nsn_sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    memset(&peer, 0, sizeof(peer));

    errno = -1;

    if (real_getpeername(
            fd,
            (struct nsn_sockaddr *)&peer,
            &peer_len) != 0)
        return 0;

    if (peer.sin_family != NSN_AF_INET)
        return 0;

    const uint8_t *ip =
        (const uint8_t *)&peer.sin_addr;

    return ip[0] == 127 &&
           ip[1] == 0 &&
           ip[2] == 0 &&
           ip[3] == 1;
}


/*
 * Wake all transports belonging to one Nintendo NSSL handle.
 */
static int nssl_shutdown_all_tracked(
    const char *reason)
{
    uint32_t mask =
        atomic_load(&nssl_public_mask);

    if (!mask)
        return 0;

    WHBLogPrintf(
        "AX: NSSL %s tracked mask=%08x",
        reason,
        (unsigned)mask);

    int count = 0;

    for (int fd = 0; fd < 32; ++fd) {
        if (!(mask & (1u << fd)))
            continue;

        int32_t connection =
            atomic_load(
                &nssl_connection_by_fd[fd]);

        if (!nssl_public_fd_is_loopback(fd)) {
            nssl_untrack_public_fd(fd);
            continue;
        }

        WHBLogPrintf(
            "AX: NSSL %s shutdown conn=%d fd=%d",
            reason,
            connection,
            fd);

        real_shutdown(
            fd,
            SHUT_RDWR);

        count++;
    }

    return count;
}

static void nssl_relay_detach_public_fd(int fd)
{
    for (int i = 0; i < NSSL_RELAY_MAX; ++i) {
        struct nssl_relay *r =
            &nssl_relays[i];

        if (!atomic_load(&r->allocated))
            continue;

        int expected = fd;

        if (atomic_compare_exchange_strong(
                &r->public_fd,
                &expected,
                -1)) {
            /*
             * The title is closing the public transport normally.
             * Stop its private relay as well.
             */
            atomic_store(&r->stop, 1);
            return;
        }
    }
}

static int nssl_native_would_block(void)
{
    return real_socketlasterr() == NSN_ERR_WOULDBLOCK;
}

static int nssl_native_peer_closed(int error)
{
    return error == NSN_ERR_CONNRESET ||
           error == NSN_ERR_NOTCONN ||
           error == NSN_ERR_PIPE;
}

static void nssl_relay_reap(void)
{
    for (int i = 0; i < NSSL_RELAY_MAX; ++i) {
        struct nssl_relay *r = &nssl_relays[i];

        if (!atomic_load(&r->allocated))
            continue;

        if (!atomic_load(&r->done))
            continue;

        if (!OSIsThreadTerminated(&r->thread))
            continue;

        OSJoinThread(&r->thread, NULL);

        atomic_store(&r->public_fd, -1);
        atomic_store(&r->allocated, 0);
    }
}

static int nssl_relay_worker(int slot, const char **argv)
{
    (void)argv;

    if (slot < 0 || slot >= NSSL_RELAY_MAX)
        return -1;

    struct nssl_relay *r = &nssl_relays[slot];

    while (!atomic_load(&r->stop) &&
           !atomic_load(&r->ready)) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }

    if (atomic_load(&r->stop))
        goto out;

    int native_fd = r->native_fd;
    int lwfd = r->lwfd;

    size_t to_ax_off = 0;
    size_t to_ax_len = 0;

    size_t to_native_off = 0;
    size_t to_native_len = 0;

    int native_rx_open = 1;
    int ax_rx_open = 1;

    int ax_wr_shutdown = 0;
    int native_wr_shutdown = 0;

    int first_native_to_ax = 1;
    int first_ax_to_native = 1;

    while (!atomic_load(&r->stop)) {
        int progress = 0;

        /*
         * Flush encrypted bytes from Nintendo NSSL toward lwIP/AX.
         */
        if (to_ax_len) {
            errno = 0;

            int n = lwip_send(
                lwfd,
                r->to_ax + to_ax_off,
                to_ax_len,
                MSG_DONTWAIT);

            if (n > 0) {
                if (first_native_to_ax) {
                    WHBLogPrintf(
                        "AX: NSSL relay first NSSL->AX bytes=%d",
                        n);
                    first_native_to_ax = 0;
                }

                r->native_to_ax += (uint64_t)n;

                to_ax_off += (size_t)n;
                to_ax_len -= (size_t)n;

                if (!to_ax_len)
                    to_ax_off = 0;

                progress = 1;
            } else if (n < 0 &&
                       errno != EWOULDBLOCK &&
                       errno != EAGAIN) {
                r->error = 1000 + errno;
                break;
            }
        }

        /*
         * Pull encrypted TLS records from the local Nintendo NSSL socket.
         */
        if (!to_ax_len && native_rx_open) {
            int n = real_recv(
                native_fd,
                r->to_ax,
                sizeof(r->to_ax),
                NSN_MSG_DONTWAIT);

            if (n > 0) {
                to_ax_off = 0;
                to_ax_len = (size_t)n;
                progress = 1;
            } else if (n == 0) {
                native_rx_open = 0;
                progress = 1;
            } else if (!nssl_native_would_block()) {
                r->error = 2000 + real_socketlasterr();
                break;
            }
        }

        /*
         * Propagate NSSL's write-side EOF to the real AX connection, while
         * keeping the reverse direction alive for TLS close_notify/data.
         */
        if (!native_rx_open &&
            !to_ax_len &&
            !ax_wr_shutdown) {
            lwip_shutdown(lwfd, SHUT_WR);
            ax_wr_shutdown = 1;
            progress = 1;
        }

        /*
         * Flush encrypted Internet bytes back toward Nintendo NSSL.
         */
        if (to_native_len) {
            int n = real_send(
                native_fd,
                r->to_native + to_native_off,
                to_native_len,
                NSN_MSG_DONTWAIT);

            if (n > 0) {
                if (first_ax_to_native) {
                    WHBLogPrintf(
                        "AX: NSSL relay first AX->NSSL bytes=%d",
                        n);
                    first_ax_to_native = 0;
                }

                r->ax_to_native += (uint64_t)n;

                to_native_off += (size_t)n;
                to_native_len -= (size_t)n;

                if (!to_native_len)
                    to_native_off = 0;

                progress = 1;
            } else if (n < 0) {
                int native_error = real_socketlasterr();

                if (native_error == NSN_ERR_WOULDBLOCK) {
                    /* Try again later. */
                } else if (nssl_native_peer_closed(native_error)) {
                    /*
                     * NSSL has finished with this local transport.
                     * Any remaining server bytes are no longer useful.
                     */
                    to_native_len = 0;
                    break;
                } else {
                    r->error = 3000 + native_error;
                    break;
                }
            }
        }

        /*
         * Pull server TLS records from the already-connected lwIP socket.
         */
        if (!to_native_len && ax_rx_open) {
            errno = 0;

            int n = lwip_recv(
                lwfd,
                r->to_native,
                sizeof(r->to_native),
                MSG_DONTWAIT);

            if (n > 0) {
                to_native_off = 0;
                to_native_len = (size_t)n;
                progress = 1;
            } else if (n == 0) {
                ax_rx_open = 0;
                progress = 1;
            } else if (errno != EWOULDBLOCK &&
                       errno != EAGAIN) {
                r->error = 4000 + errno;
                break;
            }
        }

        /*
         * Remote server closed its write side: make NSSL observe EOF once
         * everything already received has been delivered locally.
         */
        if (!ax_rx_open &&
            !to_native_len &&
            !native_wr_shutdown) {
            real_shutdown(native_fd, SHUT_WR);
            native_wr_shutdown = 1;
            progress = 1;
        }

        if (!native_rx_open &&
            !ax_rx_open &&
            !to_ax_len &&
            !to_native_len)
            break;

        if (!progress)
            OSSleepTicks(OSMillisecondsToTicks(1));
    }

out:
    if (r->native_fd >= 0) {
        real_socketclose(r->native_fd);
        r->native_fd = -1;
    }

    if (r->lwfd >= 0) {
        lwip_close(r->lwfd);
        r->lwfd = -1;
    }

    WHBLogPrintf(
        "AX: NSSL relay end slot=%d nssl_to_ax=%llu ax_to_nssl=%llu err=%d",
        slot,
        (unsigned long long)r->native_to_ax,
        (unsigned long long)r->ax_to_native,
        r->error);

    /*
     * The public socket belongs to Nintendo NSSL/the title.
     *
     * Our relay is now finished, therefore keeping this descriptor number
     * is unsafe: nsysnet may recycle the number before title teardown.
     */
    atomic_store(&r->public_fd, -1);
    atomic_store(&r->done, 1);
    return 0;
}

static int nssl_relay_reserve(void)
{
    nssl_relay_reap();

    for (int i = 0; i < NSSL_RELAY_MAX; ++i) {
        int expected = 0;

        if (!atomic_compare_exchange_strong(
                &nssl_relays[i].allocated,
                &expected,
                1))
            continue;

        struct nssl_relay *r = &nssl_relays[i];

        atomic_store(&r->ready, 0);
        atomic_store(&r->stop, 0);
        atomic_store(&r->done, 0);
        atomic_store(&r->public_fd, -1);

        r->native_fd = -1;
        r->lwfd = -1;
        r->error = 0;

        r->native_to_ax = 0;
        r->ax_to_native = 0;

        if (!OSCreateThread(
                &r->thread,
                nssl_relay_worker,
                i,
                NULL,
                r->stack + sizeof(r->stack),
                sizeof(r->stack),
                17,
                OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
            atomic_store(&r->allocated, 0);
            return -1;
        }

        OSSetThreadName(
            &r->thread,
            "AX NSSL relay");

        OSResumeThread(&r->thread);

        return i;
    }

    return -1;
}

static void nssl_relay_stop_slot(int slot)
{
    if (slot < 0 || slot >= NSSL_RELAY_MAX)
        return;

    if (!atomic_load(&nssl_relays[slot].allocated))
        return;

    atomic_store(&nssl_relays[slot].stop, 1);
}

static void nssl_relay_stop_all(void)
{
    int any = 0;

    for (int i = 0; i < NSSL_RELAY_MAX; ++i) {
        struct nssl_relay *r =
            &nssl_relays[i];

        if (atomic_load(&r->allocated)) {
            /*
             * Only stop resources owned by the relay.
             *
             * The public socket was handed to Nintendo NSSL and must remain
             * under Nintendo NSSL/the title's ownership. Closing our private
             * localhost endpoint naturally makes its peer observe EOF.
             */
            atomic_store(&r->stop, 1);
            any = 1;
        }
    }

    if (!any)
        return;

    /*
     * Relay calls are nonblocking and check stop every iteration, so they
     * should terminate within a few milliseconds. Keep lifecycle cleanup
     * bounded anyway.
     */
    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(500);

    while (OSGetTime() < deadline) {
        int alive = 0;

        for (int i = 0; i < NSSL_RELAY_MAX; ++i) {
            if (!atomic_load(&nssl_relays[i].allocated))
                continue;

            if (!OSIsThreadTerminated(&nssl_relays[i].thread))
                alive = 1;
        }

        if (!alive)
            break;

        OSSleepTicks(OSMillisecondsToTicks(1));
    }

    nssl_relay_reap();
}

/*
 * Legacy fallback: connect the public native placeholder directly to the
 * same Internet peer, then discard lwIP.
 */
static int promote_ax_socket_to_native(int sockfd)
{
    if (is_foreign(sockfd))
        return 0;

    int lwfd = stack_fd(sockfd);

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    memset(&peer, 0, sizeof(peer));

    if (lwip_getpeername(
            lwfd,
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

    errno = -1;

    int rc = real_connect(
        sockfd,
        (const struct nsn_sockaddr *)&native_peer,
        sizeof(native_peer));

    if (rc != 0) {
        int native_error = real_socketlasterr();
        return -100 - native_error;
    }

    untrack_fd(sockfd);
    atomic_store(&mapped_fd[sockfd], -1);

    lwip_close(lwfd);

    return 1;
}


/*
 * Turn an AX-backed public socket into the NSSL side of a localhost tunnel.
 *
 * The lwIP socket is NOT closed. Ownership moves from open_mask/mapped_fd
 * to the relay slot.
 */
static int bridge_ax_socket_to_nssl(
    int sockfd,
    int *relay_slot_out)
{
    if (relay_slot_out)
        *relay_slot_out = -1;

    if (is_foreign(sockfd))
        return 0;

    int lwfd = stack_fd(sockfd);

    /*
     * Verify the AX side is already a connected IPv4 TCP socket.
     */
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    memset(&peer, 0, sizeof(peer));

    if (lwip_getpeername(
            lwfd,
            (struct sockaddr *)&peer,
            &peer_len) != 0)
        return -1;

    if (peer.sin_family != AF_INET)
        return -2;

    int slot = nssl_relay_reserve();

    if (slot < 0)
        return -3;

    int listener =
        real_socket(
            NSN_AF_INET,
            SOCK_STREAM,
            0);

    if (listener < 0) {
        nssl_relay_stop_slot(slot);
        return -4;
    }

    struct nsn_sockaddr_in local;
    memset(&local, 0, sizeof(local));

    local.sin_family = NSN_AF_INET;
    local.sin_port = 0;
    if (lwip_inet_pton(
            AF_INET,
            "127.0.0.1",
            &local.sin_addr) != 1) {
        real_socketclose(listener);
        nssl_relay_stop_slot(slot);
        return -5;
    }

    if (real_bind(
            listener,
            (const struct nsn_sockaddr *)&local,
            sizeof(local)) != 0) {
        int e = real_socketlasterr();
        real_socketclose(listener);
        nssl_relay_stop_slot(slot);
        return -100 - e;
    }

    if (real_listen(listener, 1) != 0) {
        int e = real_socketlasterr();
        real_socketclose(listener);
        nssl_relay_stop_slot(slot);
        return -200 - e;
    }

    socklen_t local_len = sizeof(local);

    if (real_getsockname(
            listener,
            (struct nsn_sockaddr *)&local,
            &local_len) != 0) {
        int e = real_socketlasterr();
        real_socketclose(listener);
        nssl_relay_stop_slot(slot);
        return -300 - e;
    }

    /*
     * Connect the title's reserved native placeholder to localhost.
     * The original hostname is still passed untouched to NSSL later.
     */
    if (real_connect(
            sockfd,
            (const struct nsn_sockaddr *)&local,
            sizeof(local)) != 0) {
        int e = real_socketlasterr();
        real_socketclose(listener);
        nssl_relay_stop_slot(slot);
        return -400 - e;
    }

    /*
     * connect() has completed locally, so accept should be immediately
     * available. This exact path was validated by native_loopback_probe.
     */
    int accepted =
        real_accept(
            listener,
            NULL,
            NULL);

    real_socketclose(listener);

    if (accepted < 0) {
        int e = real_socketlasterr();
        nssl_relay_stop_slot(slot);
        return -500 - e;
    }

    struct nssl_relay *r =
        &nssl_relays[slot];

    r->native_fd = accepted;
    r->lwfd = lwfd;
    atomic_store(&r->public_fd, sockfd);

    /*
     * From this point the public descriptor is a genuine nsysnet socket
     * owned by Nintendo NSSL. The private lwIP descriptor belongs solely
     * to the relay.
     */
    untrack_fd(sockfd);
    atomic_store(&mapped_fd[sockfd], -1);

    atomic_store(&r->ready, 1);

    if (relay_slot_out)
        *relay_slot_out = slot;

    WHBLogPrintf(
        "AX: NSSL bridge ready fd=%d lwfd=%d loopback_port=%u",
        sockfd,
        lwfd,
        (unsigned)nsn_ntohs(local.sin_port));

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
    int relay_slot = -1;
    int bridge_result = 0;

    if (!is_foreign(sockfd)) {
        if (atomic_load(&nssl_bridge_enabled)) {
            bridge_result =
                bridge_ax_socket_to_nssl(
                    sockfd,
                    &relay_slot);

            WHBLogPrintf(
                "AX: NSSL bridge setup fd=%d result=%d host=%.*s",
                sockfd,
                bridge_result,
                host && hostLength > 0 ? hostLength : 0,
                host ? host : "");

            /*
             * Do not silently switch to Wi-Fi in bridge mode: that would
             * make a failed test look successful.
             */
            if (bridge_result <= 0)
                return NSSL_INVALID_FD;
        } else {
            (void)promote_ax_socket_to_native(sockfd);
        }
    }

    int32_t result =
        real_NSSLCreateConnection(
            context,
            host,
            hostLength,
            options,
            sockfd,
            block);

    WHBLogPrintf(
        "AX: NSSLCreateConnection fd=%d result=%d mode=%s",
        sockfd,
        result,
        atomic_load(&nssl_bridge_enabled)
            ? "bridge"
            : "native");

    if (relay_slot >= 0) {
        if (result < 0) {
            nssl_relay_stop_slot(
                relay_slot);
        } else {
            nssl_track_public_connection(
                sockfd,
                result);

            WHBLogPrintf(
                "AX: NSSL track conn=%d fd=%d",
                result,
                sockfd);
        }
    }

    return result;
}


/*
 * Wake the localhost transport BEFORE asking Nintendo NSSL to destroy the
 * connection. This prevents its teardown from waiting indefinitely for
 * more TLS/socket traffic from a relay which has already ended.
 */
DECL_FUNCTION(int32_t, NSSLDestroyConnection,
              int32_t connection)
{
    /*
     * Keep the localhost transport alive while Nintendo NSSL performs
     * its own TLS shutdown. Previously we forced SHUT_RDWR here, causing
     * NSSL_ERROR_SSL_SHUTDOWN_ERROR (-0x28001e).
     */
    WHBLogPrintf(
        "AX: NSSLDestroyConnection begin conn=%d mask=%08x",
        connection,
        (unsigned)atomic_load(&nssl_public_mask));

    int32_t result =
        real_NSSLDestroyConnection(
            connection);

    WHBLogPrintf(
        "AX: NSSLDestroyConnection end conn=%d result=%d",
        connection,
        result);

    uint32_t mask =
        atomic_load(&nssl_public_mask);

    for (int fd = 0; fd < 32; ++fd) {
        if (!(mask & (1u << fd)))
            continue;

        if (atomic_load(
                &nssl_connection_by_fd[fd]) !=
            connection)
            continue;

        nssl_relay_detach_public_fd(fd);
        nssl_untrack_public_fd(fd);
    }

    return result;
}


/*
 * NSSLFinish is another possible title-exit choke point.
 *
 * Ensure every remaining bridged connection observes a dead transport
 * before letting Nintendo's global NSSL cleanup proceed.
 */
DECL_FUNCTION(int32_t, NSSLFinish, void)
{
    WHBLogPrintf(
        "AX: NSSLFinish begin mask=%08x",
        (unsigned)atomic_load(&nssl_public_mask));

    int32_t result =
        real_NSSLFinish();

    WHBLogPrintf(
        "AX: NSSLFinish end result=%d",
        result);

    nssl_relay_stop_all();

    atomic_store(
        &nssl_public_mask,
        0);

    for (int fd = 0; fd < 32; ++fd)
        atomic_store(
            &nssl_connection_by_fd[fd],
            -1);

    return result;
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */

/*
 * Pretendo/Inkay rewrites these Nintendo NNCS hostnames before handing
 * them to nsysnet. When our DNS hook runs before Inkay in the Function
 * Patcher chain, that rewrite would otherwise be bypassed.
 *
 * Mirror Inkay's current DNS rewrite table here so dns=ax remains
 * compatible regardless of patch ordering.
 */
static const char *dns_rewrite_name(const char *name)
{
    if (!name)
        return NULL;

    if (strcmp(name, "nncs1.app.nintendowifi.net") == 0)
        return "nncs1.app.pretendo.cc";

    if (strcmp(name, "nncs2.app.nintendowifi.net") == 0)
        return "nncs2.app.pretendo.cc";

    return name;
}

DECL_FUNCTION(struct hostent *, gethostbyname, const char *name)
{
    if (!shim_accepts() || !ax_net_stack_ready()) return real_gethostbyname(name);

    const char *resolved_name = dns_rewrite_name(name);
    struct hostent *h = lwip_gethostbyname(resolved_name);
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


/* ------------------------------------------------------------------ */
/* Reverse IPv4 DNS / gethostbyaddr                                   */

/*
 * Native Wii U behaviour measured on hardware:
 *
 *   successful IPv4 PTR:
 *       h_name      = PTR hostname
 *       h_aliases   = { NULL }
 *       h_addr_list = { NULL }
 *       h_addrtype  = AF_INET
 *       h_length    = 4
 *
 *   no PTR / len != 4 / family != AF_INET:
 *       NULL
 *
 * gethostbyaddr() leaves h_errno completely unchanged on both success
 * and failure.
 *
 * lwIP's built-in resolver only understands A/AAAA, so PTR queries use
 * this small isolated DNS client. It deliberately does not modify
 * lwIP's dns_table or async resolver state.
 */

#define PTR_DNS_PORT        53
#define PTR_DNS_PACKET_MAX  512
#define PTR_DNS_TIMEOUT_US  750000

static char ghba_name[256];
static char *ghba_aliases[1] = { NULL };
static char *ghba_addr_list[1] = { NULL };
static struct hostent ghba_hostent;

static uint16_t ptr_dns_get16(const uint8_t *p)
{
    return (uint16_t)(
        ((uint16_t)p[0] << 8) |
        (uint16_t)p[1]);
}

static void ptr_dns_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int ptr_dns_encode_name(
    uint8_t *packet,
    size_t capacity,
    size_t *offset,
    const char *name)
{
    size_t pos = *offset;
    const char *cur = name;

    while (*cur) {
        const char *dot =
            strchr(cur, '.');

        size_t n =
            dot
                ? (size_t)(dot - cur)
                : strlen(cur);

        if (n == 0 ||
            n > 63 ||
            pos + 1 + n >= capacity)
            return -1;

        packet[pos++] =
            (uint8_t)n;

        memcpy(
            packet + pos,
            cur,
            n);

        pos += n;

        if (!dot)
            break;

        cur = dot + 1;
    }

    if (pos >= capacity)
        return -1;

    packet[pos++] = 0;
    *offset = pos;

    return 0;
}

/*
 * Skip one compressed DNS name while consuming only bytes belonging to
 * the current record.
 */
static int ptr_dns_skip_name(
    const uint8_t *packet,
    size_t packet_len,
    size_t *offset)
{
    size_t pos = *offset;

    for (unsigned labels = 0;
         labels < 128;
         labels++) {

        if (pos >= packet_len)
            return -1;

        uint8_t c =
            packet[pos];

        if (c == 0) {
            *offset = pos + 1;
            return 0;
        }

        if ((c & 0xc0) == 0xc0) {
            if (pos + 1 >= packet_len)
                return -1;

            *offset = pos + 2;
            return 0;
        }

        if ((c & 0xc0) != 0 ||
            c > 63 ||
            pos + 1u + c > packet_len)
            return -1;

        pos += 1u + c;
    }

    return -1;
}

/*
 * Decode a possibly-compressed DNS name. Used for PTR RDATA, where name
 * compression is legal.
 */
static int ptr_dns_decode_name(
    const uint8_t *packet,
    size_t packet_len,
    size_t offset,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0)
        return -1;

    size_t pos = offset;
    size_t used = 0;
    unsigned jumps = 0;
    unsigned labels = 0;

    while (labels++ < 128) {
        if (pos >= packet_len)
            return -1;

        uint8_t c =
            packet[pos];

        if (c == 0) {
            out[used] = 0;
            return 0;
        }

        if ((c & 0xc0) == 0xc0) {
            if (pos + 1 >= packet_len ||
                ++jumps > 32)
                return -1;

            size_t target =
                (size_t)(
                    ((uint16_t)(c & 0x3f) << 8) |
                    packet[pos + 1]);

            if (target >= packet_len)
                return -1;

            pos = target;
            continue;
        }

        if ((c & 0xc0) != 0 ||
            c > 63 ||
            pos + 1u + c > packet_len)
            return -1;

        if (used) {
            if (used + 1 >= out_len)
                return -1;

            out[used++] = '.';
        }

        if (used + c >= out_len)
            return -1;

        memcpy(
            out + used,
            packet + pos + 1,
            c);

        used += c;
        pos += 1u + c;
    }

    return -1;
}

static int ptr_dns_query_server(
    const ip_addr_t *server,
    const char *reverse_name,
    char *result,
    size_t result_len)
{
    if (!server ||
        ip_addr_isany(server))
        return -1;

    uint8_t query[PTR_DNS_PACKET_MAX];
    memset(query, 0, sizeof(query));

    uint16_t id =
        (uint16_t)(
            (uint64_t)OSGetTime() ^
            (uintptr_t)server ^
            (uintptr_t)reverse_name);

    ptr_dns_put16(
        query + 0,
        id);

    /* RD = recursion desired. */
    ptr_dns_put16(
        query + 2,
        0x0100);

    /* QDCOUNT = 1. */
    ptr_dns_put16(
        query + 4,
        1);

    size_t query_len = 12;

    if (ptr_dns_encode_name(
            query,
            sizeof(query),
            &query_len,
            reverse_name) < 0)
        return -1;

    if (query_len + 4 > sizeof(query))
        return -1;

    /* QTYPE PTR = 12, QCLASS IN = 1. */
    ptr_dns_put16(
        query + query_len,
        12);

    ptr_dns_put16(
        query + query_len + 2,
        1);

    query_len += 4;

    int fd =
        lwip_socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP);

    if (fd < 0)
        return -1;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));

    dst.sin_family =
        AF_INET;

    dst.sin_port =
        lwip_htons(PTR_DNS_PORT);

    dst.sin_addr.s_addr =
        ip_addr_get_ip4_u32(server);

    ssize_t sent =
        lwip_sendto(
            fd,
            query,
            query_len,
            0,
            (const struct sockaddr *)&dst,
            sizeof(dst));

    if (sent != (ssize_t)query_len) {
        lwip_close(fd);
        return -1;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = PTR_DNS_TIMEOUT_US;

    int ready =
        lwip_select(
            fd + 1,
            &readfds,
            NULL,
            NULL,
            &timeout);

    if (ready <= 0) {
        lwip_close(fd);
        return -1;
    }

    uint8_t response[PTR_DNS_PACKET_MAX];

    ssize_t received =
        lwip_recvfrom(
            fd,
            response,
            sizeof(response),
            0,
            NULL,
            NULL);

    lwip_close(fd);

    if (received < 12)
        return -1;

    size_t packet_len =
        (size_t)received;

    if (ptr_dns_get16(response + 0) != id)
        return -1;

    uint16_t flags =
        ptr_dns_get16(
            response + 2);

    /* Must be a response, not truncated, and RCODE must be zero. */
    if ((flags & 0x8000) == 0 ||
        (flags & 0x0200) != 0 ||
        (flags & 0x000f) != 0)
        return -1;

    uint16_t questions =
        ptr_dns_get16(
            response + 4);

    uint16_t answers =
        ptr_dns_get16(
            response + 6);

    size_t offset = 12;

    for (uint16_t i = 0;
         i < questions;
         i++) {

        if (ptr_dns_skip_name(
                response,
                packet_len,
                &offset) < 0)
            return -1;

        if (offset + 4 > packet_len)
            return -1;

        offset += 4;
    }

    for (uint16_t i = 0;
         i < answers;
         i++) {

        if (ptr_dns_skip_name(
                response,
                packet_len,
                &offset) < 0)
            return -1;

        if (offset + 10 > packet_len)
            return -1;

        uint16_t type =
            ptr_dns_get16(
                response + offset);

        uint16_t cls =
            ptr_dns_get16(
                response + offset + 2);

        uint16_t rdlen =
            ptr_dns_get16(
                response + offset + 8);

        offset += 10;

        if (offset + rdlen > packet_len)
            return -1;

        if (type == 12 &&
            cls == 1) {

            if (ptr_dns_decode_name(
                    response,
                    packet_len,
                    offset,
                    result,
                    result_len) == 0) {

                return 0;
            }
        }

        offset += rdlen;
    }

    return -1;
}

static int ptr_dns_lookup_ipv4(
    const void *addr,
    char *result,
    size_t result_len)
{
    const uint8_t *b =
        (const uint8_t *)addr;

    char reverse_name[64];

    int n =
        snprintf(
            reverse_name,
            sizeof(reverse_name),
            "%u.%u.%u.%u.in-addr.arpa",
            b[3],
            b[2],
            b[1],
            b[0]);

    if (n < 0 ||
        (size_t)n >= sizeof(reverse_name))
        return -1;

    for (u8_t i = 0;
         i < DNS_MAX_SERVERS;
         i++) {

        ip_addr_t server;
        ip_addr_set_zero(&server);

        LOCK_TCPIP_CORE();

        const ip_addr_t *configured =
            dns_getserver(i);

        if (configured)
            ip_addr_copy(
                server,
                *configured);

        UNLOCK_TCPIP_CORE();

        if (ip_addr_isany(&server))
            continue;

        if (ptr_dns_query_server(
                &server,
                reverse_name,
                result,
                result_len) == 0)
            return 0;
    }

    return -1;
}

DECL_FUNCTION(struct hostent *, gethostbyaddr,
              const void *addr,
              size_t len,
              int type)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready()) {

        return real_gethostbyaddr(
            addr,
            len,
            type);
    }

    /*
     * Native behaviour:
     * invalid address/size/family => NULL, without changing h_errno.
     */
    if (!addr ||
        len != 4 ||
        type != NSN_AF_INET)
        return NULL;

    /*
     * Our raw lwIP socket operations can alter errno and lwIP DNS APIs
     * share h_errno with the shim. Neither is part of the observable
     * native gethostbyaddr result, so preserve caller state.
     */
    int saved_errno =
        errno;

    int saved_h_errno =
        h_errno;

    struct hostent *result = NULL;

    if (ptr_dns_lookup_ipv4(
            addr,
            ghba_name,
            sizeof(ghba_name)) == 0) {

        ghba_aliases[0] = NULL;
        ghba_addr_list[0] = NULL;

        ghba_hostent.h_name =
            ghba_name;

        ghba_hostent.h_aliases =
            ghba_aliases;

        ghba_hostent.h_addrtype =
            NSN_AF_INET;

        ghba_hostent.h_length =
            4;

        ghba_hostent.h_addr_list =
            ghba_addr_list;

        result =
            &ghba_hostent;
    }

    h_errno =
        saved_h_errno;

    errno =
        saved_errno;

    return result;
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

static int shim_getaddrinfo_sync(
    const char *node,
    const char *service,
    const struct nsn_addrinfo *hints,
    struct nsn_addrinfo **res)
{
    if (!res)
        return NSN_EAI_FAIL;

    *res = NULL;

    struct addrinfo lh;
    struct addrinfo *lph = NULL;

    if (hints) {
        memset(&lh, 0, sizeof(lh));

        lh.ai_flags =
            ai_flags_to_lwip(hints->ai_flags);

        lh.ai_family =
            hints->ai_family;

        lh.ai_socktype =
            hints->ai_socktype;

        lh.ai_protocol =
            hints->ai_protocol;

        lph = &lh;
    }

    const char *resolved_node =
        dns_rewrite_name(node);

    struct addrinfo *lres = NULL;

    int err =
        lwip_getaddrinfo(
            resolved_node,
            service,
            lph,
            &lres);

    if (err != 0)
        return eai_to_nsn(err);

    /*
     * Convert lwIP addrinfo to the Wii U nsysnet ABI.
     */
    struct nsn_addrinfo *head = NULL;
    struct nsn_addrinfo **tail = &head;

    for (struct addrinfo *la = lres;
         la;
         la = la->ai_next) {

        size_t canonlen =
            la->ai_canonname
                ? strlen(la->ai_canonname) + 1
                : 0;

        struct nsn_addrinfo *na =
            malloc(
                sizeof(*na) +
                sizeof(struct nsn_sockaddr_in) +
                canonlen);

        if (!na) {
            err = NSN_EAI_MEMORY;
            goto fail;
        }

        na->ai_flags = la->ai_flags;
        na->ai_family = la->ai_family;
        na->ai_socktype = la->ai_socktype;
        na->ai_protocol = la->ai_protocol;
        na->ai_next = NULL;

        char *storage =
            (char *)(na + 1);

        na->ai_addr =
            (struct nsn_sockaddr *)storage;

        sockaddr_to_nsn(
            na->ai_addr,
            &na->ai_addrlen,
            la->ai_addr,
            16);

        storage +=
            sizeof(struct nsn_sockaddr_in);

        if (canonlen) {
            memcpy(
                storage,
                la->ai_canonname,
                canonlen);

            na->ai_canonname =
                storage;
        } else {
            na->ai_canonname =
                NULL;
        }

        *tail = na;
        tail = &na->ai_next;
    }

    if (!ai_remember(head)) {
        err = NSN_EAI_MEMORY;
        goto fail;
    }

    lwip_freeaddrinfo(lres);

    *res = head;
    return 0;

fail:
    lwip_freeaddrinfo(lres);

    while (head) {
        struct nsn_addrinfo *next =
            head->ai_next;

        free(head);
        head = next;
    }

    return err;
}

DECL_FUNCTION(int, getaddrinfo,
              const char *node,
              const char *service,
              const struct nsn_addrinfo *hints,
              struct nsn_addrinfo **res)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready()) {

        return real_getaddrinfo(
            node,
            service,
            hints,
            res);
    }

    return shim_getaddrinfo_sync(
        node,
        service,
        hints,
        res);
}


/* ------------------------------------------------------------------ */
/* Native-style asynchronous getaddrinfo                              */

/*
 * Native nsysnet behaviour measured on hardware:
 *
 *   uncached DNS:
 *       first call    -> EAI_INPROGRESS
 *       later polls   -> EAI_INPROGRESS
 *       completed     -> 0 + addrinfo
 *
 * Numeric/cached names may return 0 immediately.
 *
 * The async state is keyed by hostname. The result itself is not kept
 * here: lwIP puts the completed DNS answer in its DNS cache, so when a
 * poll observes READY, shim_getaddrinfo_sync() consumes that cached
 * answer and creates the normal nsysnet-compatible addrinfo list.
 */

#define ASYNC_DNS_MAX 8
#define ASYNC_DNS_TOKEN_INDEX_BITS 4
#define ASYNC_DNS_TOKEN_INDEX_MASK 0x0fu
#define ASYNC_DNS_TOKEN_TICKET_MASK 0x0fffffffu

enum async_dns_state {
    ASYNC_DNS_FREE = 0,
    ASYNC_DNS_PENDING,
    ASYNC_DNS_READY,
    ASYNC_DNS_FAILED,
    ASYNC_DNS_CLAIMED
};

struct async_dns_slot {
    atomic_int state;
    atomic_uint ticket;

    char name[DNS_MAX_NAME_LENGTH + 1];
};

static struct async_dns_slot async_dns_slots[ASYNC_DNS_MAX];

static atomic_uint async_dns_ticket_counter = 1;

static atomic_flag async_dns_table_lock =
    ATOMIC_FLAG_INIT;

static void async_dns_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
               &async_dns_table_lock,
               memory_order_acquire)) {
        OSYieldThread();
    }
}

static void async_dns_unlock(void)
{
    atomic_flag_clear_explicit(
        &async_dns_table_lock,
        memory_order_release);
}

static unsigned async_dns_new_ticket(void)
{
    unsigned ticket =
        atomic_fetch_add(
            &async_dns_ticket_counter,
            1);

    ticket &=
        ASYNC_DNS_TOKEN_TICKET_MASK;

    /*
     * Zero is reserved as "no live request".
     * Wrap would require hundreds of millions of queries.
     */
    if (!ticket)
        ticket = 1;

    return ticket;
}

static int async_dns_find_locked(
    const char *name)
{
    for (int i = 0;
         i < ASYNC_DNS_MAX;
         i++) {

        int state =
            atomic_load(
                &async_dns_slots[i].state);

        if (state == ASYNC_DNS_FREE ||
            state == ASYNC_DNS_CLAIMED)
            continue;

        if (strcmp(
                async_dns_slots[i].name,
                name) == 0)
            return i;
    }

    return -1;
}

static int async_dns_claim_locked(
    const char *name,
    uintptr_t *token_out)
{
    size_t len = strlen(name);

    if (len >= sizeof(async_dns_slots[0].name))
        return -1;

    for (int i = 0;
         i < ASYNC_DNS_MAX;
         i++) {

        int expected =
            ASYNC_DNS_FREE;

        if (!atomic_compare_exchange_strong(
                &async_dns_slots[i].state,
                &expected,
                ASYNC_DNS_CLAIMED))
            continue;

        unsigned ticket =
            async_dns_new_ticket();

        atomic_store(
            &async_dns_slots[i].ticket,
            ticket);

        memcpy(
            async_dns_slots[i].name,
            name,
            len + 1);

        /*
         * PENDING is published only after name/ticket are complete.
         */
        atomic_store(
            &async_dns_slots[i].state,
            ASYNC_DNS_PENDING);

        if (token_out) {
            *token_out =
                ((uintptr_t)ticket
                    << ASYNC_DNS_TOKEN_INDEX_BITS) |
                (uintptr_t)(i + 1);
        }

        return i;
    }

    return -1;
}

static void async_dns_release_if_ticket(
    int slot,
    unsigned ticket)
{
    if (slot < 0 ||
        slot >= ASYNC_DNS_MAX)
        return;

    if (atomic_load(
            &async_dns_slots[slot].ticket) !=
        ticket)
        return;

    atomic_store(
        &async_dns_slots[slot].state,
        ASYNC_DNS_FREE);

    atomic_store(
        &async_dns_slots[slot].ticket,
        0);
}

static void async_dns_found(
    const char *name,
    const ip_addr_t *ipaddr,
    void *callback_arg)
{
    (void)name;

    uintptr_t token =
        (uintptr_t)callback_arg;

    unsigned raw_index =
        (unsigned)(
            token &
            ASYNC_DNS_TOKEN_INDEX_MASK);

    if (raw_index == 0 ||
        raw_index > ASYNC_DNS_MAX)
        return;

    int slot =
        (int)raw_index - 1;

    unsigned ticket =
        (unsigned)(
            token >>
            ASYNC_DNS_TOKEN_INDEX_BITS);

    if (atomic_load(
            &async_dns_slots[slot].ticket) !=
        ticket)
        return;

    int expected =
        ASYNC_DNS_PENDING;

    atomic_compare_exchange_strong(
        &async_dns_slots[slot].state,
        &expected,
        ipaddr
            ? ASYNC_DNS_READY
            : ASYNC_DNS_FAILED);
}

static int async_dns_raw_error_to_nsn(
    err_t err)
{
    if (err == ERR_MEM)
        return NSN_EAI_MEMORY;

    return NSN_EAI_FAIL;
}

static int shim_getaddrinfo_async(
    const char *node,
    const char *service,
    const struct nsn_addrinfo *hints,
    struct nsn_addrinfo **res)
{
    if (!res)
        return NSN_EAI_FAIL;

    *res = NULL;

    /*
     * No hostname means no asynchronous DNS work is required.
     * Numeric/local/cache hits are also handled synchronously below.
     */
    if (!node) {
        return shim_getaddrinfo_sync(
            node,
            service,
            hints,
            res);
    }

    const char *resolved_node =
        dns_rewrite_name(node);

    if (!resolved_node ||
        !resolved_node[0])
        return NSN_EAI_NONAME;

    /*
     * First check whether this hostname already has an async request.
     */
    async_dns_lock();

    int existing =
        async_dns_find_locked(
            resolved_node);

    if (existing >= 0) {
        int state =
            atomic_load(
                &async_dns_slots[existing].state);

        if (state == ASYNC_DNS_PENDING) {
            async_dns_unlock();
            return NSN_EAI_INPROGRESS;
        }

        /*
         * The DNS callback has already run. Release our bookkeeping
         * before creating the addrinfo result.
         *
         * The successful answer is now in lwIP's DNS cache.
         */
        atomic_store(
            &async_dns_slots[existing].state,
            ASYNC_DNS_FREE);

        atomic_store(
            &async_dns_slots[existing].ticket,
            0);

        async_dns_unlock();

        if (state == ASYNC_DNS_FAILED)
            return NSN_EAI_NONAME;

        return shim_getaddrinfo_sync(
            node,
            service,
            hints,
            res);
    }

    uintptr_t token = 0;

    int slot =
        async_dns_claim_locked(
            resolved_node,
            &token);

    async_dns_unlock();

    if (slot < 0)
        return NSN_EAI_MEMORY;

    unsigned ticket =
        (unsigned)(
            token >>
            ASYNC_DNS_TOKEN_INDEX_BITS);

    ip_addr_t addr;

    /*
     * dns_gethostbyname() is lwIP's raw non-blocking DNS API.
     * With core locking enabled it can safely be entered from the
     * title's thread while the actual DNS transaction remains async.
     */
    LOCK_TCPIP_CORE();

    err_t err =
        dns_gethostbyname(
            resolved_node,
            &addr,
            async_dns_found,
            (void *)token);

    UNLOCK_TCPIP_CORE();

    if (err == ERR_INPROGRESS)
        return NSN_EAI_INPROGRESS;

    /*
     * Numeric address, localhost or DNS-cache hit: native nsysnet also
     * returns success immediately rather than EAI_INPROGRESS.
     */
    if (err == ERR_OK) {
        async_dns_release_if_ticket(
            slot,
            ticket);

        return shim_getaddrinfo_sync(
            node,
            service,
            hints,
            res);
    }

    async_dns_release_if_ticket(
        slot,
        ticket);

    return async_dns_raw_error_to_nsn(
        err);
}

DECL_FUNCTION(int, getaddrinfo_rs,
              const char *node,
              const char *service,
              const struct nsn_addrinfo *hints,
              struct nsn_addrinfo **res)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready()) {

        return real_getaddrinfo_rs(
            node,
            service,
            hints,
            res);
    }

    return shim_getaddrinfo_sync(
        node,
        service,
        hints,
        res);
}

DECL_FUNCTION(int, getaddrinfo_async,
              const char *node,
              const char *service,
              const struct nsn_addrinfo *hints,
              struct nsn_addrinfo **res)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready()) {

        return real_getaddrinfo_async(
            node,
            service,
            hints,
            res);
    }

    return shim_getaddrinfo_async(
        node,
        service,
        hints,
        res);
}

DECL_FUNCTION(int, getaddrinfo_async_rs,
              const char *node,
              const char *service,
              const struct nsn_addrinfo *hints,
              struct nsn_addrinfo **res)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready()) {

        return real_getaddrinfo_async_rs(
            node,
            service,
            hints,
            res);
    }

    return shim_getaddrinfo_async(
        node,
        service,
        hints,
        res);
}

static void async_dns_reset_all(void)
{
    /*
     * ticket=0 invalidates callbacks from a previous title generation.
     * A late callback therefore cannot complete a newly reused slot.
     */
    async_dns_lock();

    for (int i = 0;
         i < ASYNC_DNS_MAX;
         i++) {

        atomic_store(
            &async_dns_slots[i].ticket,
            0);

        atomic_store(
            &async_dns_slots[i].state,
            ASYNC_DNS_FREE);

        async_dns_slots[i].name[0] = 0;
    }

    async_dns_unlock();
}

/*
 * Native Wii U behaviour measured on hardware:
 *
 * dns_abort_by_hname() returns 0 whether the hostname has no request,
 * is already cached, matches a pending query, or differs from a pending
 * query. A pending getaddrinfo_async() also remains able to complete
 * normally after the call.
 *
 * Do not perform a stronger lwIP cancellation than the native observable
 * behaviour. When AX DNS is active this is intentionally a successful
 * no-op.
 */
DECL_FUNCTION(int, dns_abort_by_hname,
              const char *hostname)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready()) {

        return real_dns_abort_by_hname(
            hostname);
    }

    (void)hostname;
    return 0;
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

/*
 * Mark FTPiiU's pre-AX native port-21 listener for a one-shot poll failure.
 *
 * FTPiiU itself owns and closes the old fd. Its next loop iteration creates
 * a new socket after AX/shim is ready, so bind(:21) goes through lwIP and
 * ftp_rewrite_bind_to_ax().
 */
int nsysnet_shim_request_ftp_handoff(void)
{
    if (!shim_accepts() ||
        !ax_net_stack_ready() ||
        ax_net_ip4() == 0)
        return 0;

    int marked = 0;

    for (int fd = 0; fd < 32; ++fd) {
        uint32_t bit = 1u << fd;

        if (!is_foreign(fd) ||
            (atomic_load(&ftp_restart_mask) & bit))
            continue;

        struct nsn_sockaddr_in local;
        socklen_t local_len = sizeof(local);

        memset(&local, 0, sizeof(local));

        errno = -1;

        if (real_getsockname(
                fd,
                (struct nsn_sockaddr *)&local,
                &local_len) != 0)
            continue;

        if (local.sin_family != NSN_AF_INET ||
            nsn_ntohs(local.sin_port) != AX_FTP_PORT)
            continue;

        int type = 0;
        socklen_t type_len = sizeof(type);

        errno = -1;

        if (real_getsockopt(
                fd,
                NSN_SOL_SOCKET,
                NSN_SO_TYPE,
                &type,
                &type_len) != 0 ||
            type != SOCK_STREAM)
            continue;

        /*
         * Accepted FTP control connections also use local port 21.
         * The listening socket has no peer; accepted control sockets do.
         */
        struct nsn_sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        memset(&peer, 0, sizeof(peer));

        errno = -1;

        if (real_getpeername(
                fd,
                (struct nsn_sockaddr *)&peer,
                &peer_len) == 0)
            continue;

        atomic_fetch_or(
            &ftp_restart_mask,
            bit);

        WHBLogPrintf(
            "AX: FTP handoff marked native listener fd=%d",
            fd);

        marked++;
    }

    return marked;
}

/*
 * Called from WUMS_APPLICATION_REQUESTS_EXIT.
 *
 * That callback is synchronous inside Aroma's system-message handling.
 * It therefore MUST remain essentially instantaneous.
 */
void nsysnet_shim_quiesce(void)
{
    /*
     * Called synchronously from Aroma's OSReceiveMessage hook.
     *
     * Absolutely no socket I/O, lwIP calls, waits or core locks here.
     * is_foreign() observes accepting_sockets immediately, so all later
     * title socket calls go to the native placeholder descriptors.
     */
    atomic_store(&accepting_sockets, 0);
    atomic_store(&probe_owns_accepting, 0);
}

void nsysnet_shim_drain_owned_sockets(void)
{
    /*
     * CPU2 worker side of title teardown.
     *
     * Public nsysnet descriptors are NOT closed here: the title owns them
     * and will close them normally. We only retire the private lwIP backing
     * descriptors before ax_net_stop() removes the netif.
     */
    uint32_t mask =
        atomic_exchange(&open_mask, 0);

    for (int fd = 0; fd < 32; ++fd) {
        if (!(mask & (1u << fd)))
            continue;

        int lwfd =
            atomic_exchange(&mapped_fd[fd], -1);

        if (lwfd >= 0) {
            lwip_shutdown(lwfd, SHUT_RDWR);
            lwip_close(lwfd);
        }

        compat_state_reset(fd);
    }
}

/* ------------------------------------------------------------------ */
/* registration                                                        */

/*
 * The same nsysnet compatibility layer is now registered for multiple
 * Wii U processes. Keep plenty of room for every per-process patch.
 */
static PatchedFunctionHandle handles[128];
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
 * Process coverage.
 *
 * GAME is the extensively validated path.
 *
 * Wii U Menu is the first system process being enabled. Ownership is
 * deliberately conservative: descriptors that were not created by this
 * shim remain native and are passed directly to nsysnet.
 *
 * ROOT_RPX remains excluded. Aroma and other resident modules may already
 * own sockets there before this shim becomes active.
 */
#define SHIM_PATCH_PROCESS(name, process)                                         \
    do {                                                                          \
        function_replacement_data_t d = REPLACE_FUNCTION_FOR_PROCESS(             \
            name, LIBRARY_NSYSNET, name, process);                                \
        if (add_patch(&d, #name, process) < 0) goto fail;                         \
    } while (0)

#define SHIM_PATCH(name)                                                          \
    do {                                                                          \
        SHIM_PATCH_PROCESS(name, FP_TARGET_PROCESS_GAME);                         \
        SHIM_PATCH_PROCESS(name, FP_TARGET_PROCESS_WII_U_MENU);                   \
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

    /*
     * 0.2.46 diagnostic:
     * lifecycle tracing FunctionPatcher hooks intentionally disabled.
     * Keep only the normal nsysnet socket hook group for this test.
     */

    /*
     * 0.2.48 socket hook bisection - group A1.
     *
     * 5 functions x GAME + Wii U Menu = 10 handles.
     */
    SHIM_PATCH(socket);
    SHIM_PATCH(socketclose);
    SHIM_PATCH(socketclose_all);
    SHIM_PATCH(bind);
    SHIM_PATCH(connect);

    /*
     * 0.2.46 diagnostic:
     * NSSL hooks disabled to isolate the ordinary nsysnet socket hooks.
     */

    if (!atomic_load(&system_dns)) {
        SHIM_PATCH(gethostbyname);
        SHIM_PATCH(gethostbyaddr);
        SHIM_PATCH(getaddrinfo);
        SHIM_PATCH(getaddrinfo_rs);
        SHIM_PATCH(getaddrinfo_async);
        SHIM_PATCH(getaddrinfo_async_rs);
        SHIM_PATCH(dns_abort_by_hname);
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
    nsysnet_shim_quiesce();

    /*
     * The relay of an HTTP request may already have ended while Nintendo
     * NSSL still owns its public localhost socket.
     *
     * Wake every tracked NSSL transport, not merely the public fd stored in
     * the last surviving relay slot.
     */
    nssl_shutdown_all_tracked(
        "title-exit");

    nssl_relay_stop_all();
}
void nsysnet_shim_begin_title(void) {
    nsysnet_shim_stop_accepting();

    /*
     * A previous process' lwIP descriptor numbers are meaningless here.
     * The old process should have drained them on its worker; if it did not,
     * forget the stale mappings rather than touching dead lwIP state.
     */
    atomic_store(&open_mask, 0);

    for (int fd = 0; fd < 32; ++fd)
        atomic_store(&mapped_fd[fd], -1);

    atomic_store(&probe_thread, 0);
    atomic_store(&probe_baseline_open_mask, 0);
    atomic_store(&probe_baseline_ai_mask, 0);
    atomic_store(&open_mask, 0);
    atomic_store(&ax_activity_pending, 0);

    atomic_store(&ftp_native_bind_ip, 0);
    atomic_store(&ftp_ax_redirect_active, 0);
    atomic_store(&ftp_restart_mask, 0);

    atomic_store(&nssl_public_mask, 0);

    for (int fd = 0; fd < 32; ++fd)
        atomic_store(
            &nssl_connection_by_fd[fd],
            -1);

    compat_state_reset_all();
    async_dns_reset_all();

    for (int i=0; i<AI_OWNED_MAX; ++i) atomic_store(&ai_owned[i], NULL);
}

static uint32_t probe_ai_mask(void)
{
    uint32_t mask = 0;

    for (int i = 0; i < AI_OWNED_MAX; ++i) {
        if (atomic_load(&ai_owned[i]))
            mask |= 1u << i;
    }

    return mask;
}

int nsysnet_shim_begin_probe(void) {
    if (atomic_load(&probe_thread)) return -3;
    if (!ax_net_stack_ready() || !ax_net_address()) return -4;

    int was_accepting = atomic_load(&accepting_sockets);

    if (nsysnet_shim_install() < 0) return -1;

    /*
     * Automatic production routing may already own sockets before this
     * diagnostic probe begins. They are not probe leaks.
     */
    atomic_store(&probe_baseline_open_mask,
                 atomic_load(&open_mask));

    atomic_store(&probe_baseline_ai_mask,
                 probe_ai_mask());

    atomic_store(&probe_thread, (uintptr_t)OSGetCurrentThread());
    atomic_store(&accepting_sockets, 1);
    atomic_store(&probe_owns_accepting, was_accepting ? 0 : 1);

    return 0;
}

int nsysnet_shim_end_probe(void) {
    if (atomic_load(&probe_thread) !=
        (uintptr_t)OSGetCurrentThread())
        return -3;

    /*
     * Only resources added after BeginProbe count as probe leaks.
     * Production sockets which existed before the diagnostic started
     * are deliberately ignored.
     */
    uint32_t baseline_open =
        atomic_load(&probe_baseline_open_mask);

    uint32_t current_open =
        atomic_load(&open_mask);

    if (current_open & ~baseline_open)
        return -5;

    uint32_t baseline_ai =
        atomic_load(&probe_baseline_ai_mask);

    uint32_t current_ai =
        probe_ai_mask();

    if (current_ai & ~baseline_ai)
        return -5;

    if (atomic_load(&probe_owns_accepting))
        nsysnet_shim_stop_accepting();

    atomic_store(&probe_owns_accepting, 0);
    atomic_store(&probe_thread, 0);
    atomic_store(&probe_baseline_open_mask, 0);
    atomic_store(&probe_baseline_ai_mask, 0);

    return 0;
}
