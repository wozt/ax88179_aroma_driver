#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#define SERVER_PORT 19040

#define FLOOD_CHUNK 1460
#define FLOOD_CAP   (4 * 1024 * 1024)

extern int RPLWRAP(socketlasterr)(void);

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

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static int wait_event(int fd, short events, unsigned timeout_ms)
{
    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(timeout_ms);

    while (pump() && OSGetTime() < deadline) {
        struct pollfd p = {
            .fd = fd,
            .events = events,
            .revents = 0
        };

        errno = 0;

        int rc = poll(&p, 1, 0);

        if (rc < 0)
            return -1;

        if (rc > 0 && (p.revents & events))
            return 0;

        if (rc > 0 &&
            (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return -1;

        OSSleepTicks(OSMillisecondsToTicks(2));
    }

    errno = ETIMEDOUT;
    return -1;
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
        fd,
        SOL_SOCKET,
        SO_MYADDR,
        &addr,
        &len);

    char ip[32] = "?";

    if (rc == 0) {
        struct in_addr a = {
            .s_addr = addr
        };

        inet_ntop(AF_INET, &a, ip, sizeof(ip));
    }

    probe_say(
        "PATH MYADDR rc=%d errno=%d ip=%s",
        rc,
        errno,
        ip);

    close(fd);
}

static void negative_matrix(const char *name, int opt)
{
    static const int values[] = {
        INT_MIN,
        -1048576,
        -65536,
        -32768,
        -1024,
        -2,
        -1
    };

    probe_say("--- %s negative matrix ---", name);

    for (unsigned i = 0;
         i < sizeof(values) / sizeof(values[0]) && pump();
         ++i) {

        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            probe_say("%s socket FAIL errno=%d", name, errno);
            return;
        }

        int request = values[i];

        errno = 0;

        int src = setsockopt(
            fd,
            SOL_SOCKET,
            opt,
            &request,
            sizeof(request));

        int serr = errno;
        int snerr =
            src < 0 ? RPLWRAP(socketlasterr)() : 0;

        int got = 0x55555555;
        socklen_t len = sizeof(got);

        errno = 0;

        int grc = getsockopt(
            fd,
            SOL_SOCKET,
            opt,
            &got,
            &len);

        int gerr = errno;
        int gnerr =
            grc < 0 ? RPLWRAP(socketlasterr)() : 0;

        probe_say(
            "%s req=%d set=%d/%d/%d get=%d/%d/%d value=%d",
            name,
            request,
            src,
            serr,
            snerr,
            grc,
            gerr,
            gnerr,
            got);

        close(fd);
    }
}

static int connect_peer(int fd)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));

    peer.sin_family = AF_INET;
    peer.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &peer.sin_addr) != 1)
        return -1;

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

    if (wait_event(fd, POLLOUT, 5000) != 0)
        return -1;

    int error = 0;
    socklen_t len = sizeof(error);

    if (getsockopt(
            fd,
            SOL_SOCKET,
            SO_ERROR,
            &error,
            &len) != 0)
        return -1;

    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

static int get_int_opt(int fd, int opt, int *value)
{
    socklen_t len = sizeof(*value);

    errno = 0;

    return getsockopt(
        fd,
        SOL_SOCKET,
        opt,
        value,
        &len);
}

static void run_pressure(const char *name, int set_buf, int requested)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        probe_say("%s socket FAIL errno=%d", name, errno);
        return;
    }

    if (set_buf) {
        errno = 0;

        int rc = setsockopt(
            fd,
            SOL_SOCKET,
            SO_SNDBUF,
            &requested,
            sizeof(requested));

        int e = errno;
        int ne =
            rc < 0 ? RPLWRAP(socketlasterr)() : 0;

        probe_say(
            "%s set SNDBUF=%d rc=%d errno=%d nerr=%d",
            name,
            requested,
            rc,
            e,
            ne);

        if (rc != 0) {
            close(fd);
            return;
        }
    }

    int visible = -1;

    if (get_int_opt(fd, SO_SNDBUF, &visible) != 0) {
        probe_say("%s get SNDBUF FAIL errno=%d", name, errno);
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

    uint8_t buf[FLOOD_CHUNK];

    for (unsigned i = 0; i < sizeof(buf); ++i)
        buf[i] = (uint8_t)(i * 31u + requested);

    size_t total = 0;
    unsigned sends = 0;
    int blocked = 0;
    int terminal_errno = 0;
    int terminal_nerr = 0;

    while (total < FLOOD_CAP && pump()) {
        errno = 0;

        int n = send(
            fd,
            buf,
            sizeof(buf),
            0);

        if (n > 0) {
            total += (size_t)n;
            sends++;
            continue;
        }

        if (n < 0 && would_block()) {
            blocked = 1;
            terminal_errno = errno;
            terminal_nerr =
                RPLWRAP(socketlasterr)();
            break;
        }

        terminal_errno = errno;

        if (n < 0)
            terminal_nerr =
                RPLWRAP(socketlasterr)();

        break;
    }

    int txdata = -1;
    int txrc =
        get_int_opt(fd, SO_TXDATA, &txdata);

    struct pollfd p = {
        .fd = fd,
        .events = POLLOUT,
        .revents = 0
    };

    errno = 0;
    int immediate_poll = poll(&p, 1, 0);

    probe_say(
        "%s visible=%d bytes=%u sends=%u blocked=%d err=%d nerr=%d",
        name,
        visible,
        (unsigned)total,
        sends,
        blocked,
        terminal_errno,
        terminal_nerr);

    probe_say(
        "%s TXDATA rc=%d value=%d immediate_poll=%d revents=%04x",
        name,
        txrc,
        txdata,
        immediate_poll,
        (unsigned)p.revents);

    /*
     * The PC deliberately stops reading first, then resumes. A writable
     * notification here demonstrates recovery from real TCP backpressure.
     */
    errno = 0;

    int resumed =
        wait_event(fd, POLLOUT, 10000);

    probe_say(
        "%s resume rc=%d errno=%d",
        name,
        resumed,
        errno);

    if (resumed == 0) {
        int after = -1;
        int arc = get_int_opt(fd, SO_TXDATA, &after);

        probe_say(
            "%s TXDATA-after rc=%d value=%d",
            name,
            arc,
            after);
    }

    shutdown(fd, SHUT_WR);

    char reply[96];
    size_t used = 0;

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(10000);

    while (used + 1 < sizeof(reply) &&
           pump() &&
           OSGetTime() < deadline) {

        errno = 0;

        int n = recv(
            fd,
            reply + used,
            sizeof(reply) - used - 1,
            0);

        if (n > 0) {
            used += (size_t)n;
            reply[used] = 0;

            if (strchr(reply, '\n'))
                break;

            continue;
        }

        if (n == 0)
            break;

        if (would_block()) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
        }

        break;
    }

    reply[used] = 0;

    probe_say(
        "%s peer='%s'",
        name,
        reply);

    close(fd);
}


