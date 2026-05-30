/* shadow_state.c - Shadow slot state persistence
 * Extracted from schwung_shim.c for maintainability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include "shadow_state.h"

/* ============================================================================
 * Host callbacks (set by state_init)
 * ============================================================================ */

static void (*host_log)(const char *msg);
static shadow_chain_slot_t *host_chain_slots;
static int *host_solo_count;

/* Fix file ownership after writing as root */
static void chown_to_ableton(const char *path) {
    struct passwd *pw = getpwnam("ableton");
    if (pw) chown(path, pw->pw_uid, pw->pw_gid);
}

void state_init(const state_host_t *host)
{
    host_log = host->log;
    host_chain_slots = host->chain_slots;
    host_solo_count = host->solo_count;
}

/* ============================================================================
 * shadow_save_state - Write slot state to shadow_chain_config.json
 * ============================================================================ */

void shadow_save_state(void)
{
    /* Read existing config to preserve fields written by shadow_ui.js */
    FILE *f = fopen(SHADOW_CONFIG_PATH, "r");
    char patches_buf[4096] = "";
    char master_fx[256] = "";
    char master_fx_path[256] = "";
    char master_fx_chain_buf[2048] = "";
    int overlay_knobs_mode = -1;
    int resample_bridge_mode = -1;
    int link_audio_routing_saved = -1;

    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size > 0 && size < 65536) {
            char *json = malloc(size + 1);
            if (json) {
                size_t nread = fread(json, 1, size, f);
                json[nread] = '\0';

                /* Extract patches array (preserve as-is) */
                char *patches_start = strstr(json, "\"patches\":");
                if (patches_start) {
                    char *arr_start = strchr(patches_start, '[');
                    if (arr_start) {
                        int depth = 1;
                        char *arr_end = arr_start + 1;
                        while (*arr_end && depth > 0) {
                            if (*arr_end == '[') depth++;
                            else if (*arr_end == ']') depth--;
                            arr_end++;
                        }
                        int len = arr_end - arr_start;
                        if (len < (int)sizeof(patches_buf) - 1) {
                            strncpy(patches_buf, arr_start, len);
                            patches_buf[len] = '\0';
                        }
                    }
                }

                /* Extract master_fx string (legacy single-slot) */
                char *mfx = strstr(json, "\"master_fx\":");
                if (mfx) {
                    mfx = strchr(mfx, ':');
                    if (mfx) {
                        mfx++;
                        while (*mfx == ' ' || *mfx == '"') mfx++;
                        char *end = mfx;
                        while (*end && *end != '"' && *end != ',' && *end != '\n') end++;
                        int len = end - mfx;
                        if (len < (int)sizeof(master_fx) - 1) {
                            strncpy(master_fx, mfx, len);
                            master_fx[len] = '\0';
                        }
                    }
                }

                /* Extract master_fx_path string */
                char *mfxp = strstr(json, "\"master_fx_path\":");
                if (mfxp) {
                    mfxp = strchr(mfxp, ':');
                    if (mfxp) {
                        mfxp++;
                        while (*mfxp == ' ' || *mfxp == '"') mfxp++;
                        char *end = mfxp;
                        while (*end && *end != '"' && *end != ',' && *end != '\n') end++;
                        int len = end - mfxp;
                        if (len < (int)sizeof(master_fx_path) - 1) {
                            strncpy(master_fx_path, mfxp, len);
                            master_fx_path[len] = '\0';
                        }
                    }
                }

                /* Extract master_fx_chain object (written by shadow_ui.js) */
                char *mfc = strstr(json, "\"master_fx_chain\":");
                if (mfc) {
                    char *obj_start = strchr(mfc, '{');
                    if (obj_start) {
                        int depth = 1;
                        char *obj_end = obj_start + 1;
                        while (*obj_end && depth > 0) {
                            if (*obj_end == '{') depth++;
                            else if (*obj_end == '}') depth--;
                            obj_end++;
                        }
                        int len = obj_end - obj_start;
                        if (len < (int)sizeof(master_fx_chain_buf) - 1) {
                            strncpy(master_fx_chain_buf, obj_start, len);
                            master_fx_chain_buf[len] = '\0';
                        }
                    }
                }

                /* Extract overlay_knobs_mode integer */
                char *okm = strstr(json, "\"overlay_knobs_mode\":");
                if (okm) {
                    okm = strchr(okm, ':');
                    if (okm) {
                        okm++;
                        while (*okm == ' ') okm++;
                        overlay_knobs_mode = atoi(okm);
                    }
                }

                /* Extract resample_bridge_mode integer */
                char *rbm = strstr(json, "\"resample_bridge_mode\":");
                if (rbm) {
                    rbm = strchr(rbm, ':');
                    if (rbm) {
                        rbm++;
                        while (*rbm == ' ') rbm++;
                        resample_bridge_mode = atoi(rbm);
                    }
                }

                /* Extract link_audio_routing boolean */
                char *lar = strstr(json, "\"link_audio_routing\":");
                if (lar) {
                    lar = strchr(lar, ':');
                    if (lar) {
                        lar++;
                        while (*lar == ' ') lar++;
                        link_audio_routing_saved = (strncmp(lar, "true", 4) == 0) ? 1 : 0;
                    }
                }

                free(json);
            }
        }
        fclose(f);
    }

    /* Write complete config file */
    f = fopen(SHADOW_CONFIG_PATH, "w");
    if (!f) {
        if (host_log) host_log("shadow_save_state: failed to open for writing");
        return;
    }

    fprintf(f, "{\n");
    if (patches_buf[0]) {
        fprintf(f, "  \"patches\": %s,\n", patches_buf);
    }
    fprintf(f, "  \"master_fx\": \"%s\",\n", master_fx);
    if (master_fx_path[0]) {
        fprintf(f, "  \"master_fx_path\": \"%s\",\n", master_fx_path);
    }
    if (master_fx_chain_buf[0]) {
        fprintf(f, "  \"master_fx_chain\": %s,\n", master_fx_chain_buf);
    }
    if (overlay_knobs_mode >= 0) {
        fprintf(f, "  \"overlay_knobs_mode\": %d,\n", overlay_knobs_mode);
    }
    if (resample_bridge_mode >= 0) {
        fprintf(f, "  \"resample_bridge_mode\": %d,\n", resample_bridge_mode);
    }
    if (link_audio_routing_saved >= 0) {
        fprintf(f, "  \"link_audio_routing\": %s,\n", link_audio_routing_saved ? "true" : "false");
    }
    /* Volume is always the real user-set level; mute/solo are separate flags */
    fprintf(f, "  \"slot_volumes\": [%.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f],\n",
            host_chain_slots[0].volume, host_chain_slots[1].volume,
            host_chain_slots[2].volume, host_chain_slots[3].volume,
            host_chain_slots[4].volume, host_chain_slots[5].volume,
            host_chain_slots[6].volume, host_chain_slots[7].volume);
    fprintf(f, "  \"slot_channels\": [%d, %d, %d, %d, %d, %d, %d, %d],\n",
            host_chain_slots[0].channel, host_chain_slots[1].channel,
            host_chain_slots[2].channel, host_chain_slots[3].channel,
            host_chain_slots[4].channel, host_chain_slots[5].channel,
            host_chain_slots[6].channel, host_chain_slots[7].channel);
    fprintf(f, "  \"slot_forward_channels\": [%d, %d, %d, %d, %d, %d, %d, %d],\n",
            host_chain_slots[0].forward_channel, host_chain_slots[1].forward_channel,
            host_chain_slots[2].forward_channel, host_chain_slots[3].forward_channel,
            host_chain_slots[4].forward_channel, host_chain_slots[5].forward_channel,
            host_chain_slots[6].forward_channel, host_chain_slots[7].forward_channel);
    fprintf(f, "  \"slot_transpose\": [%d, %d, %d, %d, %d, %d, %d, %d],\n",
            host_chain_slots[0].transpose, host_chain_slots[1].transpose,
            host_chain_slots[2].transpose, host_chain_slots[3].transpose,
            host_chain_slots[4].transpose, host_chain_slots[5].transpose,
            host_chain_slots[6].transpose, host_chain_slots[7].transpose);
    fprintf(f, "  \"slot_muted\": [%d, %d, %d, %d, %d, %d, %d, %d],\n",
            host_chain_slots[0].muted, host_chain_slots[1].muted,
            host_chain_slots[2].muted, host_chain_slots[3].muted,
            host_chain_slots[4].muted, host_chain_slots[5].muted,
            host_chain_slots[6].muted, host_chain_slots[7].muted);
    fprintf(f, "  \"slot_soloed\": [%d, %d, %d, %d, %d, %d, %d, %d],\n",
            host_chain_slots[0].soloed, host_chain_slots[1].soloed,
            host_chain_slots[2].soloed, host_chain_slots[3].soloed,
            host_chain_slots[4].soloed, host_chain_slots[5].soloed,
            host_chain_slots[6].soloed, host_chain_slots[7].soloed);
    fprintf(f, "  \"slot_split_enabled\": [%d, %d, %d, %d],\n",
            host_chain_slots[0].split_enabled, host_chain_slots[1].split_enabled,
            host_chain_slots[2].split_enabled, host_chain_slots[3].split_enabled);
    fprintf(f, "  \"slot_split_octave\": [%d, %d, %d, %d]\n",
            host_chain_slots[0].split_octave, host_chain_slots[1].split_octave,
            host_chain_slots[2].split_octave, host_chain_slots[3].split_octave);
    fprintf(f, "}\n");
    fclose(f);
    chown_to_ableton(SHADOW_CONFIG_PATH);

    char msg[512];
    snprintf(msg, sizeof(msg), "Saved 8 slots: ch=[%d,%d,%d,%d,%d,%d,%d,%d] vol=[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]",
             host_chain_slots[0].channel, host_chain_slots[1].channel,
             host_chain_slots[2].channel, host_chain_slots[3].channel,
             host_chain_slots[4].channel, host_chain_slots[5].channel,
             host_chain_slots[6].channel, host_chain_slots[7].channel,
             host_chain_slots[0].volume, host_chain_slots[1].volume,
             host_chain_slots[2].volume, host_chain_slots[3].volume,
             host_chain_slots[4].volume, host_chain_slots[5].volume,
             host_chain_slots[6].volume, host_chain_slots[7].volume);
    if (host_log) host_log(msg);
}

