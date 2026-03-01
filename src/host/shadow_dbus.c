/* shadow_dbus.c - D-Bus monitoring, screen reader injection, volume/mute sync
 * Extracted from move_anything_shim.c for maintainability.
 *
 * Handles:
 * - D-Bus signal monitoring (screen reader text, volume changes)
 * - Screen reader announcement injection via Move's D-Bus socket
 * - Volume sync from D-Bus "Track Volume" messages
 * - Mute/solo state sync from D-Bus muted/unmuted/soloed/unsoloed messages
 * - Native overlay knob mapping from D-Bus text
 * - Hook callbacks for connect(), send(), sd_bus_default_system(), sd_bus_start()
 */

#ifndef ENABLE_SCREEN_READER
#define ENABLE_SCREEN_READER 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#if ENABLE_SCREEN_READER
#include <dbus/dbus.h>
#include <systemd/sd-bus.h>
#endif

#include "shadow_dbus.h"

/* ============================================================================
 * Internal state
 * ============================================================================ */

static dbus_host_t host;
static volatile int dbus_initialized = 0;  /* Guard: hooks are called before dbus_init() */

/* ============================================================================
 * Extern globals (defined here, declared extern in header)
 * ============================================================================ */

volatile int8_t  native_knob_slot[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
volatile uint8_t native_knob_touched[8] = {0};
volatile int     native_knob_any_touched = 0;
volatile uint8_t native_knob_mapped[8] = {0};

volatile int in_set_overview = 0;

bool tts_priority_announcement_active = false;
uint64_t tts_priority_announcement_time_ms = 0;

/* ============================================================================
 * Conditional compilation: full D-Bus implementation vs no-op stubs
 * ============================================================================ */

#if ENABLE_SCREEN_READER

/* D-Bus connection for monitoring */
static DBusConnection *shadow_dbus_conn = NULL;
static pthread_t shadow_dbus_thread;
static volatile int shadow_dbus_running = 0;

/* Move's D-Bus socket FD (ORIGINAL, for send() hook to recognize) */
static int move_dbus_socket_fd = -1;
static sd_bus *move_sdbus_conn = NULL;
static pthread_mutex_t move_dbus_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Shadow buffer for pending screen reader announcements */
static char pending_announcements[MAX_PENDING_ANNOUNCEMENTS][MAX_ANNOUNCEMENT_LEN];
static int pending_announcement_count = 0;
static pthread_mutex_t pending_announcements_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Track Move's D-Bus serial number for coordinated message injection */
static uint32_t move_dbus_serial = 0;
static pthread_mutex_t move_dbus_serial_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * Utility: parse volume from D-Bus text
 * ============================================================================ */

float shadow_parse_volume_db(const char *text)
{
    /* Format: "Track Volume X dB" or "Track Volume -inf dB" */
    if (!text) return -1.0f;

    const char *prefix = "Track Volume ";
    if (strncmp(text, prefix, strlen(prefix)) != 0) return -1.0f;

    const char *val_start = text + strlen(prefix);

    /* Handle -inf dB */
    if (strncmp(val_start, "-inf", 4) == 0) {
        return 0.0f;
    }

    /* Parse dB value */
    float db = strtof(val_start, NULL);

    /* Convert dB to linear: 10^(dB/20) */
    float linear = powf(10.0f, db / 20.0f);

    /* Clamp to reasonable range */
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 4.0f) linear = 4.0f;  /* +12 dB max */

    return linear;
}

/* ============================================================================
 * Inject pending screen reader announcements
 * ============================================================================ */

