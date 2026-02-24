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
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>

#include "quickjs.h"
#include "quickjs-libc.h"

#include "host/js_display.h"
#include "host/shadow_constants.h"
#include "../host/unified_log.h"

static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;
static shadow_ui_state_t *shadow_ui_state = NULL;
static shadow_param_t *shadow_param = NULL;
static shadow_midi_out_t *shadow_midi_out = NULL;
static shadow_midi_dsp_t *shadow_midi_dsp = NULL;
static shadow_screenreader_t *shadow_screenreader = NULL;
static shadow_overlay_state_t *shadow_overlay = NULL;

static int global_exit_flag = 0;
static uint8_t last_midi_ready = 0;
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
    if (shadow_display_shm == MAP_FAILED) { shadow_display_shm = NULL; return -1; }

    fd = shm_open(SHM_SHADOW_UI_MIDI, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_ui_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shadow_ui_midi_shm == MAP_FAILED) { shadow_ui_midi_shm = NULL; return -1; }

    fd = shm_open(SHM_SHADOW_CONTROL, O_RDWR, 0666);
    if (fd < 0) return -1;
    shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shadow_control == MAP_FAILED) { shadow_control = NULL; return -1; }

    fd = shm_open(SHM_SHADOW_UI, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_ui_state = (shadow_ui_state_t *)mmap(NULL, SHADOW_UI_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shadow_ui_state == MAP_FAILED) shadow_ui_state = NULL;
    }

    fd = shm_open(SHM_SHADOW_PARAM, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_param = (shadow_param_t *)mmap(NULL, SHADOW_PARAM_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shadow_param == MAP_FAILED) shadow_param = NULL;
    }

    fd = shm_open(SHM_SHADOW_MIDI_OUT, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_midi_out = (shadow_midi_out_t *)mmap(NULL, sizeof(shadow_midi_out_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shadow_midi_out == MAP_FAILED) shadow_midi_out = NULL;
    }

    fd = shm_open(SHM_SHADOW_MIDI_DSP, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_midi_dsp = (shadow_midi_dsp_t *)mmap(NULL, sizeof(shadow_midi_dsp_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shadow_midi_dsp == MAP_FAILED) shadow_midi_dsp = NULL;
    }

    fd = shm_open(SHM_SHADOW_SCREENREADER, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_screenreader = (shadow_screenreader_t *)mmap(NULL, sizeof(shadow_screenreader_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shadow_screenreader == MAP_FAILED) {
            shadow_screenreader = NULL;
        } else {
            unified_log("shadow_ui", LOG_LEVEL_DEBUG, "Shadow screen reader shm mapped: %p", shadow_screenreader);
        }
    }

    fd = shm_open(SHM_SHADOW_OVERLAY, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_overlay = (shadow_overlay_state_t *)mmap(NULL, SHADOW_OVERLAY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shadow_overlay == MAP_FAILED) {
            shadow_overlay = NULL;
        } else {
            unified_log("shadow_ui", LOG_LEVEL_DEBUG, "Shadow overlay shm mapped: %p", shadow_overlay);
        }
    }

    return 0;
}

static void shadow_ui_log_line(const char *msg) {
    /* Use unified log instead of separate shadow_ui.log */
    unified_log("shadow_ui", LOG_LEVEL_DEBUG, "%s", msg);
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
        JS_FreeValue(ctx, global_obj);
        return 0;
    }
    *retFunc = func;
    JS_FreeValue(ctx, global_obj);
    return 1;
}

static int callGlobalFunction(JSContext *ctx, JSValue *pfunc, unsigned char *data) {
    JSValue ret;
    int is_exception;
    if (data) {
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < 3; i++) {
            JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, data[i]));
        }
        JSValue args[1] = { arr };
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, arr);
    } else {
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 0, 0);
    }
    is_exception = JS_IsException(ret);
    if (is_exception) {
        js_std_dump_error(ctx);
    }
    JS_FreeValue(ctx, ret);
    return is_exception;
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

/* shadow_get_display_mode() -> int
 * Returns 0 if Move's UI is visible, 1 if Shadow UI is visible
 */
static JSValue js_shadow_get_display_mode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->display_mode);
}

/* shadow_set_overtake_mode(mode) -> void
 * Set overtake mode: 1=block all MIDI from reaching Move, 0=normal.
 */
static JSValue js_shadow_set_overtake_mode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t mode = 0;
    JS_ToInt32(ctx, &mode, argv[0]);
    shadow_control->overtake_mode = (uint8_t)mode;  /* 0=normal, 1=menu, 2=module */
    /* Reset MIDI sync and clear buffer when enabling overtake mode */
    if (mode != 0) {
        last_midi_ready = shadow_control->midi_ready;
        /* Clear MIDI buffer to start fresh */
        if (shadow_ui_midi_shm) {
            memset(shadow_ui_midi_shm, 0, MIDI_BUFFER_SIZE);
        }
    }
    return JS_UNDEFINED;
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

/* shadow_control_restart() -> void
 * Signal the shim to restart Move (e.g. after a core update) */
static JSValue js_shadow_control_restart(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (shadow_control) {
        shadow_control->restart_move = 1;
    }
    return JS_UNDEFINED;
}

/* shadow_load_ui_module(path) -> bool
 * Loads and evaluates a JS file (typically ui_chain.js) in the current context.
 * The loaded module can set globalThis.chain_ui to provide init/tick/onMidi functions.
 * Returns true on success, false on error.
 *
 * Uses a unique module name (path#N) for each load to bypass QuickJS's module
 * cache. This ensures overtake modules get fresh code on every launch and
 * picks up on-disk changes without restarting shadow_ui.
 * Relative imports still resolve correctly since QuickJS uses the dirname.
 */
static int shadow_ui_module_load_counter = 0;

