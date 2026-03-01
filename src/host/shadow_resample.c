/* shadow_resample.c - Native resample bridge
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "shadow_resample.h"
#include "shadow_chain_mgmt.h"  /* for shadow_master_fx_chain_active() */

/* ============================================================================
 * Static host callbacks
 * ============================================================================ */

static resample_host_t host;
static int resample_initialized = 0;

/* ============================================================================
 * Global definitions
 * ============================================================================ */

volatile native_resample_bridge_mode_t native_resample_bridge_mode = NATIVE_RESAMPLE_BRIDGE_OFF;
volatile native_sampler_source_t native_sampler_source = NATIVE_SAMPLER_SOURCE_UNKNOWN;
volatile native_sampler_source_t native_sampler_source_last_known = NATIVE_SAMPLER_SOURCE_UNKNOWN;
volatile int link_audio_routing_enabled = 0;

/* Snapshot and component buffers */
int16_t native_total_mix_snapshot[FRAMES_PER_BLOCK * 2];
volatile int native_total_mix_snapshot_valid = 0;
int16_t native_bridge_move_component[FRAMES_PER_BLOCK * 2];
int16_t native_bridge_me_component[FRAMES_PER_BLOCK * 2];
float native_bridge_capture_mv = 1.0f;
volatile int native_bridge_split_valid = 0;

/* Overwrite makeup diagnostics */
volatile float native_bridge_makeup_desired_gain = 1.0f;
volatile float native_bridge_makeup_applied_gain = 1.0f;
volatile int native_bridge_makeup_limited = 0;

/* ============================================================================
 * Local utility
 * ============================================================================ */

static void str_to_lower(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < dst_size) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = c + ('a' - 'A');
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

/* ============================================================================
 * Init
 * ============================================================================ */

void resample_init(const resample_host_t *h) {
    host = *h;
    native_resample_bridge_mode = NATIVE_RESAMPLE_BRIDGE_OFF;
    native_sampler_source = NATIVE_SAMPLER_SOURCE_UNKNOWN;
    native_sampler_source_last_known = NATIVE_SAMPLER_SOURCE_UNKNOWN;
    link_audio_routing_enabled = 0;
    native_total_mix_snapshot_valid = 0;
    native_bridge_split_valid = 0;
    native_bridge_capture_mv = 1.0f;
    native_bridge_makeup_desired_gain = 1.0f;
    native_bridge_makeup_applied_gain = 1.0f;
    native_bridge_makeup_limited = 0;
    resample_initialized = 1;
}

/* ============================================================================
 * Name helpers
 * ============================================================================ */

const char *native_sampler_source_name(native_sampler_source_t src)
{
    switch (src) {
        case NATIVE_SAMPLER_SOURCE_RESAMPLING: return "resampling";
        case NATIVE_SAMPLER_SOURCE_LINE_IN: return "line-in";
        case NATIVE_SAMPLER_SOURCE_MIC_IN: return "mic-in";
        case NATIVE_SAMPLER_SOURCE_USB_C_IN: return "usb-c-in";
        case NATIVE_SAMPLER_SOURCE_UNKNOWN:
        default: return "unknown";
    }
}

const char *native_resample_bridge_mode_name(native_resample_bridge_mode_t mode)
{
    switch (mode) {
        case NATIVE_RESAMPLE_BRIDGE_OFF: return "off";
        case NATIVE_RESAMPLE_BRIDGE_OVERWRITE: return "overwrite";
        case NATIVE_RESAMPLE_BRIDGE_MIX:
        default: return "mix";
    }
}

/* ============================================================================
 * Mode parsing and config loading
 * ============================================================================ */

native_resample_bridge_mode_t native_resample_bridge_mode_from_text(const char *text)
{
    if (!text || !text[0]) return NATIVE_RESAMPLE_BRIDGE_OFF;

    char lower[64];
    str_to_lower(lower, sizeof(lower), text);

    if (strcmp(lower, "0") == 0 || strcmp(lower, "off") == 0) {
        return NATIVE_RESAMPLE_BRIDGE_OFF;
    }
    if (strcmp(lower, "2") == 0 ||
        strcmp(lower, "overwrite") == 0 ||
        strcmp(lower, "replace") == 0) {
        return NATIVE_RESAMPLE_BRIDGE_OVERWRITE;
    }
    if (strcmp(lower, "1") == 0 || strcmp(lower, "mix") == 0) {
        return NATIVE_RESAMPLE_BRIDGE_MIX;
    }

    return NATIVE_RESAMPLE_BRIDGE_OFF;
}

