#include <wums.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/debug.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include "../net/ax_net.h"
#include "iosu_patch.h"
#include "nsysnet_shim.h"
#include "debug_progress.h"

#define AX_LOG(fmt, ...) \
    WHBLogPrintf("[%llu] AX: " fmt, \
                 (unsigned long long)OSTicksToMilliseconds(OSGetTime()), ##__VA_ARGS__)

#ifndef AX_DISABLE_SHIM
#define AX_DISABLE_SHIM 0
#endif

WUMS_MODULE_EXPORT_NAME("homebrew_ax88179");
WUMS_MODULE_AUTHOR("wozt");
WUMS_MODULE_VERSION("0.2.8-pretendo-coexist");
WUMS_MODULE_DESCRIPTION("AX88179 usermode Ethernet, DHCP, and nsysnet shim at boot");

/* Initialise the WUT devoptab so stdio (fopen/fgets/...) can access
 * devices exposed by WUMS, including fs:/vol/external01. */
WUMS_USE_WUT_DEVOPTAB();


static OSThread worker __attribute__((aligned(0x40)));
static uint8_t stack[64 * 1024] __attribute__((aligned(0x40)));
static OSThread watchdog __attribute__((aligned(0x40)));
static int watchdog_started;
static uint8_t wd_stack[16 * 1024] __attribute__((aligned(0x40)));

static int config_keep_first = 1;
static int config_shim_trace = 0;
static int config_system_dns = 0;

static void load_config(void)
{
    static const char *paths[] = {
        "fs:/vol/external01/wiiu/ax88179/config.ini",
        "/fs/vol/external01/wiiu/ax88179/config.ini",
        "fs:/wiiu/ax88179/config.ini"
    };

    FILE *f = NULL;

    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        f = fopen(paths[i], "r");
        if (f) {
            AX_LOG("CONFIG: opened %s", paths[i]);
            break;
        }
    }

    if (!f) {
        AX_LOG("CONFIG: fopen failed on all paths");
        return;
    }

    char b[128];

    while (fgets(b, sizeof(b), f)) {
        if (strstr(b, "mode=always"))
            config_keep_first = 0;
        else if (strstr(b, "mode=keep_first"))
            config_keep_first = 1;

        if (strstr(b, "dns=system"))
            config_system_dns = 1;
        else if (strstr(b, "dns=ax"))
            config_system_dns = 0;

        int level;
        if (sscanf(b, "shim_trace=%d", &level) == 1) {
            if (level < 0) level = 0;
            if (level > 2) level = 2;
            config_shim_trace = level;
        }
    }

    fclose(f);

    AX_LOG("CONFIG: DHCP mode = %s",
           config_keep_first ? "keep_first" : "always");
    AX_LOG("CONFIG: shim trace = %d", config_shim_trace);
    AX_LOG("CONFIG: DNS = %s", config_system_dns ? "system" : "ax");
}

static const char *const mark_names[] = {
    "worker started", "udp log inited", "iosu patch", "adapter opened",
    "tx queue inited", "tcpip_init returned", "tcpip thread created",
    "tcpip thread entered", "setup_cb entered", "netif added",
    "dhcp started", "ax_net_start done", "poll loop running",
    "link up", "dhcp bound", "shim installed",
};

/* If an adapter was opened but DHCP has not bound 45 s after the worker
 * starts, show on the fatal screen exactly how far the bring-up got.
 * The UDP log cannot be relied on for this: until DHCP succeeds it can
 * only leave over the console's own Wi-Fi, which may be down. */
static int run_watchdog(int argc, const char **argv)
{
    (void)argc; (void)argv;
    OSSleepTicks(OSMillisecondsToTicks(45 * 1000));
    unsigned m = ax_progress_snapshot();
    if (m & (1u << AX_MARK_DHCP_BOUND)) return 0;
    /* No adapter found is not a fault: the console was booted without
     * the dongle, or with it unplugged. Say nothing and let the console
     * carry on with its own network. */
    if (!(m & (1u << AX_MARK_ADAPTER_OPEN))) {
        AX_LOG("no adapter after 45 s, staying out of the way");
        return 0;
    }
    int last = -1;
    for (unsigned i = 0; i < sizeof(mark_names) / sizeof(mark_names[0]); i++)
        if (m & (1u << i)) last = (int)i;
    /* A log line, not OSFatal: the UDP log reaches the PC again, and a
     * fatal screen costs a power cycle for information we can now read
     * without one. */
    AX_LOG("STUCK mask=0x%04x last step: %s",
                 m, last >= 0 ? mark_names[last] : "none (worker never ran)");
    char st[160];
    ax_net_status(st, sizeof(st));
    AX_LOG("STUCK %s", st);
    return 0;
}
static atomic_bool stopping;
static int started;
static atomic_uint worker_generation;

/* The worker exclusively owns UHS and lwIP. App transition hooks only signal
 * stop, then join before per-title resources/newlib are finalized. */
