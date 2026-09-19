#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wut_rplwrap.h>

#include "probe.h"

#define PC_IP       "192.168.2.100"
#define RX_PORT     19030
#define PEER_PORT   19031

#ifndef MSG_IP_RECVTTL
#define MSG_IP_RECVTTL 0x40
#endif

extern int RPLWRAP(recvfrom_ex)(
    int socket,
    void *buffer,
    int len,
    int flags,
    struct sockaddr *from,
    int *fromlen,
    uint8_t *msg,
    int msglen);

extern int RPLWRAP(socketlasterr)(void);

extern int __wut_get_nsysnet_fd(int fd);

static int running = 1;

static int pump(void)
{
    if (running)
        running = probe_poll();

    return running;
}

static void wait_for_module(void)
{
    probe_say("Waiting 8s for module startup...");

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(8000);

    while (pump() && OSGetTime() < deadline)
        OSSleepTicks(OSMillisecondsToTicks(20));
}

static void extra_hex(
    const uint8_t *extra,
    char *out,
    size_t out_len)
{
    size_t pos = 0;

    for (unsigned i = 0; i < 16; i++) {
        int n = snprintf(
            out + pos,
            out_len - pos,
            "%s%02x",
            i ? " " : "",
            extra[i]);

        if (n < 0)
            break;

        pos += (size_t)n;

        if (pos >= out_len)
            break;
    }
}

static void show_path(int fd)
{
    struct sockaddr_in local;
    socklen_t len = sizeof(local);

    memset(&local, 0, sizeof(local));

    if (getsockname(
            fd,
            (struct sockaddr *)&local,
            &len) != 0) {
        probe_say("PATH getsockname FAIL errno=%d", errno);
        return;
    }

    char ip[32] = "?";

    inet_ntop(
        AF_INET,
        &local.sin_addr,
        ip,
        sizeof(ip));

    probe_say(
        "PATH local=%s:%u",
        ip,
        ntohs(local.sin_port));
}

/*
 * Network probes must never block the ProcUI thread.
 *
 * Wait for a datagram using zero-timeout poll() while continuing to pump
 * ProcUI. The socket itself is also O_NONBLOCK, so recvfrom_ex cannot
 * strand the application even if readiness changes unexpectedly.
 */
static int wait_for_packet(int fd, const char *name)
{
    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(5000);

    while (pump() && OSGetTime() < deadline) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0
        };

        int rc = poll(&pfd, 1, 0);

        if (rc < 0) {
            probe_say(
                "%s poll FAIL errno=%d",
                name,
                errno);
            return -1;
        }

        if (rc > 0) {
            if (pfd.revents & POLLIN)
                return 1;

            if (pfd.revents &
                (POLLERR | POLLHUP | POLLNVAL)) {
                probe_say(
                    "%s poll revents=%04x",
                    name,
                    pfd.revents);
                return -1;
            }
        }

        OSSleepTicks(OSMillisecondsToTicks(10));
    }

    if (!running)
        return 0;

    probe_say(
        "%s WAIT TIMEOUT - no datagram available",
        name);

    return -1;
}

static void run_case(
    int fd,
    int nsfd,
    const char *name,
    int flags,
    int msglen)
{
    if (wait_for_packet(fd, name) <= 0)
        return;


    static uint8_t data[128]
        __attribute__((aligned(0x40)));

    static struct sockaddr_in from
        __attribute__((aligned(0x40)));

    static uint8_t extra[0x40]
        __attribute__((aligned(0x40)));

    memset(data, 0, sizeof(data));
    memset(&from, 0, sizeof(from));
    memset(extra, 0xa5, sizeof(extra));

    int fromlen = sizeof(from);

    errno = 0;

    int rc = RPLWRAP(recvfrom_ex)(
        nsfd,
        data,
        sizeof(data) - 1,
        flags,
        (struct sockaddr *)&from,
        &fromlen,
        extra,
        msglen);

    int libc_errno = errno;

    int nerr =
        rc < 0
            ? RPLWRAP(socketlasterr)()
            : 0;

    if (rc > 0 && rc < (int)sizeof(data))
        data[rc] = 0;

    int expected_ttl = -1;

    if (rc > 0)
        sscanf((char *)data, "TTL=%d", &expected_ttl);

    char source_ip[32] = "?";

    if (rc >= 0) {
        inet_ntop(
            AF_INET,
            &from.sin_addr,
            source_ip,
            sizeof(source_ip));
    }

    char dump[80];
    memset(dump, 0, sizeof(dump));
    extra_hex(extra, dump, sizeof(dump));

    probe_say(
        "%s flags=%02x msglen=%d rc=%d errno=%d nerr=%d",
        name,
        flags,
        msglen,
        rc,
        libc_errno,
        nerr);

    probe_say(
        "  payload='%s' expected_ttl=%d from=%s:%u fromlen=%d",
        rc > 0 ? (char *)data : "",
        expected_ttl,
        source_ip,
        rc >= 0 ? ntohs(from.sin_port) : 0,
        fromlen);

    probe_say(
        "  extra[0..15]=%s",
        dump);
}

