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
#include <sched.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>

#include "quickjs.h"
#include "quickjs-libc.h"

#include "host/js_display.h"
#include "host/shadow_constants.h"
#include "host/shadow_shm_util.h"
#include "host/js_host_common.h"
#include "../host/unified_log.h"
#include "../host/analytics.h"

#define SAMPLER_CMD_PATH "/data/UserData/schwung/sampler_cmd_path.txt"

static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;
static shadow_ui_state_t *shadow_ui_state = NULL;
static shadow_param_t *shadow_param = NULL;
static shadow_midi_out_t *shadow_midi_out = NULL;
static shadow_midi_dsp_t *shadow_midi_dsp = NULL;
static shadow_midi_inject_t *shadow_midi_inject = NULL;
static schwung_ext_midi_remap_t *ext_midi_remap = NULL;
static shadow_screenreader_t *shadow_screenreader = NULL;
static shadow_overlay_state_t *shadow_overlay = NULL;

static int global_exit_flag = 0;
static uint8_t last_midi_ready = 0;
static const char *shadow_ui_pid_path = "/data/UserData/schwung/shadow_ui.pid";

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
    /* Attach to segments created by the shim (create=0). The first three
     * are required; the rest are optional. */
    shadow_display_shm = (uint8_t *)shadow_shm_map(SHM_SHADOW_DISPLAY, DISPLAY_BUFFER_SIZE, 0, 0);
    if (!shadow_display_shm) return -1;

    shadow_ui_midi_shm = (uint8_t *)shadow_shm_map(SHM_SHADOW_UI_MIDI, MIDI_BUFFER_SIZE, 0, 0);
    if (!shadow_ui_midi_shm) return -1;

    shadow_control = (shadow_control_t *)shadow_shm_map(SHM_SHADOW_CONTROL, CONTROL_BUFFER_SIZE, 0, 0);
    if (!shadow_control) return -1;

    shadow_ui_state = (shadow_ui_state_t *)shadow_shm_map(SHM_SHADOW_UI, SHADOW_UI_BUFFER_SIZE, 0, 0);

    shadow_param = (shadow_param_t *)shadow_shm_map(SHM_SHADOW_PARAM, SHADOW_PARAM_BUFFER_SIZE, 0, 0);

    shadow_midi_out = (shadow_midi_out_t *)shadow_shm_map(SHM_SHADOW_MIDI_OUT, sizeof(shadow_midi_out_t), 0, 0);

    shadow_midi_dsp = (shadow_midi_dsp_t *)shadow_shm_map(SHM_SHADOW_MIDI_DSP, sizeof(shadow_midi_dsp_t), 0, 0);

    shadow_midi_inject = (shadow_midi_inject_t *)shadow_shm_map(SHM_SHADOW_MIDI_INJECT, sizeof(shadow_midi_inject_t), 0, 0);

    ext_midi_remap = (schwung_ext_midi_remap_t *)shadow_shm_map(SHM_SHADOW_EXT_MIDI_REMAP, sizeof(schwung_ext_midi_remap_t), 0, 0);

    shadow_screenreader = (shadow_screenreader_t *)shadow_shm_map(SHM_SHADOW_SCREENREADER, sizeof(shadow_screenreader_t), 0, 0);
    if (shadow_screenreader) {
        unified_log("shadow_ui", LOG_LEVEL_DEBUG, "Shadow screen reader shm mapped: %p", shadow_screenreader);
    }

    shadow_overlay = (shadow_overlay_state_t *)shadow_shm_map(SHM_SHADOW_OVERLAY, SHADOW_OVERLAY_BUFFER_SIZE, 0, 0);
    if (shadow_overlay) {
        unified_log("shadow_ui", LOG_LEVEL_DEBUG, "Shadow overlay shm mapped: %p", shadow_overlay);
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

/* QuickJS scaffolding (JS_NewCustomContext, eval_buf, eval_file,
 * getGlobalFunction, callGlobalFunction) lives in host/js_host_common.c. */

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

/* shadow_get_open_tool_cmd() -> int (0=none, 1=open_tool; auto-clears) */
static JSValue js_shadow_get_open_tool_cmd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    uint8_t cmd = shadow_control->open_tool_cmd;
    if (cmd) shadow_control->open_tool_cmd = 0;  /* auto-clear */
    return JS_NewInt32(ctx, cmd);
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

/* shadow_inbound_pad_midi_active() -> int
 * Returns 1 if this build of the shim delivers internal pad MIDI to the
 * overtake tool's DSP on_midi on the audio thread. Capability sentinel
 * for tools that want audio-thread input: check
 * `typeof shadow_inbound_pad_midi_active === 'function'` to gate the
 * DSP-owned input path. The function's mere existence is the signal;
 * the return value is reserved for future use. */
static JSValue js_shadow_inbound_pad_midi_active(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 1);
}

/* shadow_overtake_send_external_async_active() -> int
 * Returns 1 if this build of the shim performs the ROUTE_EXTERNAL SPI
 * ioctl off the audio thread (Phase 2 worker). Capability sentinel for
 * tools that want to call overtake_host_api.midi_send_external directly
 * from their audio thread without the kernel-ioctl deadlock risk:
 * `typeof shadow_overtake_send_external_async_active === 'function'`.
 * The function's mere existence is the signal; return value reserved. */
static JSValue js_shadow_overtake_send_external_async_active(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 1);
}

/* shadow_corun_begin(target, id, keep_mask) -> void
 * Generalized co-run entry for any overtake tool. The framework yields the
 * surfaces the tool cedes (via keep_mask) to the named peer UI, and reserves
 * Back as the exit gesture for the duration of the session.
 *   target   : CORUN_TARGET_CHAIN_EDIT  (id = chain slot 0-3)
 *              CORUN_TARGET_MOVE_NATIVE (id = tool track 0-7)
 *   id       : the target's identity (chain slot or tool track).
 *   keep_mask: bitfield of CORUN_GRP_* the tool KEEPS; the rest cede to the
 *              peer. Omit or 0 = CORUN_KEEP_DEFAULT (default split).
 * Atomicity: keep_mask is written before target so the shim never sees an
 * active target paired with a stale mask. */