void shadow_inject_pending_announcements(void)
{
    pthread_mutex_lock(&pending_announcements_mutex);
    int has_pending = (pending_announcement_count > 0);
    pthread_mutex_unlock(&pending_announcements_mutex);

    if (!has_pending) return;

    pthread_mutex_lock(&move_dbus_conn_mutex);
    int fd = move_dbus_socket_fd;
    pthread_mutex_unlock(&move_dbus_conn_mutex);

    if (fd < 0) return;

    pthread_mutex_lock(&pending_announcements_mutex);
    for (int i = 0; i < pending_announcement_count; i++) {
        /* Create D-Bus signal message */
        DBusMessage *msg = dbus_message_new_signal(
            "/com/ableton/move/screenreader",
            "com.ableton.move.ScreenReader",
            "text"
        );

        if (msg) {
            const char *announce_text = pending_announcements[i];
            if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &announce_text, DBUS_TYPE_INVALID)) {
                /* Get next serial number */
                pthread_mutex_lock(&move_dbus_serial_mutex);
                move_dbus_serial++;
                uint32_t our_serial = move_dbus_serial;
                pthread_mutex_unlock(&move_dbus_serial_mutex);

                /* Set the serial number */
                dbus_message_set_serial(msg, our_serial);

                /* Serialize and write directly to Move's FD */
                char *marshalled = NULL;
                int msg_len = 0;
                if (dbus_message_marshal(msg, &marshalled, &msg_len)) {
                    ssize_t written = write(fd, marshalled, msg_len);

                    if (written > 0) {
                        char logbuf[512];
                        snprintf(logbuf, sizeof(logbuf),
                                "Screen reader: \"%s\" (injected %zd bytes to FD %d, serial=%u)",
                                announce_text, written, fd, our_serial);
                        host.log(logbuf);
                    } else {
                        char logbuf[256];
                        snprintf(logbuf, sizeof(logbuf),
                                "Screen reader: Failed to inject \"%s\" (errno=%d)",
                                announce_text, errno);
                        host.log(logbuf);
                    }

                    free(marshalled);
                }
            }
            dbus_message_unref(msg);
        }
    }
    pending_announcement_count = 0;  /* Clear after injecting */
    pthread_mutex_unlock(&pending_announcements_mutex);
}

/* ============================================================================
 * Handle screen reader text signal
 * ============================================================================ */

