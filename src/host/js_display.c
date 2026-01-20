/*
 * js_display.c - Shared display functions for Move Anything
 *
 * Provides display primitives used by both the main host and shadow UI.
 * Includes font loading (bitmap and TTF), glyph rendering, and QuickJS bindings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* STB implementations - must undefine after include to prevent double-inclusion
 * when js_display.h includes the same headers for type declarations */
#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION

#define STB_TRUETYPE_IMPLEMENTATION
#include "lib/stb_truetype.h"
#undef STB_TRUETYPE_IMPLEMENTATION

#include "js_display.h"

/* Screen buffer - shared across all display functions */
unsigned char js_display_screen_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

/* Dirty flag - set when screen changes */
int js_display_screen_dirty = 0;

/* Global font - loaded on first use */
static Font *g_font = NULL;

/* Mark screen as needing update */
static void mark_dirty(void) {
    js_display_screen_dirty = 1;
}

/* ============================================================================
 * Core Display Functions
 * ============================================================================ */

void js_display_clear(void) {
    memset(js_display_screen_buffer, 0, sizeof(js_display_screen_buffer));
    mark_dirty();
}

void js_display_set_pixel(int x, int y, int value) {
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT) {
        js_display_screen_buffer[y * DISPLAY_WIDTH + x] = value ? 1 : 0;
        mark_dirty();
    }
}

int js_display_get_pixel(int x, int y) {
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT) {
        return js_display_screen_buffer[y * DISPLAY_WIDTH + x] ? 1 : 0;
    }
    return 0;
}

void js_display_draw_rect(int x, int y, int w, int h, int value) {
    if (w <= 0 || h <= 0) return;
    for (int yi = y; yi < y + h; yi++) {
        js_display_set_pixel(x, yi, value);
        js_display_set_pixel(x + w - 1, yi, value);
    }
    for (int xi = x; xi < x + w; xi++) {
        js_display_set_pixel(xi, y, value);
        js_display_set_pixel(xi, y + h - 1, value);
    }
}

void js_display_fill_rect(int x, int y, int w, int h, int value) {
    if (w <= 0 || h <= 0) return;
    for (int yi = y; yi < y + h; yi++) {
        for (int xi = x; xi < x + w; xi++) {
            js_display_set_pixel(xi, yi, value);
        }
    }
}

void js_display_pack(uint8_t *dest) {
    int i = 0;
    for (int y = 0; y < DISPLAY_HEIGHT / 8; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            int index = (y * DISPLAY_WIDTH * 8) + x;
            unsigned char packed = 0;
            for (int j = 0; j < 8; j++) {
                int packIndex = index + j * DISPLAY_WIDTH;
                packed |= js_display_screen_buffer[packIndex] << j;
            }
            dest[i++] = packed;
        }
    }
}

/* ============================================================================
 * Font Loading
 * ============================================================================ */

Font* js_display_load_font(const char *filename, int charSpacing) {
    int width, height, n;
    unsigned char *image = stbi_load(filename, &width, &height, &n, 4);
    if (!image) {
        fprintf(stderr, "ERROR loading font: %s\n", filename);
        return NULL;
    }

    char charListFilename[256];
    snprintf(charListFilename, sizeof(charListFilename), "%s.dat", filename);
    FILE *f = fopen(charListFilename, "r");
    if (!f) {
        fprintf(stderr, "ERROR loading font charList from: %s\n", charListFilename);
        stbi_image_free(image);
        return NULL;
    }

    char charList[256];
    if (!fgets(charList, sizeof(charList), f)) {
        fclose(f);
        stbi_image_free(image);
        return NULL;
    }
    fclose(f);

    size_t numChars = strlen(charList);
    if (numChars > 0 && charList[numChars - 1] == '\n') {
        charList[--numChars] = '\0';
    }

    Font *out = malloc(sizeof(Font));
    if (!out) {
        stbi_image_free(image);
        return NULL;
    }
    memset(out, 0, sizeof(Font));
    out->charSpacing = charSpacing;
    out->is_ttf = 0;

    for (size_t i = 0; i < numChars; i++) {
        int charIndex = (int)(unsigned char)charList[i];
        int startX = -1, endX = -1;
        for (int x = 0; x < width; x++) {
            int idx = (x + (int)i * width) * 4;
            if (image[idx + 3] > 0) {
                if (startX == -1) startX = x;
                endX = x;
            }
        }
        if (startX == -1) continue;
        int charWidth = endX - startX + 1;
        FontChar fc = {0};
        fc.width = charWidth;
        fc.height = height;
        fc.data = malloc(charWidth * height);
        if (!fc.data) continue;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < charWidth; x++) {
                int idx = ((startX + x) + y * width) * 4;
                fc.data[y * charWidth + x] = image[idx + 3] > 0 ? 1 : 0;
            }
        }
        out->charData[charIndex] = fc;
    }

    stbi_image_free(image);
    printf("Loaded bitmap font: %s (%zu chars)\n", filename, numChars);
    return out;
}

