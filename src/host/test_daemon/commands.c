/*
 * commands.c — verb table + handlers for schwung-testd.
 *
 * One verb-table row per supported command; handlers operate on the
 * SHM pointers wired in via commands_init(). Stream commands
 * (SUBSCRIBE / DUMP / UNSUBSCRIBE) are channel-parametric: the verb
 * takes a channel name and dispatches via the g_streams[] registry,
 * so adding a new stream (MIDI_IN, log tail, etc.) means adding one
 * row to g_streams + wiring the SHM in commands_init.
 */

#define _GNU_SOURCE

#include "commands.h"
#include "protocol.h"
#include "shadow_midi_inject_writer.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TESTD_VERSION  "0.1.0"

/* WAIT_FRAME guards: hard cap on N (avoid runaway tests) and on wall
 * clock (avoid hangs if the shim stops ticking). */
#define TESTD_WAIT_FRAME_MAX     10000
#define TESTD_WAIT_POLL_USEC     500
#define TESTD_WAIT_TIMEOUT_SEC   30

/* SHM pointers, set by commands_init() and held until process exit. */
static daemon_shm_t g_shm;

/* --------------------------------------------------------------------------
 * Stream registry — one entry per test-bus channel.
 *
 * Adding a new stream (e.g. MIDI_IN in Phase 3):
 *   1. Add the SHM pointer to daemon_shm_t in commands.h
 *   2. Map it in schwung_testd.c:wire_shm()
 *   3. Add a row to g_streams[] below
 *   4. Wire it in commands_init() (one strcmp + assignment)
 * No new commands needed; SUBSCRIBE/DUMP/UNSUBSCRIBE handle any
 * channel by name.
 * -------------------------------------------------------------------------- */

typedef struct stream_state {
    const char         *name;        /* protocol channel name (lowercase) */
    test_stream_shm_t  *shm;         /* wired in commands_init from g_shm */
    uint32_t            baseline;    /* per-client write_seq baseline */
    int                 subscribed;  /* 1 if SUBSCRIBE outstanding */
} stream_state_t;

static stream_state_t g_streams[] = {
    {"midi_out", NULL, 0, 0},
};
static const size_t N_STREAMS = sizeof(g_streams) / sizeof(g_streams[0]);

static stream_state_t *find_stream(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < N_STREAMS; i++) {
        if (strcmp(g_streams[i].name, name) == 0) return &g_streams[i];
    }
    return NULL;
}

static void stream_disable_and_reset(stream_state_t *s) {
    if (s->shm) {
        __atomic_store_n(&s->shm->enabled, 0, __ATOMIC_RELEASE);
    }
    s->subscribed = 0;
    s->baseline = 0;
}

void commands_init(const daemon_shm_t *shm) {
    g_shm = *shm;
    /* Wire each registered stream to its SHM pointer. Phase 3 adds
     * more channels here. */
    for (size_t i = 0; i < N_STREAMS; i++) {
        if (strcmp(g_streams[i].name, "midi_out") == 0) {
            g_streams[i].shm = g_shm.midi_out_stream;
        }
    }
}

void commands_reset_client_state(void) {
    /* Called from schwung_testd.c after a client disconnects. Disables
     * every stream the prior client opened so the next client starts
     * from a clean slate — without this, a dropped TCP connection
     * leaves the shim publishing into a ring we'd misinterpret as
     * events for the new client. */
    for (size_t i = 0; i < N_STREAMS; i++) {
        stream_disable_and_reset(&g_streams[i]);
    }
}

/* ---- handlers ---------------------------------------------------------- */

static int cmd_ping(int fd, const char *args) {
    if (args && *args) return protocol_reply_err(fd, "PING takes no args");
    return protocol_reply(fd, "OK schwung-testd " TESTD_VERSION);
}

static int cmd_inject_midi(int fd, const char *args) {
    if (!args || strlen(args) != 8) {
        return protocol_reply_err(fd, "INJECT_MIDI expects 8 hex chars (1 USB-MIDI packet)");
    }
    uint8_t pkt[4];
    if (protocol_parse_hex(args, 8, pkt) < 0) {
        return protocol_reply_err(fd, "INJECT_MIDI: bad hex");
    }

    /* All four producers (shim, shadow_ui, shadow_chain forwarder, this
     * daemon) share /schwung-midi-inject. Coordination lives in the
     * MPSC helper — see src/host/shadow_midi_inject_writer.h. */
    int rc = shadow_midi_inject_push(g_shm.inject, pkt);
    if (rc == -1) {
        return protocol_reply_err(fd, "INJECT_MIDI: inject buffer full, drain not running?");
    }
    if (rc == -2) {
        return protocol_reply_err(fd, "INJECT_MIDI: prior producer stranded, packet not committed");
    }
    return protocol_reply(fd, "OK");
}