static void shadow_dbus_handle_text(const char *text)
{
    if (!text || !text[0]) return;

    /* Debug: log all D-Bus text messages */
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "D-Bus text: \"%s\" (held_track=%d)", text, *host.held_track);
        host.log(msg);
    }

    /* If Move is asking user to confirm shutdown, dismiss shadow UI so jog wheel
     * press reaches Move's native firmware instead of being captured by us.
     * Also signal the JS UI to save all state before power-off. */
    shadow_control_t *ctrl = *host.shadow_control_ptr;
    if (ctrl && strcasecmp(text, "Press wheel to shut down") == 0) {
        host.log("Shutdown prompt detected — saving state and dismissing shadow UI");
        ctrl->ui_flags |= SHADOW_UI_FLAG_SAVE_STATE;
        host.save_state();
        if (*host.display_mode) {
            *host.display_mode = 0;
            ctrl->display_mode = 0;
        }
    }

    /* Track native Move sampler source from stock announcements. */
    host.native_sampler_update(text);

    /* Set page: detect Set Overview screen for Shift+Vol+Left/Right interception */
    if (strcasecmp(text, "Set Overview") == 0 || strcasecmp(text, "Sets") == 0) {
        in_set_overview = 1;
    } else if (text[0] && strcasecmp(text, "Set Overview") != 0 &&
               strcasecmp(text, "Sets") != 0 &&
               strncmp(text, "Page ", 5) != 0) {
        /* Clear when navigating away (but not on our own "Page N of M" announcements) */
        in_set_overview = 0;
    }

    /* Native overlay knobs: parse "ME S<slot> Knob<n> <value>" from screen reader */
    if (ctrl &&
        ctrl->overlay_knobs_mode == OVERLAY_KNOBS_NATIVE &&
        native_knob_any_touched) {
        int me_slot = 0, me_knob = 0;
        if (sscanf(text, "ME S%d Knob%d", &me_slot, &me_knob) == 2 &&
            me_slot >= 1 && me_slot <= 4 && me_knob >= 1 && me_knob <= 8) {
            int idx = me_knob - 1;
            native_knob_slot[idx] = (int8_t)(me_slot - 1);
            native_knob_mapped[idx] = 1;
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "Native knob: mapped knob %d -> slot %d", me_knob, me_slot - 1);
                host.log(msg);
            }
            return;  /* Suppress TTS for ME knob macro text */
        }
    }

    /* Block D-Bus messages while priority announcement is playing */
    if (tts_priority_announcement_active) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

        if (now_ms - tts_priority_announcement_time_ms < TTS_PRIORITY_BLOCK_MS) {
            char msg[256];
            snprintf(msg, sizeof(msg), "D-Bus text BLOCKED (priority announcement): \"%s\"", text);
            host.log(msg);
            return;  /* Ignore D-Bus message during priority announcement */
        } else {
            /* Blocking period expired */
            tts_priority_announcement_active = false;
        }
    }

    /* Write screen reader text to shared memory for TTS */
    shadow_screenreader_t *sr_shm = *host.screenreader_shm;
    if (sr_shm) {
        /* Only increment sequence if text has actually changed (avoid duplicate increments) */
        if (strncmp(sr_shm->text, text, sizeof(sr_shm->text) - 1) != 0) {
            strncpy(sr_shm->text, text, sizeof(sr_shm->text) - 1);
            sr_shm->text[sizeof(sr_shm->text) - 1] = '\0';
            sr_shm->sequence++;  /* Increment to signal new message */
        }
    }

    /* Set detection handled by Settings.json polling (shadow_poll_current_set) */

    /* Check if it's a track volume message */
    if (strncmp(text, "Track Volume ", 13) == 0) {
        float volume = shadow_parse_volume_db(text);
        int held = *host.held_track;
        if (volume >= 0.0f && held >= 0 && held < SHADOW_CHAIN_INSTANCES) {
            if (!host.chain_slots[held].muted) {
                /* Update the held track's slot volume (skip if muted) */
                host.chain_slots[held].volume = volume;

                /* Log the volume sync */
                char msg[128];
                snprintf(msg, sizeof(msg), "D-Bus volume sync: slot %d = %.3f (%s)",
                         held, volume, text);
                host.log(msg);

                /* Persist slot volumes */
                host.save_state();
            }
        }
    }

    /* Auto-correct mute state from D-Bus screen reader text.
     * Move announces "<Instrument> muted" / "<Instrument> unmuted" on any
     * mute state change. Apply to the selected slot so we stay in sync even
     * when Move mutes/unmutes independently of our Mute+Track shortcut. */
    {
        int text_len = strlen(text);
        int ends_with_unmuted = (text_len >= 8 && strcmp(text + text_len - 7, "unmuted") == 0
                                 && text[text_len - 8] == ' ');
        int ends_with_muted = !ends_with_unmuted &&
                              (text_len >= 6 && strcmp(text + text_len - 5, "muted") == 0
                               && text[text_len - 6] == ' ');

        if (ends_with_muted || ends_with_unmuted) {
            host.apply_mute(*host.selected_slot, ends_with_muted);
        }
    }

    /* Auto-correct solo state from D-Bus screen reader text.
     * Move announces "<Instrument> soloed" / "<Instrument> unsoloed". */
    {
        int text_len = strlen(text);
        int ends_with_unsoloed = (text_len >= 9 && strcmp(text + text_len - 8, "unsoloed") == 0
                                  && text[text_len - 9] == ' ');
        int ends_with_soloed = !ends_with_unsoloed &&
                               (text_len >= 7 && strcmp(text + text_len - 6, "soloed") == 0
                                && text[text_len - 7] == ' ');

        if (ends_with_soloed) {
            /* Solo is exclusive — unsolo all, then solo this slot */
            int sel = *host.selected_slot;
            for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
                host.chain_slots[i].soloed = 0;
            host.chain_slots[sel].soloed = 1;
            *host.solo_count = 1;
            for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
                host.ui_state_update_slot(i);
            char msg[64];
            snprintf(msg, sizeof(msg), "D-Bus solo sync: slot %d soloed", sel);
            host.log(msg);
        } else if (ends_with_unsoloed) {
            int sel = *host.selected_slot;
            host.chain_slots[sel].soloed = 0;
            int count = 0;
            for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
                if (host.chain_slots[i].soloed) count++;
            }
            *host.solo_count = count;
            for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
                host.ui_state_update_slot(i);
            char msg[64];
            snprintf(msg, sizeof(msg), "D-Bus solo sync: slot %d unsoloed", sel);
            host.log(msg);
        }
    }

    /* After receiving any screen reader message from Move, inject our pending announcements */
    shadow_inject_pending_announcements();
}

/* ============================================================================
 * Parse D-Bus message serial number from raw bytes
 * ============================================================================ */

