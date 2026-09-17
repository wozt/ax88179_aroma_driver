#ifndef WIIU_ETHERNET_PROBE_H
#define WIIU_ETHERNET_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A screen and a way out, for every experiment in this folder.
 *
 * The first probes only spoke over UDP, which meant reading results on
 * the PC and -- worse -- no way to leave without holding the power
 * button. Asked for directly, and right: a test that strands the console
 * costs more than it measures.
 *
 * So every probe here gets the same three things. Its output on the
 * television and the GamePad as well as over the network, HOME to
 * leave, and MINUS as well in case HOME is having one of its days.
 */

/* Brings up the screen. Returns 0, or -1 having already said why over
 * the log. */
int  probe_init(const char *title);

/* Says something, on screen and over UDP at once. */
void probe_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Keeps the result readable until the user leaves. Returns when they
 * do; the caller then just returns from main. */
void probe_wait(void);

void probe_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* WIIU_ETHERNET_PROBE_H */
