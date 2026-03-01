/* shadow_pin_scanner.c - PIN display scanner for screen reader
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "shadow_pin_scanner.h"

/* ============================================================================
 * Static host callbacks
 * ============================================================================ */

static pin_scanner_host_t host;
static int pin_scanner_initialized = 0;

/* ============================================================================
 * PIN scanner state machine
 * ============================================================================ */

#define PIN_STATE_IDLE     0
#define PIN_STATE_WAITING  1  /* Waiting for PIN to render (~500ms) */
#define PIN_STATE_SCANNING 2  /* Scanning display for digits */
#define PIN_STATE_COOLDOWN 3  /* PIN spoken, cooling down before accepting new */

static int pin_state = PIN_STATE_IDLE;
static uint64_t pin_state_entered_ms = 0;
static char pin_last_spoken[8] = {0};  /* Last PIN we spoke, to avoid repeats */

/* File-scope display buffer for PIN scanning, accumulated from slices */
static uint8_t pin_display_buf[DISPLAY_BUFFER_SIZE];
static int pin_display_slices_seen[6] = {0};
static int pin_display_complete = 0;

/* ============================================================================
 * Init
 * ============================================================================ */

void pin_scanner_init(const pin_scanner_host_t *h) {
    host = *h;
    pin_state = PIN_STATE_IDLE;
    pin_state_entered_ms = 0;
    pin_last_spoken[0] = '\0';
    pin_display_complete = 0;
    memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));
    pin_scanner_initialized = 1;
}

/* ============================================================================
 * Slice accumulation
 * ============================================================================ */

void pin_accumulate_slice(int idx, const uint8_t *data, int bytes)
{
    if (idx < 0 || idx >= 6) return;
    memcpy(pin_display_buf + idx * 172, data, bytes);
    pin_display_slices_seen[idx] = 1;

    /* Check if all slices received */
    int all = 1;
    for (int i = 0; i < 6; i++) {
        if (!pin_display_slices_seen[i]) { all = 0; break; }
    }
    if (all) {
        pin_display_complete = 1;
        memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));

        /* File-triggered display dump: touch /tmp/dump_display to capture */
        if (access("/tmp/dump_display", F_OK) == 0) {
            unlink("/tmp/dump_display");
            FILE *f = fopen("/tmp/pin_display.bin", "w");
            if (f) {
                fwrite(pin_display_buf, 1, 1024, f);
                fclose(f);
                if (host.log) host.log("PIN: display buffer dumped to /tmp/pin_display.bin");
            }
        }
    }
}

/* ============================================================================
 * Digit recognition
 * ============================================================================ */

/* Digit template hash table.
 * Each digit (0-9) maps to a polynomial hash of its column bytes in pages 3-4. */
static uint32_t pin_digit_hashes[10] = {
    0x8abc24d1,   /* 0 */
    0xa8721e5e,   /* 1 */
    0x3eeaf9a2,   /* 2 */
    0xb680019e,   /* 3 */
    0xc751c4ad,   /* 4 */
    0xf7a9c384,   /* 5 */
    0xc9805ffb,   /* 6 */
    0x538e156e,   /* 7 */
    0xf35f5d11,   /* 8 */
    0xa061c01d,   /* 9 */
};

/* Compute hash for a digit group spanning columns [start, end) in pages 3-4 */
static uint32_t pin_digit_hash(const uint8_t *display, int start, int end)
{
    uint32_t hash = 5381;
    for (int c = start; c < end; c++) {
        hash = hash * 33 + display[3 * 128 + c];
        hash = hash * 33 + display[4 * 128 + c];
    }
    return hash;
}

/* Check if the display looks like a PIN screen:
 * Pages 3-4 have content, other pages are mostly blank. */
static int pin_display_is_pin_screen(const uint8_t *display)
{
    /* Count non-zero bytes in pages 3-4 */
    int active = 0;
    for (int i = 3 * 128; i < 5 * 128; i++) {
        if (display[i]) active++;
    }
    if (active < 10) return 0;  /* Too few lit pixels */

    /* Check that other pages are mostly blank */
    int other = 0;
    for (int page = 0; page < 8; page++) {
        if (page == 3 || page == 4) continue;
        for (int col = 0; col < 128; col++) {
            if (display[page * 128 + col]) other++;
        }
    }
    return other < 20;  /* Allow a few stray pixels */
}

