/*
 * shadow_constants.h - Shared constants for Shadow Instrument
 *
 * This header defines constants and structures shared between:
 * - schwung_shim.c (the LD_PRELOAD shim)
 * - shadow_ui.c (the shadow UI host)
 *
 * Single source of truth to prevent drift between components.
 */

#ifndef SHADOW_CONSTANTS_H
#define SHADOW_CONSTANTS_H

#include <stdint.h>

/* ============================================================================
 * Shared Memory Segment Names
 * ============================================================================ */

#define SHM_SHADOW_AUDIO    "/schwung-audio"    /* Shadow's mixed output */
#define SHM_SHADOW_MIDI     "/schwung-midi"     /* MIDI to shadow DSP */
#define SHM_SHADOW_UI_MIDI  "/schwung-ui-midi"  /* MIDI to shadow UI */
#define SHM_SHADOW_DISPLAY  "/schwung-display"  /* Shadow display buffer */
#define SHM_SHADOW_CONTROL  "/schwung-control"  /* Control flags/state */
#define SHM_SHADOW_MOVEIN   "/schwung-movein"   /* Move's audio for shadow */
#define SHM_SHADOW_UI       "/schwung-ui"       /* Shadow UI state */
#define SHM_SHADOW_PARAM      "/schwung-param"        /* Shadow param requests */
#define SHM_SHADOW_MIDI_OUT   "/schwung-midi-out"   /* MIDI output from shadow UI */
#define SHM_SHADOW_MIDI_DSP   "/schwung-midi-dsp"   /* MIDI from shadow UI to DSP slots */
#define SHM_SHADOW_MIDI_INJECT "/schwung-midi-inject" /* MIDI inject into Move's MIDI_IN (test bus owns it during overtake) */
#define SHM_SHADOW_MIDI_INJECT_UI "/schwung-midi-inject-ui" /* shadow UI's own inject — always bound for Move's firmware */
#define SHM_SHADOW_EXT_MIDI_REMAP "/schwung-ext-midi-remap" /* Cable-2 channel remap table */
#define SHM_SHADOW_SCREENREADER "/schwung-screenreader" /* Screen reader announcements */
#define SHM_SHADOW_OVERLAY  "/schwung-overlay"  /* Overlay state (sampler/skipback) */
#define SHM_TEST_STREAM_MIDI_OUT "/schwung-test-stream-midi-out" /* Shim → schwung-testd MIDI_OUT events (E2E test bus, dev-only) */
#define SHM_DISPLAY_LIVE    "/schwung-display-live"    /* Live display for remote viewer */
#define SHM_WEB_PARAM_SET   "/schwung-web-param-set"   /* Web UI → shim param set ring */
#define SHM_WEB_PARAM_NOTIFY "/schwung-web-param-notify" /* Shim → web UI param change ring */

/* ============================================================================
 * Audio Constants
 * ============================================================================ */

#define FRAMES_PER_BLOCK 128    /* Audio frames per ioctl block */

/* ============================================================================
 * Buffer Sizes
 * ============================================================================ */

#define MIDI_BUFFER_SIZE    256   /* Hardware mailbox MIDI area: 64 USB-MIDI packets */

/* The shim -> shadow_ui MIDI ring, and NOT the mailbox size above.
 *
 * It was MIDI_BUFFER_SIZE, which made the ring 64 packets because that is how
 * many fit in the hardware mailbox — a number with nothing to do with how much
 * MIDI a tool can be sent between two drains. A Yamaha 5F bulk dump message is
 * 158 bytes on the wire = 53 packets, so ONE nearly filled the ring and two
 * back-to-back could not fit: shadow_ui_midi_publish() fell off the end of its
 * scan and dropped whole packets, silently. Measured on a QY-70 at 6.9% of
 * 3164 messages, every shortfall a multiple of 3 and every one on the largest
 * message type (#358).
 *
 * Sharing the constant also made the obvious fix dangerous: MIDI_BUFFER_SIZE
 * still bounds `memcpy(sh_midi, hw_midi, ...)` and `hotkey_prev`, which read
 * the SPI mailbox, so enlarging IT would have read past MIDI_IN into the
 * display status word. Hence a separate name rather than a bigger number.
 *
 * 1024 = 256 packets, ~4.8 of those dump messages, against 1.2 before. The
 * cost is 768 bytes of shared memory.
 *
 * RESIZING THIS IS A LAYOUT CHANGE. The segment outlives the processes, so a
 * shim that creates it at the new size and a shadow_ui built against the old
 * one disagree — and the dangerous direction is a NEW consumer mapping 1024
 * over a stale 256-byte segment, where every touch past the end is SIGBUS.
 * shadow_shm_map() refuses a short attach rather than handing back that
 * mapping; deploy the shim and shadow_ui together, as install.sh does. */
#define SHADOW_UI_MIDI_BYTES 1024
/* Hardware MIDI_OUT region is 20 × 4-byte USB-MIDI packets = 80 bytes.
 * The display buffer starts immediately after at offset 80. Writes to
 * MIDI_OUT must be bounded by this to avoid corrupting the display. */
#define HW_MIDI_OUT_SIZE    80
#define DISPLAY_BUFFER_SIZE 1024  /* 128x64 @ 1bpp = 1024 bytes */
/*
 * The control segment. 256 bytes for a struct that currently uses ~85.
 *
 * It was 84 — not a designed size, just wherever the struct happened to end
 * after the corun masks were widened, with a `==` assert that had to be bumped
 * by hand each time. That made it read as FULL: adding a byte failed the
 * assert, which looked like a hard limit and is why the Recall Quantize
 * setting was first squeezed into two spare bits of ui_flags_ext instead of
 * being given a field.
 *
 * IT COSTS NOTHING. /dev/shm is tmpfs and allocates by page: measured on the
 * device, schwung-control declared 84 bytes and occupied 4096 — the same 8
 * blocks as schwung-ui's 512. Anything up to a page is free.
 *
 * RESIZING IS A LAYOUT CHANGE, handled the same way as SHADOW_UI_MIDI_BYTES
 * (#358) and SHADOW_MIDI_OUT_BUFFER_SIZE (#361): the segment outlives the
 * processes, so a new consumer can meet a stale short one. shadow_shm_map()
 * fstats on attach and REFUSES rather than handing back a mapping whose tail
 * is SIGBUS — the feature goes quiet and logs why. Deploy the shim and
 * shadow_ui together, as install.sh does. Growing is also the safe direction
 * for the other order: an old consumer asking for 84 attaches to a 256-byte
 * segment quite happily.
 */
#define CONTROL_BUFFER_SIZE 256
#define SHADOW_UI_BUFFER_SIZE     512
#define SHADOW_PARAM_BUFFER_SIZE  65664  /* Large buffer for complex ui_hierarchy */
/* The shadow_ui -> shim MIDI out buffer: 256 USB-MIDI packets.
 *
 * It was 512 bytes = 128 packets, and that made ONE js_shadow_midi_send() call
 * of more than 128 packets impossible: the writes past the end were refused and
 * the send returned false. A 632-byte SysEx is 212 packets, so a module could
 * not hand over a bulk dump in one call however well it was written. Measured
 * on hardware 2026-08-31: 158 B (53 pkt) and 316 B (107 pkt) went out intact,
 * 632 B came back REFUSED.
 *
 * Sized to match UI_MIDI_CARRY_PACKETS deliberately, and the two must stay
 * equal. The carry is what the shim drains this into; if this were the larger
 * of the two, a flush that the SHM buffer accepted could not fit downstream and
 * would be dropped at the carry instead -- moving the refusal one buffer along
 * and making it a drop rather than a return value, which is the exact shape
 * ui_midi_out_carry.h exists to remove.
 *
 * RESIZING THIS IS A LAYOUT CHANGE, same as SHADOW_UI_MIDI_BYTES (#361): the
 * segment outlives the processes, so deploy the shim and shadow_ui together.
 * shadow_shm_map() refuses a short attach rather than handing back a mapping
 * whose tail is SIGBUS. write_idx is a uint16_t byte offset and still addresses
 * this fine; the _Static_assert below fails if that ever stops being true. */