void native_resample_bridge_load_mode_from_shadow_config(void)
{
    const char *config_path = "/data/UserData/move-anything/shadow_config.json";
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }

    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    size_t nread = fread(json, 1, (size_t)size, f);
    fclose(f);
    json[nread] = '\0';

    char *mode_key = strstr(json, "\"resample_bridge_mode\"");
    if (mode_key) {
        char *colon = strchr(mode_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t' || *colon == '"') colon++;
            char token[32];
            size_t idx = 0;
            while (*colon && idx + 1 < sizeof(token)) {
                char c = *colon;
                if (c == '"' || c == ',' || c == '}' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                    break;
                token[idx++] = c;
                colon++;
            }
            token[idx] = '\0';
            if (token[0]) {
                native_resample_bridge_mode_t new_mode = native_resample_bridge_mode_from_text(token);
                native_resample_bridge_mode = new_mode;
                if (host.log) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Native resample bridge mode: %s (from config)",
                             native_resample_bridge_mode_name(new_mode));
                    host.log(msg);
                }
            }
        }
    }

    /* Load Link Audio routing setting */
    char *la_key = strstr(json, "\"link_audio_routing\"");
    if (la_key) {
        char *colon = strchr(la_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0 || *colon == '1') {
                link_audio_routing_enabled = 1;
            } else {
                link_audio_routing_enabled = 0;
            }
            if (host.log) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Link Audio routing: %s (from config)",
                         link_audio_routing_enabled ? "ON" : "OFF");
                host.log(msg);
            }
        }
    }

    free(json);
}

/* ============================================================================
 * Source tracking
 * ============================================================================ */

static native_sampler_source_t native_sampler_source_from_text(const char *text)
{
    if (!text || !text[0]) return NATIVE_SAMPLER_SOURCE_UNKNOWN;

    char lower[256];
    str_to_lower(lower, sizeof(lower), text);

    if (strstr(lower, "resampl")) return NATIVE_SAMPLER_SOURCE_RESAMPLING;
    if (strstr(lower, "line in") || strstr(lower, "line-in") || strstr(lower, "linein"))
        return NATIVE_SAMPLER_SOURCE_LINE_IN;
    if (strstr(lower, "usb-c") || strstr(lower, "usb c") || strstr(lower, "usbc"))
        return NATIVE_SAMPLER_SOURCE_USB_C_IN;
    if (strstr(lower, "mic") || strstr(lower, "microphone"))
        return NATIVE_SAMPLER_SOURCE_MIC_IN;

    return NATIVE_SAMPLER_SOURCE_UNKNOWN;
}

void native_sampler_update_from_dbus_text(const char *text)
{
    native_sampler_source_t parsed = native_sampler_source_from_text(text);
    if (parsed == NATIVE_SAMPLER_SOURCE_UNKNOWN) return;

    if (parsed != native_sampler_source) {
        if (host.log) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Native sampler source: %s (from \"%s\")",
                     native_sampler_source_name(parsed), text);
            host.log(msg);
        }
        native_sampler_source = parsed;
        native_sampler_source_last_known = parsed;
    }
}

/* ============================================================================
 * Snapshot capture
 * ============================================================================ */

void native_capture_total_mix_snapshot_from_buffer(const int16_t *src)
{
    if (!src) return;
    memcpy(native_total_mix_snapshot, src, RESAMPLE_AUDIO_BUFFER_SIZE);

    __sync_synchronize();
    native_total_mix_snapshot_valid = 1;
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

static int native_resample_diag_is_enabled(void)
{
    static int cached = 0;
    static int check_counter = 0;
    static int last_logged = -1;

    if (check_counter++ % 200 == 0) {
        cached = (access("/data/UserData/move-anything/native_resample_diag_on", F_OK) == 0);
        if (cached != last_logged) {
            if (host.log) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Native bridge diag: %s",
                         cached ? "enabled" : "disabled");
                host.log(msg);
            }
            last_logged = cached;
        }
    }
    return cached;
}

