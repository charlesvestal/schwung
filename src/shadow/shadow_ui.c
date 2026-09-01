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
#include "host/shadow_midi_inject_writer.h"
#include "../host/unified_log.h"
#include "../host/analytics.h"
#include "host/schwung_trace.h"   /* Phase 2: JS-side OTLP spans (js.tick, param.get) */

#define SAMPLER_CMD_PATH "/data/UserData/schwung/sampler_cmd_path.txt"

static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;
static shadow_ui_state_t *shadow_ui_state = NULL;
static shadow_param_t *shadow_param = NULL;
static shadow_midi_out_t *shadow_midi_out = NULL;
static shadow_midi_dsp_t *shadow_midi_dsp = NULL;
static shadow_midi_inject_t *shadow_midi_inject = NULL;
static shadow_midi_inject_t *shadow_midi_inject_ui = NULL;
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

    shadow_ui_midi_shm = (uint8_t *)shadow_shm_map(SHM_SHADOW_UI_MIDI, SHADOW_UI_MIDI_BYTES, 0, 0);
    if (!shadow_ui_midi_shm) return -1;

    shadow_control = (shadow_control_t *)shadow_shm_map(SHM_SHADOW_CONTROL, CONTROL_BUFFER_SIZE, 0, 0);
    if (!shadow_control) return -1;

    shadow_ui_state = (shadow_ui_state_t *)shadow_shm_map(SHM_SHADOW_UI, SHADOW_UI_BUFFER_SIZE, 0, 0);

    shadow_param = (shadow_param_t *)shadow_shm_map(SHM_SHADOW_PARAM, SHADOW_PARAM_BUFFER_SIZE, 0, 0);

    shadow_midi_out = (shadow_midi_out_t *)shadow_shm_map(SHM_SHADOW_MIDI_OUT, sizeof(shadow_midi_out_t), 0, 0);

    shadow_midi_dsp = (shadow_midi_dsp_t *)shadow_shm_map(SHM_SHADOW_MIDI_DSP, sizeof(shadow_midi_dsp_t), 0, 0);

    shadow_midi_inject = (shadow_midi_inject_t *)shadow_shm_map(SHM_SHADOW_MIDI_INJECT, sizeof(shadow_midi_inject_t), 0, 0);

    /* Our own inject ring. The shim drains this one into Move in every mode;
     * the segment above is the overtake test bus's, which during overtake is
     * republished to the module instead of reaching Move (see
     * shadow_overtake_midi.c). Absent only against a shim too old to create it,
     * where the push falls back and behaves as it did before. */
    shadow_midi_inject_ui = (shadow_midi_inject_t *)shadow_shm_map(SHM_SHADOW_MIDI_INJECT_UI, sizeof(shadow_midi_inject_t), 0, 0);

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

/*
 * shadow_get_slot_flags() -> [int, ...] | null   (bit0 = muted, bit1 = soloed)
 *
 * Built for the DRAW path, which is why it is separate from
 * shadow_get_slots(): no strings, no per-slot objects, one array of small
 * ints. The slot list needs mute/solo fresh on every frame — refreshSlots()
 * only runs every 120 ticks (~2.7s), and a mute glyph lagging a button press
 * by that long is not acceptable — but it used to get them with two get_param
 * calls per slot per draw. Eight synchronous round trips at ~2.9 ms each, and
 * measured on device they were 81% of every parameter read the UI made. This
 * is a shared-memory read: one binding crossing, ~490 ns, no IPC at all.
 *
 * Returns null against a v1 shim (fields absent → they would read 0, which is
 * indistinguishable from "nothing is muted"), so the caller can fall back.
 */
static JSValue js_shadow_get_slot_flags(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_ui_state) return JS_NULL;
    if (shadow_ui_state->version < 2) return JS_NULL;
    int count = shadow_ui_state->slot_count;
    if (count <= 0 || count > SHADOW_UI_SLOTS) count = SHADOW_UI_SLOTS;
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        int flags = (shadow_ui_state->slot_muted[i] ? 1 : 0)
                  | (shadow_ui_state->slot_soloed[i] ? 2 : 0);
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, flags));
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
 * Returns the UI flags from shared memory as ONE flat word.
 *
 * The flags live in two fields — `ui_flags` (bits 0-7) and `ui_flags_ext`
 * (bits 8+) — because `ui_flags` ran out of bits and cannot be widened
 * without moving every field behind it. See the note in shadow_constants.h.
 * JS is shown a single space so a caller never has to know which byte a flag
 * sits in; shadow_clear_ui_flags() splits the mask back apart.
 */
static JSValue js_shadow_get_ui_flags(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, (int)shadow_control->ui_flags |
                            ((int)shadow_control->ui_flags_ext << SHADOW_UI_FLAG_EXT_SHIFT));
}

/* shadow_get_open_tool_cmd() -> int (0=none, 1=open_tool; auto-clears) */
static JSValue js_shadow_get_open_tool_cmd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    uint8_t cmd = shadow_control->open_tool_cmd;
    if (cmd) shadow_control->open_tool_cmd = 0;  /* auto-clear */
    return JS_NewInt32(ctx, cmd);
}

static void features_json_set(const char *key, const char *value_json);

/* shadow_recall_quantize_set(v) -> void   (0=off, 1=beat, 2=bar, 3=two bars)
 *
 * Writes shadow_control_t.recall_quantize, which the shim reads when
 * Shift+Delete is pressed.
 */
static JSValue js_shadow_recall_quantize_set(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int v = 0;
    if (JS_ToInt32(ctx, &v, argv[0])) return JS_UNDEFINED;
    if (v < 0) v = 0;
    if (v > 3) v = 3;
    shadow_control->recall_quantize = (uint8_t)v;

    /* Persisted here rather than from JS, same as shadow_ui_trigger_set: the
     * register lives in SHM and does not survive a reboot, so the file is the
     * only copy that does. JS reads it back at startup and pushes it down. */
    static const char *NAMES[4] = { "off", "beat", "bar", "2bars" };
    char quoted[16];
    snprintf(quoted, sizeof(quoted), "\"%s\"", NAMES[v]);
    features_json_set("recall_quantize", quoted);
    return JS_UNDEFINED;
}

/* shadow_metronome_set(mode, level) -> void   (mode 0=off, 1=follow, 2=on)
 *
 * Writes shadow_control_t.metronome_mode / metronome_level, which the shim
 * reads on the SPI callback, and persists both to features.json — the register
 * lives in SHM and does not survive a reboot, exactly as for recall_quantize.
 */
static JSValue js_shadow_metronome_set(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_UNDEFINED;
    int mode = 0, level = 50;
    if (JS_ToInt32(ctx, &mode, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &level, argv[1])) return JS_UNDEFINED;
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    shadow_control->metronome_mode = (uint8_t)mode;
    shadow_control->metronome_level = (uint8_t)level;

    static const char *NAMES[3] = { "off", "follow", "on" };
    char quoted[16];
    snprintf(quoted, sizeof(quoted), "\"%s\"", NAMES[mode]);
    features_json_set("metronome_mode", quoted);
    snprintf(quoted, sizeof(quoted), "%d", level);
    features_json_set("metronome_level", quoted);
    return JS_UNDEFINED;
}

/* shadow_metronome_beats_set(n) -> void
 *
 * Bar length for the downbeat accent, from the set's time signature. NOT
 * persisted: it belongs to the set, and the shadow UI re-reads it on every
 * SET_CHANGED. 0 means unknown and the shim clamps to 4.
 */
static JSValue js_shadow_metronome_beats_set(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int n = 0;
    if (JS_ToInt32(ctx, &n, argv[0])) return JS_UNDEFINED;
    if (n < 0) n = 0;
    if (n > 32) n = 32;
    shadow_control->metronome_beats_per_bar = (uint8_t)n;
    return JS_UNDEFINED;
}

/* shadow_clear_ui_flags(mask) -> void
 * Clears the specified flags, splitting the flat mask back across the two
 * fields it came from in js_shadow_get_ui_flags.
 */