#define SHADOW_MIDI_OUT_BUFFER_SIZE 1024
#define SHADOW_MIDI_DSP_BUFFER_SIZE 512  /* MIDI to DSP buffer from shadow UI (128 packets) */
/* MIDI inject ring capacity is SHADOW_MIDI_INJECT_SLOTS (defined with the
 * struct below) — the old flat byte-buffer size is gone. */
#define SHADOW_SCREENREADER_BUFFER_SIZE 8448  /* Screen reader message buffer */
/* Overlay state. 512 for a struct that uses 256 — same reasoning as
 * CONTROL_BUFFER_SIZE above, and the same zero cost. It was exactly 256 with
 * an `==` assert, so it read as full for the same wrong reason. */
#define SHADOW_OVERLAY_BUFFER_SIZE 512

/* Web UI ring buffer sizes */
#define WEB_PARAM_KEY_LEN     64
#define WEB_PARAM_VALUE_LEN   256    /* Most values are short; hierarchy uses the old channel */
#define WEB_PARAM_SET_ENTRIES 32     /* Max pending set requests */
#define WEB_PARAM_NOTIFY_ENTRIES 64  /* Max pending change notifications */

/* ============================================================================
 * Slot Configuration
 * ============================================================================ */

#define SHADOW_CHAIN_INSTANCES 4
#define SHADOW_UI_SLOTS 4
#define SHADOW_UI_NAME_LEN 64
#define SHADOW_PARAM_KEY_LEN 64
#define SHADOW_PARAM_VALUE_LEN 65536  /* 64KB for large ui_hierarchy and state */
#define SHADOW_SCREENREADER_TEXT_LEN 8192  /* Max text length for screen reader messages */

/* ============================================================================
 * UI Flags (set in shadow_control_t.ui_flags)
 * ============================================================================ */

#define SHADOW_UI_FLAG_JUMP_TO_SLOT 0x01      /* Jump to slot settings on open */
#define SHADOW_UI_FLAG_JUMP_TO_MASTER_FX 0x02 /* Jump to Master FX on open */
#define SHADOW_UI_FLAG_JUMP_TO_OVERTAKE 0x04  /* Jump to overtake module menu */
#define SHADOW_UI_FLAG_SAVE_STATE 0x08        /* Save all state (shutdown imminent) */
#define SHADOW_UI_FLAG_JUMP_TO_SCREENREADER 0x10 /* Jump to screen reader settings */
#define SHADOW_UI_FLAG_SET_CHANGED 0x20           /* Set changed - reload slot state */
#define SHADOW_UI_FLAG_JUMP_TO_SETTINGS 0x40     /* Jump to Global Settings */
#define SHADOW_UI_FLAG_JUMP_TO_TOOLS 0x80        /* Jump to Tools menu */

/*
 * Flags 0x0100 and up live in `ui_flags_ext`, NOT in `ui_flags`.
 *
 * `ui_flags` is a uint8_t and all eight bits above are taken. Widening it is
 * not free: `ui_patch_index` (uint16_t) sits IMMEDIATELY after it at offset 8
 * with no padding to absorb the growth, so a uint16_t `ui_flags` would move
 * every field behind it and change sizeof(shadow_control_t). The shim and
 * shadow_ui are separate binaries mapping the same segment; a layout change
 * that lands in one before the other is silent corruption, not a build error.
 *
 * `ui_flags_ext` was `reserved16` — already at that offset, already the right
 * width, referenced nowhere. Using it costs zero layout change.
 *
 * js_shadow_get_ui_flags() presents the two as ONE 24-bit word
 * (`ui_flags | (ui_flags_ext << 8)`) and js_shadow_clear_ui_flags() splits a
 * mask back across them, so JS sees a flat flag space and never has to know
 * which byte a flag lives in. Keep new flags dense from 0x0100 upward.
 */
#define SHADOW_UI_FLAG_EXT_SHIFT 8
#define SHADOW_UI_FLAG_SNAPSHOT_TAKE   0x0100  /* Shift+Copy: snapshot whole rig */
#define SHADOW_UI_FLAG_SNAPSHOT_RECALL 0x0200  /* Shift+Delete: recall snapshot NOW */
#define SHADOW_UI_FLAG_SNAPSHOT_QUEUED 0x0400  /* recall armed for the next boundary */
#define SHADOW_UI_FLAG_SNAPSHOT_UNQUEUED 0x0800 /* armed recall cancelled */

/* Recall Quantize in MIDI clock pulses, from shadow_control_t.recall_quantize.
 * 0 = Off, 1 = beat, 2 = bar, 3 = two bars, at 24 PPQN. */
#define SHADOW_RECALL_Q_PULSES(v) \
    ((v) == 1 ? 24 : (v) == 2 ? 96 : (v) == 3 ? 192 : 0)

/* ============================================================================
 * Special Values
 * ============================================================================ */

#define SHADOW_PATCH_INDEX_NONE 65535

/* ============================================================================
 * Shared Structures
 * ============================================================================ */

/*
 * Control structure for communication between shim and shadow UI.
 * Must be exactly SHADOW_CONTROL_BUFFER_SIZE bytes.
 */
