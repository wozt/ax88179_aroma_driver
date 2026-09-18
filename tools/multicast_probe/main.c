#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "probe.h"

#define GROUP_ADDR "239.42.42.42"
#define WIIU_PORT  19001
#define PC_PORT    19002

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

static int recv_packet(int fd,
                       char *buf,
                       size_t size,
                       struct sockaddr_in *src)
{
    socklen_t slen = sizeof(*src);

    int n = recvfrom(fd,
                     buf,
                     size - 1,
                     0,
                     (struct sockaddr *)src,
                     &slen);

    if (n < 0)
        return -1;

    buf[n] = 0;
    return n;
}

int main(void)
{
    if (probe_init("AX Multicast Probe") != 0)
        return 1;

    probe_say("Real multicast JOIN/TX/RX/DROP test");
    probe_say("Group %s", GROUP_ADDR);

    wait_for_module();

    if (!running)
        goto done;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("FAIL socket errno=%d", errno);
        goto wait;
    }

    uint32_t myaddr = 0;
    socklen_t mylen = sizeof(myaddr);

    errno = 0;
    int rc = getsockopt(fd,
                        SOL_SOCKET,
                        SO_MYADDR,
                        &myaddr,
                        &mylen);

    if (rc != 0) {
        probe_say("FAIL SO_MYADDR rc=%d errno=%d",
                  rc, errno);
        close(fd);
        goto wait;
    }

    struct in_addr local = {
        .s_addr = myaddr
    };

    char local_ip[32] = "?";
    inet_ntop(AF_INET,
              &local,
              local_ip,
              sizeof(local_ip));

    probe_say("Local interface: %s", local_ip);

    int one = 1;

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &one,
                   sizeof(one)) != 0) {
        probe_say("FAIL SO_REUSEADDR errno=%d", errno);
        close(fd);
        goto wait;
    }

    /*
     * Non-blocking receive lets us keep ProcUI alive while waiting
     * for packets.
     */
    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_NONBLOCK,
                   &one,
                   sizeof(one)) != 0) {
        probe_say("FAIL SO_NONBLOCK errno=%d", errno);
        close(fd);
        goto wait;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(WIIU_PORT);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd,
             (struct sockaddr *)&bind_addr,
             sizeof(bind_addr)) != 0) {
        probe_say("FAIL bind :%d errno=%d",
                  WIIU_PORT, errno);
        close(fd);
        goto wait;
    }

    /*
     * Explicitly select the AX interface for multicast TX.
     */
    errno = 0;

    rc = setsockopt(fd,
                    IPPROTO_IP,
                    IP_MULTICAST_IF,
                    &local,
                    sizeof(local));

    probe_say("IP_MULTICAST_IF rc=%d errno=%d",
              rc, errno);

    if (rc != 0) {
        close(fd);
        goto wait;
    }

    unsigned char ttl = 1;

    if (setsockopt(fd,
                   IPPROTO_IP,
                   IP_MULTICAST_TTL,
                   &ttl,
                   sizeof(ttl)) != 0) {
        probe_say("FAIL MCAST_TTL errno=%d", errno);
        close(fd);
        goto wait;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));

    mreq.imr_multiaddr.s_addr =
        inet_addr(GROUP_ADDR);

    mreq.imr_interface = local;

    errno = 0;

    rc = setsockopt(fd,
                    IPPROTO_IP,
                    IP_ADD_MEMBERSHIP,
                    &mreq,
                    sizeof(mreq));

    probe_say("ADD_MEMBERSHIP rc=%d errno=%d",
              rc, errno);

    if (rc != 0) {
        close(fd);
        goto wait;
    }

    probe_say("%s", "");
    probe_say("READY");
    probe_say("Run PC sender now");
    probe_say("waiting PC-READY on %s:%d",
              GROUP_ADDR, WIIU_PORT);

    int got_ready = 0;

    OSTime ready_deadline =
        OSGetTime() + OSMillisecondsToTicks(30000);

    while (pump() &&
           OSGetTime() < ready_deadline) {

        char buf[256];
        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));

        int n = recv_packet(fd,
                            buf,
                            sizeof(buf),
                            &src);

        if (n >= 0) {
            char src_ip[32] = "?";

            inet_ntop(AF_INET,
                      &src.sin_addr,
                      src_ip,
                      sizeof(src_ip));

            probe_say("RX '%s' from %s:%u",
                      buf,
                      src_ip,
                      ntohs(src.sin_port));

            if (!strcmp(buf, "PC-READY")) {
                got_ready = 1;
                break;
            }
        }

        OSSleepTicks(OSMillisecondsToTicks(20));
    }

    if (!got_ready) {
        probe_say("FAIL: no multicast RX");
        setsockopt(fd,
                   IPPROTO_IP,
                   IP_DROP_MEMBERSHIP,
                   &mreq,
                   sizeof(mreq));
        close(fd);
        goto wait;
    }

    /*
     * Multicast TX back to the PC.
     */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));

    dst.sin_family = AF_INET;
    dst.sin_port = htons(PC_PORT);
    dst.sin_addr.s_addr = inet_addr(GROUP_ADDR);

    int tx_ok = 1;

    for (int i = 0; i < 5; i++) {
        static const char msg[] = "WIIU-TX";

        int n = sendto(fd,
                       msg,
                       sizeof(msg) - 1,
                       0,
                       (struct sockaddr *)&dst,
                       sizeof(dst));

        if (n != (int)(sizeof(msg) - 1)) {
            probe_say("TX FAIL rc=%d errno=%d",
                      n, errno);
            tx_ok = 0;
            break;
        }

        OSSleepTicks(OSMillisecondsToTicks(100));
    }

    if (tx_ok)
        probe_say("Multicast TX queued OK");

    /*
     * Leave the group. From this point PC multicast DROPTEST packets
     * must not appear, but unicast END must still arrive.
     */
    errno = 0;

    rc = setsockopt(fd,
                    IPPROTO_IP,
                    IP_DROP_MEMBERSHIP,
                    &mreq,
                    sizeof(mreq));

    probe_say("DROP_MEMBERSHIP rc=%d errno=%d",
              rc, errno);

    if (rc != 0) {
        close(fd);
        goto wait;
    }

    int drop_failed = 0;
    int got_end = 0;

    OSTime drop_deadline =
        OSGetTime() + OSMillisecondsToTicks(20000);

    probe_say("Waiting DROPTEST rejection + unicast END...");

    while (pump() &&
           OSGetTime() < drop_deadline) {

        char buf[256];
        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));

        int n = recv_packet(fd,
                            buf,
                            sizeof(buf),
                            &src);

        if (n >= 0) {
            if (!strcmp(buf, "DROPTEST")) {
                probe_say("FAIL: multicast received after DROP");
                drop_failed = 1;
            }

            if (!strcmp(buf, "END")) {
                probe_say("Unicast END received");
                got_end = 1;
                break;
            }
        }

        OSSleepTicks(OSMillisecondsToTicks(20));
    }

    if (!got_end) {
        probe_say("INCONCLUSIVE: no END control packet");
    } else if (drop_failed) {
        probe_say("MULTICAST RESULT: FAIL");
    } else if (!tx_ok) {
        probe_say("MULTICAST RESULT: TX FAIL");
    } else {
        probe_say("MULTICAST RESULT: PASS");
        probe_say("JOIN/RX/TX/DROP all behaved");
    }

    close(fd);

wait:
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    probe_shutdown();
    return 0;
}
