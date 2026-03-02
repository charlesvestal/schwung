#define _GNU_SOURCE

#ifndef ENABLE_SCREEN_READER
#define ENABLE_SCREEN_READER 1
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <math.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#if ENABLE_SCREEN_READER
#include <dbus/dbus.h>
#include <systemd/sd-bus.h>
#endif

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/shadow_constants.h"
#include "host/shadow_chain_types.h"
#include "host/unified_log.h"
#include "host/tts_engine.h"
#include "host/link_audio.h"
#include "host/shadow_sampler.h"
#include "host/shadow_set_pages.h"
#include "host/shadow_dbus.h"
#include "host/shadow_chain_mgmt.h"
#include "host/shadow_link_audio.h"
#include "host/shadow_process.h"
#include "host/shadow_resample.h"
#include "host/shadow_overlay.h"
#include "host/shadow_pin_scanner.h"
#include "host/shadow_led_queue.h"
#include "host/shadow_fd_trace.h"
#include "host/shadow_state.h"
#include "host/shadow_midi.h"

/* Debug flags - set to 1 to enable various debug logging */
#define SHADOW_TIMING_LOG 0      /* ioctl/DSP timing logs to /tmp */

unsigned char *global_mmap_addr = NULL;  /* Points to shadow_mailbox (what Move sees) */
unsigned char *hardware_mmap_addr = NULL; /* Points to real hardware mailbox */
static unsigned char shadow_mailbox[4096] __attribute__((aligned(64))); /* Shadow buffer for Move */

/* ============================================================================
 * SHADOW INSTRUMENT SUPPORT
 * ============================================================================
 * The shadow instrument allows a separate DSP process to run alongside stock
 * Move, mixing its audio output with Move's audio and optionally taking over
 * the display when in shadow mode.
 * ============================================================================ */

/* Mailbox layout constants */
#define MAILBOX_SIZE 4096
#define MIDI_OUT_OFFSET 0
#define AUDIO_OUT_OFFSET 256
#define DISPLAY_OFFSET 768
#define MIDI_IN_OFFSET 2048
#define AUDIO_IN_OFFSET 2304

#define AUDIO_BUFFER_SIZE 512      /* 128 frames * 2 channels * 2 bytes */
/* Buffer sizes from shadow_constants.h: MIDI_BUFFER_SIZE, DISPLAY_BUFFER_SIZE,
   CONTROL_BUFFER_SIZE, SHADOW_UI_BUFFER_SIZE, SHADOW_PARAM_BUFFER_SIZE */
/* FRAMES_PER_BLOCK is now defined in shadow_constants.h */

/* Move host shortcut CCs (mirror move_anything.c) */
#define CC_SHIFT 49
#define CC_JOG_CLICK 3
#define CC_JOG_WHEEL 14
#define CC_BACK 51
#define CC_MASTER_KNOB 79
#define CC_UP 55
#define CC_DOWN 54
#define CC_MENU 50
#define CC_CAPTURE 52
#define CC_UNDO 56
#define CC_LOOP 58
#define CC_COPY 60
#define CC_LEFT 62
#define CC_RIGHT 63
#define CC_KNOB1 71
#define CC_KNOB2 72
#define CC_KNOB3 73
#define CC_KNOB4 74
#define CC_KNOB5 75
#define CC_KNOB6 76
#define CC_KNOB7 77
#define CC_KNOB8 78
#define CC_PLAY 85
#define CC_REC 86
#define CC_SAMPLE 87
#define CC_MUTE 88
#define CC_MIC_IN_DETECT 114
#define CC_LINE_OUT_DETECT 115
#define CC_RECORD 118
#define CC_DELETE 119
#define CC_STEP_UI_FIRST 16
#define CC_STEP_UI_LAST 31

/* Shadow structs from shadow_constants.h: shadow_control_t, shadow_ui_state_t, shadow_param_t */
static shadow_control_t *shadow_control = NULL;
static uint8_t shadow_display_mode = 0;

static shadow_ui_state_t *shadow_ui_state = NULL;

static shadow_param_t *shadow_param = NULL;
static shadow_screenreader_t *shadow_screenreader_shm = NULL;  /* Forward declaration for D-Bus handler */
static shadow_overlay_state_t *shadow_overlay_shm = NULL;     /* Overlay state for JS rendering */

/* Display mode save/restore for overlay forcing */
/* display_overlay in shadow_control_t replaces the old display_mode forcing */

/* shadow_overlay_sync — now in shadow_overlay.c (via shadow_overlay.h) */
static volatile float shadow_master_volume;  /* Defined later */

/* Feature flags from config/features.json */
static bool shadow_ui_enabled = true;      /* Shadow UI enabled by default */
static bool standalone_enabled = true;     /* Standalone mode enabled by default */
static bool display_mirror_enabled = false; /* Display mirror off by default */
static bool set_pages_enabled = true;      /* Set pages enabled by default */

/* Link Audio state, process management — moved to shadow_link_audio.c, shadow_process.c */

/* Link Audio publisher shared memory (shim → link_subscriber) */
static link_audio_pub_shm_t *shadow_pub_audio_shm = NULL;

static void load_feature_config(void);

static uint32_t shadow_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum * 33u) ^ buf[i];
    }
    return sum;
}


/* ============================================================================
 * IN-PROCESS SHADOW CHAIN (MULTI-PATCH)
 * ============================================================================
 * Load the chain DSP inside the shim and render in the ioctl audio cadence.
 * This avoids IPC timing drift and provides a stable audio mix proof-of-concept.
 * ============================================================================ */

#define SHADOW_INPROCESS_POC 1
#define SHADOW_DISABLE_POST_IOCTL_MIDI 0  /* Set to 1 to disable post-ioctl MIDI forwarding for debugging */
/* Path constants now in shadow_set_pages.h:
 * SHADOW_CHAIN_CONFIG_PATH, SLOT_STATE_DIR, SET_STATE_DIR, ACTIVE_SET_PATH */
/* SHADOW_CHAIN_INSTANCES from shadow_constants.h */

/* System volume - for now just a placeholder, we'll find the real source */
static float shadow_master_gain = 1.0f;

/* Forward declaration */
static uint64_t now_mono_ms(void);


/* Overtake DSP state - loaded when an overtake module has a dsp.so */
static void *overtake_dsp_handle = NULL;           /* dlopen handle */
static plugin_api_v2_t *overtake_dsp_gen = NULL;   /* V2 generator plugin */
static void *overtake_dsp_gen_inst = NULL;          /* Generator instance */
static audio_fx_api_v2_t *overtake_dsp_fx = NULL;  /* V2 FX plugin */
static void *overtake_dsp_fx_inst = NULL;           /* FX instance */
static host_api_v1_t overtake_host_api;             /* Host API provided to plugin */

/* Forward declarations for overtake DSP */
static void shadow_overtake_dsp_load(const char *path);
static void shadow_overtake_dsp_unload(void);

/* Startup mod wheel reset countdown - resets mod wheel after Move finishes its startup MIDI burst */
#define STARTUP_MODWHEEL_RESET_FRAMES 20  /* ~0.6 seconds at 128 frames/block */
static int shadow_startup_modwheel_countdown = 0;

/* Deferred DSP rendering buffer - rendered post-ioctl, mixed pre-ioctl next frame.
 * Used for overtake DSP and as fallback when chain_process_fx is unavailable. */
static int16_t shadow_deferred_dsp_buffer[FRAMES_PER_BLOCK * 2];
static int shadow_deferred_dsp_valid = 0;

/* Per-slot raw synth output from render_to_buffer (no FX applied).
 * FX is processed in mix_from_buffer using same-frame Link Audio data. */
static int16_t shadow_slot_deferred[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];
static int shadow_slot_deferred_valid[SHADOW_CHAIN_INSTANCES];

/* Per-slot idle detection: skip render_block when output has been silent.
 * Wakes on MIDI dispatch with one-frame latency (2.9ms, inaudible). */
#define DSP_IDLE_THRESHOLD 344       /* ~1 second of silence before sleeping */
#define DSP_SILENCE_LEVEL 4          /* abs(sample) below this = silence */
static int shadow_slot_silence_frames[SHADOW_CHAIN_INSTANCES];
static int shadow_slot_idle[SHADOW_CHAIN_INSTANCES];
/* Phase 2: track FX output silence to skip FX processing too.
 * FX keeps running while reverb/delay tails decay (synth idle, FX active).
 * Once FX output is also silent, skip FX entirely. */
static int shadow_slot_fx_silence_frames[SHADOW_CHAIN_INSTANCES];
static int shadow_slot_fx_idle[SHADOW_CHAIN_INSTANCES];






/* ==========================================================================
 * D-Bus Volume Sync - Monitor Move's track volume via accessibility D-Bus
 * ========================================================================== */

/* Forward declarations */

/* Track button hold state for volume sync: -1 = none held, 0-3 = track 1-4 */
static volatile int shadow_held_track = -1;

/* Selected slot for Shift+Knob routing: 0-3, persists even when shadow UI is off */
static volatile int shadow_selected_slot = 0;

/* Mute button hold state: 1 while CC 88 is held, 0 when released */
static volatile int shadow_mute_held = 0;

/* Set detection globals now in shadow_set_pages.c (extern via shadow_set_pages.h):
 * sampler_set_tempo, sampler_current_set_name, sampler_current_set_uuid,
 * sampler_last_song_index, sampler_pending_song_index, sampler_pending_set_seq */
/* shadow_handle_set_loaded, shadow_poll_current_set now in shadow_set_pages.c */
/* shadow_read_set_mute_states — now in shadow_overlay.c (via shadow_overlay.h) */
static int shim_run_command(const char *const argv[]);  /* forward decl */

/* shadow_apply_mute, shadow_toggle_solo now in shadow_chain_mgmt.c */


/* D-Bus globals, shadow_parse_volume_db, priority announcement state,
 * in_set_overview — all moved to shadow_dbus.c (extern via shadow_dbus.h) */

/* Native Move sampler source tracking (from stock D-Bus announcements) */
/* Native sampler/resample types and globals — moved to shadow_resample.c */

static int shadow_read_global_volume_from_settings(float *linear_out, float *db_out);

/* Native resample bridge types and functions — moved to shadow_resample.c */

/* D-Bus inject_pending, handle_text, native_knob state, connect/send hooks —
 * all moved to shadow_dbus.c. Thin hook stubs remain here. */

/* Hook connect() to capture Move's D-Bus socket FD */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
    if (!real_connect) {
        real_connect = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
    }

    int result = real_connect(sockfd, addr, addrlen);

    if (result == 0 && addr && addr->sa_family == AF_UNIX) {
        struct sockaddr_un *un_addr = (struct sockaddr_un *)addr;
        dbus_on_connect(sockfd, un_addr->sun_path);
    }

    return result;
}

/* Hook send() to intercept Move's D-Bus messages and inject ours */
ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    static ssize_t (*real_send)(int, const void *, size_t, int) = NULL;
    if (!real_send) {
        real_send = (ssize_t (*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
    }

    ssize_t result;
    if (dbus_on_send(sockfd, buf, len, flags, real_send, &result))
        return result;

    return real_send(sockfd, buf, len, flags);
}


/* sendto() hook — intercepts Link Audio packets from Move.
 * Logic moved to shadow_link_audio.c. */
static ssize_t (*real_sendto)(int, const void *, size_t, int,
                              const struct sockaddr *, socklen_t) = NULL;

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    if (!real_sendto) {
        real_sendto = (ssize_t (*)(int, const void *, size_t, int,
                       const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "sendto");
    }

    /* Delegate Link Audio packet handling to extracted module */
    if (link_audio.enabled && len >= 12) {
        link_audio_on_sendto(sockfd, (const uint8_t *)buf, len, dest_addr, addrlen);
    }

    return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}


/* sd_bus hooks, send_screenreader_announcement, D-Bus filter/thread/start/stop —
 * all moved to shadow_dbus.c. Thin hook stubs remain here. */

#if ENABLE_SCREEN_READER
/* Hook sd_bus_default_system to capture Move's sd-bus connection */
int sd_bus_default_system(sd_bus **ret)
{
    static int (*real_default)(sd_bus**) = NULL;
    if (!real_default) {
        real_default = (int (*)(sd_bus**))dlsym(RTLD_NEXT, "sd_bus_default_system");
    }

    int result = real_default(ret);

    if (result >= 0 && ret && *ret) {
        dbus_on_sd_bus_default(*ret);
    }

    return result;
}

/* Hook sd_bus_start to capture Move's sd-bus connection */
int sd_bus_start(sd_bus *bus)
{
    static int (*real_start)(sd_bus*) = NULL;
    if (!real_start) {
        real_start = (int (*)(sd_bus*))dlsym(RTLD_NEXT, "sd_bus_start");
    }

    int result = real_start(bus);

    if (result >= 0 && bus) {
        dbus_on_sd_bus_start(bus);
    }

    return result;
}
#endif /* ENABLE_SCREEN_READER */


/* Update track button hold state from MIDI (called from ioctl hook) */
static void shadow_update_held_track(uint8_t cc, int pressed)
{
    /* Track buttons are CCs 40-43, but in reverse order:
     * CC 43 = Track 1 → slot 0
     * CC 42 = Track 2 → slot 1
     * CC 41 = Track 3 → slot 2
     * CC 40 = Track 4 → slot 3 */
    if (cc >= 40 && cc <= 43) {
        int slot = 43 - cc;  /* Reverse: CC43→0, CC42→1, CC41→2, CC40→3 */
        int old_held = shadow_held_track;
        if (pressed) {
            shadow_held_track = slot;
        } else if (shadow_held_track == slot) {
            shadow_held_track = -1;
        }
        /* Log state changes */
        if (shadow_held_track != old_held) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Track button: CC%d (track %d) %s -> held_track=%d",
                     cc, 4 - (cc - 40), pressed ? "pressed" : "released", shadow_held_track);
            shadow_log(msg);
        }
    }
}

/* ==========================================================================
 * Master Volume Sync - Read from display buffer when volume overlay shown
 * ========================================================================== */

/* Master volume for all shadow audio output (0.0 - 1.0) */
static volatile float shadow_master_volume = 1.0f;
/* Is volume knob currently being touched? (note 8) */
static volatile int shadow_volume_knob_touched = 0;
/* Is jog encoder currently being touched? (note 9) */
static volatile int shadow_jog_touched = 0;
/* Is shift button currently held? (CC 49) - global for cross-function access */
static volatile int shadow_shift_held = 0;
/* Suppress plain volume-touch hide until touch is fully released after
 * Shift+Vol shortcut launches, avoiding a brief native volume flash. */
static volatile int shadow_block_plain_volume_hide_until_release = 0;

/* ==========================================================================
 * Shift+Knob Overlay - Show parameter overlay on Move's display
 * ========================================================================== */

/* Shift+Knob overlay state and constants — moved to shadow_overlay.c */

/* ==========================================================================
 * Set Pages - now in shadow_set_pages.c/.h
 * Constants, globals, and xattr names moved to shadow_set_pages.c.
 * ========================================================================== */
/* in_set_overview now in shadow_dbus.c (extern via shadow_dbus.h) */

/* shadow_ensure_dir, shadow_copy_file, shadow_batch_migrate_sets,
 * shadow_save_config_to_dir, shadow_load_config_from_dir,
 * shadow_get_song_abl_size, shadow_set_name_looks_like_copy,
 * shadow_detect_copy_source, shadow_handle_set_loaded,
 * shadow_poll_current_set — all moved to shadow_set_pages.c */

/* shadow_copy_file — moved to shadow_set_pages.c */
/* shadow_batch_migrate_sets — moved to shadow_set_pages.c */
/* shadow_save_config_to_dir — moved to shadow_set_pages.c */
/* shadow_load_config_from_dir — moved to shadow_set_pages.c */
/* shadow_get_song_abl_size — moved to shadow_set_pages.c */
/* shadow_set_name_looks_like_copy — moved to shadow_set_pages.c */
/* shadow_detect_copy_source — moved to shadow_set_pages.c */
/* shadow_handle_set_loaded — moved to shadow_set_pages.c */
/* shadow_poll_current_set — moved to shadow_set_pages.c */

/* Remaining stub: shadow_batch_migrate_sets was here */

/* Execute a command safely using fork/execvp instead of system() */
static int shim_run_command(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Overlay font, drawing, overlay_sync — moved to shadow_overlay.c */

/* ==========================================================================
 * Set Pages helpers - moved to shadow_set_pages.c
 * (set_page_save_xattrs, set_page_restore_xattrs, set_page_move_dirs,
 *  set_page_persist, set_page_read_persisted, set_page_dbus_fire_and_forget,
 *  set_page_update_song_index, set_page_change_thread, shadow_change_set_page)
 * ========================================================================== */

/* Load feature configuration from config/features.json */
static void load_feature_config(void)
{
    const char *config_path = "/data/UserData/move-anything/config/features.json";
    FILE *f = fopen(config_path, "r");
    if (!f) {
        /* No config file - use defaults (all enabled) */
        shadow_ui_enabled = true;
        standalone_enabled = true;
        shadow_log("Features: No config file, using defaults (all enabled)");
        return;
    }

    /* Read file */
    char config_buf[512];
    size_t len = fread(config_buf, 1, sizeof(config_buf) - 1, f);
    fclose(f);
    config_buf[len] = '\0';

    /* Parse shadow_ui_enabled */
    const char *shadow_ui_key = strstr(config_buf, "\"shadow_ui_enabled\"");
    if (shadow_ui_key) {
        const char *colon = strchr(shadow_ui_key, ':');
        if (colon) {
            /* Skip whitespace */
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "false", 5) == 0) {
                shadow_ui_enabled = false;
            } else {
                shadow_ui_enabled = true;
            }
        }
    }

    /* Parse standalone_enabled */
    const char *standalone_key = strstr(config_buf, "\"standalone_enabled\"");
    if (standalone_key) {
        const char *colon = strchr(standalone_key, ':');
        if (colon) {
            /* Skip whitespace */
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "false", 5) == 0) {
                standalone_enabled = false;
            } else {
                standalone_enabled = true;
            }
        }
    }

    /* Parse link_audio_enabled (defaults to false) */
    const char *link_audio_key = strstr(config_buf, "\"link_audio_enabled\"");
    if (link_audio_key) {
        const char *colon = strchr(link_audio_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0) {
                link_audio.enabled = 1;
            }
        }
    }

    /* Parse display_mirror_enabled (defaults to false) */
    const char *display_mirror_key = strstr(config_buf, "\"display_mirror_enabled\"");
    if (display_mirror_key) {
        const char *colon = strchr(display_mirror_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0) {
                display_mirror_enabled = true;
            }
        }
    }

    /* Parse set_pages_enabled (defaults to true) */
    const char *set_pages_key = strstr(config_buf, "\"set_pages_enabled\"");
    if (set_pages_key) {
        const char *colon = strchr(set_pages_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "false", 5) == 0) {
                set_pages_enabled = false;
            }
        }
    }

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "Features: shadow_ui=%s, standalone=%s, link_audio=%s, display_mirror=%s, set_pages=%s",
             shadow_ui_enabled ? "enabled" : "disabled",
             standalone_enabled ? "enabled" : "disabled",
             link_audio.enabled ? "enabled" : "disabled",
             display_mirror_enabled ? "enabled" : "disabled",
             set_pages_enabled ? "enabled" : "disabled");
    shadow_log(log_msg);
}

static int shadow_read_global_volume_from_settings(float *linear_out, float *db_out)
{
    FILE *f = fopen("/data/UserData/settings/Settings.json", "r");
    if (!f) return 0;

    /* Read file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        fclose(f);
        return 0;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return 0;
    }

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    /* Find "globalVolume": X.X */
    const char *key = "\"globalVolume\":";
    char *pos = strstr(json, key);
    if (!pos) {
        free(json);
        return 0;
    }

    pos += strlen(key);
    while (*pos == ' ') pos++;

    float db = strtof(pos, NULL);
    float linear = (db <= -60.0f) ? 0.0f : powf(10.0f, db / 20.0f);
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 1.0f) linear = 1.0f;

    if (linear_out) *linear_out = linear;
    if (db_out) *db_out = db;

    free(json);
    return 1;
}

/* Read initial volume from Move's Settings.json */
static void shadow_read_initial_volume(void)
{
    float linear = 1.0f;
    float db = 0.0f;
    if (!shadow_read_global_volume_from_settings(&linear, &db)) {
        shadow_log("Master volume: Settings.json not found, defaulting to 1.0");
        return;
    }

    shadow_master_volume = linear;

    char msg[64];
    snprintf(msg, sizeof(msg), "Master volume: read %.1f dB -> %.3f linear", db, shadow_master_volume);
    shadow_log(msg);
}

/* ==========================================================================
 * Shadow State Persistence - Save/load slot volumes to shadow_chain_config.json
 * ========================================================================== */







