/*
 * Shadow UI Host
 *
 * Minimal QuickJS runtime that renders a shadow UI into shared memory
 * while stock Move continues running. Input arrives via shadow MIDI shm.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "quickjs.h"
#include "quickjs-libc.h"

#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "lib/stb_truetype.h"

#define SHM_SHADOW_MIDI     "/move-shadow-midi"
#define SHM_SHADOW_DISPLAY  "/move-shadow-display"
#define SHM_SHADOW_CONTROL  "/move-shadow-control"
#define SHM_SHADOW_UI       "/move-shadow-ui"

#define MIDI_BUFFER_SIZE 256
#define DISPLAY_BUFFER_SIZE 1024
#define CONTROL_BUFFER_SIZE 64
#define SHADOW_UI_BUFFER_SIZE 512

#define SHADOW_UI_NAME_LEN 64
#define SHADOW_UI_SLOTS 4

typedef struct shadow_control_t {
    volatile uint8_t display_mode;
    volatile uint8_t shadow_ready;
    volatile uint8_t should_exit;
    volatile uint8_t midi_ready;
    volatile uint8_t write_idx;
    volatile uint8_t read_idx;
    volatile uint8_t ui_slot;
    volatile uint8_t ui_flags;
    volatile uint16_t ui_patch_index;
    volatile uint16_t reserved16;
    volatile uint32_t ui_request_id;
    volatile uint32_t shim_counter;
    volatile uint8_t reserved[44];
} shadow_control_t;

typedef struct shadow_ui_state_t {
    uint32_t version;
    uint8_t slot_count;
    uint8_t reserved[3];
    uint8_t slot_channels[SHADOW_UI_SLOTS];
    char slot_names[SHADOW_UI_SLOTS][SHADOW_UI_NAME_LEN];
} shadow_ui_state_t;

typedef char shadow_control_size_check[(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_ui_state_size_check[(sizeof(shadow_ui_state_t) <= SHADOW_UI_BUFFER_SIZE) ? 1 : -1];

static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;
static shadow_ui_state_t *shadow_ui_state = NULL;

static int global_exit_flag = 0;
static uint8_t last_midi_ready = 0;
static FILE *shadow_ui_log = NULL;
static uint32_t shadow_ui_tick_count = 0;

static uint32_t shadow_ui_checksum(const unsigned char *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum * 33u) ^ buf[i];
    }
    return sum;
}

typedef struct FontChar {
    unsigned char* data;
    int width;
    int height;
} FontChar;

typedef struct Font {
    int charSpacing;
    FontChar charData[128];
    int is_ttf;
    stbtt_fontinfo ttf_info;
    unsigned char* ttf_buffer;
    float ttf_scale;
    int ttf_ascent;
    int ttf_height;
} Font;

static Font* font = NULL;
static unsigned char screen_buffer[128 * 64];
static unsigned char packed_buffer[DISPLAY_BUFFER_SIZE];
static int screen_dirty = 0;

static void dirty_screen(void) {
    screen_dirty = 1;
}

static void clear_screen(void) {
    memset(screen_buffer, 0, sizeof(screen_buffer));
    dirty_screen();
}

static void set_pixel(int x, int y, int value) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) {
        screen_buffer[y * 128 + x] = value ? 1 : 0;
        dirty_screen();
    }
}

static int get_pixel(int x, int y) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) {
        return screen_buffer[y * 128 + x] ? 1 : 0;
    }
    return 0;
}

static void draw_rect(int x, int y, int w, int h, int value) {
    if (w <= 0 || h <= 0) return;
    for (int yi = y; yi < y + h; yi++) {
        set_pixel(x, yi, value);
        set_pixel(x + w - 1, yi, value);
    }
    for (int xi = x; xi < x + w; xi++) {
        set_pixel(xi, y, value);
        set_pixel(xi, y + h - 1, value);
    }
}

static void fill_rect(int x, int y, int w, int h, int value) {
    if (w <= 0 || h <= 0) return;
    for (int yi = y; yi < y + h; yi++) {
        for (int xi = x; xi < x + w; xi++) {
            set_pixel(xi, yi, value);
        }
    }
}

static Font* load_font(const char* filename, int charSpacing) {
    int width, height, n;
    unsigned char* image = stbi_load(filename, &width, &height, &n, 4);
    if (!image) {
        printf("ERROR loading font: %s\n", filename);
        return NULL;
    }

    char charListFilename[256];
    snprintf(charListFilename, sizeof(charListFilename), "%s.dat", filename);
    FILE* f = fopen(charListFilename, "r");
    if (!f) {
        printf("ERROR loading font charList from: %s\n", charListFilename);
        stbi_image_free(image);
        return NULL;
    }

    char charList[256];
    fgets(charList, sizeof(charList), f);
    fclose(f);

    int numChars = strlen(charList);
    Font* out = malloc(sizeof(Font));
    if (!out) {
        stbi_image_free(image);
        return NULL;
    }
    memset(out, 0, sizeof(Font));
    out->charSpacing = charSpacing;
    out->is_ttf = 0;

    for (int i = 0; i < numChars; i++) {
        int charIndex = (int)charList[i];
        int startX = -1, endX = -1;
        for (int x = 0; x < width; x++) {
            int idx = (x + i * width) * 4;
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
    printf("Loaded bitmap font: %s (%d chars)\n", filename, numChars);
    return out;
}

static Font* load_ttf_font(const char* filename, int pixel_height) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("ERROR loading TTF font: %s\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    fread(buffer, 1, size, f);
    fclose(f);

    Font* out = malloc(sizeof(Font));
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

static int glyph_ttf(Font* fnt, char c, int sx, int sy, int color) {
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&fnt->ttf_info, c, &advance, &lsb);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&fnt->ttf_info, c, fnt->ttf_scale, fnt->ttf_scale, &x0, &y0, &x1, &y1);

    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) {
        return sx + (int)(advance * fnt->ttf_scale);
    }

    unsigned char* bitmap = malloc(w * h);
    if (!bitmap) {
        return sx + (int)(advance * fnt->ttf_scale);
    }

    stbtt_MakeCodepointBitmap(&fnt->ttf_info, bitmap, w, h, w, fnt->ttf_scale, fnt->ttf_scale, c);

    int draw_y = sy + fnt->ttf_ascent + y0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int val = bitmap[y * w + x];
            if (val > 64) {
                set_pixel(sx + x + x0, draw_y + y, color);
            }
        }
    }
    free(bitmap);

    return sx + (int)(advance * fnt->ttf_scale);
}

static int glyph(Font* fnt, char c, int sx, int sy, int color) {
    FontChar fc = fnt->charData[(int)c];
    if (!fc.data) return sx + fnt->charSpacing;
    for (int y = 0; y < fc.height; y++) {
        for (int x = 0; x < fc.width; x++) {
            if (fc.data[y * fc.width + x]) {
                set_pixel(sx + x, sy + y, color);
            }
        }
    }
    return sx + fc.width + fnt->charSpacing;
}

static void print(int x, int y, const char* string, int color) {
    if (!string) return;
    if (!font) {
        font = load_ttf_font("/opt/move/Fonts/unifont_jp-14.0.01.ttf", 12);
        if (!font) {
            font = load_font("/data/UserData/move-anything/host/font.png", 1);
        }
    }
    if (!font) return;

    int cursor = x;
    for (size_t i = 0; i < strlen(string); i++) {
        if (font->is_ttf) {
            cursor = glyph_ttf(font, string[i], cursor, y, color);
        } else {
            cursor = glyph(font, string[i], cursor, y, color);
        }
    }
}

static void pack_screen(uint8_t *dest) {
    int i = 0;
    for (int y = 0; y < 64 / 8; y++) {
        for (int x = 0; x < 128; x++) {
            int index = (y * 128 * 8) + x;
            unsigned char packed = 0;
            for (int j = 0; j < 8; j++) {
                int packIndex = index + j * 128;
                packed |= screen_buffer[packIndex] << j;
            }
            dest[i++] = packed;
        }
    }
}

static int open_shadow_shm(void) {
    int fd = shm_open(SHM_SHADOW_DISPLAY, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_display_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    fd = shm_open(SHM_SHADOW_MIDI, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    fd = shm_open(SHM_SHADOW_CONTROL, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    fd = shm_open(SHM_SHADOW_UI, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_ui_state = (shadow_ui_state_t *)mmap(NULL, SHADOW_UI_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
    }

    return 0;
}

static void shadow_ui_log_line(const char *msg) {
    if (!shadow_ui_log) {
        shadow_ui_log = fopen("/data/UserData/move-anything/shadow_ui.log", "a");
    }
    if (shadow_ui_log) {
        fprintf(shadow_ui_log, "%s\n", msg);
        fflush(shadow_ui_log);
    }
}

static JSContext *JS_NewCustomContext(JSRuntime *rt) {
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return NULL;
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    return ctx;
}

static int eval_buf(JSContext *ctx, const void *buf, int buf_len,
                    const char *filename, int eval_flags) {
    JSValue val;
    int ret;
    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            js_module_set_import_meta(ctx, val, 1, 1);
            val = JS_EvalFunction(ctx, val);
        }
        val = js_std_await(ctx, val);
    } else {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    }
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *filename, int module) {
    uint8_t *buf;
    int ret, eval_flags = JS_EVAL_FLAG_STRICT;
    size_t buf_len;

    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        perror(filename);
        return -1;
    }

    if (module) eval_flags |= JS_EVAL_TYPE_MODULE;
    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags);
    js_free(ctx, buf);
    return ret;
}

static int getGlobalFunction(JSContext *ctx, const char *func_name, JSValue *retFunc) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue func = JS_GetPropertyStr(ctx, global_obj, func_name);
    if (!JS_IsFunction(ctx, func)) {
        JS_FreeValue(ctx, func);
        return 0;
    }
    *retFunc = func;
    return 1;
}

static int callGlobalFunction(JSContext *ctx, JSValue *pfunc, unsigned char *data) {
    JSValue ret;
    if (data) {
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < 3; i++) {
            JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, data[i]));
        }
        JSValue args[1] = { arr };
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 1, args);
    } else {
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 0, 0);
    }
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    }
    JS_FreeValue(ctx, ret);
    return JS_IsException(ret);
}

static JSValue js_set_pixel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int x, y, value;
    if (argc < 2) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;
    value = 1;
    if (argc >= 3 && JS_ToInt32(ctx, &value, argv[2])) return JS_EXCEPTION;
    set_pixel(x, y, value);
    return JS_UNDEFINED;
}

static JSValue js_draw_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int x, y, w, h, color = 1;
    if (argc < 4) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_EXCEPTION;
    if (argc >= 5 && JS_ToInt32(ctx, &color, argv[4])) return JS_EXCEPTION;
    draw_rect(x, y, w, h, color);
    return JS_UNDEFINED;
}

static JSValue js_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int x, y, w, h, color = 1;
    if (argc < 4) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_EXCEPTION;
    if (argc >= 5 && JS_ToInt32(ctx, &color, argv[4])) return JS_EXCEPTION;
    fill_rect(x, y, w, h, color);
    return JS_UNDEFINED;
}

static JSValue js_clear_screen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    clear_screen();
    return JS_UNDEFINED;
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 3) return JS_EXCEPTION;
    int x, y, color = 1;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;
    JSValue string_val = JS_ToString(ctx, argv[2]);
    const char* string = JS_ToCString(ctx, string_val);
    if (argc >= 4 && JS_ToInt32(ctx, &color, argv[3])) {
        JS_FreeValue(ctx, string_val);
        JS_FreeCString(ctx, string);
        return JS_EXCEPTION;
    }
    print(x, y, string, color);
    JS_FreeValue(ctx, string_val);
    JS_FreeCString(ctx, string);
    return JS_UNDEFINED;
}

static JSValue js_shadow_get_slots(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_ui_state) return JS_NULL;
    JSValue arr = JS_NewArray(ctx);
    int count = shadow_ui_state->slot_count;
    if (count <= 0 || count > SHADOW_UI_SLOTS) count = SHADOW_UI_SLOTS;
    for (int i = 0; i < count; i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "channel", JS_NewInt32(ctx, shadow_ui_state->slot_channels[i]));
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, shadow_ui_state->slot_names[i]));
        JS_SetPropertyUint32(ctx, arr, i, obj);
    }
    return arr;
}

static JSValue js_shadow_request_patch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_FALSE;
    int slot = 0;
    int patch = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_FALSE;
    if (JS_ToInt32(ctx, &patch, argv[1])) return JS_FALSE;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return JS_FALSE;
    if (patch < 0) return JS_FALSE;
    shadow_control->ui_slot = (uint8_t)slot;
    shadow_control->ui_patch_index = (uint16_t)patch;
    shadow_control->ui_request_id++;
    return JS_TRUE;
}

static JSValue js_exit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    global_exit_flag = 1;
    return JS_UNDEFINED;
}

static void init_javascript(JSRuntime **prt, JSContext **pctx) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) exit(2);
    js_std_set_worker_new_context_func(JS_NewCustomContext);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewCustomContext(rt);
    if (!ctx) exit(2);
    js_std_add_helpers(ctx, -1, 0);

    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "set_pixel", JS_NewCFunction(ctx, js_set_pixel, "set_pixel", 3));
    JS_SetPropertyStr(ctx, global_obj, "draw_rect", JS_NewCFunction(ctx, js_draw_rect, "draw_rect", 5));
    JS_SetPropertyStr(ctx, global_obj, "fill_rect", JS_NewCFunction(ctx, js_fill_rect, "fill_rect", 5));
    JS_SetPropertyStr(ctx, global_obj, "clear_screen", JS_NewCFunction(ctx, js_clear_screen, "clear_screen", 0));
    JS_SetPropertyStr(ctx, global_obj, "print", JS_NewCFunction(ctx, js_print, "print", 4));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_slots", JS_NewCFunction(ctx, js_shadow_get_slots, "shadow_get_slots", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_request_patch", JS_NewCFunction(ctx, js_shadow_request_patch, "shadow_request_patch", 2));
    JS_SetPropertyStr(ctx, global_obj, "exit", JS_NewCFunction(ctx, js_exit, "exit", 0));

    *prt = rt;
    *pctx = ctx;
}

static void process_shadow_midi(JSContext *ctx, JSValue *onInternal, JSValue *onExternal) {
    if (!shadow_midi_shm) return;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = shadow_midi_shm[i] & 0x0F;
        uint8_t cable = (shadow_midi_shm[i] >> 4) & 0x0F;
        if (cin < 0x08 || cin > 0x0E) continue;
        uint8_t msg[3] = { shadow_midi_shm[i + 1], shadow_midi_shm[i + 2], shadow_midi_shm[i + 3] };
        if (msg[0] + msg[1] + msg[2] == 0) continue;
        if (cable == 0) {
            callGlobalFunction(ctx, onInternal, msg);
        } else if (cable == 2) {
            callGlobalFunction(ctx, onExternal, msg);
        }
    }
}

int main(int argc, char *argv[]) {
    const char *script = "/data/UserData/move-anything/shadow/shadow_ui.js";
    if (argc > 1) {
        script = argv[1];
    }

    if (open_shadow_shm() != 0) {
        fprintf(stderr, "shadow_ui: failed to open shared memory\n");
        return 1;
    }
    shadow_ui_log_line("shadow_ui: shared memory open");

    JSRuntime *rt = NULL;
    JSContext *ctx = NULL;
    init_javascript(&rt, &ctx);

    if (eval_file(ctx, script, 1) != 0) {
        fprintf(stderr, "shadow_ui: failed to load %s\n", script);
        shadow_ui_log_line("shadow_ui: failed to load script");
        return 1;
    }
    shadow_ui_log_line("shadow_ui: script loaded");

    JSValue JSonMidiMessageInternal;
    JSValue JSonMidiMessageExternal;
    JSValue JSinit;
    JSValue JSTick;

    if (!getGlobalFunction(ctx, "onMidiMessageInternal", &JSonMidiMessageInternal)) {
        shadow_ui_log_line("shadow_ui: onMidiMessageInternal missing");
    }
    if (!getGlobalFunction(ctx, "onMidiMessageExternal", &JSonMidiMessageExternal)) {
        shadow_ui_log_line("shadow_ui: onMidiMessageExternal missing");
    }
    if (!getGlobalFunction(ctx, "init", &JSinit)) {
        shadow_ui_log_line("shadow_ui: init missing");
    }
    int jsTickIsDefined = getGlobalFunction(ctx, "tick", &JSTick);
    if (!jsTickIsDefined) {
        shadow_ui_log_line("shadow_ui: tick missing");
    }

    callGlobalFunction(ctx, &JSinit, 0);
    shadow_ui_log_line("shadow_ui: init called");

    int refresh_counter = 0;
    while (!global_exit_flag) {
        if (shadow_control && shadow_control->should_exit) {
            break;
        }

        if (jsTickIsDefined) {
            callGlobalFunction(ctx, &JSTick, 0);
        }

        if (shadow_control && shadow_control->midi_ready != last_midi_ready) {
            last_midi_ready = shadow_control->midi_ready;
            process_shadow_midi(ctx, &JSonMidiMessageInternal, &JSonMidiMessageExternal);
        }

        shadow_ui_tick_count++;
        refresh_counter++;
        if ((screen_dirty || (refresh_counter % 30 == 0)) && shadow_display_shm) {
            pack_screen(packed_buffer);
            memcpy(shadow_display_shm, packed_buffer, DISPLAY_BUFFER_SIZE);
            screen_dirty = 0;
            if ((shadow_ui_tick_count % 120) == 0) {
                char msg[160];
                uint32_t screen_sum = shadow_ui_checksum(screen_buffer, sizeof(screen_buffer));
                uint32_t packed_sum = shadow_ui_checksum(packed_buffer, DISPLAY_BUFFER_SIZE);
                uint32_t shm_sum = shadow_ui_checksum(shadow_display_shm, DISPLAY_BUFFER_SIZE);
                snprintf(msg, sizeof(msg),
                         "shadow_ui: tick=%u screen_sum=%u packed_sum=%u shm_sum=%u",
                         shadow_ui_tick_count, screen_sum, packed_sum, shm_sum);
                shadow_ui_log_line(msg);
            }
        }

        usleep(16000);
    }

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
