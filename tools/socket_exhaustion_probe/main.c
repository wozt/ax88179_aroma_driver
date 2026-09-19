#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wut_rplwrap.h>

#include "probe.h"

#define MAX_SOCKETS 64
#define ROUNDS      3

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

static void show_path(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say(
            "PATH socket FAIL errno=%d nerr=%d",
            errno,
            RPLWRAP(socketlasterr)());
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

        inet_ntop(
            AF_INET,
            &a,
            ip,
            sizeof(ip));
    }

    probe_say(
        "PATH MYADDR rc=%d errno=%d ip=%s",
        rc,
        errno,
        ip);

    close(fd);
}

static void close_array(
    int *fds,
    int count,
    const char *name)
{
    int errors = 0;

    for (int i = 0; i < count; i++) {
        errno = 0;

        if (close(fds[i]) != 0)
            errors++;
    }

    probe_say(
        "%s close count=%d errors=%d",
        name,
        count,
        errors);
}

static void run_single_type(
    const char *name,
    int type,
    int round)
{
    int fds[MAX_SOCKETS];
    int count = 0;
    int failure_errno = 0;
    int failure_nerr = 0;
    uint32_t mask = 0;

    memset(fds, -1, sizeof(fds));

    while (count < MAX_SOCKETS && pump()) {
        errno = 0;

        int fd = socket(
            AF_INET,
            type,
            0);

        if (fd < 0) {
            failure_errno = errno;
            failure_nerr =
                RPLWRAP(socketlasterr)();
            break;
        }

        fds[count++] = fd;

        if (fd >= 0 && fd < 32)
            mask |= 1u << fd;
    }

    probe_say(
        "%s round=%d count=%d first=%d last=%d",
        name,
        round,
        count,
        count ? fds[0] : -1,
        count ? fds[count - 1] : -1);

    probe_say(
        "%s saturation errno=%d nerr=%d mask=%08x",
        name,
        failure_errno,
        failure_nerr,
        (unsigned)mask);

    if (count == MAX_SOCKETS)
        probe_say(
            "%s LIMIT >= %d (probe cap reached)",
            name,
            MAX_SOCKETS);

    close_array(
        fds,
        count,
        name);

    /*
     * Immediately verify resource reclamation.
     */
    errno = 0;

    int fd = socket(
        AF_INET,
        type,
        0);

    int reopen_errno = errno;
    int reopen_nerr =
        fd < 0
            ? RPLWRAP(socketlasterr)()
            : 0;

    probe_say(
        "%s reopen fd=%d errno=%d nerr=%d",
        name,
        fd,
        reopen_errno,
        reopen_nerr);

    if (fd >= 0)
        close(fd);
}

static void run_mixed(int round)
{
    int fds[MAX_SOCKETS];
    int types[MAX_SOCKETS];

    int count = 0;
    int tcp_count = 0;
    int udp_count = 0;
    int failure_errno = 0;
    int failure_nerr = 0;
    uint32_t mask = 0;

    memset(fds, -1, sizeof(fds));
    memset(types, 0, sizeof(types));

    while (count < MAX_SOCKETS && pump()) {
        int type =
            (count & 1)
                ? SOCK_DGRAM
                : SOCK_STREAM;

        errno = 0;

        int fd = socket(
            AF_INET,
            type,
            0);

        if (fd < 0) {
            failure_errno = errno;
            failure_nerr =
                RPLWRAP(socketlasterr)();
            break;
        }

        fds[count] = fd;
        types[count] = type;

        if (type == SOCK_STREAM)
            tcp_count++;
        else
            udp_count++;

        if (fd >= 0 && fd < 32)
            mask |= 1u << fd;

        count++;
    }

    probe_say(
        "MIXED round=%d total=%d tcp=%d udp=%d",
        round,
        count,
        tcp_count,
        udp_count);

    probe_say(
        "MIXED saturation errno=%d nerr=%d mask=%08x",
        failure_errno,
        failure_nerr,
        (unsigned)mask);

    if (count == MAX_SOCKETS)
        probe_say(
            "MIXED LIMIT >= %d (probe cap reached)",
            MAX_SOCKETS);

    close_array(
        fds,
        count,
        "MIXED");

    /*
     * Verify that both socket types work again after exhaustion.
     */
    errno = 0;
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int tcp_errno = errno;
    int tcp_nerr =
        tcp < 0
            ? RPLWRAP(socketlasterr)()
            : 0;

    errno = 0;
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int udp_errno = errno;
    int udp_nerr =
        udp < 0
            ? RPLWRAP(socketlasterr)()
            : 0;

    probe_say(
        "MIXED reopen tcp=%d/%d/%d udp=%d/%d/%d",
        tcp,
        tcp_errno,
        tcp_nerr,
        udp,
        udp_errno,
        udp_nerr);

    if (tcp >= 0)
        close(tcp);

    if (udp >= 0)
        close(udp);
}

int main(void)
{
    if (probe_init("AX Socket Exhaustion Probe") != 0)
        return 1;

    probe_say(
        "nsysnet socket exhaustion/reclamation audit");

    wait_for_module();

    if (!running)
        goto done;

    show_path();

    for (int round = 1;
         round <= ROUNDS && pump();
         round++) {

        probe_say(
            "--- TCP exhaustion round %d ---",
            round);

        run_single_type(
            "TCP",
            SOCK_STREAM,
            round);

        probe_say(
            "--- UDP exhaustion round %d ---",
            round);

        run_single_type(
            "UDP",
            SOCK_DGRAM,
            round);

        probe_say(
            "--- MIXED exhaustion round %d ---",
            round);

        run_mixed(round);

        OSSleepTicks(
            OSMillisecondsToTicks(100));
    }

    probe_say("--- END SOCKET EXHAUSTION PROBE ---");
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    probe_shutdown();
    return 0;
}