typedef struct shadow_control_t {
    volatile uint8_t display_mode;    /* 0=normal, 1=shadow */
    volatile uint8_t shadow_ready;    /* Shadow UI is ready */
    volatile uint8_t should_exit;     /* Signal shadow UI to exit */
    volatile uint8_t midi_ready;      /* New MIDI available (toggle) */
    volatile uint8_t write_idx;       /* MIDI write index */
    volatile uint8_t read_idx;        /* MIDI read index */
    volatile uint8_t ui_slot;         /* UI-highlighted slot for knob routing */
    volatile uint8_t ui_flags;        /* UI flags (SHADOW_UI_FLAG_*) */
    volatile uint16_t ui_patch_index; /* Requested patch index */
    volatile uint16_t ui_flags_ext;   /* UI flags 0x0100+ (see SHADOW_UI_FLAG_EXT_SHIFT) */
    volatile uint32_t ui_request_id;  /* Incremented on patch request */
    volatile uint32_t shim_counter;   /* Debug: shim tick counter */
    volatile uint8_t selected_slot;   /* Track-selected slot (0-3) for playback/knobs */
    volatile uint8_t shift_held;      /* Is shift button currently held? */
    volatile uint8_t overtake_mode;   /* 0=normal, 1=menu (UI events only), 2=module (all events) */
    volatile uint8_t restart_move;    /* Signal shim to restart Move (0=no, 1=restart) */
    volatile uint8_t tts_enabled;     /* Screen Reader on/off (1=on, 0=off) */
    volatile uint8_t tts_volume;      /* TTS volume (0-100) */
    volatile uint16_t tts_pitch;      /* TTS pitch in Hz (80-180) */
    volatile float tts_speed;         /* TTS speed multiplier (0.5-6.0) */
    volatile uint8_t overlay_knobs_mode; /* 0=shift, 1=jog_touch, 2=off, 3=native */
    volatile uint8_t display_mirror;     /* 0=off, 1=on (stream display to browser) */
    volatile uint8_t tts_engine;         /* 0=espeak-ng, 1=flite */
    volatile uint8_t pin_challenge_active; /* 0=none, 1=challenge detected, 2=submitted */
    volatile uint8_t display_overlay;     /* 0=off, 1=rect overlay on native, 2=fullscreen */
    volatile uint8_t overlay_rect_x;      /* Overlay rect left edge (pixels, 0-127) */
    volatile uint8_t overlay_rect_y;      /* Overlay rect top edge (pixels, 0-63) */
    volatile uint8_t overlay_rect_w;      /* Overlay rect width (pixels) */
    volatile uint8_t overlay_rect_h;      /* Overlay rect height (pixels) */
    volatile uint16_t tts_debounce_ms;   /* Screen reader debounce in ms (0-1000, default 300) */
    volatile uint8_t set_pages_enabled;  /* 0=off, 1=on (Shift+Vol+Left/Right page switching) */
    volatile uint8_t skip_led_clear;     /* 1=preserve native LEDs on entry, or request native repaint on exit */
    volatile uint8_t move_ui_mode;       /* Move's UI mode: 0=unknown, 1=session, 2=note, 3=set_overview */
    volatile uint8_t sampler_cmd;        /* 0=none, 1=start (path in file), 2=stop */
    volatile uint8_t sampler_state_val;  /* Mirrors sampler_state_t: 0=idle,1=armed,2=recording,3=preroll */
    volatile uint8_t mute_move_audio;   /* 1=zero Move's audio output (for silent clip switching) */
    volatile uint8_t sampler_ext_stop;  /* 1=sampler ignores MIDI Stop, only explicit stop works */
    volatile uint8_t wake_slots;       /* 1=clear all slot idle flags (auto-clears after read) */
    volatile uint8_t skipback_require_volume; /* 0=Shift+Capture, 1=Shift+Vol+Capture */
    volatile uint8_t preview_cmd;          /* 0=none, 1=play (path in file), 2=stop */
    volatile uint8_t pad_block;            /* 1=suppress pad notes (68-99) from reaching Move */
    volatile uint8_t suspend_overtake;  /* 1=suspend (skip exit hook), 0=normal exit */
    volatile uint8_t open_tool_cmd;     /* 0=none, 1=open tool (path in /data/UserData/schwung/open_tool_cmd.json) */
    volatile uint8_t shadow_ui_trigger; /* Shadow UI trigger mode: 0=long-press only, 1=Shift+Vol only, 2=both */
    volatile uint8_t speaker_active;    /* 1=built-in speaker active (from CC 115 line-out detect) */
    volatile uint8_t line_in_connected; /* 1=line-in cable plugged (from CC 114 mic-in detect); 0=internal mic */
    volatile uint8_t sampler_source_request; /* 0=no request, 1=set Resample, 2=set Move Input. Shim resets to 0 after applying. */
    volatile uint8_t sampler_silent;     /* 1=suppress sampler screen-reader announcements (e.g. "Sample saved") for tool-driven recordings */
    volatile uint16_t skipback_seconds; /* Skipback rolling buffer length: 30/60/120/180/240/300 */
    volatile uint8_t resume_last_tool;  /* 1=JUMP_TO_TOOLS should resume the most-recently-suspended tool instead of opening the menu */
    volatile uint8_t midi_indicator_enabled; /* 1=draw "ccN" MIDI channel indicator while a note is held */
    volatile uint8_t overtake_fx_end_of_chain; /* 1=run the overtake FX on the final Move+ME mix instead of the ME bus (whole-mix effects; no Link Audio needed) */
    /* Co-run state: one struct, accessed via helpers below. `target` selects
     * which peer co-runs (chain editor or Move firmware), `id` is its identity
     * (chain slot 0-3 or tool track 0-7; -1 unused when target=NONE), and
     * `keep_mask` is the tool's CORUN_GRP_* input manifest (0 = default split).
     * Never read these directly outside the helpers — `target` may be 0 (NONE)
     * with stale `id` / `keep_mask` from a prior session. */
    volatile struct {
        int8_t target;       /* corun_target_t */
        int8_t id;
        uint8_t flags;       /* CORUN_F_* — model selector + per-session toggles (see below) */
        uint8_t _pad;
        /* INPUT ownership. Stored ALWAYS as a keep-mask internally (CORUN_GRP_*
         * the tool owns; the rest cede to the peer) so every routing site uses
         * one representation. New cede-model tools express intent as a cede-list
         * and the host stores the complement via corun_cede_to_keep() — see
         * CORUN_F_CEDE_MODEL. Legacy tools write keep bits directly. */
        uint32_t keep_mask;
        /* LED ownership; 0 = follow keep_mask (the common "own both" case) UNLESS
         * CORUN_F_LED_DISTINCT is set, in which case this mask is authoritative
         * even when 0. Lets a tool paint a surface while ceding its presses (e.g.
         * dAVEBOx draws the track-button clip indicator but cedes the press). */
        uint32_t led_keep_mask;
    } corun;
    volatile uint8_t shadow_display_owner; /* display_owner_t: who currently owns the OLED. Independent of shadow_display_mode (which only says "shadow session active"). */
    /* 1=also strip Move's cable-0 sysex (RGB pad/clip/grid LEDs) during FULL
     * overtake, so a tool that forces Move's sequencer to keep running (e.g.
     * dAVEBOx Clock Follow) gets true full LED control instead of fighting
     * Move's repaints. Opt-in per tool (shadow_set_overtake_suppress_sysex);
     * default 0 leaves the existing sysex passthrough unchanged. This byte
     * reuses the former reserved[1] slot, but the struct still grew 72->76
     * because of corun.led_keep_mask above; CONTROL_BUFFER_SIZE was bumped to
     * match (shim creates the SHM and shadow_ui maps it via the same macro). */
    volatile uint8_t overtake_suppress_sysex;
    /* 1 = hand the eight encoder-ring LEDs (CC 71-78) back to Move.
     *
     * The knob grid paints those rings to say which physical encoder drives
     * which drawn cell. Leaving the grid used to turn them OFF, which is not
     * the same as giving them back: come out of the grid into a Schwung track
     * and Move's own eight rings stayed dark, because Move only writes an LED
     * when its value changes and nothing had changed.
     *
     * Overtake already solves this with a full snapshot/restore keyed on the
     * overtake-mode transition. The knob grid is not overtake, and does not
     * need the full thing — it touches exactly eight CC LEDs and nothing else,
     * so the restore is "replay Move's own last value for those eight", which
     * shadow_led_queue.c has been accumulating in move_cc_led_state all along.
     *
     * Set by JS on leaving the grid; the shim consumes it and clears it, so it
     * is an edge and not a state. */
    volatile uint8_t restore_knob_leds;
    /*
     * Recall Quantize: 0 = Off, 1 = beat, 2 = bar, 3 = two bars.
     *
     * Written by JS when the setting changes and once at startup; read by the
     * shim when Shift+Delete is pressed, because the shim is where MIDI clock
     * is counted. A plain field, in the headroom CONTROL_BUFFER_SIZE now has.
     *
     * It was two bits inside ui_flags_ext, on the belief that this struct was
     * full — it was not, the buffer was just sized to the struct with an `==`
     * assert. The register cost a mask, a shift, a bespoke JS binding, and one
     * real bug (the flat mask applied to ext space, which reads zero always).
     * The lesson generalises: an assert that fails when you add a field is
     * telling you to size the buffer, not to pack bits.
     *
     * It is NOT persisted here — SHM does not survive a reboot. The C setter
     * writes features.json, and JS reads that back at startup and pushes it
     * down, the same shape as shadow_ui_trigger.
     */
    volatile uint8_t recall_quantize;
    /* 1 = suppress CC 79 (master volume) and the master-touch note (8) from
     * their hardcoded overtake passthrough to Move firmware, and from the
     * plain-volume-touch OLED handoff in shadow_swap_display(). Both of
     * those exemptions exist unconditionally today so Move can show its
     * native volume overlay; a tool with its own per-track volume gesture
     * (movy: hold a track button + turn master volume) sets this for the
     * duration of that gesture so Move is excluded entirely instead of
     * quietly tracking the turn on its own copy of the value. Opt-in per
     * tool (shadow_set_overtake_suppress_master_volume); default 0 leaves
     * the existing passthrough unchanged. This byte consumes existing
     * trailing padding — CONTROL_BUFFER_SIZE was NOT bumped for it, and its
     * 84 still derives from the corun mask widening above.
     *
     * Enforced in the PRECEDENCE TAIL of the cable-0 filter loop, not in the
     * mode branches, so it also outranks capabilities.button_passthrough and
     * the move-native co-run cede (CORUN_GRP_MASTER / CORUN_GRP_TOUCH) — both
     * of which clear `filter` unconditionally and would otherwise defeat it
     * silently. Cleared by the shim on ANY overtake-mode change.
     *
     * KNOWN GAP, not fixed: raising or lowering the flag BETWEEN a note-8
     * on and its off filters the touch asymmetrically, so Move sees the press
     * and never the release and latches volume-touch state. The overtake-exit
     * path already compensates for exactly this with an injected vol_touch_off
     * (schwung_shim.c); a mid-session flip has no equivalent. A tool should
     * raise and lower the flag OUTSIDE a held master-volume touch. */
    volatile uint8_t overtake_suppress_master_volume;
    /*
     * 1 = a plain Track tap while the shadow UI is up SWITCHES SLOT instead of
     * dismissing back to Move ("Keep Schwung", Global Settings -> Display).
     *
     * APPENDED, which is only free because CONTROL_BUFFER_SIZE is 256 for a
     * struct that uses ~86. It was first written into the low byte of
     * `reserved16` — the tail had no padding at 84, so growing the struct meant
     * bumping an `==` assert that read as a hard limit. That is exactly the
     * misreading the sizing comment at the top of this file records, and
     * `reserved16` is `ui_flags_ext` now, so the byte lives at the end where a
     * new field belongs. Read live by the shim (STAY_IN_SHADOW) and by
     * schwung-manager at a raw offset — see shmconfig.go, which must move with
     * it. Default 0 keeps the tap-dismisses behaviour.
     */
    volatile uint8_t stay_in_shadow;

    /*
     * Metronome. All three APPENDED after stay_in_shadow so nothing behind
     * them moves: sizeof is a contract between two binaries, and
     * schwung-manager reads stay_in_shadow at a RAW OFFSET (shmconfig.go).
     * Appending is free; inserting is not.
     *
     * Free in the other sense too — CONTROL_BUFFER_SIZE is 256 for a struct
     * that uses ~86, and /dev/shm allocates by page, so three bytes cost
     * nothing. Only SHRINKING the container fails the build.
     *
     * These live here rather than in features.json because
     * load_feature_config() runs ONCE at init: a value parsed there would need
     * a reboot to change. Same reason recall_quantize is a field.
     */

    /* 0 = off, 1 = follow Move's metronome, 2 = always on while playing. */
    volatile uint8_t metronome_mode;

    /* 0-100 %. */
    volatile uint8_t metronome_level;

    /*
     * Beats per bar, for the downbeat accent. From timeSignature.upper in the
     * current set's Song.abl, read by the shadow UI on SET_CHANGED — never on
     * the SPI callback. 0 means "not yet known"; the consumer clamps to 4.
     */
    volatile uint8_t metronome_beats_per_bar;

    /*
     * Save Stems (Global Settings -> Audio). 0 = Master, 1 = Stems, 2 = Both.
     *
     * Governs the Quantized Sampler, Skipback and Song Mode's record button
     * alike -- one setting, because all three record through the same sampler
     * and a per-surface switch would be three places to get it wrong.
     *
     * APPENDED after the metronome trio for the same reason they were appended
     * after stay_in_shadow: sizeof is a contract between two binaries and
     * schwung-manager reads stay_in_shadow at a RAW offset (shmconfig.go).
     * Appending is free; inserting is not.
     *
     * A FIELD rather than a features.json read, because load_feature_config()
     * runs once at init and this has to be changeable without a reboot -- the
     * same reason recall_quantize and metronome_mode are fields. The C setter
     * writes features.json and JS pushes it back down at startup.
     */
    volatile uint8_t save_stems;
    /*
     * Runtime button claim: one bit per CC number (bit cc&7 of byte cc>>3).
     * A set bit withholds that CC from Move firmware and forwards it to the
     * shadow UI instead, so a module can build its own gestures on a button
     * without a press ALSO firing Move's action behind the screen.
     *
     * Opt-in on purpose. #154 blocked Undo/Copy/Delete unconditionally whenever
     * the shadow display was up and was reverted in #175 because it stole
     * Move's native Undo during ordinary chain use. This is the runtime
     * complement to the STATIC capabilities.claims_ccs / claims_edit_ccs:
     * shadow_ui reconciles it from whichever module's UI is actually on
     * screen, so Move keeps its own buttons everywhere else. The shim refuses
     * bits for the controls the host itself owns (Shift, Menu, Back, jog,
     * knobs, Mute, tracks) whatever is set here. Same shape as pad_block.
     *
     * APPENDED, like save_stems above: sizeof is a contract between two
     * binaries. Appending is free; inserting is not.
     */
    volatile uint8_t claim_cc_bits[16];
} shadow_control_t;