static JSValue js_shadow_load_ui_module(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    shadow_ui_log_line("Loading UI module:");
    shadow_ui_log_line(path);

    /* Read the file from disk */
    size_t buf_len;
    uint8_t *buf = js_load_file(ctx, &buf_len, path);
    if (!buf) {
        perror(path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Create a unique module name to bypass QuickJS module cache */
    char module_name[512];
    snprintf(module_name, sizeof(module_name), "%s#%d", path, ++shadow_ui_module_load_counter);
    JS_FreeCString(ctx, path);

    int eval_flags = JS_EVAL_FLAG_STRICT | JS_EVAL_TYPE_MODULE;
    int ret = eval_buf(ctx, buf, buf_len, module_name, eval_flags);
    js_free(ctx, buf);

    return ret == 0 ? JS_TRUE : JS_FALSE;
}

#define SHADOW_PARAM_POLL_US 200
#define SHADOW_PARAM_DEFAULT_TIMEOUT_MS 100

static uint32_t shadow_param_request_seq = 0;

static int shadow_param_timeout_to_polls(int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = SHADOW_PARAM_DEFAULT_TIMEOUT_MS;
    long total_us = (long)timeout_ms * 1000L;
    int polls = (int)(total_us / SHADOW_PARAM_POLL_US);
    if (polls < 1) polls = 1;
    return polls;
}

static uint32_t shadow_param_next_request_id(void) {
    shadow_param_request_seq++;
    if (shadow_param_request_seq == 0) {
        shadow_param_request_seq = 1;
    }
    return shadow_param_request_seq;
}

static int shadow_param_wait_idle(int timeout_ms) {
    int timeout = shadow_param_timeout_to_polls(timeout_ms);
    while (shadow_param->request_type != 0 && timeout > 0) {
        usleep(SHADOW_PARAM_POLL_US);
        timeout--;
    }
    return shadow_param->request_type == 0;
}

static int shadow_param_wait_response(uint32_t req_id, int timeout_ms) {
    int timeout = shadow_param_timeout_to_polls(timeout_ms);
    while (timeout > 0) {
        if (shadow_param->response_ready && shadow_param->response_id == req_id) {
            return shadow_param->error ? -1 : 1;
        }
        usleep(SHADOW_PARAM_POLL_US);
        timeout--;
    }
    return 0;
}

static int shadow_set_param_common(int slot, const char *key, const char *value, int timeout_ms) {
    const int overtake_fire_and_forget = (shadow_control && shadow_control->overtake_mode >= 2);

    if (!overtake_fire_and_forget) {
        if (!shadow_param_wait_idle(timeout_ms)) {
            return 0;
        }
    }

    uint32_t req_id = shadow_param_next_request_id();

    /* Copy key and value to shared memory */
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    strncpy(shadow_param->value, value, SHADOW_PARAM_VALUE_LEN - 1);
    shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';

    /* Set up request */
    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->response_id = 0;
    shadow_param->request_id = req_id;
    shadow_param->request_type = 1;  /* SET */

    /* In overtake module mode, keep this fire-and-forget so rapid encoder
     * streams do not block UI rendering. */
    if (overtake_fire_and_forget) {
        return 1;
    }

    return shadow_param_wait_response(req_id, timeout_ms) > 0;
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

    int ok = shadow_set_param_common(slot, key, value, SHADOW_PARAM_DEFAULT_TIMEOUT_MS);

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);

    return ok ? JS_TRUE : JS_FALSE;
}

/* shadow_set_param_timeout(slot, key, value, timeout_ms) -> bool
 * Timeout-aware variant used by slower operations like load_file.
 */
static JSValue js_shadow_set_param_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_param || argc < 4) return JS_FALSE;

    int slot = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_FALSE;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return JS_FALSE;

    int32_t timeout_ms = SHADOW_PARAM_DEFAULT_TIMEOUT_MS;
    if (JS_ToInt32(ctx, &timeout_ms, argv[3])) return JS_FALSE;
    if (timeout_ms <= 0) timeout_ms = SHADOW_PARAM_DEFAULT_TIMEOUT_MS;

    const char *key = JS_ToCString(ctx, argv[1]);
    if (!key) return JS_FALSE;
    const char *value = JS_ToCString(ctx, argv[2]);
    if (!value) {
        JS_FreeCString(ctx, key);
        return JS_FALSE;
    }

    int ok = shadow_set_param_common(slot, key, value, (int)timeout_ms);

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);

    return ok ? JS_TRUE : JS_FALSE;
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

    if (!shadow_param_wait_idle(SHADOW_PARAM_DEFAULT_TIMEOUT_MS)) {
        JS_FreeCString(ctx, key);
        return JS_NULL;
    }

    uint32_t req_id = shadow_param_next_request_id();

    /* Copy key to shared memory */
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    /* Clear entire value buffer to prevent any stale data */
    memset(shadow_param->value, 0, SHADOW_PARAM_VALUE_LEN);

    JS_FreeCString(ctx, key);

    /* Set up request */
    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->response_id = 0;
    shadow_param->request_id = req_id;
    shadow_param->request_type = 2;  /* GET */

    if (shadow_param_wait_response(req_id, SHADOW_PARAM_DEFAULT_TIMEOUT_MS) <= 0) {
        return JS_NULL;
    }

    return JS_NewString(ctx, shadow_param->value);
}

/* === MIDI output functions for overtake modules === */

/* Common implementation for sending MIDI via shared memory */
static JSValue js_shadow_midi_send(int cable, JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_midi_out) return JS_FALSE;
    if (argc < 1) return JS_FALSE;

    JSValueConst arr = argv[0];
    if (!JS_IsArray(ctx, arr)) return JS_FALSE;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    /* Process 4 bytes at a time (USB-MIDI packet format) */
    for (int i = 0; i < len; i += 4) {
        uint8_t packet[4] = {0, 0, 0, 0};

        for (int j = 0; j < 4 && (i + j) < len; j++) {
            JSValue elem = JS_GetPropertyUint32(ctx, arr, i + j);
            int32_t val = 0;
            JS_ToInt32(ctx, &val, elem);
            JS_FreeValue(ctx, elem);
            packet[j] = (uint8_t)(val & 0xFF);
        }

        /* Override cable number in CIN byte */
        packet[0] = (packet[0] & 0x0F) | (cable << 4);

        /* Find space in buffer and write */
        int write_offset = shadow_midi_out->write_idx;
        if (write_offset + 4 <= SHADOW_MIDI_OUT_BUFFER_SIZE) {
            memcpy(&shadow_midi_out->buffer[write_offset], packet, 4);
            shadow_midi_out->write_idx = write_offset + 4;
        }
    }

    /* Signal shim that data is ready */
    shadow_midi_out->ready++;

    return JS_TRUE;
}

