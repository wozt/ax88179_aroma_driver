#include <arpa/inet.h>
#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wut_rplwrap.h>

#include "probe.h"

#define DUMP_WORDS 24
#define HELPER_DUMP_WORDS 128

static OSDynLoad_Module g_nsysnet;

struct export_desc {
    const char *name;
    void *addr;
};

static struct export_desc exports[] = {
    { "getaddrinfo",          NULL },
    { "getaddrinfo_async",    NULL },
    { "getaddrinfo_async_rs", NULL },
    { "getaddrinfo_rs",       NULL },
    { "gethostbyaddr",        NULL },
    { "dns_abort_by_hname",   NULL },
    { "clear_resolver_cache", NULL },
    { "set_resolver_allocator", NULL },
    { "freeaddrinfo",           NULL },
};

static uintptr_t branch_target(uintptr_t pc, uint32_t insn)
{
    /*
     * PPC 'b' / 'bl':
     * opcode 18, LI=bits 6..29, AA=bit1.
     */
    if ((insn & 0xfc000000u) != 0x48000000u)
        return 0;

    int32_t disp = (int32_t)(insn & 0x03fffffcu);

    if (disp & 0x02000000)
        disp |= (int32_t)0xfc000000u;

    if (insn & 0x00000002u)
        return (uintptr_t)(uint32_t)disp;

    return pc + disp;
}

static uintptr_t find_first_direct_call(void *fn)
{
    if (!fn)
        return 0;

    volatile const uint32_t *p =
        (volatile const uint32_t *)fn;

    uintptr_t base = (uintptr_t)fn;

    /*
     * Scan enough of the tiny export wrapper to reach its common
     * getaddrinfo helper call.
     *
     * PPC opcode 18 + LK=1 = direct BL.
     */
    for (unsigned i = 0; i < 16; i++) {
        uint32_t insn = p[i];

        if ((insn & 0xfc000001u) !=
            0x48000001u)
            continue;

        uintptr_t target =
            branch_target(
                base + i * 4,
                insn);

        if (target)
            return target;
    }

    return 0;
}

static void dump_words(
    const char *name,
    uintptr_t address,
    unsigned words)
{
    if (!address)
        return;

    volatile const uint32_t *p =
        (volatile const uint32_t *)address;

    probe_say(
        "=== INTERNAL %s @ %08x words=%u ===",
        name,
        (unsigned)address,
        words);

    for (unsigned i = 0; i < words; i += 4) {
        probe_say(
            "%08x: %08x %08x %08x %08x",
            (unsigned)(address + i * 4),
            p[i + 0],
            p[i + 1],
            p[i + 2],
            p[i + 3]);

        if ((i & 0x0f) == 0x0c) {
            if (!probe_poll())
                return;

            OSSleepTicks(
                OSMillisecondsToTicks(5));
        }
    }
}

static void dump_code(const char *name, void *fn)
{
    if (!fn) {
        probe_say("%s: NULL", name);
        return;
    }

    volatile const uint32_t *p =
        (volatile const uint32_t *)fn;

    uintptr_t base = (uintptr_t)fn;

    probe_say(
        "=== EXPORT %s @ %08x ===",
        name,
        (unsigned)base);

    for (unsigned i = 0; i < DUMP_WORDS; i += 4) {
        uint32_t a = p[i + 0];
        uint32_t b = p[i + 1];
        uint32_t c = p[i + 2];
        uint32_t d = p[i + 3];

        probe_say(
            "%08x: %08x %08x %08x %08x",
            (unsigned)(base + i * 4),
            a, b, c, d);
    }

    /*
     * If the export begins with a direct branch/trampoline, follow it
     * and dump the destination too. This is useful for RPL wrappers.
     */
    for (unsigned i = 0; i < 4; i++) {
        uintptr_t pc = base + i * 4;
        uint32_t insn = p[i];

        uintptr_t target =
            branch_target(pc, insn);

        if (!target)
            continue;

        probe_say(
            "%s direct branch +%u -> %08x",
            name,
            i * 4,
            (unsigned)target);

        volatile const uint32_t *q =
            (volatile const uint32_t *)target;

        probe_say(
            "TARGET %08x: %08x %08x %08x %08x",
            (unsigned)target,
            q[0], q[1], q[2], q[3]);

        probe_say(
            "TARGET %08x: %08x %08x %08x %08x",
            (unsigned)(target + 16),
            q[4], q[5], q[6], q[7]);

        break;
    }
}

static int resolve_exports(void)
{
    OSDynLoad_Error err =
        OSDynLoad_Acquire(
            "nsysnet.rpl",
            &g_nsysnet);

    if (err != OS_DYNLOAD_OK) {
        probe_say(
            "Acquire nsysnet FAIL %08x",
            (unsigned)err);
        return -1;
    }

    for (unsigned i = 0;
         i < sizeof(exports) / sizeof(exports[0]);
         i++) {

        err = OSDynLoad_FindExport(
            g_nsysnet,
            OS_DYNLOAD_EXPORT_FUNC,
            exports[i].name,
            &exports[i].addr);

        probe_say(
            "resolve %-22s rc=%08x addr=%08x",
            exports[i].name,
            (unsigned)err,
            (unsigned)(uintptr_t)exports[i].addr);

        if (err != OS_DYNLOAD_OK)
            exports[i].addr = NULL;
    }

    return 0;
}


/* ------------------------------------------------------------------ */
/* Controlled native ABI calls                                        */

#define BEHAVIOR_STACK_SIZE (96 * 1024)
#define BEHAVIOR_TEST_COUNT 8
#define NSN_EAI_INPROGRESS 15

struct nsn_sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct nsn_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

struct nsn_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;

    uint32_t ai_addrlen;

    char *ai_canonname;
    struct nsn_sockaddr *ai_addr;
    struct nsn_addrinfo *ai_next;
};