static void shadow_inprocess_process_midi(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    /* Delayed mod wheel reset - fires after Move's startup MIDI burst settles.
     * This ensures any stale mod wheel values from Move's track state are cleared. */
    if (shadow_startup_modwheel_countdown > 0) {
        shadow_startup_modwheel_countdown--;
        if (shadow_startup_modwheel_countdown == 0) {
            shadow_log("Sending startup mod wheel reset to all slots");
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
                    if (shadow_chain_slots[s].active && shadow_chain_slots[s].instance) {
                        /* Send CC 1 = 0 (mod wheel reset) on all 16 channels */
                        for (int ch = 0; ch < 16; ch++) {
                            uint8_t mod_reset[3] = {(uint8_t)(0xB0 | ch), 1, 0};
                            shadow_plugin_v2->on_midi(shadow_chain_slots[s].instance, mod_reset, 3,
                                                      MOVE_MIDI_SOURCE_HOST);
                        }
                    }
                }
            }
        }
    }

    /* MIDI_IN (internal controls) is NOT routed to DSP here.
     * - Shadow UI handles knobs via set_param based on ui_hierarchy
     * - Capture rules are handled in shadow_filter_move_input (post-ioctl)
     * - Internal notes/CCs should only reach Move, not DSP */

    /* MIDI_OUT → DSP: Move's track output contains only musical notes.
     * Internal controls (knob touches, step buttons) do NOT appear in MIDI_OUT.
     * The buffer is refreshed from hardware after every ioctl (post-ioctl memcpy). */
    const uint8_t *out_src = global_mmap_addr + MIDI_OUT_OFFSET;
    int log_on = shadow_midi_out_log_enabled();
    static int midi_log_count = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        const uint8_t *pkt = &out_src[i];
        if (pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0) continue;

        uint8_t p0 = pkt[0], p1 = pkt[1], p2 = pkt[2], p3 = pkt[3];

        uint8_t cin = p0 & 0x0F;
        uint8_t cable = (p0 >> 4) & 0x0F;
        uint8_t status_usb = p1;

        /* Handle system realtime messages (CIN=0x0F): clock, start, continue, stop
         * These are 1-byte messages that should be broadcast to ALL active slots */
        if (cin == 0x0F && status_usb >= 0xF8 && status_usb <= 0xFF) {
            /* Sampler sees clock from cable 0 only (Move internal) to avoid double-counting */
            if (cable == 0) {
                sampler_on_clock(status_usb);
            }

            /* Only broadcast cable 2 (external USB) clock to slots.
             * Cable 0 = internal, cable 1 = TRS - both are Move's own output */
            if (cable != 2) {
                continue;
            }
            /* Broadcast to all active slots */
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                uint8_t msg[3] = { status_usb, 0, 0 };
                for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
                    if (shadow_chain_slots[s].active && shadow_chain_slots[s].instance) {
                        shadow_plugin_v2->on_midi(shadow_chain_slots[s].instance, msg, 1,
                                                  MOVE_MIDI_SOURCE_EXTERNAL);
                    }
                }
            }
            continue;  /* Done with this packet */
        }

        /* USB MIDI format: CIN in low nibble of byte 0 */
        if (cin >= 0x08 && cin <= 0x0E && (status_usb & 0x80)) {
            if ((status_usb & 0xF0) < 0x80 || (status_usb & 0xF0) > 0xE0) continue;

            /* Validate CIN matches status type (filter garbage/stale data) */
            uint8_t type = status_usb & 0xF0;
            uint8_t expected_cin = (type >> 4);  /* Note-off=0x8, Note-on=0x9, etc. */
            if (cin != expected_cin) {
                continue;  /* CIN doesn't match status - skip invalid packet */
            }

            /* Validate data bytes (MIDI data bytes must be 0-127, high bit clear) */
            if ((p2 & 0x80) || (p3 & 0x80)) {
                continue;  /* Invalid data bytes - skip garbage packet */
            }

            /* Only process cable 2 (external USB) MIDI for shadow chain.
             * Cable 0 = internal, cable 1 = TRS - both are Move's own output */
            if (cable != 2) {
                continue;
            }

            /* Filter internal control notes: knob touches (0-9) */
            if ((type == 0x90 || type == 0x80) && p2 < 10) {
                continue;
            }
            shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count);

            /* Also route to overtake DSP if loaded */
            if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->on_midi) {
                uint8_t msg[3] = { p1, p2, p3 };
                overtake_dsp_gen->on_midi(overtake_dsp_gen_inst, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            } else if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->on_midi) {
                uint8_t msg[3] = { p1, p2, p3 };
                overtake_dsp_fx->on_midi(overtake_dsp_fx_inst, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            }
        }
    }
}

static void shadow_inprocess_mix_audio(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    float mv = shadow_master_volume;
    int mfx_active = shadow_master_fx_chain_active();

    /* When MFX is active, build the mix at unity level so FX see a consistent
     * signal regardless of master volume.  Apply mv AFTER MFX instead.
     * When MFX is off, pre-scale ME by master volume (current behavior). */
    float me_input_scale;
    float move_prescale;
    float link_sub_scale;
    if (mfx_active) {
        me_input_scale = 1.0f;
        link_sub_scale = 1.0f;
        if (mv > 0.001f) {
            move_prescale = 1.0f / mv;
            if (move_prescale > 20.0f) move_prescale = 20.0f;
        } else {
            move_prescale = 1.0f;
        }
    } else {
        me_input_scale = (mv < 1.0f) ? mv : 1.0f;
        move_prescale = 1.0f;
        link_sub_scale = mv;
    }

    /* Save Move's audio for bridge split component (before mixing ME). */
    memcpy(native_bridge_move_component, mailbox_audio, AUDIO_BUFFER_SIZE);

    int32_t mix[FRAMES_PER_BLOCK * 2];
    int32_t me_full[FRAMES_PER_BLOCK * 2];  /* ME at full gain for bridge */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        mix[i] = (int32_t)lroundf((float)mailbox_audio[i] * move_prescale);
        me_full[i] = 0;
    }

    /* Track raw Move audio injected via Link Audio for subtraction */
    int32_t move_injected[FRAMES_PER_BLOCK * 2];
    int any_injected = 0;
    memset(move_injected, 0, sizeof(move_injected));

    if (shadow_plugin_v2 && shadow_plugin_v2->render_block) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;

            /* Inject Move track audio from Link Audio into chain before FX */
            int16_t move_track[FRAMES_PER_BLOCK * 2];
            int have_move_track = 0;
            if (link_audio.enabled && link_audio_routing_enabled &&
                shadow_chain_set_inject_audio &&
                s < link_audio.move_channel_count) {
                have_move_track = link_audio_read_channel(s, move_track, FRAMES_PER_BLOCK);
                if (have_move_track) {
                    shadow_chain_set_inject_audio(
                        shadow_chain_slots[s].instance,
                        move_track, FRAMES_PER_BLOCK);
                }
            }

            int16_t render_buffer[FRAMES_PER_BLOCK * 2];
            memset(render_buffer, 0, sizeof(render_buffer));
            shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                           render_buffer,
                                           MOVE_FRAMES_PER_BLOCK);
            /* Capture per-slot audio for Link Audio publisher (with slot volume) */
            if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS) {
                float cap_vol = shadow_effective_volume(s);
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    shadow_slot_capture[s][i] = (int16_t)lroundf((float)render_buffer[i] * cap_vol);
                }
                /* Write to publisher shared memory for link_subscriber */
                if (shadow_pub_audio_shm) {
                    link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                    uint32_t wp = ps->write_pos;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                        ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] = shadow_slot_capture[s][i];
                        wp++;
                    }
                    __sync_synchronize();
                    ps->write_pos = wp;
                    ps->active = 1;
                }
            }

            /* Accumulate raw Move audio for subtraction from mailbox */
            if (have_move_track) {
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                    move_injected[i] += (int32_t)move_track[i];
                any_injected = 1;
            }

            float vol = shadow_effective_volume(s);
            float gain = vol * me_input_scale;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                mix[i] += (int32_t)lroundf((float)render_buffer[i] * gain);
                me_full[i] += (int32_t)lroundf((float)render_buffer[i] * vol);
            }
        }
    }

    /* Subtract Move track audio from mix to avoid doubling.
     * Link Audio per-track streams are pre-fader; mailbox is post-fader.
     * When MFX active: mailbox was prescaled to unity, subtract at unity.
     * Otherwise: scale subtraction by Move's volume so the levels match. */
    if (any_injected) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
            mix[i] -= (int32_t)lroundf((float)move_injected[i] * link_sub_scale);
    }

    /* Save ME full-gain component for bridge split */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (me_full[i] > 32767) me_full[i] = 32767;
        if (me_full[i] < -32768) me_full[i] = -32768;
        native_bridge_me_component[i] = (int16_t)me_full[i];
    }
    native_bridge_capture_mv = mv;
    native_bridge_split_valid = 1;

    /* Clamp and write to output buffer */
    int16_t output_buffer[FRAMES_PER_BLOCK * 2];
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (mix[i] > 32767) mix[i] = 32767;
        if (mix[i] < -32768) mix[i] = -32768;
        output_buffer[i] = (int16_t)mix[i];
    }

    /* Apply master FX chain - process through all 4 slots in series */
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            s->api->process_block(s->instance, output_buffer, FRAMES_PER_BLOCK);
        }
    }

    /* Capture native bridge source AFTER master FX, BEFORE master volume.
     * This bakes master FX into native bridge resampling while keeping
     * capture independent of master-volume attenuation. */
    native_capture_total_mix_snapshot_from_buffer(output_buffer);

    /* Apply master volume AFTER MFX.  When MFX is active the mix was built
     * at unity level; scale down now so the DAC output respects master volume.
     * When MFX is off the mix is already at mv level — no extra scaling. */
    if (mfx_active && mv < 0.9999f) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            output_buffer[i] = (int16_t)lroundf((float)output_buffer[i] * mv);
        }
    }

    /* Write final output to mailbox */
    memcpy(mailbox_audio, output_buffer, sizeof(output_buffer));
}

/* === OVERTAKE DSP LOAD/UNLOAD ===
 * Overtake modules can optionally include a dsp.so that runs in the shim's
 * audio thread.  V2-only: supports both generator (plugin_api_v2_t, outputs
 * audio) and effect (audio_fx_api_v2_t, processes combined audio in-place).
 */

/* MIDI send callback for overtake DSP → chain slots */
static int overtake_midi_send_internal(const uint8_t *msg, int len) {
    if (!msg || len < 4) return 0;
    /* Build USB-MIDI packet: [CIN, status, d1, d2] */
    uint8_t cin = (msg[1] >> 4) & 0x0F;
    uint8_t pkt[4] = { cin, msg[1], msg[2], msg[3] };
    static int midi_log_count = 0;
    int log_on = shadow_midi_out_log_enabled();
    shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count);
    return len;
}

/* MIDI send callback for overtake DSP → external USB MIDI */
static int overtake_midi_send_external(const uint8_t *msg, int len) {
    if (!msg || len < 4) return 0;
    uint8_t *midi_out = shadow_mailbox + MIDI_OUT_OFFSET;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        if (midi_out[i] == 0 && midi_out[i+1] == 0 &&
            midi_out[i+2] == 0 && midi_out[i+3] == 0) {
            memcpy(&midi_out[i], msg, 4);
            return len;
        }
    }
    return 0;  /* Buffer full */
}

static void shadow_overtake_dsp_load(const char *path) {
    /* Unload previous if any */
    if (overtake_dsp_handle) {
        shadow_log("Overtake DSP: unloading previous before loading new");
        if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->destroy_instance)
            overtake_dsp_gen->destroy_instance(overtake_dsp_gen_inst);
        if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->destroy_instance)
            overtake_dsp_fx->destroy_instance(overtake_dsp_fx_inst);
        dlclose(overtake_dsp_handle);
        overtake_dsp_handle = NULL;
        overtake_dsp_gen = NULL;
        overtake_dsp_gen_inst = NULL;
        overtake_dsp_fx = NULL;
        overtake_dsp_fx_inst = NULL;
    }

    if (!path || !path[0]) return;

    overtake_dsp_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!overtake_dsp_handle) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Overtake DSP: failed to load %s: %s", path, dlerror());
        shadow_log(msg);
        return;
    }

    /* Set up host API for the overtake plugin */
    memset(&overtake_host_api, 0, sizeof(overtake_host_api));
    overtake_host_api.api_version = MOVE_PLUGIN_API_VERSION;
    overtake_host_api.sample_rate = MOVE_SAMPLE_RATE;
    overtake_host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    overtake_host_api.mapped_memory = global_mmap_addr;
    overtake_host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    overtake_host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    overtake_host_api.log = shadow_log;
    overtake_host_api.midi_send_internal = overtake_midi_send_internal;
    overtake_host_api.midi_send_external = overtake_midi_send_external;

    /* Extract module directory from dsp path */
    char module_dir[256];
    strncpy(module_dir, path, sizeof(module_dir) - 1);
    module_dir[sizeof(module_dir) - 1] = '\0';
    char *last_slash = strrchr(module_dir, '/');
    if (last_slash) *last_slash = '\0';

    /* Try V2 generator first (e.g. SEQOMD) */
    move_plugin_init_v2_fn init_gen = (move_plugin_init_v2_fn)dlsym(
        overtake_dsp_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (init_gen) {
        overtake_dsp_gen = init_gen(&overtake_host_api);
        if (overtake_dsp_gen && overtake_dsp_gen->create_instance) {
            /* Read defaults from module.json if available */
            char json_path[512];
            snprintf(json_path, sizeof(json_path), "%s/module.json", module_dir);
            char *defaults = NULL;
            FILE *f = fopen(json_path, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 16384) {
                    defaults = malloc(sz + 1);
                    if (defaults) {
                        size_t nr = fread(defaults, 1, sz, f);
                        defaults[nr] = '\0';
                        /* Extract just the "defaults" value */
                        const char *dp = strstr(defaults, "\"defaults\"");
                        if (!dp) { free(defaults); defaults = NULL; }
                    }
                }
                fclose(f);
            }

            overtake_dsp_gen_inst = overtake_dsp_gen->create_instance(
                module_dir, defaults);
            if (defaults) free(defaults);

            if (overtake_dsp_gen_inst) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Overtake DSP: loaded generator from %s", path);
                shadow_log(msg);
                return;
            }
        }
        overtake_dsp_gen = NULL;
    }

    /* Try audio FX v2 (effect mode) */
    audio_fx_init_v2_fn init_fx = (audio_fx_init_v2_fn)dlsym(
        overtake_dsp_handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (init_fx) {
        overtake_dsp_fx = init_fx(&overtake_host_api);
        if (overtake_dsp_fx && overtake_dsp_fx->create_instance) {
            overtake_dsp_fx_inst = overtake_dsp_fx->create_instance(module_dir, NULL);
            if (overtake_dsp_fx_inst) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Overtake DSP: loaded FX from %s", path);
                shadow_log(msg);
                return;
            }
        }
        overtake_dsp_fx = NULL;
    }

    /* Neither worked */
    char msg[512];
    snprintf(msg, sizeof(msg), "Overtake DSP: no V2 generator or FX entry point in %s", path);
    shadow_log(msg);
    dlclose(overtake_dsp_handle);
    overtake_dsp_handle = NULL;
}

static void shadow_overtake_dsp_unload(void) {
    if (!overtake_dsp_handle) return;

    if (overtake_dsp_gen && overtake_dsp_gen_inst) {
        if (overtake_dsp_gen->destroy_instance)
            overtake_dsp_gen->destroy_instance(overtake_dsp_gen_inst);
        shadow_log("Overtake DSP: generator unloaded");
    }
    if (overtake_dsp_fx && overtake_dsp_fx_inst) {
        if (overtake_dsp_fx->destroy_instance)
            overtake_dsp_fx->destroy_instance(overtake_dsp_fx_inst);
        shadow_log("Overtake DSP: FX unloaded");
    }

    dlclose(overtake_dsp_handle);
    overtake_dsp_handle = NULL;
    overtake_dsp_gen = NULL;
    overtake_dsp_gen_inst = NULL;
    overtake_dsp_fx = NULL;
    overtake_dsp_fx_inst = NULL;
}

/* === DEFERRED DSP RENDERING ===
 * Render DSP into buffer (slow, ~300µs) - called POST-ioctl
 * This renders audio for the NEXT frame, adding one frame of latency (~3ms)
 * but allowing Move to process pad events faster after ioctl returns.
 */
static void shadow_inprocess_render_to_buffer(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    /* Clear the deferred buffer (used for overtake DSP) */
    memset(shadow_deferred_dsp_buffer, 0, sizeof(shadow_deferred_dsp_buffer));

    /* Clear per-slot deferred buffers */
    for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
        memset(shadow_slot_deferred[s], 0, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        shadow_slot_deferred_valid[s] = 0;
    }

    /* Same-frame FX: render synth only into per-slot buffers.
     * FX + Link Audio inject are processed in mix_from_buffer (same frame as mailbox)
     * so the inject/subtract cancellation is sample-accurate. */
    int same_frame_fx = (shadow_chain_set_external_fx_mode != NULL &&
                         shadow_chain_process_fx != NULL);

    if (shadow_plugin_v2 && shadow_plugin_v2->render_block) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;

            /* Idle gate: skip render_block if synth output has been silent.
             * Buffer is already zeroed, so FX chain in mix_from_buffer still runs
             * on zeros to let reverb/delay tails decay naturally.
             * Probe every ~0.5s to detect self-generating audio (LFOs, arps). */
            if (shadow_slot_idle[s]) {
                shadow_slot_silence_frames[s]++;
                if (shadow_slot_silence_frames[s] % 172 != 0) {
                    /* Not a probe frame — skip render, mark valid so FX still runs */
                    shadow_slot_deferred_valid[s] = 1;
                    continue;
                }
                /* Probe frame: fall through to render and check output */
            }

            if (same_frame_fx) {
                /* New path: synth only → per-slot buffer. FX in mix_from_buffer. */
                shadow_chain_set_external_fx_mode(shadow_chain_slots[s].instance, 1);
                shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                               shadow_slot_deferred[s],
                                               MOVE_FRAMES_PER_BLOCK);
                shadow_slot_deferred_valid[s] = 1;
            } else {
                /* Fallback: full render (synth + FX) → accumulated buffer.
                 * No Link Audio inject (one-frame delay would cause issues). */
                int16_t render_buffer[FRAMES_PER_BLOCK * 2];
                memset(render_buffer, 0, sizeof(render_buffer));
                shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                               render_buffer, MOVE_FRAMES_PER_BLOCK);
                if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS) {
                    float cap_vol = shadow_effective_volume(s);
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                        shadow_slot_capture[s][i] = (int16_t)lroundf((float)render_buffer[i] * cap_vol);
                    /* Write to publisher shared memory for link_subscriber */
                    if (shadow_pub_audio_shm) {
                        link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                        uint32_t wp = ps->write_pos;
                        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                            ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] = shadow_slot_capture[s][i];
                            wp++;
                        }
                        __sync_synchronize();
                        ps->write_pos = wp;
                        ps->active = 1;
                    }
                }
                float vol = shadow_effective_volume(s);
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)(render_buffer[i] * vol);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    shadow_deferred_dsp_buffer[i] = (int16_t)mixed;
                }
            }

            /* Check if synth render output is silent */
            int16_t *slot_out = same_frame_fx ? shadow_slot_deferred[s] : shadow_deferred_dsp_buffer;
            int is_silent = 1;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                if (slot_out[i] > DSP_SILENCE_LEVEL || slot_out[i] < -DSP_SILENCE_LEVEL) {
                    is_silent = 0;
                    break;
                }
            }

            if (is_silent) {
                shadow_slot_silence_frames[s]++;
                if (shadow_slot_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                    shadow_slot_idle[s] = 1;
                }
            } else {
                shadow_slot_silence_frames[s] = 0;
                shadow_slot_idle[s] = 0;
            }
        }
    }

    /* Overtake DSP generator: mix its output into the deferred buffer */
    if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->render_block) {
        int16_t render_buffer[FRAMES_PER_BLOCK * 2];
        memset(render_buffer, 0, sizeof(render_buffer));
        overtake_dsp_gen->render_block(overtake_dsp_gen_inst, render_buffer, MOVE_FRAMES_PER_BLOCK);
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)render_buffer[i];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            shadow_deferred_dsp_buffer[i] = (int16_t)mixed;
        }
    }

    /* Note: Master FX is applied in mix_from_buffer() AFTER mixing with Move's audio */

    shadow_deferred_dsp_valid = 1;
}

/* Mix from pre-rendered buffer - called PRE-ioctl
 * When Link Audio is active: zeroes the mailbox and rebuilds from per-track
 * Link Audio data, routing each track through its slot's FX chain.
 * Tracks without active FX pass through at Move's volume level.
 * This eliminates dry signal leakage entirely (no subtraction needed).
 */
