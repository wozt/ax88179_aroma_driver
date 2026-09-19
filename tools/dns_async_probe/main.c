#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

#define DUMP_WORDS 24

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
    probe_say("DNS ASYNC CODE DUMP COMPLETE");

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
