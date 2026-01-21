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

#include "host/js_display.h"
#include "host/shadow_constants.h"

static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;
static shadow_ui_state_t *shadow_ui_state = NULL;
static shadow_param_t *shadow_param = NULL;

static int global_exit_flag = 0;
static uint8_t last_midi_ready = 0;
static FILE *shadow_ui_log = NULL;
static const char *shadow_ui_pid_path = "/data/UserData/move-anything/shadow_ui.pid";

/* Checksum helper for debug logging - unused in production */
static uint32_t shadow_ui_checksum(const unsigned char *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum * 33u) ^ buf[i];
    }
    return sum;
}

/* Display state - use shared buffer for packing */
static unsigned char packed_buffer[DISPLAY_BUFFER_SIZE];

static int open_shadow_shm(void) {
    int fd = shm_open(SHM_SHADOW_DISPLAY, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_display_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    fd = shm_open(SHM_SHADOW_UI_MIDI, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_ui_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

    fd = shm_open(SHM_SHADOW_PARAM, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_param = (shadow_param_t *)mmap(NULL, SHADOW_PARAM_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

static void shadow_ui_remove_pid(void) {
    unlink(shadow_ui_pid_path);
}

static void shadow_ui_write_pid(void) {
    FILE *pid_file = fopen(shadow_ui_pid_path, "w");
    if (!pid_file) {
        return;
    }
    fprintf(pid_file, "%d\n", (int)getpid());
    fclose(pid_file);
    atexit(shadow_ui_remove_pid);
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

/* shadow_set_focused_slot(slot) -> void
 * Updates the focused slot for knob CC routing without loading a patch.
 * Call this when navigating between slots in the UI.
 */
static JSValue js_shadow_set_focused_slot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int slot = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_UNDEFINED;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return JS_UNDEFINED;
    shadow_control->ui_slot = (uint8_t)slot;
    return JS_UNDEFINED;
}

/* shadow_get_ui_flags() -> int
 * Returns the UI flags from shared memory.
 */
static JSValue js_shadow_get_ui_flags(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->ui_flags);
}

/* shadow_clear_ui_flags(mask) -> void
 * Clears the specified flags from ui_flags.
 */
static JSValue js_shadow_clear_ui_flags(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int mask = 0;
    if (JS_ToInt32(ctx, &mask, argv[0])) return JS_UNDEFINED;
    shadow_control->ui_flags &= ~(uint8_t)mask;
    return JS_UNDEFINED;
}

/* shadow_get_selected_slot() -> int
 * Returns the track-selected slot (0-3) for playback/knobs.
 */
static JSValue js_shadow_get_selected_slot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->selected_slot);
}

/* shadow_get_ui_slot() -> int
 * Returns the UI-highlighted slot (0-3) set by shim for jump target.
 */
static JSValue js_shadow_get_ui_slot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->ui_slot);
}

/* shadow_get_shift_held() -> int
 * Returns 1 if shift button is currently held, 0 otherwise.
 */
static JSValue js_shadow_get_shift_held(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->shift_held);
}

/* shadow_request_exit() -> void
 * Request to exit shadow display mode and return to regular Move.
 */
static JSValue js_shadow_request_exit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (shadow_control) {
        shadow_control->display_mode = 0;
    }
    return JS_UNDEFINED;
}

/* shadow_set_param(slot, key, value) -> bool
 * Sets a parameter on the chain instance for the given slot.
 * Returns true on success, false on error.
 */
static JSValue js_shadow_set_param(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_param || argc < 3) return JS_FALSE;

    int slot = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_FALSE;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return JS_FALSE;

    const char *key = JS_ToCString(ctx, argv[1]);
    if (!key) return JS_FALSE;
    const char *value = JS_ToCString(ctx, argv[2]);
    if (!value) {
        JS_FreeCString(ctx, key);
        return JS_FALSE;
    }

    /* Copy key and value to shared memory */
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    strncpy(shadow_param->value, value, SHADOW_PARAM_VALUE_LEN - 1);
    shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);

    /* Set up request */
    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->request_type = 1;  /* SET */

    /* Wait for response with timeout */
    int timeout = 100;  /* ~100ms */
    while (!shadow_param->response_ready && timeout > 0) {
        usleep(1000);
        timeout--;
    }

    if (!shadow_param->response_ready || shadow_param->error) {
        return JS_FALSE;
    }

    return JS_TRUE;
}