static void shadow_inprocess_mix_from_buffer(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;
    if (!shadow_deferred_dsp_valid) return;  /* No buffer to mix yet */

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    float mv = shadow_master_volume;
    (void)shadow_master_fx_chain_active();  /* MFX slots processed unconditionally below */
    /* Always build the mix at unity level so sampler/skipback capture audio
     * at full gain (independent of master volume).  Apply mv at the end. */

    /* Save Move's audio for bridge split (before zeroing) */
    memcpy(native_bridge_move_component, mailbox_audio, AUDIO_BUFFER_SIZE);

    /* Accumulate ME output across slots for bridge split component */
    int32_t me_full[FRAMES_PER_BLOCK * 2];
    memset(me_full, 0, sizeof(me_full));

    /* Zero-and-rebuild approach: if Link Audio provides per-track data,
     * zero the mailbox and rebuild from Link Audio, applying FX per-slot.
     * This completely eliminates dry signal leakage — no subtraction needed.
     *
     * IMPORTANT: Only rebuild when audio data is actually flowing.
     * Session announcements set move_channel_count but don't mean audio
     * is streaming.  Without a subscriber triggering ChannelRequests,
     * the ring buffers are empty and zeroing the mailbox kills all audio. */
    uint32_t la_cur = link_audio.packets_intercepted;
    if (la_cur > la_prev_intercepted) {
        la_stale_frames = 0;
        la_prev_intercepted = la_cur;
    } else if (la_cur > 0) {
        la_stale_frames++;
    }
    /* Consider Link Audio active if packets arrived within the last ~290ms */
    int la_receiving = (la_cur > 0 && la_stale_frames < 100);

    int rebuild_from_la = (link_audio.enabled && link_audio_routing_enabled &&
                           shadow_chain_process_fx &&
                           link_audio.move_channel_count >= 4 &&
                           la_receiving);

    /* When NOT rebuilding from Link Audio, the mailbox has Move's audio at
     * mv level.  Prescale to unity so the mix (and capture) is at full gain. */
    if (!rebuild_from_la && mv > 0.001f && mv < 0.9999f) {
        float inv = 1.0f / mv;
        if (inv > 20.0f) inv = 20.0f;
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            float scaled = (float)mailbox_audio[i] * inv;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            mailbox_audio[i] = (int16_t)lroundf(scaled);
        }
    }

    if (rebuild_from_la) {
        /* Zero the mailbox — all audio reconstructed from Link Audio */
        memset(mailbox_audio, 0, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));

        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            /* Read Link Audio for this track (pre-fader, same frame as mailbox) */
            int16_t move_track[FRAMES_PER_BLOCK * 2];
            int have_move_track = 0;
            if (s < link_audio.move_channel_count) {
                have_move_track = link_audio_read_channel(s, move_track, FRAMES_PER_BLOCK);
            }

            int slot_active = (shadow_chain_slots[s].active &&
                               shadow_chain_slots[s].instance &&
                               shadow_slot_deferred_valid[s]);

            if (slot_active) {
                /* Phase 2 idle gate: skip FX when synth AND FX output are silent
                 * AND no Link Audio track data is flowing for this slot */
                if (shadow_slot_fx_idle[s] && shadow_slot_idle[s] && !have_move_track) continue;

                /* Active slot: combine synth + Link Audio, run through FX */
                int16_t fx_buf[FRAMES_PER_BLOCK * 2];
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t combined = (int32_t)shadow_slot_deferred[s][i];
                    if (have_move_track)
                        combined += (int32_t)move_track[i];
                    if (combined > 32767) combined = 32767;
                    if (combined < -32768) combined = -32768;
                    fx_buf[i] = (int16_t)combined;
                }

                /* Run FX chain */
                shadow_chain_process_fx(shadow_chain_slots[s].instance,
                                        fx_buf, MOVE_FRAMES_PER_BLOCK);

                /* Track FX output silence for phase 2 idle */
                int fx_silent = 1;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    if (fx_buf[i] > DSP_SILENCE_LEVEL || fx_buf[i] < -DSP_SILENCE_LEVEL) {
                        fx_silent = 0;
                        break;
                    }
                }
                if (fx_silent) {
                    shadow_slot_fx_silence_frames[s]++;
                    if (shadow_slot_fx_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                        shadow_slot_fx_idle[s] = 1;
                    }
                } else {
                    shadow_slot_fx_silence_frames[s] = 0;
                    shadow_slot_fx_idle[s] = 0;
                }

                /* Capture for Link Audio publisher */
                if (s < LINK_AUDIO_SHADOW_CHANNELS) {
                    float cap_vol = shadow_effective_volume(s);
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                        shadow_slot_capture[s][i] = (int16_t)lroundf((float)fx_buf[i] * cap_vol);
                    /* Write to publisher shared memory for link_subscriber */
                    if (shadow_pub_audio_shm) {
                        link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                        uint32_t wp = ps->write_pos;
                        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                            ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] = shadow_slot_capture[s][i];
                            wp++;
                        }
                        __sync_synchronize();
                        ps->write_pos = wp;
                    }
                }

                /* Add FX output to mailbox */
                float vol = shadow_effective_volume(s);
                float gain = vol;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    mailbox_audio[i] = (int16_t)mixed;
                    me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);
                }
            } else if (have_move_track) {
                /* Inactive slot: pass Link Audio through at unity level.
                 * Master volume is applied after capture at the end. */
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)move_track[i];
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    mailbox_audio[i] = (int16_t)mixed;
                }
                /* Publish Move track audio to ME channel even without a synth loaded */
                if (s < LINK_AUDIO_SHADOW_CHANNELS && shadow_pub_audio_shm) {
                    link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                    uint32_t wp = ps->write_pos;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                        ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] = move_track[i];
                        wp++;
                    }
                    __sync_synchronize();
                    ps->write_pos = wp;
                }
            }
        }
    } else if (shadow_chain_process_fx) {
        /* Fallback: no Link Audio — just process deferred synth through FX */
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_slot_deferred_valid[s] || !shadow_chain_slots[s].instance) continue;

            /* Phase 2 idle gate: skip FX when both synth AND FX output are silent */
            if (shadow_slot_fx_idle[s] && shadow_slot_idle[s]) continue;

            int16_t fx_buf[FRAMES_PER_BLOCK * 2];
            memcpy(fx_buf, shadow_slot_deferred[s], sizeof(fx_buf));
            shadow_chain_process_fx(shadow_chain_slots[s].instance,
                                    fx_buf, MOVE_FRAMES_PER_BLOCK);

            /* Write to publisher shared memory for link_subscriber */
            if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS && shadow_pub_audio_shm) {
                float cap_vol = shadow_effective_volume(s);
                link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                uint32_t wp = ps->write_pos;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] =
                        (int16_t)lroundf((float)fx_buf[i] * cap_vol);
                    wp++;
                }
                __sync_synchronize();
                ps->write_pos = wp;
            }

            /* Track FX output silence for phase 2 idle */
            int fx_silent = 1;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                if (fx_buf[i] > DSP_SILENCE_LEVEL || fx_buf[i] < -DSP_SILENCE_LEVEL) {
                    fx_silent = 0;
                    break;
                }
            }
            if (fx_silent) {
                shadow_slot_fx_silence_frames[s]++;
                if (shadow_slot_fx_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                    shadow_slot_fx_idle[s] = 1;
                }
            } else {
                shadow_slot_fx_silence_frames[s] = 0;
                shadow_slot_fx_idle[s] = 0;
            }

            float vol = shadow_effective_volume(s);
            float gain = vol;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                mailbox_audio[i] = (int16_t)mixed;
                me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);
            }
        }
    }

    /* Mix overtake DSP buffer (at unity — master volume applied after capture) */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)shadow_deferred_dsp_buffer[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
        me_full[i] += (int32_t)shadow_deferred_dsp_buffer[i];
    }

    /* Save ME full-gain component for bridge split */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (me_full[i] > 32767) me_full[i] = 32767;
        if (me_full[i] < -32768) me_full[i] = -32768;
        native_bridge_me_component[i] = (int16_t)me_full[i];
    }
    native_bridge_capture_mv = mv;
    native_bridge_split_valid = 1;

    /* Write master mix to publisher shm (slot index LINK_AUDIO_PUB_MASTER_IDX) */
    if (link_audio.enabled && shadow_pub_audio_shm) {
        link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[LINK_AUDIO_PUB_MASTER_IDX];
        uint32_t wp = ps->write_pos;
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] = native_bridge_me_component[i];
            wp++;
        }
        __sync_synchronize();
        ps->write_pos = wp;
    }

    /* Overtake DSP FX: process combined Move+shadow audio in-place */
    if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->process_block) {
        overtake_dsp_fx->process_block(overtake_dsp_fx_inst, mailbox_audio, FRAMES_PER_BLOCK);
    }

    /* Apply master FX chain to combined audio - process through all 4 slots in series */
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            s->api->process_block(s->instance, mailbox_audio, FRAMES_PER_BLOCK);
        }
    }

    /* Capture native bridge source AFTER master FX, BEFORE master volume.
     * This bakes master FX into native bridge resampling while keeping
     * capture independent of master-volume attenuation. */
    native_capture_total_mix_snapshot_from_buffer(mailbox_audio);

    /* Capture audio for sampler BEFORE master volume scaling (Resample source only) */
    if (sampler_source == SAMPLER_SOURCE_RESAMPLE) {
        sampler_capture_audio();
        sampler_tick_preroll();
        /* Skipback: always capture Resample source into rolling buffer */
        skipback_init();
        skipback_capture(mailbox_audio);
    }

    /* Apply master volume after capture.  The mix is always built at unity
     * level so that sampler/skipback capture full-gain audio.  Scale down
     * now so the DAC output respects master volume. */
    if (mv < 0.9999f) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            float scaled = (float)mailbox_audio[i] * mv;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            mailbox_audio[i] = (int16_t)lroundf(scaled);
        }
    }
}

/* Shared memory segment names from shadow_constants.h */

#define NUM_AUDIO_BUFFERS 3  /* Triple buffering */

/* Shadow shared memory pointers */
static int16_t *shadow_audio_shm = NULL;    /* Shadow's mixed output */
static int16_t *shadow_movein_shm = NULL;   /* Move's audio for shadow to read */
static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static uint8_t *display_live_shm = NULL;
static shadow_midi_out_t *shadow_midi_out_shm = NULL;  /* MIDI output from shadow UI */
static uint8_t last_shadow_midi_out_ready = 0;
static shadow_midi_dsp_t *shadow_midi_dsp_shm = NULL;  /* MIDI to DSP from shadow UI */
static uint8_t last_shadow_midi_dsp_ready = 0;

static uint32_t last_screenreader_sequence = 0;  /* Track last spoken message */
static uint64_t last_speech_time_ms = 0;  /* Rate limiting for TTS */

/* LED queue constants and state — moved to shadow_led_queue.c */

/* Shadow shared memory file descriptors */
static int shm_audio_fd = -1;
static int shm_movein_fd = -1;
static int shm_midi_fd = -1;
static int shm_ui_midi_fd = -1;
static int shm_display_fd = -1;
static int shm_control_fd = -1;
static int shm_ui_fd = -1;
static int shm_param_fd = -1;
static int shm_midi_out_fd = -1;
static int shm_midi_dsp_fd = -1;
static int shm_screenreader_fd = -1;
static int shm_pub_audio_fd = -1;
static int shm_overlay_fd = -1;

/* Shadow initialization state */
static int shadow_shm_initialized = 0;

/* Initialize shadow shared memory segments */

/* Signal handler for crash diagnostics - async-signal-safe */
static void crash_signal_handler(int sig)
{
    const char *name;
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGBUS:  name = "SIGBUS";  break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGTERM: name = "SIGTERM"; break;
        case SIGINT:  name = "SIGINT";  break;
        default:      name = "UNKNOWN"; break;
    }
    /* Build message: "Caught <signal> - terminating (pid=<pid>)" */
    char msg[128];
    int pos = 0;
    const char prefix[] = "Caught ";
    for (int i = 0; prefix[i]; i++) msg[pos++] = prefix[i];
    for (int i = 0; name[i]; i++) msg[pos++] = name[i];
    const char suffix[] = " - terminating";
    for (int i = 0; suffix[i]; i++) msg[pos++] = suffix[i];
    msg[pos] = '\0';

    unified_log_crash(msg);
    _exit(128 + sig);
}