static int cmd_wait_frame(int fd, const char *args) {
    if (!args) return protocol_reply_err(fd, "WAIT_FRAME expects N");
    char *end = NULL;
    long n = strtol(args, &end, 10);
    if (end == args || *end != '\0' || n < 1 || n > TESTD_WAIT_FRAME_MAX) {
        return protocol_reply_err(fd, "WAIT_FRAME: N must be 1..10000");
    }
    uint32_t start = g_shm.control->shim_counter;
    uint32_t target = start + (uint32_t)n;

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long long timeout_ms = (long long)TESTD_WAIT_TIMEOUT_SEC * 1000LL;
    for (;;) {
        uint32_t cur = g_shm.control->shim_counter;
        if ((int32_t)(cur - target) >= 0) {
            char line[TESTD_LINE_MAX];
            snprintf(line, sizeof(line), "OK frame=%u", cur);
            return protocol_reply(fd, line);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ms = (long long)(now.tv_sec - t0.tv_sec) * 1000LL
                             + (now.tv_nsec - t0.tv_nsec) / 1000000LL;
        if (elapsed_ms >= timeout_ms) {
            return protocol_reply_err(fd, "WAIT_FRAME: timeout (shim not ticking?)");
        }
        usleep(TESTD_WAIT_POLL_USEC);
    }
}

static int cmd_snapshot_pad_leds(int fd, const char *args) {
    if (args && *args) return protocol_reply_err(fd, "SNAPSHOT_PAD_LEDS takes no args");
    uint8_t copy[32];
    for (int i = 0; i < 32; i++) {
        copy[i] = g_shm.overlay->pad_led_colors[i];
    }
    char hex[65];
    protocol_format_hex(copy, 32, hex);
    char line[TESTD_LINE_MAX];
    snprintf(line, sizeof(line), "OK %s", hex);
    return protocol_reply(fd, line);
}

/* SNAPSHOT_STEP_LEDS deferred: it reads shadow_overlay_state_t.step_led_colors,
 * a field added by the separate "step LED visibility" shim feature that is not
 * in mainline yet. It returns with that feature + its tests in a follow-up PR. */

/* STATE — single-line snapshot of selected shim fields used as
 * precondition checks by Command-pattern UI tests (Phase 3). Fields
 * are space-separated `key=value` tokens; extend as commands need
 * more. Read-only, no side effects.
 *
 * Mixes fields from shadow_control_t and shadow_overlay_state_t —
 * both SHMs are already mapped, so combining into one STATE response
 * saves the client a round trip. */
static int cmd_state(int fd, const char *args) {
    if (args && *args) return protocol_reply_err(fd, "STATE takes no args");
    shadow_control_t       *c = g_shm.control;
    shadow_overlay_state_t *o = g_shm.overlay;
    char line[TESTD_LINE_MAX];
    /* Add fields one at a time as tests need new preconditions; never
     * remove or rename, that's a protocol break. Jack-detect fields
     * (speaker_active / line_in_connected) are the first invariants
     * we want to verify from injected CC 115/114. display_mode is
     * the "shadow UI displayed?" gate. */
    snprintf(line, sizeof(line),
             "OK move_ui_mode=%u overtake_mode=%u shift_held=%u "
             "selected_slot=%u ui_slot=%u shim_counter=%u "
             "transport_playing=%u speaker_active=%u "
             "line_in_connected=%u display_mode=%u",
             (unsigned)c->move_ui_mode,
             (unsigned)c->overtake_mode,
             (unsigned)c->shift_held,
             (unsigned)c->selected_slot,
             (unsigned)c->ui_slot,
             (unsigned)c->shim_counter,
             (unsigned)o->transport_playing,
             (unsigned)c->speaker_active,
             (unsigned)c->line_in_connected,
             (unsigned)c->display_mode);
    return protocol_reply(fd, line);
}

/* ---- channel-parametric stream commands ------------------------------- */

static int cmd_subscribe(int fd, const char *args) {
    stream_state_t *s = find_stream(args);
    if (!s) return protocol_reply_err(fd, "SUBSCRIBE: unknown channel (try: midi_out)");
    if (!s->shm) return protocol_reply_err(fd, "SUBSCRIBE: channel SHM not mapped");

    /* Enable shim's capture path and snapshot the current write_seq so
     * the next DUMP returns only events captured from this point on.
     * Re-subscribe acts as a baseline reset. */
    __atomic_store_n(&s->shm->enabled, 1, __ATOMIC_RELEASE);
    s->baseline = __atomic_load_n(&s->shm->write_seq, __ATOMIC_ACQUIRE);
    s->subscribed = 1;
    return protocol_reply(fd, "OK");
}

static int cmd_unsubscribe(int fd, const char *args) {
    stream_state_t *s = find_stream(args);
    if (!s) return protocol_reply_err(fd, "UNSUBSCRIBE: unknown channel");
    stream_disable_and_reset(s);
    return protocol_reply(fd, "OK");
}

/* DUMP <channel> — multi-line response:
 *   OK count=<N> dropped=<D>
 *   EV <frame_hex> <pkt_hex>     (×N lines)
 *   END
 */
static int cmd_dump(int fd, const char *args) {
    stream_state_t *s = find_stream(args);
    if (!s) return protocol_reply_err(fd, "DUMP: unknown channel");
    if (!s->shm) return protocol_reply_err(fd, "DUMP: channel SHM not mapped");
    if (!s->subscribed) {
        return protocol_reply_err(fd, "DUMP: not subscribed (call SUBSCRIBE first)");
    }

    uint32_t cur = __atomic_load_n(&s->shm->write_seq, __ATOMIC_ACQUIRE);

    /* Shim-restart detection: if cur is BEFORE our baseline, the
     * underlying write_seq was zeroed while we held an old value.
     * Re-baseline silently and return empty. */
    if ((int32_t)(cur - s->baseline) < 0) {
        s->baseline = cur;
        if (protocol_reply(fd, "OK count=0 dropped=0") < 0) return -1;
        return protocol_reply(fd, "END");
    }

    uint32_t delta = cur - s->baseline;
    uint32_t to_read = delta;
    uint32_t dropped = 0;
    uint32_t first = s->baseline;

    if (delta > TEST_STREAM_CAPACITY) {
        dropped = delta - TEST_STREAM_CAPACITY;
        to_read = TEST_STREAM_CAPACITY;
        first = cur - TEST_STREAM_CAPACITY;
    }

    char hdr[TESTD_LINE_MAX];
    snprintf(hdr, sizeof(hdr), "OK count=%u dropped=%u", to_read, dropped);
    if (protocol_reply(fd, hdr) < 0) return -1;

    for (uint32_t i = 0; i < to_read; i++) {
        uint32_t seq = first + i;
        test_stream_event_t ev = s->shm->buffer[seq % TEST_STREAM_CAPACITY];
        char pkt_hex[9];
        protocol_format_hex(ev.pkt, 4, pkt_hex);
        char line[TESTD_LINE_MAX];
        snprintf(line, sizeof(line), "EV %08x %s", ev.frame, pkt_hex);
        if (protocol_reply(fd, line) < 0) return -1;
    }

    if (protocol_reply(fd, "END") < 0) return -1;
    s->baseline = cur;
    return 0;
}

/* RESTART_MOVE — set `shadow_control_t.restart_move=1`. The shim sees
 * the flag on its next SPI frame and invokes /data/UserData/schwung/
 * restart-move.sh via system(), which SIGTERMs+SIGKILLs the whole
 * Move chain (MoveLauncher, MoveOriginal, schwung, shadow_ui) and
 * relaunches a fresh /opt/move/Move with shim LD_PRELOAD'd via the
 * standard wrapper. Total recovery time ~4 seconds end-to-end.
 *
 * The daemon and the test_stream SHM segments persist across the
 * restart (they're not children of MoveOriginal, kernel keeps the SHM
 * mapped). The shim's SHM init is idempotent (shm_open without
 * O_EXCL re-uses the existing segment), so shim_counter etc. continue
 * across the restart.
 *
 * After OK reply, callers should poll via STATE / WAIT_FRAME to detect
 * when the shim is ticking again — typically ~3-5 seconds. */
static int cmd_restart_move(int fd, const char *args) {
    if (args && *args) return protocol_reply_err(fd, "RESTART_MOVE takes no args");
    __atomic_store_n(&g_shm.control->restart_move, 1, __ATOMIC_RELEASE);
    return protocol_reply(fd, "OK");
}

/* SET_OPEN_TOOL <module_id> — ask shadow_ui to load the given tool
 * module (by its registry id). Mirrors what schwung-manager's web UI does
 * when the user clicks "Open in tool" — two-part message:
 *
 *   1. Write JSON payload {"tool_id":"X","file_path":""} to
 *      /data/UserData/schwung/open_tool_cmd.json
 *   2. Set shadow_control_t.open_tool_cmd = 1 to nudge shadow_ui
 *      that a new command is queued.
 *
 * shadow_ui's per-tick poll reads the flag (auto-clears in the
 * accessor), opens the JSON, finds the module by id in its
 * registry, and invokes startInteractiveTool. The module loads
 * into overtake mode — observable as state.overtake_mode == 2
 * within a few seconds.
 *
 * Validation: module_id is restricted to [a-zA-Z0-9_-]+, max 64
 * chars. The JSON we emit is hand-formatted (no escaping needed
 * given the restricted charset). file_path is empty — tools that
 * don't open a specific file ignore it; tools that do
 * (file-browser etc.) wouldn't be useful via this E2E path
 * without a separate test-side file management strategy.
 *
 * Caller should poll state() until overtake_mode reaches 2 (or
 * timeout) — load can take ~500 ms-2 s depending on module size. */
#define OPEN_TOOL_CMD_PATH "/data/UserData/schwung/open_tool_cmd.json"
#define MAX_MODULE_ID_LEN 64

static int cmd_set_open_tool(int fd, const char *args) {
    if (!args || !*args) {
        return protocol_reply_err(fd, "SET_OPEN_TOOL needs <module_id>");
    }
    /* Length first, then per-char whitelist. (Earlier loop-fused
     * version had an off-by-one — a 65-char id slipped through
     * because the length check fired one iteration late.) */
    size_t len = strlen(args);
    if (len == 0) {
        return protocol_reply_err(fd, "SET_OPEN_TOOL: empty module_id");
    }
    if (len > MAX_MODULE_ID_LEN) {
        return protocol_reply_err(fd, "SET_OPEN_TOOL: module_id too long");
    }
    /* Strict whitelist on module_id chars to keep JSON well-formed
     * without an escaping pass, AND to keep the file write safe
     * (no shell, no path traversal). */
    for (const char *p = args; *p; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok) {
            return protocol_reply_err(fd, "SET_OPEN_TOOL: module_id must match [a-zA-Z0-9_-]+");
        }
    }

    /* 1. Write JSON payload. Create+truncate so a partial old file
     * doesn't confuse shadow_ui's JSON parse.
     *
     * file_path is a non-empty placeholder ("__test_bus__") because
     * shadow_ui's open_tool_cmd handler checks `if (cmd.file_path &&
     * cmd.tool_id)` — empty string is falsy and the load is skipped.
     * Tools that don't need a file (anything with skip_file_browser:
     * true in tool_config) ignore the value. */
    char json[MAX_MODULE_ID_LEN + 128];
    int n = snprintf(json, sizeof(json),
                     "{\"tool_id\":\"%s\",\"file_path\":\"__test_bus__\"}", args);
    if (n < 0 || n >= (int)sizeof(json)) {
        return protocol_reply_err(fd, "SET_OPEN_TOOL: payload formatting failed");
    }

    int jfd = open(OPEN_TOOL_CMD_PATH,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (jfd < 0) {
        return protocol_reply_err(fd, "SET_OPEN_TOOL: cannot open JSON file for write");
    }
    ssize_t written = write(jfd, json, (size_t)n);
    if (written != n) {
        close(jfd);
        return protocol_reply_err(fd, "SET_OPEN_TOOL: short write to JSON file");
    }
    /* fsync before raising the SHM flag — release-store orders only
     * memory access between CPUs, NOT the VFS page cache. Without
     * fsync, shadow_ui's host_read_file might race the kernel's
     * deferred page flush and see an empty/truncated JSON (whose
     * `if (cmdJson)` check then silently skips the load). The
     * symptom from the test side: SET_OPEN_TOOL returns OK,
     * overtake_mode never reaches 2. */
    if (fsync(jfd) < 0) {
        close(jfd);
        return protocol_reply_err(fd, "SET_OPEN_TOOL: fsync failed");
    }
    close(jfd);

    /* 2. Raise the SHM flag. Now safe — file is on disk and
     * shadow_ui's read will see the JSON. */
    __atomic_store_n(&g_shm.control->open_tool_cmd, 1, __ATOMIC_RELEASE);

    return protocol_reply(fd, "OK");
}

/* ============================================================================
 * PARAM commands — SET_PARAM / GET_PARAM / *_FILE variants
 *
 * Route get/set requests into chain DSPs and the active overtake module
 * via the same /schwung-param SHM that shadow_ui uses. This is what
 * unlocks overtake-module E2E tests that need to:
 *   - bootstrap state beyond what input gestures can reach reasonably
 *     (load a fixture project, set rng.seed for deterministic RNG)
 *   - verify in-memory params not visible via LED / MIDI snapshots
 *     (per-step probability after a knob P-lock, track destination
 *     before pressing pads, etc.)
 *
 * Wire protocol:
 *   SET_PARAM <key> <value>            -> OK    (small scalar values)
 *   GET_PARAM <key>                    -> OK <value>
 *   SET_PARAM_FILE <key> <move_path>   -> OK    (load value from a
 *                                                 Move-side file —
 *                                                 use for >2 KiB JSON)
 *   DUMP_PARAM_FILE <key> <move_path>  -> OK    (write GET result to
 *                                                 a Move-side file —
 *                                                 test SCPs it back)
 *
 * Slot: hardcoded to 0 (the active overtake-module slot OR the
 * first chain slot when no overtake is loaded). Overtake-module tests
 * run in overtake_mode == 2 with slot 0. Multi-slot routing is out of
 * scope for the test bus today (tests can switch which slot is
 * "focused" via the existing schwung mechanisms if needed).
 *
 * Key prefixing: caller-supplied keys are sent verbatim. An overtake
 * module's params live under the "overtake_dsp:" prefix when it is the
 * active overtake module — the Python client prepends this for
 * overtake-targeted calls. master_fx, jack:, chain-slot keys go
 * through with their own prefixes unchanged.
 *
 * Race window with shadow_ui: shadow_ui is the other producer on
 * this SHM. The wait_idle protocol (busy-wait until
 * request_type == 0) serializes both producers — the larger of
 * the two timeouts wins. Tests run with shadow_ui mostly idle
 * (no human interaction), so contention is rare. If a test hits
 * this race and gets TIMEOUT, the daemon returns ERR and the
 * test should retry — see `bus.set_param()` Python helper. */

#define TESTD_PARAM_POLL_USEC      200
/* Generous timeout: shadow_ui's own default is 100 ms, but it's
 * tuned for small scalar params from JS. The test bus also routes
 * `project.json` GETs (5-50 KB of serialized state) which need
 * full overtake-DSP serialization on the audio thread — measured up to
 * ~2 s on a busy Move. 5 s gives us 2× safety margin.
 *
 * The bound is per-request, so a fast SET doesn't pay the cost of
 * a slow project.json's worst case. */
#define TESTD_PARAM_TIMEOUT_MS     5000

/* shadow_param request-id sequence. Single-producer relative to the
 * daemon (one client at a time per Phase 1 contract), so no atomics
 * needed. Starts at 1 — 0 is the "no response yet" sentinel that
 * shadow_param->response_id uses. */
static uint32_t g_testd_param_seq = 0;

static uint32_t testd_param_next_request_id(void) {
    g_testd_param_seq++;
    if (g_testd_param_seq == 0) g_testd_param_seq = 1;
    return g_testd_param_seq;
}

/* Busy-wait until shadow_param->request_type clears (peer drained the
 * previous request). Returns 1 on success, 0 on timeout.
 *
 * Acquire-load on request_type: pairs with the peer's release-store
 * when it clears the slot after processing. Without this, the ARM
 * A55 (weakly-ordered model) can reorder reads of key/value/error
 * before observing request_type==0, producing a spurious match
 * against a half-written response. plain `volatile` (which the
 * shadow_param_t fields carry) prevents compiler reordering but
 * NOT CPU reordering — atomic_load_acquire covers both. */
static int testd_param_wait_idle(int timeout_ms) {
    int polls = (timeout_ms * 1000) / TESTD_PARAM_POLL_USEC;
    if (polls < 1) polls = 1;
    while (__atomic_load_n(&g_shm.param->request_type, __ATOMIC_ACQUIRE) != 0
           && polls > 0) {
        usleep(TESTD_PARAM_POLL_USEC);
        polls--;
    }
    return __atomic_load_n(&g_shm.param->request_type, __ATOMIC_ACQUIRE) == 0;
}

/* Wait for shadow_param->response_ready with matching response_id.
 * Returns:  1 = success,  -1 = peer set error flag,  0 = timeout.
 *
 * Same acquire ordering rationale as wait_idle: load response_ready
 * with acquire so we see the peer's release-store ordering on the
 * other response fields (response_id, error, value) before deciding
 * the response is complete. */
static int testd_param_wait_response(uint32_t req_id, int timeout_ms) {
    int polls = (timeout_ms * 1000) / TESTD_PARAM_POLL_USEC;
    if (polls < 1) polls = 1;
    while (polls > 0) {
        if (__atomic_load_n(&g_shm.param->response_ready, __ATOMIC_ACQUIRE)) {
            /* Acquire above orders the subsequent plain reads below. */
            if (g_shm.param->response_id == req_id) {
                return g_shm.param->error ? -1 : 1;
            }
        }
        usleep(TESTD_PARAM_POLL_USEC);
        polls--;
    }
    return 0;
}

/* Shared SET helper: copy key+value into SHM, fire request_type=1,
 * wait for response. value_len < SHADOW_PARAM_VALUE_LEN required;
 * caller pre-validates. */
static int testd_param_do_set(int fd, const char *key, const char *value, size_t value_len) {
    if (!g_shm.param) {
        return protocol_reply_err(fd, "param SHM not mapped");
    }
    if (strlen(key) >= SHADOW_PARAM_KEY_LEN) {
        return protocol_reply_err(fd, "key too long");
    }
    if (value_len >= SHADOW_PARAM_VALUE_LEN) {
        return protocol_reply_err(fd, "value too long for param SHM");
    }

    if (!testd_param_wait_idle(TESTD_PARAM_TIMEOUT_MS)) {
        return protocol_reply_err(fd, "param SHM busy (shadow_ui not draining)");
    }

    uint32_t req_id = testd_param_next_request_id();
    strncpy(g_shm.param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    g_shm.param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    /* Use memcpy (not strncpy) because value may contain embedded NULs
     * for binary payloads. SET_PARAM_FILE reads raw file contents. */
    memcpy(g_shm.param->value, value, value_len);
    if (value_len < SHADOW_PARAM_VALUE_LEN) {
        g_shm.param->value[value_len] = '\0';
    }
    g_shm.param->slot = 0;
    g_shm.param->response_ready = 0;
    g_shm.param->error = 0;
    g_shm.param->response_id = 0;
    g_shm.param->request_id = req_id;
    /* Release-fence: ensure key/value/slot writes are visible before
     * the peer sees request_type != 0. */
    __atomic_store_n(&g_shm.param->request_type, 1, __ATOMIC_RELEASE);

    int rc = testd_param_wait_response(req_id, TESTD_PARAM_TIMEOUT_MS);
    if (rc == 0) return protocol_reply_err(fd, "param SET timeout");
    if (rc < 0) return protocol_reply_err(fd, "param SET error from peer");
    return protocol_reply(fd, "OK");
}

static int cmd_set_param(int fd, const char *args) {
    if (!args || !*args) {
        return protocol_reply_err(fd, "SET_PARAM expects <key> <value>");
    }
    /* Split on first whitespace: key + value. Value may contain spaces
     * (some chain params accept comma+space-separated lists). */
    const char *sep = strchr(args, ' ');
    if (!sep) {
        return protocol_reply_err(fd, "SET_PARAM: missing value after key");
    }
    size_t key_len = sep - args;
    if (key_len == 0) {
        return protocol_reply_err(fd, "SET_PARAM: empty key");
    }
    if (key_len >= SHADOW_PARAM_KEY_LEN) {
        return protocol_reply_err(fd, "SET_PARAM: key too long");
    }
    /* Reject '=' in key: shim's prefix-based routing (overtake_dsp:,
     * jack:, master_fx:, etc) does literal string comparisons. A
     * `key=foo` would silently miss every prefix and fall through to
     * a no-op set that still returns success — exactly the kind of
     * silent-wrong behavior tests need to catch loudly. */
    if (memchr(args, '=', key_len) != NULL) {
        return protocol_reply_err(fd, "SET_PARAM: key may not contain '='");
    }
    char key[SHADOW_PARAM_KEY_LEN];
    memcpy(key, args, key_len);
    key[key_len] = '\0';
    const char *value = sep + 1;
    return testd_param_do_set(fd, key, value, strlen(value));
}

static int cmd_get_param(int fd, const char *args) {
    if (!args || !*args) {
        return protocol_reply_err(fd, "GET_PARAM expects <key>");
    }
    /* No spaces allowed in keys; whole `args` is the key. */
    if (strchr(args, ' ') != NULL) {
        return protocol_reply_err(fd, "GET_PARAM: key may not contain spaces");
    }
    if (strlen(args) >= SHADOW_PARAM_KEY_LEN) {
        return protocol_reply_err(fd, "GET_PARAM: key too long");
    }
    if (!g_shm.param) {
        return protocol_reply_err(fd, "param SHM not mapped");
    }

    if (!testd_param_wait_idle(TESTD_PARAM_TIMEOUT_MS)) {
        return protocol_reply_err(fd, "param SHM busy");
    }

    uint32_t req_id = testd_param_next_request_id();
    strncpy(g_shm.param->key, args, SHADOW_PARAM_KEY_LEN - 1);
    g_shm.param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    /* Clear value to avoid stale data appearing in the response. */
    memset(g_shm.param->value, 0, SHADOW_PARAM_VALUE_LEN);
    g_shm.param->slot = 0;
    g_shm.param->response_ready = 0;
    g_shm.param->error = 0;
    g_shm.param->response_id = 0;
    g_shm.param->request_id = req_id;
    __atomic_store_n(&g_shm.param->request_type, 2, __ATOMIC_RELEASE);

    int rc = testd_param_wait_response(req_id, TESTD_PARAM_TIMEOUT_MS);
    if (rc == 0) return protocol_reply_err(fd, "param GET timeout");
    if (rc < 0) return protocol_reply_err(fd, "param GET error from peer");

    /* Response value must fit on the line. The cap is TESTD_LINE_MAX
     * (4 KiB). The snprintf below writes "OK %s" (3 bytes prefix +
     * value + NUL) into `line[TESTD_LINE_MAX]`; protocol_reply adds
     * `\n` in its own buffer. We need vlen + 3 + NUL ≤ TESTD_LINE_MAX,
     * i.e. vlen + 4 ≤ TESTD_LINE_MAX, i.e. vlen < TESTD_LINE_MAX - 3.
     * Large blobs like project.json (5-50 KB) overflow this and must
     * use DUMP_PARAM_FILE instead. */
    size_t vlen = strnlen(g_shm.param->value, SHADOW_PARAM_VALUE_LEN);
    if (vlen + 3 >= TESTD_LINE_MAX) {
        return protocol_reply_err(fd,
            "GET_PARAM: value too large for line protocol (use DUMP_PARAM_FILE)");
    }
    char line[TESTD_LINE_MAX];
    snprintf(line, sizeof(line), "OK %s", g_shm.param->value);
    return protocol_reply(fd, line);
}

/* SET_PARAM_FILE <key> <move_path>: read <move_path> from Move's
 * filesystem, set <key> = file contents. Used for large JSON
 * payloads (project.json ~5-50 KB) that don't fit in TESTD_LINE_MAX.
 * Caller (test side) must SCP the file to Move first. */
static int cmd_set_param_file(int fd, const char *args) {
    if (!args || !*args) {
        return protocol_reply_err(fd, "SET_PARAM_FILE expects <key> <path>");
    }
    const char *sep = strchr(args, ' ');
    if (!sep) {
        return protocol_reply_err(fd, "SET_PARAM_FILE: missing path after key");
    }
    size_t key_len = sep - args;
    if (key_len == 0 || key_len >= SHADOW_PARAM_KEY_LEN) {
        return protocol_reply_err(fd, "SET_PARAM_FILE: bad key length");
    }
    char key[SHADOW_PARAM_KEY_LEN];
    memcpy(key, args, key_len);
    key[key_len] = '\0';
    const char *path = sep + 1;

    /* Read the file. Caller-side path; daemon trusts the test runner
     * not to point at /etc/shadow. Production-deploy gate (no setuid,
     * loopback only) bounds the threat. */
    int rfd = open(path, O_RDONLY);
    if (rfd < 0) {
        return protocol_reply_err(fd, "SET_PARAM_FILE: cannot open file");
    }
    struct stat st;
    if (fstat(rfd, &st) < 0) {
        close(rfd);
        return protocol_reply_err(fd, "SET_PARAM_FILE: stat failed");
    }
    /* Reject non-regular files. A symlink to a FIFO / socket / device
     * passes open() + fstat() with a plausible st_size, then read()
     * blocks indefinitely — hanging the daemon for TESTD_PARAM_TIMEOUT_MS
     * (or longer if the FIFO never has data). */
    if (!S_ISREG(st.st_mode)) {
        close(rfd);
        return protocol_reply_err(fd, "SET_PARAM_FILE: not a regular file");
    }
    /* Cast through signed off_t check first — `(size_t)(-1)` is SIZE_MAX
     * which falsely satisfies a naked `>= SHADOW_PARAM_VALUE_LEN` check
     * for filesystems that report negative sizes under error conditions. */
    if (st.st_size < 0 || (size_t)st.st_size >= SHADOW_PARAM_VALUE_LEN) {
        close(rfd);
        return protocol_reply_err(fd, "SET_PARAM_FILE: file too large or invalid size");
    }
    /* Read into a stack buffer (>= 64 KiB so stack growth from
     * default 8 MiB is still safe). Avoids heap alloc on hot path. */
    static char file_buf[SHADOW_PARAM_VALUE_LEN];
    ssize_t n = read(rfd, file_buf, st.st_size);
    close(rfd);
    if (n != st.st_size) {
        return protocol_reply_err(fd, "SET_PARAM_FILE: short read");
    }
    return testd_param_do_set(fd, key, file_buf, (size_t)n);
}

/* DUMP_PARAM_FILE <key> <move_path>: GET <key>, write response value
 * to <move_path>. Used for large GET payloads (project.json export)
 * that don't fit in the line protocol. Test reads <move_path> back
 * via SCP. */
static int cmd_dump_param_file(int fd, const char *args) {
    if (!args || !*args) {
        return protocol_reply_err(fd, "DUMP_PARAM_FILE expects <key> <path>");
    }
    const char *sep = strchr(args, ' ');
    if (!sep) {
        return protocol_reply_err(fd, "DUMP_PARAM_FILE: missing path after key");
    }
    size_t key_len = sep - args;
    if (key_len == 0 || key_len >= SHADOW_PARAM_KEY_LEN) {
        return protocol_reply_err(fd, "DUMP_PARAM_FILE: bad key length");
    }
    char key[SHADOW_PARAM_KEY_LEN];
    memcpy(key, args, key_len);
    key[key_len] = '\0';
    const char *path = sep + 1;
    if (!g_shm.param) {
        return protocol_reply_err(fd, "param SHM not mapped");
    }

    if (!testd_param_wait_idle(TESTD_PARAM_TIMEOUT_MS)) {
        return protocol_reply_err(fd, "param SHM busy");
    }
    uint32_t req_id = testd_param_next_request_id();
    strncpy(g_shm.param->key, key, SHADOW_PARAM_KEY_LEN - 1);
    g_shm.param->key[SHADOW_PARAM_KEY_LEN - 1] = '\0';
    memset(g_shm.param->value, 0, SHADOW_PARAM_VALUE_LEN);
    g_shm.param->slot = 0;
    g_shm.param->response_ready = 0;
    g_shm.param->error = 0;
    g_shm.param->response_id = 0;
    g_shm.param->request_id = req_id;
    __atomic_store_n(&g_shm.param->request_type, 2, __ATOMIC_RELEASE);

    int rc = testd_param_wait_response(req_id, TESTD_PARAM_TIMEOUT_MS);
    if (rc == 0) return protocol_reply_err(fd, "DUMP_PARAM_FILE: GET timeout");
    if (rc < 0) return protocol_reply_err(fd, "DUMP_PARAM_FILE: peer error");

    size_t vlen = strnlen(g_shm.param->value, SHADOW_PARAM_VALUE_LEN);
    int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (wfd < 0) {
        return protocol_reply_err(fd, "DUMP_PARAM_FILE: cannot open file for write");
    }
    ssize_t written = write(wfd, g_shm.param->value, vlen);
    if (written != (ssize_t)vlen) {
        close(wfd);
        return protocol_reply_err(fd, "DUMP_PARAM_FILE: short write");
    }
    if (fsync(wfd) < 0) {
        close(wfd);
        return protocol_reply_err(fd, "DUMP_PARAM_FILE: fsync failed");
    }
    close(wfd);

    char line[64];
    snprintf(line, sizeof(line), "OK bytes=%zu", vlen);
    return protocol_reply(fd, line);
}

static int cmd_quit(int fd, const char *args) {
    (void)args;
    /* Symmetric with the post-disconnect cleanup in schwung_testd.c —
     * either path leaves the shim with no live subscriptions. */
    commands_reset_client_state();
    protocol_reply(fd, "OK bye");
    return 1;  /* signal: close connection after this reply */
}

/* ---- dispatch ---------------------------------------------------------- */

typedef int (*command_fn)(int fd, const char *args);

typedef struct {
    const char *name;
    command_fn  handler;
} command_entry_t;

static const command_entry_t g_commands[] = {
    {"PING",              cmd_ping},
    {"INJECT_MIDI",       cmd_inject_midi},
    {"WAIT_FRAME",        cmd_wait_frame},
    {"SNAPSHOT_PAD_LEDS", cmd_snapshot_pad_leds},
    {"STATE",             cmd_state},
    {"RESTART_MOVE",      cmd_restart_move},
    {"SET_OPEN_TOOL",     cmd_set_open_tool},
    {"SET_PARAM",         cmd_set_param},
    {"GET_PARAM",         cmd_get_param},
    {"SET_PARAM_FILE",    cmd_set_param_file},
    {"DUMP_PARAM_FILE",   cmd_dump_param_file},
    {"SUBSCRIBE",         cmd_subscribe},
    {"UNSUBSCRIBE",       cmd_unsubscribe},
    {"DUMP",              cmd_dump},
    {"QUIT",              cmd_quit},
    {NULL, NULL},
};

int commands_dispatch(int fd, const char *verb, const char *args) {
    for (const command_entry_t *c = g_commands; c->name; c++) {
        if (strcmp(verb, c->name) == 0) {
            return c->handler(fd, args);
        }
    }
    return protocol_reply_err(fd, "unknown command");
}