static uint32_t parse_dbus_serial(const uint8_t *buf, size_t len)
{
    /* D-Bus native wire format:
     * [0] = endianness ('l' for little-endian)
     * [1] = message type
     * [2] = flags
     * [3] = protocol version (usually 1)
     * [4-7] = body length (uint32)
     * [8-11] = serial number (uint32) */

    if (len < 12) return 0;
    if (buf[0] != 'l') return 0;  /* Only handle little-endian for now */

    uint32_t serial;
    memcpy(&serial, buf + 8, sizeof(serial));
    return serial;
}

/* ============================================================================
 * Hook callbacks - called from shim's LD_PRELOAD hooks
 * ============================================================================ */

void dbus_on_connect(int sockfd, const char *sun_path)
{
    if (!dbus_initialized) return;  /* Called before dbus_init() */

    /* Check if this is the D-Bus system bus socket */
    if (strstr(sun_path, "dbus") && strstr(sun_path, "system")) {
        pthread_mutex_lock(&move_dbus_conn_mutex);
        if (move_dbus_socket_fd == -1) {
            /* This is Move's D-Bus FD - we'll intercept writes to it */
            move_dbus_socket_fd = sockfd;
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf),
                    "D-Bus: *** INTERCEPTING Move's socket FD %d (path=%s) ***",
                    sockfd, sun_path);
            host.log(logbuf);
        }
        pthread_mutex_unlock(&move_dbus_conn_mutex);
    }
}

int dbus_on_send(int sockfd, const void *buf, size_t len, int flags,
                 ssize_t (*real_send)(int, const void *, size_t, int),
                 ssize_t *result_out)
{
    if (!dbus_initialized) return 0;  /* Called before dbus_init() */

    /* Check if this is a send to Move's D-Bus socket */
    pthread_mutex_lock(&move_dbus_conn_mutex);
    int is_move_dbus = (sockfd == move_dbus_socket_fd && move_dbus_socket_fd >= 0);
    pthread_mutex_unlock(&move_dbus_conn_mutex);

    if (!is_move_dbus) return 0;  /* Not D-Bus, caller should pass through */

    /* Parse and track Move's serial number */
    uint32_t serial = parse_dbus_serial((const uint8_t*)buf, len);
    if (serial > 0) {
        pthread_mutex_lock(&move_dbus_serial_mutex);
        if (serial > move_dbus_serial) {
            move_dbus_serial = serial;
        }
        pthread_mutex_unlock(&move_dbus_serial_mutex);
    }

    /* Forward Move's message first */
    ssize_t result = real_send(sockfd, buf, len, flags);

    /* Check if we have pending announcements to inject */
    pthread_mutex_lock(&pending_announcements_mutex);
    int has_pending = (pending_announcement_count > 0);
    pthread_mutex_unlock(&pending_announcements_mutex);

    if (has_pending && result > 0) {
        /* Inject our screen reader messages with coordinated serials */
        pthread_mutex_lock(&pending_announcements_mutex);
        for (int i = 0; i < pending_announcement_count; i++) {
            /* Create D-Bus signal message */
            DBusMessage *msg = dbus_message_new_signal(
                "/com/ableton/move/screenreader",
                "com.ableton.move.ScreenReader",
                "text"
            );

            if (msg) {
                const char *text = pending_announcements[i];
                if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID)) {
                    /* Get next serial number */
                    pthread_mutex_lock(&move_dbus_serial_mutex);
                    move_dbus_serial++;
                    uint32_t our_serial = move_dbus_serial;
                    pthread_mutex_unlock(&move_dbus_serial_mutex);

                    /* Set the serial number */
                    dbus_message_set_serial(msg, our_serial);

                    /* Serialize and send */
                    char *marshalled = NULL;
                    int msg_len = 0;
                    if (dbus_message_marshal(msg, &marshalled, &msg_len)) {
                        ssize_t written = real_send(sockfd, marshalled, msg_len, flags);

                        if (written > 0) {
                            char logbuf[512];
                            snprintf(logbuf, sizeof(logbuf),
                                    "Screen reader: \"%s\" (injected %zd bytes, serial=%u)",
                                    text, written, our_serial);
                            host.log(logbuf);
                        } else {
                            char logbuf[256];
                            snprintf(logbuf, sizeof(logbuf),
                                    "Screen reader: Failed to inject \"%s\" (errno=%d)",
                                    text, errno);
                            host.log(logbuf);
                        }

                        free(marshalled);
                    }
                }
                dbus_message_unref(msg);
            }
        }
        pending_announcement_count = 0;  /* Clear after injecting */
        pthread_mutex_unlock(&pending_announcements_mutex);
    }

    *result_out = result;
    return 1;  /* Was D-Bus, result stored */
}