/* Extract digits from display and build TTS string.
 * Returns 1 on success with pin_text and raw_digits filled, 0 on failure.
 * raw_digits must be at least 7 bytes (6 digits + NUL). */
static int pin_extract_digits(const uint8_t *display, char *pin_text, int text_len, char *raw_digits)
{
    char logbuf[512];

    if (!pin_display_is_pin_screen(display)) {
        if (host.log) host.log("PIN: display doesn't look like PIN screen");
        return 0;
    }

    /* Segment digits: find groups of consecutive non-zero columns in pages 3-4 */
    typedef struct { int start; int end; } digit_span_t;
    digit_span_t spans[8];
    int span_count = 0;
    int in_digit = 0;
    int digit_start = 0;

    for (int col = 0; col < 128; col++) {
        int has_content = display[3 * 128 + col] || display[4 * 128 + col];
        if (has_content && !in_digit) {
            digit_start = col;
            in_digit = 1;
        } else if (!has_content && in_digit) {
            if (span_count < 8) {
                spans[span_count].start = digit_start;
                spans[span_count].end = col;
                span_count++;
            }
            in_digit = 0;
        }
    }
    if (in_digit && span_count < 8) {
        spans[span_count].start = digit_start;
        spans[span_count].end = 128;
        span_count++;
    }

    /* Expect exactly 6 digit groups */
    if (span_count != 6) {
        snprintf(logbuf, sizeof(logbuf), "PIN: expected 6 digit groups, found %d", span_count);
        if (host.log) host.log(logbuf);
        /* Log digit group info for debugging */
        for (int i = 0; i < span_count; i++) {
            snprintf(logbuf, sizeof(logbuf), "PIN: group %d: cols %d-%d (width %d)",
                     i, spans[i].start, spans[i].end,
                     spans[i].end - spans[i].start);
            if (host.log) host.log(logbuf);
        }
        return 0;
    }

    /* Match each digit group */
    char digits[7] = {0};
    int all_matched = 1;

    for (int i = 0; i < 6; i++) {
        uint32_t hash = pin_digit_hash(display, spans[i].start, spans[i].end);
        int matched = -1;

        /* Look up in hash table */
        for (int d = 0; d < 10; d++) {
            if (pin_digit_hashes[d] != 0 && pin_digit_hashes[d] == hash) {
                matched = d;
                break;
            }
        }

        if (matched >= 0) {
            digits[i] = '0' + matched;
        } else {
            digits[i] = '?';
            all_matched = 0;

            /* Log the hash and raw column data for template creation */
            snprintf(logbuf, sizeof(logbuf), "PIN: digit %d (cols %d-%d) hash=0x%08x UNMATCHED",
                     i, spans[i].start, spans[i].end, hash);
            if (host.log) host.log(logbuf);

            /* Dump column bytes for pages 3-4 */
            int pos = 0;
            pos += snprintf(logbuf + pos, sizeof(logbuf) - pos, "PIN: digit %d p3:", i);
            for (int c = spans[i].start; c < spans[i].end && pos < 300; c++) {
                pos += snprintf(logbuf + pos, sizeof(logbuf) - pos,
                               " %02x", display[3 * 128 + c]);
            }
            pos += snprintf(logbuf + pos, sizeof(logbuf) - pos, " p4:");
            for (int c = spans[i].start; c < spans[i].end && pos < 480; c++) {
                pos += snprintf(logbuf + pos, sizeof(logbuf) - pos,
                               " %02x", display[4 * 128 + c]);
            }
            if (host.log) host.log(logbuf);
        }
    }
    digits[6] = '\0';

    /* Copy raw digits to output parameter for dedup */
    memcpy(raw_digits, digits, 7);

    if (!all_matched) {
        snprintf(logbuf, sizeof(logbuf), "PIN: some digits unmatched, raw string: %s", digits);
        if (host.log) host.log(logbuf);
        /* Still try to speak what we have - the user may recognize partial info */
    }

    /* Build TTS string: repeat 2 times with a pause. */
    int n = 0;
    for (int rep = 0; rep < 2; rep++) {
        if (rep > 0) n += snprintf(pin_text + n, text_len - n, ".... ");
        n += snprintf(pin_text + n, text_len - n, "Pairing pin displayed: ");
        for (int i = 0; i < 6 && n < text_len - 4; i++) {
            if (i > 0) n += snprintf(pin_text + n, text_len - n, ", ");
            n += snprintf(pin_text + n, text_len - n, "%c", digits[i]);
        }
        n += snprintf(pin_text + n, text_len - n, ". ");
    }

    snprintf(logbuf, sizeof(logbuf), "PIN: extracted digits: %s", digits);
    if (host.log) host.log(logbuf);
    return 1;
}

