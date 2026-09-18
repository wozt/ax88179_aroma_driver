#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "probe.h"

struct int_test {
    const char *name;
    int type;
    int level;
    int opt;
    int set_value;
    int do_set;
    int do_get;
};

static const struct int_test tests[] = {
    /* SOL_SOCKET */
    { "REUSEADDR",    SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR,   1,     1, 1 },
    { "KEEPALIVE",    SOCK_STREAM, SOL_SOCKET, SO_KEEPALIVE,   1,     1, 1 },
    { "DONTROUTE",    SOCK_STREAM, SOL_SOCKET, SO_DONTROUTE,   1,     1, 1 },
    { "BROADCAST",    SOCK_DGRAM,  SOL_SOCKET, SO_BROADCAST,   1,     1, 1 },
    { "OOBINLINE",    SOCK_STREAM, SOL_SOCKET, SO_OOBINLINE,   1,     1, 1 },
    { "TCPSACK",      SOCK_STREAM, SOL_SOCKET, SO_TCPSACK,     1,     1, 1 },
    { "WINSCALE",     SOCK_STREAM, SOL_SOCKET, SO_WINSCALE,    1,     1, 1 },
    { "SNDBUF",       SOCK_STREAM, SOL_SOCKET, SO_SNDBUF,      65536, 1, 1 },
    { "RCVBUF",       SOCK_STREAM, SOL_SOCKET, SO_RCVBUF,      65536, 1, 1 },
    { "SNDLOWAT",     SOCK_STREAM, SOL_SOCKET, SO_SNDLOWAT,    1,     1, 1 },
    { "RCVLOWAT",     SOCK_STREAM, SOL_SOCKET, SO_RCVLOWAT,    1,     1, 1 },
    { "ERROR",        SOCK_STREAM, SOL_SOCKET, SO_ERROR,       0,     0, 1 },
    { "TYPE",         SOCK_STREAM, SOL_SOCKET, SO_TYPE,        0,     0, 1 },
    { "HOPCNT",       SOCK_STREAM, SOL_SOCKET, SO_HOPCNT,      0,     0, 1 },
    { "MAXMSG",       SOCK_STREAM, SOL_SOCKET, SO_MAXMSG,      1460,  1, 1 },
    { "RXDATA",       SOCK_STREAM, SOL_SOCKET, SO_RXDATA,      0,     0, 1 },
    { "TXDATA",       SOCK_STREAM, SOL_SOCKET, SO_TXDATA,      0,     0, 1 },
    { "MYADDR",       SOCK_STREAM, SOL_SOCKET, SO_MYADDR,      0,     0, 1 },
    { "NBIO",         SOCK_STREAM, SOL_SOCKET, SO_NBIO,        1,     1, 1 },
    { "NONBLOCK",     SOCK_STREAM, SOL_SOCKET, SO_NONBLOCK,    1,     1, 1 },
    { "NOSLOWSTART",  SOCK_STREAM, SOL_SOCKET, SO_NOSLOWSTART, 1,     1, 1 },

    /* IPPROTO_IP */
    { "IP_TOS",       SOCK_DGRAM, IPPROTO_IP, IP_TOS,            0x10, 1, 1 },
    { "IP_TTL",       SOCK_DGRAM, IPPROTO_IP, IP_TTL,            42,   1, 1 },

    /* SOL_TCP */
    { "ACKDELAY",     SOCK_STREAM, SOL_TCP, TCP_ACKDELAYTIME, 100,  1, 1 },
    { "NOACKDELAY",   SOCK_STREAM, SOL_TCP, TCP_NOACKDELAY,   1,    1, 1 },
    { "TCP_MAXSEG",   SOCK_STREAM, SOL_TCP, TCP_MAXSEG,       1200, 1, 1 },
    { "TCP_NODELAY",  SOCK_STREAM, SOL_TCP, TCP_NODELAY,      1,    1, 1 },
    { "TCP_UNKNOWN",  SOCK_STREAM, SOL_TCP, TCP_UNKNOWN,      2,    1, 1 },
};

static int running = 1;

static int pump(void)
{
    if (running)
        running = probe_poll();

    return running;
}

static void wait_for_module(void)
{
    probe_say("Waiting 8s for AX/module startup...");

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(8000);

    while (pump() && OSGetTime() < deadline)
        OSSleepTicks(OSMillisecondsToTicks(20));
}

static void show_path(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("PATH socket failed errno=%d", errno);
        return;
    }

    uint32_t addr = 0;
    socklen_t len = sizeof(addr);

    errno = 0;
    int rc = getsockopt(fd,
                        SOL_SOCKET,
                        SO_MYADDR,
                        &addr,
                        &len);
    int err = errno;

    char ip[32] = "?";

    if (rc == 0) {
        struct in_addr a = { .s_addr = addr };
        if (!inet_ntop(AF_INET, &a, ip, sizeof(ip)))
            strcpy(ip, "?");
    }

    probe_say("PATH MYADDR rc=%d errno=%d ip=%s",
              rc, err, ip);

    close(fd);
}

