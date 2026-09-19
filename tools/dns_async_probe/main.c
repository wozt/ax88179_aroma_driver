#include <coreinit/dynload.h>
#include <coreinit/thread.h>
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

int main(void)
{
    if (probe_init("nsysnet DNS Async ABI Probe") != 0)
        return 1;

    probe_say(
        "Passive ABI audit: NO undocumented function is called");

    probe_say(
        "Dumping native nsysnet export code only");

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
