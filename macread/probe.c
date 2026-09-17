#include "probe.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <whb/log.h>
#include <whb/log_udp.h>

#include "proc.h"
#include "ui.h"

#define LINES 22
#define LINE_LEN 110

static char g_lines[LINES][LINE_LEN];
static int  g_count;
static char g_title[64];
static int  g_up;

static void draw(void)
{
    ui_begin();
    ui_text(30, 20, UI_SIZE_TITLE, UI_TEXT, "%s", g_title);
    for (int i = 0; i < g_count; i++) {
        ui_text(30, 80 + i * 26, UI_SIZE_BODY, UI_DIM, "%s", g_lines[i]);
    }
    ui_text(30, UI_HEIGHT - 44, UI_SIZE_BODY, UI_TEXT, "HOME or MINUS to leave");
    ui_present();
}

int probe_init(const char *title)
{
    WHBLogUdpInit();
    snprintf(g_title, sizeof(g_title), "%s", title ? title : "probe");
    WHBLogPrintf("%s: starting", g_title);
    proc_init();

    char why[128];
    if (ui_init(why, sizeof(why)) != 0) {
        WHBLogPrintf("%s: no screen -- %s", g_title, why);
        return -1;
    }
    g_up = 1;
    draw();
    return 0;
}

void probe_say(const char *fmt, ...)
{
    char line[LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    WHBLogPrintf("%s", line);
    if (g_count < LINES) {
        snprintf(g_lines[g_count++], LINE_LEN, "%s", line);
    } else {
        /* Oldest out. A probe that says more than fits should say less,
         * but losing the end would be worse than losing the start. */
        memmove(g_lines[0], g_lines[1], sizeof(g_lines) - LINE_LEN);
        snprintf(g_lines[LINES - 1], LINE_LEN, "%s", line);
    }
    if (g_up) {
        draw();
    }
}

void probe_wait(void)
{
    UiInput in;
    memset(&in, 0, sizeof(in));
    while (proc_running()) {
        ui_poll(&in);
        /* proc.c turns a denied HOME into a quit; this covers the pad's
         * MINUS and anything SDL calls a quit. */
        if (in.quit) {
            proc_stop();
        }
        draw();
    }
}

void probe_shutdown(void)
{
    if (g_up) {
        ui_shutdown();
        g_up = 0;
    }
    WHBLogPrintf("%s: done", g_title);
    WHBLogUdpDeinit();
    proc_shutdown();
}