/* ------------------------------------------------------------------ */
/* Deterministic single-call SNDBUF characterization                  */

/*
 * Keep both address and length 0x40-aligned so native nsysnet send()
 * can pass the payload as one IOSVec instead of splitting it into
 * before/middle/after vectors.
 *
 * 128 KiB is deliberately larger than every SNDBUF value tested so the
 * return value exposes the actual amount accepted by one send().
 */
static uint8_t single_buf[128 * 1024]
    __attribute__((aligned(0x40)));

static void run_single_shot(
    const char *name,
    int requested)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        probe_say("%s socket FAIL errno=%d", name, errno);
        return;
    }

    errno = 0;

    int rc = setsockopt(
        fd,
        SOL_SOCKET,
        SO_SNDBUF,
        &requested,
        sizeof(requested));

    int set_errno = errno;
    int set_nerr =
        rc < 0 ? RPLWRAP(socketlasterr)() : 0;

    if (rc != 0) {
        probe_say(
            "%s set SNDBUF=%d rc=%d errno=%d nerr=%d",
            name,
            requested,
            rc,
            set_errno,
            set_nerr);

        close(fd);
        return;
    }

    int visible = -1;

    if (get_int_opt(fd, SO_SNDBUF, &visible) != 0) {
        probe_say(
            "%s get SNDBUF FAIL errno=%d",
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

    /*
     * Do not log/pump between connect and the two send() calls.
     * We want the least disturbed view possible of the native send queue.
     */
    for (unsigned i = 0; i < sizeof(single_buf); ++i)
        single_buf[i] = (uint8_t)(i * 31u + requested);

    errno = 0;

    int first = send(
        fd,
        single_buf,
        sizeof(single_buf),
        0);

    int first_errno = errno;
    int first_nerr =
        first < 0 ? RPLWRAP(socketlasterr)() : 0;

    int tx1 = -1;
    int tx1_rc =
        get_int_opt(fd, SO_TXDATA, &tx1);

    errno = 0;

    int second = send(
        fd,
        single_buf,
        sizeof(single_buf),
        0);

    int second_errno = errno;
    int second_nerr =
        second < 0 ? RPLWRAP(socketlasterr)() : 0;

    int tx2 = -1;
    int tx2_rc =
        get_int_opt(fd, SO_TXDATA, &tx2);

    probe_say(
        "%s visible=%d first=%d err=%d nerr=%d TX1=%d/%d",
        name,
        visible,
        first,
        first_errno,
        first_nerr,
        tx1_rc,
        tx1);

    probe_say(
        "%s second=%d err=%d nerr=%d TX2=%d/%d",
        name,
        second,
        second_errno,
        second_nerr,
        tx2_rc,
        tx2);

    shutdown(fd, SHUT_WR);

    char reply[96];
    size_t used = 0;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(10000);

    while (used + 1 < sizeof(reply) &&
           pump() &&
           OSGetTime() < deadline) {

        errno = 0;

        int n = recv(
            fd,
            reply + used,
            sizeof(reply) - used - 1,
            0);

        if (n > 0) {
            used += (size_t)n;
            reply[used] = 0;

            if (strchr(reply, '\n'))
                break;

            continue;
        }

        if (n == 0)
            break;

        if (would_block()) {
            OSSleepTicks(
                OSMillisecondsToTicks(2));
            continue;
        }

        break;
    }

    reply[used] = 0;

    probe_say(
        "%s peer='%s'",
        name,
        reply);

    close(fd);
}

int main(void)
{
    if (probe_init("AX TCP Backpressure Probe") != 0)
        return 1;

    probe_say("SNDBUF/RCVBUF + real TCP backpressure characterization");

    wait_for_module();

    if (!running)
        goto done;

    show_path();

    if (running) {
        probe_say("--- TCP SNDBUF SINGLE SHOT ---");

        probe_say(
            "SINGLE-BUF addr=%08x mod40=%u size=%u",
            (unsigned)(uintptr_t)single_buf,
            (unsigned)((uintptr_t)single_buf & 0x3f),
            (unsigned)sizeof(single_buf));

        run_single_shot("ONE-1", 1);
        run_single_shot("ONE-4096", 4096);
        run_single_shot("ONE-8192", 8192);
        run_single_shot("ONE-16384", 16384);
        run_single_shot("ONE-65535", 65535);
    }

    probe_say("--- END BACKPRESSURE PROBE ---");
    probe_say("HOME -> Quitter");

    probe_wait();

done:
    probe_shutdown();
    return 0;
}