static void run_int_test(const struct int_test *t)
{
    int fd = socket(AF_INET, t->type, 0);

    if (fd < 0) {
        probe_say("%-12s socket FAIL errno=%d",
                  t->name, errno);
        return;
    }

    int src = 999;
    int serr = 0;

    if (t->do_set) {
        int value = t->set_value;

        errno = 0;

        src = setsockopt(fd,
                         t->level,
                         t->opt,
                         &value,
                         sizeof(value));

        serr = errno;
    }

    int grc = 999;
    int gerr = 0;
    int value = 0x55555555;
    socklen_t len = sizeof(value);

    if (t->do_get) {
        errno = 0;

        grc = getsockopt(fd,
                         t->level,
                         t->opt,
                         &value,
                         &len);

        gerr = errno;
    }

    probe_say("%-12s s=%d/%d g=%d/%d v=%d len=%u",
              t->name,
              src,
              serr,
              grc,
              gerr,
              value,
              (unsigned)len);

    close(fd);
}


static void run_u8_test(const char *name, int opt, unsigned set_value)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("%s socket FAIL errno=%d", name, errno);
        return;
    }

    unsigned char setv = (unsigned char)set_value;

    errno = 0;
    int src = setsockopt(fd,
                         IPPROTO_IP,
                         opt,
                         &setv,
                         sizeof(setv));
    int serr = errno;

    unsigned char got = 0x55;
    socklen_t len = sizeof(got);

    errno = 0;
    int grc = getsockopt(fd,
                         IPPROTO_IP,
                         opt,
                         &got,
                         &len);
    int gerr = errno;

    probe_say("%-12s u8 s=%d/%d g=%d/%d v=%u len=%u",
              name,
              src, serr,
              grc, gerr,
              (unsigned)got,
              (unsigned)len);

    close(fd);
}

static void run_buffer_matrix(const char *name, int opt)
{
    static const int sizes[] = {
        1024,
        4096,
        8192,
        16384,
        32767,
        32768,
        65535,
        65536
    };

    probe_say("--- %s matrix ---", name);

    for (unsigned i = 0;
         i < sizeof(sizes) / sizeof(sizes[0]) && pump();
         i++) {

        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            probe_say("%s socket FAIL errno=%d",
                      name, errno);
            return;
        }

        int request = sizes[i];

        errno = 0;
        int src = setsockopt(fd,
                             SOL_SOCKET,
                             opt,
                             &request,
                             sizeof(request));
        int serr = errno;

        int got = -1;
        socklen_t len = sizeof(got);

        errno = 0;
        int grc = getsockopt(fd,
                             SOL_SOCKET,
                             opt,
                             &got,
                             &len);
        int gerr = errno;

        probe_say("%s req=%d s=%d/%d g=%d/%d v=%d",
                  name,
                  request,
                  src, serr,
                  grc, gerr,
                  got);

        close(fd);

        OSSleepTicks(OSMillisecondsToTicks(20));
    }
}

static void run_linger_test(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        probe_say("LINGER socket FAIL errno=%d", errno);
        return;
    }

    struct linger set_linger = {
        .l_onoff = 1,
        .l_linger = 2
    };

    errno = 0;

    int src = setsockopt(fd,
                         SOL_SOCKET,
                         SO_LINGER,
                         &set_linger,
                         sizeof(set_linger));

    int serr = errno;

    struct linger got = {
        .l_onoff = -1,
        .l_linger = -1
    };

    socklen_t len = sizeof(got);

    errno = 0;

    int grc = getsockopt(fd,
                         SOL_SOCKET,
                         SO_LINGER,
                         &got,
                         &len);

    int gerr = errno;

    probe_say("LINGER       s=%d/%d g=%d/%d v=%d,%d",
              src,
              serr,
              grc,
              gerr,
              got.l_onoff,
              got.l_linger);

    close(fd);
}

int main(void)
{
    if (probe_init("AX Socket Option Probe") != 0)
        return 1;

    probe_say("Native/AX sockopt compatibility audit");
    probe_say("Use HOME menu -> Quitter when done");

    wait_for_module();

    if (!running)
        goto done;

    show_path();

    probe_say("--- SOL_SOCKET / IP / TCP ---");

    for (unsigned i = 0;
         i < sizeof(tests) / sizeof(tests[0]) && pump();
         i++) {
        run_int_test(&tests[i]);
        OSSleepTicks(OSMillisecondsToTicks(20));
    }

    if (running)
        run_linger_test();

    /*
     * nsysnet's multicast TTL/loop options are byte-sized. The original
     * generic int probe was therefore not meaningful on big-endian Wii U.
     */
    if (running) {
        probe_say("--- multicast u8 ---");
        run_u8_test("MCAST_TTL", IP_MULTICAST_TTL, 7);
        run_u8_test("MCAST_LOOP", IP_MULTICAST_LOOP, 1);
    }

    /*
     * Find the real nsysnet accepted ranges for socket buffers instead
     * of guessing from one 65536-byte request.
     */
    if (running)
        run_buffer_matrix("SNDBUF", SO_SNDBUF);

    if (running)
        run_buffer_matrix("RCVBUF", SO_RCVBUF);

    if (running) {
        probe_say("--- END SOCKOPT PROBE ---");
        probe_say("HOME -> Quitter");
        probe_wait();
    }

done:
    probe_shutdown();
    return 0;
}