void dbus_on_sd_bus_default(void *bus_ptr)
{
    if (!dbus_initialized) return;  /* Called before dbus_init() */
    sd_bus *bus = (sd_bus *)bus_ptr;
    pthread_mutex_lock(&move_dbus_conn_mutex);
    if (!move_sdbus_conn) {
        move_sdbus_conn = sd_bus_ref(bus);
        const char *unique_name = NULL;
        sd_bus_get_unique_name(bus, &unique_name);
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf),
                "D-Bus: *** CAPTURED sd-bus connection via sd_bus_default_system (sender=%s) ***",
                unique_name ? unique_name : "?");
        host.log(logbuf);
    }
    pthread_mutex_unlock(&move_dbus_conn_mutex);
}

void dbus_on_sd_bus_start(void *bus_ptr)
{
    if (!dbus_initialized) return;  /* Called before dbus_init() */
    sd_bus *bus = (sd_bus *)bus_ptr;
    pthread_mutex_lock(&move_dbus_conn_mutex);
    if (!move_sdbus_conn) {
        move_sdbus_conn = sd_bus_ref(bus);
        const char *unique_name = NULL;
        sd_bus_get_unique_name(bus, &unique_name);
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf),
                "D-Bus: *** CAPTURED sd-bus connection via sd_bus_start (sender=%s) ***",
                unique_name ? unique_name : "?");
        host.log(logbuf);
    }
    pthread_mutex_unlock(&move_dbus_conn_mutex);
}

/* ============================================================================
 * Queue a screen reader announcement
 * ============================================================================ */

void send_screenreader_announcement(const char *text)
{
    if (!text || !text[0]) return;

    pthread_mutex_lock(&move_dbus_conn_mutex);
    int sock_fd = move_dbus_socket_fd;
    pthread_mutex_unlock(&move_dbus_conn_mutex);

    if (sock_fd < 0) {
        /* Haven't captured Move's FD yet */
        return;
    }

    /* Add to pending queue */
    pthread_mutex_lock(&pending_announcements_mutex);
    if (pending_announcement_count < MAX_PENDING_ANNOUNCEMENTS) {
        size_t text_len = strlen(text);
        if (text_len >= MAX_ANNOUNCEMENT_LEN) {
            text_len = MAX_ANNOUNCEMENT_LEN - 1;
        }
        memcpy(pending_announcements[pending_announcement_count], text, text_len);
        pending_announcements[pending_announcement_count][text_len] = '\0';
        pending_announcement_count++;

        char logbuf[512];
        snprintf(logbuf, sizeof(logbuf),
                "Screen reader: Queued \"%s\" (pending=%d)",
                text, pending_announcement_count);
        host.log(logbuf);
    } else {
        host.log("Screen reader: Queue full, dropping announcement");
    }
    pthread_mutex_unlock(&pending_announcements_mutex);

    /* Flush immediately so announcements aren't delayed until next D-Bus activity */
    shadow_inject_pending_announcements();
}

/* ============================================================================
 * D-Bus filter function to receive signals
 * ============================================================================ */