/* shadow_get_param(slot, key) -> string or null
 * Gets a parameter from the chain instance for the given slot.
 * Returns the value as a string, or null on error.
 */
static JSValue js_shadow_get_param(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_param || argc < 2) return JS_NULL;

    int slot = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_NULL;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return JS_NULL;

    const char *key = JS_ToCString(ctx, argv[1]);
    if (!key) return JS_NULL;

    /* Copy key to shared memory */
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    shadow_param->value[0] = '\0';

    JS_FreeCString(ctx, key);

    /* Set up request */
    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->request_type = 2;  /* GET */

    /* Wait for response with timeout */
    int timeout = 100;  /* ~100ms */
    while (!shadow_param->response_ready && timeout > 0) {
        usleep(1000);
        timeout--;
    }

    if (!shadow_param->response_ready || shadow_param->error) {
        return JS_NULL;
    }

    return JS_NewString(ctx, shadow_param->value);
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

    /* Enable ES module imports (e.g., import { ... } from '../shared/constants.mjs') */
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

    JSValue global_obj = JS_GetGlobalObject(ctx);

    /* Register shared display bindings (set_pixel, draw_rect, fill_rect, clear_screen, print) */
    js_display_register_bindings(ctx, global_obj);

    /* Register shadow-specific bindings */
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_slots", JS_NewCFunction(ctx, js_shadow_get_slots, "shadow_get_slots", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_request_patch", JS_NewCFunction(ctx, js_shadow_request_patch, "shadow_request_patch", 2));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_focused_slot", JS_NewCFunction(ctx, js_shadow_set_focused_slot, "shadow_set_focused_slot", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_ui_flags", JS_NewCFunction(ctx, js_shadow_get_ui_flags, "shadow_get_ui_flags", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_clear_ui_flags", JS_NewCFunction(ctx, js_shadow_clear_ui_flags, "shadow_clear_ui_flags", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_selected_slot", JS_NewCFunction(ctx, js_shadow_get_selected_slot, "shadow_get_selected_slot", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_ui_slot", JS_NewCFunction(ctx, js_shadow_get_ui_slot, "shadow_get_ui_slot", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_shift_held", JS_NewCFunction(ctx, js_shadow_get_shift_held, "shadow_get_shift_held", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_request_exit", JS_NewCFunction(ctx, js_shadow_request_exit, "shadow_request_exit", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_param", JS_NewCFunction(ctx, js_shadow_set_param, "shadow_set_param", 3));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_param", JS_NewCFunction(ctx, js_shadow_get_param, "shadow_get_param", 2));
    JS_SetPropertyStr(ctx, global_obj, "exit", JS_NewCFunction(ctx, js_exit, "exit", 0));

    *prt = rt;
    *pctx = ctx;
}

static int process_shadow_midi(JSContext *ctx, JSValue *onInternal, JSValue *onExternal) {
    if (!shadow_ui_midi_shm) return 0;
    int handled = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = shadow_ui_midi_shm[i] & 0x0F;
        uint8_t cable = (shadow_ui_midi_shm[i] >> 4) & 0x0F;
        if (cin < 0x08 || cin > 0x0E) continue;
        uint8_t msg[3] = { shadow_ui_midi_shm[i + 1], shadow_ui_midi_shm[i + 2], shadow_ui_midi_shm[i + 3] };
        if (msg[0] + msg[1] + msg[2] == 0) continue;
        handled = 1;
        if (cable == 2) {
            callGlobalFunction(ctx, onExternal, msg);
        } else {
            callGlobalFunction(ctx, onInternal, msg);
        }
    }
    return handled;
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
    shadow_ui_write_pid();

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
            int had_event = process_shadow_midi(ctx, &JSonMidiMessageInternal, &JSonMidiMessageExternal);
            if (had_event && shadow_ui_midi_shm) {
                memset(shadow_ui_midi_shm, 0, MIDI_BUFFER_SIZE);
            }
        }

        refresh_counter++;
        if ((js_display_screen_dirty || (refresh_counter % 30 == 0)) && shadow_display_shm) {
            js_display_pack(packed_buffer);
            memcpy(shadow_display_shm, packed_buffer, DISPLAY_BUFFER_SIZE);
            js_display_screen_dirty = 0;
        }

        usleep(16000);
    }

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