/* move_midi_external_send([cin, status, data1, data2, ...]) -> bool
 * Queues MIDI to be sent to USB-A (cable 2).
 */
static JSValue js_move_midi_external_send(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    return js_shadow_midi_send(2, ctx, this_val, argc, argv);
}

/* move_midi_internal_send([cin, status, data1, data2]) -> bool
 * Queues MIDI to be sent to Move LEDs (cable 0).
 */
static JSValue js_move_midi_internal_send(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    return js_shadow_midi_send(0, ctx, this_val, argc, argv);
}

/* shadow_send_midi_to_dsp([status, d1, d2]) -> bool
 * Routes raw 3-byte MIDI to shadow chain DSP slots via shared memory.
 * Channel in status byte determines which slot(s) receive the message.
 */
static JSValue js_shadow_send_midi_to_dsp(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_midi_dsp) return JS_FALSE;
    if (argc < 1) return JS_FALSE;

    JSValueConst arr = argv[0];
    if (!JS_IsArray(ctx, arr)) return JS_FALSE;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    if (len < 3) return JS_FALSE;

    uint8_t msg[3];
    for (int j = 0; j < 3; j++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, j);
        int32_t val = 0;
        JS_ToInt32(ctx, &val, elem);
        JS_FreeValue(ctx, elem);
        msg[j] = (uint8_t)(val & 0xFF);
    }

    /* Write 4-byte aligned: [status, d1, d2, 0] */
    int write_offset = shadow_midi_dsp->write_idx;
    if (write_offset + 4 <= SHADOW_MIDI_DSP_BUFFER_SIZE) {
        shadow_midi_dsp->buffer[write_offset] = msg[0];
        shadow_midi_dsp->buffer[write_offset + 1] = msg[1];
        shadow_midi_dsp->buffer[write_offset + 2] = msg[2];
        shadow_midi_dsp->buffer[write_offset + 3] = 0;
        shadow_midi_dsp->write_idx = write_offset + 4;
    }

    /* Signal shim that data is ready */
    shadow_midi_dsp->ready++;

    return JS_TRUE;
}

/* shadow_log(message) - Log to shadow_ui.log from JS */
static JSValue js_shadow_log(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (msg) {
        shadow_ui_log_line(msg);
        JS_FreeCString(ctx, msg);
    }
    return JS_UNDEFINED;
}

/* Unified logging from JS - logs to debug.log */
static JSValue js_unified_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    const char *source = JS_ToCString(ctx, argv[0]);
    const char *msg = JS_ToCString(ctx, argv[1]);

    if (source && msg) {
        unified_log(source, LOG_LEVEL_DEBUG, "%s", msg);
    }

    if (source) JS_FreeCString(ctx, source);
    if (msg) JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

/* Check if unified logging is enabled */
static JSValue js_unified_log_enabled(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, unified_log_enabled());
}

/* === Host functions for store operations === */

#define BASE_DIR "/data/UserData/move-anything"
#define MODULES_DIR "/data/UserData/move-anything/modules"
#define CURL_PATH "/data/UserData/move-anything/bin/curl"

/* Helper: validate path is within BASE_DIR to prevent directory traversal */
/* Execute a command safely using fork/execvp instead of system() */
static int run_command(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static int validate_path(const char *path) {
    if (!path || strlen(path) < strlen(BASE_DIR)) return 0;
    if (strncmp(path, BASE_DIR, strlen(BASE_DIR)) != 0) return 0;
    if (strstr(path, "..") != NULL) return 0;

    /* Resolve symlinks and re-check the resolved path */
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        if (strncmp(resolved, BASE_DIR, strlen(BASE_DIR)) != 0) return 0;
    }
    return 1;
}

