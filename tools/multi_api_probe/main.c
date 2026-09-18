#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/dynload.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "probe.h"

#define PC_IP      "192.168.2.100"
#define RX_PORT    19020
#define TX_PORT_A  19021
#define TX_PORT_B  19022

/*
 * nsysnet exports these functions but the normal WUT headers do not
 * currently provide their full public declarations.
 *
 * Keep these structures explicit so we can characterize the native ABI
 * before implementing the AX shim.
 */
struct ax_recvfrom_multi_buffers {
    void *buffer;
    unsigned int bufferlen;

    struct sockaddr *froms;
    unsigned int fromslen;

    int *results;
    unsigned int resultslen;
};

struct ax_sendto_multi_ex_buffers {
    void *buffer;
    unsigned int bufferlen;

    int *datagram_lens;
    unsigned int datagram_lens_len;

    struct sockaddr *dests;
    unsigned int destslen;

    int *results;
    unsigned int resultslen;
};

typedef int (*recvfrom_multi_fn)(
    int socket,
    int flags,
    struct ax_recvfrom_multi_buffers *buffs,
    int recv_datagram_len,
    int recv_datagram_count,
    struct timeval *timeout);

typedef int (*sendto_multi_ex_fn)(
    int socket,
    int flags,
    struct ax_sendto_multi_ex_buffers *buffs,
    int send_datagram_count);

static OSDynLoad_Module nsysnet_module;
static recvfrom_multi_fn p_recvfrom_multi;
static sendto_multi_ex_fn p_sendto_multi_ex;

static int resolve_multi_exports(void)
{
    OSDynLoad_Error err;

    err = OSDynLoad_Acquire("nsysnet.rpl",
                            &nsysnet_module);

    if (err != OS_DYNLOAD_OK) {
        probe_say("OSDynLoad_Acquire(nsysnet) FAIL %08x",
                  (unsigned)err);
        return -1;
    }

    err = OSDynLoad_FindExport(
        nsysnet_module,
        OS_DYNLOAD_EXPORT_FUNC,
        "recvfrom_multi",
        (void **)&p_recvfrom_multi);

    if (err != OS_DYNLOAD_OK || !p_recvfrom_multi) {
        probe_say("FindExport recvfrom_multi FAIL %08x",
                  (unsigned)err);
        return -1;
    }

    err = OSDynLoad_FindExport(
        nsysnet_module,
        OS_DYNLOAD_EXPORT_FUNC,
        "sendto_multi_ex",
        (void **)&p_sendto_multi_ex);

    if (err != OS_DYNLOAD_OK || !p_sendto_multi_ex) {
        probe_say("FindExport sendto_multi_ex FAIL %08x",
                  (unsigned)err);
        return -1;
    }

    probe_say("nsysnet multi exports resolved");
    return 0;
}

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
        probe_say("PATH socket FAIL errno=%d", errno);
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

    char ip[32] = "?";

    if (rc == 0) {
        struct in_addr a = { .s_addr = addr };

        inet_ntop(AF_INET,
                  &a,
                  ip,
                  sizeof(ip));
    }

    probe_say("PATH rc=%d errno=%d ip=%s",
              rc,
              errno,
              ip);

    close(fd);
}

static void setup_addr(struct sockaddr_in *addr,
                       const char *ip,
                       unsigned port)
{
    memset(addr, 0, sizeof(*addr));

    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = inet_addr(ip);
}

static void test_sendto_multi_ex(void)
{
    probe_say("--- sendto_multi_ex ---");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("socket FAIL errno=%d", errno);
        return;
    }

    /*
     * Two concatenated datagrams:
     *
     *   "EX-A"       -> 4 bytes
     *   "EX-BBBBB"   -> 8 bytes
     */
    static const char payload[] =
        "EX-A"
        "EX-BBBBB";

    int lens[2] = {
        4,
        8
    };

    struct sockaddr_in dests[2];

    setup_addr(&dests[0],
               PC_IP,
               TX_PORT_A);

    setup_addr(&dests[1],
               PC_IP,
               TX_PORT_B);

    int results[2] = {
        0x55555555,
        0x55555555
    };

    struct ax_sendto_multi_ex_buffers b = {
        .buffer = (void *)payload,
        .bufferlen = sizeof(payload) - 1,

        .datagram_lens = lens,
        .datagram_lens_len = 2,

        .dests = (struct sockaddr *)dests,
        .destslen = 2,

        .results = results,
        .resultslen = 2
    };

    errno = 0;

    int rc = p_sendto_multi_ex(fd,
                               0,
                               &b,
                               2);

    int err = errno;

    probe_say(
        "sendto_multi_ex rc=%d errno=%d",
        rc,
        err);

    probe_say(
        "results[0]=%d results[1]=%d",
        results[0],
        results[1]);

    close(fd);
}

static void test_recvfrom_multi(void)
{
    probe_say("--- recvfrom_multi ---");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("socket FAIL errno=%d", errno);
        return;
    }

    int one = 1;

    setsockopt(fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &one,
               sizeof(one));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));

    local.sin_family = AF_INET;
    local.sin_port = htons(RX_PORT);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd,
             (struct sockaddr *)&local,
             sizeof(local)) != 0) {
        probe_say("bind FAIL errno=%d", errno);
        close(fd);
        return;
    }

    /*
     * Three fixed 64-byte receive slots.
     */
    uint8_t data[3][64];

    memset(data, 0x55, sizeof(data));

    struct sockaddr_in froms[3];

    memset(froms, 0, sizeof(froms));

    int results[3] = {
        0x55555555,
        0x55555555,
        0x55555555
    };

    struct ax_recvfrom_multi_buffers b = {
        .buffer = data,
        .bufferlen = sizeof(data),

        .froms = (struct sockaddr *)froms,
        .fromslen = 3,

        .results = results,
        .resultslen = 3
    };

    struct timeval timeout = {
        .tv_sec = 10,
        .tv_usec = 0
    };

    probe_say(
        "waiting for 3 datagrams on UDP :%d...",
        RX_PORT);

    errno = 0;

    int rc = p_recvfrom_multi(fd,
                              0,
                              &b,
                              64,
                              3,
                              &timeout);

    int err = errno;

    probe_say(
        "recvfrom_multi rc=%d errno=%d",
        rc,
        err);

    for (int i = 0; i < 3; i++) {
        char ip[32] = "?";

        inet_ntop(AF_INET,
                  &froms[i].sin_addr,
                  ip,
                  sizeof(ip));

        int n = results[i];

        char text[65];
        memset(text, 0, sizeof(text));

        if (n > 0 && n <= 64)
            memcpy(text, data[i], n);

        probe_say(
            "[%d] result=%d from=%s:%u data='%s'",
            i,
            results[i],
            ip,
            ntohs(froms[i].sin_port),
            text);
    }

    close(fd);
}

int main(void)
{
    if (probe_init("AX Multi API Probe") != 0)
        return 1;

    probe_say("nsysnet multi-datagram ABI audit");
    probe_say("FIRST RUN MUST USE route=native");
    probe_say("Start the PC helper BEFORE this probe.");

    wait_for_module();

    if (!running)
        goto done;

    if (resolve_multi_exports() != 0)
        goto wait;

    show_path();

    test_sendto_multi_ex();

    if (running)
        test_recvfrom_multi();

    probe_say("--- END MULTI API PROBE ---");

wait:
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    if (nsysnet_module) {
        OSDynLoad_Release(nsysnet_module);
        nsysnet_module = NULL;
    }

    probe_shutdown();
    return 0;
}