static void init_shadow_shm(void)
{
    if (shadow_shm_initialized) return;

    /* Initialize unified logging first so we can log during shm init */
    unified_log_init();

    /* Install crash signal handlers */
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGBUS,  crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGTERM, crash_signal_handler);

    /* Log startup identity (always-on, no flag needed) */
    {
        char init_msg[64];
        snprintf(init_msg, sizeof(init_msg), "Shim init: pid=%d ppid=%d", getpid(), getppid());
        unified_log_crash(init_msg);
    }

    printf("Shadow: Initializing shared memory...\n");

    /* Create/open audio shared memory - triple buffered */
    size_t triple_audio_size = AUDIO_BUFFER_SIZE * NUM_AUDIO_BUFFERS;
    shm_audio_fd = shm_open(SHM_SHADOW_AUDIO, O_CREAT | O_RDWR, 0666);
    if (shm_audio_fd >= 0) {
        ftruncate(shm_audio_fd, triple_audio_size);
        shadow_audio_shm = (int16_t *)mmap(NULL, triple_audio_size,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, shm_audio_fd, 0);
        if (shadow_audio_shm == MAP_FAILED) {
            shadow_audio_shm = NULL;
            printf("Shadow: Failed to mmap audio shm\n");
        } else {
            memset(shadow_audio_shm, 0, triple_audio_size);
        }
    } else {
        printf("Shadow: Failed to create audio shm\n");
    }

    /* Create/open Move audio input shared memory (for shadow to read Move's audio) */
    shm_movein_fd = shm_open(SHM_SHADOW_MOVEIN, O_CREAT | O_RDWR, 0666);
    if (shm_movein_fd >= 0) {
        ftruncate(shm_movein_fd, AUDIO_BUFFER_SIZE);
        shadow_movein_shm = (int16_t *)mmap(NULL, AUDIO_BUFFER_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, shm_movein_fd, 0);
        if (shadow_movein_shm == MAP_FAILED) {
            shadow_movein_shm = NULL;
            printf("Shadow: Failed to mmap movein shm\n");
        } else {
            memset(shadow_movein_shm, 0, AUDIO_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create movein shm\n");
    }

    /* Create/open MIDI shared memory */
    shm_midi_fd = shm_open(SHM_SHADOW_MIDI, O_CREAT | O_RDWR, 0666);
    if (shm_midi_fd >= 0) {
        ftruncate(shm_midi_fd, MIDI_BUFFER_SIZE);
        shadow_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, shm_midi_fd, 0);
        if (shadow_midi_shm == MAP_FAILED) {
            shadow_midi_shm = NULL;
            printf("Shadow: Failed to mmap MIDI shm\n");
        } else {
            memset(shadow_midi_shm, 0, MIDI_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create MIDI shm\n");
    }

    /* Create/open UI MIDI shared memory */
    shm_ui_midi_fd = shm_open(SHM_SHADOW_UI_MIDI, O_CREAT | O_RDWR, 0666);
    if (shm_ui_midi_fd >= 0) {
        ftruncate(shm_ui_midi_fd, MIDI_BUFFER_SIZE);
        shadow_ui_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, shm_ui_midi_fd, 0);
        if (shadow_ui_midi_shm == MAP_FAILED) {
            shadow_ui_midi_shm = NULL;
            printf("Shadow: Failed to mmap UI MIDI shm\n");
        } else {
            memset(shadow_ui_midi_shm, 0, MIDI_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create UI MIDI shm\n");
    }

    /* Create/open display shared memory */
    shm_display_fd = shm_open(SHM_SHADOW_DISPLAY, O_CREAT | O_RDWR, 0666);
    if (shm_display_fd >= 0) {
        ftruncate(shm_display_fd, DISPLAY_BUFFER_SIZE);
        shadow_display_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, shm_display_fd, 0);
        if (shadow_display_shm == MAP_FAILED) {
            shadow_display_shm = NULL;
            printf("Shadow: Failed to mmap display shm\n");
        } else {
            memset(shadow_display_shm, 0, DISPLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create display shm\n");
    }

    /* Create/open live display shared memory (for remote display server) */
    int shm_display_live_fd = shm_open(SHM_DISPLAY_LIVE, O_CREAT | O_RDWR, 0666);
    if (shm_display_live_fd >= 0) {
        ftruncate(shm_display_live_fd, DISPLAY_BUFFER_SIZE);
        display_live_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, shm_display_live_fd, 0);
        if (display_live_shm == MAP_FAILED) {
            display_live_shm = NULL;
            printf("Shadow: Failed to mmap live display shm\n");
        } else {
            memset(display_live_shm, 0, DISPLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create live display shm\n");
    }

    /* Create/open control shared memory - DON'T zero it, shadow_poc owns the state */
    shm_control_fd = shm_open(SHM_SHADOW_CONTROL, O_CREAT | O_RDWR, 0666);
    if (shm_control_fd >= 0) {
        ftruncate(shm_control_fd, CONTROL_BUFFER_SIZE);
        shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE,
                                                   PROT_READ | PROT_WRITE,
                                                   MAP_SHARED, shm_control_fd, 0);
        if (shadow_control == MAP_FAILED) {
            shadow_control = NULL;
            printf("Shadow: Failed to mmap control shm\n");
        }
        if (shadow_control) {
            /* Avoid sticky shadow state across restarts. */
            shadow_display_mode = 0;
            shadow_control->display_mode = 0;
            shadow_control->should_exit = 0;
            shadow_control->midi_ready = 0;
            shadow_control->write_idx = 0;
            shadow_control->read_idx = 0;
            shadow_control->ui_slot = 0;
            shadow_control->ui_flags = 0;
            shadow_control->ui_patch_index = 0;
            shadow_control->ui_request_id = 0;
            /* Initialize TTS defaults */
            shadow_control->tts_enabled = 0;    /* Screen Reader off by default */
            shadow_control->tts_volume = 70;    /* 70% volume */
            shadow_control->tts_pitch = 110;    /* 110 Hz */
            shadow_control->tts_speed = 1.0f;   /* Normal speed */
            shadow_control->tts_engine = 0;     /* 0=espeak-ng, 1=flite */
            shadow_control->overlay_knobs_mode = OVERLAY_KNOBS_NATIVE; /* Native by default */
            shadow_control->tts_debounce_ms = 300; /* default debounce ms */
        }
    } else {
        printf("Shadow: Failed to create control shm\n");
    }

    /* Create/open UI shared memory (slot labels/state) */
    shm_ui_fd = shm_open(SHM_SHADOW_UI, O_CREAT | O_RDWR, 0666);
    if (shm_ui_fd >= 0) {
        ftruncate(shm_ui_fd, SHADOW_UI_BUFFER_SIZE);
        shadow_ui_state = (shadow_ui_state_t *)mmap(NULL, SHADOW_UI_BUFFER_SIZE,
                                                    PROT_READ | PROT_WRITE,
                                                    MAP_SHARED, shm_ui_fd, 0);
        if (shadow_ui_state == MAP_FAILED) {
            shadow_ui_state = NULL;
            printf("Shadow: Failed to mmap UI shm\n");
        } else {
            memset(shadow_ui_state, 0, SHADOW_UI_BUFFER_SIZE);
            shadow_ui_state->version = 1;
            shadow_ui_state->slot_count = SHADOW_UI_SLOTS;
        }
    } else {
        printf("Shadow: Failed to create UI shm\n");
    }

    /* Create/open param shared memory (for set_param/get_param requests) */
    shm_param_fd = shm_open(SHM_SHADOW_PARAM, O_CREAT | O_RDWR, 0666);
    if (shm_param_fd >= 0) {
        ftruncate(shm_param_fd, SHADOW_PARAM_BUFFER_SIZE);
        shadow_param = (shadow_param_t *)mmap(NULL, SHADOW_PARAM_BUFFER_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, shm_param_fd, 0);
        if (shadow_param == MAP_FAILED) {
            shadow_param = NULL;
            printf("Shadow: Failed to mmap param shm\n");
        } else {
            memset(shadow_param, 0, SHADOW_PARAM_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create param shm\n");
    }

    /* Create/open MIDI out shared memory (for shadow UI to send MIDI) */
    shm_midi_out_fd = shm_open(SHM_SHADOW_MIDI_OUT, O_CREAT | O_RDWR, 0666);
    if (shm_midi_out_fd >= 0) {
        ftruncate(shm_midi_out_fd, sizeof(shadow_midi_out_t));
        shadow_midi_out_shm = (shadow_midi_out_t *)mmap(NULL, sizeof(shadow_midi_out_t),
                                                         PROT_READ | PROT_WRITE,
                                                         MAP_SHARED, shm_midi_out_fd, 0);
        if (shadow_midi_out_shm == MAP_FAILED) {
            shadow_midi_out_shm = NULL;
            printf("Shadow: Failed to mmap midi_out shm\n");
        } else {
            memset(shadow_midi_out_shm, 0, sizeof(shadow_midi_out_t));
        }
    } else {
        printf("Shadow: Failed to create midi_out shm\n");
    }

    /* Create/open MIDI-to-DSP shared memory (for shadow UI to route MIDI to chain slots) */
    shm_midi_dsp_fd = shm_open(SHM_SHADOW_MIDI_DSP, O_CREAT | O_RDWR, 0666);
    if (shm_midi_dsp_fd >= 0) {
        ftruncate(shm_midi_dsp_fd, sizeof(shadow_midi_dsp_t));
        shadow_midi_dsp_shm = (shadow_midi_dsp_t *)mmap(NULL, sizeof(shadow_midi_dsp_t),
                                                         PROT_READ | PROT_WRITE,
                                                         MAP_SHARED, shm_midi_dsp_fd, 0);
        if (shadow_midi_dsp_shm == MAP_FAILED) {
            shadow_midi_dsp_shm = NULL;
            printf("Shadow: Failed to mmap midi_dsp shm\n");
        } else {
            memset(shadow_midi_dsp_shm, 0, sizeof(shadow_midi_dsp_t));
        }
    } else {
        printf("Shadow: Failed to create midi_dsp shm\n");
    }

    /* Create/open screen reader shared memory (for accessibility: TTS and D-Bus announcements) */
    shm_screenreader_fd = shm_open(SHM_SHADOW_SCREENREADER, O_CREAT | O_RDWR, 0666);
    if (shm_screenreader_fd >= 0) {
        ftruncate(shm_screenreader_fd, sizeof(shadow_screenreader_t));
        shadow_screenreader_shm = (shadow_screenreader_t *)mmap(NULL, sizeof(shadow_screenreader_t),
                                                                 PROT_READ | PROT_WRITE,
                                                                 MAP_SHARED, shm_screenreader_fd, 0);
        if (shadow_screenreader_shm == MAP_FAILED) {
            shadow_screenreader_shm = NULL;
            printf("Shadow: Failed to mmap screenreader shm\n");
        } else {
            memset(shadow_screenreader_shm, 0, sizeof(shadow_screenreader_t));
        }
    } else {
        printf("Shadow: Failed to create screenreader shm\n");
    }

    /* Create/open overlay state shared memory (sampler/skipback state for JS rendering) */
    shm_overlay_fd = shm_open(SHM_SHADOW_OVERLAY, O_CREAT | O_RDWR, 0666);
    if (shm_overlay_fd >= 0) {
        ftruncate(shm_overlay_fd, SHADOW_OVERLAY_BUFFER_SIZE);
        shadow_overlay_shm = (shadow_overlay_state_t *)mmap(NULL, SHADOW_OVERLAY_BUFFER_SIZE,
                                                             PROT_READ | PROT_WRITE,
                                                             MAP_SHARED, shm_overlay_fd, 0);
        if (shadow_overlay_shm == MAP_FAILED) {
            shadow_overlay_shm = NULL;
            printf("Shadow: Failed to mmap overlay shm\n");
        } else {
            memset(shadow_overlay_shm, 0, SHADOW_OVERLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create overlay shm\n");
    }

    /* TTS engine uses lazy initialization - will init on first speak */
    tts_set_volume(70);  /* Set volume early (safe, doesn't require TTS init) */
    printf("Shadow: TTS engine configured (will init on first use)\n");

    /* Create/open Link Audio publisher shared memory */
    shm_pub_audio_fd = shm_open(SHM_LINK_AUDIO_PUB, O_CREAT | O_RDWR, 0666);
    if (shm_pub_audio_fd >= 0) {
        ftruncate(shm_pub_audio_fd, sizeof(link_audio_pub_shm_t));
        shadow_pub_audio_shm = (link_audio_pub_shm_t *)mmap(NULL,
            sizeof(link_audio_pub_shm_t),
            PROT_READ | PROT_WRITE, MAP_SHARED, shm_pub_audio_fd, 0);
        if (shadow_pub_audio_shm == MAP_FAILED) {
            shadow_pub_audio_shm = NULL;
            printf("Shadow: Failed to mmap pub audio shm\n");
        } else {
            memset(shadow_pub_audio_shm, 0, sizeof(link_audio_pub_shm_t));
            shadow_pub_audio_shm->magic = LINK_AUDIO_PUB_SHM_MAGIC;
            shadow_pub_audio_shm->version = LINK_AUDIO_PUB_SHM_VERSION;
            printf("Shadow: Link Audio publisher shm initialized (%zu bytes)\n",
                   sizeof(link_audio_pub_shm_t));
        }
    } else {
        printf("Shadow: Failed to create pub audio shm\n");
    }

    /* Initialize Link Audio state */
    memset(&link_audio, 0, sizeof(link_audio));
    link_audio.move_socket_fd = -1;
    link_audio.publisher_socket_fd = -1;
    memset(shadow_slot_capture, 0, sizeof(shadow_slot_capture));

    shadow_shm_initialized = 1;
    printf("Shadow: Shared memory initialized (audio=%p, midi=%p, ui_midi=%p, display=%p, control=%p, ui=%p, param=%p, midi_out=%p, midi_dsp=%p, screenreader=%p, overlay=%p, pub_audio=%p)\n",
           shadow_audio_shm, shadow_midi_shm, shadow_ui_midi_shm,
           shadow_display_shm, shadow_control, shadow_ui_state, shadow_param, shadow_midi_out_shm, shadow_midi_dsp_shm, shadow_screenreader_shm, shadow_overlay_shm, shadow_pub_audio_shm);
}

/* Monitor screen reader messages and speak them with TTS (debounced) */
#define TTS_DEBOUNCE_MS_DEFAULT 300  /* Default debounce: 300ms of silence before speaking */
static char pending_tts_message[SHADOW_SCREENREADER_TEXT_LEN] = {0};
static uint64_t last_message_time_ms = 0;
static bool has_pending_message = false;

static void shadow_check_screenreader(void)
{
    if (!shadow_screenreader_shm) return;

    /* Get current time in milliseconds */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

    /* Check if there's a new message (sequence incremented) */
    uint32_t current_sequence = shadow_screenreader_shm->sequence;
    if (current_sequence != last_screenreader_sequence) {
        /* New message arrived - buffer it and reset debounce timer */
        if (shadow_screenreader_shm->text[0] != '\0') {
            strncpy(pending_tts_message, shadow_screenreader_shm->text, sizeof(pending_tts_message) - 1);
            pending_tts_message[sizeof(pending_tts_message) - 1] = '\0';
            last_message_time_ms = now_ms;
            has_pending_message = true;
        }
        last_screenreader_sequence = current_sequence;
        return;
    }

    /* Check if debounce period has elapsed and we have a pending message */
    uint16_t debounce_ms = shadow_control ? shadow_control->tts_debounce_ms : TTS_DEBOUNCE_MS_DEFAULT;
    if (has_pending_message && (now_ms - last_message_time_ms >= debounce_ms)) {
        /* Apply TTS settings from shared memory before speaking */
        if (shadow_control) {
            /* Check for engine switch (must happen before other settings) */
            const char *current_engine = tts_get_engine();
            const char *requested_engine = shadow_control->tts_engine == 1 ? "flite" : "espeak";
            if (strcmp(current_engine, requested_engine) != 0) {
                tts_set_engine(requested_engine);
            }

            tts_set_enabled(shadow_control->tts_enabled != 0);
            tts_set_volume(shadow_control->tts_volume);
            tts_set_speed(shadow_control->tts_speed);
            tts_set_pitch((float)shadow_control->tts_pitch);
        }

        /* Speak the buffered message */
        if (tts_speak(pending_tts_message)) {
            last_speech_time_ms = now_ms;
        }
        has_pending_message = false;
        pending_tts_message[0] = '\0';
    }
}

/* ==========================================================================
 * PIN Challenge Display Scanner
 *
 * Monitors the pin_challenge_active flag set by the web shim when a browser
 * connects to move.local and triggers a PIN challenge. When detected, we
 * wait for the PIN to render on the display, extract the 6 digits, and
 * speak them via TTS.
 *
 * Display format: 128x64 @ 1bpp, column-major (8 pages of 128 bytes).
 * PIN digits appear on pages 3-4 only, all other pages are blank.
 * ========================================================================== */

/* PIN scanner state — moved to shadow_pin_scanner.c */

/* Shift+Menu double-click detection state */
static uint64_t shift_menu_pending_ms = 0;
static int shift_menu_pending = 0;

/* PIN scanner functions — moved to shadow_pin_scanner.c */

/* Mix shadow audio into mailbox audio buffer - TRIPLE BUFFERED */
static void shadow_mix_audio(void)
{
    if (!shadow_audio_shm || !global_mmap_addr) return;
    if (!shadow_control || !shadow_control->shadow_ready) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);

    /* Check for new screen reader messages and speak them */
    shadow_check_screenreader();

    /* TTS test: speak once after 3 seconds to verify audio works */
    static int tts_test_frame_count = 0;
    static bool tts_test_done = false;
    if (!tts_test_done && shadow_control->shadow_ready) {
        tts_test_frame_count++;
        if (tts_test_frame_count == 1035) {  /* ~3 seconds at 44.1kHz, 128 frames/block */
            printf("TTS test: Speaking test phrase...\n");
            /* Apply TTS settings before test phrase */
            {
                const char *current_engine = tts_get_engine();
                const char *requested_engine = shadow_control->tts_engine == 1 ? "flite" : "espeak";
                if (strcmp(current_engine, requested_engine) != 0) {
                    tts_set_engine(requested_engine);
                }
            }
            tts_set_enabled(shadow_control->tts_enabled != 0);
            tts_set_volume(shadow_control->tts_volume);
            tts_set_speed(shadow_control->tts_speed);
            tts_set_pitch((float)shadow_control->tts_pitch);
            tts_speak("Text to speech is working");
            tts_test_done = true;
        }
    }

    /* Increment shim counter for shadow's drift correction */
    shadow_control->shim_counter++;

    /* Copy Move's audio to shared memory so shadow can mix it */
    if (shadow_movein_shm) {
        memcpy(shadow_movein_shm, mailbox_audio, AUDIO_BUFFER_SIZE);
    }

    /*
     * Triple buffering read strategy:
     * - Read from buffer that's 2 behind write (gives shadow time to render)
     * - This adds ~6ms latency but smooths out timing jitter
     */
    uint8_t write_idx = shadow_control->write_idx;
    uint8_t read_idx = (write_idx + NUM_AUDIO_BUFFERS - 2) % NUM_AUDIO_BUFFERS;

    /* Update read index for shadow's reference */
    shadow_control->read_idx = read_idx;

    /* Get pointer to the buffer we should read */
    int16_t *src_buffer = shadow_audio_shm + (read_idx * FRAMES_PER_BLOCK * 2);

    /* 0 = mix shadow with Move, 1 = replace Move audio entirely */
    #define SHADOW_AUDIO_REPLACE 0

    #if SHADOW_AUDIO_REPLACE
    /* Replace Move's audio entirely with shadow audio */
    memcpy(mailbox_audio, src_buffer, AUDIO_BUFFER_SIZE);
    #else
    /* Mix shadow audio with Move's audio */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)src_buffer[i];
        /* Clip to int16 range */
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }
    #endif

    /* NOTE: TTS mixing moved to shadow_mix_tts() which runs AFTER
     * shadow_inprocess_mix_from_buffer(). That function zeros the mailbox
     * when Link Audio is active, so TTS must be mixed in afterward. */
}

/* Mix TTS audio into mailbox.  Called AFTER shadow_inprocess_mix_from_buffer()
 * because that function may zero-and-rebuild the mailbox when Link Audio is
 * active.  Mixing TTS here ensures it is never wiped by the rebuild. */
static void shadow_mix_tts(void)
{
    if (!global_mmap_addr) return;
    if (!tts_is_speaking()) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    static int16_t tts_buffer[FRAMES_PER_BLOCK * 2];  /* Stereo interleaved */
    int frames_read = tts_get_audio(tts_buffer, FRAMES_PER_BLOCK);

    if (frames_read > 0) {
        float mv = shadow_master_volume;
        for (int i = 0; i < frames_read * 2; i++) {
            int32_t scaled_tts = (int32_t)lroundf((float)tts_buffer[i] * mv);
            int32_t mixed = (int32_t)mailbox_audio[i] + scaled_tts;
            /* Clip to int16 range */
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            mailbox_audio[i] = (int16_t)mixed;
        }
    }
}

/* LED queue functions — moved to shadow_led_queue.c */


/* Check for and send screen reader announcements via D-Bus */
static void shadow_check_screenreader_announcements(void) {
    static uint32_t last_announcement_sequence = 0;

    if (!shadow_screenreader_shm) return;

    /* Check if there's a new message (sequence incremented) */
    uint32_t current_sequence = shadow_screenreader_shm->sequence;
    if (current_sequence == last_announcement_sequence) return;

    last_announcement_sequence = current_sequence;

    /* Queue announcement for D-Bus broadcast */
    if (shadow_screenreader_shm->text[0]) {
        send_screenreader_announcement(shadow_screenreader_shm->text);
        /* Inject immediately - don't wait for Move's next D-Bus activity */
        shadow_inject_pending_announcements();
    }
}




/* Swap display buffer if in shadow mode */
static void shadow_swap_display(void)
{
    static uint32_t ui_check_counter = 0;
    static int display_phase = 0;  /* 0-6: phases of display push */
    static int display_hidden_for_volume = 0;

    if (!shadow_display_shm || !global_mmap_addr) {
        return;
    }
    if (!shadow_control || !shadow_control->shadow_ready) {
        return;
    }
    if (!shadow_display_mode) {
        display_phase = 0;
        display_hidden_for_volume = 0;
        shadow_block_plain_volume_hide_until_release = 0;
        return;  /* Not in shadow mode */
    }
    if (!shadow_volume_knob_touched) {
        shadow_block_plain_volume_hide_until_release = 0;
    }
    if (shadow_volume_knob_touched && !shadow_shift_held) {
        if (shadow_block_plain_volume_hide_until_release) {
            /* Keep shadow UI visible until shortcut's volume touch is fully released. */
            if (display_hidden_for_volume) {
                display_phase = 0;
                display_hidden_for_volume = 0;
            }
        } else if (!shadow_control->overtake_mode) {
            /* Let native Move overlays remain visible while volume touch is held. */
            display_phase = 0;
            display_hidden_for_volume = 1;
            return;
        }
    } else if (display_hidden_for_volume) {
        /* Restart shadow slicing cleanly after releasing volume touch. */
        display_phase = 0;
        display_hidden_for_volume = 0;
    }
    if ((ui_check_counter++ % 256) == 0) {
        launch_shadow_ui();
    }

    /* Composite overlays onto shadow display if active */
    static uint8_t shadow_composited[DISPLAY_BUFFER_SIZE];
    const uint8_t *display_src = shadow_display_shm;

    if (skipback_overlay_timeout > 0) {
        skipback_overlay_timeout--;
        shadow_overlay_sync();
    }

    /* Write full display to DISPLAY_OFFSET (768) */
    memcpy(global_mmap_addr + DISPLAY_OFFSET, display_src, DISPLAY_BUFFER_SIZE);

    /* Write display using slice protocol - one slice per ioctl */
    /* No rate limiting because we must overwrite Move every ioctl */

    if (display_phase == 0) {
        /* Phase 0: Zero out slice area - signals start of new frame */
        global_mmap_addr[80] = 0;
        memset(global_mmap_addr + 84, 0, 172);
    } else {
        /* Phases 1-6: Write slices 0-5 */
        int slice = display_phase - 1;
        int slice_offset = slice * 172;
        int slice_bytes = (slice == 5) ? 164 : 172;
        global_mmap_addr[80] = slice + 1;
        memcpy(global_mmap_addr + 84, display_src + slice_offset, slice_bytes);
    }

    display_phase = (display_phase + 1) % 7;  /* Cycle 0,1,2,3,4,5,6,0,... */
}

/* Callback for chain_mgmt: handle shim-specific param prefixes.
 * Reads/writes shadow_param->key/value/error/result_len directly.
 * Returns 1 if handled, 0 if not. */
static int shim_handle_param_special(uint8_t req_type, uint32_t req_id) {
    (void)req_id;
    const char *key = shadow_param->key;

    /* overtake_dsp:<sub_key> */
    if (strncmp(key, "overtake_dsp:", 13) == 0) {
        const char *param_key = key + 13;
        if (req_type == 1) {  /* SET */
            if (strcmp(param_key, "load") == 0) {
                shadow_overtake_dsp_load(shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (strcmp(param_key, "unload") == 0) {
                shadow_overtake_dsp_unload();
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->set_param) {
                overtake_dsp_gen->set_param(overtake_dsp_gen_inst, param_key, shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->set_param) {
                overtake_dsp_fx->set_param(overtake_dsp_fx_inst, param_key, shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else {
                shadow_param->error = 13;
                shadow_param->result_len = -1;
            }
        } else if (req_type == 2) {  /* GET */
            int len = -1;
            if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->get_param) {
                len = overtake_dsp_gen->get_param(overtake_dsp_gen_inst, param_key,
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
            } else if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->get_param) {
                len = overtake_dsp_fx->get_param(overtake_dsp_fx_inst, param_key,
                                                  shadow_param->value, SHADOW_PARAM_VALUE_LEN);
            }
            if (len >= 0) {
                shadow_param->error = 0;
                shadow_param->result_len = len;
            } else {
                shadow_param->error = 14;
                shadow_param->result_len = -1;
            }
        }
        return 1;
    }

    /* master_fx:resample_bridge */
    if (strncmp(key, "master_fx:", 10) == 0) {
        const char *fx_key = key + 10;
        if (strcmp(fx_key, "resample_bridge") == 0) {
            if (req_type == 1) {
                native_resample_bridge_mode_t new_mode =
                    native_resample_bridge_mode_from_text(shadow_param->value);
                if (new_mode != native_resample_bridge_mode) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Native resample bridge mode: %s",
                             native_resample_bridge_mode_name(new_mode));
                    shadow_log(msg);
                }
                native_resample_bridge_mode = new_mode;
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (req_type == 2) {
                int mode = (int)native_resample_bridge_mode;
                if (mode < 0 || mode > 2) mode = 0;
                shadow_param->result_len = snprintf(shadow_param->value,
                    SHADOW_PARAM_VALUE_LEN, "%d", mode);
                shadow_param->error = 0;
            }
            return 1;
        }
        /* master_fx:link_audio_routing */
        if (strcmp(fx_key, "link_audio_routing") == 0) {
            if (req_type == 1) {
                int val = atoi(shadow_param->value);
                link_audio_routing_enabled = val ? 1 : 0;
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Link Audio routing: %s",
                             link_audio_routing_enabled ? "ON" : "OFF");
                    shadow_log(msg);
                }
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (req_type == 2) {
                shadow_param->result_len = snprintf(shadow_param->value,
                    SHADOW_PARAM_VALUE_LEN, "%d", link_audio_routing_enabled);
                shadow_param->error = 0;
            }
            return 1;
        }
        /* master_fx:link_audio_publish */
        if (strcmp(fx_key, "link_audio_publish") == 0) {
            if (req_type == 1) {
                int val = atoi(shadow_param->value);
                link_audio_publish_enabled = val ? 1 : 0;
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Link Audio publish: %s",
                             link_audio_publish_enabled ? "ON" : "OFF");
                    shadow_log(msg);
                }
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (req_type == 2) {
                shadow_param->result_len = snprintf(shadow_param->value,
                    SHADOW_PARAM_VALUE_LEN, "%d", link_audio_publish_enabled);
                shadow_param->error = 0;
            }
            return 1;
        }
        /* master_fx:system_link_enabled (GET-only, reads Move's Settings.json) */
        if (strcmp(fx_key, "system_link_enabled") == 0) {
            if (req_type == 2) {
                int enabled = 0;
                FILE *f = fopen("/data/UserData/settings/Settings.json", "r");
                if (f) {
                    char buf[1024];
                    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                    fclose(f);
                    buf[n] = '\0';
                    char *p = strstr(buf, "\"isLinkEnabled\"");
                    if (p) {
                        p = strchr(p, ':');
                        if (p) {
                            p++;
                            while (*p == ' ' || *p == '\t') p++;
                            enabled = (strncmp(p, "true", 4) == 0) ? 1 : 0;
                        }
                    }
                }
                shadow_param->result_len = snprintf(shadow_param->value,
                    SHADOW_PARAM_VALUE_LEN, "%d", enabled);
                shadow_param->error = 0;
            } else {
                shadow_param->error = 1; /* read-only */
                shadow_param->result_len = 0;
            }
            return 1;
        }
    }

    return 0;  /* Not handled */
}

void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat64)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{

    printf(">>>>>>>>>>>>>>>>>>>>>>>> Hooked mmap...\n");
    if (!real_mmap)
    {
        real_mmap = dlsym(RTLD_NEXT, "mmap");
        if (!real_mmap)
        {
            fprintf(stderr, "Error: dlsym failed to find mmap\n");
            exit(1);
        }
    }

    void *result = real_mmap(addr, length, prot, flags, fd, offset);

    if (length == 4096)
    {
        /* Store the real hardware mailbox address */
        hardware_mmap_addr = result;

        /* Give Move our shadow buffer instead - we'll sync in ioctl hook */
        global_mmap_addr = shadow_mailbox;
        memset(shadow_mailbox, 0, sizeof(shadow_mailbox));

        printf("Shadow mailbox: Move sees %p, hardware at %p\n",
               (void*)shadow_mailbox, result);

        /* Initialize shadow shared memory when we detect the SPI mailbox */
        init_shadow_shm();
        /* Initialize link audio subsystem (before load_feature_config sets link_audio.enabled) */
        {
            link_audio_host_t la_host = {
                .log = shadow_log,
                .real_sendto_ptr = &real_sendto,
                .chain_slots = shadow_chain_slots,
            };
            shadow_link_audio_init(&la_host);
        }
        load_feature_config();  /* Load feature flags from config */

        /* Initialize chain management subsystem (must be before sampler - provides shadow_log) */
        {
            chain_mgmt_host_t cm_host = {
                .shadow_control_ptr = &shadow_control,
                .shadow_param_ptr = &shadow_param,
                .shadow_ui_state_ptr = &shadow_ui_state,
                .global_mmap_addr_ptr = &global_mmap_addr,
                .overlay_sync = shadow_overlay_sync,
                .run_command = shim_run_command,
                .launch_shadow_ui = launch_shadow_ui,
                .shadow_ui_enabled = &shadow_ui_enabled,
                .startup_modwheel_countdown = &shadow_startup_modwheel_countdown,
                .startup_modwheel_reset_frames = STARTUP_MODWHEEL_RESET_FRAMES,
                .handle_param_special = shim_handle_param_special,
            };
            chain_mgmt_init(&cm_host);
        }
        /* Initialize sampler subsystem with callbacks to shim functions */
        {
            sampler_host_t sampler_host = {
                .log = shadow_log,
                .announce = send_screenreader_announcement,
                .overlay_sync = shadow_overlay_sync,
                .run_command = shim_run_command,
                .global_mmap_addr = &global_mmap_addr,
                .hardware_mmap_addr = &hardware_mmap_addr,
            };
            sampler_init(&sampler_host, &sampler_set_tempo);
        }
        /* Initialize set pages subsystem with callbacks to shim functions */
        {
            set_pages_host_t sp_host = {
                .log = shadow_log,
                .announce = send_screenreader_announcement,
                .overlay_sync = shadow_overlay_sync,
                .run_command = shim_run_command,
                .save_state = shadow_save_state,
                .read_set_mute_states = shadow_read_set_mute_states,
                .read_set_tempo = sampler_read_set_tempo,
                .ui_state_update_slot = shadow_ui_state_update_slot,
                .ui_state_refresh = shadow_ui_state_refresh,
                .chain_parse_channel = shadow_chain_parse_channel,
                .chain_slots = shadow_chain_slots,
                .shadow_control_ptr = &shadow_control,
                .solo_count = (volatile int *)&shadow_solo_count,
            };
            set_pages_init(&sp_host);
        }
        if (shadow_control) {
            shadow_control->display_mirror = display_mirror_enabled ? 1 : 0;
            shadow_control->set_pages_enabled = set_pages_enabled ? 1 : 0;
        }
        /* Initialize process management subsystem */
        {
            process_host_t proc_host = {
                .log = shadow_log,
                .get_bpm = (float (*)(void *))sampler_get_bpm,
                .link_audio = &link_audio,
            };
            process_init(&proc_host);
        }
        /* Initialize resample bridge */
        {
            resample_host_t res_host = {
                .log = shadow_log,
                .global_mmap_addr = &global_mmap_addr,
                .shadow_master_volume = &shadow_master_volume,
            };
            resample_init(&res_host);
        }
        /* Initialize overlay drawing */
        {
            overlay_host_t ov_host = {
                .log = shadow_log,
                .announce = send_screenreader_announcement,
                .shadow_control = &shadow_control,
                .shadow_overlay_shm = &shadow_overlay_shm,
                .chain_slots = shadow_chain_slots,
                .plugin_v2 = &shadow_plugin_v2,
            };
            overlay_init(&ov_host);
        }
        /* Initialize PIN scanner */
        {
            pin_scanner_host_t pin_host = {
                .log = shadow_log,
                .tts_speak = tts_speak,
                .shadow_control = &shadow_control,
            };
            pin_scanner_init(&pin_host);
        }
        /* Initialize LED queue */
        {
            led_queue_host_t led_host = {
                .midi_out_buf = shadow_mailbox + MIDI_OUT_OFFSET,
                .shadow_control = &shadow_control,
                .shadow_ui_midi_shm = &shadow_ui_midi_shm,
            };
            led_queue_init(&led_host);
        }
        /* Initialize state persistence */
        {
            state_host_t st_host = {
                .log = shadow_log,
                .chain_slots = shadow_chain_slots,
                .solo_count = &shadow_solo_count,
            };
            state_init(&st_host);
        }
        /* Initialize MIDI routing */
        {
            midi_host_t midi_host = {
                .log = shadow_log,
                .midi_out_logf = shadow_midi_out_logf,
                .midi_out_log_enabled = shadow_midi_out_log_enabled,
                .ui_state_update_slot = shadow_ui_state_update_slot,
                .master_fx_forward_midi = shadow_master_fx_forward_midi,
                .queue_led = shadow_queue_led,
                .init_led_queue = shadow_init_led_queue,
                .chain_slots = shadow_chain_slots,
                .plugin_v2 = &shadow_plugin_v2,
                .shadow_control = &shadow_control,
                .global_mmap_addr = &global_mmap_addr,
                .shadow_inprocess_ready = &shadow_inprocess_ready,
                .shadow_display_mode = &shadow_display_mode,
                .shadow_midi_shm = &shadow_midi_shm,
                .shadow_midi_out_shm = &shadow_midi_out_shm,
                .shadow_ui_midi_shm = &shadow_ui_midi_shm,
                .shadow_midi_dsp_shm = &shadow_midi_dsp_shm,
                .shadow_mailbox = shadow_mailbox,
                .master_fx_capture = &shadow_master_fx_capture,
                .slot_idle = shadow_slot_idle,
                .slot_silence_frames = shadow_slot_silence_frames,
                .slot_fx_idle = shadow_slot_fx_idle,
                .slot_fx_silence_frames = shadow_slot_fx_silence_frames,
            };
            midi_routing_init(&midi_host);
        }
        /* Start Link Audio monitor — it will launch the subscriber
         * once link_audio_routing_enabled is set from config */
        if (link_audio.enabled) {
            start_link_sub_monitor();
        }
        native_resample_bridge_load_mode_from_shadow_config();  /* Restore bridge mode on Move restart */
#if SHADOW_INPROCESS_POC
        shadow_inprocess_load_chain();
        /* Initialize D-Bus subsystem with callbacks to shim functions */
        {
            dbus_host_t dbus_host = {
                .log = shadow_log,
                .save_state = shadow_save_state,
                .apply_mute = shadow_apply_mute,
                .ui_state_update_slot = shadow_ui_state_update_slot,
                .native_sampler_update = native_sampler_update_from_dbus_text,
                .chain_slots = shadow_chain_slots,
                .shadow_control_ptr = &shadow_control,
                .display_mode = &shadow_display_mode,
                .held_track = (volatile int *)&shadow_held_track,
                .selected_slot = (volatile int *)&shadow_selected_slot,
                .solo_count = (volatile int *)&shadow_solo_count,
                .screenreader_shm = &shadow_screenreader_shm,
            };
            dbus_init(&dbus_host);
        }
        shadow_dbus_start();  /* Start D-Bus monitoring for volume sync */
        shadow_read_initial_volume();  /* Read initial master volume from settings */
        shadow_load_state();  /* Load saved slot volumes */

        /* Sync mute/solo from Song.abl at boot — the saved state may be stale
         * if the user changed mute/solo in Move's native UI after our last save. */
        if (sampler_current_set_name[0]) {
            int boot_muted[4], boot_soloed[4];
            int n = shadow_read_set_mute_states(sampler_current_set_name, boot_muted, boot_soloed);
            if (n > 0) {
                shadow_solo_count = 0;
                for (int i = 0; i < n && i < SHADOW_CHAIN_INSTANCES; i++) {
                    shadow_chain_slots[i].muted = boot_muted[i];
                    shadow_chain_slots[i].soloed = boot_soloed[i];
                    if (boot_soloed[i]) shadow_solo_count++;
                    shadow_ui_state_update_slot(i);
                }
                char m[128];
                snprintf(m, sizeof(m), "Boot Song.abl sync: muted=[%d,%d,%d,%d] soloed=[%d,%d,%d,%d]",
                         boot_muted[0], boot_muted[1], boot_muted[2], boot_muted[3],
                         boot_soloed[0], boot_soloed[1], boot_soloed[2], boot_soloed[3]);
                shadow_log(m);
            }
        }

        /* Initialize TTS and sync loaded state to shared memory */
        tts_init(44100);
        if (shadow_control) {
            shadow_control->tts_enabled = tts_get_enabled() ? 1 : 0;
            shadow_control->tts_volume = tts_get_volume();
            shadow_control->tts_speed = tts_get_speed();
            shadow_control->tts_pitch = (uint16_t)tts_get_pitch();
            shadow_control->tts_engine = (strcmp(tts_get_engine(), "flite") == 0) ? 1 : 0;
            unified_log("shim", LOG_LEVEL_INFO,
                       "TTS initialized, synced to shared memory: enabled=%s speed=%.2f pitch=%.1f volume=%d",
                       shadow_control->tts_enabled ? "ON" : "OFF",
                       shadow_control->tts_speed, (float)shadow_control->tts_pitch, shadow_control->tts_volume);
        }
#endif

        /* Return shadow buffer to Move instead of hardware address */
        printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%lld, result=%p (returning shadow)\n",
               addr, length, prot, flags, fd, (long long)offset, result);
        return shadow_mailbox;
    }

    printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%lld, result=%p\n",
           addr, length, prot, flags, fd, (long long)offset, result);

    return result;
}

static int open_common(const char *pathname, int flags, va_list ap, int use_openat, int dirfd)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(ap, int);
    }

    int fd;
    if (use_openat) {
        if (!real_openat) {
            real_openat = dlsym(RTLD_NEXT, "openat");
        }
        fd = real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
    } else {
        if (!real_open) {
            real_open = dlsym(RTLD_NEXT, "open");
        }
        fd = real_open ? real_open(pathname, flags, mode) : -1;
    }



    if (fd >= 0 && (path_matches_midi(pathname) || path_matches_spi(pathname))) {
        track_fd(fd, pathname);
        if (path_matches_midi(pathname) && trace_midi_fd_enabled())
            fd_trace_log_midi("OPEN", fd, pathname);
        if (path_matches_spi(pathname) && trace_spi_io_enabled())
            fd_trace_log_spi("OPEN", fd, pathname);
    }

    return fd;
}

int open(const char *pathname, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_common(pathname, flags, ap, 0, AT_FDCWD);
    va_end(ap);
    return fd;
}

int open64(const char *pathname, int flags, ...)
{
    if (!real_open64) {
        real_open64 = dlsym(RTLD_NEXT, "open64");
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(ap, int);
    }
    int fd = real_open64 ? real_open64(pathname, flags, mode) : -1;
    va_end(ap);

    if (fd >= 0 && (path_matches_midi(pathname) || path_matches_spi(pathname))) {
        track_fd(fd, pathname);
        if (path_matches_midi(pathname) && trace_midi_fd_enabled())
            fd_trace_log_midi("OPEN64", fd, pathname);
        if (path_matches_spi(pathname) && trace_spi_io_enabled())
            fd_trace_log_spi("OPEN64", fd, pathname);
    }
    return fd;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_common(pathname, flags, ap, 1, dirfd);
    va_end(ap);
    return fd;
}

int openat64(int dirfd, const char *pathname, int flags, ...)
{
    if (!real_openat64) {
        real_openat64 = dlsym(RTLD_NEXT, "openat64");
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(ap, int);
    }
    int fd = real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : -1;
    va_end(ap);

    if (fd >= 0 && (path_matches_midi(pathname) || path_matches_spi(pathname))) {
        track_fd(fd, pathname);
        if (path_matches_midi(pathname) && trace_midi_fd_enabled())
            fd_trace_log_midi("OPENAT64", fd, pathname);
        if (path_matches_spi(pathname) && trace_spi_io_enabled())
            fd_trace_log_spi("OPENAT64", fd, pathname);
    }
    return fd;
}

int close(int fd)
{
    if (!real_close) {
        real_close = dlsym(RTLD_NEXT, "close");
    }
    const char *path = tracked_path_for_fd(fd);
    if (path && path_matches_midi(path) && trace_midi_fd_enabled())
        fd_trace_log_midi("CLOSE", fd, path);
    if (path && path_matches_spi(path) && trace_spi_io_enabled())
        fd_trace_log_spi("CLOSE", fd, path);
    untrack_fd(fd);
    return real_close ? real_close(fd) : -1;
}

/* write() hook removed - conflicts with system headers
 * Using send() hook instead for D-Bus interception */

ssize_t read(int fd, void *buf, size_t count)
{
    if (!real_read) {
        real_read = dlsym(RTLD_NEXT, "read");
    }
    ssize_t ret = real_read ? real_read(fd, buf, count) : -1;
    const char *path = tracked_path_for_fd(fd);
    if (path && buf && ret > 0) {
        log_fd_bytes("READ ", fd, path, (const unsigned char *)buf, (size_t)ret);
    }
    return ret;
}


int (*real_ioctl)(int, unsigned long, ...) = NULL;

int shiftHeld = 0;
int volumeTouched = 0;
int wheelTouched = 0;
int knob8touched = 0;
int alreadyLaunched = 0;       /* Prevent multiple launches */

/* Debug logging disabled for performance - set to 1 to enable */
#define SHADOW_HOTKEY_DEBUG 0
#if SHADOW_HOTKEY_DEBUG
static FILE *hotkey_state_log = NULL;
#endif
static uint64_t shift_on_ms = 0;
static uint64_t vol_on_ms = 0;
static uint8_t hotkey_prev[MIDI_BUFFER_SIZE];
static int hotkey_prev_valid = 0;
static int shift_armed = 1;   /* Start armed so first press works */
static int volume_armed = 1;  /* Start armed so first press works */

static void log_hotkey_state(const char *tag);

static uint64_t now_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int within_window(uint64_t now, uint64_t ts, uint64_t window_ms)
{
    return ts > 0 && now >= ts && (now - ts) <= window_ms;
}

#define SHADOW_HOTKEY_WINDOW_MS 1500
#define SHADOW_HOTKEY_GRACE_MS 2000
static uint64_t shadow_hotkey_enable_ms = 0;
static int shadow_inject_knob_release = 0;  /* Set when toggling shadow mode to inject note-offs */

/* Shift+Vol+Knob1 toggle removed - use Track buttons or Shift+Jog instead */

static void log_hotkey_state(const char *tag)
{
#if SHADOW_HOTKEY_DEBUG
    if (!hotkey_state_log)
    {
        hotkey_state_log = fopen("/data/UserData/move-anything/hotkey_state.log", "a");
    }
    if (hotkey_state_log)
    {
        time_t now = time(NULL);
        fprintf(hotkey_state_log, "%ld %s shift=%d vol=%d knob8=%d\n",
                (long)now, tag, shiftHeld, volumeTouched, knob8touched);
        fflush(hotkey_state_log);
    }
#else
    (void)tag;
#endif
}

void midi_monitor()
{
    if (!global_mmap_addr)
    {
        return;
    }

    uint8_t *src = (hardware_mmap_addr ? hardware_mmap_addr : global_mmap_addr) + MIDI_IN_OFFSET;

    /* NOTE: Shadow mode MIDI filtering now happens AFTER ioctl in the ioctl() function.
     * This function only handles hotkey detection for shadow mode toggle. */

    if (!hotkey_prev_valid) {
        memcpy(hotkey_prev, src, MIDI_BUFFER_SIZE);
        hotkey_prev_valid = 1;
        return;
    }

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4)
    {
        if (memcmp(&src[i], &hotkey_prev[i], 4) == 0) {
            continue;
        }
        memcpy(&hotkey_prev[i], &src[i], 4);

        unsigned char *byte = &src[i];
        unsigned char cable = (*byte & 0b11110000) >> 4;
        unsigned char code_index_number = (*byte & 0b00001111);
        unsigned char midi_0 = *(byte + 1);
        unsigned char midi_1 = *(byte + 2);
        unsigned char midi_2 = *(byte + 3);

        if (code_index_number == 2 || code_index_number == 1 || (cable == 0xf && code_index_number == 0xb && midi_0 == 176))
        {
            continue;
        }

        if (midi_0 + midi_1 + midi_2 == 0)
        {
            continue;
        }

        int controlMessage = 0xb0;
        if (midi_0 == controlMessage)
        {
            if (midi_1 == 0x31)
            {
                if (midi_2 == 0x7f)
                {
#if SHADOW_HOTKEY_DEBUG
                    printf("Shift on\n");
#endif

                    if (!shiftHeld && shift_armed) {
                        shiftHeld = 1;
                        shadow_shift_held = 1;  /* Sync global for cross-function access */
                        if (shadow_control) shadow_control->shift_held = 1;
                        shift_on_ms = now_mono_ms();
                        log_hotkey_state("shift_on");
                                            }
                }
                else
                {
#if SHADOW_HOTKEY_DEBUG
                    printf("Shift off\n");
#endif

                    shiftHeld = 0;
                    shadow_shift_held = 0;  /* Sync global for cross-function access */
                    if (shadow_control) shadow_control->shift_held = 0;
                    shift_armed = 1;
                    shift_on_ms = 0;
                    log_hotkey_state("shift_off");
                }
            }

        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x07)
        {
            if (midi_2 == 0x7f)
            {
                if (!knob8touched) {
                    knob8touched = 1;
#if SHADOW_HOTKEY_DEBUG
                    printf("Knob 8 touch start\n");
#endif
                    log_hotkey_state("knob8_on");
                }
            }
            else
            {
                knob8touched = 0;
#if SHADOW_HOTKEY_DEBUG
                printf("Knob 8 touch stop\n");
#endif
                log_hotkey_state("knob8_off");
            }
        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x08)
        {
            if (midi_2 == 0x7f)
            {
                if (!volumeTouched && volume_armed) {
                    volumeTouched = 1;
                    shadow_volume_knob_touched = 1;  /* Sync global for cross-function access */
                    vol_on_ms = now_mono_ms();
                    log_hotkey_state("vol_on");
                }
            }
            else
            {
                volumeTouched = 0;
                shadow_volume_knob_touched = 0;  /* Sync global for cross-function access */
                volume_armed = 1;
                vol_on_ms = 0;
                log_hotkey_state("vol_off");
            }
        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x09)
        {
            if (midi_2 == 0x7f)
            {
                wheelTouched = 1;
            }
            else
            {
                wheelTouched = 0;
            }
        }

        /* OLD standalone launch code - check feature flag */
        if (shiftHeld && volumeTouched && knob8touched && !alreadyLaunched && standalone_enabled)
        {
            alreadyLaunched = 1;
            printf("Launching Move Anything!\n");
            link_sub_kill();
            launchChildAndKillThisProcess("/data/UserData/move-anything/start.sh", "start.sh", "");
        }

    }
}

