#include "proc.h"

#include <coreinit/foreground.h>
#include <coreinit/systeminfo.h>
#include <coreinit/title.h>
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
    if (g_from_hbl) {
        proc_stop();
    }
    return 0;
}

void proc_init(void)
{
    const uint64_t title = OSGetTitleID();
    g_from_hbl = (title == HBL_TITLE_ID || title == MII_MAKER_JPN_TITLE_ID ||
                  title == MII_MAKER_USA_TITLE_ID || title == MII_MAKER_EUR_TITLE_ID);
    WHBLogPrintf("proc: title %016llx, %s", (unsigned long long)title,
                 g_from_hbl ? "launched into a borrowed title" : "owns its title");

    if (g_from_hbl) {
        /* Taken over rather than left to the system, so the denial
         * below is ours to act on. */
        OSEnableHomeButtonMenu(FALSE);
    }

    g_running = 1;
    ProcUIInitEx(&on_save, NULL);

    if (g_from_hbl) {
        ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, &on_home_denied, NULL, 100);
    }
}

int proc_running(void)
{
    const ProcUIStatus status = ProcUIProcessMessages(TRUE);
    if (status == PROCUI_STATUS_EXITING) {
        g_running = 0;
    } else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        /* The system wants the screen back. Saying so is what lets it
         * take it -- and what lets it be given back afterwards. */
        ProcUIDrawDoneRelease();
    }

    if (!g_running) {
        ProcUIShutdown();
    }
    return g_running;
}

void proc_stop(void)
{
    if (g_from_hbl) {
        /* Just stop; proc_shutdown() puts the title back. */
        g_running = 0;
    } else {
        SYSLaunchMenu();
    }
}

void proc_shutdown(void)
{
    g_running = 0;
    if (g_from_hbl) {
        /* The one call that actually returns a borrowed title. Without
         * it the console sits on a black screen until it is held down.
         */
        WHBLogPrintf("proc: SYSRelaunchTitle to give the title back");
        SYSRelaunchTitle(0, NULL);
    }
}
