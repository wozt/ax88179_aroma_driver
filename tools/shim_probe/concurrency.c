/*
 * Sustained AX/lwIP concurrency test.
 *
 * The main thread creates all title-visible sockets. Worker threads then
 * exercise them concurrently without touching the shim enrollment state.
 */
#include "concurrency.h"
#include "probe.h"

#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <whb/log.h>

#define WORKERS          8
#define TCP_WORKERS      4
#define ROUNDS           8192
#define ROUND_PAUSE_MS   5
#define TEST_TIMEOUT_MS  180000

struct job {
    int fd;
    int type;
    int error;
    unsigned bytes;
    atomic_int done;
    atomic_int rounds;
};

static struct job jobs[WORKERS];

static OSThread threads[WORKERS]
    __attribute__((aligned(64)));

static unsigned char stacks[WORKERS][32 * 1024]
    __attribute__((aligned(64)));

static atomic_int cancel_jobs;
static atomic_int start_jobs;

static int
wait_fd(int fd, short events)
{
    OSTime end =
        OSGetTime() +
        OSMillisecondsToTicks(4000);

    while (!atomic_load(&cancel_jobs) &&
           OSGetTime() < end) {

        struct pollfd p = {
            .fd = fd,
            .events = events
        };

        int n = poll(&p, 1, 0);

        if (n < 0)
            return -1;

        if (n &&
            (p.revents & events))
            return 0;

        if (n &&
            (p.revents &
             (POLLERR | POLLHUP | POLLNVAL))) {
            errno = EIO;
            return -1;
        }

        OSSleepTicks(
            OSMillisecondsToTicks(1));
    }

    errno =
        atomic_load(&cancel_jobs)
            ? ECANCELED
            : ETIMEDOUT;

    return -1;
}

static int
exchange(struct job *j, unsigned round)
{
    static const unsigned sizes[] = {
        32,
        504,
        1024,
        1400
    };

    unsigned size =
        sizes[round % 4];

    unsigned char tx[1400];
    unsigned char rx[1400];

    for (unsigned i = 0; i < size; ++i)
        tx[i] =
            (unsigned char)(
                i * 31 +
                round +
                j->type * 7);

    unsigned sent = 0;
    unsigned received = 0;

    while (sent < size) {
        if (wait_fd(j->fd, POLLOUT))
            return -1;

        int n =
            send(
                j->fd,
                tx + sent,
                size - sent,
                0);

        if (n < 0 &&
            (errno == EAGAIN ||
             errno == EWOULDBLOCK))
            continue;

        if (n <= 0)
            return -1;

        if (j->type == SOCK_DGRAM &&
            (unsigned)n != size) {
            errno = EIO;
            return -1;
        }

        sent += (unsigned)n;
    }

    while (received < size) {
        if (wait_fd(j->fd, POLLIN))
            return -1;

        int n =
            recv(
                j->fd,
                rx + received,
                size - received,
                0);

        if (n < 0 &&
            (errno == EAGAIN ||
             errno == EWOULDBLOCK))
            continue;

        if (n <= 0) {
            if (!n)
                errno = ECONNRESET;
            return -1;
        }

        if (j->type == SOCK_DGRAM &&
            (unsigned)n != size) {
            errno = EIO;
            return -1;
        }

        received += (unsigned)n;
    }

    if (memcmp(tx, rx, size) != 0) {
        errno = EIO;
        return -1;
    }

    j->bytes += size;
    return 0;
}

static int
worker(int index, const char **unused)
{
    (void)unused;

    struct job *j =
        &jobs[index];

    while (!atomic_load(&start_jobs) &&
           !atomic_load(&cancel_jobs))
        OSSleepTicks(
            OSMillisecondsToTicks(1));

    for (unsigned r = 0;
         r < ROUNDS &&
         !atomic_load(&cancel_jobs);
         ++r) {

        if (exchange(j, r)) {
            j->error =
                errno ? errno : EIO;
            break;
        }

        atomic_store(
            &j->rounds,
            (int)r + 1);

        OSSleepTicks(
            OSMillisecondsToTicks(
                ROUND_PAUSE_MS));
    }

    atomic_store(
        &j->done,
        1);

    return 0;
}

static int
connect_fd(
    int fd,
    int (*pump)(void))
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(18879)
    };

    inet_pton(
        AF_INET,
        "192.168.2.100",
        &addr.sin_addr);

    if (fcntl(
            fd,
            F_SETFL,
            O_NONBLOCK) < 0)
        return -1;

    int rc =
        connect(
            fd,
            (struct sockaddr *)&addr,
            sizeof(addr));

    if (rc < 0 &&
        errno != EINPROGRESS &&
        errno != EALREADY &&
        errno != EWOULDBLOCK)
        return -1;

    OSTime end =
        OSGetTime() +
        OSMillisecondsToTicks(4000);

    while (pump() &&
           OSGetTime() < end) {

        struct pollfd p = {
            .fd = fd,
            .events = POLLOUT
        };

        if (poll(&p, 1, 0) < 0)
            return -1;

        if (p.revents) {
            int error = 0;
            socklen_t size =
                sizeof(error);

            return
                getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &error,
                    &size) == 0 &&
                error == 0
                    ? 0
                    : -1;
        }
    }

    return -1;
}