// unsigned long ioctlCounter = 0;
int ioctl(int fd, unsigned long request, ...)
{
    if (!real_ioctl)
    {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl)
        {
            fprintf(stderr, "Error: dlsym failed to find ioctl\n");
            exit(1);
        }
    }

    va_list ap;
    void *argp = NULL;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);

    /* === COMPREHENSIVE IOCTL TIMING === */
    static struct timespec ioctl_start, pre_end, post_start, ioctl_end;
    static uint64_t total_sum = 0, pre_sum = 0, ioctl_sum = 0, post_sum = 0;
    static uint64_t total_max = 0, pre_max = 0, ioctl_max = 0, post_max = 0;
    static int timing_count = 0;
    static int baseline_mode = -1;  /* -1 = unknown, 0 = full mode, 1 = baseline only */

    /* === GRANULAR PRE-IOCTL TIMING === */
    static struct timespec section_start, section_end;
    static uint64_t midi_mon_sum = 0, midi_mon_max = 0;
    static uint64_t fwd_midi_sum = 0, fwd_midi_max = 0;
    static uint64_t mix_audio_sum = 0, mix_audio_max = 0;
    static uint64_t ui_req_sum = 0, ui_req_max = 0;
    static uint64_t param_req_sum = 0, param_req_max = 0;
    static uint64_t proc_midi_sum = 0, proc_midi_max = 0;
    static uint64_t inproc_mix_sum = 0, inproc_mix_max = 0;
    static uint64_t display_sum = 0, display_max = 0;
    static int granular_count = 0;

#define TIME_SECTION_START() clock_gettime(CLOCK_MONOTONIC, &section_start)
#define TIME_SECTION_END(sum_var, max_var) do { \
    clock_gettime(CLOCK_MONOTONIC, &section_end); \
    uint64_t _section_us = (section_end.tv_sec - section_start.tv_sec) * 1000000 + \
                   (section_end.tv_nsec - section_start.tv_nsec) / 1000; \
    sum_var += _section_us; \
    if (_section_us > max_var) max_var = _section_us; \
} while(0)

    /* === OVERRUN DETECTION AND RECOVERY === */
    static int consecutive_overruns = 0;
    static int skip_dsp_this_frame = 0;
    static uint64_t last_frame_total_us = 0;
#define OVERRUN_THRESHOLD_US 2850  /* Start worrying at 2850µs (98% of budget) */
#define SKIP_DSP_THRESHOLD 3       /* Skip DSP after 3 consecutive overruns */

    /* Check for baseline timing mode (set SHADOW_BASELINE=1 to disable all processing) */
    if (baseline_mode < 0) {
        const char *env = getenv("SHADOW_BASELINE");
        baseline_mode = (env && env[0] == '1') ? 1 : 0;
#if SHADOW_TIMING_LOG
        if (baseline_mode) {
            FILE *f = fopen("/tmp/ioctl_timing.log", "a");
            if (f) { fprintf(f, "=== BASELINE MODE: All processing disabled ===\n"); fclose(f); }
        }
#endif
    }

    clock_gettime(CLOCK_MONOTONIC, &ioctl_start);

    /* === IOCTL GAP DETECTION (always-on, no flag needed) === */
    {
        static struct timespec last_ioctl_time = {0, 0};
        if (last_ioctl_time.tv_sec > 0) {
            uint64_t gap_ms = (ioctl_start.tv_sec - last_ioctl_time.tv_sec) * 1000 +
                              (ioctl_start.tv_nsec - last_ioctl_time.tv_nsec) / 1000000;
            if (gap_ms > 1000) {
                char gap_msg[64];
                snprintf(gap_msg, sizeof(gap_msg), "Ioctl gap: %lu ms", (unsigned long)gap_ms);
                unified_log_crash(gap_msg);
            }
        }
        last_ioctl_time = ioctl_start;
    }

    /* === HEARTBEAT (every ~5700 frames / ~100s) === */
    {
        static uint32_t heartbeat_counter = 0;
        heartbeat_counter++;
        if (heartbeat_counter >= 5700) {
            heartbeat_counter = 0;
            if (unified_log_enabled()) {
                /* No file I/O here — fopen("/proc/self/statm") can block
                 * when the disk is busy, causing audio clicks. */
                unified_log("shim", LOG_LEVEL_DEBUG,
                    "Heartbeat: pid=%d overruns=%d display_mode=%d la_pkts=%u la_ch=%d la_stale=%u la_sub_pid=%d la_restarts=%d pin_chal=%d",
                    getpid(), consecutive_overruns,
                    shadow_display_mode,
                    link_audio.packets_intercepted, link_audio.move_channel_count,
                    la_stale_frames, (int)link_sub_pid, link_sub_restart_count,
                    shadow_control ? shadow_control->pin_challenge_active : -1);
            }
        }
    }

    /* === SET DETECTION (poll every ~3s / ~1000 frames) === */
    {
        static uint32_t set_poll_counter = 0;
        set_poll_counter++;
        if (set_poll_counter >= 500) {  /* ~1.5s at 44100/128 */
            set_poll_counter = 0;
            shadow_poll_current_set();
        }
    }

    /* Link subscriber stale/restart recovery runs in a background monitor thread
     * to keep process management and waitpid() out of this real-time path. */
    if (link_audio.enabled) {
        uint32_t la_pkts_now = link_audio.packets_intercepted;
        if (la_pkts_now > link_sub_ever_received) {
            link_sub_ever_received = la_pkts_now;
        }
    }

    /* Check if previous frame overran - if so, consider skipping expensive work */
    if (last_frame_total_us > OVERRUN_THRESHOLD_US) {
        consecutive_overruns++;
        if (consecutive_overruns >= SKIP_DSP_THRESHOLD) {
            skip_dsp_this_frame = 1;
#if SHADOW_TIMING_LOG
            static int skip_log_count = 0;
            if (skip_log_count++ < 10 || skip_log_count % 100 == 0) {
                FILE *f = fopen("/tmp/ioctl_timing.log", "a");
                if (f) {
                    fprintf(f, "SKIP_DSP: consecutive_overruns=%d, last_frame=%llu us\n",
                            consecutive_overruns, (unsigned long long)last_frame_total_us);
                    fclose(f);
                }
            }
#endif
        }
    } else {
        consecutive_overruns = 0;
        skip_dsp_this_frame = 0;
    }

    /* Skip all processing in baseline mode to measure pure Move ioctl time */
    if (baseline_mode) goto do_ioctl;

    // TODO: Consider using move-anything host code and quickjs for flexibility
    TIME_SECTION_START();
    midi_monitor();
    TIME_SECTION_END(midi_mon_sum, midi_mon_max);

    /* Check if shadow UI requested exit via shared memory */
    if (shadow_control && shadow_display_mode && !shadow_control->display_mode) {
        shadow_display_mode = 0;
        shadow_inject_knob_release = 1;  /* Inject note-offs when exiting shadow mode */
    }

    /* NOTE: MIDI filtering moved to AFTER ioctl - see post-ioctl section below */

    /* === SHADOW INSTRUMENT: PRE-IOCTL PROCESSING === */

    /* Forward MIDI BEFORE ioctl - hardware clears the buffer during transaction */
    TIME_SECTION_START();
    shadow_forward_midi();
    TIME_SECTION_END(fwd_midi_sum, fwd_midi_max);

    /* Mix shadow audio into mailbox BEFORE hardware transaction */
    TIME_SECTION_START();
    shadow_mix_audio();
    TIME_SECTION_END(mix_audio_sum, mix_audio_max);

