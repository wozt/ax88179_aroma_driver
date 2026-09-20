#include <wums.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/debug.h>
#include <coreinit/title.h>
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
WUMS_MODULE_VERSION("0.2.39-exit-file-trace");
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
static int config_force_native = 0;
static int config_nssl_bridge = 0;

static void exit_trace(const char *msg)
{
    FILE *f =
        fopen("fs:/vol/external01/ax88179_exit.log", "a");

    if (!f)
        return;

    fprintf(
        f,
        "[%llu] %s\n",
        (unsigned long long)
            OSTicksToMilliseconds(OSGetTime()),
        msg);

    fflush(f);
    fclose(f);
}

static void exit_trace_int(
    const char *prefix,
    int value)
{
    char line[128];

    snprintf(
        line,
        sizeof(line),
        "%s%d",
        prefix,
        value);

    exit_trace(line);
}

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
            break;
        }
    }

    if (!f) {
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

        if (strstr(b, "route=native"))
            config_force_native = 1;
        else if (strstr(b, "route=ax"))
            config_force_native = 0;

        if (strstr(b, "nssl=bridge"))
            config_nssl_bridge = 1;
        else if (strstr(b, "nssl=native"))
            config_nssl_bridge = 0;

        int level;
        if (sscanf(b, "shim_trace=%d", &level) == 1) {
            if (level < 0) level = 0;
            if (level > 2) level = 2;
            config_shim_trace = level;
        }
    }

    fclose(f);

}

static const char *const mark_names[] = {
    "worker started", "udp log inited", "iosu patch", "adapter opened",
    "tx queue inited", "tcpip_init returned", "tcpip thread created",
    "tcpip thread entered", "setup_cb entered", "netif added",
    "dhcp started", "ax_net_start done", "poll loop running",
    "link up", "dhcp bound", "shim installed",
};

static atomic_bool stopping;
static atomic_bool title_ending;
static int started;
static atomic_uint worker_generation;
static atomic_int menu_seen;

/* If an adapter was opened but DHCP has not bound 45 s after the worker
 * starts, show on the fatal screen exactly how far the bring-up got.
 * The UDP log cannot be relied on for this: until DHCP succeeds it can
 * only leave over the console's own Wi-Fi, which may be down. */
