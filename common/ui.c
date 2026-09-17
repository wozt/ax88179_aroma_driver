#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <coreinit/memory.h>
#include <whb/log.h>

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static TTF_Font     *g_body;
static TTF_Font     *g_title;

/*
 * Rendered strings, kept.
 *
 * TTF_RenderUTF8_Blended builds a surface and a texture every time it is
 * called. A status screen redraws a dozen strings sixty times a second,
 * and almost all of them are the same strings as last frame -- so they
 * are kept until the text changes. Small and fixed: a menu does not have
 * unbounded text, and an unbounded cache on a console with no swap is a
 * leak with a nicer name.
 */
#define CACHE_SIZE 48
typedef struct {
    char        text[96];
    int         size;
    UiColour    colour;
    SDL_Texture *texture;
    int         w, h;
    uint32_t    used;   /* for evicting the least recently wanted */
} TextEntry;

static TextEntry g_cache[CACHE_SIZE];
static uint32_t  g_clock;

static SDL_Color to_sdl(UiColour c)
{
    SDL_Color s = { c.r, c.g, c.b, c.a };
    return s;
}

static TTF_Font *font_for(int size)
{
    return (size >= UI_SIZE_TITLE) ? g_title : g_body;
}

static TextEntry *entry_for(int size, UiColour colour, const char *text)
{
    TTF_Font *font = font_for(size);
    if (!font || !text || !text[0]) {
        return NULL;
    }

    TextEntry *oldest = &g_cache[0];
    for (int i = 0; i < CACHE_SIZE; i++) {
        TextEntry *e = &g_cache[i];
        if (e->texture && e->size == size && memcmp(&e->colour, &colour, sizeof(colour)) == 0 &&
            strncmp(e->text, text, sizeof(e->text) - 1) == 0) {
            e->used = ++g_clock;
            return e;
        }
        if (!e->texture) {
            oldest = e;
        } else if (oldest->texture && e->used < oldest->used) {
            oldest = e;
        }
    }

    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, to_sdl(colour));
    if (!surface) {
        return NULL;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    const int w = surface->w, h = surface->h;
    SDL_FreeSurface(surface);
    if (!texture) {
        return NULL;
    }

    if (oldest->texture) {
        SDL_DestroyTexture(oldest->texture);
    }
    snprintf(oldest->text, sizeof(oldest->text), "%s", text);
    oldest->size = size;
    oldest->colour = colour;
    oldest->texture = texture;
    oldest->w = w;
    oldest->h = h;
    oldest->used = ++g_clock;
    return oldest;
}

int ui_init(char *why, size_t why_size)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        snprintf(why, why_size, "SDL_Init: %s", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) {
        snprintf(why, why_size, "TTF_Init: %s", TTF_GetError());
        return -1;
    }

    g_window = SDL_CreateWindow("capture2cloud", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                UI_WIDTH, UI_HEIGHT, SDL_WINDOW_SHOWN);
    if (!g_window) {
        snprintf(why, why_size, "window: %s", SDL_GetError());
        return -1;
    }
    /*
     * PRESENTVSYNC, and it is not a nicety.
     *
     * Without it this loop runs as fast as the CPU allows, and two
     * things break at once. The console's keyboard is driven by how
     * often Calc() is called, so its animations race -- reported as "il
     * tourne super vite en accéléré". And VPADRead returns a sample
     * only sixty times a second, so nearly every iteration gets
     * VPAD_READ_NO_SAMPLES and the keyboard is handed no input at all,
     * which is why its touch did nothing.
     */
    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        snprintf(why, why_size, "renderer: %s", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

    /*
     * The console's own font, out of shared memory.
     *
     * Every Wii U has it, so nothing has to be shipped beside the .rpx
     * and nothing has to be found on the SD card at run time. It is the
     * same typeface the system menu uses, which is also why the result
     * looks like it belongs here.
     */
    void *font_data = NULL;
    uint32_t font_size = 0;
    if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font_data, &font_size) ||
        !font_data || font_size == 0) {
        snprintf(why, why_size, "the system font is not available");
        return -1;
    }
    WHBLogPrintf("ui: system font, %u bytes", (unsigned)font_size);

    /* Two RWops, because TTF_OpenFontRW takes ownership of the one it is
     * given and both fonts read the same block. */
    g_body = TTF_OpenFontRW(SDL_RWFromConstMem(font_data, (int)font_size), 1, UI_SIZE_BODY);
    g_title = TTF_OpenFontRW(SDL_RWFromConstMem(font_data, (int)font_size), 1, UI_SIZE_TITLE);
    if (!g_body || !g_title) {
        snprintf(why, why_size, "cannot open the system font: %s", TTF_GetError());
        return -1;
    }

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(g_renderer, &info) == 0) {
        WHBLogPrintf("ui: renderer '%s', accelerated=%d", info.name ? info.name : "?",
                     (info.flags & SDL_RENDERER_ACCELERATED) ? 1 : 0);
    }
    return 0;
}

