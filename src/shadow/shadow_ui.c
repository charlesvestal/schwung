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
#include <sys/wait.h>
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

    fd = shm_open(SHM_SHADOW_MIDI_OUT, O_RDWR, 0666);
    if (fd >= 0) {
        shadow_midi_out = (shadow_midi_out_t *)mmap(NULL, sizeof(shadow_midi_out_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
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
 */
static JSValue js_shadow_load_ui_module(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    shadow_ui_log_line("Loading UI module:");
    shadow_ui_log_line(path);

    int ret = eval_file(ctx, path, 1);  /* Load as ES module */
    JS_FreeCString(ctx, path);

    return ret == 0 ? JS_TRUE : JS_FALSE;
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
    /* Clear entire value buffer to prevent any stale data */
    memset(shadow_param->value, 0, SHADOW_PARAM_VALUE_LEN);

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
static int validate_path(const char *path) {
    if (!path || strlen(path) < strlen(BASE_DIR)) return 0;
    if (strncmp(path, BASE_DIR, strlen(BASE_DIR)) != 0) return 0;
    if (strstr(path, "..") != NULL) return 0;
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

    /* Validate destination path */
    if (!validate_path(dest_path)) {
        shadow_ui_log_line("host_http_download: invalid dest path");
        fprintf(stderr, "host_http_download: invalid dest path: %s\n", dest_path);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    shadow_ui_log_line("host_http_download: path validated, running curl");

    /* Build curl command - use -k to skip SSL verification, timeouts to prevent hangs */
    /* Short timeout (15s) is fine for catalog/release.json; module tarballs are small enough too */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -fsSLk --connect-timeout 5 --max-time 15 -o \"%s\" \"%s\" 2>&1",
             CURL_PATH, dest_path, url);

    shadow_ui_log_line(cmd);

    int result = system(cmd);

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

    /* Build tar command */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\" 2>&1",
             tar_path, dest_dir);

    int result = system(cmd);

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

    /* Build rm command */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>&1", path);

    int result = system(cmd);

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

    /* Build mkdir command */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" 2>&1", path);

    int result = system(cmd);

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
    const char *subdirs[] = { "", "sound_generators", "audio_fx", "midi_fx", "utilities", NULL };

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
 * In shadow UI context, just mark display as dirty.
 * The main loop handles copying to shared memory.
 */
static JSValue js_host_flush_display(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    /* Mark display as dirty - main loop will copy to shared memory */
    js_display_screen_dirty = 1;
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
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_mode", JS_NewCFunction(ctx, js_shadow_set_overtake_mode, "shadow_set_overtake_mode", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_request_exit", JS_NewCFunction(ctx, js_shadow_request_exit, "shadow_request_exit", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_control_restart", JS_NewCFunction(ctx, js_shadow_control_restart, "shadow_control_restart", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_load_ui_module", JS_NewCFunction(ctx, js_shadow_load_ui_module, "shadow_load_ui_module", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_param", JS_NewCFunction(ctx, js_shadow_set_param, "shadow_set_param", 3));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_param", JS_NewCFunction(ctx, js_shadow_get_param, "shadow_get_param", 2));

    /* Register MIDI output functions for overtake modules */
    JS_SetPropertyStr(ctx, global_obj, "move_midi_external_send", JS_NewCFunction(ctx, js_move_midi_external_send, "move_midi_external_send", 1));
    JS_SetPropertyStr(ctx, global_obj, "move_midi_internal_send", JS_NewCFunction(ctx, js_move_midi_internal_send, "move_midi_internal_send", 1));

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
            /* Always clear buffer after processing - even if no events were found,
             * the buffer may contain data the shim wrote that we couldn't parse.
             * This prevents the buffer from filling up and blocking new writes. */
            if (shadow_ui_midi_shm) {
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
