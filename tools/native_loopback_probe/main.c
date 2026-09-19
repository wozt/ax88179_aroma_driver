
#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "probe.h"

#define TEST_BYTES (64 * 1024)

static int running = 1;

static uint8_t txbuf[TEST_BYTES];
static uint8_t rxbuf[TEST_BYTES];

static int pump(void)
{
    if (running)
        running = probe_poll();
    return running;
}

static int would_block(void)
{
    return errno == EAGAIN ||
           errno == EWOULDBLOCK ||
           errno == EINPROGRESS ||
           errno == EALREADY;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;

    return fcntl(fd,
                 F_SETFL,
                 flags | O_NONBLOCK);
}

static void show_endpoint(int fd, const char *name)
{
    struct sockaddr_in local;
    struct sockaddr_in peer;

    socklen_t llen = sizeof(local);
    socklen_t plen = sizeof(peer);

    memset(&local, 0, sizeof(local));
    memset(&peer, 0, sizeof(peer));

    char lip[32] = "?";
    char pip[32] = "?";

    int lrc =
        getsockname(fd,
                    (struct sockaddr *)&local,
                    &llen);

    int prc =
        getpeername(fd,
                    (struct sockaddr *)&peer,
                    &plen);

    if (lrc == 0)
        inet_ntop(AF_INET,
                  &local.sin_addr,
                  lip,
                  sizeof(lip));

    if (prc == 0)
        inet_ntop(AF_INET,
                  &peer.sin_addr,
                  pip,
                  sizeof(pip));

    probe_say(
        "LOOPBACK %s fd=%d local=%s:%u peer=%s:%u",
        name,
        fd,
        lip,
        lrc == 0 ? ntohs(local.sin_port) : 0,
        pip,
        prc == 0 ? ntohs(peer.sin_port) : 0);
}

static int transfer(int src,
                    int dst,
                    const char *name,
                    uint8_t seed)
{
    for (unsigned i = 0; i < TEST_BYTES; i++)
        txbuf[i] =
            (uint8_t)(
                seed +
                i * 37u +
                (i >> 8));

    memset(rxbuf, 0, sizeof(rxbuf));

    size_t sent = 0;
    size_t received = 0;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(10000);

    while (pump() &&
           OSGetTime() < deadline &&
           received < TEST_BYTES) {

        struct pollfd p[2] = {
            {
                .fd = src,
                .events = POLLOUT,
                .revents = 0
            },
            {
                .fd = dst,
                .events = POLLIN,
                .revents = 0
            }
        };

        int prc = poll(p, 2, 0);

        if (prc < 0) {
            probe_say(
                "LOOPBACK %s poll FAIL errno=%d",
                name,
                errno);
            return -1;
        }

        if (sent < TEST_BYTES &&
            (p[0].revents & POLLOUT)) {

            size_t left =
                TEST_BYTES - sent;

            if (left > 4096)
                left = 4096;

            errno = 0;

            int n =
                send(src,
                     txbuf + sent,
                     left,
                     0);

            if (n > 0) {
                sent += (size_t)n;
            } else if (n < 0 &&
                       !would_block()) {
                probe_say(
                    "LOOPBACK %s send FAIL errno=%d sent=%u",
                    name,
                    errno,
                    (unsigned)sent);
                return -1;
            }
        }

        if (p[1].revents & POLLIN) {
            size_t left =
                TEST_BYTES - received;

            if (left > 4096)
                left = 4096;

            errno = 0;

            int n =
                recv(dst,
                     rxbuf + received,
                     left,
                     0);

            if (n > 0) {
                received += (size_t)n;
            } else if (n == 0) {
                probe_say(
                    "LOOPBACK %s unexpected EOF",
                    name);
                return -1;
            } else if (!would_block()) {
                probe_say(
                    "LOOPBACK %s recv FAIL errno=%d received=%u",
                    name,
                    errno,
                    (unsigned)received);
                return -1;
            }
        }

        if (p[0].revents &
            (POLLERR | POLLHUP | POLLNVAL)) {
            probe_say(
                "LOOPBACK %s source revents=%04x",
                name,
                p[0].revents);
            return -1;
        }

        if (p[1].revents &
            (POLLERR | POLLHUP | POLLNVAL)) {
            probe_say(
                "LOOPBACK %s destination revents=%04x",
                name,
                p[1].revents);
            return -1;
        }

        OSSleepTicks(
            OSMillisecondsToTicks(1));
    }

    probe_say(
        "LOOPBACK %s sent=%u recv=%u",
        name,
        (unsigned)sent,
        (unsigned)received);

    if (sent != TEST_BYTES ||
        received != TEST_BYTES) {
        probe_say(
            "LOOPBACK %s TIMEOUT/SHORT",
            name);
        return -1;
    }

    if (memcmp(txbuf,
               rxbuf,
               TEST_BYTES) != 0) {

        unsigned bad = 0;

        while (bad < TEST_BYTES &&
               txbuf[bad] == rxbuf[bad])
            bad++;

        probe_say(
            "LOOPBACK %s DATA MISMATCH offset=%u tx=%02x rx=%02x",
            name,
            bad,
            bad < TEST_BYTES ? txbuf[bad] : 0,
            bad < TEST_BYTES ? rxbuf[bad] : 0);

        return -1;
    }

    probe_say(
        "LOOPBACK %s 65536 bytes PASS",
        name);

    return 0;
}