/* Values for shadow_control_t.save_stems. */
#define SAVE_STEMS_MASTER 0
#define SAVE_STEMS_STEMS  1
#define SAVE_STEMS_BOTH   2
/* Write the mixed master file? Both Master and Both do. */
#define SAVE_STEMS_WANTS_MASTER(v) ((v) != SAVE_STEMS_STEMS)
/* Write the per-stem files? Both Stems and Both do. */
#define SAVE_STEMS_WANTS_STEMS(v)  ((v) != SAVE_STEMS_MASTER)

/* Co-run control-surface groups. A co-running overtake tool declares which
 * groups it KEEPS (corun_keep_mask); every other group's input cedes to the
 * co-run UI (Schwung's chain editor, or Move firmware). One canonical
 * event->group map (corun_group_for_event) is shared by every routing site so
 * the let-through and suppress-from-tool decisions can never drift apart. */
/* Single discrete buttons each get their own bit; continuous/multi-key surfaces
 * (pads, steps, knobs, touch, the track-button row, jog) are one group each
 * (per-key co-run ownership isn't meaningful). Bit 15 is reserved for the legacy
 * CORUN_KEEP_BACK flag; new button groups skip it and stay <= 25 so the JS-side
 * signed `| 0` coercion never sees a negative mask. */
#define CORUN_GRP_OLED          (1u << 0)
#define CORUN_GRP_PADS          (1u << 1)
#define CORUN_GRP_STEPS         (1u << 2)
/* Bit 3 retired: TRANSPORT is now the composite of the real transport buttons
 * (defined below). The old single-bit value is reserved/unused — it was never
 * returned by corun_group_for_event, so nothing routed on it. */
#define CORUN_GRP_JOG           (1u << 4)  /* jog turn (CC 14) + jog click (CC 3) */
#define CORUN_GRP_TRACK_BUTTONS (1u << 5)  /* CC 40-43 */
#define CORUN_GRP_KNOBS         (1u << 6)  /* CC 71-78 */
#define CORUN_GRP_MASTER        (1u << 7)  /* CC 79 */
#define CORUN_GRP_SHIFT         (1u << 8)  /* CC 49 */
#define CORUN_GRP_BACK          (1u << 9)  /* CC 51 */
#define CORUN_GRP_MENU          (1u << 10) /* CC 50 */
#define CORUN_GRP_TOUCH         (1u << 11) /* capacitive-touch notes 0-9 */
#define CORUN_GRP_MUTE          (1u << 12) /* CC 88 */
/* Discrete transport + edit + nav buttons — each independently keep/cede-able. */
#define CORUN_GRP_PLAY          (1u << 13) /* CC 85 */
#define CORUN_GRP_REC           (1u << 14) /* CC 86 */
#define CORUN_GRP_SAMPLE        (1u << 16) /* CC 118 (Record/Sample) */
#define CORUN_GRP_LOOP          (1u << 17) /* CC 58 */
#define CORUN_GRP_COPY          (1u << 18) /* CC 60 */
#define CORUN_GRP_DELETE        (1u << 19) /* CC 119 */
#define CORUN_GRP_UNDO          (1u << 20) /* CC 56 */
#define CORUN_GRP_CAPTURE       (1u << 21) /* CC 52 */
#define CORUN_GRP_NAV_UP        (1u << 22) /* CC 55 */
#define CORUN_GRP_NAV_DOWN      (1u << 23) /* CC 54 */
#define CORUN_GRP_NAV_LEFT      (1u << 24) /* CC 62 */
#define CORUN_GRP_NAV_RIGHT     (1u << 25) /* CC 63 */

/* Convenience composites — keep/cede a cluster in one bit-op (optional). */
#define CORUN_GRP_TRANSPORT (CORUN_GRP_PLAY | CORUN_GRP_REC | CORUN_GRP_SAMPLE | CORUN_GRP_LOOP)
#define CORUN_GRP_NAV       (CORUN_GRP_NAV_UP | CORUN_GRP_NAV_DOWN | CORUN_GRP_NAV_LEFT | CORUN_GRP_NAV_RIGHT)
#define CORUN_GRP_EDIT      (CORUN_GRP_COPY | CORUN_GRP_DELETE | CORUN_GRP_UNDO | CORUN_GRP_CAPTURE)

