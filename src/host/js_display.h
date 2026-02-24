/*
 * js_display.h - Shared display functions for Move Anything
 *
 * This header provides display primitives used by both the main host
 * and shadow UI. Each host links against js_display.c.
 */

#ifndef JS_DISPLAY_H
#define JS_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include "quickjs.h"
#include "lib/stb_truetype.h"

/* Screen dimensions */
#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 64
#define DISPLAY_BUFFER_SIZE 1024  /* Packed: 128 * 64 / 8 */

/* Font character data */
typedef struct FontChar {
    unsigned char *data;
    int width;
    int height;
} FontChar;

/* Font structure (bitmap or TTF) */
typedef struct Font {
    int charSpacing;
    FontChar charData[256];
    int is_ttf;
    stbtt_fontinfo ttf_info;
    unsigned char *ttf_buffer;
    float ttf_scale;
    int ttf_ascent;
    int ttf_height;
} Font;

/* Screen buffer - defined in js_display.c */
extern unsigned char js_display_screen_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

/* Dirty flag - set when screen changes, host should check and clear */
extern int js_display_screen_dirty;

/* Core display functions */
void js_display_clear(void);
void js_display_set_pixel(int x, int y, int value);
int  js_display_get_pixel(int x, int y);
void js_display_draw_rect(int x, int y, int w, int h, int value);
void js_display_fill_rect(int x, int y, int w, int h, int value);
void js_display_print(int x, int y, const char *string, int color);
void js_display_pack(uint8_t *dest);

/* Font loading */
Font* js_display_load_font(const char *filename, int charSpacing);
Font* js_display_load_ttf_font(const char *filename, int pixel_height);

/* Glyph rendering */
int js_display_glyph(Font *fnt, char c, int sx, int sy, int color);
int js_display_glyph_ttf(Font *fnt, char c, int sx, int sy, int color);

/* QuickJS bindings - register with JS_SetPropertyStr */
JSValue js_display_bind_set_pixel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_display_bind_draw_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_display_bind_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_display_bind_clear_screen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_display_bind_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* Helper to register all display bindings on a JS global object */
void js_display_register_bindings(JSContext *ctx, JSValue global_obj);

#endif /* JS_DISPLAY_H */