int main(void)
{
    /*
     * Important:
     *
     * Create these descriptors before probe_init() and before the AX
     * module's startup guard can install/enrol the shim.
     *
     * They must remain genuine Nintendo nsysnet descriptors. This is
     * the same technique already validated by shim_probe's native
     * coexistence socket.
     */
    int listener =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    int client =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (probe_init("Native nsysnet loopback probe") != 0) {
        if (listener >= 0)
            close(listener);
        if (client >= 0)
            close(client);
        return 1;
    }

    probe_say(
        "LOOPBACK early native fds listener=%d client=%d",
        listener,
        client);

    if (listener < 0 ||
        client < 0) {
        probe_say(
            "LOOPBACK early socket creation FAIL errno=%d",
            errno);
        goto wait;
    }

    if (set_nonblocking(listener) != 0 ||
        set_nonblocking(client) != 0) {
        probe_say(
            "LOOPBACK O_NONBLOCK FAIL errno=%d",
            errno);
        goto wait;
    }

    int one = 1;

    setsockopt(listener,
               SOL_SOCKET,
               SO_REUSEADDR,
               &one,
               sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);

    if (inet_pton(AF_INET,
                  "127.0.0.1",
                  &addr.sin_addr) != 1) {
        probe_say(
            "LOOPBACK inet_pton FAIL");
        goto wait;
    }

    errno = 0;

    int rc =
        bind(listener,
             (struct sockaddr *)&addr,
             sizeof(addr));

    probe_say(
        "LOOPBACK bind(127.0.0.1:0) rc=%d errno=%d",
        rc,
        errno);

    if (rc != 0)
        goto wait;

    errno = 0;

    rc = listen(listener, 4);

    probe_say(
        "LOOPBACK listen rc=%d errno=%d",
        rc,
        errno);

    if (rc != 0)
        goto wait;

    socklen_t alen = sizeof(addr);

    memset(&addr, 0, sizeof(addr));

    if (getsockname(
            listener,
            (struct sockaddr *)&addr,
            &alen) != 0) {
        probe_say(
            "LOOPBACK listener getsockname FAIL errno=%d",
            errno);
        goto wait;
    }

    char listen_ip[32];

    inet_ntop(AF_INET,
              &addr.sin_addr,
              listen_ip,
              sizeof(listen_ip));

    probe_say(
        "LOOPBACK listener=%s:%u",
        listen_ip,
        ntohs(addr.sin_port));

    errno = 0;

    rc =
        connect(client,
                (struct sockaddr *)&addr,
                sizeof(addr));

    int connect_errno = errno;

    probe_say(
        "LOOPBACK connect rc=%d errno=%d",
        rc,
        connect_errno);

    if (rc < 0 &&
        connect_errno != EINPROGRESS &&
        connect_errno != EWOULDBLOCK &&
        connect_errno != EALREADY)
        goto wait;

    int accepted = -1;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(5000);

    while (pump() &&
           OSGetTime() < deadline &&
           accepted < 0) {

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);

        memset(&peer, 0, sizeof(peer));

        errno = 0;

        accepted =
            accept(listener,
                   (struct sockaddr *)&peer,
                   &plen);

        if (accepted >= 0)
            break;

        if (!would_block()) {
            probe_say(
                "LOOPBACK accept FAIL errno=%d",
                errno);
            goto wait;
        }

        OSSleepTicks(
            OSMillisecondsToTicks(2));
    }

    if (accepted < 0) {
        probe_say(
            "LOOPBACK accept TIMEOUT");
        goto wait;
    }

    if (set_nonblocking(accepted) != 0) {
        probe_say(
            "LOOPBACK accepted O_NONBLOCK FAIL errno=%d",
            errno);
        close(accepted);
        goto wait;
    }

    int so_error = -1;
    socklen_t so_len = sizeof(so_error);

    errno = 0;

    rc =
        getsockopt(client,
                   SOL_SOCKET,
                   SO_ERROR,
                   &so_error,
                   &so_len);

    probe_say(
        "LOOPBACK client SO_ERROR rc=%d errno=%d value=%d",
        rc,
        errno,
        so_error);

    if (rc != 0 ||
        so_error != 0) {
        close(accepted);
        goto wait;
    }

    show_endpoint(listener, "listener");
    show_endpoint(client, "client");
    show_endpoint(accepted, "accepted");

    int c2s =
        transfer(client,
                 accepted,
                 "CLIENT->SERVER",
                 0x23);

    int s2c =
        c2s == 0
            ? transfer(accepted,
                       client,
                       "SERVER->CLIENT",
                       0x91)
            : -1;

    if (c2s == 0 &&
        s2c == 0) {

        probe_say("%s", "");
        probe_say(
            "LOOPBACK RESULT: PASS");

        probe_say(
            "native nsysnet 127.0.0.1 TCP tunnel is viable");
    } else {
        probe_say("%s", "");
        probe_say(
            "LOOPBACK RESULT: FAIL");
    }

    close(accepted);

wait:
    if (listener >= 0)
        close(listener);

    if (client >= 0)
        close(client);

    probe_say("HOME -> Quitter");
    probe_wait();

    probe_shutdown();
    return 0;
}