/* host_file_exists(path) -> bool */
static JSValue js_host_file_exists(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    if (!validate_path(path)) {
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    struct stat st;
    int exists = (stat(path, &st) == 0);

    JS_FreeCString(ctx, path);
    return exists ? JS_TRUE : JS_FALSE;
}

/* host_http_download(url, dest_path) -> bool */
static JSValue js_host_http_download(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    shadow_ui_log_line("host_http_download: called");
    if (argc < 2) {
        shadow_ui_log_line("host_http_download: argc < 2");
        return JS_FALSE;
    }

    const char *url = JS_ToCString(ctx, argv[0]);
    const char *dest_path = JS_ToCString(ctx, argv[1]);

    if (!url || !dest_path) {
        shadow_ui_log_line("host_http_download: null url or dest_path");
        if (url) JS_FreeCString(ctx, url);
        if (dest_path) JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    shadow_ui_log_line("host_http_download: url and path ok");
    shadow_ui_log_line(url);
    shadow_ui_log_line(dest_path);

    /* Validate URL scheme - only allow https:// and http:// */
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        shadow_ui_log_line("host_http_download: invalid URL scheme");
        fprintf(stderr, "host_http_download: invalid URL scheme: %s\n", url);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    /* Validate destination path */
    if (!validate_path(dest_path)) {
        shadow_ui_log_line("host_http_download: invalid dest path");
        fprintf(stderr, "host_http_download: invalid dest path: %s\n", dest_path);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    shadow_ui_log_line("host_http_download: path validated, running curl");

    const char *argv_cmd[] = {
        CURL_PATH, "-fsSLk", "--connect-timeout", "5", "--max-time", "600",
        "-o", dest_path, url, NULL
    };
    int result = run_command(argv_cmd);

    shadow_ui_log_line("host_http_download: curl returned");

    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, dest_path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_extract_tar(tar_path, dest_dir) -> bool */
static JSValue js_host_extract_tar(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_FALSE;
    }

    const char *tar_path = JS_ToCString(ctx, argv[0]);
    const char *dest_dir = JS_ToCString(ctx, argv[1]);

    if (!tar_path || !dest_dir) {
        if (tar_path) JS_FreeCString(ctx, tar_path);
        if (dest_dir) JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate paths */
    if (!validate_path(tar_path) || !validate_path(dest_dir)) {
        fprintf(stderr, "host_extract_tar: invalid path(s)\n");
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    const char *argv_cmd[] = {
        "tar", "-xzf", tar_path, "-C", dest_dir, NULL
    };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, tar_path);
    JS_FreeCString(ctx, dest_dir);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_extract_tar_strip(tar_path, dest_dir, strip_components) -> bool
 * Like host_extract_tar but with --strip-components for tarballs with a top-level dir */
static JSValue js_host_extract_tar_strip(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 3) {
        return JS_FALSE;
    }

    const char *tar_path = JS_ToCString(ctx, argv[0]);
    const char *dest_dir = JS_ToCString(ctx, argv[1]);
    int strip = 0;
    JS_ToInt32(ctx, &strip, argv[2]);

    if (!tar_path || !dest_dir) {
        if (tar_path) JS_FreeCString(ctx, tar_path);
        if (dest_dir) JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate paths */
    if (!validate_path(tar_path) || !validate_path(dest_dir)) {
        fprintf(stderr, "host_extract_tar_strip: invalid path(s)\n");
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate strip range */
    if (strip < 0 || strip > 5) {
        fprintf(stderr, "host_extract_tar_strip: invalid strip value: %d\n", strip);
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Build tar command */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\" --strip-components=%d 2>&1",
             tar_path, dest_dir, strip);

    int result = system(cmd);

    JS_FreeCString(ctx, tar_path);
    JS_FreeCString(ctx, dest_dir);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_system_cmd(cmd) -> int (exit code, -1 on error)
 * Run a shell command with allowlist validation.
 * Commands must start with an allowed prefix for safety. */
static JSValue js_host_system_cmd(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_NewInt32(ctx, -1);
    }

    const char *cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd) {
        return JS_NewInt32(ctx, -1);
    }

    /* Validate command starts with an allowed prefix */
    static const char *allowed_prefixes[] = {
        "tar ", "cp ", "mv ", "mkdir ", "rm ", "ls ", "test ", "chmod ", "sh ",
        NULL
    };

    int allowed = 0;
    for (int i = 0; allowed_prefixes[i]; i++) {
        if (strncmp(cmd, allowed_prefixes[i], strlen(allowed_prefixes[i])) == 0) {
            allowed = 1;
            break;
        }
    }

    if (!allowed) {
        fprintf(stderr, "host_system_cmd: command not allowed: %.40s...\n", cmd);
        JS_FreeCString(ctx, cmd);
        return JS_NewInt32(ctx, -1);
    }

    int result = system(cmd);
    JS_FreeCString(ctx, cmd);

    if (result == -1) {
        return JS_NewInt32(ctx, -1);
    }
    return JS_NewInt32(ctx, WEXITSTATUS(result));
}

/* host_remove_dir(path) -> bool */
static JSValue js_host_remove_dir(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    /* Validate path - must be within modules directory for safety */
    if (!validate_path(path)) {
        fprintf(stderr, "host_remove_dir: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Additional safety: must be within base directory (modules, staging, backup, tmp) */
    if (strncmp(path, MODULES_DIR, strlen(MODULES_DIR)) != 0 &&
        strncmp(path, BASE_DIR "/update-staging", strlen(BASE_DIR "/update-staging")) != 0 &&
        strncmp(path, BASE_DIR "/update-backup", strlen(BASE_DIR "/update-backup")) != 0 &&
        strncmp(path, BASE_DIR "/tmp", strlen(BASE_DIR "/tmp")) != 0) {
        fprintf(stderr, "host_remove_dir: path not allowed: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    const char *argv_cmd[] = { "rm", "-rf", path, NULL };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_read_file(path) -> string or null */
static JSValue js_host_read_file(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_NULL;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_NULL;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_read_file: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Limit to 1MB for safety */
    if (size > 1024 * 1024) {
        fprintf(stderr, "host_read_file: file too large: %s\n", path);
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    size_t bytes_read = fread(buf, 1, size, f);
    buf[bytes_read] = '\0';
    fclose(f);

    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    JS_FreeCString(ctx, path);

    return result;
}

/* host_write_file(path, content) -> bool */
static JSValue js_host_write_file(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    const char *content = JS_ToCString(ctx, argv[1]);
    if (!content) {
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_write_file: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return JS_FALSE;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "host_write_file: cannot open file: %s\n", path);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return JS_FALSE;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);

    return (written == len) ? JS_TRUE : JS_FALSE;
}

/* host_ensure_dir(path) -> bool - creates directory if it doesn't exist */
static JSValue js_host_ensure_dir(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_ensure_dir: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    const char *argv_cmd[] = { "mkdir", "-p", path, NULL };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* Helper: read a simple JSON string value from a file */
static int read_json_string(const char *filepath, const char *key, char *out, size_t out_len) {
    FILE *f = fopen(filepath, "r");
    if (!f) return 0;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Simple key search: "key": "value" */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *pos = strstr(buf, search);
    if (!pos) return 0;

    pos += strlen(search);
    /* Skip whitespace and colon */
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return 0;
    pos++;  /* Skip opening quote */

    /* Copy until closing quote */
    size_t i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return 1;
}

/* host_list_modules() -> [{id, name, version}, ...] */
static JSValue js_host_list_modules(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;

    JSValue arr = JS_NewArray(ctx);
    int idx = 0;

    /* Subdirectories to scan */
    const char *subdirs[] = { "", "sound_generators", "audio_fx", "midi_fx", "utilities", "overtake", "other", NULL };

    for (int s = 0; subdirs[s] != NULL; s++) {
        char dir_path[512];
        if (subdirs[s][0] == '\0') {
            snprintf(dir_path, sizeof(dir_path), "%s", MODULES_DIR);
        } else {
            snprintf(dir_path, sizeof(dir_path), "%s/%s", MODULES_DIR, subdirs[s]);
        }

        DIR *dir = opendir(dir_path);
        if (!dir) continue;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            /* Check if it's a directory with module.json */
            char module_json_path[1024];
            snprintf(module_json_path, sizeof(module_json_path), "%s/%s/module.json",
                     dir_path, ent->d_name);

            struct stat st;
            if (stat(module_json_path, &st) != 0) continue;

            /* Read module.json */
            char id[128] = "", name[256] = "", version[64] = "";
            read_json_string(module_json_path, "id", id, sizeof(id));
            read_json_string(module_json_path, "name", name, sizeof(name));
            read_json_string(module_json_path, "version", version, sizeof(version));

            if (id[0] == '\0') continue;  /* Skip if no id */

            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, id));
            JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name[0] ? name : id));
            JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, version[0] ? version : "0.0.0"));
            JS_SetPropertyUint32(ctx, arr, idx++, obj);
        }
        closedir(dir);
    }

    return arr;
}

/* host_rescan_modules() -> void
 * In shadow UI context, this is a no-op since the host manages module loading.
 * After installing, the shadow UI just needs to rescan its own list.
 */
static JSValue js_host_rescan_modules(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    /* No-op - shadow UI doesn't manage the host's module list */
    return JS_UNDEFINED;
}

/* host_flush_display() -> void
 * Immediately pack and copy display to shared memory.
 * This is critical for showing progress during blocking operations
 * (e.g. catalog fetch) where the main loop can't run.
 */
static JSValue js_host_flush_display(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (shadow_display_shm) {
        js_display_pack(packed_buffer);
        memcpy(shadow_display_shm, packed_buffer, DISPLAY_BUFFER_SIZE);
    }
    js_display_screen_dirty = 0;
    return JS_UNDEFINED;
}

static JSValue js_host_send_screenreader(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_screenreader) {
        return JS_UNDEFINED;
    }

    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_UNDEFINED;
    }

    /* Write message to shared memory */
    strncpy(shadow_screenreader->text, text, SHADOW_SCREENREADER_TEXT_LEN - 1);
    shadow_screenreader->text[SHADOW_SCREENREADER_TEXT_LEN - 1] = '\0';

    /* Get current time in milliseconds */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    shadow_screenreader->timestamp_ms = (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));

    /* Increment sequence to signal new message */
    shadow_screenreader->sequence++;

    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* tts_set_enabled(enabled) - Write to shared memory */
static JSValue js_tts_set_enabled(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->tts_enabled = enabled ? 1 : 0;

    return JS_UNDEFINED;
}

/* tts_get_enabled() -> bool - Read from shared memory */
static JSValue js_tts_get_enabled(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, true);
    return JS_NewBool(ctx, shadow_control->tts_enabled != 0);
}

/* display_mirror_set(enabled) - Write to shared memory + persist to features.json */
static JSValue js_display_mirror_set(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->display_mirror = enabled ? 1 : 0;

    /* Persist to features.json */
    const char *config_path = "/data/UserData/move-anything/config/features.json";
    char buf[512];
    size_t len = 0;
    FILE *f = fopen(config_path, "r");
    if (f) {
        len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[len] = '\0';

    /* Check if key already exists */
    char *key = strstr(buf, "\"display_mirror_enabled\"");
    if (key) {
        /* Replace the value */
        char *colon = strchr(key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ') colon++;
            char *val_end = colon;
            while (*val_end && *val_end != ',' && *val_end != '\n' && *val_end != '}') val_end++;
            /* Build new file content */
            char newbuf[512];
            int prefix_len = (int)(colon - buf);
            int suffix_start = (int)(val_end - buf);
            snprintf(newbuf, sizeof(newbuf), "%.*s%s%s",
                     prefix_len, buf,
                     enabled ? "true" : "false",
                     buf + suffix_start);
            f = fopen(config_path, "w");
            if (f) { fputs(newbuf, f); fclose(f); }
        }
    } else if (len > 0) {
        /* Append before closing brace */
        char *brace = strrchr(buf, '}');
        if (brace) {
            char newbuf[512];
            int prefix_len = (int)(brace - buf);
            snprintf(newbuf, sizeof(newbuf), "%.*s,\n  \"display_mirror_enabled\": %s\n}",
                     prefix_len, buf, enabled ? "true" : "false");
            f = fopen(config_path, "w");
            if (f) { fputs(newbuf, f); fclose(f); }
        }
    }

    return JS_UNDEFINED;
}

/* display_mirror_get() -> bool - Read from shared memory */
static JSValue js_display_mirror_get(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, shadow_control->display_mirror != 0);
}

/* tts_set_speed(speed) - Write to shared memory */
static JSValue js_tts_set_speed(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    double speed = 0;
    JS_ToFloat64(ctx, &speed, argv[0]);

    /* Clamp to valid range */
    if (speed < 0.5) speed = 0.5;
    if (speed > 6.0) speed = 6.0;

    shadow_control->tts_speed = (float)speed;

    return JS_UNDEFINED;
}

/* tts_get_speed() -> float - Read from shared memory */
static JSValue js_tts_get_speed(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewFloat64(ctx, 1.0);
    return JS_NewFloat64(ctx, (double)shadow_control->tts_speed);
}

/* tts_set_pitch(pitch_hz) - Write to shared memory */
static JSValue js_tts_set_pitch(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    double pitch = 0;
    JS_ToFloat64(ctx, &pitch, argv[0]);

    /* Clamp to valid range */
    if (pitch < 80.0) pitch = 80.0;
    if (pitch > 180.0) pitch = 180.0;

    shadow_control->tts_pitch = (uint16_t)pitch;

    return JS_UNDEFINED;
}

/* tts_get_pitch() -> float - Read from shared memory */
static JSValue js_tts_get_pitch(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewFloat64(ctx, 110.0);
    return JS_NewFloat64(ctx, (double)shadow_control->tts_pitch);
}

/* tts_set_volume(volume) - Write to shared memory */
static JSValue js_tts_set_volume(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int volume = 0;
    JS_ToInt32(ctx, &volume, argv[0]);

    /* Clamp to valid range */
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    shadow_control->tts_volume = (uint8_t)volume;

    return JS_UNDEFINED;
}

/* tts_get_volume() -> int - Read from shared memory */
static JSValue js_tts_get_volume(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 70);
    return JS_NewInt32(ctx, shadow_control->tts_volume);
}