int main(void)
{
    if (probe_init("AX recvfrom_ex Probe") != 0)
        return 1;

    probe_say("recvfrom_ex native ABI / TTL audit");
    probe_say("Start the PC helper BEFORE this probe.");

    wait_for_module();

    if (!running)
        goto done;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("socket FAIL errno=%d", errno);
        goto wait;
    }

    int one = 1;

    setsockopt(
        fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &one,
        sizeof(one));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));

    local.sin_family = AF_INET;
    local.sin_port = htons(RX_PORT);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind(
            fd,
            (struct sockaddr *)&local,
            sizeof(local)) != 0) {
        probe_say("bind FAIL errno=%d", errno);
        close(fd);
        goto wait;
    }

    /*
     * Connect UDP to the helper's fixed source port. Besides filtering
     * the sender, this gives getsockname() a concrete local route, so
     * PATH tells us native Wi-Fi vs AX.
     */
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));

    peer.sin_family = AF_INET;
    peer.sin_port = htons(PEER_PORT);
    peer.sin_addr.s_addr = inet_addr(PC_IP);

    if (connect(
            fd,
            (struct sockaddr *)&peer,
            sizeof(peer)) != 0) {
        probe_say("connect FAIL errno=%d", errno);
        close(fd);
        goto wait;
    }

    /*
     * Absolute safety net: even after poll() says readable, the raw
     * recvfrom_ex call must never be able to block the ProcUI thread.
     */
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0 ||
        fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        probe_say("fcntl O_NONBLOCK FAIL errno=%d", errno);
        close(fd);
        goto wait;
    }

    show_path(fd);

    int nsfd = __wut_get_nsysnet_fd(fd);

    probe_say(
        "fd=%d raw_nsysnet_fd=%d",
        fd,
        nsfd);

    if (nsfd < 0) {
        probe_say("fd conversion FAIL errno=%d", errno);
        close(fd);
        goto wait;
    }

    probe_say("--- baseline: no MSG_IP_RECVTTL ---");

    run_case(
        fd,
        nsfd,
        "BASE",
        0,
        64);

    probe_say("--- msglen=0 edge characterization ---");

    /*
     * First determine whether a zero metadata length is itself legal when
     * no extended information is requested.
     */
    run_case(
        fd,
        nsfd,
        "ZERO-BASE",
        0,
        0);

    /*
     * Repeat the exact native discrepancy several times.
     *
     * If MSG_IP_RECVTTL + msglen=0 rejects the call without consuming
     * the queued UDP datagram, all three calls should fail while the
     * following msglen=1 control receives that same head datagram.
     */
    run_case(
        fd,
        nsfd,
        "ZERO-TTL-A",
        MSG_IP_RECVTTL,
        0);

    run_case(
        fd,
        nsfd,
        "ZERO-TTL-B",
        MSG_IP_RECVTTL,
        0);

    run_case(
        fd,
        nsfd,
        "ZERO-TTL-C",
        MSG_IP_RECVTTL,
        0);

    probe_say("--- MSG_IP_RECVTTL msglen >= 1 control matrix ---");

    static const int lens[] = {
        1, 2, 4, 8, 16, 64
    };

    for (unsigned i = 0;
         i < sizeof(lens) / sizeof(lens[0]);
         i++) {
        char name[32];

        snprintf(
            name,
            sizeof(name),
            "TTL-LEN-%d",
            lens[i]);

        run_case(
            fd,
            nsfd,
            name,
            MSG_IP_RECVTTL,
            lens[i]);
    }

    probe_say("--- END recvfrom_ex PROBE ---");

    close(fd);

wait:
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    probe_shutdown();
    return 0;
}