static JSValue js_shadow_clear_ui_flags(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int mask = 0;
    if (JS_ToInt32(ctx, &mask, argv[0])) return JS_UNDEFINED;
    shadow_control->ui_flags &= ~(uint8_t)(mask & 0xFF);
    shadow_control->ui_flags_ext &= ~(uint16_t)((unsigned)mask >> SHADOW_UI_FLAG_EXT_SHIFT);
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

/* shadow_overtake_move_inject_active() -> int
 * Capability sentinel: an overtake DSP's midi_inject_to_move callback uses a
 * dedicated queue that continues into Move while the takeover is active. */
static JSValue js_shadow_overtake_move_inject_active(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
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
    if (keep < 0 || keep > 0x7FFFFFFF) return JS_UNDEFINED;
    /* Reset LED-keep to "follow keep_mask"; a tool opts into the lights/input
     * split by calling shadow_corun_set_led_keep_mask() after begin. flags=0 =
     * legacy keep-list model (this is the legacy entry point). */
    shadow_control->corun.flags = 0;
    shadow_control->corun.led_keep_mask = 0;
    if (target == CORUN_TARGET_CHAIN_EDIT) {
        if (id < 0 || id >= SHADOW_UI_SLOTS) return JS_UNDEFINED;
        shadow_control->corun.keep_mask = (uint32_t)keep;
        shadow_control->corun.id = (int8_t)id;
        shadow_control->ui_slot = (uint8_t)id;
        shadow_control->corun.target = CORUN_TARGET_CHAIN_EDIT;
        /* chain-edit renders via shadow_ui; OLED owner stays SCHWUNG_UI. */
        shadow_control->shadow_display_owner = DISPLAY_OWNER_SCHWUNG_UI;
    } else if (target == CORUN_TARGET_MOVE_NATIVE) {
        if (id < 0 || id > 7) return JS_UNDEFINED;
        shadow_control->corun.keep_mask = (uint32_t)keep;
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
    shadow_control->corun.flags = 0;
    shadow_control->corun.keep_mask = 0;
    shadow_control->corun.led_keep_mask = 0;
    /* Returning to the tool's shadow session — shadow_ui resumes rendering. */
    shadow_control->shadow_display_owner = DISPLAY_OWNER_SCHWUNG_UI;
    return JS_UNDEFINED;
}

/* shadow_corun_set_led_keep_mask(mask) -> void
 * Co-run lights/input split: declare which CORUN_GRP_* groups the tool owns for
 * LED stripping, independent of the input keep_mask. Call after shadow_corun_begin.
 * 0 = follow keep_mask (the default). Lets a tool paint a surface (e.g. the
 * track-button clip indicator) while still ceding its presses to the peer. */
static JSValue js_shadow_corun_set_led_keep_mask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t mask = 0;
    JS_ToInt32(ctx, &mask, argv[0]);
    if (mask < 0 || mask > 0x7FFFFFFF) return JS_UNDEFINED;
    shadow_control->corun.led_keep_mask = (uint32_t)mask;
    return JS_UNDEFINED;
}

/* shadow_corun_overlay(active, keep_mask) -> void
 * Present (active=1) or dismiss (active=0) a registered shadow_ui view as a
 * temporary overlay over the active co-run target. Sets keep_mask and flips OLED
 * ownership WITHOUT touching corun.target, so the consumer tool's corun_state()
 * view and its state machine stay put (no teardown). On dismiss the OLED returns
 * to the underlay: Move firmware for a move_native underlay, shadow_ui otherwise. */
static JSValue js_shadow_corun_overlay(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_UNDEFINED;
    int active = 0, keep = 0;
    if (JS_ToInt32(ctx, &active, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &keep, argv[1])) return JS_UNDEFINED;
    if (keep < 0 || keep > 0x7FFFFFFF) return JS_UNDEFINED;
    shadow_control->corun.keep_mask = (uint32_t)keep;
    if (active) {
        shadow_control->shadow_display_owner = DISPLAY_OWNER_SCHWUNG_UI;
    } else {
        shadow_control->shadow_display_owner =
            (shadow_control->corun.target == CORUN_TARGET_MOVE_NATIVE)
                ? DISPLAY_OWNER_MOVE_FIRMWARE
                : DISPLAY_OWNER_SCHWUNG_UI;
    }
    return JS_UNDEFINED;
}

/* shadow_corun_begin_cede(target, id, cede_mask, flags) -> void
 * Cede-model entry point: the tool KEEPS the whole control surface and cedes only
 * the CORUN_GRP_* groups in cede_mask to the peer (keep-by-default). Optional
 * flags may set CORUN_F_OWN_BACK (tool handles Back itself instead of the
 * framework exit gesture). Mirrors shadow_corun_begin's target/id validation and
 * atomicity (mask + flags before target). Stored internally as the keep-mask
 * complement so every routing site keeps one representation. */
static JSValue js_shadow_corun_begin_cede(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_UNDEFINED;
    int target = -1, id = -1, cede = 0, flags = 0;
    if (JS_ToInt32(ctx, &target, argv[0])) return JS_UNDEFINED;
    if (JS_ToInt32(ctx, &id, argv[1])) return JS_UNDEFINED;
    if (argc >= 3 && JS_ToInt32(ctx, &cede, argv[2])) return JS_UNDEFINED;
    if (argc >= 4 && JS_ToInt32(ctx, &flags, argv[3])) return JS_UNDEFINED;
    if (cede < 0 || cede > 0x7FFFFFFF) return JS_UNDEFINED;
    uint8_t f = (uint8_t)(CORUN_F_CEDE_MODEL | ((uint32_t)flags & CORUN_F_OWN_BACK));
    /* LED follows input unless the tool later calls set_led_cede_mask. */
    shadow_control->corun.led_keep_mask = 0;
    if (target == CORUN_TARGET_CHAIN_EDIT) {
        if (id < 0 || id >= SHADOW_UI_SLOTS) return JS_UNDEFINED;
        shadow_control->corun.flags = f;
        shadow_control->corun.keep_mask = corun_cede_to_keep((uint32_t)cede);
        shadow_control->corun.id = (int8_t)id;
        shadow_control->ui_slot = (uint8_t)id;
        shadow_control->corun.target = CORUN_TARGET_CHAIN_EDIT;
        shadow_control->shadow_display_owner = DISPLAY_OWNER_SCHWUNG_UI;
    } else if (target == CORUN_TARGET_MOVE_NATIVE) {
        if (id < 0 || id > 7) return JS_UNDEFINED;
        shadow_control->corun.flags = f;
        shadow_control->corun.keep_mask = corun_cede_to_keep((uint32_t)cede);
        shadow_control->corun.id = (int8_t)id;
        shadow_control->corun.target = CORUN_TARGET_MOVE_NATIVE;
        shadow_control->shadow_display_owner = DISPLAY_OWNER_MOVE_FIRMWARE;
    } else {
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* shadow_corun_set_cede_mask(cede_mask) -> void
 * Update the cede-list mid-session (e.g. opening an overlay). Ensures cede model. */
static JSValue js_shadow_corun_set_cede_mask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t cede = 0;
    JS_ToInt32(ctx, &cede, argv[0]);
    if (cede < 0 || cede > 0x7FFFFFFF) return JS_UNDEFINED;
    shadow_control->corun.flags |= CORUN_F_CEDE_MODEL;
    shadow_control->corun.keep_mask = corun_cede_to_keep((uint32_t)cede);
    return JS_UNDEFINED;
}

/* shadow_corun_set_led_cede_mask(led_cede_mask) -> void
 * Cede-model LED split: cede these groups' LEDs to the peer, paint the rest. Sets
 * CORUN_F_LED_DISTINCT so led_keep_mask is authoritative even at 0 (= cede no
 * LEDs), rather than the legacy "0 follows input" sentinel. */
static JSValue js_shadow_corun_set_led_cede_mask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t cede = 0;
    JS_ToInt32(ctx, &cede, argv[0]);
    if (cede < 0 || cede > 0x7FFFFFFF) return JS_UNDEFINED;
    shadow_control->corun.flags |= CORUN_F_LED_DISTINCT;
    shadow_control->corun.led_keep_mask = corun_cede_to_keep((uint32_t)cede);
    return JS_UNDEFINED;
}

/* shadow_corun_state() -> { target, id, keep_mask, flags } | null
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
    JS_SetPropertyStr(ctx, obj, "flags", JS_NewInt32(ctx, (int)shadow_control->corun.flags));
    return obj;
}

/* shadow_corun_event_owner(status, d1) -> CORUN_OWNER_*
 * Single source of truth for "who owns this control-surface event right now",
 * exposed for JS dispatch decisions (e.g. a canvas overlay deciding whether to
 * consume an event or let it fall through to the tool). Wraps the C
 * corun_event_owner so the keep/cede spec, legacy carve-out, and Back handling
 * never drift between C and JS. `status` is the MIDI status byte (the low nibble
 * is ignored — only the type matters). Returns CORUN_OWNER_TOOL when no co-run. */
static JSValue js_shadow_corun_event_owner(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 2) return JS_NewInt32(ctx, CORUN_OWNER_TOOL);
    int status = 0, d1 = 0;
    if (JS_ToInt32(ctx, &status, argv[0])) return JS_NewInt32(ctx, CORUN_OWNER_TOOL);
    if (JS_ToInt32(ctx, &d1, argv[1])) return JS_NewInt32(ctx, CORUN_OWNER_TOOL);
    corun_owner_t owner = corun_event_owner(shadow_control, (uint8_t)(status & 0xF0), (uint8_t)d1);
    return JS_NewInt32(ctx, (int)owner);
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
            memset(shadow_ui_midi_shm, 0, SHADOW_UI_MIDI_BYTES);
        }
    } else {
        /* Clear the full-overtake sysex-suppression opt-in here, the one point
         * every exit path (exit/hide/complete/suspend) funnels through, so the
         * next overtake tool can't inherit a stale suppress flag. Mirrors the
         * shim's init-time reset. (led_keep_mask is already reset on the
         * co-run begin/end + Back-exit paths.) */
        shadow_control->overtake_suppress_sysex = 0;
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
 * Set skip_led_clear so the LED queue preserves native LEDs on overtake entry,
 * or (when set immediately before mode 0) skips snapshot replay and lets Move
 * repaint its native surface. The audio-side exit transition consumes the flag.
 */
static JSValue js_shadow_set_skip_led_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->skip_led_clear = flag ? 1 : 0;
    return JS_UNDEFINED;
}