#if SHADOW_INPROCESS_POC
    TIME_SECTION_START();
    shadow_inprocess_handle_ui_request();
    TIME_SECTION_END(ui_req_sum, ui_req_max);

    TIME_SECTION_START();
    shadow_inprocess_handle_param_request();
    TIME_SECTION_END(param_req_sum, param_req_max);

    /* Forward CC/pitch bend/aftertouch from external MIDI to MIDI_OUT
     * so DSP routing can pick them up (Move only echoes notes, not these) */
    shadow_forward_external_cc_to_out();

    TIME_SECTION_START();
    shadow_inprocess_process_midi();
    TIME_SECTION_END(proc_midi_sum, proc_midi_max);

    /* Drain MIDI-to-DSP from shadow UI (overtake modules sending to chain slots) */
    shadow_drain_ui_midi_dsp();

    /* Pre-ioctl: Mix from pre-rendered buffer (FAST, ~5µs)
     * DSP was rendered post-ioctl in the previous frame.
     * This adds ~3ms latency but lets Move process pad events faster.
     */
    static uint64_t mix_time_sum = 0;
    static int mix_time_count = 0;
    static uint64_t mix_time_max = 0;

    /* Always run pre-ioctl mix/capture path.
     * This path is lightweight and feeds native bridge state; skipping it causes
     * stale/invalid bridge snapshots and inconsistent resample behavior. */
    {
        struct timespec mix_start, mix_end;
        clock_gettime(CLOCK_MONOTONIC, &mix_start);

        shadow_inprocess_mix_from_buffer();  /* Fast: just memcpy+mix */

        clock_gettime(CLOCK_MONOTONIC, &mix_end);
        uint64_t mix_us = (mix_end.tv_sec - mix_start.tv_sec) * 1000000 +
                          (mix_end.tv_nsec - mix_start.tv_nsec) / 1000;
        mix_time_sum += mix_us;
        mix_time_count++;
        if (mix_us > mix_time_max) mix_time_max = mix_us;

        /* Track in granular timing */
        inproc_mix_sum += mix_us;
        if (mix_us > inproc_mix_max) inproc_mix_max = mix_us;
    }

    /* Update publisher shm slot active flags (subscriber reads these).
     * When Link Audio is receiving Move per-track audio, always mark all
     * 4 slots active so Live sees ME-1 through ME-4 even without synths.
     * The mix_from_buffer path publishes Move audio for inactive slots.
     * If link_audio_publish_enabled is off, deactivate all slots so the
     * subscriber won't create sinks and no shadow audio flows to Live. */
    if (shadow_pub_audio_shm && link_audio.enabled) {
        if (!link_audio_publish_enabled) {
            for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++)
                shadow_pub_audio_shm->slots[i].active = 0;
            shadow_pub_audio_shm->slots[LINK_AUDIO_PUB_MASTER_IDX].active = 0;
            shadow_pub_audio_shm->num_slots = 0;
        } else {
            int la_flowing = (link_audio.packets_intercepted > 0 &&
                              link_audio.move_channel_count >= 4);
            for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
                int is_active = la_flowing ||
                                (i < SHADOW_CHAIN_INSTANCES &&
                                 shadow_chain_slots[i].active &&
                                 shadow_chain_slots[i].instance != NULL);
                shadow_pub_audio_shm->slots[i].active = is_active;
            }
            /* Master slot is always active when Link Audio is flowing */
            shadow_pub_audio_shm->slots[LINK_AUDIO_PUB_MASTER_IDX].active = la_flowing;
            shadow_pub_audio_shm->num_slots = la_flowing ? LINK_AUDIO_PUB_SLOT_COUNT : 0;
        }
    }

    /* Mix TTS audio AFTER inprocess mix (which may zero-rebuild mailbox for Link Audio) */
    shadow_mix_tts();

    /* Signal Link Audio publisher thread to drain accumulated audio */
    if (link_audio.publisher_running) {
        link_audio.publisher_tick = 1;
    }

    /* Log pre-ioctl mix timing every 1000 blocks (~23 seconds) */
    if (mix_time_count >= 1000) {
#if SHADOW_TIMING_LOG
        uint64_t avg = mix_time_sum / mix_time_count;
        FILE *f = fopen("/tmp/dsp_timing.log", "a");
        if (f) {
            fprintf(f, "Pre-ioctl mix (from buffer): avg=%llu us, max=%llu us\n",
                    (unsigned long long)avg, (unsigned long long)mix_time_max);
            fclose(f);
        }
#endif
        mix_time_sum = 0;
        mix_time_count = 0;
        mix_time_max = 0;
    }
#endif

    /* === SLICE-BASED DISPLAY CAPTURE FOR VOLUME === */
    TIME_SECTION_START();  /* Start timing display section */
    static uint8_t captured_slices[6][172];
    static uint8_t slice_fresh[6] = {0};  /* Reset each time we want new capture */
    static int volume_capture_active = 0;
    static int volume_capture_cooldown = 0;
    static int volume_capture_warmup = 0;  /* Wait for Move to render overlay */

    /* Native Move display is visible either when shadow mode is off, or when
     * plain volume-touch temporarily hides shadow UI to reveal Move overlays. */
    int native_display_visible = (!shadow_display_mode) ||
                                 (shadow_display_mode &&
                                  shadow_volume_knob_touched &&
                                  !shadow_shift_held &&
                                  shadow_control &&
                                  !shadow_control->overtake_mode);

    if (global_mmap_addr && native_display_visible) {
        uint8_t *mem = (uint8_t *)global_mmap_addr;
        uint8_t slice_num = mem[80];

        /* Always capture incoming slices */
        if (slice_num >= 1 && slice_num <= 6) {
            int idx = slice_num - 1;
            int bytes = (idx == 5) ? 164 : 172;
            memcpy(captured_slices[idx], mem + 84, 172);
            slice_fresh[idx] = 1;

            /* Always accumulate into PIN display buffer for dump trigger */
            pin_accumulate_slice(idx, mem + 84, bytes);
        }

        /* When volume knob touched (and no track held), start capturing */
        if (shadow_volume_knob_touched && shadow_held_track < 0) {
            if (!volume_capture_active) {
                volume_capture_active = 1;
                volume_capture_warmup = 18;  /* Wait ~3 frames (6 slices * 3) for overlay to render */
                memset(slice_fresh, 0, 6);  /* Reset freshness */
            }

            /* Decrement warmup and skip reading until warmup complete */
            if (volume_capture_warmup > 0) {
                volume_capture_warmup--;
                memset(slice_fresh, 0, 6);  /* Discard stale slices during warmup */
            }

            /* Check if all slices are fresh */
            int all_fresh = 1;
            for (int i = 0; i < 6; i++) {
                if (!slice_fresh[i]) all_fresh = 0;
            }

            if (all_fresh && volume_capture_cooldown == 0) {
                /* Reconstruct display */
                uint8_t full_display[1024];
                for (int s = 0; s < 6; s++) {
                    int offset = s * 172;
                    int bytes = (s == 5) ? 164 : 172;
                    memcpy(full_display + offset, captured_slices[s], bytes);
                }

                /* Find the volume position indicator in the gap between VU bars.
                 * Rows 30-32 are blank on the volume overlay except for the 1-pixel
                 * vertical indicator.  Require: vertical alignment on rows 30+31+32
                 * at the same column AND the gap rows are otherwise blank (total lit
                 * pixels across all three rows <= 6).  Waveform screens have many
                 * scattered pixels on these rows. */
                int bar_col = -1;
                int gap_total_lit = 0;
                {
                    int page3 = 30 / 8;  /* page 3 for rows 30-31 */
                    int page4 = 32 / 8;  /* page 4 for row 32 */
                    int bit30 = 30 % 8;
                    int bit31 = 31 % 8;
                    int bit32 = 32 % 8;
                    for (int col = 0; col < 128; col++) {
                        int l30 = !!(full_display[page3 * 128 + col] & (1 << bit30));
                        int l31 = !!(full_display[page3 * 128 + col] & (1 << bit31));
                        int l32 = !!(full_display[page4 * 128 + col] & (1 << bit32));
                        gap_total_lit += l30 + l31 + l32;
                        if (l30 && l31 && l32 && bar_col < 0)
                            bar_col = col;
                    }
                }

                if (bar_col >= 0 && gap_total_lit <= 6) {
                    float normalized = (float)(bar_col - 4) / (122.0f - 4.0f);
                    if (normalized < 0.0f) normalized = 0.0f;
                    if (normalized > 1.0f) normalized = 1.0f;

                    /* Map pixel bar position to amplitude matching Move's volume curve.
                     * sqrt model: dB = -70 * (1 - sqrt(pos))
                     * Measured from Move's Settings.json globalVolume:
                     *   pos 0.25 → -33.2 dB (model: -35.0)
                     *   pos 0.50 → -19.9 dB (model: -20.5)
                     *   pos 0.75 → -10.4 dB (model:  -9.4)
                     *   pos 1.00 →   0.0 dB (model:   0.0) */
                    float amplitude;
                    if (normalized <= 0.0f) {
                        amplitude = 0.0f;
                    } else if (normalized >= 1.0f) {
                        amplitude = 1.0f;
                    } else {
                        float db = -70.0f * (1.0f - sqrtf(normalized));
                        amplitude = powf(10.0f, db / 20.0f);
                    }

                    if (fabsf(amplitude - shadow_master_volume) > 0.003f) {
                        shadow_master_volume = amplitude;
                        float db_val = (amplitude > 0.0f) ? (20.0f * log10f(amplitude)) : -99.0f;
                        char msg[112];
                        snprintf(msg, sizeof(msg), "Master volume: x=%d pos=%.3f dB=%.1f amp=%.4f", bar_col, normalized, db_val, amplitude);
                        shadow_log(msg);
                    }
                }

                memset(slice_fresh, 0, 6);  /* Reset for next capture */
                volume_capture_cooldown = 12;  /* ~2 display frames between reads */
            }
        } else {
            volume_capture_active = 0;
            volume_capture_warmup = 0;  /* Reset warmup for next touch */
        }

        if (volume_capture_cooldown > 0) volume_capture_cooldown--;

        /* === OVERLAY COMPOSITING ===
         * JS sets display_overlay in shadow_control_t:
         *   0 = off (normal native display)
         *   1 = rect overlay (blit rect from shadow display onto native)
         *   2 = fullscreen (replace native display with shadow display)
         * All overlays (sampler, skipback, shift+knob) are JS-rendered. */
        int shift_knob_overlay_on = (shift_knob_overlay_active && shift_knob_overlay_timeout > 0);
        int sampler_overlay_on = (sampler_overlay_active &&
                                  (sampler_state != SAMPLER_IDLE || sampler_overlay_timeout > 0));
        int sampler_fullscreen_on = (sampler_fullscreen_active &&
                                     (sampler_state != SAMPLER_IDLE || sampler_overlay_timeout > 0));
        int skipback_overlay_on = (skipback_overlay_timeout > 0);
        int set_page_overlay_on = (set_page_overlay_active && set_page_overlay_timeout > 0);

        /* Read JS display_overlay request */
        uint8_t disp_overlay = shadow_control ? shadow_control->display_overlay : 0;

        int any_overlay = shift_knob_overlay_on || sampler_overlay_on ||
                          sampler_fullscreen_on || skipback_overlay_on ||
                          set_page_overlay_on || disp_overlay;
        if (any_overlay && slice_num >= 1 && slice_num <= 6) {
            static uint8_t overlay_display[1024];
            static int overlay_frame_ready = 0;

            if (slice_num == 1) {
                /* Track MIDI clock staleness (once per frame) */
                if (sampler_clock_active) {
                    sampler_clock_stale_frames++;
                    if (sampler_clock_stale_frames > SAMPLER_CLOCK_STALE_THRESHOLD) {
                        sampler_clock_active = 0;
                        sampler_clock_stale_frames = 0;
                    }
                }

                /* Update VU / sync for sampler when active */
                if (sampler_fullscreen_on || sampler_overlay_on) {
                    sampler_update_vu();
                    shadow_overlay_sync();
                }

                if (disp_overlay == 2 && shadow_display_shm) {
                    /* JS fullscreen: replace native display with shadow display */
                    memcpy(overlay_display, shadow_display_shm, 1024);
                    overlay_frame_ready = 1;
                } else if (disp_overlay == 1 && shadow_display_shm && shadow_control) {
                    /* JS rect overlay: reconstruct native, blit shadow rect on top */
                    int all_present = 1;
                    for (int i = 0; i < 6; i++) {
                        if (!slice_fresh[i]) all_present = 0;
                    }
                    if (all_present) {
                        for (int s = 0; s < 6; s++) {
                            int offset = s * 172;
                            int bytes = (s == 5) ? 164 : 172;
                            memcpy(overlay_display + offset, captured_slices[s], bytes);
                        }
                        overlay_blit_rect(overlay_display, shadow_display_shm,
                                          shadow_control->overlay_rect_x,
                                          shadow_control->overlay_rect_y,
                                          shadow_control->overlay_rect_w,
                                          shadow_control->overlay_rect_h);
                        overlay_frame_ready = 1;
                    }
                } else if (!disp_overlay) {
                    overlay_frame_ready = 0;
                }

                /* Decrement timeouts once per frame */
                if (shift_knob_overlay_on) {
                    shift_knob_overlay_timeout--;
                    if (shift_knob_overlay_timeout <= 0) {
                        shift_knob_overlay_active = 0;
                        shadow_overlay_sync();
                    }
                }
                if ((sampler_overlay_on || sampler_fullscreen_on) && sampler_state == SAMPLER_IDLE) {
                    sampler_overlay_timeout--;
                    if (sampler_overlay_timeout <= 0) {
                        sampler_overlay_active = 0;
                        sampler_fullscreen_active = 0;
                        shadow_overlay_sync();
                    }
                }
                if (skipback_overlay_on) {
                    skipback_overlay_timeout--;
                    if (skipback_overlay_timeout <= 0)
                        shadow_overlay_sync();
                }
                if (set_page_overlay_on) {
                    set_page_overlay_timeout--;
                    if (set_page_overlay_timeout <= 0) {
                        set_page_overlay_active = 0;
                        shadow_overlay_sync();
                    }
                }

                if (!any_overlay)
                    overlay_frame_ready = 0;
            }

            /* Copy overlay-composited slice back to mailbox */
            if (overlay_frame_ready) {
                int idx = slice_num - 1;
                int offset = idx * 172;
                int bytes = (idx == 5) ? 164 : 172;
                memcpy(mem + 84, overlay_display + offset, bytes);
            }
        }
    }

    /* Write display BEFORE ioctl - overwrites Move's content right before send */
    shadow_swap_display();
    TIME_SECTION_END(display_sum, display_max);  /* End timing display section */

    /* Capture final display to live shm for remote viewer.
     * Shadow mode: copy from shadow display shm (full composited frame).
     * Native mode: reconstruct from captured slices (written above). */
    if (display_live_shm && shadow_control && shadow_control->display_mirror) {
        if (shadow_display_mode && shadow_display_shm) {
            memcpy(display_live_shm, shadow_display_shm, DISPLAY_BUFFER_SIZE);
        } else {
            static uint8_t live_native[DISPLAY_BUFFER_SIZE];
            static int live_slice_seen[6] = {0};
            uint8_t cur_slice = global_mmap_addr ? ((uint8_t *)global_mmap_addr)[80] : 0;
            if (cur_slice >= 1 && cur_slice <= 6) {
                int idx = cur_slice - 1;
                int bytes = (idx == 5) ? 164 : 172;
                memcpy(live_native + idx * 172, (uint8_t *)global_mmap_addr + 84, bytes);
                live_slice_seen[idx] = 1;
                /* On last slice, push full frame */
                if (cur_slice == 6) {
                    int all = 1;
                    for (int i = 0; i < 6; i++) { if (!live_slice_seen[i]) all = 0; }
                    if (all) {
                        memcpy(display_live_shm, live_native, DISPLAY_BUFFER_SIZE);
                        memset(live_slice_seen, 0, sizeof(live_slice_seen));
                    }
                }
            }
        }
    }

    /* === PIN CHALLENGE SCANNER ===
     * Check if a web PIN challenge is active and speak the digits. */
    pin_check_and_speak();

    /* Mark end of pre-ioctl processing */
    clock_gettime(CLOCK_MONOTONIC, &pre_end);