static JSValue js_shadow_corun_begin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_UNDEFINED;
    int target = -1, id = -1, keep = 0;
    if (JS_ToInt32(ctx, &target, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &id, argv[1])) return JS_UNDEFINED;
    if (argc >= 3 && JS_ToInt32(ctx, &keep, argv[2])) return JS_UNDEFINED;
    if (keep < 0 || keep > 0xFFFF) return JS_UNDEFINED;
    if (target == CORUN_TARGET_CHAIN_EDIT) {
        if (id < 0 || id >= SHADOW_UI_SLOTS) return JS_UNDEFINED;
        shadow_control->corun.keep_mask = (uint16_t)keep;
        shadow_control->corun.id = (int8_t)id;
        shadow_control->ui_slot = (uint8_t)id;
        shadow_control->corun.target = CORUN_TARGET_CHAIN_EDIT;
        /* chain-edit renders via shadow_ui; OLED owner stays SCHWUNG_UI. */
        shadow_control->shadow_display_owner = DISPLAY_OWNER_SCHWUNG_UI;
    } else if (target == CORUN_TARGET_MOVE_NATIVE) {
        if (id < 0 || id > 7) return JS_UNDEFINED;
        shadow_control->corun.keep_mask = (uint16_t)keep;
        shadow_control->corun.id = (int8_t)id;
        shadow_control->corun.target = CORUN_TARGET_MOVE_NATIVE;
        /* move_native cedes the OLED to Move firmware. shadow_display_mode
         * stays armed so MIDI filters remain active. */
        shadow_control->shadow_display_owner = DISPLAY_OWNER_MOVE_FIRMWARE;
    } else {
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* shadow_corun_end() -> void
 * Exit co-run; restore full overtake ownership to the tool. The shim's Back
 * handler also calls this when the user presses Back during a session.
 * Target is cleared before keep_mask so the shim sees co-run off first. */
static JSValue js_shadow_corun_end(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_UNDEFINED;
    shadow_control->corun.target = CORUN_TARGET_NONE;
    shadow_control->corun.id = -1;
    shadow_control->corun.keep_mask = 0;
    /* Returning to the tool's shadow session — shadow_ui resumes rendering. */
    shadow_control->shadow_display_owner = DISPLAY_OWNER_SCHWUNG_UI;
    return JS_UNDEFINED;
}

/* shadow_corun_state() -> { target, id, keep_mask } | null
 * Returns null when no co-run is active, else the current state. Tools poll
 * this each frame to detect framework-driven exit (Back press) and to
 * reconcile their own mirror. */
static JSValue js_shadow_corun_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control || !corun_active(shadow_control)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "target", JS_NewInt32(ctx, (int)corun_target(shadow_control)));
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, corun_id(shadow_control)));
    JS_SetPropertyStr(ctx, obj, "keep_mask", JS_NewInt32(ctx, (int)corun_keep_mask(shadow_control)));
    return obj;
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

/* shadow_get_move_ui_mode() -> int
 * Returns Move's UI mode from shared control struct:
 * 0=unknown, 1=session, 2=note, 3=set_overview
 */
static JSValue js_shadow_get_move_ui_mode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->move_ui_mode);
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
    /* NOTE: Shift-off and volume-touch-off injection on overtake exit is
     * handled by the shim's ioctl handler (transition detection), not here.
     * This covers all cases including D-Bus shutdown prompt direct writes. */
    return JS_UNDEFINED;
}

/* shadow_set_suspend_overtake(flag) -> void
 * Set suspend_overtake flag so the shim skips the exit hook on overtake exit.
 * Must be called BEFORE shadow_set_overtake_mode(0).
 */
static JSValue js_shadow_set_suspend_overtake(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->suspend_overtake = (uint8_t)(flag ? 1 : 0);
    return JS_UNDEFINED;
}

/* shadow_get_suspend_overtake() -> int
 * Read the suspend_overtake flag (set by shim for Shift+Vol+Back).
 */
static JSValue js_shadow_get_suspend_overtake(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, shadow_control->suspend_overtake);
}

/* shadow_consume_resume_last_tool() -> int
 * Read-and-clear the resume_last_tool hint set by the shim on Shift+Step13 long-press.
 */
static JSValue js_shadow_consume_resume_last_tool(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    int v = shadow_control->resume_last_tool ? 1 : 0;
    shadow_control->resume_last_tool = 0;
    return JS_NewInt32(ctx, v);
}

/* shadow_set_skip_led_clear(flag) -> void
 * Set skip_led_clear flag so the LED queue preserves pad colors on overtake entry.
 * Must be called BEFORE shadow_set_overtake_mode(2).
 */
static JSValue js_shadow_set_skip_led_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->skip_led_clear = flag ? 1 : 0;
    return JS_UNDEFINED;
}

/* host_mute_move_audio(flag) -> void
 * Mute/unmute Move's audio output. Used for silent clip switching. */
static JSValue js_host_mute_move_audio(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->mute_move_audio = flag ? 1 : 0;
    return JS_UNDEFINED;
}

/* shadow_get_pad_led_snapshot() -> object { "68": color, "69": color, ... }
 * Read cached LED colors for pads (notes 68-99) from overlay SHM.
 * The shim continuously writes Move's MIDI_OUT LED state here. */
static JSValue js_shadow_get_pad_led_snapshot(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    for (int i = 0; i < 32; i++) {
        int note = 68 + i;
        int color = shadow_overlay ? (int)shadow_overlay->pad_led_colors[i] : 0;
        char key[4];
        snprintf(key, sizeof(key), "%d", note);
        JS_SetPropertyStr(ctx, obj, key, JS_NewInt32(ctx, color));
    }
    return obj;
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

static int shadow_set_param_common(int slot, const char *key, const char *value, int timeout_ms, int force_blocking) {
    const int overtake_fire_and_forget = !force_blocking && (shadow_control && shadow_control->overtake_mode >= 2);

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

    int ok = shadow_set_param_common(slot, key, value, SHADOW_PARAM_DEFAULT_TIMEOUT_MS, 0);

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);

    return ok ? JS_TRUE : JS_FALSE;
}

/* shadow_set_param_timeout(slot, key, value, timeout_ms) -> bool
 * Timeout-aware variant that always blocks (bypasses overtake fire-and-forget).
 * Use for critical params that must be delivered before a subsequent get_param.
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

    int ok = shadow_set_param_common(slot, key, value, (int)timeout_ms, 1);

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

    /* Signal shim that data is ready (barrier ensures buffer writes are visible first) */
    __sync_synchronize();
    shadow_midi_dsp->ready++;

    return JS_TRUE;
}

