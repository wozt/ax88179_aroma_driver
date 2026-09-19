#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wut_rplwrap.h>

#include "probe.h"

extern int RPLWRAP(socketlasterr)(void);

#define PEER_IP       "192.168.2.100"
#define PEER_CTRL     19049
#define DATA_BASE     19050

#define SOCKETS       8
#define ROUNDS        3
#define RCVBUF_VALUE  65535

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

static int make_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return -1;

    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int get_rxdata(int fd)
{
    int v = -1;
    socklen_t len = sizeof(v);

    if (getsockopt(
            fd,
            SOL_SOCKET,
            SO_RXDATA,
            &v,
            &len) != 0)
        return -1;

    return v;
}

static unsigned drain_socket(
    int fd,
    unsigned *bytes)
{
    /*
     * Native nsysnet receive APIs have already shown alignment-sensitive
     * ABIs in the multi/recvfrom_ex characterization. Keep both output
     * buffers on 0x40 boundaries while testing ordinary recvfrom().
     */
    static uint8_t buf[2048]
        __attribute__((aligned(0x40)));

    static struct sockaddr_in from
        __attribute__((aligned(0x40)));

    unsigned packets = 0;

    *bytes = 0;

    for (;;) {
        socklen_t from_len = sizeof(from);

        memset(&from, 0, sizeof(from));

        errno = 0;

        int n = recvfrom(
            fd,
            buf,
            sizeof(buf),
            0,
            (struct sockaddr *)&from,
            &from_len);

        if (n > 0) {
            packets++;
            *bytes += (unsigned)n;
            continue;
        }

        if (n == 0)
            break;

        if (errno == EAGAIN ||
            errno == EWOULDBLOCK)
            break;

        {
            int e = errno;
            int nerr = RPLWRAP(socketlasterr)();

            probe_say(
                "drain fd=%d FAIL errno=%d nerr=%d",
                fd,
                e,
                nerr);
        }
        break;
    }

    return packets;
}

static int send_control(
    int fd,
    const char *text)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));

    peer.sin_family = AF_INET;
    peer.sin_port = htons(PEER_CTRL);

    inet_pton(AF_INET, PEER_IP, &peer.sin_addr);

    return sendto(
        fd,
        text,
        strlen(text),
        0,
        (struct sockaddr *)&peer,
        sizeof(peer));
}

int main(void)
{
    if (probe_init("AX Buffer Exhaustion Probe") != 0)
        return 1;

    probe_say(
        "UDP global receive-buffer exhaustion + recovery");
    probe_say(
        "HARNESS recvfrom-aligned-v1");

    wait_ms(8000);

    if (!running)
        goto done;

    show_path();

    int ctrl = socket(AF_INET, SOCK_DGRAM, 0);

    if (ctrl < 0) {
        probe_say("control socket FAIL errno=%d", errno);
        goto wait;
    }

    int fd[SOCKETS];

    for (int i = 0; i < SOCKETS; i++)
        fd[i] = -1;

    for (int i = 0; i < SOCKETS; i++) {
        fd[i] = socket(AF_INET, SOCK_DGRAM, 0);

        if (fd[i] < 0) {
            probe_say(
                "socket[%d] FAIL errno=%d",
                i, errno);
            goto cleanup;
        }

        int rcv = RCVBUF_VALUE;

        errno = 0;

        if (setsockopt(
                fd[i],
                SOL_SOCKET,
                SO_RCVBUF,
                &rcv,
                sizeof(rcv)) != 0) {
            probe_say(
                "socket[%d] RCVBUF FAIL errno=%d",
                i, errno);
            goto cleanup;
        }

        if (make_nonblocking(fd[i]) != 0) {
            probe_say(
                "socket[%d] nonblock FAIL errno=%d",
                i, errno);
            goto cleanup;
        }

        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));

        local.sin_family = AF_INET;
        local.sin_port = htons(DATA_BASE + i);
        local.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(
                fd[i],
                (struct sockaddr *)&local,
                sizeof(local)) != 0) {
            probe_say(
                "bind[%d] port=%d FAIL errno=%d",
                i,
                DATA_BASE + i,
                errno);
            goto cleanup;
        }
    }

    probe_say(
        "bound %d UDP sockets ports %d..%d RCVBUF=%d",
        SOCKETS,
        DATA_BASE,
        DATA_BASE + SOCKETS - 1,
        RCVBUF_VALUE);

    for (int round = 1;
         running && round <= ROUNDS;
         round++) {

        /* Flush anything left from the previous round. */
        for (int i = 0; i < SOCKETS; i++) {
            unsigned bytes;
            drain_socket(fd[i], &bytes);
        }

        char msg[128];

        snprintf(
            msg, sizeof(msg),
            "READY round=%d sockets=%d base=%d\n",
            round, SOCKETS, DATA_BASE);

        if (send_control(ctrl, msg) < 0) {
            probe_say(
                "round=%d READY send FAIL errno=%d",
                round, errno);
            break;
        }

        probe_say(
            "--- round %d burst ---",
            round);

        /*
         * Peer starts immediately after READY.
         * Deliberately do not recv while it fills the queues.
         */
        wait_ms(2000);

        unsigned total_packets = 0;
        unsigned total_bytes = 0;

        for (int i = 0; i < SOCKETS; i++) {
            int rx = get_rxdata(fd[i]);

            unsigned bytes = 0;
            unsigned packets =
                drain_socket(fd[i], &bytes);

            total_packets += packets;
            total_bytes += bytes;

            probe_say(
                "round=%d sock=%d port=%d "
                "RXDATA=%d packets=%u bytes=%u",
                round,
                i,
                DATA_BASE + i,
                rx,
                packets,
                bytes);
        }

        probe_say(
            "round=%d TOTAL packets=%u bytes=%u",
            round,
            total_packets,
            total_bytes);

        snprintf(
            msg, sizeof(msg),
            "DRAINED round=%d\n",
            round);

        send_control(ctrl, msg);

        /*
         * Peer now sends three small recovery datagrams to every
         * data socket. All global receive resources should have been
         * released by the drain above.
         */
        wait_ms(1000);

        unsigned recovered = 0;
        unsigned recovery_packets = 0;

        for (int i = 0; i < SOCKETS; i++) {
            unsigned bytes = 0;
            unsigned packets =
                drain_socket(fd[i], &bytes);

            if (packets)
                recovered++;

            recovery_packets += packets;

            probe_say(
                "round=%d recovery sock=%d "
                "packets=%u bytes=%u",
                round,
                i,
                packets,
                bytes);
        }

        probe_say(
            "round=%d RECOVERY sockets=%u/%d packets=%u",
            round,
            recovered,
            SOCKETS,
            recovery_packets);
    }

cleanup:
    for (int i = 0; i < SOCKETS; i++) {
        if (fd[i] >= 0)
            close(fd[i]);
    }

    close(ctrl);

wait:
    probe_say("--- END BUFFER EXHAUSTION PROBE ---");
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    probe_shutdown();
    return 0;
}
