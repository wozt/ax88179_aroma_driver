#include "proc.h"

#include <coreinit/foreground.h>
#include <coreinit/systeminfo.h>
#include <coreinit/title.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <whb/log.h>

/* The titles a homebrew is launched INTO. A program running in one of
 * these does not own it and cannot simply end: it has to put the title
 * back. */
#define HBL_TITLE_ID           0x0005000013374842ULL
#define MII_MAKER_JPN_TITLE_ID 0x000500101004A000ULL
#define MII_MAKER_USA_TITLE_ID 0x000500101004A100ULL
#define MII_MAKER_EUR_TITLE_ID 0x000500101004A200ULL

static int g_running;
static int g_from_hbl;
static int g_exiting;

static uint32_t on_save(void *context)
{
    (void)context;
    OSSavesDone_ReadyToRelease();
    return 0;
}

/*
 * The system refusing to open the HOME overlay is our cue to quit.
 *
 * From a borrowed title the overlay is not allowed, so HOME otherwise
 * does nothing whatsoever -- which is precisely what was reported, and
 * what left the console needing the power button.
 */
static uint32_t on_home_denied(void *context)
{
    (void)context;
    proc_stop();
    return 0;
}

void proc_init(void)
{
    const uint64_t title = OSGetTitleID();
    g_from_hbl = (title == HBL_TITLE_ID || title == MII_MAKER_JPN_TITLE_ID ||
                  title == MII_MAKER_USA_TITLE_ID || title == MII_MAKER_EUR_TITLE_ID);
    WHBLogPrintf("proc: title %016llx, %s", (unsigned long long)title,
                 g_from_hbl ? "launched into a borrowed title" : "owns its title");

    OSEnableHomeButtonMenu(FALSE);
    g_running = 1;
    g_exiting = 0;
    ProcUIInitEx(&on_save, NULL);
    ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, &on_home_denied, NULL, 100);

}

int proc_running(void)
{
    if (!g_running) return 0;
    const ProcUIStatus status = ProcUIProcessMessages(TRUE);
    if (status == PROCUI_STATUS_EXITING) {
        g_running = 0;
        g_exiting = 1;
    } else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        /* The system wants the screen back. Saying so is what lets it
         * take it -- and what lets it be given back afterwards. */
        ProcUIDrawDoneRelease();
    }

    return g_running;
}

void proc_stop(void)
{
    /* Let main release lwIP/UHS and SDL before requesting title teardown. */
    g_running = 0;
}

void proc_shutdown(void)
{
    g_running = 0;
    if (!g_exiting) {
        if (g_from_hbl) SYSRelaunchTitle(0, NULL);
        else SYSLaunchMenu();
        /* Keep servicing the transition until Cafe OS acknowledges exit. */
        while (!g_exiting) {
            ProcUIStatus status = ProcUIProcessMessages(TRUE);
            if (status == PROCUI_STATUS_EXITING) g_exiting = 1;
            else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) ProcUIDrawDoneRelease();
            OSSleepTicks(OSMillisecondsToTicks(10));
        }
    }
    ProcUIShutdown();
}
