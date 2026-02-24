/* shadow_overlay.c - Display overlay drawing and state sync
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "shadow_overlay.h"
#include "shadow_sampler.h"
#include "shadow_set_pages.h"

/* ============================================================================
 * Static host callbacks
 * ============================================================================ */

static overlay_host_t host;
static int overlay_initialized = 0;

/* ============================================================================
 * Global definitions - shift+knob overlay state
 * ============================================================================ */

int shift_knob_overlay_active = 0;
int shift_knob_overlay_timeout = 0;
int shift_knob_overlay_slot = 0;
int shift_knob_overlay_knob = 0;
char shift_knob_overlay_patch[64] = "";
char shift_knob_overlay_param[64] = "";
char shift_knob_overlay_value[32] = "";

/* ============================================================================
 * Init
 * ============================================================================ */

void overlay_init(const overlay_host_t *h) {
    host = *h;
    shift_knob_overlay_active = 0;
    shift_knob_overlay_timeout = 0;
    overlay_initialized = 1;
}

/* ============================================================================
 * Font data - Minimal 5x7 font for overlay text (ASCII 32-127)
 * ============================================================================ */

const uint8_t overlay_font_5x7[96][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  32   */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /*  33 ! */
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, /*  34 " */
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /*  35 # */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /*  36 $ */
    {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}, /*  37 % */
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /*  38 & */
    {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}, /*  39 ' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /*  40 ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /*  41 ) */
    {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}, /*  42 * */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /*  43 + */
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, /*  44 , */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /*  45 - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /*  46 . */
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, /*  47 / */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /*  48 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /*  49 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /*  50 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /*  51 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /*  52 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /*  53 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /*  54 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /*  55 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /*  56 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /*  57 9 */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /*  58 : */
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, /*  59 ; */
    {0x01,0x02,0x04,0x08,0x04,0x02,0x01}, /*  60 < */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /*  61 = */
    {0x10,0x08,0x04,0x02,0x04,0x08,0x10}, /*  62 > */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /*  63 ? */
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /*  64 @ */
    {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, /*  65 A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /*  66 B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /*  67 C */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /*  68 D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /*  69 E */
    {0x1F,0x10,0x10,0x1C,0x10,0x10,0x10}, /*  70 F */
    {0x0E,0x11,0x10,0x10,0x13,0x11,0x0E}, /*  71 G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /*  72 H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /*  73 I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /*  74 J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /*  75 K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /*  76 L */
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11}, /*  77 M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /*  78 N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /*  79 O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /*  80 P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /*  81 Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /*  82 R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /*  83 S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /*  84 T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /*  85 U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /*  86 V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /*  87 W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /*  88 X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /*  89 Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /*  90 Z */
    {0x07,0x04,0x04,0x04,0x04,0x04,0x07}, /*  91 [ */
    {0x10,0x10,0x08,0x04,0x02,0x01,0x01}, /*  92 \ */
    {0x1C,0x04,0x04,0x04,0x04,0x04,0x1C}, /*  93 ] */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /*  94 ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /*  95 _ */
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, /*  96 ` */
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /*  97 a */
    {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, /*  98 b */
    {0x00,0x00,0x0E,0x10,0x10,0x10,0x0E}, /*  99 c */
    {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, /* 100 d */
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* 101 e */
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, /* 102 f */
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x06}, /* 103 g */
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, /* 104 h */
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* 105 i */
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* 106 j */
    {0x08,0x08,0x09,0x0A,0x0C,0x0A,0x09}, /* 107 k */
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 108 l */
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, /* 109 m */
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, /* 110 n */
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* 111 o */
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* 112 p */
    {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}, /* 113 q */
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* 114 r */
    {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, /* 115 s */
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, /* 116 t */
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, /* 117 u */
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* 118 v */
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* 119 w */
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* 120 x */
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* 121 y */
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* 122 z */
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* 123 { */
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* 124 | */
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* 125 } */
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, /* 126 ~ */
    {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, /* 127 DEL */
};

/* ============================================================================
 * Drawing primitives
 * ============================================================================ */

void overlay_draw_char(uint8_t *buf, int x, int y, char c, int color)
{
    if (c < 32 || c > 127) c = '?';
    const uint8_t *glyph = overlay_font_5x7[c - 32];

    for (int row = 0; row < 7; row++) {
        int screen_y = y + row;
        if (screen_y < 0 || screen_y >= 64) continue;

        int page = screen_y / 8;
        int bit = screen_y % 8;

        for (int col = 0; col < 5; col++) {
            int screen_x = x + col;
            if (screen_x < 0 || screen_x >= 128) continue;

            int byte_idx = page * 128 + screen_x;
            int pixel_on = (glyph[row] >> (4 - col)) & 1;

            if (pixel_on) {
                if (color)
                    buf[byte_idx] |= (1 << bit);   /* Set pixel */
                else
                    buf[byte_idx] &= ~(1 << bit);  /* Clear pixel */
            }
        }
    }
}

void overlay_draw_string(uint8_t *buf, int x, int y, const char *str, int color)
{
    while (*str) {
        overlay_draw_char(buf, x, y, *str, color);
        x += 6;  /* 5 pixel width + 1 pixel spacing */
        str++;
    }
}

void overlay_fill_rect(uint8_t *buf, int x, int y, int w, int h, int color)
{
    for (int row = y; row < y + h && row < 64; row++) {
        if (row < 0) continue;
        int page = row / 8;
        int bit = row % 8;

        for (int col = x; col < x + w && col < 128; col++) {
            if (col < 0) continue;
            int byte_idx = page * 128 + col;

            if (color)
                buf[byte_idx] |= (1 << bit);
            else
                buf[byte_idx] &= ~(1 << bit);
        }
    }
}

/* ============================================================================
 * Shift+Knob overlay
 * ============================================================================ */

void overlay_draw_shift_knob(uint8_t *buf)
{
    if (!shift_knob_overlay_active || shift_knob_overlay_timeout <= 0) return;

    /* Box dimensions: 3 lines of text + padding */
    int box_w = 100;
    int box_h = 30;
    int box_x = (128 - box_w) / 2;
    int box_y = (64 - box_h) / 2;

    /* Draw background (black) and border (white) */
    overlay_fill_rect(buf, box_x, box_y, box_w, box_h, 0);
    overlay_fill_rect(buf, box_x, box_y, box_w, 1, 1);           /* Top border */
    overlay_fill_rect(buf, box_x, box_y + box_h - 1, box_w, 1, 1); /* Bottom border */
    overlay_fill_rect(buf, box_x, box_y, 1, box_h, 1);           /* Left border */
    overlay_fill_rect(buf, box_x + box_w - 1, box_y, 1, box_h, 1); /* Right border */

    /* Draw text lines */
    int text_x = box_x + 4;
    int text_y = box_y + 3;

    overlay_draw_string(buf, text_x, text_y, shift_knob_overlay_patch, 1);
    overlay_draw_string(buf, text_x, text_y + 9, shift_knob_overlay_param, 1);
    overlay_draw_string(buf, text_x, text_y + 18, shift_knob_overlay_value, 1);
}

void shift_knob_update_overlay(int slot, int knob_num, uint8_t cc_value)
{
    (void)cc_value;  /* No longer used - we show "Unmapped" instead */
    shadow_control_t *ctrl = host.shadow_control ? *host.shadow_control : NULL;
    uint8_t okm = ctrl ? ctrl->overlay_knobs_mode : 3; /* OVERLAY_KNOBS_NATIVE */
    if (okm == OVERLAY_KNOBS_OFF) return;
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;

    shift_knob_overlay_slot = slot;
    shift_knob_overlay_knob = knob_num;  /* 1-8 */
    shift_knob_overlay_active = 1;
    shift_knob_overlay_timeout = SHIFT_KNOB_OVERLAY_FRAMES;

    /* Copy slot name with "S#: " prefix */
    const char *name = host.chain_slots[slot].patch_name;
    if (name[0] == '\0') {
        snprintf(shift_knob_overlay_patch, sizeof(shift_knob_overlay_patch),
                 "S%d", slot + 1);
    } else {
        snprintf(shift_knob_overlay_patch, sizeof(shift_knob_overlay_patch),
                 "S%d: %s", slot + 1, name);
    }

    /* Query parameter name and value from DSP */
    int mapped = 0;
    const plugin_api_v2_t *pv2 = host.plugin_v2 ? *host.plugin_v2 : NULL;
    if (pv2 && pv2->get_param && host.chain_slots[slot].instance) {
        char key[32];
        char buf[64];
        int len;

        /* Get knob_N_name - if this succeeds, the knob is mapped */
        snprintf(key, sizeof(key), "knob_%d_name", knob_num);
        len = pv2->get_param(host.chain_slots[slot].instance, key, buf, sizeof(buf));
        if (len > 0) {
            mapped = 1;
            buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
            strncpy(shift_knob_overlay_param, buf, sizeof(shift_knob_overlay_param) - 1);
            shift_knob_overlay_param[sizeof(shift_knob_overlay_param) - 1] = '\0';

            /* Get knob_N_value */
            snprintf(key, sizeof(key), "knob_%d_value", knob_num);
            len = pv2->get_param(host.chain_slots[slot].instance, key, buf, sizeof(buf));
            if (len > 0) {
                buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
                strncpy(shift_knob_overlay_value, buf, sizeof(shift_knob_overlay_value) - 1);
                shift_knob_overlay_value[sizeof(shift_knob_overlay_value) - 1] = '\0';
            } else {
                strncpy(shift_knob_overlay_value, "?", sizeof(shift_knob_overlay_value) - 1);
            }
        }
    }

    /* Show "Unmapped" if knob has no mapping */
    if (!mapped) {
        snprintf(shift_knob_overlay_param, sizeof(shift_knob_overlay_param), "Knob %d", knob_num);
        strncpy(shift_knob_overlay_value, "Unmapped", sizeof(shift_knob_overlay_value) - 1);
        shift_knob_overlay_value[sizeof(shift_knob_overlay_value) - 1] = '\0';
    }

    /* Screen reader: announce param and value */
    {
        char sr_buf[192];
        snprintf(sr_buf, sizeof(sr_buf), "%s, %s", shift_knob_overlay_param, shift_knob_overlay_value);
        if (host.announce) host.announce(sr_buf);
    }

    shadow_overlay_sync();
}

/* ============================================================================
 * Set mute state reader
 * ============================================================================ */

int shadow_read_set_mute_states(const char *set_name, int muted_out[4], int soloed_out[4]) {
    memset(muted_out, 0, 4 * sizeof(int));
    memset(soloed_out, 0, 4 * sizeof(int));
    if (!set_name || !set_name[0]) return 0;

    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) return 0;

    char best_path[512] = "";
    time_t best_mtime = 0;
    struct dirent *entry;

    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/%s/Song.abl",
                 SAMPLER_SETS_DIR, entry->d_name, set_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_mtime > best_mtime) {
                best_mtime = st.st_mtime;
                snprintf(best_path, sizeof(best_path), "%s", path);
            }
        }
    }
    closedir(sets_dir);

    if (best_path[0] == '\0') return 0;

    FILE *f = fopen(best_path, "r");
    if (!f) return 0;

    /* Parse by tracking brace depth CHARACTER-BY-CHARACTER */
    int mute_count = 0;
    int solo_count = 0;
    int brace_depth = 0;
    int in_tracks = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f) && (mute_count < 4 || solo_count < 4)) {
        for (char *p = line; *p; p++) {
            if (*p == '{') {
                brace_depth++;
            } else if (*p == '}') {
                brace_depth--;
            } else if (*p == '"') {
                if (!in_tracks && brace_depth == 1 && strncmp(p, "\"tracks\"", 8) == 0) {
                    in_tracks = 1;
                    p += 7;
                    continue;
                }
                if (in_tracks && brace_depth == 3 && strncmp(p, "\"speakerOn\"", 11) == 0 && mute_count < 4) {
                    char *val = strchr(p + 11, ':');
                    if (val) {
                        muted_out[mute_count] = strstr(val, "false") ? 1 : 0;
                        mute_count++;
                    }
                    p += 10;
                    continue;
                }
                if (in_tracks && brace_depth == 3 && strncmp(p, "\"solo-cue\"", 10) == 0 && solo_count < 4) {
                    char *val = strchr(p + 10, ':');
                    if (val) {
                        soloed_out[solo_count] = strstr(val, "true") ? 1 : 0;
                        solo_count++;
                    }
                    p += 9;
                    continue;
                }
            }
        }
    }
    fclose(f);

    if (mute_count > 0 && host.log) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Set states from %s: muted=[%d,%d,%d,%d] soloed=[%d,%d,%d,%d]",
                 set_name, muted_out[0], muted_out[1], muted_out[2], muted_out[3],
                 soloed_out[0], soloed_out[1], soloed_out[2], soloed_out[3]);
        host.log(msg);
    }
    return mute_count;
}