extern int RPLWRAP(getaddrinfo_async)(
    const char *node,
    const char *service,
    const struct nsn_addrinfo *hints,
    struct nsn_addrinfo **res);

extern int RPLWRAP(getaddrinfo_async_rs)(
    const char *node,
    const char *service,
    const struct nsn_addrinfo *hints,
    struct nsn_addrinfo **res);

extern int RPLWRAP(getaddrinfo_rs)(
    const char *node,
    const char *service,
    const struct nsn_addrinfo *hints,
    struct nsn_addrinfo **res);

extern void RPLWRAP(freeaddrinfo)(
    struct nsn_addrinfo *res);

typedef int (*raw_getaddrinfo_fn)(
    const char *node,
    const char *service,
    const struct nsn_addrinfo *hints,
    struct nsn_addrinfo **res);

typedef void (*raw_freeaddrinfo_fn)(
    struct nsn_addrinfo *res);

struct behavior_spec {
    const char *label;
    const char *export_name;
    const char *node;
};

struct behavior_result {
    const char *label;
    const char *node;

    int rc;
    uint64_t elapsed_ms;

    uintptr_t immediate_res;
    uintptr_t delayed_res;

    int nodes;

    int first_family;
    int first_socktype;
    int first_protocol;

    uint16_t first_port;
    uint32_t first_addr;
};

static const struct behavior_spec behavior_specs[] = {
    { "sync-num",     "getaddrinfo",          "127.0.0.1" },
    { "async-num",    "getaddrinfo_async",    "127.0.0.1" },
    { "rs-num",       "getaddrinfo_rs",       "127.0.0.1" },
    { "async-rs-num", "getaddrinfo_async_rs", "127.0.0.1" },

    /*
     * Different names deliberately avoid having the first native call
     * populate a resolver cache entry used by every following variant.
     */
    { "sync-dns",     "getaddrinfo",          "example.com" },
    { "async-dns",    "getaddrinfo_async",    "example.net" },
    { "rs-dns",       "getaddrinfo_rs",       "example.org" },
    { "async-rs-dns", "getaddrinfo_async_rs", "iana.org" },
};

static struct behavior_result behavior_results[BEHAVIOR_TEST_COUNT];
static struct nsn_addrinfo *behavior_res_slots[BEHAVIOR_TEST_COUNT];

static OSThread behavior_thread
    __attribute__((aligned(0x40)));

static uint8_t behavior_stack[BEHAVIOR_STACK_SIZE]
    __attribute__((aligned(0x40)));

static atomic_int behavior_done;

static void *find_export_addr(const char *name)
{
    for (unsigned i = 0;
         i < sizeof(exports) / sizeof(exports[0]);
         i++) {

        if (strcmp(exports[i].name, name) == 0)
            return exports[i].addr;
    }

    return NULL;
}

static void snapshot_addrinfo(
    struct behavior_result *out,
    struct nsn_addrinfo *res)
{
    if (!out || !res)
        return;

    struct nsn_addrinfo *cur = res;

    while (cur && out->nodes < 8) {
        if (out->nodes == 0) {
            out->first_family = cur->ai_family;
            out->first_socktype = cur->ai_socktype;
            out->first_protocol = cur->ai_protocol;

            if (cur->ai_addr &&
                cur->ai_addrlen >= sizeof(struct nsn_sockaddr_in) &&
                cur->ai_addr->sa_family == 2) {

                struct nsn_sockaddr_in *sin =
                    (struct nsn_sockaddr_in *)cur->ai_addr;

                out->first_port = sin->sin_port;
                out->first_addr = sin->sin_addr;
            }
        }

        out->nodes++;
        cur = cur->ai_next;
    }
}

static int behavior_worker(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    for (unsigned i = 0;
         i < BEHAVIOR_TEST_COUNT;
         i++) {

        struct behavior_result *out =
            &behavior_results[i];

        const struct behavior_spec *spec =
            &behavior_specs[i];

        memset(out, 0, sizeof(*out));

        out->label = spec->label;
        out->node = spec->node;

        raw_getaddrinfo_fn fn =
            (raw_getaddrinfo_fn)find_export_addr(
                spec->export_name);

        if (!fn) {
            out->rc = 0x7fffffff;
            continue;
        }

        struct nsn_addrinfo hints;
        memset(&hints, 0, sizeof(hints));

        hints.ai_family = 2;      /* AF_INET */
        hints.ai_socktype = 1;    /* SOCK_STREAM */
        hints.ai_protocol = 6;    /* TCP */

        behavior_res_slots[i] = NULL;

        OSTime begin = OSGetTime();

        int rc =
            fn(spec->node,
               "80",
               &hints,
               &behavior_res_slots[i]);

        OSTime end = OSGetTime();

        out->rc = rc;

        out->elapsed_ms =
            OSTicksToMilliseconds(end - begin);

        out->immediate_res =
            (uintptr_t)behavior_res_slots[i];

        /*
         * If "async" really means that completion happens after the
         * public function returns with EAI_INPROGRESS, keep the caller
         * supplied result slot alive and inspect it again later.
         *
         * The slot is global rather than stack-local specifically so
         * that a genuine asynchronous native resolver cannot write into
         * a dead stack frame.
         */
        if (rc == NSN_EAI_INPROGRESS) {
            OSSleepTicks(
                OSMillisecondsToTicks(2000));
        }

        out->delayed_res =
            (uintptr_t)behavior_res_slots[i];

        if (behavior_res_slots[i]) {
            snapshot_addrinfo(
                out,
                behavior_res_slots[i]);
        }

        /*
         * A completed POSIX-style result is safe to release immediately.
         * Do not free a possible genuinely asynchronous result here.
         */
        if (rc == 0 &&
            behavior_res_slots[i] &&
            native_free) {

            native_free(
                behavior_res_slots[i]);

            behavior_res_slots[i] = NULL;
        }
    }

    atomic_store(&behavior_done, 1);
    return 0;
}