/* ============================================================================
 * Main PIN scanner state machine
 * ============================================================================ */

void pin_check_and_speak(void)
{
    shadow_control_t *ctrl = host.shadow_control ? *host.shadow_control : NULL;
    if (!ctrl) return;

    /* Get current time */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

    uint8_t challenge = ctrl->pin_challenge_active;

    /* If challenge-response submitted (2), cancel any active scan */
    if (challenge == 2 && pin_state != PIN_STATE_IDLE && pin_state != PIN_STATE_COOLDOWN) {
        if (host.log) host.log("PIN: challenge-response submitted, cancelling scan");
        pin_state = PIN_STATE_COOLDOWN;
        pin_state_entered_ms = now_ms;
        return;
    }

    /* State machine */
    switch (pin_state) {
    case PIN_STATE_IDLE:
        /* Only trigger on challenge=1 from IDLE */
        if (challenge == 1) {
            pin_state = PIN_STATE_WAITING;
            pin_state_entered_ms = now_ms;
            pin_display_complete = 0;
            memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));
            if (host.log) host.log("PIN: challenge detected, waiting for display render");
        }
        break;

    case PIN_STATE_WAITING:
        /* Wait 500ms for the PIN to render on the display */
        if (now_ms - pin_state_entered_ms > 500) {
            pin_state = PIN_STATE_SCANNING;
            pin_display_complete = 0;
            memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));
            if (host.log) host.log("PIN: entering scan mode");
        }
        break;

    case PIN_STATE_SCANNING:
        if (pin_display_complete) {
            char pin_text[512];
            char raw_digits[8] = {0};
            if (pin_extract_digits(pin_display_buf, pin_text, sizeof(pin_text), raw_digits)) {
                if (strcmp(raw_digits, pin_last_spoken) == 0) {
                    /* Same PIN as last time — skip to avoid repeating */
                    pin_state = PIN_STATE_COOLDOWN;
                    pin_state_entered_ms = now_ms;
                } else {
                    { char lb[128]; snprintf(lb, sizeof(lb), "PIN: speaking '%s'", pin_text); if (host.log) host.log(lb); }
                    if (host.tts_speak) host.tts_speak(pin_text);
                    strncpy(pin_last_spoken, raw_digits, sizeof(pin_last_spoken) - 1);
                    pin_state = PIN_STATE_COOLDOWN;
                    pin_state_entered_ms = now_ms;
                }
            } else {
                /* Try again on next full frame */
                pin_display_complete = 0;
            }
        }
        /* Timeout after 10 seconds of scanning */
        if (now_ms - pin_state_entered_ms > 10000) {
            if (host.log) host.log("PIN: scan timeout");
            pin_state = PIN_STATE_COOLDOWN;
            pin_state_entered_ms = now_ms;
        }
        break;

    case PIN_STATE_COOLDOWN:
        /* Wait for the challenge flag to clear naturally, then return to IDLE. */
        if (challenge == 0 || challenge == 2) {
            pin_state = PIN_STATE_IDLE;
            pin_last_spoken[0] = '\0';  /* Clear dedup so new session can repeat */
            if (host.log) host.log("PIN: challenge cleared, returning to idle");
        } else if (now_ms - pin_state_entered_ms > 5000) {
            pin_state = PIN_STATE_IDLE;
            if (host.log) host.log("PIN: cooldown timeout, returning to idle");
        }
        break;

    default:
        break;
    }
}