/* ============================================================================
 * Blit and overlay sync
 * ============================================================================ */

void overlay_blit_rect(uint8_t *dst, const uint8_t *src,
                       int rx, int ry, int rw, int rh) {
    int x_end = rx + rw;
    int y_end = ry + rh;
    if (x_end > 128) x_end = 128;
    if (y_end > 64) y_end = 64;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;

    for (int y = ry; y < y_end; y++) {
        int page = y / 8;
        int bit  = y % 8;
        uint8_t mask = (uint8_t)(1 << bit);

        for (int x = rx; x < x_end; x++) {
            int idx = page * 128 + x;
            dst[idx] = (dst[idx] & ~mask) | (src[idx] & mask);
        }
    }
}

void shadow_overlay_sync(void) {
    shadow_overlay_state_t *ov = host.shadow_overlay_shm ? *host.shadow_overlay_shm : NULL;
    if (!ov) return;

    /* Determine overlay type (priority: sampler > skipback > shift+knob) */
    if (sampler_fullscreen_active &&
        (sampler_state != SAMPLER_IDLE || sampler_overlay_timeout > 0)) {
        ov->overlay_type = SHADOW_OVERLAY_SAMPLER;
    } else if (skipback_overlay_timeout > 0) {
        ov->overlay_type = SHADOW_OVERLAY_SKIPBACK;
    } else if (set_page_overlay_active && set_page_overlay_timeout > 0) {
        ov->overlay_type = SHADOW_OVERLAY_SET_PAGE;
    } else if (shift_knob_overlay_active && shift_knob_overlay_timeout > 0) {
        ov->overlay_type = SHADOW_OVERLAY_SHIFT_KNOB;
    } else {
        ov->overlay_type = SHADOW_OVERLAY_NONE;
    }

    /* Sampler state */
    ov->sampler_state = (uint8_t)sampler_state;
    ov->sampler_source = (uint8_t)sampler_source;
    ov->sampler_cursor = (uint8_t)sampler_menu_cursor;
    ov->sampler_fullscreen = sampler_fullscreen_active ? 1 : 0;
    ov->sampler_duration_bars = (uint16_t)sampler_duration_options[sampler_duration_index];
    ov->sampler_vu_peak = sampler_vu_peak;
    ov->sampler_bars_completed = (uint16_t)sampler_bars_completed;
    ov->sampler_target_bars = (uint16_t)sampler_duration_options[sampler_duration_index];
    ov->sampler_overlay_timeout = (uint16_t)sampler_overlay_timeout;
    ov->sampler_samples_written = sampler_samples_written;
    ov->sampler_clock_count = (uint32_t)sampler_clock_count;
    ov->sampler_target_pulses = (uint32_t)sampler_target_pulses;
    ov->sampler_fallback_blocks = (uint32_t)sampler_fallback_blocks;
    ov->sampler_fallback_target = (uint32_t)sampler_fallback_target;
    ov->sampler_clock_received = sampler_clock_received ? 1 : 0;

    /* Skipback state */
    ov->skipback_active = (skipback_overlay_timeout > 0) ? 1 : 0;
    ov->skipback_overlay_timeout = (uint16_t)skipback_overlay_timeout;

    /* Shift+knob state */
    ov->shift_knob_active = (shift_knob_overlay_active && shift_knob_overlay_timeout > 0) ? 1 : 0;
    ov->shift_knob_timeout = (uint16_t)shift_knob_overlay_timeout;
    memcpy((char *)ov->shift_knob_patch, shift_knob_overlay_patch, 64);
    memcpy((char *)ov->shift_knob_param, shift_knob_overlay_param, 64);
    memcpy((char *)ov->shift_knob_value, shift_knob_overlay_value, 32);

    /* Set page state */
    ov->set_page_active = (set_page_overlay_active && set_page_overlay_timeout > 0) ? 1 : 0;
    ov->set_page_current = (uint8_t)set_page_current;
    ov->set_page_total = SET_PAGES_TOTAL;
    ov->set_page_timeout = (uint16_t)set_page_overlay_timeout;
    ov->set_page_loading = (uint8_t)set_page_loading;

    /* Increment sequence to notify JS of state change */
    ov->sequence++;
}