int
probe_concurrent(
    int (*pump)(void),
    int native_fd)
{
    int made = 0;
    int result = -1;

    memset(
        jobs,
        0,
        sizeof(jobs));

    atomic_store(
        &cancel_jobs,
        0);

    atomic_store(
        &start_jobs,
        0);

    for (int i = 0;
         i < WORKERS;
         ++i) {

        jobs[i].fd = -1;

        jobs[i].type =
            i < TCP_WORKERS
                ? SOCK_STREAM
                : SOCK_DGRAM;
    }

    probe_say(
        "AXSTRESS 8 workers: 4 TCP + 4 UDP, %d rounds each",
        ROUNDS);

    /*
     * Eight AX sockets remain well below the characterized native
     * title-visible limit of 28 descriptors.
     */
    for (int i = 0;
         i < WORKERS;
         ++i) {

        jobs[i].fd =
            socket(
                AF_INET,
                jobs[i].type,
                0);

        if (jobs[i].fd < 0 ||
            connect_fd(
                jobs[i].fd,
                pump))
            goto done;
    }

    /*
     * Keep one genuinely native socket in the mix as a regression check.
     */
    if (native_fd < 0 ||
        connect_fd(
            native_fd,
            pump))
        goto done;

    {
        struct sockaddr_in local;
        socklen_t local_len =
            sizeof(local);

        char ip[32] = "?";

        memset(
            &local,
            0,
            sizeof(local));

        if (getsockname(
                native_fd,
                (struct sockaddr *)&local,
                &local_len) == 0) {

            inet_ntop(
                AF_INET,
                &local.sin_addr,
                ip,
                sizeof(ip));
        }

        probe_say(
            "AXSTRESS native coexist socket=%s:%u",
            ip,
            ntohs(local.sin_port));
    }

    const char marker[] =
        "AX-native-stress-coexistence";

    if (send(
            native_fd,
            marker,
            sizeof(marker),
            0) !=
        (int)sizeof(marker))
        goto done;

    int native_ok = 0;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(4000);

    while (pump() &&
           OSGetTime() < deadline) {

        struct pollfd p = {
            .fd = native_fd,
            .events = POLLIN
        };

        if (poll(
                &p,
                1,
                0) < 0)
            goto done;

        if (p.revents & POLLIN) {
            char reply[
                sizeof(marker)];

            native_ok =
                recv(
                    native_fd,
                    reply,
                    sizeof(reply),
                    0) ==
                    sizeof(reply) &&
                !memcmp(
                    marker,
                    reply,
                    sizeof(marker));

            break;
        }
    }

    probe_say(
        "AXSTRESS native coexistence: %s",
        native_ok
            ? "PASS"
            : "FAIL");

    if (!native_ok)
        goto done;

    for (int i = 0;
         i < WORKERS;
         ++i) {

        if (!OSCreateThread(
                &threads[i],
                worker,
                i,
                NULL,
                stacks[i] +
                    sizeof(stacks[i]),
                sizeof(stacks[i]),
                16,
                OS_THREAD_ATTRIB_AFFINITY_CPU1))
            goto done;

        ++made;

        OSSetThreadName(
            &threads[i],
            jobs[i].type ==
                    SOCK_STREAM
                ? "AX TCP stress"
                : "AX UDP stress");

        OSResumeThread(
            &threads[i]);
    }

    atomic_store(
        &start_jobs,
        1);

    probe_say(
        "AXSTRESS started");

    deadline =
        OSGetTime() +
        OSMillisecondsToTicks(
            TEST_TIMEOUT_MS);

    OSTime next_progress =
        OSGetTime() +
        OSMillisecondsToTicks(5000);

    for (;;) {
        int all_done = 1;

        for (int i = 0;
             i < WORKERS;
             ++i) {

            if (!atomic_load(
                    &jobs[i].done)) {
                all_done = 0;
                break;
            }
        }

        if (all_done)
            break;

        if (!pump() ||
            OSGetTime() >= deadline)
            goto done;

        if (OSGetTime() >=
            next_progress) {

            int min_round =
                ROUNDS;

            int total_rounds =
                0;

            for (int i = 0;
                 i < WORKERS;
                 ++i) {

                int r =
                    atomic_load(
                        &jobs[i].rounds);

                total_rounds += r;

                if (r < min_round)
                    min_round = r;
            }

            probe_say(
                "AXSTRESS progress min=%d/%d total=%d/%d",
                min_round,
                ROUNDS,
                total_rounds,
                WORKERS * ROUNDS);

            next_progress =
                OSGetTime() +
                OSMillisecondsToTicks(
                    5000);
        }

        OSSleepTicks(
            OSMillisecondsToTicks(2));
    }

    result = 0;

    for (int i = 0;
         i < WORKERS;
         ++i) {

        if (atomic_load(
                &jobs[i].rounds) !=
                ROUNDS ||
            jobs[i].error != 0) {
            result = -1;
            break;
        }
    }

done:
    atomic_store(
        &cancel_jobs,
        1);

    for (int i = 0;
         i < made;
         ++i)
        OSJoinThread(
            &threads[i],
            NULL);

    unsigned total_bytes = 0;

    for (int i = 0;
         i < WORKERS;
         ++i) {

        int rounds =
            atomic_load(
                &jobs[i].rounds);

        total_bytes +=
            jobs[i].bytes;

        WHBLogPrintf(
            "AXSTRESS worker=%d type=%s rounds=%d/%d bytes=%u errno=%d",
            i,
            jobs[i].type ==
                    SOCK_STREAM
                ? "TCP"
                : "UDP",
            rounds,
            ROUNDS,
            jobs[i].bytes,
            jobs[i].error);

        if (jobs[i].fd >= 0 &&
            close(jobs[i].fd) < 0)
            result = -1;
    }

    probe_say(
        "AXSTRESS result=%s total_bytes=%u",
        result == 0
            ? "PASS"
            : "FAIL",
        total_bytes);

    return result;
}
