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

#define SERVER_IP   "192.168.2.100"
#define SERVER_PORT 19041
#define EXPECTED    (256 * 1024)

extern int RPLWRAP(socketlasterr)(void);

static int running = 1;

static int pump(void)
{
    if (running)
        running = probe_poll();
    return running;
}

static void wait_ms(unsigned ms)
{
    OSTime end =
        OSGetTime() + OSMillisecondsToTicks(ms);

    while (pump() && OSGetTime() < end)
        OSSleepTicks(OSMillisecondsToTicks(2));
}

static void wait_for_module(void)
{
    probe_say("Waiting 8s for module startup...");
    wait_ms(8000);
}

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void show_path(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("PATH socket FAIL errno=%d", errno);
        return;
    }

    uint32_t addr = 0;
    socklen_t len = sizeof(addr);

    errno = 0;

    int rc = getsockopt(
        fd, SOL_SOCKET, SO_MYADDR,
        &addr, &len);

    char ip[32] = "?";

    if (rc == 0) {
        struct in_addr a = { .s_addr = addr };
        inet_ntop(AF_INET, &a, ip, sizeof(ip));
    }

    probe_say(
        "PATH MYADDR rc=%d errno=%d ip=%s",
        rc, errno, ip);

    close(fd);
}

static int get_int_opt(int fd, int opt, int *v)
{
    socklen_t len = sizeof(*v);
    errno = 0;

    return getsockopt(
        fd, SOL_SOCKET, opt,
        v, &len);
}

static int connect_peer(int fd)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));

    peer.sin_family = AF_INET;
    peer.sin_port = htons(SERVER_PORT);

    inet_pton(AF_INET, SERVER_IP, &peer.sin_addr);

    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    errno = 0;

    int rc = connect(
        fd,
        (struct sockaddr *)&peer,
        sizeof(peer));

    if (rc < 0 &&
        errno != EINPROGRESS &&
        errno != EALREADY &&
        errno != EWOULDBLOCK)
        return -1;

    OSTime end =
        OSGetTime() + OSMillisecondsToTicks(5000);

    while (pump() && OSGetTime() < end) {
        struct pollfd p = {
            .fd = fd,
            .events = POLLOUT
        };

        if (poll(&p, 1, 0) < 0)
            return -1;

        if (p.revents & POLLOUT) {
            int err = 0;
            socklen_t len = sizeof(err);

            if (getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &err,
                    &len) != 0)
                return -1;

            if (err) {
                errno = err;
                return -1;
            }

            return 0;
        }

        OSSleepTicks(OSMillisecondsToTicks(2));
    }

    errno = ETIMEDOUT;
    return -1;
}

static void sample_rx(int fd, const char *tag)
{
    int rx = -1;

    errno = 0;
    int rc = get_int_opt(fd, SO_RXDATA, &rx);
    int e = errno;

    struct pollfd p = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0
    };

    errno = 0;
    int prc = poll(&p, 1, 0);

    probe_say(
        "%s RXDATA=%d/%d/%d poll=%d revents=%04x",
        tag,
        rc,
        e,
        rx,
        prc,
        (unsigned)p.revents);
}

static void run_case(
    const char *name,
    int do_set,
    int requested)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        probe_say("%s socket FAIL errno=%d", name, errno);
        return;
    }

    if (do_set) {
        errno = 0;

        int rc = setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &requested,
            sizeof(requested));

        probe_say(
            "%s set RCVBUF=%d rc=%d errno=%d nerr=%d",
            name,
            requested,
            rc,
            errno,
            rc < 0 ? RPLWRAP(socketlasterr)() : 0);

        if (rc != 0) {
            close(fd);
            return;
        }
    }

    int visible = -1;

    if (get_int_opt(fd, SO_RCVBUF, &visible) != 0) {
        probe_say(
            "%s get RCVBUF FAIL errno=%d",
            name,
            errno);
        close(fd);
        return;
    }

    if (connect_peer(fd) != 0) {
        probe_say(
            "%s connect FAIL errno=%d nerr=%d",
            name,
            errno,
            RPLWRAP(socketlasterr)());
        close(fd);
        return;
    }

    probe_say("%s visible=%d connected", name, visible);

    /* Do not recv yet. Let the peer fill the native receive path. */
    wait_ms(250);
    sample_rx(fd, "  t=250ms");

    wait_ms(500);
    sample_rx(fd, "  t=750ms");

    wait_ms(750);
    sample_rx(fd, "  t=1500ms");

    unsigned total = 0;
    int eof = 0;
    uint8_t buf[4096];

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(15000);

    while (pump() &&
           OSGetTime() < deadline) {

        errno = 0;

        int n = recv(
            fd,
            buf,
            sizeof(buf),
            0);

        if (n > 0) {
            total += (unsigned)n;
            continue;
        }

        if (n == 0) {
            eof = 1;
            break;
        }

        if (would_block()) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
        }

        probe_say(
            "%s recv FAIL errno=%d nerr=%d",
            name,
            errno,
            RPLWRAP(socketlasterr)());
        break;
    }

    int after = -1;
    get_int_opt(fd, SO_RXDATA, &after);

    probe_say(
        "%s drain bytes=%u expected=%u eof=%d RXDATA-after=%d",
        name,
        total,
        (unsigned)EXPECTED,
        eof,
        after);

    const char ack[] = "DRAINED\n";
    send(fd, ack, sizeof(ack) - 1, 0);

    close(fd);
}

int main(void)
{
    if (probe_init("AX TCP Receive Pressure Probe") != 0)
        return 1;

    probe_say("RCVBUF + SO_RXDATA real TCP receive characterization");

    wait_for_module();

    if (!running)
        goto done;

    show_path();

    probe_say("--- TCP receive pressure ---");

    run_case("DEFAULT",      0, 0);
    run_case("RCVBUF-1",     1, 1);
    run_case("RCVBUF-4096",  1, 4096);
    run_case("RCVBUF-8192",  1, 8192);
    run_case("RCVBUF-16384", 1, 16384);
    run_case("RCVBUF-65535", 1, 65535);

    probe_say("--- END RECEIVE PRESSURE PROBE ---");
    probe_say("HOME -> Quitter");

    probe_wait();

done:
    probe_shutdown();
    return 0;
}
