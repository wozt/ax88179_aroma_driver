/* Ordinary WUT sockets only: the RPX neither links lwIP nor accesses UHS. */
#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/dynload.h>
#include <coreinit/time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include "probe.h"
#include "concurrency.h"

static int running = 1;
static int pump(void) { if (running) running = probe_poll(); return running; }

static int wait_socket(int fd, short events) {
    OSTime deadline = OSGetTime() + OSMillisecondsToTicks(4000);
    while (pump() && OSGetTime() < deadline) {
        struct pollfd p = {.fd=fd, .events=events};
        int rc = poll(&p, 1, 0);
        if (rc < 0 || (p.revents & (POLLERR | POLLNVAL))) return -1;
        if (rc > 0) return 0;
    }
    return -1;
}
static int test(int type) {
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    int ok = -1;
    struct sockaddr_in server = {.sin_family=AF_INET, .sin_port=htons(18879)};
    inet_pton(AF_INET, "192.168.2.100", &server.sin_addr);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto done;
    int rc = connect(fd, (struct sockaddr *)&server, sizeof(server));
    if (rc < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
        probe_say("AXPROBE connect type=%d errno=%d", type, errno); goto done;
    }
    if (wait_socket(fd, POLLOUT) < 0) goto done;
    int error = 0; socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
        probe_say("AXPROBE SO_ERROR=%d", error); goto done;
    }
    struct sockaddr_in local; len = sizeof(local);
    if (!getsockname(fd, (struct sockaddr *)&local, &len)) {
        char ip[16]; inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        probe_say("AXPROBE %s local=%s:%u", type == SOCK_STREAM ? "TCP" : "UDP", ip, ntohs(local.sin_port));
    }
    static const unsigned sizes[] = {32, 504, 1024, 1400};
    char tx[1400], rx[1400];
    for (unsigned n = 0; n < sizeof(sizes)/sizeof(sizes[0]); n++) {
        unsigned size=sizes[n];
        for (unsigned i=0; i<size; i++) tx[i]=(char)(i * 31 + n);
        unsigned sent=0, received=0;
        while (sent < size) {
            if (wait_socket(fd, POLLOUT) < 0) goto done;
            rc=send(fd, tx+sent, size-sent, 0);
            if (rc <= 0 || (type == SOCK_DGRAM && (unsigned)rc != size)) goto done;
            sent += rc;
        }
        while (received < size) {
            if (wait_socket(fd, POLLIN) < 0) goto done;
            rc=recv(fd, rx+received, size-received, 0);
            if (rc <= 0 || (type == SOCK_DGRAM && (unsigned)rc != size)) goto done;
            received += rc;
        }
        if (memcmp(tx, rx, size)) goto done;
        probe_say("AXPROBE %s echo %u PASS", type == SOCK_STREAM ? "TCP" : "UDP", size);
    }
    ok=0;
done:
    close(fd); return ok;
}
int main(void) {
    if (probe_init("AX88179 socket shim: TCP / UDP") != 0) {
        probe_shutdown(); return 1;
    }
    probe_say("AXPROBE waiting 25s for DHCP; HOME / MINUS cancels");
    OSTime ready = OSGetTime() + OSMillisecondsToTicks(25000);
    while (pump() && OSGetTime() < ready) {}
    /* Deliberately native: allocated before AXShimBeginProbe. */
    int native_fd = running ? socket(AF_INET, SOCK_DGRAM, 0) : -1;
    OSDynLoad_Module module = 0;
    int (*begin_probe)(void) = NULL, (*end_probe)(void) = NULL;
    if (running && (OSDynLoad_Acquire("homebrew_ax88179", &module) != OS_DYNLOAD_OK ||
        OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "AXShimBeginProbe", (void **)&begin_probe) != OS_DYNLOAD_OK ||
        OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "AXShimEndProbe", (void **)&end_probe) != OS_DYNLOAD_OK)) {
        probe_say("AXPROBE module API unavailable; no Ethernet test performed");
    } else if (running) {
        int activated = begin_probe();
        probe_say("AXPROBE activation=%d (0=ready)", activated);
        if (activated != 0) goto finished;
        int tcp=test(SOCK_STREAM);
        int udp=running ? test(SOCK_DGRAM) : -1;
        if (running) probe_say("AXPROBE finished TCP=%s UDP=%s", tcp ? "FAIL" : "PASS", udp ? "FAIL" : "PASS");
        int concurrent = running ? probe_concurrent(pump, native_fd) : -1;
        if (running) probe_say("AXCONCURRENT result=%s", concurrent ? "FAIL" : "PASS");
        int ended = end_probe();
        if (running) probe_say("AXPROBE release=%d (0=no owned sockets remain)", ended);
        else WHBLogPrintf("AXPROBE release after HOME=%d", ended);
    }
finished:
    if (native_fd >= 0) close(native_fd);
    if (module) OSDynLoad_Release(module);
    if (running) probe_wait();
    probe_shutdown(); return 0;
}