do_ioctl:
    /* In baseline mode, pre_end wasn't set - set it now */
    if (baseline_mode) clock_gettime(CLOCK_MONOTONIC, &pre_end);

    /* === SHADOW UI MIDI OUT (PRE-IOCTL) ===
     * Inject any MIDI from shadow UI into the mailbox before sync.
     * In overtake mode, also clears Move's cable 0 packets when shadow has new data. */
    shadow_clear_move_leds_if_overtake();  /* Free buffer space before inject */
    shadow_inject_ui_midi_out();
    shadow_flush_pending_leds();  /* Rate-limited LED output */

    /* === SCREEN READER ANNOUNCEMENTS ===
     * Check for and send accessibility announcements via D-Bus. */
    shadow_check_screenreader_announcements();

    /* === SHORTCUT INDICATOR LEDS ===
     * When Shift+Vol held, light step icon LEDs (CCs 16-31 = icons below steps).
     * Uses shadow_queue_led which gets flushed by shadow_flush_pending_leds above. */
    {
        static int shortcut_leds_on = 0;
        int want_on = shadow_shift_held && shadow_volume_knob_touched;
        if (want_on && !shortcut_leds_on) {
            shadow_queue_led(0x0B, 0xB0, 28, 118);  /* Step 13 icon = LightGrey (Tools) */
            shortcut_leds_on = 1;
        } else if (!want_on && shortcut_leds_on) {
            shadow_queue_led(0x0B, 0xB0, 28, 0);    /* Step 13 icon = off */
            shortcut_leds_on = 0;
        }
    }

    /* === SHADOW MAILBOX SYNC (PRE-IOCTL) ===
     * Copy shadow mailbox to hardware before ioctl.
     * Move has been writing to shadow_mailbox; now we send that to hardware. */
    if (hardware_mmap_addr) {
        memcpy(hardware_mmap_addr, shadow_mailbox, MAILBOX_SIZE);
    }

    /* === HARDWARE TRANSACTION === */
    int result = real_ioctl(fd, request, argp);

    /* === SHADOW MAILBOX SYNC (POST-IOCTL) ===
     * Copy hardware mailbox back to shadow, filtering MIDI_IN.
     * Hardware has filled in new data; we filter it before Move sees it.
     * This eliminates race conditions - Move only sees our shadow buffer. */
    if (hardware_mmap_addr) {
        /* Copy non-MIDI sections directly */
        memcpy(shadow_mailbox + MIDI_OUT_OFFSET, hardware_mmap_addr + MIDI_OUT_OFFSET,
               AUDIO_OUT_OFFSET - MIDI_OUT_OFFSET);  /* MIDI_OUT: 0-255 */
        memcpy(shadow_mailbox + AUDIO_OUT_OFFSET, hardware_mmap_addr + AUDIO_OUT_OFFSET,
               DISPLAY_OFFSET - AUDIO_OUT_OFFSET);   /* AUDIO_OUT: 256-767 */
        memcpy(shadow_mailbox + DISPLAY_OFFSET, hardware_mmap_addr + DISPLAY_OFFSET,
               MIDI_IN_OFFSET - DISPLAY_OFFSET);     /* DISPLAY: 768-2047 */
        memcpy(shadow_mailbox + AUDIO_IN_OFFSET, hardware_mmap_addr + AUDIO_IN_OFFSET,
               MAILBOX_SIZE - AUDIO_IN_OFFSET);      /* AUDIO_IN: 2304-4095 */

        /* Bridge Move Everything's total mix into native resampling path when selected. */
        native_resample_bridge_apply();

        /* Capture audio for sampler post-ioctl (Move Input source only - fresh hardware input) */
        if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT) {
            sampler_capture_audio();
            sampler_tick_preroll();
            /* Skipback: always capture Move Input source into rolling buffer */
            skipback_init();
            skipback_capture((int16_t *)(hardware_mmap_addr + AUDIO_IN_OFFSET));
        }

        /* Copy MIDI_IN with filtering when in shadow display mode */
        uint8_t *hw_midi = hardware_mmap_addr + MIDI_IN_OFFSET;
        uint8_t *sh_midi = shadow_mailbox + MIDI_IN_OFFSET;
        int overtake_mode = shadow_control ? shadow_control->overtake_mode : 0;

        if (shadow_display_mode && shadow_control) {
            /* Filter MIDI_IN: zero out jog/back/knobs */
            for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
                uint8_t cin = hw_midi[j] & 0x0F;
                uint8_t cable = (hw_midi[j] >> 4) & 0x0F;
                uint8_t status = hw_midi[j + 1];
                uint8_t type = status & 0xF0;
                uint8_t d1 = hw_midi[j + 2];

                int filter = 0;

                /* Only filter internal cable (0x00) */
                if (cable == 0x00) {
                    /* Overtake mode split:
                     * - mode 2 (module): block all cable 0 events from Move
                     * - mode 1 (menu): allow only volume touch/turn passthrough */
                    if (overtake_mode == 2) {
                        filter = 1;
                    } else if (overtake_mode == 1) {
                        filter = 1;
                        if (cin == 0x0B && type == 0xB0 && d1 == CC_MASTER_KNOB) {
                            filter = 0;
                        }
                        if ((cin == 0x09 || cin == 0x08) &&
                            (type == 0x90 || type == 0x80) &&
                            d1 == 8) {
                            filter = 0;
                        }
                    } else {
                        /* CC messages: filter jog/back controls (let up/down through for octave) */
                        if (cin == 0x0B && type == 0xB0) {
                            if (d1 == CC_JOG_WHEEL || d1 == CC_JOG_CLICK || d1 == CC_BACK) {
                                filter = 1;
                            }
                            /* Filter knob CCs when shift held */
                            if (d1 >= CC_KNOB1 && d1 <= CC_KNOB8) {
                                filter = 1;
                            }
                            /* Filter Menu and Jog Click CCs when Shift+Volume shortcut is active */
                            if ((d1 == CC_MENU || d1 == CC_JOG_CLICK) && shadow_shift_held && shadow_volume_knob_touched) {
                                filter = 1;
                            }
                        }
                        /* Note messages: filter knob touches (0-7,9).
                         * Keep note 8 (volume touch) so Move can do track+volume
                         * and native volume workflows while shadow UI is active. */
                        if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80)) {
                            if (d1 <= 7 || d1 == 9) {
                                filter = 1;
                            }
                        }
                    }
                }

                if (filter) {
                    /* Zero the event in shadow buffer */
                    sh_midi[j] = 0;
                    sh_midi[j + 1] = 0;
                    sh_midi[j + 2] = 0;
                    sh_midi[j + 3] = 0;
                } else {
                    /* Copy event as-is */
                    sh_midi[j] = hw_midi[j];
                    sh_midi[j + 1] = hw_midi[j + 1];
                    sh_midi[j + 2] = hw_midi[j + 2];
                    sh_midi[j + 3] = hw_midi[j + 3];
                }
            }
        } else {
            /* Not in shadow mode - copy MIDI_IN directly */
            memcpy(sh_midi, hw_midi, MIDI_BUFFER_SIZE);
        }

        /* === SHIFT+MENU SHORTCUT DETECTION AND BLOCKING (POST-IOCTL) ===
         * Scan hardware MIDI_IN for Shift+Menu, perform action, and block from reaching Move.
         * This works regardless of shadow_display_mode.
         * Skip entirely in overtake mode - overtake module owns all input. */
        if (overtake_mode) goto skip_shift_menu;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = hw_midi[j] & 0x0F;
            uint8_t cable = (hw_midi[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */
            if (cin == 0x0B) {  /* Control Change */
                uint8_t d1 = hw_midi[j + 2];
                uint8_t d2 = hw_midi[j + 3];

                /* Shift + Menu: single press = Master FX / screen reader settings
                 *                double press = toggle screen reader on/off
                 * First press is deferred 400ms to detect double-click. */
                /* Block Menu CC entirely when Shift is held (both press and release) */
                if (d1 == CC_MENU && shadow_shift_held) {
                    if (d2 > 0 && shadow_control) {
                        struct timespec sm_ts;
                        clock_gettime(CLOCK_MONOTONIC, &sm_ts);
                        uint64_t sm_now = (uint64_t)(sm_ts.tv_sec * 1000) + (sm_ts.tv_nsec / 1000000);

                        if (shift_menu_pending && (sm_now - shift_menu_pending_ms) < 300) {
                            /* Double-click: toggle screen reader */
                            shift_menu_pending = 0;
                            uint8_t was_on = shadow_control->tts_enabled;
                            shadow_control->tts_enabled = was_on ? 0 : 1;
                            tts_set_enabled(!was_on);
                            tts_speak(was_on ? "Screen reader off" : "Screen reader on");
                            shadow_log(was_on ? "Shift+Menu double-click: screen reader OFF"
                                              : "Shift+Menu double-click: screen reader ON");
                        } else {
                            /* First press: defer action */
                            shift_menu_pending = 1;
                            shift_menu_pending_ms = sm_now;
                        }
                    }
                    /* Block Menu CC from reaching Move by zeroing in shadow buffer */
                    char block_msg[128];
                    snprintf(block_msg, sizeof(block_msg), "Blocking Menu CC (POST-IOCTL d2=%d)", d2);
                    shadow_log(block_msg);
                    sh_midi[j] = 0;
                    sh_midi[j + 1] = 0;
                    sh_midi[j + 2] = 0;
                    sh_midi[j + 3] = 0;
                }
            }
        }
        skip_shift_menu:

        /* Deferred Shift+Menu single-press action (fires 400ms after first press if no double-click) */
        if (shift_menu_pending && shadow_control) {
            struct timespec sm_ts2;
            clock_gettime(CLOCK_MONOTONIC, &sm_ts2);
            uint64_t sm_now2 = (uint64_t)(sm_ts2.tv_sec * 1000) + (sm_ts2.tv_nsec / 1000000);
            if (sm_now2 - shift_menu_pending_ms >= 300) {
                shift_menu_pending = 0;
                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg), "Shift+Menu single-press (deferred), shadow_ui_enabled=%s",
                         shadow_ui_enabled ? "true" : "false");
                shadow_log(log_msg);

                if (shadow_ui_enabled) {
                    if (!shadow_display_mode) {
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();
                    } else {
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
                    }
                } else {
                    shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SCREENREADER;
                    shadow_display_mode = 1;
                    shadow_control->display_mode = 1;
                    launch_shadow_ui();
                }
            }
        }

        /* === SAMPLER MIDI FILTERING ===
         * Block events from reaching Move for sampler use.
         * Always block Shift+Record so the first press doesn't leak through.
         * Block jog while sampler is armed or recording. */
        {
            for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
                uint8_t cin = sh_midi[j] & 0x0F;
                uint8_t cable = (sh_midi[j] >> 4) & 0x0F;
                if (cable != 0x00) continue;
                uint8_t s_type = sh_midi[j + 1] & 0xF0;
                uint8_t s_d1 = sh_midi[j + 2];

                if (cin == 0x0B && s_type == 0xB0) {
                    /* Block Record (CC 118) from Move: always when Shift held,
                     * and also when sampler is non-idle (armed or recording) */
                    if (s_d1 == CC_RECORD && (shadow_shift_held || sampler_state != SAMPLER_IDLE)) {
                        sh_midi[j] = 0; sh_midi[j+1] = 0; sh_midi[j+2] = 0; sh_midi[j+3] = 0;
                    }
                    /* Block Shift+Capture from reaching Move */
                    if (s_d1 == CC_CAPTURE && shadow_shift_held) {
                        sh_midi[j] = 0; sh_midi[j+1] = 0; sh_midi[j+2] = 0; sh_midi[j+3] = 0;
                    }
                    /* Block jog/back while sampler UI is fullscreen and active */
                    if (sampler_state != SAMPLER_IDLE && sampler_fullscreen_active) {
                        if (s_d1 == CC_JOG_WHEEL || s_d1 == CC_JOG_CLICK || s_d1 == CC_BACK) {
                            sh_midi[j] = 0; sh_midi[j+1] = 0; sh_midi[j+2] = 0; sh_midi[j+3] = 0;
                        }
                    }
                }
            }
        }

        /* Memory barrier to ensure all writes are visible */
        __sync_synchronize();
    }

    /* Mark start of post-ioctl processing */
    clock_gettime(CLOCK_MONOTONIC, &post_start);

    /* Skip post-ioctl processing in baseline mode */
    if (baseline_mode) goto do_timing;

    /* === POST-IOCTL: TRACK BUTTON AND VOLUME KNOB DETECTION ===
     * Scan for track button CCs (40-43) for D-Bus volume sync,
     * and volume knob touch (note 8) for master volume display reading.
     * NOTE: We scan hardware_mmap_addr (unfiltered) because shadow_mailbox is already filtered. */
    if (hardware_mmap_addr && shadow_inprocess_ready) {
        uint8_t *src = hardware_mmap_addr + MIDI_IN_OFFSET;
        int overtake_active = shadow_control ? shadow_control->overtake_mode : 0;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* CC messages (CIN 0x0B) */
            if (cin == 0x0B && type == 0xB0) {
                /* In overtake mode, skip all shortcuts except Shift+Vol+Jog Click (exit) */
                if (overtake_active && !(d1 == CC_JOG_CLICK && shadow_shift_held && shadow_volume_knob_touched)) {
                    continue;
                }
                /* DEBUG: log CCs while shift held */
                if (shadow_shift_held && d2 > 0) {
                    char dbg[64];
                    snprintf(dbg, sizeof(dbg), "Shift+CC: cc=%d val=%d", d1, d2);
                    shadow_log(dbg);
                }
                /* Track buttons are CCs 40-43 */
                if (d1 >= 40 && d1 <= 43) {
                    int pressed = (d2 > 0);
                    shadow_update_held_track(d1, pressed);

                    /* Update selected slot when track is pressed (for Shift+Knob routing)
                     * Track buttons are reversed: CC43=Track1, CC42=Track2, CC41=Track3, CC40=Track4 */
                    if (pressed) {
                        int new_slot = 43 - d1;  /* Reverse: CC43→0, CC42→1, CC41→2, CC40→3 */
                        if (new_slot != shadow_selected_slot) {
                            shadow_selected_slot = new_slot;
                            /* Sync to shared memory for shadow UI and Shift+Knob routing */
                            if (shadow_control) {
                                shadow_control->selected_slot = (uint8_t)new_slot;
                                shadow_control->ui_slot = (uint8_t)new_slot;
                            }
                            char msg[64];
                            snprintf(msg, sizeof(msg), "Selected slot: %d (Track %d)", new_slot, new_slot + 1);
                            shadow_log(msg);
                        }

                        /* Shift + Mute + Track = toggle solo; Mute + Track = toggle mute */
                        if (shadow_mute_held) {
                            if (shadow_shift_held) {
                                shadow_toggle_solo(new_slot);
                            } else {
                                shadow_apply_mute(new_slot, !shadow_chain_slots[new_slot].muted);
                            }
                        }

                        /* Shift + Volume + Track = jump to that slot's edit screen (if shadow UI enabled) */
                        if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                            shadow_block_plain_volume_hide_until_release = 1;
                            shadow_control->ui_slot = new_slot;
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SLOT;
                            if (!shadow_display_mode) {
                                /* From Move mode: launch shadow UI */
                                shadow_display_mode = 1;
                                shadow_control->display_mode = 1;
                                launch_shadow_ui();
                            }
                            /* If already in shadow mode, flag will be picked up by tick() */
                            /* Block Track CC from reaching Move */
                            uint8_t *sh = shadow_mailbox + MIDI_IN_OFFSET;
                            sh[j] = 0; sh[j+1] = 0; sh[j+2] = 0; sh[j+3] = 0;
                            src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                        }

                        /* Shift + Track (without Volume) while shadow UI is displayed = dismiss shadow UI
                         * and let the Track CC pass through to Move for native track settings */
                        if (shadow_display_mode && shadow_shift_held && !shadow_volume_knob_touched &&
                            shadow_control) {
                            shadow_display_mode = 0;
                            shadow_control->display_mode = 0;
                            shadow_log("Shift+Track: dismissing shadow UI");
                        }
                    }
                }

                /* Mute button (CC 88): track held state */
                if (d1 == CC_MUTE) {
                    shadow_mute_held = (d2 > 0) ? 1 : 0;
                }

                /* Shift + Volume + Jog Click = toggle overtake module menu (if shadow UI enabled) */
                if (d1 == CC_JOG_CLICK && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                        if (!shadow_display_mode) {
                            /* From Move mode: launch shadow UI and show overtake menu */
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_OVERTAKE;
                            shadow_display_mode = 1;
                            shadow_control->display_mode = 1;
                            launch_shadow_ui();
                        } else {
                            /* Already in shadow mode: toggle - if in overtake, exit to Move */
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_OVERTAKE;
                        }
                        /* Block Jog Click from reaching Move */
                        src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                    }
                }

                /* Shift+Capture: save skipback buffer */
                if (d1 == CC_CAPTURE && d2 > 0 && shadow_shift_held) {
                    skipback_trigger_save();
                    src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                }

                /* Shift+Vol+Left/Right: set page navigation (when enabled) */
                if (shadow_control && shadow_control->set_pages_enabled &&
                    shadow_shift_held && shadow_volume_knob_touched && d2 > 0) {
                    if (d1 == CC_LEFT && set_page_current > 0) {
                        shadow_change_set_page(set_page_current - 1);
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    } else if (d1 == CC_RIGHT && set_page_current < SET_PAGES_TOTAL - 1) {
                        shadow_change_set_page(set_page_current + 1);
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    }
                }

                /* Sample/Record button (CC 118) - sampler intercept */
                if (d1 == CC_RECORD && d2 > 0) {
                    if (shadow_shift_held) {
                        /* Shift+Sample: arm/resume/cancel/force-stop */
                        if (sampler_state == SAMPLER_IDLE && !shadow_display_mode) {
                            sampler_state = SAMPLER_ARMED;
                            sampler_overlay_active = 1;
                            sampler_overlay_timeout = 0;
                            sampler_fullscreen_active = 1;
                            sampler_menu_cursor = SAMPLER_MENU_SOURCE;
                            shadow_overlay_sync();
                            shadow_log("Sampler: ARMED");
                            {
                                char sr_buf[256];
                                const char *src = (sampler_source == SAMPLER_SOURCE_RESAMPLE)
                                    ? "Resample" : "Move Input";
                                snprintf(sr_buf, sizeof(sr_buf),
                                    "Quantized Sampler. Source: %s. "
                                    "Press play or a pad to begin recording.",
                                    src);
                                send_screenreader_announcement(sr_buf);
                            }
                        } else if (sampler_state != SAMPLER_IDLE && !sampler_fullscreen_active) {
                            sampler_overlay_active = 1;
                            sampler_overlay_timeout = 0;
                            sampler_fullscreen_active = 1;
                            shadow_overlay_sync();
                            shadow_log("Sampler: fullscreen resumed via Shift+Sample");
                            send_screenreader_announcement("Sampler resumed");
                        } else if (sampler_state == SAMPLER_ARMED) {
                            sampler_state = SAMPLER_IDLE;
                            sampler_overlay_active = 0;
                            sampler_fullscreen_active = 0;
                            shadow_overlay_sync();
                            shadow_log("Sampler: cancelled");
                            send_screenreader_announcement("Sampler cancelled");
                        } else if (sampler_state == SAMPLER_RECORDING) {
                            shadow_log("Sampler: force stop via Shift+Sample");
                            sampler_stop_recording();
                        } else if (sampler_state == SAMPLER_PREROLL) {
                            shadow_log("Sampler: preroll cancelled via Shift+Sample");
                            sampler_stop_recording();
                        }
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    } else if (sampler_state == SAMPLER_RECORDING) {
                        /* Bare Sample while recording: stop */
                        shadow_log("Sampler: stopped via Sample button");
                        sampler_stop_recording();
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    } else if (sampler_state == SAMPLER_PREROLL) {
                        /* Bare Sample while preroll: cancel back to armed */
                        shadow_log("Sampler: preroll cancelled via Sample button");
                        sampler_stop_recording();
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    }
                }

                /* Back button while sampler is visible = hide sampler UI */
                if (d1 == CC_BACK && d2 > 0 &&
                    sampler_state != SAMPLER_IDLE && sampler_fullscreen_active) {
                    sampler_overlay_active = 0;
                    sampler_overlay_timeout = 0;
                    sampler_fullscreen_active = 0;
                    shadow_overlay_sync();
                    shadow_log("Sampler: fullscreen dismissed via Back");
                    send_screenreader_announcement("Sampler hidden. Shift+Sample to resume.");
                    src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                }

                /* Jog wheel while sampler is armed = navigate menu */
                if (d1 == CC_JOG_WHEEL &&
                    sampler_state == SAMPLER_ARMED && sampler_fullscreen_active) {
                    /* Decode relative value: 1-63=CW, 65-127=CCW */
                    if (d2 >= 1 && d2 <= 63) {
                        if (sampler_menu_cursor < SAMPLER_MENU_COUNT - 1)
                            sampler_menu_cursor++;
                    } else if (d2 >= 65 && d2 <= 127) {
                        if (sampler_menu_cursor > 0)
                            sampler_menu_cursor--;
                    }
                    shadow_overlay_sync();
                    sampler_announce_menu_item();
                    /* Block jog from reaching Move/shadow UI */
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }

                /* Jog click while sampler is armed = cycle selected menu item */
                if (d1 == CC_JOG_CLICK && d2 > 0 &&
                    sampler_state == SAMPLER_ARMED && sampler_fullscreen_active) {
                    if (sampler_menu_cursor == SAMPLER_MENU_SOURCE) {
                        sampler_source = (sampler_source == SAMPLER_SOURCE_RESAMPLE)
                            ? SAMPLER_SOURCE_MOVE_INPUT : SAMPLER_SOURCE_RESAMPLE;
                    } else if (sampler_menu_cursor == SAMPLER_MENU_DURATION) {
                        sampler_duration_index = (sampler_duration_index + 1) % SAMPLER_DURATION_COUNT;
                    } else if (sampler_menu_cursor == SAMPLER_MENU_PREROLL) {
                        sampler_preroll_enabled = !sampler_preroll_enabled;
                    }
                    shadow_overlay_sync();
                    sampler_announce_menu_item();
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }
            }

            /* Note On/Off messages (CIN 0x09/0x08) for knob touches and step buttons */
            if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80)) {
                int touched = (type == 0x90 && d2 > 0);

                /* Volume knob touch (note 8) */
                if (d1 == 8) {
                    if (touched != shadow_volume_knob_touched) {
                        shadow_volume_knob_touched = touched;
                        volumeTouched = touched;
                        if (!touched) {
                            shadow_block_plain_volume_hide_until_release = 0;
                        }
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Volume knob touch: %s", touched ? "ON" : "OFF");
                        shadow_log(msg);
                    }
                }

                /* Jog encoder touch (note 9) */
                if (d1 == 9) {
                    shadow_jog_touched = touched;
                }

                /* Knob 8 touch (note 7) */
                if (d1 == 7) {
                    if (touched != knob8touched) {
                        knob8touched = touched;
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Knob 8 touch: %s", touched ? "ON" : "OFF");
                        shadow_log(msg);
                    }
                }

                /* Shift + Volume + Knob8 = launch standalone Move Anything (if enabled) */
                if (shadow_shift_held && shadow_volume_knob_touched && knob8touched && !alreadyLaunched && standalone_enabled) {
                    alreadyLaunched = 1;
                    shadow_log("Launching Move Anything (Shift+Vol+Knob8)!");
                    link_sub_kill();
                    launchChildAndKillThisProcess("/data/UserData/move-anything/start.sh", "start.sh", "");
                }

                /* Shift + Volume + Step 2 (note 17) = jump to Global Settings */
                if (d1 == 17 && type == 0x90 && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                        shadow_block_plain_volume_hide_until_release = 1;
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SETTINGS;
                        /* Always ensure display shows shadow UI */
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();  /* No-op if already running */
                        /* Block Step note from reaching Move */
                        uint8_t *sh = shadow_mailbox + MIDI_IN_OFFSET;
                        sh[j] = 0; sh[j+1] = 0; sh[j+2] = 0; sh[j+3] = 0;
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    }
                }

                /* Shift + Volume + Step 13 (note 28) = jump to Tools menu */
                if (d1 == 28 && type == 0x90 && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                        shadow_block_plain_volume_hide_until_release = 1;
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_TOOLS;
                        /* Always ensure display shows shadow UI */
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();  /* No-op if already running */
                        /* Block Step note from reaching Move */
                        uint8_t *sh = shadow_mailbox + MIDI_IN_OFFSET;
                        sh[j] = 0; sh[j+1] = 0; sh[j+2] = 0; sh[j+3] = 0;
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    }
                }

                /* Shift + Step button while shadow UI is displayed = dismiss shadow UI
                 * (user is loading a native Move component to edit) */
                if (shadow_display_mode && shadow_shift_held && !shadow_volume_knob_touched &&
                    type == 0x90 && d2 > 0 &&
                    d1 >= CC_STEP_UI_FIRST && d1 <= CC_STEP_UI_LAST &&
                    shadow_control) {
                    shadow_display_mode = 0;
                    shadow_control->display_mode = 0;
                    shadow_log("Shift+Step: dismissing shadow UI");
                }

                /* Pad note-on while sampler armed = trigger recording (or preroll) */
                if (type == 0x90 && d2 > 0 && d1 >= 68 && d1 <= 99 &&
                    sampler_state == SAMPLER_ARMED) {
                    if (sampler_preroll_enabled && sampler_duration_options[sampler_duration_index] > 0) {
                        shadow_log("Sampler: triggered preroll by pad note-on");
                        sampler_start_preroll();
                    } else {
                        shadow_log("Sampler: triggered by pad note-on");
                        sampler_start_recording();
                    }
                    /* Do NOT block the note - it must play so it gets recorded */
                }
            }
        }

        /* External MIDI trigger (cable 2): any note-on triggers recording when armed */
        if (sampler_state == SAMPLER_ARMED) {
            for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
                uint8_t cable = (src[j] >> 4) & 0x0F;
                uint8_t cin = src[j] & 0x0F;
                if (cable != 0x02) continue;
                if (cin == 0x09) {  /* Note-on */
                    uint8_t vel = src[j + 3];
                    if (vel > 0) {
                        if (sampler_preroll_enabled && sampler_duration_options[sampler_duration_index] > 0) {
                            shadow_log("Sampler: triggered preroll by external MIDI (cable 2)");
                            sampler_start_preroll();
                        } else {
                            shadow_log("Sampler: triggered by external MIDI (cable 2)");
                            sampler_start_recording();
                        }
                        /* Do NOT block - let note pass through for playback/recording */
                        break;
                    }
                }
            }
        }
    }

    /* === POST-IOCTL: OVERLAY KNOB INTERCEPTION (MOVE MODE) ===
     * When in Move mode (not shadow mode) and the overlay activation condition is met,
     * intercept knob CCs (71-78) and route to shadow chain DSP.
     * Also block knob touch notes (0-7) to prevent them reaching Move.
     * Activation depends on overlay_knobs_mode: Shift (0), Jog Touch (1), Off (2), or Native (3). */
    uint8_t overlay_knobs_mode = shadow_control ? shadow_control->overlay_knobs_mode : OVERLAY_KNOBS_NATIVE;
    int overlay_active = 0;
    if (overlay_knobs_mode == OVERLAY_KNOBS_SHIFT) overlay_active = shiftHeld;
    else if (overlay_knobs_mode == OVERLAY_KNOBS_JOG_TOUCH) overlay_active = shadow_jog_touched;

    if (!shadow_display_mode && overlay_active && shadow_ui_enabled &&
        shadow_inprocess_ready && global_mmap_addr) {
        uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* Handle knob touch notes 0-7 - block from Move, show overlay */
            if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80) && d1 <= 7) {
                int knob_num = d1 + 1;  /* Note 0 = Knob 1, etc. */
                /* Use ui_slot from shadow UI navigation, fall back to track button selection */
                int slot = (shadow_control && shadow_control->ui_slot < SHADOW_CHAIN_INSTANCES)
                           ? shadow_control->ui_slot : shadow_selected_slot;
                if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) slot = 0;

                /* Note On (touch start) - show overlay and hold it */
                if (type == 0x90 && d2 > 0) {
                    shift_knob_update_overlay(slot, knob_num, 0);
                    /* Set timeout very high so it stays visible until Note Off */
                    shift_knob_overlay_timeout = 10000;
                }
                /* Note Off (touch release) - start normal timeout for fade */
                else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
                    /* Only fade if this is the knob that's currently shown */
                    if (shift_knob_overlay_active && shift_knob_overlay_knob == knob_num) {
                        shift_knob_overlay_timeout = SHIFT_KNOB_OVERLAY_FRAMES;
                        shadow_overlay_sync();
                    }
                }
                /* Block touch note from reaching Move */
                src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                continue;
            }

            /* Handle knob CC messages - adjust parameter via set_param */
            if (cin == 0x0B && type == 0xB0 && d1 >= 71 && d1 <= 78) {
                int knob_num = d1 - 70;  /* 1-8 */
                /* Use ui_slot from shadow UI navigation, fall back to track button selection */
                int slot = (shadow_control && shadow_control->ui_slot < SHADOW_CHAIN_INSTANCES)
                           ? shadow_control->ui_slot : shadow_selected_slot;
                if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) slot = 0;

                /* Debug: log knob CC received */
                {
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "Shift+Knob: CC=%d knob=%d d2=%d slot=%d active=%d v2=%d set_param=%d",
                             d1, knob_num, d2, slot,
                             shadow_chain_slots[slot].active,
                             shadow_plugin_v2 ? 1 : 0,
                             (shadow_plugin_v2 && shadow_plugin_v2->set_param) ? 1 : 0);
                    shadow_log(dbg);
                }

                /* Adjust parameter if slot is active */
                if (shadow_chain_slots[slot].active && shadow_plugin_v2 && shadow_plugin_v2->set_param) {
                    /* Decode relative encoder value to delta (1 = CW, 127 = CCW) */
                    int delta = 0;
                    if (d2 >= 1 && d2 <= 63) delta = d2;      /* Clockwise: 1-63 */
                    else if (d2 >= 65 && d2 <= 127) delta = d2 - 128;  /* Counter-clockwise: -63 to -1 */

                    if (delta != 0) {
                        /* Adjust parameter via knob_N_adjust */
                        char key[32];
                        char val[16];
                        snprintf(key, sizeof(key), "knob_%d_adjust", knob_num);
                        snprintf(val, sizeof(val), "%d", delta);
                        shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, key, val);
                    }
                }

                /* Always show overlay (shows "Unmapped" for unmapped knobs) */
                shift_knob_update_overlay(slot, knob_num, d2);

                /* Block CC from reaching Move when shift held */
                src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
            }
        }
    }

    /* === POST-IOCTL: NATIVE OVERLAY KNOB INTERCEPTION (MOVE MODE) ===
     * In Native mode, knob touches pass through to Move so the ME Slot preset
     * macros fire and produce D-Bus screen reader text ("ME S1 Knob3 57.42").
     * The D-Bus handler parses the text and maps knob -> shadow slot.
     * Once mapped, subsequent CCs are intercepted and routed to shadow DSP. */
    if (!shadow_display_mode && overlay_knobs_mode == OVERLAY_KNOBS_NATIVE &&
        shadow_ui_enabled && shadow_inprocess_ready && global_mmap_addr) {
        uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* Handle knob touch notes 0-7 - let pass through to Move, track touch state */
            if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80) && d1 <= 7) {
                int idx = d1;  /* Note 0 = knob index 0 */

                if (type == 0x90 && d2 > 0) {
                    /* Touch start - flag as touched, clear any stale mapping */
                    native_knob_touched[idx] = 1;
                    native_knob_mapped[idx] = 0;
                    native_knob_slot[idx] = -1;
                    native_knob_any_touched = 1;
                } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
                    /* Touch release - clear mapping and touch state */
                    native_knob_touched[idx] = 0;
                    native_knob_mapped[idx] = 0;
                    native_knob_slot[idx] = -1;
                    /* Recompute any_touched */
                    int any = 0;
                    for (int k = 0; k < 8; k++) {
                        if (native_knob_touched[k]) { any = 1; break; }
                    }
                    native_knob_any_touched = any;
                    /* Start overlay fade timeout */
                    int knob_num = idx + 1;
                    if (shift_knob_overlay_active && shift_knob_overlay_knob == knob_num) {
                        shift_knob_overlay_timeout = SHIFT_KNOB_OVERLAY_FRAMES;
                        shadow_overlay_sync();
                    }
                }
                /* DO NOT block - let touch note pass through to Move */
                continue;
            }

            /* Handle knob CC messages (71-78) */
            if (cin == 0x0B && type == 0xB0 && d1 >= 71 && d1 <= 78) {
                int idx = d1 - 71;     /* 0-7 */
                int knob_num = idx + 1; /* 1-8 */

                if (native_knob_mapped[idx] && native_knob_slot[idx] >= 0) {
                    /* Mapped: intercept CC and route to shadow slot */
                    int slot = native_knob_slot[idx];
                    if (slot < SHADOW_CHAIN_INSTANCES &&
                        shadow_chain_slots[slot].active &&
                        shadow_plugin_v2 && shadow_plugin_v2->set_param) {
                        int delta = 0;
                        if (d2 >= 1 && d2 <= 63) delta = d2;
                        else if (d2 >= 65 && d2 <= 127) delta = d2 - 128;

                        if (delta != 0) {
                            char key[32];
                            char val[16];
                            snprintf(key, sizeof(key), "knob_%d_adjust", knob_num);
                            snprintf(val, sizeof(val), "%d", delta);
                            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, key, val);
                        }
                    }
                    /* Show overlay */
                    shift_knob_update_overlay(native_knob_slot[idx], knob_num, d2);
                    /* Block CC from reaching Move */
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }
                /* else: not yet mapped - let CC pass through to Move so macro fires D-Bus text */
            }
        }
    }

    /* Clear overlay when Shift is released */
    if (!shiftHeld && shift_knob_overlay_active) {
        /* Don't immediately clear - let timeout handle it for smooth experience */
    }

    /* === POST-IOCTL: FORWARD MIDI TO SHADOW UI AND HANDLE CAPTURE RULES ===
     * Shadow mailbox sync already filtered MIDI_IN for Move.
     * Here we scan the UNFILTERED hardware buffer to:
     * 1. Forward relevant events to shadow_ui_midi_shm
     * 2. Handle capture rules (route captured events to DSP) */