/* ============================================================================
 * shadow_load_state - Read slot state from shadow_chain_config.json
 * ============================================================================ */

void shadow_load_state(void)
{
    FILE *f = fopen(SHADOW_CONFIG_PATH, "r");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    /* Parse slot_volumes array */
    const char *key = "\"slot_volumes\":";
    char *pos = strstr(json, key);
    if (pos) {
        pos = strchr(pos, '[');
        if (pos) {
            float v[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            int parsed = sscanf(pos, "[%f, %f, %f, %f, %f, %f, %f, %f]", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            if (parsed == 4) {
                // Fallback for 4 slots
                sscanf(pos, "[%f, %f, %f, %f]", &v[0], &v[1], &v[2], &v[3]);
            }
            int lim = (parsed >= 8) ? 8 : 4;
            for (int i = 0; i < lim; i++) {
                if (v[i] < 0.0f) v[i] = 0.0f;
                if (v[i] > 4.0f) v[i] = 4.0f;
                host_chain_slots[i].volume = v[i];
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "Loaded slot volumes (%d slots): [%.2f, %.2f, %.2f, %.2f ...]",
                     lim, v[0], v[1], v[2], v[3]);
            if (host_log) host_log(msg);
        }
    }

    /* Parse slot_channels (receive channel) array */
    const char *ch_key = "\"slot_channels\":";
    char *ch_pos = strstr(json, ch_key);
    if (ch_pos) {
        ch_pos = strchr(ch_pos, '[');
        if (ch_pos) {
            int c[8] = {0, 1, 2, 3, SHADOW_CHANNEL_SPLIT, SHADOW_CHANNEL_SPLIT, SHADOW_CHANNEL_SPLIT, SHADOW_CHANNEL_SPLIT};
            int parsed = sscanf(ch_pos, "[%d, %d, %d, %d, %d, %d, %d, %d]", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]);
            if (parsed == 4) {
                sscanf(ch_pos, "[%d, %d, %d, %d]", &c[0], &c[1], &c[2], &c[3]);
            }
            int lim = (parsed >= 8) ? 8 : 4;
            for (int i = 0; i < lim; i++) {
                host_chain_slots[i].channel = c[i];
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "Loaded slot channels (%d slots): [%d, %d, %d, %d ...]",
                     lim, c[0], c[1], c[2], c[3]);
            if (host_log) host_log(msg);
        }
    }

    /* Parse slot_forward_channels array */
    const char *fwd_key = "\"slot_forward_channels\":";
    char *fwd_pos = strstr(json, fwd_key);
    if (fwd_pos) {
        fwd_pos = strchr(fwd_pos, '[');
        if (fwd_pos) {
            int f[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
            int parsed = sscanf(fwd_pos, "[%d, %d, %d, %d, %d, %d, %d, %d]", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7]);
            if (parsed == 4) {
                sscanf(fwd_pos, "[%d, %d, %d, %d]", &f[0], &f[1], &f[2], &f[3]);
            }
            int lim = (parsed >= 8) ? 8 : 4;
            for (int i = 0; i < lim; i++) {
                host_chain_slots[i].forward_channel = f[i];
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "Loaded slot fwd channels (%d slots): [%d, %d, %d, %d ...]",
                     lim, f[0], f[1], f[2], f[3]);
            if (host_log) host_log(msg);
        }
    }

    /* Parse slot_transpose array */
    const char *tr_key = "\"slot_transpose\":";
    char *tr_pos = strstr(json, tr_key);
    if (tr_pos) {
        tr_pos = strchr(tr_pos, '[');
        if (tr_pos) {
            int t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int parsed = sscanf(tr_pos, "[%d, %d, %d, %d, %d, %d, %d, %d]", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]);
            if (parsed == 4) {
                sscanf(tr_pos, "[%d, %d, %d, %d]", &t[0], &t[1], &t[2], &t[3]);
            }
            int lim = (parsed >= 8) ? 8 : 4;
            for (int i = 0; i < lim; i++) {
                if (t[i] < -12) t[i] = -12;
                if (t[i] > 12) t[i] = 12;
                host_chain_slots[i].transpose = t[i];
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "Loaded slot transpose (%d slots): [%d, %d, %d, %d ...]",
                     lim, t[0], t[1], t[2], t[3]);
            if (host_log) host_log(msg);
        }
    }

    /* Parse slot_muted array */
    const char *muted_key = "\"slot_muted\":";
    char *muted_pos = strstr(json, muted_key);
    if (muted_pos) {
        muted_pos = strchr(muted_pos, '[');
        if (muted_pos) {
            int m[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int parsed = sscanf(muted_pos, "[%d, %d, %d, %d, %d, %d, %d, %d]", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7]);
            if (parsed == 4) {
                sscanf(muted_pos, "[%d, %d, %d, %d]", &m[0], &m[1], &m[2], &m[3]);
            }
            int lim = (parsed >= 8) ? 8 : 4;
            for (int i = 0; i < lim; i++) {
                host_chain_slots[i].muted = m[i];
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "Loaded slot muted (%d slots): [%d, %d, %d, %d ...]",
                     lim, m[0], m[1], m[2], m[3]);
            if (host_log) host_log(msg);
        }
    }

    /* Parse slot_soloed array */
    const char *soloed_key = "\"slot_soloed\":";
    char *soloed_pos = strstr(json, soloed_key);
    *host_solo_count = 0;
    if (soloed_pos) {
        soloed_pos = strchr(soloed_pos, '[');
        if (soloed_pos) {
            int s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int parsed = sscanf(soloed_pos, "[%d, %d, %d, %d, %d, %d, %d, %d]", &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7]);
            if (parsed == 4) {
                sscanf(soloed_pos, "[%d, %d, %d, %d]", &s[0], &s[1], &s[2], &s[3]);
            }
            int lim = (parsed >= 8) ? 8 : 4;
            for (int i = 0; i < lim; i++) {
                host_chain_slots[i].soloed = s[i];
                if (s[i]) (*host_solo_count)++;
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "Loaded slot soloed (%d slots): [%d, %d, %d, %d ...]",
                     lim, s[0], s[1], s[2], s[3]);
            if (host_log) host_log(msg);
        }
    }

    /* Parse slot_split_enabled array */
    const char *sp_key = "\"slot_split_enabled\":";
    char *sp_pos = strstr(json, sp_key);
    if (sp_pos) {
        sp_pos = strchr(sp_pos, '[');
        if (sp_pos) {
            int sp[4] = {0, 0, 0, 0};
            if (sscanf(sp_pos, "[%d, %d, %d, %d]", &sp[0], &sp[1], &sp[2], &sp[3]) == 4) {
                for (int i = 0; i < 4; i++) {
                    host_chain_slots[i].split_enabled = sp[i];
                }
            }
        }
    }

    /* Parse slot_split_octave array */
    const char *spo_key = "\"slot_split_octave\":";
    char *spo_pos = strstr(json, spo_key);
    if (spo_pos) {
        spo_pos = strchr(spo_pos, '[');
        if (spo_pos) {
            int spo[4] = {4, 4, 4, 4};
            if (sscanf(spo_pos, "[%d, %d, %d, %d]", &spo[0], &spo[1], &spo[2], &spo[3]) == 4) {
                for (int i = 0; i < 4; i++) {
                    if (spo[i] < 0) spo[i] = 0;
                    if (spo[i] > 10) spo[i] = 10;
                    host_chain_slots[i].split_octave = spo[i];
                }
            }
        }
    }

    free(json);
}