static int run_network(int argc, const char **argv)
{
    (void)argc; (void)argv;
    ax_mark(AX_MARK_WORKER_STARTED);
    WHBLogUdpInit();
    ax_mark(AX_MARK_UDP_LOG);
    /* RPXLoader/wiiload starts a short-lived title to receive the payload.
     * Touching UHS during that transfer can strand the worker in teardown.
     * Let short-lived loader titles exit before opening the adapter. */
    /*
     * The first worker starts while Aroma/RPXLoader is still going through
     * its initial title transitions. Touching UHS too early during that
     * first boot can prevent the AX88179 bring-up entirely.
     *
     * Later application transitions only need a short guard before
     * reacquiring UHS.
     */
    unsigned generation =
        atomic_fetch_add_explicit(&worker_generation, 1,
                                  memory_order_relaxed);

    const unsigned startup_delay_ms = (generation == 0) ? 25000 : 2000;

    AX_LOG("start guard=%ums gen=%u %s",
           startup_delay_ms, generation,
           AX_DISABLE_SHIM ? "shim=off" : "shim=on");

    for (unsigned waited = 0; waited < startup_delay_ms; waited += 100) {
        if (atomic_load_explicit(&stopping, memory_order_acquire))
            goto cleanup;

        OSSleepTicks(OSMillisecondsToTicks(100));
    }

    /* A fresh process: nothing lwIP left behind is still valid. */
    nsysnet_shim_set_trace_level(config_shim_trace);
    nsysnet_shim_set_system_dns(config_system_dns);
    ax_net_set_session_lease_mode(config_keep_first);
    AX_LOG("config dhcp=%s trace=%d dns=%s",
           config_keep_first ? "keep_first" : "always",
           config_shim_trace,
           config_system_dns ? "system" : "ax");

    ax_net_forget();

    /* Hooks are activated automatically at boot when SHIM=1, not by an UPID test.
     * The shim installs on the first title that starts, and covers all GAME processes.
     * UPID 15 also hosts the initial environment loader, so we don't use UPID filtering. */
    /*
     * The IOSU patch, applied here and not by a separate app.
     *
     * Without it the endpoint enable is refused and nothing moves. The
     * write is volatile, so this reapplies it every boot before the
     * adapter is touched -- which is the whole difference between a
     * module that runs in the background and an app someone has to
     * remember to launch. Applying it is idempotent and it refuses an
     * unexpected instruction, so a re-run or a firmware it does not
     * recognise cannot make things worse.
     */
    {
        char pwhy[160];
        ax_mark(AX_MARK_IOSU_PATCH);
        if (iosu_patch_apply(pwhy, sizeof(pwhy)) != 0) {
            AX_LOG("IOSU patch NOT applied (%s) -- endpoints will stay locked", pwhy);
        } else {
            AX_LOG("IOSU patch %s", pwhy);
        }
    }

    /* Open adapter */
    Ax88179 *ax = NULL;
    char why[160];
    OSTime t_open = OSGetTime();
    ax = ax88179_open(why, sizeof(why));
    AX_LOG("open %llums",
           (unsigned long long)OSTicksToMilliseconds(OSGetTime() - t_open));
    if (!ax) {
        AX_LOG("%s", why);
        goto cleanup;
    }
    ax_mark(AX_MARK_ADAPTER_OPEN);

    /* Initialize lwIP */
    OSTime t_net = OSGetTime();
    if (ax_net_start(ax) != 0) {
        AX_LOG("network initialization failed");
        ax88179_close(ax);
        goto cleanup;
    }
    AX_LOG("net %llums",
           (unsigned long long)OSTicksToMilliseconds(OSGetTime() - t_net));
    ax_mark(AX_MARK_NET_STARTED);

    /* Wait for DHCP or timeout (30s) */
    ax_mark(AX_MARK_DHCP_STARTED);
    OSTime dhcp_deadline = OSGetTime() + OSMillisecondsToTicks(30000);
    int dhcp_done = 0;
    while (!dhcp_done && OSGetTime() < dhcp_deadline) {
        int n = ax_net_poll();
        if (n < 0) {
            if (OSGetTime() >= dhcp_deadline) break;
            OSSleepTicks(OSMillisecondsToTicks(100));
            continue;
        }
        const char *ip = ax_net_address();
        if (ip && ip[0]) {
            dhcp_done = 1;
            ax_mark(AX_MARK_DHCP_BOUND);
            if (ax_net_using_cached_lease())
                AX_LOG("lease cached %s", ip);
            else
                AX_LOG("lease DHCP %s", ip);
        }
        if (OSGetTime() >= dhcp_deadline) break;
        OSSleepTicks(OSMillisecondsToTicks(100));
    }

    if (!dhcp_done) {
        AX_LOG("DHCP timeout or failed");
        ax_net_stop();
        ax88179_close(ax);
        goto cleanup;
    }

    /* Install nsysnet shim hooks — ONLY if SHIM is enabled */
#if !AX_DISABLE_SHIM
    if (nsysnet_shim_install() == 0) {
        ax_mark(AX_MARK_SHIM_INSTALLED);
        AX_LOG("shim ready hooks=%d", handle_count);
    } else {
        AX_LOG("FAILED to install shim hooks");
    }
#else
    AX_LOG("SHIM disabled in build, no hooks installed");
#endif

    /* Main polling loop */
    ax_mark(AX_MARK_POLL_LOOP);
    char previous_ip[16] = "";
    uint32_t last_beat = 0;

    while (!atomic_load_explicit(&stopping, memory_order_acquire)) {
        int n = ax_net_poll();

        /*
         * Deferred traces only: logging directly from sendto()/recvfrom()
         * can recursively enter the UDP logger.
         */
        ax_net_wire_trace_drain();
        nsysnet_shim_trace_drain();

        const char *ip = ax_net_address();

        /* Log IP changes and status */
        if (ip && strcmp(ip, previous_ip)) {
            strncpy(previous_ip, ip, sizeof(previous_ip) - 1);
            AX_LOG("ready %s", ip);
            ax_mark(AX_MARK_DHCP_BOUND);
            /* Reopen UDP log on the adapter now */
            WHBLogUdpDeinit();
            if (WHBLogUdpInit())
                AX_LOG("udp-log %s", ip);
        } else if (!ip && previous_ip[0]) {
            previous_ip[0] = 0;
            AX_LOG("link/lease unavailable");
        }

        /* Status heartbeat while waiting for DHCP */
        if (!ip[0]) {
            uint32_t now = (uint32_t)OSTicksToMilliseconds(OSGetTime());
            if ((uint32_t)(now - last_beat) >= 3000) {
                last_beat = now;
                char st[160];
                ax_net_status(st, sizeof(st));
                AX_LOG("%s", st);
            }
        }

        /* Pace polling if stack not running */
        if (n < 0) OSSleepTicks(OSMillisecondsToTicks(1));

        /* Check for HOME/MINUS exit (if display available) */
        if (ax_display_check_exit()) {
            AX_LOG("HOME/MINUS requested, exiting");
            break;
        }
    }

    /* Cleanup */
#if !AX_DISABLE_SHIM
    nsysnet_shim_stop_accepting();
#endif
    ax_net_stop();
    ax_mark(AX_MARK_NET_STOP);
    ax88179_close(ax);
    ax_mark(AX_MARK_ADAPTER_CLOSED);

cleanup:
    AX_LOG("stopped");
    WHBLogUdpDeinit();
    return 0;
}