/* ------------------------------------------------------------------ */
/* Async resolver polling semantics                                   */

struct async_poll_result {
    const char *label;
    const char *node;

    int initial_rc;
    int final_rc;

    int attempts;
    uint64_t elapsed_ms;

    uintptr_t res;

    int nodes;
    int first_family;
    int first_socktype;
    int first_protocol;
    uint16_t first_port;
    uint32_t first_addr;
};

static struct async_poll_result poll_results[2];

static void run_one_async_poll(
    struct async_poll_result *out,
    const char *label,
    const char *export_name,
    const char *node)
{
    memset(out, 0, sizeof(*out));

    out->label = label;
    out->node = node;
    out->initial_rc = 0x7fffffff;
    out->final_rc = 0x7fffffff;

    raw_getaddrinfo_fn fn =
        (raw_getaddrinfo_fn)find_export_addr(
            export_name);

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    if (!fn)
        return;

    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    OSTime begin = OSGetTime();
    OSTime deadline =
        begin + OSMillisecondsToTicks(5000);

    int rc = NSN_EAI_INPROGRESS;

    while (OSGetTime() < deadline) {
        res = NULL;

        rc =
            fn(node,
               "80",
               &hints,
               &res);

        out->attempts++;

        if (out->attempts == 1)
            out->initial_rc = rc;

        if (rc != NSN_EAI_INPROGRESS)
            break;

        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    out->final_rc = rc;
    out->elapsed_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    out->res = (uintptr_t)res;

    if (res) {
        struct behavior_result tmp;
        memset(&tmp, 0, sizeof(tmp));

        snapshot_addrinfo(
            &tmp,
            res);

        out->nodes = tmp.nodes;
        out->first_family = tmp.first_family;
        out->first_socktype = tmp.first_socktype;
        out->first_protocol = tmp.first_protocol;
        out->first_port = tmp.first_port;
        out->first_addr = tmp.first_addr;
    }

    if (rc == 0 &&
        res &&
        native_free) {

        native_free(res);
    }
}

static int poll_worker(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Fresh hostnames compared with the previous test so this test does
     * not accidentally consume an entry created by the earlier native
     * resolver calls.
     */
    run_one_async_poll(
        &poll_results[0],
        "async-poll",
        "getaddrinfo_async",
        "www.debian.org");

    run_one_async_poll(
        &poll_results[1],
        "async-rs-poll",
        "getaddrinfo_async_rs",
        "www.kernel.org");

    atomic_store(&behavior_done, 1);
    return 0;
}

static void run_async_poll_probe(void)
{
    memset(
        poll_results,
        0,
        sizeof(poll_results));

    atomic_store(&behavior_done, 0);

    probe_say("%s", "");
    probe_say(
        "=== ASYNC GETADDRINFO POLLING TEST ===");

    BOOL created =
        OSCreateThread(
            &behavior_thread,
            poll_worker,
            0,
            NULL,
            behavior_stack + sizeof(behavior_stack),
            sizeof(behavior_stack),
            16,
            OS_THREAD_ATTRIB_AFFINITY_ANY);

    if (!created) {
        probe_say(
            "async poll worker OSCreateThread FAIL");
        return;
    }

    OSSetThreadName(
        &behavior_thread,
        "AX DNS poll worker");

    OSResumeThread(
        &behavior_thread);

    while (!atomic_load(&behavior_done)) {
        probe_poll();

        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    int thread_result = -1;

    OSJoinThread(
        &behavior_thread,
        &thread_result);

    probe_say(
        "async poll worker result=%d",
        thread_result);

    for (unsigned i = 0; i < 2; i++) {
        struct async_poll_result *r =
            &poll_results[i];

        probe_say(
            "POLL-GAI %-13s node=%s",
            r->label,
            r->node);

        probe_say(
            "  initial=%d final=%d attempts=%d elapsed=%llu ms res=%08x",
            r->initial_rc,
            r->final_rc,
            r->attempts,
            (unsigned long long)r->elapsed_ms,
            (unsigned)r->res);

        if (r->nodes > 0) {
            char ip[32] = "?";

            struct in_addr addr = {
                .s_addr = r->first_addr
            };

            inet_ntop(
                AF_INET,
                &addr,
                ip,
                sizeof(ip));

            probe_say(
                "  nodes=%d first=%s:%u fam=%d type=%d proto=%d",
                r->nodes,
                ip,
                ntohs(r->first_port),
                r->first_family,
                r->first_socktype,
                r->first_protocol);
        }
    }

    probe_say(
        "=== END ASYNC GETADDRINFO POLLING TEST ===");
}


typedef int (*shim_gai_fn)(
    const char *,
    const char *,
    const struct nsn_addrinfo *,
    struct nsn_addrinfo **);

static void run_one_shim_poll(
    const char *label,
    shim_gai_fn fn,
    const char *node)
{
    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    OSTime begin = OSGetTime();
    OSTime deadline =
        begin + OSMillisecondsToTicks(5000);

    int attempts = 0;
    int initial = 0x7fffffff;
    int rc = NSN_EAI_INPROGRESS;

    while (OSGetTime() < deadline) {
        res = NULL;

        rc =
            fn(node,
               "80",
               &hints,
               &res);

        attempts++;

        if (attempts == 1)
            initial = rc;

        if (rc != NSN_EAI_INPROGRESS)
            break;

        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    uint64_t elapsed =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "SHIM-GAI %-14s node=%s",
        label,
        node);

    probe_say(
        "  initial=%d final=%d attempts=%d elapsed=%llu ms res=%08x",
        initial,
        rc,
        attempts,
        (unsigned long long)elapsed,
        (unsigned)(uintptr_t)res);

    if (rc == 0 && res) {
        struct nsn_addrinfo *first = res;

        char ip[32] = "?";
        unsigned port = 0;

        if (first->ai_addr &&
            first->ai_addrlen >=
                sizeof(struct nsn_sockaddr_in) &&
            first->ai_addr->sa_family == 2) {

            struct nsn_sockaddr_in *sin =
                (struct nsn_sockaddr_in *)
                    first->ai_addr;

            struct in_addr a = {
                .s_addr = sin->sin_addr
            };

            inet_ntop(
                AF_INET,
                &a,
                ip,
                sizeof(ip));

            port = ntohs(
                sin->sin_port);
        }

        probe_say(
            "  first=%s:%u fam=%d type=%d proto=%d",
            ip,
            port,
            first->ai_family,
            first->ai_socktype,
            first->ai_protocol);

        RPLWRAP(freeaddrinfo)(res);
    }
}


typedef void (*raw_clear_resolver_cache_fn)(void);

static void run_clear_resolver_cache_probe(void)
{
    probe_say("%s", "");
    probe_say(
        "=== NATIVE CLEAR RESOLVER CACHE TEST ===");

    raw_getaddrinfo_fn sync_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo");

    raw_getaddrinfo_fn async_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo_async");

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    raw_clear_resolver_cache_fn clear_fn =
        (raw_clear_resolver_cache_fn)find_export_addr(
            "clear_resolver_cache");

    if (!sync_fn ||
        !async_fn ||
        !native_free ||
        !clear_fn) {

        probe_say(
            "CLEAR-CACHE missing native export");
        return;
    }

    const char *host =
        "www.openbsd.org";

    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    /*
     * Step 1:
     * Populate the native resolver cache synchronously.
     */
    OSTime begin =
        OSGetTime();

    int rc =
        sync_fn(
            host,
            "80",
            &hints,
            &res);

    uint64_t populate_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "CLEAR-CACHE populate rc=%d ms=%llu res=%08x",
        rc,
        (unsigned long long)populate_ms,
        (unsigned)(uintptr_t)res);

    if (rc != 0 || !res) {
        probe_say(
            "CLEAR-CACHE populate FAIL");
        return;
    }

    native_free(res);
    res = NULL;

    /*
     * Step 2:
     * Prove the hostname is now cached.
     *
     * Native async getaddrinfo should return rc=0 immediately.
     */
    begin =
        OSGetTime();

    rc =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    uint64_t cached_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "CLEAR-CACHE before clear rc=%d ms=%llu res=%08x",
        rc,
        (unsigned long long)cached_ms,
        (unsigned)(uintptr_t)res);

    if (rc == 0 && res) {
        native_free(res);
        res = NULL;
    }

    /*
     * Step 3:
     * Native cache flush.
     */
    clear_fn();

    probe_say(
        "CLEAR-CACHE clear_resolver_cache returned");

    /*
     * Step 4:
     * The same hostname must no longer be immediately available.
     */
    begin =
        OSGetTime();

    rc =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    uint64_t after_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "CLEAR-CACHE after clear initial=%d ms=%llu res=%08x",
        rc,
        (unsigned long long)after_ms,
        (unsigned)(uintptr_t)res);

    int initial_after_clear =
        rc;

    int attempts = 1;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(5000);

    while (rc == NSN_EAI_INPROGRESS &&
           OSGetTime() < deadline) {

        OSSleepTicks(
            OSMillisecondsToTicks(10));

        res = NULL;

        rc =
            async_fn(
                host,
                "80",
                &hints,
                &res);

        attempts++;
    }

    uint64_t refill_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "CLEAR-CACHE refill initial=%d final=%d attempts=%d elapsed=%llu ms res=%08x",
        initial_after_clear,
        rc,
        attempts,
        (unsigned long long)refill_ms,
        (unsigned)(uintptr_t)res);

    if (rc == 0 && res)
        native_free(res);

    probe_say(
        "=== END NATIVE CLEAR RESOLVER CACHE TEST ===");
}


