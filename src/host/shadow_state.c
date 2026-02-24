/* shadow_state.c - Shadow slot state persistence
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shadow_state.h"

/* ============================================================================
 * Host callbacks (set by state_init)
 * ============================================================================ */

static void (*host_log)(const char *msg);
static shadow_chain_slot_t *host_chain_slots;
static int *host_solo_count;

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

        if (size > 0 && size < 16384) {
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
    fprintf(f, "  \"slot_volumes\": [%.3f, %.3f, %.3f, %.3f],\n",
            host_chain_slots[0].volume,
            host_chain_slots[1].volume,
            host_chain_slots[2].volume,
            host_chain_slots[3].volume);
    fprintf(f, "  \"slot_forward_channels\": [%d, %d, %d, %d],\n",
            host_chain_slots[0].forward_channel,
            host_chain_slots[1].forward_channel,
            host_chain_slots[2].forward_channel,
            host_chain_slots[3].forward_channel);
    fprintf(f, "  \"slot_muted\": [%d, %d, %d, %d],\n",
            host_chain_slots[0].muted,
            host_chain_slots[1].muted,
            host_chain_slots[2].muted,
            host_chain_slots[3].muted);
    fprintf(f, "  \"slot_soloed\": [%d, %d, %d, %d]\n",
            host_chain_slots[0].soloed,
            host_chain_slots[1].soloed,
            host_chain_slots[2].soloed,
            host_chain_slots[3].soloed);
    fprintf(f, "}\n");
    fclose(f);

    char msg[256];
    snprintf(msg, sizeof(msg), "Saved slots: vol=[%.2f,%.2f,%.2f,%.2f] muted=[%d,%d,%d,%d] soloed=[%d,%d,%d,%d]",
             host_chain_slots[0].volume, host_chain_slots[1].volume,
             host_chain_slots[2].volume, host_chain_slots[3].volume,
             host_chain_slots[0].muted, host_chain_slots[1].muted,
             host_chain_slots[2].muted, host_chain_slots[3].muted,
             host_chain_slots[0].soloed, host_chain_slots[1].soloed,
             host_chain_slots[2].soloed, host_chain_slots[3].soloed);
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
            float v0, v1, v2, v3;
            if (sscanf(pos, "[%f, %f, %f, %f]", &v0, &v1, &v2, &v3) == 4) {
                host_chain_slots[0].volume = v0;
                host_chain_slots[1].volume = v1;
                host_chain_slots[2].volume = v2;
                host_chain_slots[3].volume = v3;

                char msg[128];
                snprintf(msg, sizeof(msg), "Loaded slot volumes: [%.2f, %.2f, %.2f, %.2f]",
                         v0, v1, v2, v3);
                if (host_log) host_log(msg);
            }
        }
    }

    /* Parse slot_forward_channels array */
    const char *fwd_key = "\"slot_forward_channels\":";
    char *fwd_pos = strstr(json, fwd_key);
    if (fwd_pos) {
        fwd_pos = strchr(fwd_pos, '[');
        if (fwd_pos) {
            int f0, f1, f2, f3;
            if (sscanf(fwd_pos, "[%d, %d, %d, %d]", &f0, &f1, &f2, &f3) == 4) {
                host_chain_slots[0].forward_channel = f0;
                host_chain_slots[1].forward_channel = f1;
                host_chain_slots[2].forward_channel = f2;
                host_chain_slots[3].forward_channel = f3;

                char msg[128];
                snprintf(msg, sizeof(msg), "Loaded slot fwd channels: [%d, %d, %d, %d]",
                         f0, f1, f2, f3);
                if (host_log) host_log(msg);
            }
        }
    }

    /* Parse slot_muted array */
    const char *muted_key = "\"slot_muted\":";
    char *muted_pos = strstr(json, muted_key);
    if (muted_pos) {
        muted_pos = strchr(muted_pos, '[');
        if (muted_pos) {
            int m0, m1, m2, m3;
            if (sscanf(muted_pos, "[%d, %d, %d, %d]", &m0, &m1, &m2, &m3) == 4) {
                host_chain_slots[0].muted = m0;
                host_chain_slots[1].muted = m1;
                host_chain_slots[2].muted = m2;
                host_chain_slots[3].muted = m3;
                char msg[128];
                snprintf(msg, sizeof(msg), "Loaded slot muted: [%d, %d, %d, %d]",
                         m0, m1, m2, m3);
                if (host_log) host_log(msg);
            }
        }
    }

    /* Parse slot_soloed array */
    const char *soloed_key = "\"slot_soloed\":";
    char *soloed_pos = strstr(json, soloed_key);
    *host_solo_count = 0;
    if (soloed_pos) {
        soloed_pos = strchr(soloed_pos, '[');
        if (soloed_pos) {
            int s0, s1, s2, s3;
            if (sscanf(soloed_pos, "[%d, %d, %d, %d]", &s0, &s1, &s2, &s3) == 4) {
                int sol[4] = {s0, s1, s2, s3};
                for (int i = 0; i < 4; i++) {
                    host_chain_slots[i].soloed = sol[i];
                    if (sol[i]) (*host_solo_count)++;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "Loaded slot soloed: [%d, %d, %d, %d]",
                         s0, s1, s2, s3);
                if (host_log) host_log(msg);
            }
        }
    }

    free(json);
}