/* host_ext_midi_remap_set(in_ch, out_ch) -> bool
 * Set cable-2 channel remap entry. in_ch and out_ch are 0-indexed (0-15).
 * out_ch >= 16 (or 0xFF) means passthrough for that input channel.
 * Returns true on success, false if SHM unavailable or args invalid.
 */
static JSValue js_host_ext_midi_remap_set(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (!ext_midi_remap || argc < 2) return JS_FALSE;
    int32_t in_ch = 0, out_ch = 0;
    if (JS_ToInt32(ctx, &in_ch, argv[0]) < 0) return JS_FALSE;
    if (JS_ToInt32(ctx, &out_ch, argv[1]) < 0) return JS_FALSE;
    if (in_ch < 0 || in_ch > 15) return JS_FALSE;
    uint8_t mapped = (out_ch == (int32_t)EXT_MIDI_REMAP_BLOCK) ? EXT_MIDI_REMAP_BLOCK :
                     (out_ch < 0 || out_ch > 15) ? EXT_MIDI_REMAP_PASSTHROUGH :
                     (uint8_t)out_ch;
    ext_midi_remap->remap[in_ch] = mapped;
    __sync_synchronize();
    return JS_TRUE;
}

/* host_ext_midi_remap_clear() -> bool
 * Clear all remap entries (full passthrough). Does not change enabled flag.
 */
static JSValue js_host_ext_midi_remap_clear(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!ext_midi_remap) return JS_FALSE;
    memset((void *)ext_midi_remap->remap, EXT_MIDI_REMAP_PASSTHROUGH, 16);
    __sync_synchronize();
    return JS_TRUE;
}

/* host_ext_midi_remap_enable(on) -> bool
 * Enable or disable the remap feature. Disabled = bypass entire codepath in shim.
 */
static JSValue js_host_ext_midi_remap_enable(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (!ext_midi_remap || argc < 1) return JS_FALSE;
    int on = JS_ToBool(ctx, argv[0]);
    if (on < 0) return JS_FALSE;
    ext_midi_remap->enabled = on ? 1 : 0;
    __sync_synchronize();
    return JS_TRUE;
}

/* move_midi_inject_to_move([cin, status, d1, d2, ...]) -> bool
 * Injects USB-MIDI packets into Move's MIDI_IN buffer via shared memory.
 * Move processes these as if they came from a hardware MIDI source.
 *
 * Cable bits in packet[0] are preserved as-is. The caller chooses how
 * Move firmware routes the event:
 *
 *   cable 0 (CIN nibble 0x0X) — internal hardware. Move treats the
 *     event as a physical pad / button / knob press. Use this to
 *     simulate the surface (song-mode auto-playback triggers pads
 *     this way: pkt[0] = 0x09 for note-on, 0x08 for note-off).
 *
 *   cable 2 (CIN nibble 0x2X) — external USB MIDI input. Move treats
 *     the event as if it arrived from a USB-A MIDI device. Routes
 *     through Move's track-input dispatcher and reaches the
 *     internal track synths. Use this when JS wants to play a Move
 *     track instrument as MIDI.
 */
static JSValue js_move_midi_inject_to_move(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_midi_inject) return JS_FALSE;
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

        /* Cable bits in packet[0] preserved as-is — caller picks the
         * route (cable 0 = pad simulation, cable 2 = external MIDI in
         * to the track synth). See function docblock. */

        /* Find space in buffer and write */
        int write_offset = shadow_midi_inject->write_idx;
        if (write_offset + 4 <= SHADOW_MIDI_INJECT_BUFFER_SIZE) {
            memcpy(&shadow_midi_inject->buffer[write_offset], packet, 4);
            shadow_midi_inject->write_idx = write_offset + 4;
        }
    }

    /* Signal shim that data is ready */
    __sync_synchronize();
    shadow_midi_inject->ready++;

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

#define MODULES_DIR "/data/UserData/schwung/modules"

/* run_command / validate_path and the shared file/store/http bindings
 * (host_file_exists, host_read_file(_base64), host_write_file,
 * host_http_download(_background), host_http_request_background,
 * host_extract_tar(_strip), host_ensure_dir, host_remove_dir) live in
 * host/js_host_common.c and are registered via js_host_register_common(). */

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

    /* Use fork/exec instead of system() to drop inherited FIFO scheduling.
     * shadow_ui runs at SCHED_FIFO 70 (inherited from MoveOriginal's audio
     * thread via LD_PRELOAD shim). system() preserves this, so every child
     * process (RNBO, jack_midi_connect, taskset, etc.) also runs at FIFO 70,
     * competing with the SPI driver and causing audio glitches. */
    pid_t pid = fork();
    if (pid == -1) {
        JS_FreeCString(ctx, cmd);
        return JS_NewInt32(ctx, -1);
    }
    if (pid == 0) {
        /* Child: drop to SCHED_OTHER before exec */
        struct sched_param sp = { .sched_priority = 0 };
        sched_setscheduler(0, SCHED_OTHER, &sp);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    JS_FreeCString(ctx, cmd);

    /* Wait for child (matches system() behavior) */
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return JS_NewInt32(ctx, WEXITSTATUS(status));
    }
    return JS_NewInt32(ctx, -1);
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

/* host_track_event(event_name, properties_json) -> void */
static JSValue js_host_track_event(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_UNDEFINED;

    const char *props = NULL;
    if (argc >= 2) {
        props = JS_ToCString(ctx, argv[1]);
    }

    analytics_track(event, props);

    if (props) JS_FreeCString(ctx, props);
    JS_FreeCString(ctx, event);
    return JS_UNDEFINED;
}

/* host_get_setting / host_set_setting for analytics_enabled */
static JSValue js_host_get_analytics_enabled(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, analytics_enabled());
}

static JSValue js_host_set_analytics_enabled(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int val;
    if (!JS_ToInt32(ctx, &val, argv[0])) {
        analytics_set_enabled(val ? 1 : 0);
    }
    return JS_UNDEFINED;
}