void native_compute_audio_metrics(const int16_t *buf, native_audio_metrics_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    if (!buf) return;

    double sum_l = 0.0;
    double sum_r = 0.0;
    double sum_mid = 0.0;
    double sum_side = 0.0;
    double sum_low_l = 0.0;
    double sum_low_r = 0.0;
    float lp_l = 0.0f;
    float lp_r = 0.0f;
    const float alpha = 0.028f;  /* ~200 Hz one-pole lowpass at 44.1 kHz */

    for (int i = 0; i < FRAMES_PER_BLOCK; i++) {
        float l = (float)buf[i * 2] / 32768.0f;
        float r = (float)buf[i * 2 + 1] / 32768.0f;
        float mid = 0.5f * (l + r);
        float side = 0.5f * (l - r);

        sum_l += (double)l * (double)l;
        sum_r += (double)r * (double)r;
        sum_mid += (double)mid * (double)mid;
        sum_side += (double)side * (double)side;

        lp_l += alpha * (l - lp_l);
        lp_r += alpha * (r - lp_r);
        sum_low_l += (double)lp_l * (double)lp_l;
        sum_low_r += (double)lp_r * (double)lp_r;
    }

    const float inv_n = 1.0f / (float)FRAMES_PER_BLOCK;
    m->rms_l = sqrtf((float)sum_l * inv_n);
    m->rms_r = sqrtf((float)sum_r * inv_n);
    m->rms_mid = sqrtf((float)sum_mid * inv_n);
    m->rms_side = sqrtf((float)sum_side * inv_n);
    m->rms_low_l = sqrtf((float)sum_low_l * inv_n);
    m->rms_low_r = sqrtf((float)sum_low_r * inv_n);
}

static void native_resample_diag_log_skip(native_resample_bridge_mode_t mode, const char *reason)
{
    static int skip_counter = 0;
    if (!native_resample_diag_is_enabled()) return;
    if (skip_counter++ % 200 != 0) return;

    if (host.log) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Native bridge diag: skip reason=%s mode=%s src=%s last=%s",
                 reason ? reason : "unknown",
                 native_resample_bridge_mode_name(mode),
                 native_sampler_source_name(native_sampler_source),
                 native_sampler_source_name(native_sampler_source_last_known));
        host.log(msg);
    }
}

static void native_resample_diag_log_apply(native_resample_bridge_mode_t mode,
                                           const int16_t *src,
                                           const int16_t *dst)
{
    static int apply_counter = 0;
    if (!native_resample_diag_is_enabled()) return;
    if (apply_counter++ % 200 != 0) return;

    native_audio_metrics_t src_m;
    native_audio_metrics_t dst_m;
    native_compute_audio_metrics(src, &src_m);
    native_compute_audio_metrics(dst, &dst_m);

    int overwrite_diff = -1;
    if (mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE && src && dst) {
        overwrite_diff = 0;
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            if (src[i] != dst[i]) overwrite_diff++;
        }
    }

    float src_side_ratio = src_m.rms_side / (src_m.rms_mid + 1e-9f);
    float dst_side_ratio = dst_m.rms_side / (dst_m.rms_mid + 1e-9f);

    if (host.log) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Native bridge diag: apply mode=%s src=%s last=%s mv=%.3f split=%d mfx=%d makeup=(%.2fx->%.2fx lim=%d) tap=post-fx-premaster src_rms=(%.4f,%.4f) dst_rms=(%.4f,%.4f) src_low=(%.4f,%.4f) dst_low=(%.4f,%.4f) side_ratio=(%.4f->%.4f) overwrite_diff=%d",
                 native_resample_bridge_mode_name(mode),
                 native_sampler_source_name(native_sampler_source),
                 native_sampler_source_name(native_sampler_source_last_known),
                 (double)(host.shadow_master_volume ? *host.shadow_master_volume : 0.0f),
                 (int)native_bridge_split_valid,
                 shadow_master_fx_chain_active(),
                 native_bridge_makeup_desired_gain,
                 native_bridge_makeup_applied_gain,
                 (int)native_bridge_makeup_limited,
                 src_m.rms_l, src_m.rms_r,
                 dst_m.rms_l, dst_m.rms_r,
                 src_m.rms_low_l, src_m.rms_low_r,
                 dst_m.rms_low_l, dst_m.rms_low_r,
                 src_side_ratio, dst_side_ratio,
                 overwrite_diff);
        host.log(msg);
    }
}