#if !SHADOW_DISABLE_POST_IOCTL_MIDI
    if (shadow_display_mode && shadow_control && hardware_mmap_addr) {
        uint8_t *src = hardware_mmap_addr + MIDI_IN_OFFSET;  /* Scan unfiltered hardware buffer */
        int overtake_mode = shadow_control->overtake_mode;

        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            /* In overtake mode, allow sysex (CIN 0x04-0x07) and normal messages (0x08-0x0E) */
            if (overtake_mode) {
                if (cin < 0x04 || cin > 0x0E) continue;
            } else {
                if (cin < 0x08 || cin > 0x0E) continue;
                if (cable != 0x00) continue;  /* Only internal cable 0 (Move hardware) */
            }

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* In overtake mode, forward events to shadow UI.
             * overtake_mode=1 (menu): only forward UI events (jog, click, back)
             * overtake_mode=2 (module): forward ALL events (all cables) */
            if (overtake_mode && shadow_ui_midi_shm) {
                /* In menu mode (1), only forward essential UI events */
                if (overtake_mode == 1) {
                    int is_ui_event = (type == 0xB0 &&
                                      (d1 == 14 || d1 == 3 || d1 == 51 ||  /* jog, click, back */
                                       (d1 >= 40 && d1 <= 43)));           /* track buttons */
                    if (!is_ui_event) continue;  /* Skip non-UI events in menu mode */
                }

                /* Queue cable 2 note-on messages (external LED commands like M8)
                 * for rate-limited forwarding to prevent buffer overflow */
                if (cable == 0x02 && type == 0x90) {
                    shadow_queue_input_led(src[j], status, d1, d2);
                    continue;
                }

                /* All other messages: forward directly */
                for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                    if (shadow_ui_midi_shm[slot] == 0) {
                        shadow_ui_midi_shm[slot] = src[j];
                        shadow_ui_midi_shm[slot + 1] = status;
                        shadow_ui_midi_shm[slot + 2] = d1;
                        shadow_ui_midi_shm[slot + 3] = d2;
                        shadow_control->midi_ready++;
                        break;
                    }
                }
                continue;  /* Skip normal processing in overtake mode */
            }

            /* Handle CC events */
            if (type == 0xB0) {
                /* CCs to forward to shadow UI:
                 * - CC 14 (jog wheel), CC 3 (jog click), CC 51 (back)
                 * - CC 40-43 (track buttons)
                 * - CC 71-78 (knobs) */
                int forward_to_shadow = (d1 == 14 || d1 == 3 || d1 == 51 ||
                                         (d1 >= 40 && d1 <= 43) || (d1 >= 71 && d1 <= 78));

                if (forward_to_shadow && shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = 0x0B;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }

                /* Check capture rules for CCs (beyond the hardcoded blocks) */
                /* Skip knobs - they're handled by shadow UI, not routed to DSP */
                int is_knob_cc = (d1 >= 71 && d1 <= 78);
                {
                    const shadow_capture_rules_t *capture = shadow_get_focused_capture();
                    if (capture && capture_has_cc(capture, d1) && !is_knob_cc) {
                        /* Route captured CC to focused slot's DSP */
                        int slot = shadow_control ? shadow_control->ui_slot : 0;
                        if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES &&
                            shadow_chain_slots[slot].active &&
                            shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                            uint8_t msg[3] = { status, d1, d2 };
                            shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                                      MOVE_MIDI_SOURCE_INTERNAL);
                        }
                    }
                }
                continue;
            }

            /* Handle note events */
            if (type == 0x90 || type == 0x80) {
                /* Forward track notes (40-43) to shadow UI for slot switching */
                if (d1 >= 40 && d1 <= 43 && shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = (type == 0x90) ? 0x09 : 0x08;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }

                /* Forward knob touch notes (0-7) to shadow UI for peek-at-value */
                if (d1 <= 7 && shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = (type == 0x90) ? 0x09 : 0x08;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }

                /* Check capture rules for focused slot.
                 * Never route knob touch notes (0-9) to DSP even if in capture rules. */
                {
                    const shadow_capture_rules_t *capture = shadow_get_focused_capture();
                    if (capture && d1 >= 10 && capture_has_note(capture, d1)) {
                        /* Route captured note to focused slot's DSP */
                        int slot = shadow_control ? shadow_control->ui_slot : 0;
                        if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES &&
                            shadow_chain_slots[slot].active &&
                            shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                            uint8_t msg[3] = { status, d1, d2 };
                            shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                                      MOVE_MIDI_SOURCE_INTERNAL);
                        }
                    }
                }

                /* Broadcast internal MIDI to ALL active slots for audio FX (e.g. ducker).
                 * FX_BROADCAST only forwards to audio FX, not synth/MIDI FX, so this
                 * is safe even for the focused slot that received normal dispatch. */
                if (d1 >= 10 && shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                    for (int si = 0; si < SHADOW_CHAIN_INSTANCES; si++) {
                        if (!shadow_chain_slots[si].active || !shadow_chain_slots[si].instance)
                            continue;
                        uint8_t msg[3] = { status, d1, d2 };
                        shadow_plugin_v2->on_midi(shadow_chain_slots[si].instance, msg, 3,
                                                  MOVE_MIDI_SOURCE_FX_BROADCAST);
                    }
                }

                /* Forward note events to master FX (e.g. ducker) */
                if (d1 >= 10) {
                    uint8_t msg[3] = { status, d1, d2 };
                    shadow_master_fx_forward_midi(msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
                }
                continue;
            }
        }

        /* Flush pending input LED queue (for cable 2 external MIDI in overtake mode) */
        shadow_flush_pending_input_leds();
    }
#endif /* !SHADOW_DISABLE_POST_IOCTL_MIDI */

    /* === POST-IOCTL: INJECT KNOB RELEASE EVENTS ===
     * When toggling shadow mode, inject note-off events for knob touches
     * so Move doesn't think knobs are still being held.
     * This MUST happen AFTER filtering to avoid being zeroed out. */
#if !SHADOW_DISABLE_POST_IOCTL_MIDI
    if (shadow_inject_knob_release && global_mmap_addr) {
        shadow_inject_knob_release = 0;
        uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
        /* Find empty slots and inject note-offs for knobs 0, 7, 8 (Knob1, Knob8, Volume) */
        const uint8_t knob_notes[] = { 0, 7, 8 };  /* Knob 1, Knob 8, Volume */
        int injected = 0;
        for (int j = 0; j < MIDI_BUFFER_SIZE && injected < 3; j += 4) {
            if (src[j] == 0 && src[j+1] == 0 && src[j+2] == 0 && src[j+3] == 0) {
                /* Empty slot - inject note-off */
                src[j] = 0x08;  /* CIN = Note Off, Cable 0 */
                src[j + 1] = 0x80;  /* Note Off, channel 0 */
                src[j + 2] = knob_notes[injected];  /* Note number */
                src[j + 3] = 0x00;  /* Velocity 0 */
                injected++;
            }
        }
    }
#endif /* !SHADOW_DISABLE_POST_IOCTL_MIDI */

#if SHADOW_INPROCESS_POC
    /* === POST-IOCTL: DEFERRED DSP RENDERING (SLOW, ~300µs) ===
     * Render DSP for the NEXT frame. This happens AFTER the ioctl returns,
     * so Move gets to process pad events before we do heavy DSP work.
     * The rendered audio will be mixed in pre-ioctl of the next frame.
     */
    {
        static uint64_t render_time_sum = 0;
        static int render_time_count = 0;
        static uint64_t render_time_max = 0;

        struct timespec render_start, render_end;
        clock_gettime(CLOCK_MONOTONIC, &render_start);

        shadow_inprocess_render_to_buffer();  /* Slow: actual DSP rendering */

        clock_gettime(CLOCK_MONOTONIC, &render_end);
        uint64_t render_us = (render_end.tv_sec - render_start.tv_sec) * 1000000 +
                              (render_end.tv_nsec - render_start.tv_nsec) / 1000;
        render_time_sum += render_us;
        render_time_count++;
        if (render_us > render_time_max) render_time_max = render_us;

        /* Log DSP render timing every 1000 blocks (~23 seconds) */
        if (render_time_count >= 1000) {
#if SHADOW_TIMING_LOG
            uint64_t avg = render_time_sum / render_time_count;
            FILE *f = fopen("/tmp/dsp_timing.log", "a");
            if (f) {
                fprintf(f, "Post-ioctl DSP render: avg=%llu us, max=%llu us\n",
                        (unsigned long long)avg, (unsigned long long)render_time_max);
                fclose(f);
            }
#endif
            render_time_sum = 0;
            render_time_count = 0;
            render_time_max = 0;
        }
    }
#endif

    /* === POST-IOCTL: CHECK FOR RESTART REQUEST === */
    /* Shadow UI can request a Move restart (e.g. after core update) */
    if (shadow_control && shadow_control->restart_move) {
        shadow_control->restart_move = 0;
        shadow_control->should_exit = 1;  /* Tell shadow_ui to exit */
        shadow_log("Restart requested by shadow UI — restarting Move");
        /* Use restart script for clean restart (kill as root, start fresh).
         * launchChildAndKillThisProcess won't work because MoveOriginal has
         * file capabilities that trigger AT_SECURE, blocking LD_PRELOAD
         * when forked from a non-root process. */
        system("/data/UserData/move-anything/restart-move.sh");
    }

do_timing:
    /* === COMPREHENSIVE IOCTL TIMING CALCULATIONS === */
    clock_gettime(CLOCK_MONOTONIC, &ioctl_end);

    uint64_t pre_us = (pre_end.tv_sec - ioctl_start.tv_sec) * 1000000 +
                      (pre_end.tv_nsec - ioctl_start.tv_nsec) / 1000;
    uint64_t ioctl_us = (post_start.tv_sec - pre_end.tv_sec) * 1000000 +
                        (post_start.tv_nsec - pre_end.tv_nsec) / 1000;
    uint64_t post_us = (ioctl_end.tv_sec - post_start.tv_sec) * 1000000 +
                       (ioctl_end.tv_nsec - post_start.tv_nsec) / 1000;
    uint64_t total_us = (ioctl_end.tv_sec - ioctl_start.tv_sec) * 1000000 +
                        (ioctl_end.tv_nsec - ioctl_start.tv_nsec) / 1000;

    total_sum += total_us;
    pre_sum += pre_us;
    ioctl_sum += ioctl_us;
    post_sum += post_us;
    timing_count++;

    if (total_us > total_max) total_max = total_us;
    if (pre_us > pre_max) pre_max = pre_us;
    if (ioctl_us > ioctl_max) ioctl_max = ioctl_us;
    if (post_us > post_max) post_max = post_us;

#if SHADOW_TIMING_LOG
    /* Warn immediately if total hook time >2ms */
    if (total_us > 2000) {
        static int hook_overrun_count = 0;
        hook_overrun_count++;
        if (hook_overrun_count <= 10 || hook_overrun_count % 100 == 0) {
            FILE *f = fopen("/tmp/ioctl_timing.log", "a");
            if (f) {
                fprintf(f, "WARNING: Hook overrun #%d: total=%llu us (pre=%llu, ioctl=%llu, post=%llu)\n",
                        hook_overrun_count, (unsigned long long)total_us,
                        (unsigned long long)pre_us, (unsigned long long)ioctl_us,
                        (unsigned long long)post_us);
                fclose(f);
            }
        }
    }
#endif

    /* Log every 1000 blocks (~23 seconds) */
    if (timing_count >= 1000) {
#if SHADOW_TIMING_LOG
        FILE *f = fopen("/tmp/ioctl_timing.log", "a");
        if (f) {
            fprintf(f, "Ioctl timing (1000 blocks): total avg=%llu max=%llu | pre avg=%llu max=%llu | ioctl avg=%llu max=%llu | post avg=%llu max=%llu\n",
                    (unsigned long long)(total_sum / timing_count), (unsigned long long)total_max,
                    (unsigned long long)(pre_sum / timing_count), (unsigned long long)pre_max,
                    (unsigned long long)(ioctl_sum / timing_count), (unsigned long long)ioctl_max,
                    (unsigned long long)(post_sum / timing_count), (unsigned long long)post_max);
            fclose(f);
        }
#endif
        total_sum = pre_sum = ioctl_sum = post_sum = 0;
        total_max = pre_max = ioctl_max = post_max = 0;
        timing_count = 0;
    }

    /* Log granular pre-ioctl timing every 1000 blocks */
    granular_count++;
    if (granular_count >= 1000) {
#if SHADOW_TIMING_LOG
        FILE *f = fopen("/tmp/ioctl_timing.log", "a");
        if (f) {
            fprintf(f, "Granular: midi_mon avg=%llu max=%llu | fwd_midi avg=%llu max=%llu | "
                       "mix_audio avg=%llu max=%llu | ui_req avg=%llu max=%llu | "
                       "param_req avg=%llu max=%llu | proc_midi avg=%llu max=%llu | "
                       "inproc_mix avg=%llu max=%llu | display avg=%llu max=%llu\n",
                    (unsigned long long)(midi_mon_sum / granular_count), (unsigned long long)midi_mon_max,
                    (unsigned long long)(fwd_midi_sum / granular_count), (unsigned long long)fwd_midi_max,
                    (unsigned long long)(mix_audio_sum / granular_count), (unsigned long long)mix_audio_max,
                    (unsigned long long)(ui_req_sum / granular_count), (unsigned long long)ui_req_max,
                    (unsigned long long)(param_req_sum / granular_count), (unsigned long long)param_req_max,
                    (unsigned long long)(proc_midi_sum / granular_count), (unsigned long long)proc_midi_max,
                    (unsigned long long)(inproc_mix_sum / granular_count), (unsigned long long)inproc_mix_max,
                    (unsigned long long)(display_sum / granular_count), (unsigned long long)display_max);
            fclose(f);
        }
#endif
        midi_mon_sum = midi_mon_max = fwd_midi_sum = fwd_midi_max = 0;
        mix_audio_sum = mix_audio_max = ui_req_sum = ui_req_max = 0;
        param_req_sum = param_req_max = proc_midi_sum = proc_midi_max = 0;
        inproc_mix_sum = inproc_mix_max = display_sum = display_max = 0;
        granular_count = 0;
    }

    /* Record frame time for overrun detection in next iteration */
    last_frame_total_us = total_us;

    return result;
}
