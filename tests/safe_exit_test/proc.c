#include "proc.h"

#include <coreinit/foreground.h>
#include <proc_ui/procui.h>
#include <whb/log.h>

static int g_running;
static int g_last_status = -1;

static uint32_t on_save(void *context)
{
    (void)context;
    OSSavesDone_ReadyToRelease();
    return 0;
}

void proc_init(void)
{
    WHBLogPrintf("proc: normal HOME overlay mode");
    g_running = 1;
    g_last_status = -1;
    ProcUIInitEx(&on_save, NULL);
}

int proc_running(void)
{
    const ProcUIStatus status = ProcUIProcessMessages(TRUE);
    if ((int)status != g_last_status) {
        WHBLogPrintf("AXPROBE ProcUI: status=%d", (int)status);
        g_last_status = status;
    }
    if (status == PROCUI_STATUS_EXITING) {
        g_running = 0;
    } else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        ProcUIDrawDoneRelease();
    }
    if (!g_running) {
        WHBLogPrintf("AXPROBE ProcUI: shutdown begin");
        ProcUIShutdown();
        WHBLogPrintf("AXPROBE ProcUI: shutdown end");
    }
    return g_running;
}

void proc_stop(void)
{
    WHBLogPrintf("AXPROBE ProcUI: proc_stop ignored in HOME overlay mode");
}

void proc_shutdown(void)
{
    WHBLogPrintf("proc: shutdown complete, returning from main");
}