static void run_clear_pending_probe(void)
{
    probe_say("%s", "");
    probe_say(
        "=== NATIVE CLEAR PENDING DNS TEST ===");

    raw_getaddrinfo_fn async_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo_async");

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    raw_clear_resolver_cache_fn clear_fn =
        (raw_clear_resolver_cache_fn)find_export_addr(
            "clear_resolver_cache");

    if (!async_fn ||
        !native_free ||
        !clear_fn) {

        probe_say(
            "CLEAR-PENDING missing native export");
        return;
    }

    /*
     * Fresh hostname not used by the earlier tests.
     */
    const char *host =
        "www.netbsd.org";

    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    OSTime begin =
        OSGetTime();

    int rc =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    probe_say(
        "CLEAR-PENDING start rc=%d res=%08x",
        rc,
        (unsigned)(uintptr_t)res);

    if (rc == 0 && res) {
        /*
         * Unexpected cache hit. Do not pretend this tested the pending
         * case.
         */
        native_free(res);

        probe_say(
            "CLEAR-PENDING INCONCLUSIVE: hostname already cached");

        probe_say(
            "=== END NATIVE CLEAR PENDING DNS TEST ===");
        return;
    }

    if (rc != NSN_EAI_INPROGRESS) {
        probe_say(
            "CLEAR-PENDING unexpected initial rc=%d",
            rc);

        probe_say(
            "=== END NATIVE CLEAR PENDING DNS TEST ===");
        return;
    }

    /*
     * Issue ioctl 0x32 while the native resolver request is genuinely
     * outstanding.
     */
    clear_fn();

    probe_say(
        "CLEAR-PENDING clear called while rc=15");

    /*
     * Do NOT call getaddrinfo_async during this interval.
     *
     * If the original request survives the clear, it has time to finish
     * and the next poll should normally be an immediate cache hit.
     *
     * If clear cancels/flushes it, the next call may have to begin a new
     * lookup and return EAI_INPROGRESS again.
     */
    OSSleepTicks(
        OSMillisecondsToTicks(750));

    res = NULL;

    OSTime first_poll_begin =
        OSGetTime();

    rc =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    uint64_t first_poll_ms =
        OSTicksToMilliseconds(
            OSGetTime() - first_poll_begin);

    probe_say(
        "CLEAR-PENDING first poll after 750ms rc=%d ms=%llu res=%08x",
        rc,
        (unsigned long long)first_poll_ms,
        (unsigned)(uintptr_t)res);

    int first_poll_rc = rc;
    int attempts = 1;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(5000);

    while (rc == NSN_EAI_INPROGRESS &&
           OSGetTime() < deadline) {

        OSSleepTicks(
            OSMillisecondsToTicks(10));

        res = NULL;

        rc =
            async_fn(
                host,
                "80",
                &hints,
                &res);

        attempts++;
    }

    uint64_t total_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "CLEAR-PENDING final first_poll=%d final=%d attempts=%d total=%llu ms res=%08x",
        first_poll_rc,
        rc,
        attempts,
        (unsigned long long)total_ms,
        (unsigned)(uintptr_t)res);

    if (rc == 0 && res) {
        struct nsn_addrinfo *first =
            res;

        char ip[32] = "?";

        if (first->ai_addr &&
            first->ai_addrlen >=
                sizeof(struct nsn_sockaddr_in) &&
            first->ai_addr->sa_family == 2) {

            struct nsn_sockaddr_in *sin =
                (struct nsn_sockaddr_in *)
                    first->ai_addr;

            struct in_addr a = {
                .s_addr = sin->sin_addr
            };

            inet_ntop(
                AF_INET,
                &a,
                ip,
                sizeof(ip));
        }

        probe_say(
            "CLEAR-PENDING result ip=%s",
            ip);

        native_free(res);
    }

    probe_say(
        "=== END NATIVE CLEAR PENDING DNS TEST ===");
}