static int run_watchdog(int argc, const char **argv)
{
    (void)argc; (void)argv;

    /*
     * Never sleep for 45 seconds in one uninterruptible chunk.
     * APPLICATION_ENDS must be able to stop this thread quickly.
     */
    for (unsigned waited = 0;
         waited < 45 * 1000;
         waited += 100) {

        if (atomic_load_explicit(
                &stopping,
                memory_order_acquire))
            return 0;

        OSSleepTicks(
            OSMillisecondsToTicks(100));
    }

    if (atomic_load_explicit(
            &stopping,
            memory_order_acquire))
        return 0;

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
 /*
 * Wii U Menu title IDs:
 *   JPN 0005001010040000
 *   USA 0005001010040100
 *   EUR 0005001010040200
 *
 * EnvironmentLoader/Aroma runs before this. We deliberately do absolutely
 * no AX/UHS/lwIP/shim/UDP-log work until one of these real Menu titles has
 * actually started.
 */
static int is_wiiu_menu_title(uint64_t title_id)
{
    return title_id == 0x0005001010040000ULL ||
           title_id == 0x0005001010040100ULL ||
           title_id == 0x0005001010040200ULL;
}

/*
 * Recover from a physical AX88179 USB removal without restarting the
 * title or the console.
 *
 * Existing TCP connections are not promised to survive a real cable/USB
 * outage. The guarantee here is that the interface itself is rebuilt and
 * new traffic can use AX again after the device returns.
 */
static int
recover_adapter(Ax88179 **current)
{
    if (!current)
        return -1;

    /*
     * Logging is currently routed through AX, so it cannot be relied on
     * while the device is absent. Re-create the UDP logger only after the
     * recovered interface has an address again.
     */
    WHBLogUdpDeinit();

    if (*current) {
        ax_net_stop();
        ax88179_close(*current);
        *current = NULL;
    }

    /*
     * Replugging USB power-cycles the chip. Do not use the title-transition
     * warm PHY shortcut on the replacement device.
     */
    ax88179_force_cold_next_open();

    while (!atomic_load_explicit(&stopping, memory_order_acquire)) {
        OSSleepTicks(OSMillisecondsToTicks(1000));

        char why[160];
        Ax88179 *candidate =
            ax88179_open(why, sizeof(why));

        if (!candidate)
            continue;

        if (ax_net_start(candidate) != 0) {
            ax88179_close(candidate);
            ax88179_force_cold_next_open();
            continue;
        }

        /*
         * keep_first normally restores the cached address immediately
         * once link comes back. dhcp=always may need a complete new lease,
         * so allow enough time for either mode.
         */
        OSTime deadline =
            OSGetTime() +
            OSMillisecondsToTicks(35000);

        int healthy = 0;

        while (!atomic_load_explicit(&stopping, memory_order_acquire) &&
               OSGetTime() < deadline) {

            int n = ax_net_poll();

            if (n == -2)
                break;

            const char *ip =
                ax_net_address();

            if (ip && ip[0]) {
                *current = candidate;
                healthy = 1;

                /*
                 * The old logger socket died with the missing interface.
                 * Open a fresh one now that the AX route exists again.
                 */
                WHBLogUdpInit();
                AX_LOG("hotplug recovered %s", ip);
                break;
            }

            OSSleepTicks(
                OSMillisecondsToTicks(20));
        }

        if (healthy)
            return 0;

        ax_net_stop();
        ax88179_close(candidate);
        ax88179_force_cold_next_open();
    }

    return -1;
}

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

    /*
     * EnvironmentLoader/Aroma is now filtered before the worker even exists,
     * so the old 25 second heuristic is no longer needed. This small delay
     * only lets the real application settle before UHS/lwIP starts.
     */
    const unsigned startup_delay_ms = 2000;

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
    nsysnet_shim_set_force_native(config_force_native);
    nsysnet_shim_set_nssl_bridge(config_nssl_bridge);
    ax_net_set_session_lease_mode(config_keep_first);
    AX_LOG("config dhcp=%s trace=%d dns=%s route=%s nssl=%s",
           config_keep_first ? "keep_first" : "always",
           config_shim_trace,
           config_system_dns ? "system" : "ax",
           config_force_native ? "native" : "ax",
           config_nssl_bridge ? "bridge" : "native");

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

        /*
         * FTPiiU may already own a native/Wi-Fi :21 listener. Ask FTPiiU
         * to discard/recreate it through its normal network-loss path.
         * The replacement socket is then created through the active AX shim.
         */
        int ftp_handoff =
            nsysnet_shim_request_ftp_handoff();

        if (ftp_handoff > 0)
            AX_LOG("FTP native listener handoff requested fd_count=%d",
                   ftp_handoff);
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

        if (n == -2) {
            /*
             * Repeated PHY/control failures mean the UHS interface has
             * disappeared. Rebuild it in-place instead of spinning forever
             * on a dead handle.
             */
            previous_ip[0] = 0;

            if (recover_adapter(&ax) != 0)
                break;

            continue;
        }

        const char *ip = ax_net_address();

        /*
         * Socket hooks only set an atomic flag. Logging happens here on
         * the worker thread so it cannot disturb the title's
         * socketlasterr() state.
         */
        if (nsysnet_shim_take_ax_activity()) {
            AX_LOG("title traffic routed through AX88179 (%s)",
                   ip && ip[0] ? ip : "IP unavailable");
        }

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
        if (!ip || !ip[0]) {
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
    int ending_title =
        atomic_load_explicit(
            &title_ending,
            memory_order_acquire);

    exit_trace_int(
        "worker cleanup begin ending=",
        ending_title);

#if !AX_DISABLE_SHIM
    exit_trace("before nsysnet_shim_stop_accepting");

    nsysnet_shim_stop_accepting();

    exit_trace("after nsysnet_shim_stop_accepting");

    /*
     * During an ordinary runtime stop, close lwIP-owned sockets.
     *
     * During APPLICATION_ENDS the title has already performed its socket
     * and NSSL cleanup, so do not touch those PCBs again.
     */
    if (!ending_title)
        nsysnet_shim_drain_owned_sockets();
#endif

    if (ending_title) {
        exit_trace("before ax_net_abandon_title");

        int tcpip_rc =
            ax_net_abandon_title();

        exit_trace_int(
            "after ax_net_abandon_title rc=",
            tcpip_rc);
    } else {
        ax_net_stop();
    }

    ax_mark(AX_MARK_NET_STOP);

    if (ax) {
        if (ending_title) {
            exit_trace("before ax88179_abandon_title");

            ax88179_abandon_title(ax);

            exit_trace("after ax88179_abandon_title");
        } else {
            ax88179_close(ax);
        }

        ax = NULL;
    }

    ax_mark(AX_MARK_ADAPTER_CLOSED);

    exit_trace("worker cleanup complete");

cleanup:
    exit_trace("worker returning");

    AX_LOG("stopped");
    WHBLogUdpDeinit();
    return 0;
}

static void note_exit_requested(void)
{
    if (!started) return;

    /*
     * REQUESTS_EXIT means the title has only been asked to leave.
     * Keep AX/lwIP/NSSL fully alive until __PPCExit/APPLICATION_ENDS.
     */
    AX_LOG("REQUESTS_EXIT title=%016llx keep-network-alive",
           (unsigned long long)OSGetTitleID());
}

/*
 * Our own APPLICATION_ENDS is too early for destructive cleanup:
 * Aroma still has to call APPLICATION_ENDS for every module after us.
 *
 * Keep AX/lwIP/NSSL alive until ALL_APPLICATION_ENDS_DONE.
 */
static void note_application_ends(void)
{
    if (!started)
        return;

    AX_LOG("APPLICATION_ENDS title=%016llx defer-cleanup",
           (unsigned long long)OSGetTitleID());
}

/*
 * Aroma __PPCExit order:
 *
 *   APPLICATION_ENDS (all modules)
 *   ALL_APPLICATION_ENDS_DONE
 *   FINI_WUT_SOCKETS
 *   FINI_WUT_DEVOPTAB
 *   real___PPCExit
 *
 * Therefore this is the correct point to stop our per-title resources:
 * every other module has already run its APPLICATION_ENDS, while WUT
 * sockets and devoptab still exist.
 */
static void stop_after_all_application_ends(void)
{
    if (!started)
        return;

    AX_LOG("ALL_APPLICATION_ENDS_DONE title=%016llx stopping",
           (unsigned long long)OSGetTitleID());

    exit_trace("ALL_APPLICATION_ENDS_DONE entered");

    atomic_store_explicit(
        &title_ending,
        true,
        memory_order_release);

    atomic_store_explicit(
        &stopping,
        true,
        memory_order_release);

    exit_trace("worker stop signalled");

    /*
     * Relay shutdown can consume up to roughly 500 ms and tcpip shutdown
     * another ~100 ms. Give the worker a comfortable ceiling here.
     */
    for (int i = 0;
         i < 200 &&
         !OSIsThreadTerminated(&worker);
         ++i) {

        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    exit_trace_int(
        "worker terminated after wait=",
        OSIsThreadTerminated(&worker) ? 1 : 0);

    if (OSIsThreadTerminated(&worker)) {
        exit_trace("before worker join");

        OSJoinThread(&worker, NULL);

        exit_trace("after worker join");
    } else {
        exit_trace("worker still alive after 2s");

        AX_LOG("ALL_APPLICATION_ENDS_DONE worker still alive");
    }

    /*
     * The watchdog is cooperative since 0.2.36 and checks stopping every
     * 100 ms.
     */
    if (watchdog_started) {
        for (int i = 0;
             i < 50 &&
             !OSIsThreadTerminated(&watchdog);
             ++i) {

            OSSleepTicks(
                OSMillisecondsToTicks(10));
        }

        exit_trace_int(
            "watchdog terminated after wait=",
            OSIsThreadTerminated(&watchdog) ? 1 : 0);

        if (OSIsThreadTerminated(&watchdog)) {
            exit_trace("before watchdog join");

            OSJoinThread(&watchdog, NULL);

            exit_trace("after watchdog join");
        }

        watchdog_started = 0;
    }

    started = 0;

    exit_trace("ALL_APPLICATION_ENDS_DONE returning");
}



/*
 * Full stop is retained only for actual module deinitialisation, where we
 * are not sitting inside a title's __PPCExit transition.
 */
static void stop_worker(void)
{
    if (!started)
        return;

    nsysnet_shim_quiesce();

    atomic_store_explicit(
        &stopping,
        true,
        memory_order_release);

    for (int i = 0;
         i < 200 &&
         !OSIsThreadTerminated(&worker);
         ++i) {
        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    if (OSIsThreadTerminated(&worker))
        OSJoinThread(&worker, NULL);

    started = 0;
}

WUMS_INITIALIZE(args)
{
    (void)args;

    /*
     * SD access only. In particular, do NOT initialise logging/networking
     * here: EnvironmentLoader/Aroma is still bootstrapping at this point.
     */
    load_config();
}

WUMS_APPLICATION_STARTS()
{
    uint64_t title_id = OSGetTitleID();

    /*
     * The WUMS module is resident while EnvironmentLoader/Aroma itself is
     * still executing. Before the first real Wii U Menu title appears:
     *
     *   - no worker
     *   - no UDP logger
     *   - no IOSU endpoint patch
     *   - no UHS access
     *   - no lwIP
     *   - no FunctionPatcher nsysnet shim
     *
     * Waiting 2 seconds or 2 minutes on the Aroma selector therefore has
     * absolutely no influence on the network driver anymore.
     */
    if (!atomic_load(&menu_seen)) {
        if (!is_wiiu_menu_title(title_id))
            return;

        atomic_store(&menu_seen, 1);
    }

    if (started) return;

    nsysnet_shim_begin_title();

    atomic_store_explicit(
        &title_ending,
        false,
        memory_order_release);

    atomic_store_explicit(
        &stopping,
        false,
        memory_order_release);
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

WUMS_APPLICATION_REQUESTS_EXIT() { note_exit_requested(); }
WUMS_APPLICATION_ENDS() { note_application_ends(); }
WUMS_ALL_APPLICATION_ENDS_DONE() { stop_after_all_application_ends(); }
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