/* The buttons that were UNCLASSIFIED before the cede model. A legacy keep-list
 * tool (CORUN_F_CEDE_MODEL clear) never named these, so they stay with the tool
 * regardless of its keep_mask — the byte-for-byte legacy carve-out. */
#define CORUN_GRP_EXTENDED_ALL (CORUN_GRP_TRANSPORT | CORUN_GRP_EDIT | CORUN_GRP_NAV)

/* Every routable group bit — the universe over which a cede-mask is complemented
 * into an internal keep-mask (corun_cede_to_keep). Excludes bit 3 (retired) and
 * bit 15 (the CORUN_KEEP_BACK legacy flag). */
#define CORUN_GRP_ALL (CORUN_GRP_OLED | CORUN_GRP_PADS | CORUN_GRP_STEPS | CORUN_GRP_JOG \
    | CORUN_GRP_TRACK_BUTTONS | CORUN_GRP_KNOBS | CORUN_GRP_MASTER | CORUN_GRP_SHIFT \
    | CORUN_GRP_BACK | CORUN_GRP_MENU | CORUN_GRP_TOUCH | CORUN_GRP_MUTE | CORUN_GRP_EXTENDED_ALL)

/* corun.flags bits. */
#define CORUN_F_CEDE_MODEL  (1u << 0) /* keep_mask is a complemented cede-mask; extended buttons participate */
#define CORUN_F_OWN_BACK    (1u << 1) /* tool handles Back itself (no framework auto-exit); cede model */
#define CORUN_F_LED_DISTINCT (1u << 2) /* led_keep_mask is authoritative even when 0 (don't follow keep_mask) */

/* Legacy default keep-set: the canonical sequencer-style co-run split — tool
 * keeps pads, step buttons, transport, Menu, and Mute; cedes the nav surface +
 * screen. Used whenever a LEGACY tool leaves keep_mask == 0. */
#define CORUN_KEEP_DEFAULT (CORUN_GRP_PADS | CORUN_GRP_STEPS | CORUN_GRP_TRANSPORT | CORUN_GRP_MENU | CORUN_GRP_MUTE)

/* Legacy opt-out flag (bit 15, lives in a legacy tool's keep_mask): when set the
 * framework does NOT auto-exit on Back — Back routes per the keep_mask. Cede-model
 * tools use CORUN_F_OWN_BACK instead. Kept for byte-identical legacy behavior. */
#define CORUN_KEEP_BACK (1u << 15)

/* Map a raw cable-0 MIDI event to its control-surface group, or 0 if it isn't a
 * routable surface control (sensor CCs like the mic/speaker plug-detect 114/115
 * — those can't be ceded and always stay with the tool). type is the status
 * nibble (0xB0 CC, 0x90/0x80 note); d1 the data byte. Every routable input is
 * now classified; whether an extended button actually participates in keep/cede
 * depends on the model (legacy carve-out, see corun_event_owner). */
static inline uint32_t corun_group_for_event(uint8_t type, uint8_t d1) {
    if (type == 0xB0) {
        switch (d1) {
            case 3:  case 14: return CORUN_GRP_JOG;
            case 40: case 41: case 42: case 43: return CORUN_GRP_TRACK_BUTTONS;
            case 49: return CORUN_GRP_SHIFT;
            case 50: return CORUN_GRP_MENU;
            case 51: return CORUN_GRP_BACK;
            case 79: return CORUN_GRP_MASTER;
            case 88: return CORUN_GRP_MUTE;
            case 85: return CORUN_GRP_PLAY;
            case 86: return CORUN_GRP_REC;
            case 118: return CORUN_GRP_SAMPLE;
            case 58: return CORUN_GRP_LOOP;
            case 60: return CORUN_GRP_COPY;
            case 119: return CORUN_GRP_DELETE;
            case 56: return CORUN_GRP_UNDO;
            case 52: return CORUN_GRP_CAPTURE;
            case 55: return CORUN_GRP_NAV_UP;
            case 54: return CORUN_GRP_NAV_DOWN;
            case 62: return CORUN_GRP_NAV_LEFT;
            case 63: return CORUN_GRP_NAV_RIGHT;
            default: if (d1 >= 71 && d1 <= 78) return CORUN_GRP_KNOBS;
                     return 0;
        }
    }
    if (type == 0x90 || type == 0x80) {
        if (d1 <= 9) return CORUN_GRP_TOUCH;
        if (d1 >= 16 && d1 <= 31) return CORUN_GRP_STEPS; /* step-button row */
        if (d1 >= 68 && d1 <= 99) return CORUN_GRP_PADS;
    }
    return 0;
}

/* Translate a module's cede-list into the internal keep-mask: it keeps the whole
 * routable surface except what it cedes. Used by the host's cede-model API. */
static inline uint32_t corun_cede_to_keep(uint32_t cede_mask) {
    return (~cede_mask) & CORUN_GRP_ALL;
}

/* Effective input keep-mask. Cede-model tools store a complete complemented mask
 * (keep_mask==0 legitimately means "keep nothing / cede everything"), so it is
 * returned verbatim. Legacy tools leave keep_mask==0 to mean "default split". */
static inline uint32_t corun_keep_mask_eff(const volatile shadow_control_t *ctrl) {
    if (!ctrl) return CORUN_KEEP_DEFAULT;
    if (ctrl->corun.flags & CORUN_F_CEDE_MODEL) return ctrl->corun.keep_mask;
    return ctrl->corun.keep_mask ? ctrl->corun.keep_mask : CORUN_KEEP_DEFAULT;
}

/* Effective LED keep-mask: which groups the tool owns for LED stripping. A tool
 * that paints a surface but cedes its input sets led_keep_mask separately. When
 * unset (0) and not flagged distinct, LED ownership follows the input mask, so
 * the common case (own both) needs no extra call. CORUN_F_LED_DISTINCT makes the
 * led mask authoritative even at 0 (needed by cede-model tools, where 0 is a
 * meaningful "cede all LEDs" value rather than the follow sentinel). */
static inline uint32_t corun_led_keep_mask_eff(const volatile shadow_control_t *ctrl) {
    if (!ctrl) return CORUN_KEEP_DEFAULT;
    uint32_t led = ctrl->corun.led_keep_mask;
    if ((ctrl->corun.flags & CORUN_F_LED_DISTINCT) || led) return led;
    return corun_keep_mask_eff(ctrl);
}

/* Co-run target. Stored in shadow_control->corun.target as int8_t. */
typedef enum {
    CORUN_TARGET_NONE        = 0,
    CORUN_TARGET_CHAIN_EDIT  = 1, /* Schwung chain editor co-runs (id = chain slot 0-3) */
    CORUN_TARGET_MOVE_NATIVE = 2, /* Move firmware co-runs (id = tool track 0-7) */
} corun_target_t;

/* OLED ownership during a co-run / shadow session. Stored as uint8_t. */
typedef enum {
    DISPLAY_OWNER_TOOL          = 0, /* the active tool (default, including non-shadow Move mode) */
    DISPLAY_OWNER_SCHWUNG_UI    = 1, /* the schwung shadow UI (overtake menu, chain editor, …) */
    DISPLAY_OWNER_MOVE_FIRMWARE = 2, /* Move firmware (during move_native co-run) */
} display_owner_t;

/* Result of corun_event_owner: which side a given control-surface event belongs
 * to right now. Each call site switches on this once instead of mirroring the
 * `grp && !(keep_mask & grp)` check. */
typedef enum {
    CORUN_OWNER_TOOL = 0, /* event goes to the active tool */
    CORUN_OWNER_PEER,     /* event goes to the co-run UI (chain editor or Move firmware) */
    CORUN_OWNER_BOTH,     /* event reaches both (currently unused; reserved for future shared gestures) */
    CORUN_OWNER_NONE,     /* event is consumed by the framework, reaches neither (e.g. Back exit gesture during co-run) */
} corun_owner_t;