/* tts_set_engine(name) - Write engine choice to shared memory (0=espeak, 1=flite) */
static JSValue js_tts_set_engine(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;

    if (strcmp(name, "flite") == 0) {
        shadow_control->tts_engine = 1;
    } else {
        shadow_control->tts_engine = 0;  /* default: espeak */
    }

    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* tts_get_engine() -> string - Read engine choice from shared memory */
static JSValue js_tts_get_engine(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewString(ctx, "espeak");
    return JS_NewString(ctx, shadow_control->tts_engine == 1 ? "flite" : "espeak");
}

/* tts_set_debounce(ms) - Write debounce time to shared memory */
static JSValue js_tts_set_debounce(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int ms = 0;
    JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 0) ms = 0;
    if (ms > 1000) ms = 1000;

    shadow_control->tts_debounce_ms = (uint16_t)ms;

    return JS_UNDEFINED;
}

/* tts_get_debounce() -> int - Read debounce time from shared memory */
static JSValue js_tts_get_debounce(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 300);
    return JS_NewInt32(ctx, shadow_control->tts_debounce_ms);
}

/* overlay_knobs_set_mode(mode) - Write to shared memory (0=shift, 1=jog_touch, 2=off, 3=native) */
static JSValue js_overlay_knobs_set_mode(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int mode = 0;
    JS_ToInt32(ctx, &mode, argv[0]);
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    shadow_control->overlay_knobs_mode = (uint8_t)mode;

    return JS_UNDEFINED;
}

