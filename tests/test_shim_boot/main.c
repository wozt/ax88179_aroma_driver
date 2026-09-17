/*
 * Simple test to validate shim boot behavior.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "probe.h"
#include <whb/log.h>
#include <whb/log_udp.h>

#define SERVER_IP "192.168.2.100"
#define SERVER_PORT 18879
#define STATUS_INTERVAL 3

static uint32_t last_status_time = 0;
static int test_tcp_running = 1;
static int test_udp_running = 1;
static int running = 1;
static const unsigned test_sizes[] = {32, 504, 1024, 1400};

static int pump(void)
{
    if (running) running = probe_poll();
    return running;
}

static int wait_socket(int fd, short events)
{
    OSTime deadline = OSGetTime() + OSMillisecondsToTicks(4000);
    while (pump() && OSGetTime() < deadline) {
        struct pollfd p = {.fd = fd, .events = events};
        int rc = poll(&p, 1, 0);
        if (rc < 0 || (p.revents & (POLLERR | POLLNVAL))) return -1;
        if (rc > 0) return 0;
        OSSleepTicks(OSMillisecondsToTicks(5));
    }
    return -1;
}

static void print_status(const char *ip, int rx, int tx, int errors)
{
    char status[256];
    uint32_t now = (uint32_t)OSTicksToMilliseconds(OSGetTime());
    
    if ((uint32_t)(now - last_status_time) >= STATUS_INTERVAL * 1000) {
        last_status_time = now;
        snprintf(status, sizeof(status),
                 "AX88179 Test\n"
                 "============\n"
                 "IP: %s\n"
                 "TCP: %s  UDP: %s\n"
                 "RX: %d  TX: %d  ERR: %d\n"
                 "\n"
                 "HOME menu -> Quitter\n"
                 "Tests keep running until system exit",
                 ip ? ip : "NONE",
                 test_tcp_running ? "TESTING" : "STOPPED",
                 test_udp_running ? "TESTING" : "STOPPED",
                 rx, tx, errors);
        probe_say(status);
    }

}

/*
 * Ready = a socket created NOW is routed to the AX88179 stack and that
 * stack has a DHCP lease. No PC round-trip: the shim marks native
 * fallback sockets with errno=-1 and serves SO_MYADDR from the lwIP
 * netif (0.0.0.0 until bound). Anything else (real nsysnet socket, or
 * lwIP before DHCP) means "not yet".
 */
static int wait_ax_ping_ready(void)
{
    OSTime deadline = OSGetTime() + OSMillisecondsToTicks(120000);
    int tries = 0;
    while (pump() && OSGetTime() < deadline) {
        errno = 0;
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            uint32_t addr = 0;
            socklen_t len = sizeof(addr);
            int rc = getsockopt(fd, SOL_SOCKET, SO_MYADDR, &addr, &len);
            close(fd);
            struct in_addr expected_ax;
            inet_pton(AF_INET, "192.168.2.190", &expected_ax);

            if (rc == 0 && addr == expected_ax.s_addr) {
                struct in_addr a = { .s_addr = addr };
                char buf[16];
                inet_ntop(AF_INET, &a, buf, sizeof(buf));
                probe_say("AX socket path ready after %d tries (IP %s)", tries + 1, buf);
                return 0;
            }
        }
        if ((tries++ % 10) == 0) {
            probe_say("Waiting for AX socket path... try %d", tries);
        }
        OSSleepTicks(OSMillisecondsToTicks(500));
    }
    probe_say("AX socket path wait timed out");
    return -1;
}

static const char *current_ax_ip(char *buf, size_t n)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "NONE";
    uint32_t addr = 0;
    socklen_t len = sizeof(addr);
    int rc = getsockopt(fd, SOL_SOCKET, SO_MYADDR, &addr, &len);
    close(fd);
    if (rc != 0 || addr == 0) return "NONE";
    struct in_addr a = { .s_addr = addr };
    return inet_ntop(AF_INET, &a, buf, n) ? buf : "NONE";
}