/* shadow_restore_knob_leds() -> void
 * Hand the eight encoder-ring LEDs (CC 71-78) back to Move.
 *
 * The knob grid paints those rings; turning them off on the way out is not the
 * same as giving them back, because Move writes an LED only when its value
 * changes. The shim replays Move's own last value for each of the eight and
 * clears the flag, so this is an edge — call it once on leaving the grid. */
static JSValue js_shadow_restore_knob_leds(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_UNDEFINED;
    shadow_control->restore_knob_leds = 1;
    return JS_UNDEFINED;
}

/* shadow_set_overtake_fx_end_of_chain(flag) -> void
 * Opt an overtake audio-FX module into processing the FINAL mix (Move's own
 * tracks + the ME bus) instead of the ME bus alone. Lets a whole-mix effect
 * hear Move without Link Audio routing. Set from the module's `end_of_chain`
 * capability on load; cleared on overtake exit. */
static JSValue js_shadow_set_overtake_fx_end_of_chain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->overtake_fx_end_of_chain = flag ? 1 : 0;
    return JS_UNDEFINED;
}

/* shadow_set_overtake_suppress_sysex(flag) -> void
 * Opt a full-overtake tool into stripping Move's cable-0 sysex (RGB pad/clip/
 * grid LEDs) so Move's running-sequencer repaints don't fight the tool's LEDs.
 * Set on tool init; cleared automatically on overtake exit (and below). */
static JSValue js_shadow_set_overtake_suppress_sysex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->overtake_suppress_sysex = flag ? 1 : 0;
    return JS_UNDEFINED;
}

/* shadow_set_overtake_suppress_master_volume(flag) -> void
 * Opt a full-overtake tool into suppressing CC 79 / master-touch note 8's
 * hardcoded passthrough to Move firmware (and the matching OLED handoff in
 * shadow_swap_display()) for the duration the tool sets. Set/cleared around
 * the tool's own volume gesture; cleared automatically on shim init. */
static JSValue js_shadow_set_overtake_suppress_master_volume(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!shadow_control || argc < 1) return JS_UNDEFINED;
    int32_t flag = 0;
    JS_ToInt32(ctx, &flag, argv[0]);
    shadow_control->overtake_suppress_master_volume = flag ? 1 : 0;
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

/*
 * Claim the parameter channel, requiring BOTH that it is idle and that there
 * is no unread response sitting in it.
 *
 * The protocol had no acknowledgement step, so a published response had no
 * owner. `shadow_param_publish_response` sets response_ready = 1 and clears
 * request_type in the same breath, and the next claimer used to CAS on
 * request_type alone and then zero response_ready as part of filling in its
 * own request. A response could therefore be destroyed before the client that
 * asked for it ever looked at it — after which nothing would ever re-publish
 * it, and that client blocked until its 100ms deadline and returned null.
 *
 * That is the whole of the residual UI stall, and it is why the two earlier
 * fixes here did not touch it: the atomic claim settled who owns the REQUEST
 * slot, and releaseIfMine stopped an in-flight request being cleared, but
 * neither gave a RESPONSE a lifetime.
 *
 * Measured signature, consistently: claim=0ms (no contention at all),
 * response timed out, and at give-up request_type=0, response_ready=1 with a
 * response_id belonging to the other process. It also explains why pausing
 * schwung-manager made it disappear, and why the failure was always a full
 * timeout rather than a slow read — the answer was destroyed, not delayed.
 *
 * So the channel is now free only when request_type == 0 AND
 * response_ready == 0, and the owner clears response_ready once it has copied
 * its value out (shadow_param_consume). Both bytes live in the same 32-bit
 * word, so one compare-exchange tests and takes them together.
 *
 * Layout of that word (little-endian): byte0 request_type, byte1 slot,
 * byte2 response_ready, byte3 error. Carrying bytes 1-3 through unchanged
 * means a concurrent field write just makes the CAS retry.
 */
static inline volatile uint32_t *shadow_param_head_word(void) {
    return (volatile uint32_t *)&shadow_param->request_type;
}

#define SHADOW_PARAM_RT_MASK 0x000000FFu
#define SHADOW_PARAM_RR_MASK 0x00FF0000u

/*
 * If a response goes unread this long, take the channel anyway.
 *
 * Only reachable if a client died between being answered and consuming its
 * answer. Without it that would wedge every parameter read on the device
 * permanently, which is a far worse failure than the stale response this
 * discards. Comfortably longer than the 100ms request deadline, so it can
 * never fire against a client that is merely slow.
 */
#define SHADOW_PARAM_STEAL_AFTER_US 250000L

/*
 * Set when we deliberately walk away from a request without reading its
 * answer (the overtake fire-and-forget SET). Nobody will ever consume that
 * response, so the next claim may discard it immediately instead of waiting
 * out the steal timer — which at encoder-stream rates would otherwise stall
 * every write behind a dead response.
 */
static int shadow_param_orphan_pending = 0;

/*
 * When we first saw the channel idle-but-unread, or 0 if it is not in that
 * state. Monotonic microseconds.
 *
 * This has to live ACROSS calls. It used to be a per-call local, which made
 * the steal above unreachable: the deadline that bounds this loop is 100 ms
 * and the steal threshold is 250 ms, so the counter was reset to 0 on entry
 * and the loop always exited long before it could get there. An orphaned
 * response therefore wedged the channel until something happened to set
 * shadow_param_orphan_pending — and nothing outside overtake mode's
 * fire-and-forget SET ever does. "Longer than the request deadline" is the
 * right design; it just has to be measured against the channel's history
 * rather than one caller's attempt.
 */
static uint64_t shadow_param_blocked_since_us = 0;

/* State of the channel head word the last time a claim gave up (see the
 * give-up reporter below). */
static uint8_t  param_claim_fail_rt = 0;
static uint8_t  param_claim_fail_rr = 0;
static uint32_t param_claim_fail_blocked_ms = 0;

