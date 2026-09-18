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

#include "probe.h"

#define SERVER_PORT 19010
#define BULK_SIZE   (128 * 1024)

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

static int would_block(void)
{
    return errno == EWOULDBLOCK || errno == EAGAIN;
}

static int set_nonblocking(int fd)
{
    int one = 1;

    return setsockopt(fd,
                      SOL_SOCKET,
                      SO_NONBLOCK,
                      &one,
                      sizeof(one));
}

static void show_socket_info(int fd, const char *name)
{
    struct sockaddr_in local;
    struct sockaddr_in peer;

    socklen_t llen = sizeof(local);
    socklen_t plen = sizeof(peer);

    memset(&local, 0, sizeof(local));
    memset(&peer, 0, sizeof(peer));

    char lip[32] = "?";
    char pip[32] = "?";

    int lrc = getsockname(fd,
                          (struct sockaddr *)&local,
                          &llen);

    int prc = getpeername(fd,
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

    int type = -1;
    socklen_t tlen = sizeof(type);

    errno = 0;

    int trc = getsockopt(fd,
                         SOL_SOCKET,
                         SO_TYPE,
                         &type,
                         &tlen);

    probe_say("%s local=%s:%u peer=%s:%u",
              name,
              lip,
              lrc == 0 ? ntohs(local.sin_port) : 0,
              pip,
              prc == 0 ? ntohs(peer.sin_port) : 0);

    probe_say("%s SO_TYPE rc=%d errno=%d type=%d",
              name,
              trc,
              errno,
              type);
}

static int wait_accept(int listener,
                       struct sockaddr_in *peer,
                       unsigned timeout_ms)
{
    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(timeout_ms);

    while (pump() && OSGetTime() < deadline) {
        socklen_t len = sizeof(*peer);

        memset(peer, 0, sizeof(*peer));

        errno = 0;

        int fd = accept(listener,
                        (struct sockaddr *)peer,
                        &len);

        if (fd >= 0)
            return fd;

        if (!would_block()) {
            probe_say("accept FAIL errno=%d", errno);
            return -2;
        }

        OSSleepTicks(OSMillisecondsToTicks(5));
    }

    return -1;
}

static int send_all(int fd,
                    const void *data,
                    size_t size,
                    unsigned timeout_ms)
{
    const uint8_t *p = data;
    size_t sent = 0;

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(timeout_ms);

    while (sent < size &&
           pump() &&
           OSGetTime() < deadline) {

        errno = 0;

        int n = send(fd,
                     p + sent,
                     size - sent,
                     0);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n < 0 && would_block()) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
        }

        probe_say("send FAIL rc=%d errno=%d", n, errno);
        return -1;
    }

    return sent == size ? 0 : -1;
}

static int recv_line(int fd,
                     char *out,
                     size_t cap,
                     unsigned timeout_ms)
{
    size_t used = 0;

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(timeout_ms);

    while (used + 1 < cap &&
           pump() &&
           OSGetTime() < deadline) {

        errno = 0;

        int n = recv(fd,
                     out + used,
                     cap - used - 1,
                     0);

        if (n > 0) {
            used += (size_t)n;
            out[used] = 0;

            if (strchr(out, '\n'))
                return (int)used;

            continue;
        }

        if (n == 0)
            break;

        if (would_block()) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
        }

        probe_say("recv line FAIL errno=%d", errno);
        return -1;
    }

    out[used] = 0;
    return used ? (int)used : -1;
}

static uint32_t fnv1a_update(uint32_t hash,
                             const uint8_t *data,
                             size_t size)
{
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }

    return hash;
}

static uint32_t expected_bulk_hash(void)
{
    uint32_t hash = 2166136261u;

    for (unsigned i = 0; i < BULK_SIZE; i++) {
        uint8_t b =
            (uint8_t)((i * 37u + 11u) & 0xffu);

        hash = fnv1a_update(hash, &b, 1);
    }

    return hash;
}

static int test_short_client(int fd)
{
    char line[64];

    if (set_nonblocking(fd) != 0) {
        probe_say("child NONBLOCK FAIL errno=%d", errno);
        return -1;
    }

    show_socket_info(fd, "accepted");

    int n = recv_line(fd,
                      line,
                      sizeof(line),
                      5000);

    if (n < 0) {
        probe_say("short client RX FAIL");
        return -1;
    }

    probe_say("short RX '%s'", line);

    const char *reply = NULL;

    if (!strcmp(line, "CLIENT1\n"))
        reply = "ACK CLIENT1\n";
    else if (!strcmp(line, "CLIENT2\n"))
        reply = "ACK CLIENT2\n";
    else {
        probe_say("unexpected short client payload");
        return -1;
    }

    if (send_all(fd,
                 reply,
                 strlen(reply),
                 5000) != 0) {
        probe_say("short client TX FAIL");
        return -1;
    }

    return 0;
}

