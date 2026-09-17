#ifndef CAPTURE2WIIU_UI_H
#define CAPTURE2WIIU_UI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Drawing and input, on SDL2.
 *
 * Not OSScreen, and that is the one architectural fact this client is
 * built around: measured on the console, once OSScreen has been up, GX2
 * renders nothing at all -- WHBGfxInit still succeeds, frames still
 * count, and the screen stays black. Everything that matters here is
 * GX2: the console's own keyboard, the HOME overlay, and eventually an
 * NV12 video texture. So the program is GX2 from its first line, and
 * SDL2's Wii U renderer -- which reports itself as "WiiU GX2",
 * accelerated -- is how it gets there without a shader assembler, which
 * this toolchain does not have.
 *
 * One window, mirrored to the television and the GamePad. Touch arrives
 * in this window's coordinates, so there is nothing left to guess about
 * which resolution the panel speaks -- a question that cost two rounds
 * against the raw VPAD API.
 */

#define UI_WIDTH  1280
#define UI_HEIGHT 720

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r, g, b, a;
} UiColour;

#define UI_BG       ((UiColour){ 0x10, 0x14, 0x1a, 0xFF })
#define UI_PANEL    ((UiColour){ 0x1c, 0x24, 0x30, 0xFF })
#define UI_FIELD    ((UiColour){ 0x26, 0x30, 0x3e, 0xFF })
#define UI_ACCENT   ((UiColour){ 0x2f, 0x6f, 0x4e, 0xFF })
#define UI_DANGER   ((UiColour){ 0x6f, 0x2f, 0x2f, 0xFF })
#define UI_TEXT     ((UiColour){ 0xEC, 0xF0, 0xF4, 0xFF })
#define UI_DIM      ((UiColour){ 0x8a, 0x95, 0xa3, 0xFF })

typedef struct {
    int quit;        /* the system asked, or the user did */
    int touch_x, touch_y;
    int touching;    /* a finger is down now */
    int tapped;      /* a finger was lifted this frame: act on this */
    uint32_t pressed;/* SDL_CONTROLLER_BUTTON_* mask of new presses */
} UiInput;

/* Brings up SDL, the window, the renderer and the console's own font.
 * Returns 0, or -1 with the reason in `why`. */
int  ui_init(char *why, size_t why_size);
void ui_shutdown(void);

/* One frame: poll, draw, show. */
void ui_poll(UiInput *in);
void ui_begin(void);
void ui_present(void);

/*
 * Pushes out everything SDL has queued, so raw GX2 drawing can happen
 * next and land on top rather than underneath.
 *
 * SDL's renderer batches; its own documentation says to call this
 * between its API and a lower-level graphics API. The console's
 * keyboard is exactly that -- it draws itself with GX2, inside our
 * frame, because SDL already owns GX2 and a second WHBGfxInit() simply
 * fails. Which it did, and said so on screen.
 */
void ui_flush(void);

void ui_fill(int x, int y, int w, int h, UiColour c);
/* A filled rectangle with a one-pixel border, for a field or a button. */
void ui_box(int x, int y, int w, int h, UiColour fill, UiColour border);

/* Text at a point, or centred in a rectangle. `size` is the point size;
 * two are cached, so anything else is rounded to the nearer of them. */
void ui_text(int x, int y, int size, UiColour c, const char *fmt, ...);
void ui_text_centred(int x, int y, int w, int h, int size, UiColour c, const char *text);

/* How wide a string would be, for laying out beside it. */
int  ui_text_width(int size, const char *text);

#define UI_SIZE_BODY  28
#define UI_SIZE_TITLE 44

#ifdef __cplusplus
}
#endif

#endif /* CAPTURE2WIIU_UI_H */