void ui_shutdown(void)
{
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (g_cache[i].texture) {
            SDL_DestroyTexture(g_cache[i].texture);
            g_cache[i].texture = NULL;
        }
    }
    if (g_body) TTF_CloseFont(g_body);
    if (g_title) TTF_CloseFont(g_title);
    g_body = g_title = NULL;
    TTF_Quit();
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_renderer = NULL;
    g_window = NULL;
    SDL_Quit();
}

void ui_poll(UiInput *in)
{
    const int was_touching = in->touching;
    in->quit = 0;
    in->tapped = 0;
    in->pressed = 0;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            in->quit = 1;
            break;
        /* The panel arrives as BOTH a finger and a mouse. The mouse is
         * taken because it is already in this window's pixels; the
         * finger would need the same multiplication done by hand. */
        case SDL_MOUSEBUTTONDOWN:
            in->touch_x = e.button.x;
            in->touch_y = e.button.y;
            in->touching = 1;
            break;
        case SDL_MOUSEMOTION:
            in->touch_x = e.motion.x;
            in->touch_y = e.motion.y;
            break;
        case SDL_MOUSEBUTTONUP:
            in->touch_x = e.button.x;
            in->touch_y = e.button.y;
            in->touching = 0;
            break;
        case SDL_FINGERDOWN:
            in->touching = 1;
            break;
        case SDL_FINGERUP:
            in->touching = 0;
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            in->pressed |= (1u << e.cbutton.button);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(e.cdevice.which);
            break;
        default:
            break;
        }
    }

    /* Acted on the release, so a finger resting on a button is one
     * press and not sixty. */
    if (was_touching && !in->touching) {
        in->tapped = 1;
    }
}

void ui_begin(void)
{
    SDL_SetRenderDrawColor(g_renderer, UI_BG.r, UI_BG.g, UI_BG.b, 255);
    SDL_RenderClear(g_renderer);
}

void ui_present(void)
{
    SDL_RenderPresent(g_renderer);
}

void ui_flush(void)
{
    SDL_RenderFlush(g_renderer);
}

void ui_fill(int x, int y, int w, int h, UiColour c)
{
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(g_renderer, &r);
}

void ui_box(int x, int y, int w, int h, UiColour fill, UiColour border)
{
    ui_fill(x, y, w, h, fill);
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(g_renderer, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(g_renderer, &r);
}

void ui_text(int x, int y, int size, UiColour c, const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    TextEntry *e = entry_for(size, c, text);
    if (!e) {
        return;
    }
    SDL_Rect dst = { x, y, e->w, e->h };
    SDL_RenderCopy(g_renderer, e->texture, NULL, &dst);
}

void ui_text_centred(int x, int y, int w, int h, int size, UiColour c, const char *text)
{
    TextEntry *e = entry_for(size, c, text);
    if (!e) {
        return;
    }
    SDL_Rect dst = { x + (w - e->w) / 2, y + (h - e->h) / 2, e->w, e->h };
    SDL_RenderCopy(g_renderer, e->texture, NULL, &dst);
}

int ui_text_width(int size, const char *text)
{
    int w = 0, h = 0;
    TTF_Font *font = font_for(size);
    if (font && text && TTF_SizeUTF8(font, text, &w, &h) == 0) {
        return w;
    }
    return 0;
}
