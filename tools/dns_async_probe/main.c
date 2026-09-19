#include <arpa/inet.h>
#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