/* Accessors — never touch the corun struct directly. */
static inline int corun_active(const volatile shadow_control_t *ctrl) {
    return ctrl && ctrl->corun.target != CORUN_TARGET_NONE;
}
static inline corun_target_t corun_target(const volatile shadow_control_t *ctrl) {
    return ctrl ? (corun_target_t)ctrl->corun.target : CORUN_TARGET_NONE;
}
static inline int corun_id(const volatile shadow_control_t *ctrl) {
    return ctrl ? (int)(int8_t)ctrl->corun.id : -1;
}
static inline uint32_t corun_keep_mask(const volatile shadow_control_t *ctrl) {
    return ctrl ? ctrl->corun.keep_mask : 0;
}

/* Single source of truth for "who owns this event right now?". Both the sh_midi
 * let-through filter and the forward-to-shadow_ui suppress filter call this and
 * switch on the result. Adding a new corun target = extend this function; no
 * mirror checks anywhere else can drift. */
static inline corun_owner_t corun_event_owner(const volatile shadow_control_t *ctrl, uint8_t type, uint8_t d1) {
    if (!corun_active(ctrl)) return CORUN_OWNER_TOOL;
    uint32_t grp = corun_group_for_event(type, d1);
    int cede_model = (ctrl->corun.flags & CORUN_F_CEDE_MODEL) != 0;
    /* Back: framework-reserved as the exit gesture by default — the shim's own
     * handler ends the session on press, neither side sees the event. A tool that
     * prefers its own exit gesture signals it (cede model: CORUN_F_OWN_BACK;
     * legacy: CORUN_KEEP_BACK in keep_mask); Back then routes per the normal
     * keep/cede rule below (to the peer for sub-view nav unless the tool also
     * keeps CORUN_GRP_BACK). (Menu remains tool-owned by default.) */
    int own_back = cede_model ? (ctrl->corun.flags & CORUN_F_OWN_BACK)
                              : (ctrl->corun.keep_mask & CORUN_KEEP_BACK);
    if (grp == CORUN_GRP_BACK && !own_back) {
        return CORUN_OWNER_NONE;
    }
    if (!grp) return CORUN_OWNER_TOOL; /* unclassified (sensor) events stay with tool */
    /* Legacy carve-out: a pre-cede tool never named the extended buttons, so they
     * stay with it regardless of its keep_mask — byte-identical to pre-cede
     * behavior. Cede-model tools govern the full surface uniformly (no carve-out). */
    if (!cede_model && (grp & CORUN_GRP_EXTENDED_ALL)) return CORUN_OWNER_TOOL;
    uint32_t keep = corun_keep_mask_eff(ctrl);
    return (keep & grp) ? CORUN_OWNER_TOOL : CORUN_OWNER_PEER;
}

/*
 * UI state structure for slot information.
 * Must fit within SHADOW_UI_BUFFER_SIZE bytes.
 */
typedef struct shadow_ui_state_t {
    uint32_t version;
    uint8_t slot_count;
    uint8_t reserved[3];
    uint8_t slot_channels[SHADOW_UI_SLOTS];      /* 0=all, 1-16=specific channel */
    uint16_t slot_volumes[SHADOW_UI_SLOTS];      /* 0-400 percentage */
    int8_t slot_forward_ch[SHADOW_UI_SLOTS];     /* -2=passthrough, -1=auto, 0-15=channel */
    char slot_names[SHADOW_UI_SLOTS][SHADOW_UI_NAME_LEN];
    /* --- version 2 --- Appended, so every offset above is unchanged and a
     * mismatched pair reads zeros here rather than garbage. Consumers must
     * still gate on `version >= 2`: zero means "not muted", which is a
     * plausible-looking wrong answer against a stale shim (a known failure
     * mode — see the web-update shim mirroring in schwung-manager).
     *
     * These exist because reading them over the param SHM cost more than
     * everything else the UI does put together: measured on device, the slot
     * list's per-draw `slot:muted`/`slot:soloed` reads were 81% of ALL
     * parameter reads, at ~2.9ms per synchronous round trip, to decide
     * whether to draw an "M" or an "S". Mirrored here they cost nothing.
     * Every mutation path already funnels through
     * shadow_ui_state_update_slot(), which publishes them. */
    uint8_t slot_muted[SHADOW_UI_SLOTS];         /* 0/1 */
    uint8_t slot_soloed[SHADOW_UI_SLOTS];        /* 0/1 */
} shadow_ui_state_t;

/* Bump when a field is APPENDED to shadow_ui_state_t; consumers gate on it.
 * Never reorder or resize an existing field — the two processes are deployed
 * as separate files and can be out of step across an upgrade. */
#define SHADOW_UI_STATE_VERSION 2

/*
 * Parameter request structure for get/set operations.
 * Must fit within SHADOW_PARAM_BUFFER_SIZE bytes.
 */
/*
 * request_type value used to CLAIM the channel before the request is filled
 * in. The channel is a single slot shared by TWO processes (shadow_ui and
 * schwung-manager), so "spin until it reads 0, then write it" is a
 * time-of-check/time-of-use race — both can see 0 and both write, and the
 * loser waits out its whole timeout for a response_id that never comes.
 * Clients claim with a compare-exchange 0 -> CLAIMED instead.
 *
 * The servicer MUST treat CLAIMED as "busy, not mine": the key, slot and
 * request_id are not written yet, so serving it would answer a garbage
 * request and — worse — clear request_type, releasing a claim its owner
 * still believes it holds.
 */
#define SHADOW_PARAM_CLAIMED 0xFF

/*
 * Request-ID ranges are DISJOINT per client, and must stay that way.
 * waitResponse matches on `response_id == my_request_id`, so if two clients
 * can mint the same id, one can match the OTHER's response and read its
 * value — silent wrong data rather than an error.
 *   shadow_ui        : 1 .. 0xFFFEFFFF
 *   schwung-manager  : 0xFFFF0000 ..   (also what publish_response uses to
 *                      recognise web-originated SETs and skip re-notifying)
 */
#define SHADOW_PARAM_WEB_REQ_ID_BASE 0xFFFF0000u

typedef struct shadow_param_t {
    volatile uint8_t request_type;   /* 0=none, 1=set, 2=get, 0xFF=claimed */
    volatile uint8_t slot;           /* Which chain slot (0-3) */
    volatile uint8_t response_ready; /* Set by shim when response is ready */
    volatile uint8_t error;          /* Non-zero on error */
    volatile uint32_t request_id;    /* Monotonic request ID assigned by shadow UI */
    volatile uint32_t response_id;   /* Request ID this response corresponds to */
    volatile int32_t result_len;     /* Length of result, -1 on error */
    /* OTLP trace context (Phase 2b): shadow_ui stamps the open param.get span so
     * the shim can emit param.serve as its child. 0 = untraced. */
    volatile uint64_t trace_id;
    volatile uint64_t parent_span_id;
    char key[SHADOW_PARAM_KEY_LEN];
    char value[SHADOW_PARAM_VALUE_LEN];
} shadow_param_t;

/*
 * MIDI output structure for shadow UI to send MIDI to hardware.
 * Used by overtake modules (M8, MIDI Controller, etc.) to send MIDI
 * to external USB devices (cable 2) or control Move LEDs (cable 0).
 */
typedef struct shadow_midi_out_t {
    /* uint16_t, and it MUST be: the buffer is SHADOW_MIDI_OUT_BUFFER_SIZE
     * bytes (now 1024) and this is a BYTE offset into it. As a uint8_t it
     * saturated at 255, so
     *   - only the first 63 packets of a 128-packet buffer were addressable,
     *     and the back half was never read at all;
     *   - `write_idx = write_offset + 4` wrapped 252 -> 0, silently rewinding
     *     the buffer so later packets overwrote earlier ones mid-flush;
     *   - the `write_offset + 4 <= SHADOW_MIDI_OUT_BUFFER_SIZE` bounds check
     *     in js_shadow_midi_send could never fire, because a uint8_t cannot
     *     reach 512. The guard read as a working overflow check and was dead.
     * This is the "~64 packets, >60/frame overflows" limit CLAUDE.md recorded
     * as a property of the buffer; it was a property of THIS FIELD. Widening it
     * costs nothing — it takes one of the reserved bytes, so sizeof is
     * unchanged and both mappers (shadow_ui.c:83, schwung_shim.c:3417) use
     * sizeof. */
    volatile uint16_t write_idx;     /* Shadow UI increments after writing */
    volatile uint8_t ready;          /* Toggle to signal new data */
    volatile uint8_t reserved[1];
    uint8_t buffer[SHADOW_MIDI_OUT_BUFFER_SIZE];  /* USB-MIDI packets (4 bytes each) */
} shadow_midi_out_t;