static DBusHandlerResult shadow_dbus_filter(DBusConnection *conn, DBusMessage *msg, void *data)
{
    (void)conn;
    (void)data;

    /* Log ALL D-Bus signals for discovery (temporary) */
    {
        const char *iface = dbus_message_get_interface(msg);
        const char *member = dbus_message_get_member(msg);
        const char *path = dbus_message_get_path(msg);
        const char *sender = dbus_message_get_sender(msg);
        int msg_type = dbus_message_get_type(msg);

        /* Log WebServiceAuthentication method calls (challenge/PIN flow) */
        if (msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL &&
            iface && strcmp(iface, "com.ableton.move.WebServiceAuthentication") == 0) {
            char arg_preview[128] = "";
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter) &&
                dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                const char *s = NULL;
                dbus_message_iter_get_basic(&iter, &s);
                if (s) snprintf(arg_preview, sizeof(arg_preview), " arg0=\"%.60s\"", s);
            }
            char logbuf[512];
            snprintf(logbuf, sizeof(logbuf), "D-Bus AUTH: %s.%s path=%s sender=%s%s",
                     iface, member ? member : "?", path ? path : "?",
                     sender ? sender : "?", arg_preview);
            host.log(logbuf);
        }

        if (msg_type == DBUS_MESSAGE_TYPE_SIGNAL) {
            /* Extract first string arg if present */
            char arg_preview[128] = "";
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter)) {
                int atype = dbus_message_iter_get_arg_type(&iter);
                if (atype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&iter, &s);
                    if (s) snprintf(arg_preview, sizeof(arg_preview), " arg0=\"%.100s\"", s);
                } else if (atype == DBUS_TYPE_INT32) {
                    int32_t v;
                    dbus_message_iter_get_basic(&iter, &v);
                    snprintf(arg_preview, sizeof(arg_preview), " arg0=%d", v);
                } else if (atype == DBUS_TYPE_UINT32) {
                    uint32_t v;
                    dbus_message_iter_get_basic(&iter, &v);
                    snprintf(arg_preview, sizeof(arg_preview), " arg0=%u", v);
                } else if (atype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t v;
                    dbus_message_iter_get_basic(&iter, &v);
                    snprintf(arg_preview, sizeof(arg_preview), " arg0=%s", v ? "true" : "false");
                }
            }

            char logbuf[512];
            snprintf(logbuf, sizeof(logbuf), "D-Bus signal: %s.%s path=%s sender=%s%s",
                     iface ? iface : "?", member ? member : "?",
                     path ? path : "?", sender ? sender : "?", arg_preview);
            host.log(logbuf);

            /* Track serial numbers from Move's messages */
            if (sender && strstr(sender, ":1.")) {
                uint32_t serial = dbus_message_get_serial(msg);
                if (serial > 0) {
                    pthread_mutex_lock(&move_dbus_serial_mutex);
                    if (serial > move_dbus_serial) {
                        move_dbus_serial = serial;
                    }
                    pthread_mutex_unlock(&move_dbus_serial_mutex);
                }
            }
        }
    }

    if (dbus_message_is_signal(msg, "com.ableton.move.ScreenReader", "text")) {
        DBusMessageIter args;
        if (dbus_message_iter_init(msg, &args)) {
            if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
                const char *text = NULL;
                dbus_message_iter_get_basic(&args, &text);
                shadow_dbus_handle_text(text);
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ============================================================================
 * D-Bus monitoring thread
 * ============================================================================ */

static void *shadow_dbus_thread_func(void *arg)
{
    (void)arg;

    DBusError err;
    dbus_error_init(&err);

    /* Connect to system bus */
    shadow_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        host.log("D-Bus: Failed to connect to system bus");
        dbus_error_free(&err);
        return NULL;
    }

    if (!shadow_dbus_conn) {
        host.log("D-Bus: Connection is NULL");
        return NULL;
    }

    /* Scan existing FDs to find Move's D-Bus socket */
    host.log("D-Bus: Scanning file descriptors for Move's D-Bus socket...");
    for (int fd = 3; fd < 256; fd++) {
        struct sockaddr_un addr;
        socklen_t addr_len = sizeof(addr);

        if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            if (addr.sun_family == AF_UNIX &&
                strstr(addr.sun_path, "dbus") &&
                strstr(addr.sun_path, "system")) {

                /* Get our own FD for comparison */
                int our_fd = -1;
                dbus_connection_get_unix_fd(shadow_dbus_conn, &our_fd);

                if (fd != our_fd) {
                    /* This is Move's FD! Store ORIGINAL for send() hook to recognize */
                    pthread_mutex_lock(&move_dbus_conn_mutex);
                    move_dbus_socket_fd = fd;
                    pthread_mutex_unlock(&move_dbus_conn_mutex);

                    char logbuf[256];
                    snprintf(logbuf, sizeof(logbuf),
                            "D-Bus: *** FOUND Move's D-Bus socket FD %d (path=%s) ***",
                            fd, addr.sun_path);
                    host.log(logbuf);

                    snprintf(logbuf, sizeof(logbuf),
                            "D-Bus: Will intercept writes to FD %d via send() hook", fd);
                    host.log(logbuf);

                    break;
                }
            }
        }
    }

    /* Subscribe to ALL signals for discovery (tempo detection, etc.)
     * NOTE: We explicitly DON'T subscribe to com.ableton.move.ScreenReader
     * because stock Move's web server treats that as a competing client
     * and shows "single window" error. We only SEND to that interface. */
    const char *rule_all = "type='signal'";
    dbus_bus_add_match(shadow_dbus_conn, rule_all, &err);
    dbus_connection_flush(shadow_dbus_conn);

    /* Also try to eavesdrop on WebServiceAuthentication method calls (PIN flow) */
    if (!dbus_error_is_set(&err)) {
        const char *rule_auth = "type='method_call',interface='com.ableton.move.WebServiceAuthentication'";
        dbus_bus_add_match(shadow_dbus_conn, rule_auth, &err);
        if (dbus_error_is_set(&err)) {
            host.log("D-Bus: Auth eavesdrop match failed (expected - may need display-based PIN detection)");
            dbus_error_free(&err);
        } else {
            host.log("D-Bus: Auth eavesdrop match added - will monitor setSecret calls");
            dbus_connection_flush(shadow_dbus_conn);
        }
    }

    if (dbus_error_is_set(&err)) {
        host.log("D-Bus: Failed to add match rule");
        dbus_error_free(&err);
        return NULL;
    }

    /* Add message filter */
    if (!dbus_connection_add_filter(shadow_dbus_conn, shadow_dbus_filter, NULL, NULL)) {
        host.log("D-Bus: Failed to add filter");
        return NULL;
    }

    host.log("D-Bus: Connected and listening for screenreader signals");

    /* Send test announcements via the new shadow buffer architecture.
     * These are queued and injected via send() hook with coordinated serial numbers. */
    send_screenreader_announcement("Move Anything Screen Reader Test");
    sleep(1);
    send_screenreader_announcement("Screen Reader Active");

    /* Main loop - process D-Bus messages */
    while (shadow_dbus_running) {
        /* Non-blocking read with timeout */
        dbus_connection_read_write(shadow_dbus_conn, 100);  /* 100ms timeout */

        /* Dispatch any pending messages */
        while (dbus_connection_dispatch(shadow_dbus_conn) == DBUS_DISPATCH_DATA_REMAINS) {
            /* Keep dispatching */
        }
    }

    host.log("D-Bus: Thread exiting");
    return NULL;
}

