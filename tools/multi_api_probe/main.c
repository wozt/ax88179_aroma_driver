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

struct ax_nsysnet_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
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
    struct ax_nsysnet_timeval *timeout);

typedef int (*sendto_multi_fn)(
    int socket,
    const void *buffer,
    int len,
    int flags,
    const struct sockaddr *dest_addrs,
    int dest_count);

typedef int (*sendto_multi_ex_fn)(
    int socket,
    int flags,
    struct ax_sendto_multi_ex_buffers *buffs,
    int send_datagram_count);

typedef int (*socketlasterr_fn)(void);

/*
 * socket() de WUT renvoie un fd POSIX/devoptab.
 * Les exports nsysnet bruts attendent le fd nsysnet interne.
 */
extern int __wut_get_nsysnet_fd(int fd);

static OSDynLoad_Module nsysnet_module;
static recvfrom_multi_fn p_recvfrom_multi;
static sendto_multi_fn p_sendto_multi;
static sendto_multi_ex_fn p_sendto_multi_ex;
static socketlasterr_fn p_socketlasterr;

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
        "sendto_multi",
        (void **)&p_sendto_multi);

    if (err != OS_DYNLOAD_OK || !p_sendto_multi) {
        probe_say("FindExport sendto_multi FAIL %08x",
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

    err = OSDynLoad_FindExport(
        nsysnet_module,
        OS_DYNLOAD_EXPORT_FUNC,
        "socketlasterr",
        (void **)&p_socketlasterr);

    if (err != OS_DYNLOAD_OK || !p_socketlasterr) {
        probe_say("FindExport socketlasterr FAIL %08x",
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
    probe_say("--- sendto_multi baseline + EX matrix ---");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        probe_say("socket FAIL errno=%d", errno);
        return;
    }

    int nsfd = __wut_get_nsysnet_fd(fd);

    probe_say("send fd=%d raw_nsysnet_fd=%d",
              fd,
              nsfd);

    if (nsfd < 0) {
        probe_say("fd conversion FAIL errno=%d", errno);
        close(fd);
        return;
    }

    /*
     * Huge aligned backing buffers so every tested capacity is safe.
     */
    static uint8_t payload[0x100]
        __attribute__((aligned(0x40)));

    static int lens[64]
        __attribute__((aligned(0x40)));

    static struct sockaddr_in dests[8]
        __attribute__((aligned(0x40)));

    static int results[64]
        __attribute__((aligned(0x40)));

    static struct ax_sendto_multi_ex_buffers b
        __attribute__((aligned(0x40)));

    memset(payload, 0, sizeof(payload));
    memset(lens, 0, sizeof(lens));
    memset(dests, 0, sizeof(dests));

    memcpy(payload + 0, "EX-A", 4);
    memcpy(payload + 4, "EX-BBBBB", 8);

    lens[0] = 4;
    lens[1] = 8;

    setup_addr(&dests[0], PC_IP, TX_PORT_A);
    setup_addr(&dests[1], PC_IP, TX_PORT_B);

    /*
     * First prove that raw nsysnet sendto_multi works with this fd and
     * destination array.
     */
    static const char base[] = "MULTI-BASE";

    errno = 0;

    int base_rc = p_sendto_multi(
        nsfd,
        base,
        sizeof(base) - 1,
        0,
        (const struct sockaddr *)dests,
        2);

    int base_nerr =
        base_rc < 0 ? p_socketlasterr() : 0;

    probe_say(
        "sendto_multi baseline rc=%d nsysnet_err=%d",
        base_rc,
        base_nerr);

    /*
     * Candidate meanings:
     *
     * bufferlen:
     *   12    exact useful payload
     *   0x20  payload padded to cache-line size
     *   0x100 actual backing-buffer capacity
     *
     * lens/results len:
     *   2     element count
     *   8     byte size OR padded element count (8 ints = 0x20)
     *   0x20  padded byte size
     *
     * destslen:
     *   2     destination count
     *   0x20  two sockaddr_in in bytes
     *   0x40  possible extra vector padding
     */
    static const unsigned bufferlens[] = {
        12,
        0x40,
        0x100
    };

    static const unsigned intlens[] = {
        2,
        16,
        0x40
    };

    static const unsigned destlens[] = {
        2,
        4,
        0x40
    };

    int attempt = 0;
    int found = 0;

    for (unsigned bi = 0;
         bi < sizeof(bufferlens) / sizeof(bufferlens[0]);
         bi++) {

        for (unsigned li = 0;
             li < sizeof(intlens) / sizeof(intlens[0]);
             li++) {

            for (unsigned di = 0;
                 di < sizeof(destlens) / sizeof(destlens[0]);
                 di++) {

                for (unsigned ri = 0;
                     ri < sizeof(intlens) / sizeof(intlens[0]);
                     ri++) {

                    attempt++;

                    for (unsigned i = 0; i < 64; i++)
                        results[i] = 0x55555555;

                    memset(&b, 0, sizeof(b));

                    b.buffer = payload;
                    b.bufferlen = bufferlens[bi];

                    b.datagram_lens = lens;
                    b.datagram_lens_len = intlens[li];

                    b.dests = (struct sockaddr *)dests;
                    b.destslen = destlens[di];

                    b.results = results;
                    b.resultslen = intlens[ri];

                    int rc = p_sendto_multi_ex(
                        nsfd,
                        0,
                        &b,
                        2);

                    int nerr =
                        rc < 0 ? p_socketlasterr() : 0;

                    /*
                     * Do not spam all 81 EINVAL cases.
                     * Only print a candidate that changes behavior.
                     */
                    if (rc >= 0 || nerr != 11) {
                        probe_say(
                            "EX candidate #%d B=%u L=%u D=%u R=%u",
                            attempt,
                            b.bufferlen,
                            b.datagram_lens_len,
                            b.destslen,
                            b.resultslen);

                        probe_say(
                            "EX rc=%d nerr=%d results=%d,%d",
                            rc,
                            nerr,
                            results[0],
                            results[1]);

                        found = 1;
                        goto matrix_done;
                    }
                }
            }
        }
    }

matrix_done:

    if (!found) {
        probe_say(
            "EX MATRIX: all %d candidates returned EINVAL",
            attempt);
    } else {
        probe_say("EX MATRIX: non-EINVAL candidate FOUND");
    }

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
    uint8_t data[3][64]
        __attribute__((aligned(0x40)));

    memset(data, 0x55, sizeof(data));

    struct sockaddr_in froms[4]
        __attribute__((aligned(0x40)));

    memset(froms, 0, sizeof(froms));

    int results[16]
        __attribute__((aligned(0x40)));

    for (int i = 0; i < 16; i++)
        results[i] = 0x55555555;

    struct ax_recvfrom_multi_buffers b
        __attribute__((aligned(0x40))) = {
        .buffer = data,
        .bufferlen = sizeof(data),

        .froms = (struct sockaddr *)froms,
        .fromslen = 0x40,

        .results = results,
        .resultslen = 0x40
    };

    struct ax_nsysnet_timeval timeout
        __attribute__((aligned(0x40))) = {
        .tv_sec = 10,
        .tv_usec = 0
    };

    probe_say(
        "timeval sizeof libc=%u raw_nsysnet=%u",
        (unsigned)sizeof(struct timeval),
        (unsigned)sizeof(struct ax_nsysnet_timeval));

    probe_say(
        "waiting for 3 datagrams on UDP :%d...",
        RX_PORT);

    int nsfd = __wut_get_nsysnet_fd(fd);

    probe_say("recv fd=%d raw_nsysnet_fd=%d",
              fd,
              nsfd);

    if (nsfd < 0) {
        probe_say("recv fd conversion FAIL errno=%d",
                  errno);
        close(fd);
        return;
    }

    probe_say(
        "recv align b=%02x data=%02x froms=%02x results=%02x timeout=%02x",
        (unsigned)((uintptr_t)&b & 0x3f),
        (unsigned)((uintptr_t)data & 0x3f),
        (unsigned)((uintptr_t)froms & 0x3f),
        (unsigned)((uintptr_t)results & 0x3f),
        (unsigned)((uintptr_t)&timeout & 0x3f));

    errno = 0;

    int rc = p_recvfrom_multi(nsfd,
                              0,
                              &b,
                              64,
                              3,
                              &timeout);

    int err = errno;
    int nerr = rc < 0 ? p_socketlasterr() : 0;

    probe_say(
        "recvfrom_multi rc=%d errno=%d nsysnet_err=%d",
        rc,
        err,
        nerr);

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