/* The field above is load-bearing and its width is not obvious from its uses.
 * A byte offset into the buffer must be able to reach the end of it. */
_Static_assert((1ull << (8 * sizeof(((shadow_midi_out_t *)0)->write_idx))) >
                   SHADOW_MIDI_OUT_BUFFER_SIZE,
               "shadow_midi_out_t.write_idx cannot address its own buffer");

/*
 * MIDI-to-DSP structure for shadow UI to send MIDI to chain DSP slots.
 * Used by overtake modules to route MIDI to sound generators/effects.
 * Messages are raw 3-byte MIDI (status, data1, data2), stored 4-byte aligned.
 */
typedef struct shadow_midi_dsp_t {
    volatile uint8_t write_idx;      /* Shadow UI increments after writing */
    volatile uint8_t ready;          /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    uint8_t buffer[SHADOW_MIDI_DSP_BUFFER_SIZE];  /* Raw MIDI (4 bytes each: status, d1, d2, pad) */
} shadow_midi_dsp_t;

/*
 * MIDI Inject ring — multi-producer, single-consumer SHM channel.
 * Lets multiple processes inject USB-MIDI packets into Move's MIDI_IN
 * buffer so Move firmware processes them as real hardware events.
 * Packets are 4-byte USB-MIDI format (CIN/cable, status, data1, data2).
 *
 * Producers (multiple, may run concurrently across processes):
 *   - JS host: shadow_ui.c:js_move_midi_inject_to_move (schwung_host)
 *   - Shim:    schwung_shim.c overtake-entry "shift off"
 *   - Shim:    schwung_shim.c overtake-exit 4× CC release
 *   - Shim:    shadow_midi.c:shadow_chain_midi_inject (cable-2 forwarder)
 * Overtake DSP midi_inject_to_move is intentionally separate; see
 * shadow_overtake_midi.c. While overtake is active this shared queue has a
 * different single consumer: the takeover test-bus publisher.
 *
 * Consumer (single, in shim's SPI thread, ~344 Hz):
 *   - shadow_midi.c:shadow_drain_midi_inject
 *
 * Design — bounded MPSC queue (Dmitry Vyukov's per-slot-sequence scheme):
 * an array of fixed slots, each carrying a `seq` number and one 4-byte
 * packet. Indices are monotonic and masked into the slot array; nothing
 * is ever reset or memset.
 *
 *   enqueue_pos — producers claim a slot by fetch/CAS-advancing this
 *                 monotonic counter; the slot is slots[pos & MASK].
 *   read_pos    — the single consumer's cursor; it advances only past
 *                 packets it has actually consumed.
 *   slot.seq    — publication state. Initialized to its index i. A
 *                 producer that claimed `pos` publishes by storing
 *                 seq = pos+1 (RELEASE); the consumer detects "ready"
 *                 when seq == read_pos+1 (ACQUIRE) and frees the slot for
 *                 a future lap by storing seq = pos + SLOTS (RELEASE).
 *
 * Why this and not a byte ring with alloc/commit cursors + a reset:
 *   - The consumer never resets cursors or memsets the buffer, so it
 *     cannot race a cross-process producer mid-write (the old reset path
 *     could satisfy a producer's commit-wait with reset-to-0 and let it
 *     write into wiped space → cable=0/CIN=0 packet → Move SIGABRT).
 *   - Producers never wait for each other to commit (no FIFO-commit spin),
 *     so a producer preempted between claim and publish cannot stall the
 *     SPI-thread producer on the realtime path. A preempted producer only
 *     delays the consumer past its one slot, and self-heals on resume.
 *   - All cursor/seq access is atomic — no volatile-store-vs-atomic-CAS
 *     data race.
 *
 * All producers MUST go through shadow_midi_inject_push() and the consumer
 * through shadow_midi_inject_peek()/_pop() in shadow_midi_inject_writer.h —
 * do not touch slots or cursors directly. Ad-hoc writers race the consumer
 * (a torn / zero packet reaches MIDI_IN, where cable=0/CIN=0 reads as "misc
 * function" and Move firmware aborts — see shadow_midi.c).
 *
 * The SHM must be initialized once by its creator (the shim) via
 * shadow_midi_inject_init() before any producer maps it — zero-fill alone
 * is NOT a valid initial state (it needs seq[i] = i). Layout/size changed
 * vs the prior byte-ring, so shim + schwung_host must be built together.
 */
#define SHADOW_MIDI_INJECT_SLOTS 64   /* power of two; ring capacity in packets */
#define SHADOW_MIDI_INJECT_MASK  (SHADOW_MIDI_INJECT_SLOTS - 1)

typedef struct shadow_midi_inject_slot_t {
    uint32_t seq;        /* publication sequence (accessed via __atomic_*) */
    uint8_t  pkt[4];     /* one 4-byte USB-MIDI packet (cable/CIN, status, d1, d2) */
} shadow_midi_inject_slot_t;   /* 8 bytes, naturally aligned */

typedef struct shadow_midi_inject_t {
    uint32_t enqueue_pos;   /* producer claim cursor (monotonic, __atomic_*) */
    uint32_t read_pos;      /* single-consumer cursor (monotonic, __atomic_*) */
    shadow_midi_inject_slot_t slots[SHADOW_MIDI_INJECT_SLOTS];
} shadow_midi_inject_t;

/*
 * Cable-2 (external USB) MIDI channel remap table.
 * Active overtake module writes; shim reads on every SPI frame and
 * rewrites the channel byte of cable-2 MIDI_IN events before Move
 * firmware processes them. Solves the cable-2 echo cascade by
 * remapping in-place rather than re-injecting from JS.
 *
 * Disabled globally whenever any chain slot is configured forward=THRU
 * (MPE passthrough). Reset to all-passthrough by the shim on overtake
 * exit (forced — never trusted to JS, since shadow_ui cleanup may not
 * run on crash).
 */
typedef struct schwung_ext_midi_remap_t {
    volatile uint8_t version;        /* 1 = current contract version */
    volatile uint8_t enabled;        /* 0 = bypass, 1 = active */
    volatile uint8_t remap[16];      /* remap[in_ch] = out_ch (0-indexed).
                                      * 0xFF = passthrough for that channel. */
    uint8_t _reserved[46];           /* reserved for v2 (per-source remap, etc) */
} schwung_ext_midi_remap_t;          /* 64 bytes total */

#define EXT_MIDI_REMAP_PASSTHROUGH 0xFF
#define EXT_MIDI_REMAP_BLOCK       0xFE  /* block note-ons from Move; shadow_ui still sees original */
#define EXT_MIDI_REMAP_VERSION     1

/*
 * Web UI param set ring — web server writes, shim drains each audio block.
 * Fire-and-forget: no response needed. ~3ms latency (one audio block).
 * Uses linear buffer with toggle-ready pattern (same as MIDI inject).
 */
typedef struct web_param_set_entry_t {
    uint8_t slot;
    uint8_t reserved[3];
    char key[WEB_PARAM_KEY_LEN];
    char value[WEB_PARAM_VALUE_LEN];
} web_param_set_entry_t;

typedef struct web_param_set_ring_t {
    volatile uint8_t write_idx;    /* Web server increments after writing */
    volatile uint8_t ready;        /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    web_param_set_entry_t entries[WEB_PARAM_SET_ENTRIES];
} web_param_set_ring_t;

/*
 * Web UI param notify ring — shim writes when params change, web server reads.
 * Pushes changed values to browser via WebSocket without polling.
 */
typedef struct web_param_notify_entry_t {
    uint8_t slot;
    uint8_t reserved[3];
    char key[WEB_PARAM_KEY_LEN];
    char value[WEB_PARAM_VALUE_LEN];
} web_param_notify_entry_t;