Font* js_display_load_ttf_font(const char *filename, int pixel_height) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "ERROR loading TTF font: %s\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    if (fread(buffer, 1, size, f) != (size_t)size) {
        free(buffer);
        fclose(f);
        return NULL;
    }
    fclose(f);

    Font *out = malloc(sizeof(Font));
    if (!out) {
        free(buffer);
        return NULL;
    }
    memset(out, 0, sizeof(Font));
    out->is_ttf = 1;
    out->ttf_buffer = buffer;
    out->charSpacing = 1;

    if (!stbtt_InitFont(&out->ttf_info, buffer, 0)) {
        free(buffer);
        free(out);
        return NULL;
    }

    out->ttf_scale = stbtt_ScaleForPixelHeight(&out->ttf_info, pixel_height);
    out->ttf_height = pixel_height;
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&out->ttf_info, &ascent, &descent, &lineGap);
    out->ttf_ascent = (int)(ascent * out->ttf_scale);

    printf("Loaded TTF font: %s (height=%d)\n", filename, pixel_height);
    return out;
}

/* ============================================================================
 * Glyph Rendering
 * ============================================================================ */

int js_display_glyph_ttf(Font *fnt, char c, int sx, int sy, int color) {
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&fnt->ttf_info, c, &advance, &lsb);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&fnt->ttf_info, c, fnt->ttf_scale, fnt->ttf_scale, &x0, &y0, &x1, &y1);

    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) {
        return sx + (int)(advance * fnt->ttf_scale);
    }

    unsigned char *bitmap = malloc(w * h);
    if (!bitmap) {
        return sx + (int)(advance * fnt->ttf_scale);
    }

    stbtt_MakeCodepointBitmap(&fnt->ttf_info, bitmap, w, h, w, fnt->ttf_scale, fnt->ttf_scale, c);

    int draw_y = sy + fnt->ttf_ascent + y0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int val = bitmap[y * w + x];
            if (val > 64) {
                js_display_set_pixel(sx + x + x0, draw_y + y, color);
            }
        }
    }
    free(bitmap);

    return sx + (int)(advance * fnt->ttf_scale);
}

int js_display_glyph(Font *fnt, char c, int sx, int sy, int color) {
    FontChar fc = fnt->charData[(int)(unsigned char)c];
    if (!fc.data) return sx + fnt->charSpacing;
    for (int y = 0; y < fc.height; y++) {
        for (int x = 0; x < fc.width; x++) {
            if (fc.data[y * fc.width + x]) {
                js_display_set_pixel(sx + x, sy + y, color);
            }
        }
    }
    return sx + fc.width + fnt->charSpacing;
}

/* ============================================================================
 * Print Function
 * ============================================================================ */

void js_display_print(int x, int y, const char *string, int color) {
    if (!string) return;

    /* Lazy load font on first use */
    if (!g_font) {
        /* Try TTF first (available on Move hardware) */
        g_font = js_display_load_ttf_font("/opt/move/Fonts/unifont_jp-14.0.01.ttf", 12);
        if (!g_font) {
            /* Fall back to bitmap font */
            g_font = js_display_load_font("/data/UserData/move-anything/host/font.png", 1);
        }
    }
    if (!g_font) return;

    int cursor = x;
    for (size_t i = 0; i < strlen(string); i++) {
        if (g_font->is_ttf) {
            cursor = js_display_glyph_ttf(g_font, string[i], cursor, y, color);
        } else {
            cursor = js_display_glyph(g_font, string[i], cursor, y, color);
        }
    }
}

/* ============================================================================
 * QuickJS Bindings
 * ============================================================================ */

JSValue js_display_bind_set_pixel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    int x, y, value;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &value, argv[2])) return JS_UNDEFINED;
    js_display_set_pixel(x, y, value);
    return JS_UNDEFINED;
}

JSValue js_display_bind_draw_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 5) return JS_UNDEFINED;
    int x, y, w, h, value;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &value, argv[4])) return JS_UNDEFINED;
    js_display_draw_rect(x, y, w, h, value);
    return JS_UNDEFINED;
}

JSValue js_display_bind_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 5) return JS_UNDEFINED;
    int x, y, w, h, value;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &value, argv[4])) return JS_UNDEFINED;
    js_display_fill_rect(x, y, w, h, value);
    return JS_UNDEFINED;
}

JSValue js_display_bind_clear_screen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    js_display_clear();
    return JS_UNDEFINED;
}

JSValue js_display_bind_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 4) return JS_UNDEFINED;
    int x, y, color;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_UNDEFINED;
    const char *str = JS_ToCString(ctx, argv[2]);
    if (!str) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &color, argv[3])) {
        JS_FreeCString(ctx, str);
        return JS_UNDEFINED;
    }
    js_display_print(x, y, str, color);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

void js_display_register_bindings(JSContext *ctx, JSValue global_obj) {
    JS_SetPropertyStr(ctx, global_obj, "set_pixel",
        JS_NewCFunction(ctx, js_display_bind_set_pixel, "set_pixel", 3));
    JS_SetPropertyStr(ctx, global_obj, "draw_rect",
        JS_NewCFunction(ctx, js_display_bind_draw_rect, "draw_rect", 5));
    JS_SetPropertyStr(ctx, global_obj, "fill_rect",
        JS_NewCFunction(ctx, js_display_bind_fill_rect, "fill_rect", 5));
    JS_SetPropertyStr(ctx, global_obj, "clear_screen",
        JS_NewCFunction(ctx, js_display_bind_clear_screen, "clear_screen", 0));
    JS_SetPropertyStr(ctx, global_obj, "print",
        JS_NewCFunction(ctx, js_display_bind_print, "print", 4));
}