static uint64_t shadow_param_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static int shadow_param_claim(int timeout_ms) {
    long waited_us = 0;
    const long limit_us =
        (long)(timeout_ms > 0 ? timeout_ms : SHADOW_PARAM_DEFAULT_TIMEOUT_MS) * 1000L;

    while (waited_us < limit_us) {
        uint32_t w = __atomic_load_n(shadow_param_head_word(), __ATOMIC_ACQUIRE);
        int idle   = (w & SHADOW_PARAM_RT_MASK) == 0;
        int unread = (w & SHADOW_PARAM_RR_MASK) != 0;

        /* Track how long the channel has been sitting on an answer nobody is
         * collecting. Any other state means somebody is legitimately mid-flight,
         * so the clock restarts. */
        uint64_t blocked_us = 0;
        if (idle && unread) {
            uint64_t now = shadow_param_now_us();
            if (shadow_param_blocked_since_us == 0) {
                shadow_param_blocked_since_us = now;
            } else {
                blocked_us = now - shadow_param_blocked_since_us;
            }
        } else {
            shadow_param_blocked_since_us = 0;
        }

        if (idle && (!unread || shadow_param_orphan_pending
                     || blocked_us >= (uint64_t)SHADOW_PARAM_STEAL_AFTER_US)) {
            /* Take it, and clear any stale response in the same step so the
             * next waiter cannot mistake it for its own. */
            uint32_t nw = (w & ~(SHADOW_PARAM_RT_MASK | SHADOW_PARAM_RR_MASK))
                        | (uint32_t)SHADOW_PARAM_CLAIMED;
            if (__atomic_compare_exchange_n(shadow_param_head_word(), &w, nw,
                                            0 /* strong */,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                shadow_param_orphan_pending = 0;
                shadow_param_blocked_since_us = 0;
                return 1;
            }
            continue;   /* word moved under us — re-read, do not burn the deadline */
        }

        long step = (waited_us < 1000) ? SHADOW_PARAM_POLL_US : 1000;
        usleep((useconds_t)step);
        waited_us += step;
    }

    /* Record WHICH wedge beat us, so the give-up line can say so. The two look
     * identical in the counters but have opposite causes: request_type != 0 is
     * somebody else's request still in flight (or a requester that died holding
     * the channel), while request_type == 0 with response_ready == 1 is an
     * answer nobody collected. Without this the claim-stage give-up prints
     * "where=-" and the two are indistinguishable after the fact. */
    {
        uint32_t w = __atomic_load_n(shadow_param_head_word(), __ATOMIC_ACQUIRE);
        param_claim_fail_rt = (uint8_t)(w & SHADOW_PARAM_RT_MASK);
        param_claim_fail_rr = (uint8_t)((w & SHADOW_PARAM_RR_MASK) >> 16);
        param_claim_fail_blocked_ms =
            (shadow_param_blocked_since_us == 0)
                ? 0u
                : (uint32_t)((shadow_param_now_us()
                              - shadow_param_blocked_since_us) / 1000ull);
    }
    return 0;
}

/*
 * Release the response half of the channel.
 *
 * MUST be called once the value has been copied out, and equally on give-up —
 * until it is, no one may claim, which is exactly the point: it is what stops
 * the next requester destroying an answer somebody is still waiting for.
 */
static void shadow_param_consume(void) {
    __atomic_and_fetch(shadow_param_head_word(), ~SHADOW_PARAM_RR_MASK,
                       __ATOMIC_RELEASE);
}

/*
 * Report a param call that gave up, with WHICH STAGE gave up and on what key.
 *
 * A span carries a name, not a key, and the tail we are chasing is ~0.5% of
 * reads — rare enough that only naming the actual key and stage will identify
 * it. Aggregated and emitted at most once a second, because a diagnostic that
 * writes per event is how a previous round of ~150ms stalls got mistaken for
 * scheduling contention.
 */
static unsigned param_slow_n, param_slow_max_ms;
static char param_slow_key[SHADOW_PARAM_KEY_LEN];
static char param_slow_why[24];

/*
 * A read that took absurdly long but SUCCEEDED is the case that has resisted
 * three fixes, and neither a span nor a give-up counter can name it: spans
 * carry no key, and a success logs no give-up. So record the key, the
 * duration, and which side of the exchange was slow, for the worst one in
 * each one-second window.
 */
static void shadow_param_note_duration(const char *key, unsigned ms,
                                       const char *why) {
    if (ms < 90) return;
    param_slow_n++;
    if (ms > param_slow_max_ms) {
        param_slow_max_ms = ms;
        if (key) { strncpy(param_slow_key, key, sizeof(param_slow_key) - 1);
                   param_slow_key[sizeof(param_slow_key) - 1] = '\0'; }
        strncpy(param_slow_why, why, sizeof(param_slow_why) - 1);
        param_slow_why[sizeof(param_slow_why) - 1] = '\0';
    }
}

static void shadow_param_note_slow(const char *stage, const char *key) {
    static unsigned n_claim, n_resp, n_err;
    static char last_key[SHADOW_PARAM_KEY_LEN];
    static time_t last_report;

    if (stage) {
        if (stage[0] == 'c') n_claim++;
        else if (stage[0] == 'r') n_resp++;
        else n_err++;
        if (key) { strncpy(last_key, key, sizeof(last_key) - 1);
                   last_key[sizeof(last_key) - 1] = '\0'; }
    }

    time_t now = time(NULL);
    if (now == last_report) return;
    last_report = now;
    if (!(n_claim | n_resp | n_err | param_slow_n)) return;
    char line[400];
    snprintf(line, sizeof(line),
             "param_giveup: claim=%u response=%u error=%u last_key=%s"
             " | claimwedge: rt=%u rr=%u blocked=%ums"
             " | slow>=90ms: n=%u worst=%ums key=%s where=%s",
             n_claim, n_resp, n_err, last_key,
             param_claim_fail_rt, param_claim_fail_rr,
             param_claim_fail_blocked_ms,
             param_slow_n, param_slow_max_ms,
             param_slow_key[0] ? param_slow_key : "-",
             param_slow_why[0] ? param_slow_why : "-");
    shadow_ui_log_line(line);
    n_claim = n_resp = n_err = 0;
    param_slow_n = param_slow_max_ms = 0;
    param_slow_key[0] = param_slow_why[0] = '\0';
    /* Clear too, so a window with no claim failures reports 0/0 rather than
     * the previous window's wedge. */
    param_claim_fail_rt = param_claim_fail_rr = 0;
    param_claim_fail_blocked_ms = 0;
}

static int shadow_param_wait_response(uint32_t req_id, int timeout_ms) {
    /*
     * Backoff, NOT a fixed pre-sleep.
     *
     * A flat 2.2ms sleep before polling was tried and it made things worse:
     * measured, 14% of reads complete in under 2ms (the request lands just
     * before the shim's service point and is answered almost immediately), and
     * a floor under every read penalises all of them to fix the rare one.
     *
     * So: poll fine-grained at first to keep that fast path intact, then
     * lengthen the sleep. A read that has already waited a millisecond is
     * waiting on the next SPI frame (~2.9ms) and there is nothing to be gained
     * from asking 14 more times — each ask is a syscall and a chance to be
     * descheduled and not come back promptly, which is what stretched a 2.8ms
     * read into 150ms.
     */
    long waited_us = 0;
    const long limit_us = (timeout_ms > 0 ? timeout_ms : SHADOW_PARAM_DEFAULT_TIMEOUT_MS) * 1000L;
    while (waited_us < limit_us) {
        /* Acquire-load pairs with the writer's release-store of response_ready
         * (shadow_param_publish_response + the shim sites). It guarantees the
         * response fields (response_id, error, value) written before the flag
         * are visible once we observe response_ready == 1. A plain volatile
         * load is NOT an acquire on weakly-ordered ARMv8, so without this the
         * reader could match the flag yet read stale fields. */
        if (__atomic_load_n(&shadow_param->response_ready, __ATOMIC_ACQUIRE)
            && shadow_param->response_id == req_id) {
            return shadow_param->error ? -1 : 1;
        }
        /* 200us while the fast path is still plausible (first ~1ms), then
         * 1ms — by then we are simply waiting for the next SPI frame. Turns
         * a ~14-wakeup read into ~4 without slowing the quick ones. */
        long step = (waited_us < 1000) ? SHADOW_PARAM_POLL_US : 1000;
        usleep((useconds_t)step);
        waited_us += step;
    }
    return 0;
}

static int shadow_set_param_common(int slot, const char *key, const char *value, int timeout_ms, int force_blocking) {
    const int overtake_fire_and_forget = !force_blocking && (shadow_control && shadow_control->overtake_mode >= 2);

    /* Span the write the same way param.get spans the read. This path is only
     * fire-and-forget under overtake (mode >= 2); everywhere else — including
     * the param-pages knob grid, which is a shadow UI view and not an overtake
     * module — it is a synchronous round-trip to the shim, serviced once per
     * SPI frame. That makes a knob stream a sequence of frame-quantised waits
     * rather than the cheap writes the JS comments describe, and until now the
     * read path was instrumented and the write path was not. */
    TRACE_SCOPE("param.set");

    {
        /* Claim on BOTH paths. Fire-and-forget used to write key, value, slot
         * and request_id with no claim at all, which could land on top of a
         * request another process had in flight — the very race the claim
         * exists to prevent. It just claims briefly and drops the write if the
         * channel is busy, which is what "fire and forget" already means.
         *
         * Split out so the trace distinguishes the two ways this blocks:
         * waiting for the single SHM slot to free (contention with another
         * request) from waiting for the shim to service ours (frame
         * quantisation). They call for different fixes. */
        int idle;
        { TRACE_SCOPE("param.set.idle");
          idle = shadow_param_claim(overtake_fire_and_forget ? 5 : timeout_ms); }
        if (!idle) {
            return 0;
        }
    }

    uint32_t req_id = shadow_param_next_request_id();

    /* Copy key and value to shared memory */
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    strncpy(shadow_param->value, value, SHADOW_PARAM_VALUE_LEN - 1);
    shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';

    /* Propagate the open param.set span so the shim emits its param.serve as
     * our child, exactly as get_param does (0/0 when tracing is off). Until
     * this was added the SET path left these fields holding a PRIOR GET's
     * context, which is why shadow_chain_mgmt.c deliberately ignored them on a
     * SET and let param.serve float as its own root — a served SET could not
     * be tied back to the JS call that asked for it. */
    uint64_t _tr = 0, _sp = 0;
    schwung_trace_current(&_tr, &_sp);
    shadow_param->trace_id = _tr;
    shadow_param->parent_span_id = _sp;

    /* Set up request */
    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->response_id = 0;
    shadow_param->request_id = req_id;
    /* Release-store the commit flag so the shim's acquire-load sees all the
     * request fields written above (incl. trace context) before it acts. */
    __atomic_store_n(&shadow_param->request_type, (uint8_t)1, __ATOMIC_RELEASE);  /* SET */

    /* In overtake module mode, keep this fire-and-forget so rapid encoder
     * streams do not block UI rendering. */
    if (overtake_fire_and_forget) {
        /* We will never read the answer; let the next claim bin it. */
        shadow_param_orphan_pending = 1;
        return 1;
    }

    int ok = shadow_param_wait_response(req_id, timeout_ms) > 0;
    shadow_param_consume();
    return ok;
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

/* Bulk param request (request_type 3 = BULK_GET, 4 = BULK_SET).
 * argv: (slot, key, valueBlob). key is the routing marker ("overtake_dsp:");
 * valueBlob is the length-prefixed payload (see shim_handle_param_bulk):
 * keys for GET, key/value pairs for SET. Collapses N param round-trips into
 * one. Returns the response blob string (GET), JS_TRUE (SET), or null on
 * failure. Uses ToCStringLen / NewStringLen so binary-ish blobs survive. */
static JSValue shadow_param_bulk_js(JSContext *ctx, int argc, JSValueConst *argv,
                                    uint8_t req_type) {
    if (!shadow_param || argc < 3) return JS_NULL;
    int slot = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_NULL;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return JS_NULL;
    const char *key = JS_ToCString(ctx, argv[1]);
    if (!key) return JS_NULL;
    size_t vlen = 0;
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[2]);
    if (!value) { JS_FreeCString(ctx, key); return JS_NULL; }
    if (vlen >= SHADOW_PARAM_VALUE_LEN) vlen = SHADOW_PARAM_VALUE_LEN - 1;

    if (!shadow_param_claim(SHADOW_PARAM_DEFAULT_TIMEOUT_MS)) {
        JS_FreeCString(ctx, key); JS_FreeCString(ctx, value); return JS_NULL;
    }
    uint32_t req_id = shadow_param_next_request_id();
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    memcpy(shadow_param->value, value, vlen);
    shadow_param->value[vlen] = '\0';
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);

    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->response_id = 0;
    shadow_param->request_id = req_id;
    /* Release-store the commit flag so the shim's acquire-load sees the key /
     * value payload and request_id written above before it acts on them. */
    __atomic_store_n(&shadow_param->request_type, req_type, __ATOMIC_RELEASE);

    if (shadow_param_wait_response(req_id, SHADOW_PARAM_DEFAULT_TIMEOUT_MS) <= 0) {
        shadow_param_consume();
        return JS_NULL;
    }
    if (req_type == 4) { shadow_param_consume(); return JS_TRUE; }  /* BULK_SET */
    if (shadow_param->error) { shadow_param_consume(); return JS_NULL; }
    int rlen = shadow_param->result_len;
    if (rlen < 0) { shadow_param_consume(); return JS_NULL; }
    if (rlen >= SHADOW_PARAM_VALUE_LEN) rlen = SHADOW_PARAM_VALUE_LEN - 1;
    JSValue bout = JS_NewStringLen(ctx, shadow_param->value, (size_t)rlen);
    shadow_param_consume();
    return bout;
}