/* host_list_modules() -> [{id, name, version}, ...] */
static JSValue js_host_list_modules(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;

    JSValue arr = JS_NewArray(ctx);
    int idx = 0;

    /* Subdirectories to scan */
    const char *subdirs[] = { "", "sound_generators", "audio_fx", "midi_fx", "utilities", "overtake", "tools", "other", NULL };

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
            char scan_packs[128] = "", component_type[32] = "";
            read_json_string(module_json_path, "id", id, sizeof(id));
            read_json_string(module_json_path, "name", name, sizeof(name));
            read_json_string(module_json_path, "version", version, sizeof(version));
            read_json_string(module_json_path, "scan_packs", scan_packs, sizeof(scan_packs));
            read_json_string(module_json_path, "component_type", component_type, sizeof(component_type));

            if (id[0] == '\0') continue;  /* Skip if no id */

            if (scan_packs[0]) {
                /* Expand packs: scan subdirectory for extracted pack directories
                 * with info.json. Each pack becomes a separate module entry.
                 * The base module is hidden. */
                char packs_dir[1024];
                snprintf(packs_dir, sizeof(packs_dir), "%s/%s/%s",
                         dir_path, ent->d_name, scan_packs);

                /* Auto-extract any .rnbopack tarballs */
                DIR *rpdir = opendir(packs_dir);
                if (rpdir) {
                    struct dirent *rpent;
                    while ((rpent = readdir(rpdir)) != NULL) {
                        const char *ext = strstr(rpent->d_name, ".rnbopack");
                        if (!ext || ext[9] != '\0') continue;
                        char stem[128];
                        strncpy(stem, rpent->d_name, sizeof(stem) - 1);
                        stem[sizeof(stem) - 1] = '\0';
                        char *dot = strstr(stem, ".rnbopack");
                        if (dot) *dot = '\0';
                        char check_info[1024];
                        snprintf(check_info, sizeof(check_info), "%s/%s/info.json", packs_dir, stem);
                        if (stat(check_info, &st) == 0) continue; /* already extracted */
                        char cmd[2048];
                        snprintf(cmd, sizeof(cmd), "mkdir -p '%s/%s' && tar -xf '%s/%s' -C '%s/%s' --strip-components=1 2>/dev/null",
                                 packs_dir, stem, packs_dir, rpent->d_name, packs_dir, stem);
                        system(cmd);
                    }
                    closedir(rpdir);
                }

                DIR *pdir = opendir(packs_dir);
                if (pdir) {
                    struct dirent *pent;
                    while ((pent = readdir(pdir)) != NULL) {
                        if (pent->d_name[0] == '.') continue;
                        char pack_info[1024];
                        snprintf(pack_info, sizeof(pack_info), "%s/%s/info.json",
                                 packs_dir, pent->d_name);
                        if (stat(pack_info, &st) != 0) continue;

                        char pack_name[256] = "";
                        read_json_string(pack_info, "name", pack_name, sizeof(pack_name));
                        if (pack_name[0] == '\0')
                            strncpy(pack_name, pent->d_name, sizeof(pack_name) - 1);

                        char pack_id[128];
                        snprintf(pack_id, sizeof(pack_id), "%s-%s", id, pent->d_name);

                        JSValue obj = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, pack_id));
                        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, pack_name));
                        JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, version[0] ? version : "0.0.0"));
                        if (component_type[0])
                            JS_SetPropertyStr(ctx, obj, "component_type", JS_NewString(ctx, component_type));
                        JS_SetPropertyUint32(ctx, arr, idx++, obj);
                    }
                    closedir(pdir);
                }
                continue;  /* Don't add the base module */
            }

            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, id));
            JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name[0] ? name : id));
            JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, version[0] ? version : "0.0.0"));
            if (component_type[0])
                JS_SetPropertyStr(ctx, obj, "component_type", JS_NewString(ctx, component_type));
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

/* Read-modify-write one key in features.json. Reads the whole file — the
 * old per-setter fixed 512-byte buffers truncated a grown config, so any
 * settings toggle destroyed every key past the cut on rewrite. value_json
 * is raw JSON ("true", "30", "\"both\""). Creates a minimal config if the
 * file is missing or empty. */
static void features_json_set(const char *key, const char *value_json) {
    const char *config_path = "/data/UserData/schwung/config/features.json";
    char *buf = NULL;
    size_t len = 0;
    FILE *f = fopen(config_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= 65536) {
            buf = malloc((size_t)sz + 1);
            if (buf) {
                len = fread(buf, 1, (size_t)sz, f);
                buf[len] = '\0';
            }
        }
        fclose(f);
    }

    if (!buf || len == 0) {
        free(buf);
        f = fopen(config_path, "w");
        if (f) {
            fprintf(f, "{\n  \"%s\": %s\n}\n", key, value_json);
            fclose(f);
        }
        return;
    }

    size_t out_cap = len + strlen(key) + strlen(value_json) + 16;
    char *out = malloc(out_cap);
    if (!out) {
        free(buf);
        return;
    }

    char quoted_key[128];
    snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
    char *kpos = strstr(buf, quoted_key);
    int built = 0;
    if (kpos) {
        char *colon = strchr(kpos, ':');
        if (colon) {
            colon++;
            while (*colon == ' ') colon++;
            char *val_end = colon;
            while (*val_end && *val_end != ',' && *val_end != '\n' && *val_end != '}') val_end++;
            snprintf(out, out_cap, "%.*s%s%s",
                     (int)(colon - buf), buf, value_json, val_end);
            built = 1;
        }
    }
    if (!built) {
        char *brace = strrchr(buf, '}');
        if (brace) {
            snprintf(out, out_cap, "%.*s,\n  \"%s\": %s\n}",
                     (int)(brace - buf), buf, key, value_json);
            built = 1;
        }
    }
    if (built) {
        f = fopen(config_path, "w");
        if (f) {
            fputs(out, f);
            fclose(f);
        }
    }
    free(out);
    free(buf);
}

/* display_mirror_set(enabled) - Write to shared memory + persist to features.json */
static JSValue js_display_mirror_set(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->display_mirror = enabled ? 1 : 0;

    features_json_set("display_mirror_enabled", enabled ? "true" : "false");

    return JS_UNDEFINED;
}

/* display_mirror_set_shm(enabled) - Write to shared memory ONLY (no file I/O).
 * Safe to call from tick() for web→device config sync. */
static JSValue js_display_mirror_set_shm(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;
    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->display_mirror = enabled ? 1 : 0;
    return JS_UNDEFINED;
}

/* display_mirror_get() -> bool - Read from shared memory */
static JSValue js_display_mirror_get(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, shadow_control->display_mirror != 0);
}

/* set_pages_set(enabled) - Write to shared memory + persist to features.json */
static JSValue js_set_pages_set(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->set_pages_enabled = enabled ? 1 : 0;

    features_json_set("set_pages_enabled", enabled ? "true" : "false");

    return JS_UNDEFINED;
}