typedef struct web_param_notify_ring_t {
    volatile uint8_t write_idx;    /* Shim increments after writing */
    volatile uint8_t ready;        /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    web_param_notify_entry_t entries[WEB_PARAM_NOTIFY_ENTRIES];
} web_param_notify_ring_t;

/*
 * Screen reader message structure.
 * Supports both D-Bus announcements and on-device TTS.
 * Shadow UI writes text and updates fields, shim reads and processes.
 */
typedef struct shadow_screenreader_t {
    volatile uint32_t sequence;      /* Incremented for each new message (TTS) */
    volatile uint32_t timestamp_ms;  /* Timestamp of message (for rate limiting) */
    char text[SHADOW_SCREENREADER_TEXT_LEN];
} shadow_screenreader_t;

/* ============================================================================
 * Overlay State (sampler/skipback, shared from shim to shadow UI)
 * ============================================================================ */

#define SHADOW_OVERLAY_NONE       0
#define SHADOW_OVERLAY_SAMPLER    1
#define SHADOW_OVERLAY_SKIPBACK   2
#define SHADOW_OVERLAY_SHIFT_KNOB 3
#define SHADOW_OVERLAY_SET_PAGE   4

#define SHADOW_SAMPLER_IDLE       0
#define SHADOW_SAMPLER_ARMED      1
#define SHADOW_SAMPLER_RECORDING  2
#define SHADOW_SAMPLER_PREROLL    3
#define SHADOW_SAMPLER_PAUSED    4

/*
 * Overlay state structure for communication from shim to shadow UI.
 * The shim publishes sampler/skipback state here; JS reads it to render overlays.
 * Must fit within SHADOW_OVERLAY_BUFFER_SIZE bytes.
 */
typedef struct shadow_overlay_state_t {
    volatile uint32_t sequence;             /* Incremented on state change; JS polls cheaply */

    volatile uint8_t  overlay_type;         /* NONE / SAMPLER / SKIPBACK */
    volatile uint8_t  sampler_state;        /* IDLE / ARMED / RECORDING / PREROLL / PAUSED */
    volatile uint8_t  sampler_source;       /* 0=Resample, 1=Move Input */
    volatile uint8_t  sampler_cursor;       /* 0=Source menu, 1=Duration menu */

    volatile uint8_t  sampler_fullscreen;   /* 1 = fullscreen takeover */
    volatile uint8_t  skipback_active;      /* 1 = show toast */
    volatile uint16_t sampler_duration_bars; /* 0=until stop, 1/2/4/8/16 */

    volatile int16_t  sampler_vu_peak;      /* Raw peak (0-32767), updated at audio rate */
    volatile uint16_t sampler_bars_completed;
    volatile uint16_t sampler_target_bars;
    volatile uint16_t sampler_overlay_timeout;  /* Frames left for "saved" msg */
    volatile uint16_t skipback_overlay_timeout; /* Frames left for toast */

    volatile uint32_t sampler_samples_written;
    volatile uint32_t sampler_clock_count;
    volatile uint32_t sampler_target_pulses;
    volatile uint32_t sampler_fallback_blocks;
    volatile uint32_t sampler_fallback_target;
    volatile uint8_t  sampler_clock_received;
    volatile uint8_t  transport_playing;       /* 1 = MIDI Start seen, 0 = MIDI Stop seen */

    /* Shift+knob overlay */
    volatile uint8_t  shift_knob_active;        /* 1 = showing shift+knob overlay */
    volatile uint16_t shift_knob_timeout;       /* Frames remaining */
    char shift_knob_patch[64];                  /* Patch/slot name */
    char shift_knob_param[64];                  /* Parameter name */
    char shift_knob_value[32];                  /* Parameter value */

    /* Set page overlay */
    volatile uint8_t  set_page_active;            /* 1 = showing set page toast */
    volatile uint8_t  set_page_current;           /* Current page (0-7) */
    volatile uint8_t  set_page_total;             /* Total pages (8) */
    volatile uint8_t  set_page_loading;           /* 1 = loading (pre-restart), 0 = loaded */
    volatile uint16_t set_page_timeout;           /* Frames remaining for toast */

    /* Preroll state */
    volatile uint8_t  sampler_preroll_enabled;    /* 0=off, 1=on */
    volatile uint8_t  sampler_preroll_active;     /* 1 = currently in preroll countdown */
    volatile uint16_t sampler_preroll_bars_done;  /* Bars completed in preroll */

    volatile float    sampler_bpm;                /* Project BPM from MIDI clock or fallback */

    /* Pad LED colors (notes 68-99, written by shim from Move's MIDI_OUT cache).
     * Index 0 = note 68 (track 4 pad A), index 31 = note 99 (track 1 pad H). */
    volatile uint8_t  pad_led_colors[32];  /* velocity/color for each pad */
} shadow_overlay_state_t;

/* ============================================================================
 * Test-stream rings (Phase 2 of E2E test infra, flagist0/schwung#2)
 * ============================================================================
 *
 * SPSC channel: shim writes, schwung-testd reads. Used to expose
 * shim-internal events (currently: every MIDI_OUT packet observed in
 * Move's mailbox each SPI frame) to a subscribed test runner so it can
 * assert on the sequence of events emitted between two test actions.
 *
 * Capture is gated by `enabled` — daemon sets it on first SUBSCRIBE,
 * clears it on UNSUBSCRIBE. When disabled the shim's per-frame check is
 * one atomic load + a branch (~1 ns), so the cost is negligible when
 * tests aren't running.
 *
 * Ring uses a monotonic uint32 sequence cursor: writer keeps incrementing
 * `write_seq` and storing into `buffer[write_seq % CAPACITY]`. Reader tracks
 * its own `last_read`; if the writer ran more than CAPACITY events past the
 * reader, the oldest were overwritten — reader fast-forwards and increments
 * dropped. Single producer (shim SPI post-transfer) + single consumer
 * (schwung-testd); ACQUIRE/RELEASE on write_seq carries the buffer writes.
 */

#define TEST_STREAM_CAPACITY 1024  /* events per channel (8 KB / channel) */

typedef struct test_stream_event {
    uint32_t frame;     /* shim_counter at capture time */
    uint8_t  pkt[4];    /* USB-MIDI packet: cable+CIN, status, d1, d2 */
} test_stream_event_t;  /* 8 bytes — keep tight */

typedef struct test_stream_shm {
    volatile uint32_t enabled;     /* 0 = no capture, 1 = capturing */
    volatile uint32_t write_seq;   /* monotonic event count (mod CAPACITY = slot) */
    volatile uint32_t dropped;     /* total events dropped since enabled (informational) */
    volatile uint32_t reserved;
    test_stream_event_t buffer[TEST_STREAM_CAPACITY];
} test_stream_shm_t;

/* Compile-time size checks */
typedef char test_stream_size_check[(sizeof(test_stream_shm_t) == 16 + TEST_STREAM_CAPACITY * 8) ? 1 : -1];
/*
 * FITS IN, rather than EQUALS. The buffer is a container with headroom now, so
 * adding a field is free; `==` meant every addition had to bump the constant,
 * and a failing assert reads as "there is no room" rather than "say how much
 * you want".
 *
 * The second assert is the one that still matters: the buffer must never
 * SHRINK. A tidy-up that resized it down to sizeof would make every already
 * deployed consumer's attach fail against the smaller segment.
 */
typedef char shadow_control_size_check[(sizeof(shadow_control_t) <= CONTROL_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_control_floor_check[(CONTROL_BUFFER_SIZE >= 256) ? 1 : -1];
typedef char shadow_ui_state_size_check[(sizeof(shadow_ui_state_t) <= SHADOW_UI_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_param_size_check[(sizeof(shadow_param_t) <= SHADOW_PARAM_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_screenreader_size_check[(sizeof(shadow_screenreader_t) <= SHADOW_SCREENREADER_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_overlay_size_check[(sizeof(shadow_overlay_state_t) <= SHADOW_OVERLAY_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_overlay_floor_check[(SHADOW_OVERLAY_BUFFER_SIZE >= 512) ? 1 : -1];
typedef char schwung_ext_midi_remap_size_check[(sizeof(schwung_ext_midi_remap_t) == 64) ? 1 : -1];

#endif /* SHADOW_CONSTANTS_H */