/* ============================================================================
 * Start / stop D-Bus monitoring
 * ============================================================================ */

void shadow_dbus_start(void)
{
    if (shadow_dbus_running) return;

    shadow_dbus_running = 1;
    if (pthread_create(&shadow_dbus_thread, NULL, shadow_dbus_thread_func, NULL) != 0) {
        host.log("D-Bus: Failed to create thread");
        shadow_dbus_running = 0;
    }
}

void shadow_dbus_stop(void)
{
    if (!shadow_dbus_running) return;

    shadow_dbus_running = 0;
    pthread_join(shadow_dbus_thread, NULL);

    if (shadow_dbus_conn) {
        dbus_connection_unref(shadow_dbus_conn);
        shadow_dbus_conn = NULL;
    }
}

#else /* !ENABLE_SCREEN_READER */

/* Screen reader disabled at build time: no-op stubs */

float shadow_parse_volume_db(const char *text)
{
    (void)text;
    return -1.0f;
}

void shadow_inject_pending_announcements(void)
{
}

void send_screenreader_announcement(const char *text)
{
    (void)text;
}

void dbus_on_connect(int sockfd, const char *sun_path)
{
    (void)sockfd;
    (void)sun_path;
}

int dbus_on_send(int sockfd, const void *buf, size_t len, int flags,
                 ssize_t (*real_send)(int, const void *, size_t, int),
                 ssize_t *result_out)
{
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)real_send;
    (void)result_out;
    return 0;  /* Not D-Bus, pass through */
}

void dbus_on_sd_bus_default(void *bus_ptr)
{
    (void)bus_ptr;
}

void dbus_on_sd_bus_start(void *bus_ptr)
{
    (void)bus_ptr;
}

void shadow_dbus_start(void)
{
}

void shadow_dbus_stop(void)
{
}

#endif /* ENABLE_SCREEN_READER */

/* ============================================================================
 * Initialization (unconditional)
 * ============================================================================ */

void dbus_init(const dbus_host_t *h)
{
    host = *h;
    dbus_initialized = 1;
}