static int poll_native_async_failure(
    raw_getaddrinfo_fn fn,
    raw_freeaddrinfo_fn native_free,
    const char *host,
    int *attempts_out,
    uint64_t *elapsed_out)
{
    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    OSTime begin = OSGetTime();
    OSTime deadline =
        begin + OSMillisecondsToTicks(5000);

    int attempts = 0;
    int rc = NSN_EAI_INPROGRESS;

    while (OSGetTime() < deadline) {
        res = NULL;

        rc =
            fn(host,
               "80",
               &hints,
               &res);

        attempts++;

        if (rc != NSN_EAI_INPROGRESS)
            break;

        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    if (elapsed_out) {
        *elapsed_out =
            OSTicksToMilliseconds(
                OSGetTime() - begin);
    }

    if (attempts_out)
        *attempts_out = attempts;

    if (rc == 0 && res && native_free) {
        native_free(res);
    }

    return rc;
}

static void run_clear_negative_probe(void)
{
    probe_say("%s", "");
    probe_say(
        "=== NATIVE NEGATIVE DNS CACHE TEST ===");

    raw_getaddrinfo_fn async_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo_async");

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    raw_clear_resolver_cache_fn clear_fn =
        (raw_clear_resolver_cache_fn)find_export_addr(
            "clear_resolver_cache");

    if (!async_fn ||
        !native_free ||
        !clear_fn) {

        probe_say(
            "NEG-CACHE missing native export");
        return;
    }

    const char *host =
        "ax88179-negative-cache-probe.invalid";

    /*
     * First resolution: should normally begin asynchronously and end
     * with an EAI_* failure.
     */
    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    OSTime begin = OSGetTime();

    int initial1 =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    if (res) {
        native_free(res);
        res = NULL;
    }

    int final1 = initial1;
    int attempts1 = 1;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(5000);

    while (final1 == NSN_EAI_INPROGRESS &&
           OSGetTime() < deadline) {

        OSSleepTicks(
            OSMillisecondsToTicks(10));

        res = NULL;

        final1 =
            async_fn(
                host,
                "80",
                &hints,
                &res);

        attempts1++;

        if (res) {
            native_free(res);
            res = NULL;
        }
    }

    uint64_t elapsed1 =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "NEG-CACHE first initial=%d final=%d attempts=%d elapsed=%llu ms",
        initial1,
        final1,
        attempts1,
        (unsigned long long)elapsed1);

    /*
     * Second call immediately after the failure.
     *
     * If the failure itself is cached, this should return the final
     * error immediately instead of EAI_INPROGRESS.
     */
    begin = OSGetTime();

    res = NULL;

    int second_initial =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    uint64_t second_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "NEG-CACHE second initial=%d ms=%llu res=%08x",
        second_initial,
        (unsigned long long)second_ms,
        (unsigned)(uintptr_t)res);

    if (res) {
        native_free(res);
        res = NULL;
    }

    /*
     * If that second call started another request, allow it to finish
     * before testing clear_resolver_cache so no request is left pending.
     */
    if (second_initial == NSN_EAI_INPROGRESS) {
        int dummy_attempts = 0;
        uint64_t dummy_elapsed = 0;

        int second_final =
            poll_native_async_failure(
                async_fn,
                native_free,
                host,
                &dummy_attempts,
                &dummy_elapsed);

        probe_say(
            "NEG-CACHE second completion final=%d extra_attempts=%d elapsed=%llu ms",
            second_final,
            dummy_attempts,
            (unsigned long long)dummy_elapsed);
    }

    clear_fn();

    probe_say(
        "NEG-CACHE clear_resolver_cache returned");

    /*
     * Critical observation:
     *
     * cached-negative before clear + EAI_INPROGRESS after clear
     * would demonstrate that ioctl 0x32 clears negative resolver state.
     */
    begin = OSGetTime();

    res = NULL;

    int after_clear =
        async_fn(
            host,
            "80",
            &hints,
            &res);

    uint64_t after_ms =
        OSTicksToMilliseconds(
            OSGetTime() - begin);

    probe_say(
        "NEG-CACHE after clear initial=%d ms=%llu res=%08x",
        after_clear,
        (unsigned long long)after_ms,
        (unsigned)(uintptr_t)res);

    if (res) {
        native_free(res);
        res = NULL;
    }

    /*
     * Clean up an async request started by the final observation.
     */
    if (after_clear == NSN_EAI_INPROGRESS) {
        int attempts = 0;
        uint64_t elapsed = 0;

        int final =
            poll_native_async_failure(
                async_fn,
                native_free,
                host,
                &attempts,
                &elapsed);

        probe_say(
            "NEG-CACHE after-clear completion final=%d attempts=%d elapsed=%llu ms",
            final,
            attempts,
            (unsigned long long)elapsed);
    }

    probe_say(
        "=== END NATIVE NEGATIVE DNS CACHE TEST ===");
}