/* set_pages_set_shm(enabled) - Write to shared memory ONLY (no file I/O).
 * Safe to call from tick() for web→device config sync. */
static JSValue js_set_pages_set_shm(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;
    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->set_pages_enabled = enabled ? 1 : 0;
    return JS_UNDEFINED;
}

/* set_pages_get() -> bool - Read from shared memory */
static JSValue js_set_pages_get(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 1);
    return JS_NewBool(ctx, shadow_control->set_pages_enabled != 0);
}

/* midi_indicator_set(enabled) - Write to shared memory + persist to features.json.
 * The shim reads the SHM byte from the SPI callback path, so updates take effect
 * on the next frame without any file I/O on the realtime path. */
static JSValue js_midi_indicator_set(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->midi_indicator_enabled = enabled ? 1 : 0;

    /* Persist to features.json (off the SPI callback path - safe). */
    features_json_set("midi_indicator_enabled", enabled ? "true" : "false");

    return JS_UNDEFINED;
}

/* midi_indicator_get() -> bool - Read from shared memory */
static JSValue js_midi_indicator_get(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, shadow_control->midi_indicator_enabled != 0);
}

/* shadow_ui_trigger value names. Index matches the uint8 stored in shadow_control. */
static const char *SHADOW_UI_TRIGGER_NAMES[3] = {"long_press", "shift_vol", "both"};

static int clamp_shadow_ui_trigger(int v) {
    if (v < 0) return 0;
    if (v > 2) return 2;
    return v;
}

/* shadow_ui_trigger_set(mode) - Write to shared memory + persist to features.json.
 * mode: 0=long_press, 1=shift_vol, 2=both. */
static JSValue js_shadow_ui_trigger_set(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int mode = 0;
    JS_ToInt32(ctx, &mode, argv[0]);
    mode = clamp_shadow_ui_trigger(mode);
    shadow_control->shadow_ui_trigger = (uint8_t)mode;

    /* Persist to features.json. Any legacy "long_press_shadow" key lingers
     * harmlessly — load_feature_config prefers shadow_ui_trigger, and
     * install.sh rewrites features.json on next install. */
    char quoted_val[32];
    snprintf(quoted_val, sizeof(quoted_val), "\"%s\"", SHADOW_UI_TRIGGER_NAMES[mode]);
    features_json_set("shadow_ui_trigger", quoted_val);

    return JS_UNDEFINED;
}

/* shadow_ui_trigger_set_shm(mode) - Write to shared memory ONLY (no file I/O).
 * Safe to call from tick() for web→device config sync. */
static JSValue js_shadow_ui_trigger_set_shm(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;
    int mode = 0;
    JS_ToInt32(ctx, &mode, argv[0]);
    shadow_control->shadow_ui_trigger = (uint8_t)clamp_shadow_ui_trigger(mode);
    return JS_UNDEFINED;
}

/* shadow_ui_trigger_get() -> int (0=long_press, 1=shift_vol, 2=both) */
static JSValue js_shadow_ui_trigger_get(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 2);
    return JS_NewInt32(ctx, clamp_shadow_ui_trigger(shadow_control->shadow_ui_trigger));
}

/* skipback_shortcut_set(require_volume) - Write to shared memory + persist to features.json */
static JSValue js_skipback_shortcut_set(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int require_volume = 0;
    JS_ToInt32(ctx, &require_volume, argv[0]);
    shadow_control->skipback_require_volume = require_volume ? 1 : 0;

    features_json_set("skipback_require_volume", require_volume ? "true" : "false");

    return JS_UNDEFINED;
}

/* skipback_shortcut_get() -> bool - Read from shared memory */
static JSValue js_skipback_shortcut_get(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, shadow_control->skipback_require_volume != 0);
}

/* skipback_seconds_set(seconds) - Persist to features.json + write SHM.
 * Valid values: 30, 60, 120, 180, 240, 300. The shim watches SHM and
 * dispatches the buffer resize off the audio path. */
static JSValue js_skipback_seconds_set(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int seconds = 0;
    JS_ToInt32(ctx, &seconds, argv[0]);
    if (seconds < 30) seconds = 30;
    if (seconds > 300) seconds = 300;

    shadow_control->skipback_seconds = (uint16_t)seconds;

    char value_str[16];
    snprintf(value_str, sizeof(value_str), "%d", seconds);
    features_json_set("skipback_seconds", value_str);

    return JS_UNDEFINED;
}

/* skipback_seconds_get() -> int - Read currently configured length from SHM */
static JSValue js_skipback_seconds_get(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 30);
    int v = (int)shadow_control->skipback_seconds;
    if (v <= 0) v = 30;
    return JS_NewInt32(ctx, v);
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
    JS_SetPropertyStr(ctx, obj, "transportPlaying", JS_NewInt32(ctx, shadow_overlay->transport_playing));
    JS_SetPropertyStr(ctx, obj, "samplerBpm", JS_NewFloat64(ctx, shadow_overlay->sampler_bpm));

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

    /* Preroll state */
    JS_SetPropertyStr(ctx, obj, "samplerPrerollEnabled", JS_NewInt32(ctx, shadow_overlay->sampler_preroll_enabled));
    JS_SetPropertyStr(ctx, obj, "samplerPrerollActive", JS_NewInt32(ctx, shadow_overlay->sampler_preroll_active));
    JS_SetPropertyStr(ctx, obj, "samplerPrerollBarsDone", JS_NewInt32(ctx, shadow_overlay->sampler_preroll_bars_done));

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

#define PREVIEW_CMD_PATH "/data/UserData/schwung/preview_cmd_path.txt"

/* host_pad_block(enable) - suppress pad notes from reaching Move firmware */
static JSValue js_host_pad_block(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_FALSE;
    int val = 0;
    JS_ToInt32(ctx, &val, argv[0]);
    shadow_control->pad_block = val ? 1 : 0;
    shadow_ui_log_line(val ? "shadow_ui: pad_block ON" : "shadow_ui: pad_block OFF");
    return JS_TRUE;
}

/* host_preview_play(path) - play WAV file for browser preview via shim IPC */
static JSValue js_host_preview_play(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_FALSE;

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    FILE *f = fopen(PREVIEW_CMD_PATH, "w");
    if (f) {
        fputs(path, f);
        fclose(f);
    }
    JS_FreeCString(ctx, path);

    shadow_control->preview_cmd = 1;
    return JS_TRUE;
}