static void stop_worker(void)
{
    if (!started) return;
    nsysnet_shim_stop_accepting();
    atomic_store_explicit(&stopping, true, memory_order_release);
    /* Bounded join: a worker stuck in an ioctl must not deadlock the
     * whole app transition (that hangs the boot splash). Give it 2 s,
     * then leave the thread to die with the process. */
    for (int i = 0; i < 200 && !OSIsThreadTerminated(&worker); i++)
        OSSleepTicks(OSMillisecondsToTicks(10));
    if (!OSIsThreadTerminated(&worker)) {
        AX_LOG("worker stuck, leaving it to the process teardown");
    } else {
        OSJoinThread(&worker, NULL);
    }
    started = 0;
}

WUMS_INITIALIZE(args)
{
    (void)args;

    AX_LOG("CONFIG: loading during WUMS initialization");
    load_config();

    /* Device handles belong to a title; create them in APPLICATION_STARTS. */
}

WUMS_APPLICATION_STARTS()
{
    if (started) return;
    nsysnet_shim_begin_title();
    atomic_store_explicit(&stopping, false, memory_order_release);
    /* Core 2, not "any". Everything this module runs is background work,
     * and a thread of ours that spins must not be able to starve the
     * title's own main thread -- that is what turned a corrupted lwIP
     * timeout list into a console stuck on the boot logo instead of a
     * module that simply failed. */
    if (OSCreateThread(&worker, run_network, 0, NULL, stack + sizeof(stack),
                       sizeof(stack), 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        started = 1;
        OSSetThreadName(&worker, "AX88179 network");
        OSResumeThread(&worker);
        /* A title shorter than the watchdog's own sleep leaves the
         * previous thread still running on this struct; reusing it then
         * corrupts both. */
        if ((!watchdog_started || OSIsThreadTerminated(&watchdog)) &&
            OSCreateThread(&watchdog, run_watchdog, 0, NULL, wd_stack + sizeof(wd_stack),
                           sizeof(wd_stack), 24, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
            watchdog_started = 1;
            OSSetThreadName(&watchdog, "AX88179 watchdog");
            OSResumeThread(&watchdog);
        }
    }
}

WUMS_APPLICATION_REQUESTS_EXIT() { stop_worker(); }
WUMS_APPLICATION_ENDS() { stop_worker(); }
WUMS_DEINITIALIZE() { stop_worker(); }

/* Runtime entry points used by shim_probe through OSDynLoad. */
int AXShimBeginProbe(void) {
#if AX_DISABLE_SHIM
    return -2;
#else
    return nsysnet_shim_begin_probe();
#endif
}
int AXShimEndProbe(void) { return nsysnet_shim_end_probe(); }
WUMS_EXPORT_FUNCTION(AXShimBeginProbe);
WUMS_EXPORT_FUNCTION(AXShimEndProbe);