typedef int (*raw_dns_abort_fn)(
    const char *hostname);

static int start_fresh_native_async(
    raw_getaddrinfo_fn fn,
    raw_freeaddrinfo_fn native_free,
    const char *const *candidates,
    unsigned count,
    const char **chosen_out)
{
    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    for (unsigned i = 0; i < count; i++) {
        struct nsn_addrinfo *res = NULL;

        int rc =
            fn(candidates[i],
               "80",
               &hints,
               &res);

        if (rc == NSN_EAI_INPROGRESS) {
            *chosen_out = candidates[i];
            return rc;
        }

        if (rc == 0 && res)
            native_free(res);
    }

    *chosen_out = NULL;
    return 0x7fffffff;
}

static int poll_native_async_to_end(
    raw_getaddrinfo_fn fn,
    raw_freeaddrinfo_fn native_free,
    const char *host,
    int initial_rc,
    int *attempts_out)
{
    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    int rc = initial_rc;
    int attempts = 0;

    OSTime deadline =
        OSGetTime() +
        OSMillisecondsToTicks(5000);

    while (rc == NSN_EAI_INPROGRESS &&
           OSGetTime() < deadline) {

        OSSleepTicks(
            OSMillisecondsToTicks(10));

        struct nsn_addrinfo *res = NULL;

        rc =
            fn(host,
               "80",
               &hints,
               &res);

        attempts++;

        if (rc == 0 && res)
            native_free(res);
    }

    if (attempts_out)
        *attempts_out = attempts;

    return rc;
}

static void run_dns_abort_probe(void)
{
    probe_say("%s", "");
    probe_say(
        "=== NATIVE DNS ABORT BY HNAME TEST ===");

    raw_getaddrinfo_fn async_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo_async");

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    raw_dns_abort_fn abort_fn =
        (raw_dns_abort_fn)find_export_addr(
            "dns_abort_by_hname");

    if (!async_fn ||
        !native_free ||
        !abort_fn) {

        probe_say(
            "DNS-ABORT missing native export");
        return;
    }

    /*
     * Several positive hostnames are provided because the native/IOSU
     * resolver may retain cache entries across earlier probe activity.
     *
     * We only use a hostname whose FIRST call returns EAI_INPROGRESS.
     */
    static const char *abort_candidates[] = {
        "www.rust-lang.org",
        "www.gentoo.org",
        "www.postgresql.org",
        "www.opensuse.org"
    };

    static const char *control_candidates[] = {
        "www.llvm.org",
        "www.python.org",
        "www.x.org",
        "www.apache.org"
    };

    const char *abort_host = NULL;

    int start_rc =
        start_fresh_native_async(
            async_fn,
            native_free,
            abort_candidates,
            sizeof(abort_candidates) /
                sizeof(abort_candidates[0]),
            &abort_host);

    if (start_rc != NSN_EAI_INPROGRESS ||
        !abort_host) {

        probe_say(
            "DNS-ABORT INCONCLUSIVE: no fresh abort hostname");

        probe_say(
            "=== END NATIVE DNS ABORT BY HNAME TEST ===");
        return;
    }

    probe_say(
        "DNS-ABORT started host=%s rc=%d",
        abort_host,
        start_rc);

    /*
     * Call abort immediately while the query is known to be pending.
     */
    OSTime abort_begin =
        OSGetTime();

    int abort_rc =
        abort_fn(abort_host);

    uint64_t abort_ms =
        OSTicksToMilliseconds(
            OSGetTime() - abort_begin);

    probe_say(
        "DNS-ABORT call host=%s rc=%d ms=%llu",
        abort_host,
        abort_rc,
        (unsigned long long)abort_ms);

    /*
     * No polling for 750 ms:
     *
     * - if abort failed to stop the request, its successful DNS answer
     *   should normally be in the resolver cache;
     *
     * - if abort really cancelled it, the next getaddrinfo_async should
     *   normally have to start again and return EAI_INPROGRESS.
     */
    OSSleepTicks(
        OSMillisecondsToTicks(750));

    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    OSTime poll_begin =
        OSGetTime();

    int after_abort =
        async_fn(
            abort_host,
            "80",
            &hints,
            &res);

    uint64_t poll_ms =
        OSTicksToMilliseconds(
            OSGetTime() - poll_begin);

    probe_say(
        "DNS-ABORT after 750ms rc=%d ms=%llu res=%08x",
        after_abort,
        (unsigned long long)poll_ms,
        (unsigned)(uintptr_t)res);

    if (after_abort == 0 && res) {
        native_free(res);
        res = NULL;
    }

    int abort_completion_attempts = 0;

    int abort_final =
        poll_native_async_to_end(
            async_fn,
            native_free,
            abort_host,
            after_abort,
            &abort_completion_attempts);

    probe_say(
        "DNS-ABORT final=%d extra_attempts=%d",
        abort_final,
        abort_completion_attempts);

    /*
     * Control: repeat the same experiment without dns_abort_by_hname.
     * A normal positive lookup should be complete after the same 750 ms.
     */
    const char *control_host = NULL;

    int control_start =
        start_fresh_native_async(
            async_fn,
            native_free,
            control_candidates,
            sizeof(control_candidates) /
                sizeof(control_candidates[0]),
            &control_host);

    if (control_start != NSN_EAI_INPROGRESS ||
        !control_host) {

        probe_say(
            "DNS-ABORT CONTROL INCONCLUSIVE: no fresh hostname");

        probe_say(
            "=== END NATIVE DNS ABORT BY HNAME TEST ===");
        return;
    }

    probe_say(
        "DNS-ABORT control started host=%s rc=%d",
        control_host,
        control_start);

    OSSleepTicks(
        OSMillisecondsToTicks(750));

    res = NULL;

    int control_after =
        async_fn(
            control_host,
            "80",
            &hints,
            &res);

    probe_say(
        "DNS-ABORT control after 750ms rc=%d res=%08x",
        control_after,
        (unsigned)(uintptr_t)res);

    if (control_after == 0 && res)
        native_free(res);

    int control_attempts = 0;

    int control_final =
        poll_native_async_to_end(
            async_fn,
            native_free,
            control_host,
            control_after,
            &control_attempts);

    probe_say(
        "DNS-ABORT control final=%d extra_attempts=%d",
        control_final,
        control_attempts);

    probe_say(
        "=== END NATIVE DNS ABORT BY HNAME TEST ===");
}