/* overlay_knobs_get_mode() -> int - Read from shared memory */
static JSValue js_overlay_knobs_get_mode(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->overlay_knobs_mode);
}

/* === Overlay state bridge functions === */

static JSValue js_shadow_get_overlay_sequence(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_overlay) return JS_NewUint32(ctx, 0);
    return JS_NewUint32(ctx, shadow_overlay->sequence);
}

static JSValue js_shadow_get_overlay_state(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    if (!shadow_overlay) {
        JS_SetPropertyStr(ctx, obj, "type", JS_NewInt32(ctx, 0));
        return obj;
    }

    JS_SetPropertyStr(ctx, obj, "type", JS_NewInt32(ctx, shadow_overlay->overlay_type));
    JS_SetPropertyStr(ctx, obj, "samplerState", JS_NewInt32(ctx, shadow_overlay->sampler_state));
    JS_SetPropertyStr(ctx, obj, "samplerSource", JS_NewInt32(ctx, shadow_overlay->sampler_source));
    JS_SetPropertyStr(ctx, obj, "samplerCursor", JS_NewInt32(ctx, shadow_overlay->sampler_cursor));
    JS_SetPropertyStr(ctx, obj, "samplerFullscreen", JS_NewInt32(ctx, shadow_overlay->sampler_fullscreen));
    JS_SetPropertyStr(ctx, obj, "skipbackActive", JS_NewInt32(ctx, shadow_overlay->skipback_active));
    JS_SetPropertyStr(ctx, obj, "samplerDurationBars", JS_NewInt32(ctx, shadow_overlay->sampler_duration_bars));
    JS_SetPropertyStr(ctx, obj, "samplerVuPeak", JS_NewInt32(ctx, shadow_overlay->sampler_vu_peak));
    JS_SetPropertyStr(ctx, obj, "samplerBarsCompleted", JS_NewInt32(ctx, shadow_overlay->sampler_bars_completed));
    JS_SetPropertyStr(ctx, obj, "samplerTargetBars", JS_NewInt32(ctx, shadow_overlay->sampler_target_bars));
    JS_SetPropertyStr(ctx, obj, "samplerOverlayTimeout", JS_NewInt32(ctx, shadow_overlay->sampler_overlay_timeout));
    JS_SetPropertyStr(ctx, obj, "skipbackOverlayTimeout", JS_NewInt32(ctx, shadow_overlay->skipback_overlay_timeout));
    JS_SetPropertyStr(ctx, obj, "samplerSamplesWritten", JS_NewUint32(ctx, shadow_overlay->sampler_samples_written));
    JS_SetPropertyStr(ctx, obj, "samplerClockCount", JS_NewUint32(ctx, shadow_overlay->sampler_clock_count));
    JS_SetPropertyStr(ctx, obj, "samplerTargetPulses", JS_NewUint32(ctx, shadow_overlay->sampler_target_pulses));
    JS_SetPropertyStr(ctx, obj, "samplerFallbackBlocks", JS_NewUint32(ctx, shadow_overlay->sampler_fallback_blocks));
    JS_SetPropertyStr(ctx, obj, "samplerFallbackTarget", JS_NewUint32(ctx, shadow_overlay->sampler_fallback_target));
    JS_SetPropertyStr(ctx, obj, "samplerClockReceived", JS_NewInt32(ctx, shadow_overlay->sampler_clock_received));

    /* Shift+knob overlay */
    JS_SetPropertyStr(ctx, obj, "shiftKnobActive", JS_NewInt32(ctx, shadow_overlay->shift_knob_active));
    JS_SetPropertyStr(ctx, obj, "shiftKnobTimeout", JS_NewInt32(ctx, shadow_overlay->shift_knob_timeout));
    JS_SetPropertyStr(ctx, obj, "shiftKnobPatch", JS_NewString(ctx, (const char *)shadow_overlay->shift_knob_patch));
    JS_SetPropertyStr(ctx, obj, "shiftKnobParam", JS_NewString(ctx, (const char *)shadow_overlay->shift_knob_param));
    JS_SetPropertyStr(ctx, obj, "shiftKnobValue", JS_NewString(ctx, (const char *)shadow_overlay->shift_knob_value));

    /* Set page overlay */
    JS_SetPropertyStr(ctx, obj, "setPageActive", JS_NewInt32(ctx, shadow_overlay->set_page_active));
    JS_SetPropertyStr(ctx, obj, "setPageCurrent", JS_NewInt32(ctx, shadow_overlay->set_page_current));
    JS_SetPropertyStr(ctx, obj, "setPageTotal", JS_NewInt32(ctx, shadow_overlay->set_page_total));
    JS_SetPropertyStr(ctx, obj, "setPageTimeout", JS_NewInt32(ctx, shadow_overlay->set_page_timeout));
    JS_SetPropertyStr(ctx, obj, "setPageLoading", JS_NewInt32(ctx, shadow_overlay->set_page_loading));

    return obj;
}