static JSValue js_shadow_get_params(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val; return shadow_param_bulk_js(ctx, argc, argv, 3);
}

static JSValue js_shadow_set_params(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val; return shadow_param_bulk_js(ctx, argc, argv, 4);
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

    /* Span the synchronous round-trip (busy-wait to the shim, serviced once per
     * SPI frame). Correlates on the timeline with the shim's param.serve span —
     * this is the "where does the tick time go" measurement. Closes on every
     * return below via cleanup. */
    TRACE_SCOPE("param.get");

    /* Claim and response are spanned SEPARATELY because the two failure modes
     * are completely different problems and the aggregate cannot tell them
     * apart: time in `claim` means somebody else is holding the channel, time
     * in `response` means our request was accepted and never answered. The
     * ~0.5%-of-reads timeout tail survived two fixes aimed at the first, which
     * is exactly the sort of thing an undifferentiated span hides. */
    struct timespec _t0, _t1, _t2;
    clock_gettime(CLOCK_MONOTONIC, &_t0);
    int claimed;
    { TRACE_SCOPE("param.get.claim");
      claimed = shadow_param_claim(SHADOW_PARAM_DEFAULT_TIMEOUT_MS); }
    clock_gettime(CLOCK_MONOTONIC, &_t1);
    if (!claimed) {
        shadow_param_note_slow("claim", key);
        JS_FreeCString(ctx, key);
        return JS_NULL;
    }

    uint32_t req_id = shadow_param_next_request_id();

    /* Copy key to shared memory */
    strncpy(shadow_param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    shadow_param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    /* Clear entire value buffer to prevent any stale data */
    memset(shadow_param->value, 0, SHADOW_PARAM_VALUE_LEN);

    /* Keep the key for the slow-read report below; the JS string is freed here
     * because the shim already has its own copy in SHM. */
    char key_copy[SHADOW_PARAM_KEY_LEN];
    strncpy(key_copy, key, sizeof(key_copy) - 1);
    key_copy[sizeof(key_copy) - 1] = '\0';
    JS_FreeCString(ctx, key);

    /* Propagate the open param.get span as trace context so the shim emits its
     * param.serve as our child (0/0 when tracing is off). */
    uint64_t _tr = 0, _sp = 0;
    schwung_trace_current(&_tr, &_sp);
    shadow_param->trace_id = _tr;
    shadow_param->parent_span_id = _sp;

    /* Set up request */
    shadow_param->slot = (uint8_t)slot;
    shadow_param->response_ready = 0;
    shadow_param->error = 0;
    shadow_param->response_id = 0;
    shadow_param->request_id = req_id;
    /* Release-store the commit flag so the shim's acquire-load sees trace_id /
     * parent_span_id / key (written above) before it services this GET. */
    __atomic_store_n(&shadow_param->request_type, (uint8_t)2, __ATOMIC_RELEASE);  /* GET */

    int got;
    { TRACE_SCOPE("param.get.response");
      got = shadow_param_wait_response(req_id, SHADOW_PARAM_DEFAULT_TIMEOUT_MS); }
    clock_gettime(CLOCK_MONOTONIC, &_t2);
    {
        unsigned claim_ms = (unsigned)((_t1.tv_sec - _t0.tv_sec) * 1000
                          + (_t1.tv_nsec - _t0.tv_nsec) / 1000000);
        unsigned resp_ms  = (unsigned)((_t2.tv_sec - _t1.tv_sec) * 1000
                          + (_t2.tv_nsec - _t1.tv_nsec) / 1000000);
        char why[48];   /* "FAIL r=150 rt=0 rr=1 other" is 26 — 24 truncated it */
        /* On failure, snapshot the channel: rt!=0 means the shim never picked
         * our request up; rt==0 with rr==1 and a foreign rid means it answered
         * somebody else over the top of us. */
        snprintf(why, sizeof(why), "%s r=%u rt=%u rr=%u %s",
                 got > 0 ? "OK" : "FAIL", resp_ms,
                 (unsigned)shadow_param->request_type,
                 (unsigned)shadow_param->response_ready,
                 (shadow_param->response_id == req_id) ? "mine" : "other");
        shadow_param_note_duration(key_copy, claim_ms + resp_ms, why);
    }
    if (got <= 0) {
        /* Nothing usable, but release the response half anyway: an answer that
         * landed just after we stopped waiting must not be left for the next
         * requester to mistake for its own. */
        shadow_param_consume();
        shadow_param_note_slow(got == 0 ? "response-timeout" : "error", key);
        return JS_NULL;
    }

    /*
     * Copy the value, then CHECK IT IS STILL OURS.
     *
     * With claim() now refusing a channel that still holds an unread response,
     * nobody can take it from under us here — this check is belt and braces
     * for a stale or stolen response, and it is cheap.
     */
    JSValue out = JS_NewString(ctx, shadow_param->value);
    int still_ours = (shadow_param->response_id == req_id);
    shadow_param_consume();   /* release the response half for the next claimer */
    if (!still_ours) {
        JS_FreeValue(ctx, out);
        return JS_NULL;
    }
    return out;
}

/* === MIDI output functions for overtake modules === */

/* Packets discarded because the shadow-UI MIDI-out SHM buffer was full when
 * the caller wrote. Was silently zero-information before: the write returned
 * success either way. See js_shadow_midi_send. */
static long shadow_midi_out_drops = 0;


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
    int dropped = 0;
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
            shadow_midi_out->write_idx = (uint16_t)(write_offset + 4);
        } else {
            dropped++;
        }
    }

    /* Signal shim that data is ready */
    shadow_midi_out->ready++;

    /* A write that discards and reports success is how an LED goes permanently
     * wrong: input_filter's setLED records the colour it believes the hardware
     * now shows and suppresses the next identical repaint, so a packet lost
     * here is never retried. Report the failure so the caller can decline to
     * cache it, and count it so "sometimes drops LEDs" is a number rather than
     * a feeling. Logging here is safe — shadow_ui is a separate SCHED_OTHER
     * process, not the SPI callback — but it is rate-limited so a flood cannot
     * turn a dropped LED into a dropped audio block. */
    if (dropped) {
        shadow_midi_out_drops += dropped;
        static time_t last_report = 0;
        time_t now = time(NULL);
        if (now != last_report) {
            last_report = now;
            unified_log("shadow_ui", LOG_LEVEL_DEBUG,
                        "shadow MIDI out: buffer full, dropped %d packet(s) "
                        "(%ld total) - more than %d bytes queued in one flush",
                        dropped, shadow_midi_out_drops,
                        SHADOW_MIDI_OUT_BUFFER_SIZE);
        }
        return JS_FALSE;
    }

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
 *
 * Packets go into the UI's OWN ring (/schwung-midi-inject-ui), not the
 * test bus's. While overtake is active the shim pops the test-bus ring
 * onto the module — publishing it back to this very process as if a
 * control had been pressed — so a tool that injects a button CC there is
 * writing its own input. song-mode did, and its Play CC re-entered
 * onMidiMessageInternal and toggled playback on a loop.
 */