static void run_dns_abort_rc_matrix(void)
{
    probe_say("%s", "");
    probe_say(
        "=== NATIVE DNS ABORT RETURN MATRIX ===");

    raw_getaddrinfo_fn sync_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo");

    raw_getaddrinfo_fn async_fn =
        (raw_getaddrinfo_fn)find_export_addr(
            "getaddrinfo_async");

    raw_freeaddrinfo_fn native_free =
        (raw_freeaddrinfo_fn)find_export_addr(
            "freeaddrinfo");

    raw_dns_abort_fn abort_fn =
        (raw_dns_abort_fn)find_export_addr(
            "dns_abort_by_hname");

    if (!sync_fn ||
        !async_fn ||
        !native_free ||
        !abort_fn) {

        probe_say(
            "DNS-ABORT-MATRIX missing export");
        return;
    }

    /*
     * Case 1: hostname for which this probe never started a query.
     */
    const char *never_started =
        "ax88179-abort-never-started.invalid";

    int rc =
        abort_fn(never_started);

    probe_say(
        "DNS-ABORT-MATRIX no-request host=%s rc=%d",
        never_started,
        rc);

    /*
     * Case 2: hostname definitely resolved before abort().
     */
    const char *cached_host =
        "www.ietf.org";

    struct nsn_addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = 2;
    hints.ai_socktype = 1;
    hints.ai_protocol = 6;

    struct nsn_addrinfo *res = NULL;

    int populate_rc =
        sync_fn(
            cached_host,
            "80",
            &hints,
            &res);

    probe_say(
        "DNS-ABORT-MATRIX cached populate rc=%d res=%08x",
        populate_rc,
        (unsigned)(uintptr_t)res);

    if (populate_rc == 0 && res) {
        native_free(res);
        res = NULL;
    }

    rc =
        abort_fn(cached_host);

    probe_say(
        "DNS-ABORT-MATRIX cached host=%s abort_rc=%d",
        cached_host,
        rc);

    /*
     * Case 3: same hostname while getaddrinfo_async returned
     * EAI_INPROGRESS.
     */
    static const char *same_candidates[] = {
        "www.cmake.org",
        "www.perl.org",
        "www.sqlite.org",
        "www.netlib.org"
    };

    const char *same_host = NULL;

    int same_start =
        start_fresh_native_async(
            async_fn,
            native_free,
            same_candidates,
            sizeof(same_candidates) /
                sizeof(same_candidates[0]),
            &same_host);

    if (same_start == NSN_EAI_INPROGRESS &&
        same_host) {

        int same_abort =
            abort_fn(same_host);

        probe_say(
            "DNS-ABORT-MATRIX pending-same host=%s start=%d abort_rc=%d",
            same_host,
            same_start,
            same_abort);

        int attempts = 0;

        int final =
            poll_native_async_to_end(
                async_fn,
                native_free,
                same_host,
                same_start,
                &attempts);

        probe_say(
            "DNS-ABORT-MATRIX pending-same final=%d attempts=%d",
            final,
            attempts);
    } else {
        probe_say(
            "DNS-ABORT-MATRIX pending-same INCONCLUSIVE");
    }

    /*
     * Case 4:
     * Keep one request pending but ask abort() for another hostname
     * which has no corresponding request.
     */
    static const char *other_candidates[] = {
        "www.lua.org",
        "www.haskell.org",
        "www.ruby-lang.org",
        "www.php.net"
    };

    const char *other_host = NULL;

    int other_start =
        start_fresh_native_async(
            async_fn,
            native_free,
            other_candidates,
            sizeof(other_candidates) /
                sizeof(other_candidates[0]),
            &other_host);

    if (other_start == NSN_EAI_INPROGRESS &&
        other_host) {

        const char *wrong_host =
            "ax88179-abort-wrong-host.invalid";

        int other_abort =
            abort_fn(wrong_host);

        probe_say(
            "DNS-ABORT-MATRIX pending-other active=%s abort=%s start=%d abort_rc=%d",
            other_host,
            wrong_host,
            other_start,
            other_abort);

        int attempts = 0;

        int final =
            poll_native_async_to_end(
                async_fn,
                native_free,
                other_host,
                other_start,
                &attempts);

        probe_say(
            "DNS-ABORT-MATRIX pending-other final=%d attempts=%d",
            final,
            attempts);
    } else {
        probe_say(
            "DNS-ABORT-MATRIX pending-other INCONCLUSIVE");
    }

    probe_say(
        "=== END NATIVE DNS ABORT RETURN MATRIX ===");
}