/* host_preview_stop() - stop preview playback via shim IPC */
static JSValue js_host_preview_stop(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    shadow_control->preview_cmd = 2;
    return JS_TRUE;
}

/* host_sampler_start(path) - start recording to custom path via shim IPC */
static JSValue js_host_sampler_start(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_FALSE;

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    /* Write path to file for shim to read */
    FILE *f = fopen(SAMPLER_CMD_PATH, "w");
    if (f) {
        fputs(path, f);
        fclose(f);
    }
    JS_FreeCString(ctx, path);

    /* Signal shim to start recording */
    shadow_control->sampler_cmd = 1;
    return JS_TRUE;
}

/* host_sampler_stop() - stop recording via shim IPC */
static JSValue js_host_sampler_stop(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    shadow_control->sampler_cmd = 2;
    return JS_TRUE;
}

/* host_sampler_pause() - pause recording via shim IPC */
static JSValue js_host_sampler_pause(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    shadow_control->sampler_cmd = 3;
    return JS_TRUE;
}

/* host_sampler_resume() - resume recording via shim IPC */
static JSValue js_host_sampler_resume(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    shadow_control->sampler_cmd = 4;
    return JS_TRUE;
}

/* host_sampler_is_paused() - query if sampler is paused */
static JSValue js_host_sampler_is_paused(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    return (shadow_control->sampler_state_val == 4) ? JS_TRUE : JS_FALSE;  /* 4 = SAMPLER_PAUSED */
}

/* host_sampler_is_recording() - query sampler state from shim */
static JSValue js_host_sampler_is_recording(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    return (shadow_control->sampler_state_val == 2) ? JS_TRUE : JS_FALSE;  /* 2 = SAMPLER_RECORDING */
}

/* host_speaker_active() -> bool. True when built-in speakers active (no headphones plugged). */
static JSValue js_host_speaker_active(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    return shadow_control->speaker_active ? JS_TRUE : JS_FALSE;
}

/* host_line_in_connected() -> bool. True when a cable is plugged into the line-in jack. */
static JSValue js_host_line_in_connected(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    return shadow_control->line_in_connected ? JS_TRUE : JS_FALSE;
}

/* host_get_module_metadata(id) -> object | null
 * Returns the parsed module.json contents for the given module id, or null
 * if not found. Used by feedback_gate to inspect capabilities/component_type. */
static JSValue js_host_get_module_metadata(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;

    /* Defense in depth: reject ids that could escape the modules tree. */
    if (strchr(id, '/') || strstr(id, "..")) {
        JS_FreeCString(ctx, id);
        return JS_NULL;
    }

    /* NOTE: parse_module_json in module_manager.c does similar file I/O but
     * parses into a module_info_t struct; this binding needs the raw JSValue
     * for JS-side inspection, so the read path is duplicated here. */
    /* Try each category dir until module.json found. */
    static const char *bases[] = {
        "/data/UserData/schwung/modules",
        "/data/UserData/schwung/modules/sound_generators",
        "/data/UserData/schwung/modules/audio_fx",
        "/data/UserData/schwung/modules/midi_fx",
        "/data/UserData/schwung/modules/tools",
    };
    char path[512];
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/module.json", bases[i], id);
        f = fopen(path, "r");
        if (f) break;
    }
    JS_FreeCString(ctx, id);
    if (!f) return JS_NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return JS_NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return JS_NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    JSValue parsed = JS_ParseJSON(ctx, buf, n, "module.json");
    free(buf);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, parsed);
        return JS_NULL;
    }
    return parsed;
}

/* host_sampler_set_source(source) - request sampler source change.
 * source: 0 = Resample (Schwung mix), 1 = Move Input (mic / line-in).
 * Applied by shim on next idle tick. Returns true if request submitted. */
static JSValue js_host_sampler_set_source(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_FALSE;
    int src = 0;
    JS_ToInt32(ctx, &src, argv[0]);
    /* shim encoding: 1 = Resample, 2 = Move Input (0 = "no request") */
    shadow_control->sampler_source_request = (src == 1) ? 2 : 1;
    return JS_TRUE;
}

/* host_sampler_set_silent(enabled) - suppress sampler screen-reader chatter.
 * Tools that record audio behind the user's back call set_silent(true) on
 * entry so the system "Sample saved" voice prompt stays out of the way of
 * their own replies. Reset to false on exit. */
static JSValue js_host_sampler_set_silent(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_FALSE;
    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->sampler_silent = enabled ? 1 : 0;
    return JS_TRUE;
}

/* host_sampler_get_samples_written() - return number of samples written so far */
static JSValue js_host_sampler_get_samples_written(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_overlay) return JS_NewInt32(ctx, 0);
    return JS_NewUint32(ctx, shadow_overlay->sampler_samples_written);
}

/* host_sampler_set_external_stop(flag) - set/clear external-stop-only mode */
static JSValue js_host_sampler_set_external_stop(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_FALSE;
    int val = 0;
    JS_ToInt32(ctx, &val, argv[0]);
    shadow_control->sampler_ext_stop = val ? 1 : 0;
    return JS_TRUE;
}