static JSValue js_shadow_set_display_overlay(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control) return JS_UNDEFINED;
    int mode = 0, x = 0, y = 0, w = 0, h = 0;
    if (argc >= 1) JS_ToInt32(ctx, &mode, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &x, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &y, argv[2]);
    if (argc >= 4) JS_ToInt32(ctx, &w, argv[3]);
    if (argc >= 5) JS_ToInt32(ctx, &h, argv[4]);
    shadow_control->display_overlay = (uint8_t)mode;
    shadow_control->overlay_rect_x = (uint8_t)x;
    shadow_control->overlay_rect_y = (uint8_t)y;
    shadow_control->overlay_rect_w = (uint8_t)w;
    shadow_control->overlay_rect_h = (uint8_t)h;
    return JS_UNDEFINED;
}

/* === End host functions === */

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
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_display_mode", JS_NewCFunction(ctx, js_shadow_get_display_mode, "shadow_get_display_mode", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_mode", JS_NewCFunction(ctx, js_shadow_set_overtake_mode, "shadow_set_overtake_mode", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_request_exit", JS_NewCFunction(ctx, js_shadow_request_exit, "shadow_request_exit", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_control_restart", JS_NewCFunction(ctx, js_shadow_control_restart, "shadow_control_restart", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_load_ui_module", JS_NewCFunction(ctx, js_shadow_load_ui_module, "shadow_load_ui_module", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_param", JS_NewCFunction(ctx, js_shadow_set_param, "shadow_set_param", 3));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_param_timeout", JS_NewCFunction(ctx, js_shadow_set_param_timeout, "shadow_set_param_timeout", 4));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_param", JS_NewCFunction(ctx, js_shadow_get_param, "shadow_get_param", 2));

    /* Register MIDI output functions for overtake modules */
    JS_SetPropertyStr(ctx, global_obj, "move_midi_external_send", JS_NewCFunction(ctx, js_move_midi_external_send, "move_midi_external_send", 1));
    JS_SetPropertyStr(ctx, global_obj, "move_midi_internal_send", JS_NewCFunction(ctx, js_move_midi_internal_send, "move_midi_internal_send", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_send_midi_to_dsp", JS_NewCFunction(ctx, js_shadow_send_midi_to_dsp, "shadow_send_midi_to_dsp", 1));

    /* Register logging function for JS modules */
    JS_SetPropertyStr(ctx, global_obj, "shadow_log", JS_NewCFunction(ctx, js_shadow_log, "shadow_log", 1));
    JS_SetPropertyStr(ctx, global_obj, "unified_log", JS_NewCFunction(ctx, js_unified_log, "unified_log", 2));
    JS_SetPropertyStr(ctx, global_obj, "unified_log_enabled", JS_NewCFunction(ctx, js_unified_log_enabled, "unified_log_enabled", 0));

    /* Register host functions for store operations */
    JS_SetPropertyStr(ctx, global_obj, "host_file_exists", JS_NewCFunction(ctx, js_host_file_exists, "host_file_exists", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_read_file", JS_NewCFunction(ctx, js_host_read_file, "host_read_file", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_write_file", JS_NewCFunction(ctx, js_host_write_file, "host_write_file", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_http_download", JS_NewCFunction(ctx, js_host_http_download, "host_http_download", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_extract_tar", JS_NewCFunction(ctx, js_host_extract_tar, "host_extract_tar", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_extract_tar_strip", JS_NewCFunction(ctx, js_host_extract_tar_strip, "host_extract_tar_strip", 3));
    JS_SetPropertyStr(ctx, global_obj, "host_system_cmd", JS_NewCFunction(ctx, js_host_system_cmd, "host_system_cmd", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_ensure_dir", JS_NewCFunction(ctx, js_host_ensure_dir, "host_ensure_dir", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_remove_dir", JS_NewCFunction(ctx, js_host_remove_dir, "host_remove_dir", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_list_modules", JS_NewCFunction(ctx, js_host_list_modules, "host_list_modules", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_rescan_modules", JS_NewCFunction(ctx, js_host_rescan_modules, "host_rescan_modules", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_flush_display", JS_NewCFunction(ctx, js_host_flush_display, "host_flush_display", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_send_screenreader", JS_NewCFunction(ctx, js_host_send_screenreader, "host_send_screenreader", 1));

    /* Register TTS control functions */
    JS_SetPropertyStr(ctx, global_obj, "tts_set_enabled", JS_NewCFunction(ctx, js_tts_set_enabled, "tts_set_enabled", 1));
    JS_SetPropertyStr(ctx, global_obj, "tts_get_enabled", JS_NewCFunction(ctx, js_tts_get_enabled, "tts_get_enabled", 0));
    JS_SetPropertyStr(ctx, global_obj, "tts_set_speed", JS_NewCFunction(ctx, js_tts_set_speed, "tts_set_speed", 1));
    JS_SetPropertyStr(ctx, global_obj, "tts_get_speed", JS_NewCFunction(ctx, js_tts_get_speed, "tts_get_speed", 0));
    JS_SetPropertyStr(ctx, global_obj, "tts_set_pitch", JS_NewCFunction(ctx, js_tts_set_pitch, "tts_set_pitch", 1));
    JS_SetPropertyStr(ctx, global_obj, "tts_get_pitch", JS_NewCFunction(ctx, js_tts_get_pitch, "tts_get_pitch", 0));
    JS_SetPropertyStr(ctx, global_obj, "tts_set_volume", JS_NewCFunction(ctx, js_tts_set_volume, "tts_set_volume", 1));
    JS_SetPropertyStr(ctx, global_obj, "tts_get_volume", JS_NewCFunction(ctx, js_tts_get_volume, "tts_get_volume", 0));
    JS_SetPropertyStr(ctx, global_obj, "tts_set_engine", JS_NewCFunction(ctx, js_tts_set_engine, "tts_set_engine", 1));
    JS_SetPropertyStr(ctx, global_obj, "tts_get_engine", JS_NewCFunction(ctx, js_tts_get_engine, "tts_get_engine", 0));
    JS_SetPropertyStr(ctx, global_obj, "tts_set_debounce", JS_NewCFunction(ctx, js_tts_set_debounce, "tts_set_debounce", 1));
    JS_SetPropertyStr(ctx, global_obj, "tts_get_debounce", JS_NewCFunction(ctx, js_tts_get_debounce, "tts_get_debounce", 0));

    /* Register overlay knobs mode functions */
    JS_SetPropertyStr(ctx, global_obj, "overlay_knobs_set_mode", JS_NewCFunction(ctx, js_overlay_knobs_set_mode, "overlay_knobs_set_mode", 1));
    JS_SetPropertyStr(ctx, global_obj, "overlay_knobs_get_mode", JS_NewCFunction(ctx, js_overlay_knobs_get_mode, "overlay_knobs_get_mode", 0));

    /* Register display mirror functions */
    JS_SetPropertyStr(ctx, global_obj, "display_mirror_set", JS_NewCFunction(ctx, js_display_mirror_set, "display_mirror_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "display_mirror_get", JS_NewCFunction(ctx, js_display_mirror_get, "display_mirror_get", 0));

    /* Register overlay state functions (sampler/skipback state from shim) */
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_overlay_sequence", JS_NewCFunction(ctx, js_shadow_get_overlay_sequence, "shadow_get_overlay_sequence", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_overlay_state", JS_NewCFunction(ctx, js_shadow_get_overlay_state, "shadow_get_overlay_state", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_display_overlay", JS_NewCFunction(ctx, js_shadow_set_display_overlay, "shadow_set_display_overlay", 5));

    JS_SetPropertyStr(ctx, global_obj, "exit", JS_NewCFunction(ctx, js_exit, "exit", 0));

    JS_FreeValue(ctx, global_obj);

    *prt = rt;
    *pctx = ctx;
}

static int process_shadow_midi(JSContext *ctx, JSValue *onInternal, JSValue *onExternal) {
    if (!shadow_ui_midi_shm) return 0;
    int handled = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = shadow_ui_midi_shm[i] & 0x0F;
        uint8_t cable = (shadow_ui_midi_shm[i] >> 4) & 0x0F;

        /* CIN 0x04-0x07: SysEx, CIN 0x08-0x0E: Note/CC/etc */
        if (cin < 0x04 || cin > 0x0E) continue;
        uint8_t msg[3] = { shadow_ui_midi_shm[i + 1], shadow_ui_midi_shm[i + 2], shadow_ui_midi_shm[i + 3] };
        if (msg[0] + msg[1] + msg[2] == 0) continue;
        handled = 1;
        if (cable == 2) {
            /* Re-lookup onMidiMessageExternal each time in case overtake module replaced it */
            JSValue freshExternal;
            if (getGlobalFunction(ctx, "onMidiMessageExternal", &freshExternal)) {
                callGlobalFunction(ctx, &freshExternal, msg);
                JS_FreeValue(ctx, freshExternal);
            }
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
    unified_log_init();
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

    JSValue JSonMidiMessageInternal = JS_UNDEFINED;
    JSValue JSonMidiMessageExternal = JS_UNDEFINED;
    JSValue JSinit = JS_UNDEFINED;
    JSValue JSTick = JS_UNDEFINED;
    JSValue JSSaveState = JS_UNDEFINED;

    if (!getGlobalFunction(ctx, "onMidiMessageInternal", &JSonMidiMessageInternal)) {
        shadow_ui_log_line("shadow_ui: onMidiMessageInternal missing");
    }
    if (!getGlobalFunction(ctx, "onMidiMessageExternal", &JSonMidiMessageExternal)) {
        shadow_ui_log_line("shadow_ui: onMidiMessageExternal missing");
    }
    int jsInitIsDefined = getGlobalFunction(ctx, "init", &JSinit);
    if (!jsInitIsDefined) {
        shadow_ui_log_line("shadow_ui: init missing");
    }
    int jsTickIsDefined = getGlobalFunction(ctx, "tick", &JSTick);
    if (!jsTickIsDefined) {
        shadow_ui_log_line("shadow_ui: tick missing");
    }
    int jsSaveStateIsDefined = getGlobalFunction(ctx, "shadow_save_state_now", &JSSaveState);
    if (!jsSaveStateIsDefined) {
        shadow_ui_log_line("shadow_ui: shadow_save_state_now missing");
    }

    if (jsInitIsDefined) callGlobalFunction(ctx, &JSinit, 0);
    shadow_ui_log_line("shadow_ui: init called");

    int refresh_counter = 0;
    while (!global_exit_flag) {
        if (shadow_control && shadow_control->should_exit) {
            if (jsSaveStateIsDefined) {
                callGlobalFunction(ctx, &JSSaveState, 0);
            }
            break;
        }

        /* Process incoming MIDI BEFORE tick() so that the current frame's
         * drawUI() reflects the latest input (knob CCs, button presses).
         * This eliminates one full loop iteration of display latency. */
        if (shadow_control && shadow_control->midi_ready != last_midi_ready) {
            last_midi_ready = shadow_control->midi_ready;
            process_shadow_midi(ctx, &JSonMidiMessageInternal, &JSonMidiMessageExternal);
            /* Always clear buffer after processing - even if no events were found,
             * the buffer may contain data the shim wrote that we couldn't parse.
             * This prevents the buffer from filling up and blocking new writes. */
            if (shadow_ui_midi_shm) {
                memset(shadow_ui_midi_shm, 0, MIDI_BUFFER_SIZE);
            }
        }

        if (jsTickIsDefined) {
            callGlobalFunction(ctx, &JSTick, 0);
        }

        refresh_counter++;
        if ((js_display_screen_dirty || (refresh_counter % 30 == 0)) && shadow_display_shm) {
            js_display_pack(packed_buffer);
            memcpy(shadow_display_shm, packed_buffer, DISPLAY_BUFFER_SIZE);
            js_display_screen_dirty = 0;
        }

        /* Overtake modules need a faster tick rate for responsive display/LED
         * updates.  Normal shadow UI (slot management) is fine at ~60 Hz. */
        if (shadow_control && shadow_control->overtake_mode >= 2) {
            usleep(2000);   /* ~500 Hz effective (minus tick work) */
        } else {
            usleep(16000);  /* ~60 Hz for normal shadow UI */
        }
    }

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