/* Test TCP echo */
static int test_tcp_echo(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        probe_say("TCP socket() failed: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in server = {
        .sin_family = AF_INET, .sin_port = htons(SERVER_PORT)
    };
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);
    
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        probe_say("TCP nonblock failed: %s", strerror(errno));
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
            probe_say("TCP connect() failed: %s", strerror(errno));
            close(fd); return -1;
        }
    }
    if (wait_socket(fd, POLLOUT) < 0) { probe_say("TCP connect timeout"); close(fd); return -1; }

    int error = 0;
    socklen_t elen = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &elen) < 0 || error) {
        probe_say("TCP SO_ERROR=%d", error);
        close(fd); return -1;
    }

    char tx[1400], rx[1400];
    for (unsigned i = 0; i < sizeof(test_sizes)/sizeof(test_sizes[0]) && pump(); i++) {
        unsigned size = test_sizes[i];
        for (unsigned j = 0; j < size; j++) tx[j] = (char)(j * 31 + i);
        unsigned sent = 0, received = 0;
        while (sent < size) {
            if (wait_socket(fd, POLLOUT) < 0) { close(fd); return -1; }
            int n = send(fd, tx + sent, size - sent, 0);
            if (n <= 0) { close(fd); return -1; }
            sent += (unsigned)n;
        }
        while (received < size) {
            if (wait_socket(fd, POLLIN) < 0) { close(fd); return -1; }
            int n = recv(fd, rx + received, size - received, 0);
            if (n <= 0) { close(fd); return -1; }
            received += (unsigned)n;
        }
        if (memcmp(tx, rx, size) != 0) { probe_say("TCP echo FAIL %u", size); close(fd); return -1; }
        probe_say("TCP echo PASS %u", size);
    }
    close(fd); return running ? 0 : -1;
}

/* Test UDP echo */
static int test_udp_echo(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { probe_say("UDP socket() failed"); close(fd); return -1; }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        probe_say("UDP nonblock failed: %s", strerror(errno));
        close(fd); return -1;
    }
    
    struct sockaddr_in server = { .sin_family = AF_INET, .sin_port = htons(SERVER_PORT) };
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);
    
    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        probe_say("UDP connect() failed: %s", strerror(errno));
        close(fd); return -1;
    }

    for (unsigned i = 0; i < sizeof(test_sizes)/sizeof(test_sizes[0]) && pump(); i++) {
        unsigned size = test_sizes[i];
        char tx[1400], rx[1400];
        for (unsigned j = 0; j < size; j++) tx[j] = (char)(j * 31 + i);

        if (wait_socket(fd, POLLOUT) < 0 ||
            send(fd, tx, size, 0) != (int)size) {
            probe_say("UDP send(%u) failed", size); close(fd); return -1;
        }

        if (wait_socket(fd, POLLIN) < 0) { probe_say("UDP recv timeout %u", size); close(fd); return -1; }
        int n = recv(fd, rx, size, 0);
        if (n != (int)size) {
            probe_say("UDP recv(%u) failed", size); close(fd); return -1;
        }
        if (memcmp(tx, rx, size) != 0) {
            probe_say("UDP echo FAIL %u", size); close(fd); return -1;
        }
        probe_say("UDP echo PASS %u", size);
    }
    
    close(fd); return running ? 0 : -1;
}

int main(void)
{
    int tcp_errors = 0, udp_errors = 0, rx_packets = 0, tx_packets = 0;
    
    if (probe_init("AX88179 Shim Boot Test") != 0) return 1;
    if (WHBLogUdpInit() != 0) probe_say("Failed to init UDP log");
    
    probe_say("AX88179 Shim Boot Test");
    probe_say("Testing network socket intercept...");
    probe_say("Server: %s:%d", SERVER_IP, SERVER_PORT);
    probe_say("Use HOME menu -> Quitter to exit");
    probe_say("Waiting for AX socket path after app transition...");
    while (pump() && wait_ax_ping_ready() != 0) {
        tcp_errors++;
        udp_errors++;
        break;
    }
    
    last_status_time = (uint32_t)OSTicksToMilliseconds(OSGetTime());
    
    while (1) {
        if (!pump()) {
            probe_say("System requested exit");
            break;
        }
        
        char ipbuf[16];
        const char *ip = current_ax_ip(ipbuf, sizeof(ipbuf));
        print_status(ip, rx_packets, tx_packets, tcp_errors + udp_errors);
        
        if (test_tcp_running && test_tcp_echo() != 0) {
            tcp_errors++; test_tcp_running = 0;
        }
        if (test_udp_running && test_udp_echo() != 0) {
            udp_errors++; test_udp_running = 0;
        }
        
        rx_packets++; tx_packets++;
        for (int i = 0; i < 50 && pump(); i++)
            OSSleepTicks(OSMillisecondsToTicks(10));
    }
    
    probe_say("Test complete");
    probe_say("TCP errors: %d  UDP errors: %d", tcp_errors, udp_errors);
    probe_say("Total RX: %d  TX: %d", rx_packets, tx_packets);
    
    probe_shutdown();
    return 0;
}