static JSValue js_move_midi_inject_to_move(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val;
    shadow_midi_inject_t *ring = shadow_midi_inject_ui ? shadow_midi_inject_ui
                                                       : shadow_midi_inject;
    if (!ring) return JS_FALSE;
    if (argc < 1) return JS_FALSE;

    JSValueConst arr = argv[0];
    if (!JS_IsArray(ctx, arr)) return JS_FALSE;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    /* Process 4 bytes at a time (USB-MIDI packet format).
     * Each packet goes through the MPSC helper (shadow_midi_inject_push),
     * which CAS-reserves a slot and publishes it with a release-store on
     * the slot's seq. The helper handles the cross-process race with the
     * other producers (the shim's own writers, the test daemon) — see
     * shadow_midi_inject_writer.h. */
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

        if (shadow_midi_inject_push(ring, packet) != 0) {
            /* Ring full (drain starved) — drop this packet and stop the
             * batch (subsequent packets would just back up). */
            break;
        }
    }

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

/* stay_in_shadow_set(enabled) - Write to shared memory + persist to features.json.
 * The shim reads the SHM byte from the SPI callback path, so the toggle takes
 * effect on the next Track tap without any file I/O on the realtime path. */
static JSValue js_stay_in_shadow_set(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->stay_in_shadow = enabled ? 1 : 0;

    /* Persist to features.json (off the SPI callback path - safe). */
    features_json_set("stay_in_shadow", enabled ? "true" : "false");

    return JS_UNDEFINED;
}

/* stay_in_shadow_set_shm(enabled) - Write to shared memory ONLY (no file I/O).
 * Safe to call from tick() for web->device config sync. */
static JSValue js_stay_in_shadow_set_shm(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;
    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->stay_in_shadow = enabled ? 1 : 0;
    return JS_UNDEFINED;
}

/* stay_in_shadow_get() -> bool - Read from shared memory.
 * Unmapped answers the DEFAULT (on), the way shadow_ui_trigger_get answers 2 —
 * a fallback that reports the opposite of the default draws the switch wrong
 * for the frames before the segment is there. */
static JSValue js_stay_in_shadow_get(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 1);
    return JS_NewBool(ctx, shadow_control->stay_in_shadow != 0);
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

/* === OTLP tracing bindings (Phase 2) ===
 * host_trace_begin(name) -> handle (small int; 0 when tracing is off / no slot)
 * host_trace_end(handle)  -> closes the span opened by host_trace_begin.
 * Single-threaded shadow_ui → the per-thread span stack nests correctly, so
 * spans opened inside the C js.tick scope become its children. A forgotten
 * end() can't corrupt the stack (schwung_trace_end unwinds to the matching id);
 * the only cost is the leaked handle-table slot, which is reclaimed at the end
 * of each js.tick (the table is per-tick — see the main loop). span_ids are
 * 64-bit (process-salted) and don't fit a JS double, so handles are small
 * indices into a table that holds the real span_id. Off by default. */
#define JS_TRACE_MAX_OPEN 16
static uint64_t js_trace_open[JS_TRACE_MAX_OPEN];   /* full span_id per slot; 0 = free */