/* host_wake_all_slots() - clear idle flags on all shadow slots */
static JSValue js_host_wake_all_slots(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    shadow_control->wake_slots = 1;
    return JS_TRUE;
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
    JS_SetPropertyStr(ctx, global_obj, "shadow_inbound_pad_midi_active", JS_NewCFunction(ctx, js_shadow_inbound_pad_midi_active, "shadow_inbound_pad_midi_active", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_overtake_send_external_async_active", JS_NewCFunction(ctx, js_shadow_overtake_send_external_async_active, "shadow_overtake_send_external_async_active", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_begin", JS_NewCFunction(ctx, js_shadow_corun_begin, "shadow_corun_begin", 3));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_end", JS_NewCFunction(ctx, js_shadow_corun_end, "shadow_corun_end", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_state", JS_NewCFunction(ctx, js_shadow_corun_state, "shadow_corun_state", 0));
    /* Co-run target enum (matches corun_target_t in shadow_constants.h). */
    JS_SetPropertyStr(ctx, global_obj, "CORUN_TARGET_NONE",        JS_NewInt32(ctx, CORUN_TARGET_NONE));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_TARGET_CHAIN_EDIT",  JS_NewInt32(ctx, CORUN_TARGET_CHAIN_EDIT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_TARGET_MOVE_NATIVE", JS_NewInt32(ctx, CORUN_TARGET_MOVE_NATIVE));
    /* Control-surface group bitfield (matches CORUN_GRP_* in shadow_constants.h). */
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_OLED",          JS_NewInt32(ctx, CORUN_GRP_OLED));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_PADS",          JS_NewInt32(ctx, CORUN_GRP_PADS));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_STEPS",         JS_NewInt32(ctx, CORUN_GRP_STEPS));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_TRANSPORT",     JS_NewInt32(ctx, CORUN_GRP_TRANSPORT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_JOG",           JS_NewInt32(ctx, CORUN_GRP_JOG));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_TRACK_BUTTONS", JS_NewInt32(ctx, CORUN_GRP_TRACK_BUTTONS));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_KNOBS",         JS_NewInt32(ctx, CORUN_GRP_KNOBS));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_MASTER",        JS_NewInt32(ctx, CORUN_GRP_MASTER));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_SHIFT",         JS_NewInt32(ctx, CORUN_GRP_SHIFT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_BACK",          JS_NewInt32(ctx, CORUN_GRP_BACK));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_MENU",          JS_NewInt32(ctx, CORUN_GRP_MENU));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_TOUCH",         JS_NewInt32(ctx, CORUN_GRP_TOUCH));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_KEEP_DEFAULT",      JS_NewInt32(ctx, CORUN_KEEP_DEFAULT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_KEEP_BACK",         JS_NewInt32(ctx, CORUN_KEEP_BACK));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_open_tool_cmd",
        JS_NewCFunction(ctx, js_shadow_get_open_tool_cmd, "shadow_get_open_tool_cmd", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_selected_slot", JS_NewCFunction(ctx, js_shadow_get_selected_slot, "shadow_get_selected_slot", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_ui_slot", JS_NewCFunction(ctx, js_shadow_get_ui_slot, "shadow_get_ui_slot", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_shift_held", JS_NewCFunction(ctx, js_shadow_get_shift_held, "shadow_get_shift_held", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_display_mode", JS_NewCFunction(ctx, js_shadow_get_display_mode, "shadow_get_display_mode", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_move_ui_mode", JS_NewCFunction(ctx, js_shadow_get_move_ui_mode, "shadow_get_move_ui_mode", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_mode", JS_NewCFunction(ctx, js_shadow_set_overtake_mode, "shadow_set_overtake_mode", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_skip_led_clear", JS_NewCFunction(ctx, js_shadow_set_skip_led_clear, "shadow_set_skip_led_clear", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_suspend_overtake", JS_NewCFunction(ctx, js_shadow_set_suspend_overtake, "shadow_set_suspend_overtake", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_suspend_overtake", JS_NewCFunction(ctx, js_shadow_get_suspend_overtake, "shadow_get_suspend_overtake", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_consume_resume_last_tool", JS_NewCFunction(ctx, js_shadow_consume_resume_last_tool, "shadow_consume_resume_last_tool", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_mute_move_audio", JS_NewCFunction(ctx, js_host_mute_move_audio, "host_mute_move_audio", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_pad_led_snapshot", JS_NewCFunction(ctx, js_shadow_get_pad_led_snapshot, "shadow_get_pad_led_snapshot", 0));
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
    JS_SetPropertyStr(ctx, global_obj, "move_midi_inject_to_move", JS_NewCFunction(ctx, js_move_midi_inject_to_move, "move_midi_inject_to_move", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_ext_midi_remap_set", JS_NewCFunction(ctx, js_host_ext_midi_remap_set, "host_ext_midi_remap_set", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_ext_midi_remap_clear", JS_NewCFunction(ctx, js_host_ext_midi_remap_clear, "host_ext_midi_remap_clear", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_ext_midi_remap_enable", JS_NewCFunction(ctx, js_host_ext_midi_remap_enable, "host_ext_midi_remap_enable", 1));

    /* Register logging function for JS modules */
    JS_SetPropertyStr(ctx, global_obj, "shadow_log", JS_NewCFunction(ctx, js_shadow_log, "shadow_log", 1));
    JS_SetPropertyStr(ctx, global_obj, "unified_log", JS_NewCFunction(ctx, js_unified_log, "unified_log", 2));
    JS_SetPropertyStr(ctx, global_obj, "unified_log_enabled", JS_NewCFunction(ctx, js_unified_log_enabled, "unified_log_enabled", 0));

    /* Register host functions for store operations.
     * Shared bindings (host_file_exists, host_read_file(_base64),
     * host_write_file, host_http_download(_background),
     * host_http_request_background, host_extract_tar(_strip),
     * host_ensure_dir, host_remove_dir) come from js_host_common.c. */
    js_host_register_common(ctx);
    JS_SetPropertyStr(ctx, global_obj, "host_system_cmd", JS_NewCFunction(ctx, js_host_system_cmd, "host_system_cmd", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_list_modules", JS_NewCFunction(ctx, js_host_list_modules, "host_list_modules", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_rescan_modules", JS_NewCFunction(ctx, js_host_rescan_modules, "host_rescan_modules", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_track_event", JS_NewCFunction(ctx, js_host_track_event, "host_track_event", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_get_analytics_enabled", JS_NewCFunction(ctx, js_host_get_analytics_enabled, "host_get_analytics_enabled", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_set_analytics_enabled", JS_NewCFunction(ctx, js_host_set_analytics_enabled, "host_set_analytics_enabled", 1));
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
    JS_SetPropertyStr(ctx, global_obj, "display_mirror_set_shm", JS_NewCFunction(ctx, js_display_mirror_set_shm, "display_mirror_set_shm", 1));

    /* Register set pages functions */
    JS_SetPropertyStr(ctx, global_obj, "set_pages_set", JS_NewCFunction(ctx, js_set_pages_set, "set_pages_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "set_pages_get", JS_NewCFunction(ctx, js_set_pages_get, "set_pages_get", 0));
    JS_SetPropertyStr(ctx, global_obj, "set_pages_set_shm", JS_NewCFunction(ctx, js_set_pages_set_shm, "set_pages_set_shm", 1));

    /* Register MIDI channel indicator functions */
    JS_SetPropertyStr(ctx, global_obj, "midi_indicator_set", JS_NewCFunction(ctx, js_midi_indicator_set, "midi_indicator_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "midi_indicator_get", JS_NewCFunction(ctx, js_midi_indicator_get, "midi_indicator_get", 0));

    /* Register long-press shadow shortcut functions */
    JS_SetPropertyStr(ctx, global_obj, "shadow_ui_trigger_set", JS_NewCFunction(ctx, js_shadow_ui_trigger_set, "shadow_ui_trigger_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_ui_trigger_get", JS_NewCFunction(ctx, js_shadow_ui_trigger_get, "shadow_ui_trigger_get", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_ui_trigger_set_shm", JS_NewCFunction(ctx, js_shadow_ui_trigger_set_shm, "shadow_ui_trigger_set_shm", 1));

    /* Register skipback shortcut functions */
    JS_SetPropertyStr(ctx, global_obj, "skipback_shortcut_set", JS_NewCFunction(ctx, js_skipback_shortcut_set, "skipback_shortcut_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "skipback_shortcut_get", JS_NewCFunction(ctx, js_skipback_shortcut_get, "skipback_shortcut_get", 0));
    JS_SetPropertyStr(ctx, global_obj, "skipback_seconds_set", JS_NewCFunction(ctx, js_skipback_seconds_set, "skipback_seconds_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "skipback_seconds_get", JS_NewCFunction(ctx, js_skipback_seconds_get, "skipback_seconds_get", 0));

    /* Register overlay state functions (sampler/skipback state from shim) */
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_overlay_sequence", JS_NewCFunction(ctx, js_shadow_get_overlay_sequence, "shadow_get_overlay_sequence", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_overlay_state", JS_NewCFunction(ctx, js_shadow_get_overlay_state, "shadow_get_overlay_state", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_display_overlay", JS_NewCFunction(ctx, js_shadow_set_display_overlay, "shadow_set_display_overlay", 5));

    /* Register pad block function */
    JS_SetPropertyStr(ctx, global_obj, "host_pad_block", JS_NewCFunction(ctx, js_host_pad_block, "host_pad_block", 1));

    /* Register preview player functions */
    JS_SetPropertyStr(ctx, global_obj, "host_preview_play", JS_NewCFunction(ctx, js_host_preview_play, "host_preview_play", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_preview_stop", JS_NewCFunction(ctx, js_host_preview_stop, "host_preview_stop", 0));

    /* Register host hardware-state query functions (feedback gate, etc.) */
    JS_SetPropertyStr(ctx, global_obj, "host_speaker_active", JS_NewCFunction(ctx, js_host_speaker_active, "host_speaker_active", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_line_in_connected", JS_NewCFunction(ctx, js_host_line_in_connected, "host_line_in_connected", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_get_module_metadata", JS_NewCFunction(ctx, js_host_get_module_metadata, "host_get_module_metadata", 1));

    /* Register sampler control functions */
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_start", JS_NewCFunction(ctx, js_host_sampler_start, "host_sampler_start", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_stop", JS_NewCFunction(ctx, js_host_sampler_stop, "host_sampler_stop", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_is_recording", JS_NewCFunction(ctx, js_host_sampler_is_recording, "host_sampler_is_recording", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_set_source", JS_NewCFunction(ctx, js_host_sampler_set_source, "host_sampler_set_source", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_set_silent", JS_NewCFunction(ctx, js_host_sampler_set_silent, "host_sampler_set_silent", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_get_samples_written", JS_NewCFunction(ctx, js_host_sampler_get_samples_written, "host_sampler_get_samples_written", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_set_external_stop", JS_NewCFunction(ctx, js_host_sampler_set_external_stop, "host_sampler_set_external_stop", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_pause", JS_NewCFunction(ctx, js_host_sampler_pause, "host_sampler_pause", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_resume", JS_NewCFunction(ctx, js_host_sampler_resume, "host_sampler_resume", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_sampler_is_paused", JS_NewCFunction(ctx, js_host_sampler_is_paused, "host_sampler_is_paused", 0));
    JS_SetPropertyStr(ctx, global_obj, "host_wake_all_slots", JS_NewCFunction(ctx, js_host_wake_all_slots, "host_wake_all_slots", 0));

    JS_SetPropertyStr(ctx, global_obj, "exit", JS_NewCFunction(ctx, js_exit, "exit", 0));

    JS_FreeValue(ctx, global_obj);

    *prt = rt;
    *pctx = ctx;
}

static int process_shadow_midi(JSContext *ctx, JSValue *onInternal, JSValue *onExternal) {
    if (!shadow_ui_midi_shm) return 0;
    int handled = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        /* Acquire-load the gate byte: pairs with the producer's release-store
         * in shadow_ui_midi_publish() (schwung_shim.c). Ensures bytes 1-3 are
         * visible whenever byte 0 is nonzero. */
        uint8_t head = __atomic_load_n(&shadow_ui_midi_shm[i], __ATOMIC_ACQUIRE);
        uint8_t cin = head & 0x0F;
        uint8_t cable = (head >> 4) & 0x0F;

        /* CIN 0x04-0x07: SysEx, CIN 0x08-0x0E: Note/CC/etc */
        if (cin < 0x04 || cin > 0x0E) continue;
        uint8_t msg[3] = { shadow_ui_midi_shm[i + 1], shadow_ui_midi_shm[i + 2], shadow_ui_midi_shm[i + 3] };
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
        /* Release the slot back to the producer. Producer overwrites bytes
         * 1-3 unconditionally on next claim, so we only need to clear byte 0. */
        __atomic_store_n(&shadow_ui_midi_shm[i], 0, __ATOMIC_RELEASE);
    }
    return handled;
}

int main(int argc, char *argv[]) {
    const char *script = "/data/UserData/schwung/shadow/shadow_ui.js";
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

    /* Initialize analytics */
    {
        char version[32] = "unknown";
        FILE *vf = fopen("/data/UserData/schwung/host/version.txt", "r");
        if (vf) {
            if (fgets(version, sizeof(version), vf)) {
                char *nl = strchr(version, '\n');
                if (nl) *nl = '\0';
            }
            fclose(vf);
        }
        analytics_init(version);
        /* NOTE: app_launched is emitted from shadow_ui.js, not here. If we
         * emit from C, first-boot-after-install users miss the event —
         * analytics_enabled() is false until the JS opt-in prompt resolves,
         * so analytics_track() silently drops the call. */
    }

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
            /* process_shadow_midi releases each slot byte-0 individually after
             * dispatch — do NOT wholesale-memset here. Wiping the whole ring
             * after dispatch races with the shim writer and silently drops
             * events written between the shim's slot-empty check and our
             * clear (manifested as dropped pad note-offs under burst). */
            process_shadow_midi(ctx, &JSonMidiMessageInternal, &JSonMidiMessageExternal);
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