/* ============================================================================
 * Source gating and apply
 * ============================================================================ */

int native_resample_bridge_source_allows_apply(native_resample_bridge_mode_t mode)
{
    if (mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE) return 1;

    native_sampler_source_t src = native_sampler_source;

    if (src == NATIVE_SAMPLER_SOURCE_MIC_IN) return 0;
    if (src == NATIVE_SAMPLER_SOURCE_USB_C_IN) return 0;
    return 1;
}

static int16_t clamp_i16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Overwrite path with component-based compensation. */
static void native_resample_bridge_apply_overwrite_makeup(const int16_t *src,
                                                          int16_t *dst,
                                                          size_t samples)
{
    if (!src || !dst || samples == 0) return;

    float mv = native_bridge_capture_mv;
    if (mv < 0.001f) {
        memcpy(dst, src, samples * sizeof(int16_t));
        native_bridge_makeup_desired_gain = 0.0f;
        native_bridge_makeup_applied_gain = 1.0f;
        native_bridge_makeup_limited = 0;
        return;
    }

    float inv_mv = 1.0f / mv;
    float max_makeup = 20.0f;

    if (!shadow_master_fx_chain_active() && native_bridge_split_valid) {
        float native_gain = (inv_mv < max_makeup) ? inv_mv : max_makeup;
        int limiter_hit = 0;

        for (size_t i = 0; i < samples; i++) {
            float move_scaled = (float)native_bridge_move_component[i] * native_gain;
            float me = (float)native_bridge_me_component[i];
            float sum = move_scaled + me;
            if (sum > 32767.0f) { sum = 32767.0f; limiter_hit = 1; }
            if (sum < -32768.0f) { sum = -32768.0f; limiter_hit = 1; }
            dst[i] = (int16_t)lroundf(sum);
        }

        native_bridge_makeup_desired_gain = inv_mv;
        native_bridge_makeup_applied_gain = native_gain;
        native_bridge_makeup_limited = limiter_hit;
    } else if (shadow_master_fx_chain_active()) {
        memcpy(dst, src, samples * sizeof(int16_t));
        native_bridge_makeup_desired_gain = 1.0f;
        native_bridge_makeup_applied_gain = 1.0f;
        native_bridge_makeup_limited = 0;
    } else {
        memcpy(dst, src, samples * sizeof(int16_t));
        native_bridge_makeup_desired_gain = 1.0f;
        native_bridge_makeup_applied_gain = 1.0f;
        native_bridge_makeup_limited = 0;
    }
}

void native_resample_bridge_apply(void)
{
    unsigned char *mmap_addr = host.global_mmap_addr ? *host.global_mmap_addr : NULL;
    if (!mmap_addr || !native_total_mix_snapshot_valid) return;

    native_resample_bridge_mode_t mode = native_resample_bridge_mode;
    if (mode == NATIVE_RESAMPLE_BRIDGE_OFF) {
        native_resample_diag_log_skip(mode, "mode_off");
        return;
    }

    if (!native_resample_bridge_source_allows_apply(mode)) {
        native_resample_diag_log_skip(mode, "source_blocked");
        return;
    }

    int16_t *dst = (int16_t *)(mmap_addr + RESAMPLE_AUDIO_IN_OFFSET);
    if (mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE) {
        int16_t compensated_snapshot[FRAMES_PER_BLOCK * 2];
        native_resample_bridge_apply_overwrite_makeup(
            native_total_mix_snapshot,
            compensated_snapshot,
            FRAMES_PER_BLOCK * 2
        );
        memcpy(dst, compensated_snapshot, RESAMPLE_AUDIO_BUFFER_SIZE);
        native_resample_diag_log_apply(mode, native_total_mix_snapshot, dst);
        return;
    }

    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)dst[i] + (int32_t)native_total_mix_snapshot[i];
        dst[i] = clamp_i16(mixed);
    }
    native_resample_diag_log_apply(mode, native_total_mix_snapshot, dst);
}