static JSValue js_host_trace_begin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !atomic_load_explicit(&schwung_trace_on, memory_order_relaxed))
        return JS_NewInt32(ctx, 0);
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewInt32(ctx, 0);
    trace_handle_t h = schwung_trace_begin(schwung_trace_intern_copy(name));
    JS_FreeCString(ctx, name);
    if (h.span_id == 0) return JS_NewInt32(ctx, 0);
    for (int i = 0; i < JS_TRACE_MAX_OPEN; i++) {
        if (js_trace_open[i] == 0) { js_trace_open[i] = h.span_id; return JS_NewInt32(ctx, i + 1); }
    }
    /* table full: span is open on the stack but unaddressable; an enclosing
     * end() will unwind (drop) it. Return 0 so JS doesn't try to close it.
     * Warn once so the silent drop is diagnosable. */
    static int warned_full = 0;
    if (!warned_full) {
        warned_full = 1;
        unified_log("trace", LOG_LEVEL_WARN,
                    "js_trace_open full (%d open) — JS span dropped; "
                    "raise JS_TRACE_MAX_OPEN or check for missing host_trace_end",
                    JS_TRACE_MAX_OPEN);
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_host_trace_end(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t idx = 0;
    if (JS_ToInt32(ctx, &idx, argv[0]) || idx <= 0 || idx > JS_TRACE_MAX_OPEN) return JS_UNDEFINED;
    uint64_t sid = js_trace_open[idx - 1];
    if (sid == 0) return JS_UNDEFINED;
    js_trace_open[idx - 1] = 0;
    trace_handle_t h = { sid };
    schwung_trace_end(&h);
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
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_slot_flags", JS_NewCFunction(ctx, js_shadow_get_slot_flags, "shadow_get_slot_flags", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_request_patch", JS_NewCFunction(ctx, js_shadow_request_patch, "shadow_request_patch", 2));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_focused_slot", JS_NewCFunction(ctx, js_shadow_set_focused_slot, "shadow_set_focused_slot", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_ui_flags", JS_NewCFunction(ctx, js_shadow_get_ui_flags, "shadow_get_ui_flags", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_recall_quantize_set", JS_NewCFunction(ctx, js_shadow_recall_quantize_set, "shadow_recall_quantize_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_metronome_set", JS_NewCFunction(ctx, js_shadow_metronome_set, "shadow_metronome_set", 2));
    JS_SetPropertyStr(ctx, global_obj, "shadow_metronome_beats_set", JS_NewCFunction(ctx, js_shadow_metronome_beats_set, "shadow_metronome_beats_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_clear_ui_flags", JS_NewCFunction(ctx, js_shadow_clear_ui_flags, "shadow_clear_ui_flags", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_inbound_pad_midi_active", JS_NewCFunction(ctx, js_shadow_inbound_pad_midi_active, "shadow_inbound_pad_midi_active", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_overtake_move_inject_active", JS_NewCFunction(ctx, js_shadow_overtake_move_inject_active, "shadow_overtake_move_inject_active", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_overtake_send_external_async_active", JS_NewCFunction(ctx, js_shadow_overtake_send_external_async_active, "shadow_overtake_send_external_async_active", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_begin", JS_NewCFunction(ctx, js_shadow_corun_begin, "shadow_corun_begin", 3));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_end", JS_NewCFunction(ctx, js_shadow_corun_end, "shadow_corun_end", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_set_led_keep_mask", JS_NewCFunction(ctx, js_shadow_corun_set_led_keep_mask, "shadow_corun_set_led_keep_mask", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_state", JS_NewCFunction(ctx, js_shadow_corun_state, "shadow_corun_state", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_overlay", JS_NewCFunction(ctx, js_shadow_corun_overlay, "shadow_corun_overlay", 2));
    /* Cede-default model API (keep-by-default; tool lists only what it cedes). */
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_begin_cede", JS_NewCFunction(ctx, js_shadow_corun_begin_cede, "shadow_corun_begin_cede", 4));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_set_cede_mask", JS_NewCFunction(ctx, js_shadow_corun_set_cede_mask, "shadow_corun_set_cede_mask", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_set_led_cede_mask", JS_NewCFunction(ctx, js_shadow_corun_set_led_cede_mask, "shadow_corun_set_led_cede_mask", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_corun_event_owner", JS_NewCFunction(ctx, js_shadow_corun_event_owner, "shadow_corun_event_owner", 2));
    /* Owner enum (matches corun_owner_t) — for JS dispatch decisions. */
    JS_SetPropertyStr(ctx, global_obj, "CORUN_OWNER_TOOL", JS_NewInt32(ctx, CORUN_OWNER_TOOL));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_OWNER_PEER", JS_NewInt32(ctx, CORUN_OWNER_PEER));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_OWNER_BOTH", JS_NewInt32(ctx, CORUN_OWNER_BOTH));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_OWNER_NONE", JS_NewInt32(ctx, CORUN_OWNER_NONE));
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
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_MUTE",          JS_NewInt32(ctx, CORUN_GRP_MUTE));
    /* Extended buttons — now first-class, published so modules stop hand-copying. */
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_PLAY",          JS_NewInt32(ctx, CORUN_GRP_PLAY));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_REC",           JS_NewInt32(ctx, CORUN_GRP_REC));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_SAMPLE",        JS_NewInt32(ctx, CORUN_GRP_SAMPLE));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_LOOP",          JS_NewInt32(ctx, CORUN_GRP_LOOP));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_COPY",          JS_NewInt32(ctx, CORUN_GRP_COPY));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_DELETE",        JS_NewInt32(ctx, CORUN_GRP_DELETE));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_UNDO",          JS_NewInt32(ctx, CORUN_GRP_UNDO));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_CAPTURE",       JS_NewInt32(ctx, CORUN_GRP_CAPTURE));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_NAV_UP",        JS_NewInt32(ctx, CORUN_GRP_NAV_UP));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_NAV_DOWN",      JS_NewInt32(ctx, CORUN_GRP_NAV_DOWN));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_NAV_LEFT",      JS_NewInt32(ctx, CORUN_GRP_NAV_LEFT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_NAV_RIGHT",     JS_NewInt32(ctx, CORUN_GRP_NAV_RIGHT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_NAV",           JS_NewInt32(ctx, CORUN_GRP_NAV));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_GRP_EDIT",          JS_NewInt32(ctx, CORUN_GRP_EDIT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_KEEP_DEFAULT",      JS_NewInt32(ctx, CORUN_KEEP_DEFAULT));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_KEEP_BACK",         JS_NewInt32(ctx, CORUN_KEEP_BACK));
    JS_SetPropertyStr(ctx, global_obj, "CORUN_F_OWN_BACK",        JS_NewInt32(ctx, CORUN_F_OWN_BACK));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_open_tool_cmd",
        JS_NewCFunction(ctx, js_shadow_get_open_tool_cmd, "shadow_get_open_tool_cmd", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_selected_slot", JS_NewCFunction(ctx, js_shadow_get_selected_slot, "shadow_get_selected_slot", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_ui_slot", JS_NewCFunction(ctx, js_shadow_get_ui_slot, "shadow_get_ui_slot", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_shift_held", JS_NewCFunction(ctx, js_shadow_get_shift_held, "shadow_get_shift_held", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_display_mode", JS_NewCFunction(ctx, js_shadow_get_display_mode, "shadow_get_display_mode", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_move_ui_mode", JS_NewCFunction(ctx, js_shadow_get_move_ui_mode, "shadow_get_move_ui_mode", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_mode", JS_NewCFunction(ctx, js_shadow_set_overtake_mode, "shadow_set_overtake_mode", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_skip_led_clear", JS_NewCFunction(ctx, js_shadow_set_skip_led_clear, "shadow_set_skip_led_clear", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_restore_knob_leds", JS_NewCFunction(ctx, js_shadow_restore_knob_leds, "shadow_restore_knob_leds", 0));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_suppress_sysex", JS_NewCFunction(ctx, js_shadow_set_overtake_suppress_sysex, "shadow_set_overtake_suppress_sysex", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_suppress_master_volume", JS_NewCFunction(ctx, js_shadow_set_overtake_suppress_master_volume, "shadow_set_overtake_suppress_master_volume", 1));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_overtake_fx_end_of_chain", JS_NewCFunction(ctx, js_shadow_set_overtake_fx_end_of_chain, "shadow_set_overtake_fx_end_of_chain", 1));
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
    JS_SetPropertyStr(ctx, global_obj, "shadow_get_params", JS_NewCFunction(ctx, js_shadow_get_params, "shadow_get_params", 3));
    JS_SetPropertyStr(ctx, global_obj, "shadow_set_params", JS_NewCFunction(ctx, js_shadow_set_params, "shadow_set_params", 3));

    /* OTLP tracing (Phase 2) */
    JS_SetPropertyStr(ctx, global_obj, "host_trace_begin", JS_NewCFunction(ctx, js_host_trace_begin, "host_trace_begin", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_trace_end", JS_NewCFunction(ctx, js_host_trace_end, "host_trace_end", 1));

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
    JS_SetPropertyStr(ctx, global_obj, "stay_in_shadow_set", JS_NewCFunction(ctx, js_stay_in_shadow_set, "stay_in_shadow_set", 1));
    JS_SetPropertyStr(ctx, global_obj, "stay_in_shadow_get", JS_NewCFunction(ctx, js_stay_in_shadow_get, "stay_in_shadow_get", 0));
    JS_SetPropertyStr(ctx, global_obj, "stay_in_shadow_set_shm", JS_NewCFunction(ctx, js_stay_in_shadow_set_shm, "stay_in_shadow_set_shm", 1));
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
    for (int i = 0; i < SHADOW_UI_MIDI_BYTES; i += 4) {
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

/* =========================================================================
 * Loop profiler — where a shadow_ui iteration actually spends its time.
 *
 * The trace spans `js.tick` and its param children, which is most of the
 * iteration but NOT all of it. Three things run every loop and carry no
 * instrument at all:
 *
 *   - process_shadow_midi(), which dispatches into JS handlers that call
 *     setParam — and which runs hardest exactly when a knob is being turned,
 *     i.e. during the frames the lag is reported on;
 *   - js_display_pack() + the 1KB memcpy, which the comment says runs every
 *     30 ticks but actually runs EVERY tick whenever the screen is dirty,
 *     which on the knob grid is every tick since it now redraws every tick;
 *   - schwung_trace_poll_enable(), a filesystem check on eMMC.
 *
 * A span cannot see them (they are outside the js.tick scope) and the JS-side
 * fps counter cannot either (it counts draws, not the loop). So the residual
 * "54-56 not 60, with dips to 45" was being hunted with an instrument that is
 * blind to a third of the loop.
 *
 * This measures the whole iteration, phase by phase, and reports ONCE PER
 * SECOND — one log line, deliberately, because a diagnostic that writes files
 * per event is how ~150ms stalls got chased as scheduling contention (see the
 * param_tally note). Cost when disarmed is five clock_gettime calls per
 * iteration, ~125ns against a 16.67ms period.
 *
 * Arm with:  touch /data/UserData/schwung/loop_stats_on
 * ========================================================================= */
#define LOOP_STATS_FLAG "/data/UserData/schwung/loop_stats_on"

typedef struct {
    uint64_t midi_ns, tick_ns, pack_ns, other_ns, total_ns;
} loop_phase_t;

static struct {
    int armed;
    int iters;
    int overruns;               /* iterations whose work exceeded the period */
    uint64_t period_ns;
    loop_phase_t sum;
    loop_phase_t max;           /* per-phase maxima, independently */
    loop_phase_t worst;         /* full breakdown of the single worst iteration */
    uint64_t late_max_ns;       /* worst wake-up lateness vs the deadline */
    uint64_t late_sum_ns;
    uint64_t window_start_ns;
} loop_stats;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void loop_stats_poll_enable(void) {
    loop_stats.armed = (access(LOOP_STATS_FLAG, F_OK) == 0);
}

static void loop_stats_record(const loop_phase_t *p, uint64_t late_ns, uint64_t period_ns) {
    loop_stats.iters++;
    loop_stats.period_ns = period_ns;
    loop_stats.sum.midi_ns  += p->midi_ns;
    loop_stats.sum.tick_ns  += p->tick_ns;
    loop_stats.sum.pack_ns  += p->pack_ns;
    loop_stats.sum.other_ns += p->other_ns;
    loop_stats.sum.total_ns += p->total_ns;
    if (p->midi_ns  > loop_stats.max.midi_ns)  loop_stats.max.midi_ns  = p->midi_ns;
    if (p->tick_ns  > loop_stats.max.tick_ns)  loop_stats.max.tick_ns  = p->tick_ns;
    if (p->pack_ns  > loop_stats.max.pack_ns)  loop_stats.max.pack_ns  = p->pack_ns;
    if (p->other_ns > loop_stats.max.other_ns) loop_stats.max.other_ns = p->other_ns;
    if (p->total_ns > loop_stats.max.total_ns) { loop_stats.max.total_ns = p->total_ns;
                                                 loop_stats.worst = *p; }
    if (p->total_ns > period_ns) loop_stats.overruns++;
    loop_stats.late_sum_ns += late_ns;
    if (late_ns > loop_stats.late_max_ns) loop_stats.late_max_ns = late_ns;
}

/* One line per second, or nothing. Returns after resetting the window. */
static void loop_stats_report(uint64_t now) {
    if (!loop_stats.window_start_ns) { loop_stats.window_start_ns = now; return; }
    uint64_t dt = now - loop_stats.window_start_ns;
    if (dt < 1000000000ULL) return;

    int n = loop_stats.iters ? loop_stats.iters : 1;
    if (loop_stats.armed) {
        char line[512];
        /*
         * avg is where the time goes; max says whether the dips are one long
         * stall or a cluster of medium ones — which is the open question. If
         * `worst` is dominated by a phase, that phase is the answer; if
         * `late` is large while `total` is small, the process was descheduled
         * and nothing in this loop is at fault.
         */
        snprintf(line, sizeof(line),
                 "loop_stats: %d iters %d overrun (>%.1fms) | avg us midi=%llu tick=%llu pack=%llu other=%llu total=%llu"
                 " | max us midi=%llu tick=%llu pack=%llu other=%llu"
                 " | worst iter us total=%llu (midi=%llu tick=%llu pack=%llu other=%llu)"
                 " | late us avg=%llu max=%llu",
                 loop_stats.iters, loop_stats.overruns, loop_stats.period_ns / 1e6,
                 (unsigned long long)(loop_stats.sum.midi_ns  / n / 1000),
                 (unsigned long long)(loop_stats.sum.tick_ns  / n / 1000),
                 (unsigned long long)(loop_stats.sum.pack_ns  / n / 1000),
                 (unsigned long long)(loop_stats.sum.other_ns / n / 1000),
                 (unsigned long long)(loop_stats.sum.total_ns / n / 1000),
                 (unsigned long long)(loop_stats.max.midi_ns  / 1000),
                 (unsigned long long)(loop_stats.max.tick_ns  / 1000),
                 (unsigned long long)(loop_stats.max.pack_ns  / 1000),
                 (unsigned long long)(loop_stats.max.other_ns / 1000),
                 (unsigned long long)(loop_stats.max.total_ns / 1000),
                 (unsigned long long)(loop_stats.worst.midi_ns  / 1000),
                 (unsigned long long)(loop_stats.worst.tick_ns  / 1000),
                 (unsigned long long)(loop_stats.worst.pack_ns  / 1000),
                 (unsigned long long)(loop_stats.worst.other_ns / 1000),
                 (unsigned long long)(loop_stats.late_sum_ns / n / 1000),
                 (unsigned long long)(loop_stats.late_max_ns / 1000));
        shadow_ui_log_line(line);
    }
    memset(&loop_stats.sum,   0, sizeof(loop_stats.sum));
    memset(&loop_stats.max,   0, sizeof(loop_stats.max));
    memset(&loop_stats.worst, 0, sizeof(loop_stats.worst));
    loop_stats.iters = loop_stats.overruns = 0;
    loop_stats.late_max_ns = loop_stats.late_sum_ns = 0;
    loop_stats.window_start_ns = now;
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

    /* OTLP tracing (Phase 2): own process-local ring + exporter, off unless the
     * touch-file is present. Distinct service name → its own traces file; the
     * shim's spans share the system-wide CLOCK_MONOTONIC_RAW timebase, so both
     * align sub-ms on one Tempo timeline. */
    schwung_trace_init("schwung-shadow-ui");

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
        /* Deadline for the next tick — see the pacing note at the end of the loop. */
    struct timespec next_tick = { 0, 0 };

    while (!global_exit_flag) {
        /* Loop profiler; see loop_stats above. `t_wake` is when this iteration
         * actually started, which against the deadline it was sleeping to
         * gives wake-up lateness — the one number that separates "our work
         * overran" from "the scheduler did not run us". */
        loop_phase_t ph = {0};
        uint64_t t_wake = now_ns(), t_a = t_wake, t_b;
        uint64_t late_ns = 0;
        if (next_tick.tv_sec) {
            uint64_t deadline = (uint64_t)next_tick.tv_sec * 1000000000ULL
                              + (uint64_t)next_tick.tv_nsec;
            if (t_wake > deadline) late_ns = t_wake - deadline;
        }

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
        t_b = now_ns(); ph.midi_ns = t_b - t_a; t_a = t_b;

        if (jsTickIsDefined) {
            TRACE_SCOPE("js.tick");   /* root span; param.get etc. nest under it */
            callGlobalFunction(ctx, &JSTick, 0);
        }
        t_b = now_ns(); ph.tick_ns = t_b - t_a; t_a = t_b;
        /* The js.tick scope above unwound the per-thread span stack back to
         * depth 0, so any span a JS module opened via host_trace_begin and
         * forgot to host_trace_end() is already dropped from the stack — but its
         * handle-table slot would otherwise stay occupied forever. Reclaim the
         * whole table here (JS spans are per-tick by contract): a stale slot's
         * span_id is provably dead, and a late host_trace_end() against it is a
         * safe no-op. Without this, a module that consistently skips
         * host_trace_end fills all JS_TRACE_MAX_OPEN slots in ~16 ticks, after
         * which every host_trace_begin (from any module) silently returns 0. */
        for (int i = 0; i < JS_TRACE_MAX_OPEN; i++) js_trace_open[i] = 0;

        refresh_counter++;
        /* Poll the trace touch-file off the hot loop (~once/sec at 60 Hz). */
        if ((refresh_counter & 0x3F) == 0) { schwung_trace_poll_enable(); loop_stats_poll_enable(); }
        /* `other` is the two touch-file polls, on their own, because they are
         * the only eMMC access on this loop and a blocking one would look
         * exactly like the unexplained periodic dips. Once every 64
         * iterations, so its MAX is the number to read, not its average. */
        t_b = now_ns(); ph.other_ns = t_b - t_a; t_a = t_b;
        if ((js_display_screen_dirty || (refresh_counter % 30 == 0)) && shadow_display_shm) {
            js_display_pack(packed_buffer);
            memcpy(shadow_display_shm, packed_buffer, DISPLAY_BUFFER_SIZE);
            js_display_screen_dirty = 0;
        }
        t_b = now_ns();
        ph.pack_ns = t_b - t_a;
        ph.total_ns = t_b - t_wake;
        loop_stats_record(&ph, late_ns, (uint64_t)((shadow_control && shadow_control->overtake_mode >= 2)
                                                   ? 2000000L : 16666667L));
        loop_stats_report(t_b);

        /*
         * Sleep to an absolute DEADLINE, not for a fixed duration.
         *
         * This used to be usleep(16000) with a comment claiming ~60 Hz. It was
         * never 60 Hz: sleeping a fixed amount AFTER the work makes the rate
         * 1 / (work + 16ms), so it moves with whatever the tick happens to do.
         * Measured on device, the model fits exactly — 7.3ms of work gave
         * 42.3 ticks/sec (predicted 42.9), and 12.3ms gave 36 (predicted 35.3).
         *
         * That single line is the whole of the "dropped frames / jagged"
         * report. The UI draws once per tick and draws == ticks in every
         * window, so an irregular tick IS an irregular frame rate, and every
         * extra parameter read — each ~2.8ms — permanently lowered the ceiling
         * for the entire view. Uneven motion reads far worse than slow motion.
         *
         * With a deadline the period is what it says it is, and work only
         * matters if it exceeds the period. Overrun resets the phase to now
         * rather than firing a catch-up burst of back-to-back ticks, which on
         * a UI loop would show up as a visible stutter-then-sprint.
         */
        const long period_ns = (shadow_control && shadow_control->overtake_mode >= 2)
                             ? 2000000L      /* 500 Hz for overtake modules */
                             : 16666667L;    /* 60 Hz for the normal shadow UI */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        if (next_tick.tv_sec == 0) next_tick = now_ts;   /* first pass */
        next_tick.tv_nsec += period_ns;
        while (next_tick.tv_nsec >= 1000000000L) {
            next_tick.tv_nsec -= 1000000000L;
            next_tick.tv_sec++;
        }
        /* Behind the deadline (a tick overran, or the process was descheduled)
         * — give up the missed slots instead of sprinting to catch up. */
        if (next_tick.tv_sec < now_ts.tv_sec ||
            (next_tick.tv_sec == now_ts.tv_sec && next_tick.tv_nsec < now_ts.tv_nsec)) {
            next_tick = now_ts;
        } else {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        }
    }

    schwung_trace_shutdown();   /* flush + stop the trace exporter (drain last spans) */
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
