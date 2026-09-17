#include <coreinit/thread.h>
#include <whb/log.h>
#include "probe.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("AX Safe Exit v9 - HOME menu quit") != 0) {
        return 1;
    }

    int frame = 0;
    probe_say("Open HOME menu, choose Quitter");
    probe_say("No auto-exit, no forced SYSLaunchMenu");
    while (probe_poll()) {
        if (++frame % 300 == 0) {
            probe_say("ax_safe_exit9: alive frame=%d", frame);
        }
        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    probe_shutdown();
    return 0;
}
