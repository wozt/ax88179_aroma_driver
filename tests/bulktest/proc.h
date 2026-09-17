#ifndef CAPTURE2WIIU_PROC_H
#define CAPTURE2WIIU_PROC_H

/*
 * Starting, running and -- the hard part -- leaving.
 *
 * libwhb's WHBProc* is not enough here, and the console proved it twice
 * by needing the power button held down. Two things it does not do:
 *
 * HOW YOU LEAVE DEPENDS ON HOW YOU WERE STARTED. A program launched
 * into the Homebrew Launcher's title -- which is what wiiload does, and
 * what Aroma's menu entries do -- has to call SYSRelaunchTitle() to get
 * back. SYSLaunchMenu(), which is right for a program that owns its own
 * title, leaves that one on a black screen for ever. That was the bug.
 *
 * AND THE HOME BUTTON HAS TO BE TAKEN OVER. From the Homebrew
 * Launcher's title the system refuses to open the HOME overlay, so
 * pressing HOME does nothing at all unless the program asks to be told
 * about the refusal and treats it as "quit". That is what
 * PROCUI_CALLBACK_HOME_BUTTON_DENIED is for.
 *
 * Modelled on moonlight-wiiu's src/wiiu/proc.c, which is itself based
 * on wut's libwhb and carries the same note about why it had to be
 * rewritten.
 */

#ifdef __cplusplus
extern "C" {
#endif

void proc_init(void);

/* Call once per frame. Returns 0 when it is time to stop, and does the
 * foreground handover the system asks for on the way. */
int  proc_running(void);

/* Ask to leave -- from a button, or a menu. */
void proc_stop(void);

/* Finish up. Does the relaunch when one is needed. */
void proc_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CAPTURE2WIIU_PROC_H */