static void run_shim_dns_probe(void)
{
    probe_say("%s", "");
    probe_say(
        "=== AX SHIM ASYNC DNS TEST ===");

    /*
     * Numeric address must complete immediately.
     */
    run_one_shim_poll(
        "async-num",
        RPLWRAP(getaddrinfo_async),
        "127.0.0.1");

    /*
     * Fresh hostnames exercise actual lwIP/AX DNS.
     */
    run_one_shim_poll(
        "async",
        RPLWRAP(getaddrinfo_async),
        "www.gnu.org");

    run_one_shim_poll(
        "async-rs",
        RPLWRAP(getaddrinfo_async_rs),
        "www.archlinux.org");

    /*
     * Sync _rs must behave exactly like normal getaddrinfo.
     */
    run_one_shim_poll(
        "sync-rs",
        RPLWRAP(getaddrinfo_rs),
        "www.freebsd.org");

    probe_say(
        "=== END AX SHIM ASYNC DNS TEST ===");
}

static void run_behavior_probe(void)
{
    memset(
        behavior_results,
        0,
        sizeof(behavior_results));

    memset(
        behavior_res_slots,
        0,
        sizeof(behavior_res_slots));

    atomic_store(&behavior_done, 0);

    probe_say("%s", "");
    probe_say(
        "=== CONTROLLED RAW GETADDRINFO TESTS ===");

    probe_say(
        "calls execute on worker; ProcUI stays alive");

    BOOL created =
        OSCreateThread(
            &behavior_thread,
            behavior_worker,
            0,
            NULL,
            behavior_stack + sizeof(behavior_stack),
            sizeof(behavior_stack),
            16,
            OS_THREAD_ATTRIB_AFFINITY_ANY);

    if (!created) {
        probe_say(
            "behavior worker OSCreateThread FAIL");
        return;
    }

    OSSetThreadName(
        &behavior_thread,
        "AX DNS ABI worker");

    OSResumeThread(
        &behavior_thread);

    while (!atomic_load(&behavior_done)) {
        probe_poll();

        OSSleepTicks(
            OSMillisecondsToTicks(10));
    }

    int thread_result = -1;

    OSJoinThread(
        &behavior_thread,
        &thread_result);

    probe_say(
        "behavior worker result=%d",
        thread_result);

    for (unsigned i = 0;
         i < BEHAVIOR_TEST_COUNT;
         i++) {

        struct behavior_result *r =
            &behavior_results[i];

        probe_say(
            "RAW-GAI %-12s node=%s rc=%d ms=%llu",
            r->label,
            r->node,
            r->rc,
            (unsigned long long)r->elapsed_ms);

        probe_say(
            "  res immediate=%08x delayed=%08x nodes=%d",
            (unsigned)r->immediate_res,
            (unsigned)r->delayed_res,
            r->nodes);

        if (r->nodes > 0) {
            char ip[32] = "?";

            struct in_addr addr = {
                .s_addr = r->first_addr
            };

            inet_ntop(
                AF_INET,
                &addr,
                ip,
                sizeof(ip));

            probe_say(
                "  first fam=%d type=%d proto=%d addr=%s:%u",
                r->first_family,
                r->first_socktype,
                r->first_protocol,
                ip,
                ntohs(r->first_port));
        }
    }

    probe_say(
        "=== END CONTROLLED RAW GETADDRINFO TESTS ===");
}

int main(void)
{
    if (probe_init("nsysnet DNS Async ABI Probe") != 0)
        return 1;

    probe_say(
        "Phase 1: passive native nsysnet ABI audit");

    probe_say(
        "Phase 2: controlled raw resolver calls");

    if (resolve_exports() != 0)
        goto wait;

    probe_say("%s", "");

    for (unsigned i = 0;
         i < sizeof(exports) / sizeof(exports[0]);
         i++) {

        dump_code(
            exports[i].name,
            exports[i].addr);

        /*
         * Keep ProcUI alive and make log ordering easy to read.
         */
        for (int n = 0; n < 3; n++) {
            if (!probe_poll())
                goto done;

            OSSleepTicks(
                OSMillisecondsToTicks(10));
        }
    }

    probe_say("%s", "");

    dump_words(
        "DNS_ABORT_BY_HNAME FULL",
        (uintptr_t)find_export_addr(
            "dns_abort_by_hname"),
        64);

    probe_say("%s", "");

    /*
     * All four public getaddrinfo wrappers observed so far call the
     * same internal routine. Find it dynamically from the native
     * getaddrinfo_async wrapper instead of hardcoding its address.
     */
    uintptr_t helper =
        find_first_direct_call(
            exports[1].addr);

    probe_say(
        "getaddrinfo_async first BL target=%08x",
        (unsigned)helper);

    if (helper) {
        dump_words(
            "GETADDRINFO COMMON HELPER",
            helper,
            HELPER_DUMP_WORDS);
    } else {
        probe_say(
            "COMMON HELPER NOT FOUND");
    }

    probe_say("%s", "");
    probe_say("DNS ASYNC HELPER DUMP COMPLETE");

    run_behavior_probe();

    run_async_poll_probe();

    run_clear_resolver_cache_probe();

    run_clear_pending_probe();

    run_clear_negative_probe();

    run_dns_abort_probe();

    run_dns_abort_rc_matrix();

    run_shim_dns_probe();

wait:
    probe_say("HOME -> Quitter");
    probe_wait();

done:
    if (g_nsysnet) {
        OSDynLoad_Release(g_nsysnet);
        g_nsysnet = NULL;
    }

    probe_shutdown();
    return 0;
}