static int test_bulk_client(int fd)
{
    if (set_nonblocking(fd) != 0) {
        probe_say("bulk NONBLOCK FAIL errno=%d", errno);
        return -1;
    }

    show_socket_info(fd, "bulk");

    uint8_t buf[4096];

    size_t total = 0;
    uint32_t hash = 2166136261u;

    OSTime deadline =
        OSGetTime() + OSMillisecondsToTicks(30000);

    int saw_eof = 0;

    while (pump() &&
           OSGetTime() < deadline) {

        errno = 0;

        int n = recv(fd,
                     buf,
                     sizeof(buf),
                     0);

        if (n > 0) {
            total += (size_t)n;
            hash = fnv1a_update(hash,
                                buf,
                                (size_t)n);

            if (total > BULK_SIZE) {
                probe_say("bulk FAIL: too much data %u",
                          (unsigned)total);
                return -1;
            }

            continue;
        }

        if (n == 0) {
            saw_eof = 1;
            break;
        }

        if (would_block()) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
        }

        probe_say("bulk recv FAIL errno=%d", errno);
        return -1;
    }

    uint32_t expected = expected_bulk_hash();

    probe_say("bulk bytes=%u hash=%08x expected=%08x eof=%d",
              (unsigned)total,
              (unsigned)hash,
              (unsigned)expected,
              saw_eof);

    if (!saw_eof ||
        total != BULK_SIZE ||
        hash != expected) {
        probe_say("BULK DATA: FAIL");
        return -1;
    }

    /*
     * The peer has performed shutdown(SHUT_WR), so recv() returned EOF.
     * TCP is still half-open in our direction: sending must continue
     * to work.
     */
    char reply[96];

    snprintf(reply,
             sizeof(reply),
             "BULK-OK %u %08x\n",
             (unsigned)total,
             (unsigned)hash);

    if (send_all(fd,
                 reply,
                 strlen(reply),
                 5000) != 0) {
        probe_say("send after peer SHUT_WR FAIL");
        return -1;
    }

    errno = 0;

    int rc = shutdown(fd, SHUT_WR);

    probe_say("server shutdown(SHUT_WR) rc=%d errno=%d",
              rc,
              errno);

    if (rc != 0)
        return -1;

    probe_say("BULK DATA + HALF-CLOSE: PASS");
    return 0;
}

int main(void)
{
    if (probe_init("AX TCP Server Probe") != 0)
        return 1;

    probe_say("TCP server/listen/accept integration test");

    wait_for_module();

    if (!running)
        goto done;

    int listener = socket(AF_INET,
                          SOCK_STREAM,
                          0);

    if (listener < 0) {
        probe_say("socket FAIL errno=%d", errno);
        goto wait;
    }

    uint32_t myaddr = 0;
    socklen_t mylen = sizeof(myaddr);

    if (getsockopt(listener,
                   SOL_SOCKET,
                   SO_MYADDR,
                   &myaddr,
                   &mylen) != 0) {
        probe_say("SO_MYADDR FAIL errno=%d", errno);
        close(listener);
        goto wait;
    }

    char local_ip[32] = "?";
    struct in_addr local = { .s_addr = myaddr };

    inet_ntop(AF_INET,
              &local,
              local_ip,
              sizeof(local_ip));

    probe_say("AX interface: %s", local_ip);

    int one = 1;

    if (setsockopt(listener,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &one,
                   sizeof(one)) != 0) {
        probe_say("SO_REUSEADDR FAIL errno=%d", errno);
        close(listener);
        goto wait;
    }

    if (set_nonblocking(listener) != 0) {
        probe_say("listener NONBLOCK FAIL errno=%d", errno);
        close(listener);
        goto wait;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    errno = 0;

    if (bind(listener,
             (struct sockaddr *)&addr,
             sizeof(addr)) != 0) {
        probe_say("bind :%d FAIL errno=%d",
                  SERVER_PORT,
                  errno);
        close(listener);
        goto wait;
    }

    errno = 0;

    if (listen(listener, 2) != 0) {
        probe_say("listen FAIL errno=%d", errno);
        close(listener);
        goto wait;
    }

    int type = -1;
    socklen_t typelen = sizeof(type);

    errno = 0;

    int trc = getsockopt(listener,
                         SOL_SOCKET,
                         SO_TYPE,
                         &type,
                         &typelen);

    probe_say("listener SO_TYPE rc=%d errno=%d type=%d",
              trc,
              errno,
              type);

    probe_say("%s", "");
    probe_say("READY %s:%d", local_ip, SERVER_PORT);
    probe_say("Run PC TCP client now");

    /*
     * First phase: the PC opens two TCP connections before either one
     * is serviced. This exercises the listen backlog and two accepted
     * sockets at the same time.
     */
    int children[2] = { -1, -1 };

    for (int i = 0; i < 2; i++) {
        struct sockaddr_in peer;

        children[i] =
            wait_accept(listener,
                        &peer,
                        30000);

        if (children[i] < 0) {
            probe_say("FAIL accepting short client %d",
                      i + 1);
            goto cleanup;
        }

        probe_say("accepted short client %d fd=%d",
                  i + 1,
                  children[i]);
    }

    int short_ok = 1;

    for (int i = 0; i < 2; i++) {
        if (test_short_client(children[i]) != 0)
            short_ok = 0;

        close(children[i]);
        children[i] = -1;
    }

    if (!short_ok) {
        probe_say("BACKLOG/SHORT CONNECTIONS: FAIL");
        goto cleanup;
    }

    probe_say("BACKLOG/SHORT CONNECTIONS: PASS");

    /*
     * Second phase: 128 KiB transfer followed by client SHUT_WR.
     */
    probe_say("waiting bulk connection...");

    struct sockaddr_in bulk_peer;

    int bulk =
        wait_accept(listener,
                    &bulk_peer,
                    30000);

    if (bulk < 0) {
        probe_say("FAIL accepting bulk client");
        goto cleanup;
    }

    int bulk_ok = test_bulk_client(bulk);

    close(bulk);

    if (bulk_ok != 0) {
        probe_say("TCP SERVER RESULT: FAIL");
        goto cleanup;
    }

    probe_say("TCP SERVER RESULT: PASS");
    probe_say("listen/backlog/accept/RX/TX/half-close OK");

cleanup:
    for (int i = 0; i < 2; i++) {
        if (children[i] >= 0)
            close(children[i]);
    }

    close(listener);

wait:
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    probe_shutdown();
    return 0;
}
