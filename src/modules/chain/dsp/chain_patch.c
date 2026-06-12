/*
 * Signal Chain — patch + master-preset persistence and loading.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"

/* Fix file ownership after writing as root */
static void chown_to_ableton(const char *path) {
    struct passwd *pw = getpwnam("ableton");
    if (pw) chown(path, pw->pw_uid, pw->pw_gid);
}

/* Generate a patch name from components */
static void generate_patch_name(char *out, int out_len,
                                const char *synth, int preset,
                                const char *fx1, const char *fx2) {
    /* (The old preset-name lookup read the v1 singleton globals, which were
     * never populated under the v2 instance API — so in deployment this
     * always produced the bare "synth NN" form. Keep that behavior.) */
    snprintf(out, out_len, "%s %02d", synth, preset);

    /* Append FX names */
    if (fx1 && fx1[0] != '\0') {
        int len = strlen(out);
        snprintf(out + len, out_len - len, " + %s", fx1);
    }
    if (fx2 && fx2[0] != '\0') {
        int len = strlen(out);
        snprintf(out + len, out_len - len, " + %s", fx2);
    }
}

static void sanitize_filename(char *out, int out_len, const char *name) {
    int j = 0;
    for (int i = 0; name[i] && j < out_len - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            out[j++] = c + 32; /* lowercase */
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (c == ' ' || c == '-') {
            out[j++] = '_';
        }
        /* Skip other characters */
    }
    out[j] = '\0';
}

static int check_filename_exists(const char *dir, const char *base, char *out_path, int out_len) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.json", dir, base);

    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return 1; /* Exists */
    }

    strncpy(out_path, path, out_len - 1);
    out_path[out_len - 1] = '\0';
    return 0;
}

/* Compare function for sorting patches alphabetically by name */
static int compare_patches(const void *a, const void *b) {
    const patch_info_t *pa = (const patch_info_t *)a;
    const patch_info_t *pb = (const patch_info_t *)b;
    return strcasecmp(pa->name, pb->name);
}

/* V2 scan patches - simple version */
int v2_scan_patches(chain_instance_t *inst) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    if (!inst) return -1;

    snprintf(patches_dir, sizeof(patches_dir), "%s/../../patches", inst->module_dir);
    inst->patch_count = 0;
    /* Clear old data to prevent stale/garbage entries */
    memset(inst->patches, 0, sizeof(inst->patches));

    DIR *dir = opendir(patches_dir);
    if (!dir) {
        snprintf(msg, sizeof(msg), "Cannot open patches dir: %s", patches_dir);
        v2_chain_log(inst, msg);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->patch_count < MAX_PATCHES) {
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 5, ".json") != 0) continue;

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", patches_dir, entry->d_name);

        patch_info_t *patch = &inst->patches[inst->patch_count];
        if (v2_parse_patch_file(inst, path, patch) == 0) {
            strncpy(patch->path, path, MAX_PATH_LEN - 1);
            inst->patch_count++;
        }
    }

    closedir(dir);

    /* Sort patches alphabetically by name */
    if (inst->patch_count > 1) {
        qsort(inst->patches, inst->patch_count, sizeof(patch_info_t), compare_patches);
    }

    return inst->patch_count;
}

/* Atomically write a buffer to path via temp file + rename, checking every
 * step so disk-full/short writes fail loudly instead of leaving a corrupt
 * file in place. Returns 0 on success, -1 on failure (callers log). */
static int chain_write_file_atomic(const char *path, const char *data, size_t len) {
    char tmppath[MAX_PATH_LEN + 8];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
    FILE *f = fopen(tmppath, "w");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    int close_err = fclose(f);
    if (written != len || close_err != 0) {
        remove(tmppath);
        return -1;
    }
    if (rename(tmppath, path) != 0) {
        remove(tmppath);
        return -1;
    }
    return 0;
}

/* Write a patch file as {name, version, chain}. Heap-sized — chain JSON can
 * exceed 8 KB (fat synth state up to SHADOW_PARAM_VALUE_LEN), so a fixed
 * buffer would silently truncate the saved patch. */
static int v2_write_patch_file(chain_instance_t *inst, const char *filepath,
                               const char *name, const char *json_data) {
    char msg[256];
    size_t cap = strlen(json_data) + strlen(name) + 128;
    char *final_json = malloc(cap);
    if (!final_json) {
        snprintf(msg, sizeof(msg), "[v2] Patch save failed: out of memory (%zu bytes)", cap);
        v2_chain_log(inst, msg);
        return -1;
    }
    int n = snprintf(final_json, cap,
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"chain\": %s\n"
        "}\n",
        name, json_data);
    if (n < 0 || (size_t)n >= cap) {
        free(final_json);
        snprintf(msg, sizeof(msg), "[v2] Patch save failed: JSON larger than buffer (%d/%zu)", n, cap);
        v2_chain_log(inst, msg);
        return -1;
    }

    int rc = chain_write_file_atomic(filepath, final_json, (size_t)n);
    free(final_json);
    if (rc != 0) {
        snprintf(msg, sizeof(msg), "[v2] Patch write failed: %s", filepath);
        v2_chain_log(inst, msg);
        return -1;
    }
    chown_to_ableton(filepath);
    return 0;
}

/* V2 save patch - uses instance data instead of globals */
int v2_save_patch(chain_instance_t *inst, const char *json_data) {
    char msg[256];
    char patches_dir[MAX_PATH_LEN];
    snprintf(patches_dir, sizeof(patches_dir), "%s/../../patches", inst->module_dir);

    /* Parse incoming JSON to get components for name generation */
    char synth[MAX_NAME_LEN] = "sf2";
    int preset = 0;
    char fx1[MAX_NAME_LEN] = "";
    char fx2[MAX_NAME_LEN] = "";

    json_get_string_in_section(json_data, "synth", "module", synth, sizeof(synth));
    json_get_int_in_section(json_data, "config", "preset", &preset);

    /* Parse audio_fx to get fx1 and fx2 */
    const char *fx_pos = strstr(json_data, "\"audio_fx\"");
    if (fx_pos) {
        const char *bracket = strchr(fx_pos, '[');
        if (bracket) {
            const char *type1 = strstr(bracket, "\"type\"");
            if (type1) {
                const char *colon = strchr(type1, ':');
                if (colon) {
                    const char *q1 = strchr(colon, '"');
                    if (q1) {
                        q1++;
                        const char *q2 = strchr(q1, '"');
                        if (q2) {
                            int len = q2 - q1;
                            if (len < MAX_NAME_LEN) {
                                strncpy(fx1, q1, len);
                                fx1[len] = '\0';
                            }
                            const char *type2 = strstr(q2, "\"type\"");
                            if (type2) {
                                colon = strchr(type2, ':');
                                if (colon) {
                                    q1 = strchr(colon, '"');
                                    if (q1) {
                                        q1++;
                                        q2 = strchr(q1, '"');
                                        if (q2) {
                                            len = q2 - q1;
                                            if (len < MAX_NAME_LEN) {
                                                strncpy(fx2, q1, len);
                                                fx2[len] = '\0';
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Check for custom name first, otherwise generate from components */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_data, "custom_name", name, sizeof(name)) != 0) {
        generate_patch_name(name, sizeof(name), synth, preset, fx1, fx2);
    }

    /* Sanitize to filename */
    char base_filename[MAX_NAME_LEN];
    sanitize_filename(base_filename, sizeof(base_filename), name);

    /* Find available filename */
    char filepath[MAX_PATH_LEN];
    if (check_filename_exists(patches_dir, base_filename, filepath, sizeof(filepath))) {
        for (int i = 2; i < 100; i++) {
            char suffixed[MAX_NAME_LEN];
            snprintf(suffixed, sizeof(suffixed), "%s_%02d", base_filename, i);
            if (!check_filename_exists(patches_dir, suffixed, filepath, sizeof(filepath))) {
                int namelen = strlen(name);
                snprintf(name + namelen, sizeof(name) - namelen, " %02d", i);
                break;
            }
        }
    }

    /* Escape name for JSON embedding (handle quotes and backslashes) */
    char escaped_name[MAX_NAME_LEN * 2];
    {
        int si = 0, di = 0;
        while (name[si] && di < (int)sizeof(escaped_name) - 2) {
            if (name[si] == '"' || name[si] == '\\') {
                escaped_name[di++] = '\\';
            }
            escaped_name[di++] = name[si++];
        }
        escaped_name[di] = '\0';
    }

    if (v2_write_patch_file(inst, filepath, escaped_name, json_data) != 0) {
        return -1;
    }

    snprintf(msg, sizeof(msg), "[v2] Saved patch: %s", filepath);
    v2_chain_log(inst, msg);

    return 0;
}

/* V2 update patch - uses instance data instead of globals */
int v2_update_patch(chain_instance_t *inst, int index, const char *json_data) {
    char msg[256];

    if (index < 0 || index >= inst->patch_count) {
        snprintf(msg, sizeof(msg), "[v2] Invalid patch index for update: %d (count=%d)", index, inst->patch_count);
        v2_chain_log(inst, msg);
        return -1;
    }

    const char *filepath = inst->patches[index].path;

    /* Check for custom name, otherwise keep existing name */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_data, "custom_name", name, sizeof(name)) != 0) {
        strncpy(name, inst->patches[index].name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    if (v2_write_patch_file(inst, filepath, name, json_data) != 0) {
        return -1;
    }

    snprintf(msg, sizeof(msg), "[v2] Updated patch: %s", filepath);
    v2_chain_log(inst, msg);

    return 0;
}

/* V2 delete patch - uses instance data instead of globals */
int v2_delete_patch(chain_instance_t *inst, int index) {
    char msg[256];

    if (index < 0 || index >= inst->patch_count) {
        snprintf(msg, sizeof(msg), "[v2] Invalid patch index for delete: %d (count=%d)", index, inst->patch_count);
        v2_chain_log(inst, msg);
        return -1;
    }

    const char *path = inst->patches[index].path;

    if (remove(path) != 0) {
        snprintf(msg, sizeof(msg), "[v2] Failed to delete patch: %s", path);
        v2_chain_log(inst, msg);
        return -1;
    }

    snprintf(msg, sizeof(msg), "[v2] Deleted patch: %s", path);
    v2_chain_log(inst, msg);

    /* Adjust current_patch index if needed */
    if (index == inst->current_patch) {
        inst->current_patch = -1;  /* Mark as no patch loaded */
    } else if (index < inst->current_patch) {
        inst->current_patch--;
    }

    return 0;
}

/* ========== Master Preset Functions ========== */

#define PRESETS_MASTER_DIR "/data/UserData/schwung/presets_master"

/* Master preset info storage (simpler than chain patches) */
char master_preset_names[MAX_MASTER_PRESETS][MAX_NAME_LEN];
char master_preset_paths[MAX_MASTER_PRESETS][MAX_PATH_LEN];
int master_preset_count = 0;

static void ensure_presets_master_dir(void) {
    struct stat st = {0};
    if (stat(PRESETS_MASTER_DIR, &st) == -1) {
        mkdir(PRESETS_MASTER_DIR, 0755);
    }
}

void scan_master_presets(void) {
    master_preset_count = 0;
    /* Clear old data to prevent stale/garbage entries */
    memset(master_preset_names, 0, sizeof(master_preset_names));
    memset(master_preset_paths, 0, sizeof(master_preset_paths));
    ensure_presets_master_dir();

    DIR *dir = opendir(PRESETS_MASTER_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && master_preset_count < MAX_MASTER_PRESETS) {
        if (entry->d_type != DT_REG) continue;

        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;

        /* Extract name without .json extension */
        size_t name_len = len - 5;
        if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;

        /* Try to read the "name" field from the JSON file */
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", PRESETS_MASTER_DIR, name);

        FILE *f = fopen(path, "r");
        if (f) {
            char json_buf[2048];
            size_t read_len = fread(json_buf, 1, sizeof(json_buf) - 1, f);
            json_buf[read_len] = '\0';
            fclose(f);

            /* Try to get name from JSON */
            char parsed_name[MAX_NAME_LEN] = {0};
            if (json_get_string(json_buf, "name", parsed_name, sizeof(parsed_name)) == 0) {
                strncpy(master_preset_names[master_preset_count], parsed_name, MAX_NAME_LEN - 1);
                master_preset_names[master_preset_count][MAX_NAME_LEN - 1] = '\0';
            } else {
                /* Fall back to filename */
                memcpy(master_preset_names[master_preset_count], name, name_len);
                master_preset_names[master_preset_count][name_len] = '\0';
            }
        } else {
            memcpy(master_preset_names[master_preset_count], name, name_len);
            master_preset_names[master_preset_count][name_len] = '\0';
        }

        strncpy(master_preset_paths[master_preset_count], path, MAX_PATH_LEN - 1);
        master_preset_paths[master_preset_count][MAX_PATH_LEN - 1] = '\0';
        master_preset_count++;
    }
    closedir(dir);
}

/* Helper to extract JSON object section as string */
/* Extract one FX section as a malloc'd string ("null" if absent). Heap-sized
 * because FX state blobs can exceed any reasonable fixed buffer; truncating
 * mid-JSON would corrupt the saved preset. Returns NULL on OOM. */
static char *extract_fx_section_dup(const char *json, const char *key) {
    const char *start = NULL;
    const char *end = NULL;
    if (json_get_section_bounds(json, key, &start, &end) != 0) {
        return strdup("null");
    }
    size_t len = (size_t)(end - start + 1);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* Build the wrapped master-preset JSON on the heap. Returns NULL on OOM or
 * format overflow (callers log and fail the save). */
static char *build_master_preset_json(const char *name, const char *json_str) {
    char *fx1 = extract_fx_section_dup(json_str, "fx1");
    char *fx2 = extract_fx_section_dup(json_str, "fx2");
    char *fx3 = extract_fx_section_dup(json_str, "fx3");
    char *fx4 = extract_fx_section_dup(json_str, "fx4");
    char *out = NULL;
    if (fx1 && fx2 && fx3 && fx4) {
        size_t cap = strlen(fx1) + strlen(fx2) + strlen(fx3) + strlen(fx4)
                   + strlen(name) + 160;
        out = malloc(cap);
        if (out) {
            int n = snprintf(out, cap,
                "{\n"
                "    \"name\": \"%s\",\n"
                "    \"version\": 1,\n"
                "    \"master_fx\": {\n"
                "        \"fx1\": %s,\n"
                "        \"fx2\": %s,\n"
                "        \"fx3\": %s,\n"
                "        \"fx4\": %s\n"
                "    }\n"
                "}\n",
                name, fx1, fx2, fx3, fx4);
            if (n < 0 || (size_t)n >= cap) {
                free(out);
                out = NULL;
            }
        }
    }
    free(fx1);
    free(fx2);
    free(fx3);
    free(fx4);
    return out;
}

int save_master_preset(const char *json_str) {
    ensure_presets_master_dir();

    /* Get custom_name from JSON */
    char name[MAX_NAME_LEN] = "Master FX";
    json_get_string(json_str, "custom_name", name, sizeof(name));

    /* Debug: log incoming JSON and extracted name */
    {
        struct stat st;
        if (stat(CHAIN_DEBUG_FLAG_PATH, &st) == 0) {
            FILE *dbg = fopen(CHAIN_DEBUG_LOG_PATH, "a");
            if (dbg) {
                fprintf(dbg, "save_master_preset json='%.200s'\n", json_str);
                fprintf(dbg, "save_master_preset name='%s' len=%zu\n", name, strlen(name));
                fclose(dbg);
            }
        }
    }

    /* Sanitize name for filename */
    char filename[MAX_NAME_LEN];
    sanitize_filename(filename, sizeof(filename), name);

    /* Build path */
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.json", PRESETS_MASTER_DIR, filename);

    /* Build wrapped JSON (heap-sized; FX state can exceed fixed buffers) */
    char *final_json = build_master_preset_json(name, json_str);
    if (!final_json) {
        chain_log("Failed to build master preset JSON (out of memory)");
        return -1;
    }

    int rc = chain_write_file_atomic(path, final_json, strlen(final_json));
    free(final_json);
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to save master preset: %s", path);
        chain_log(msg);
        return -1;
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Saved master preset: %s", name);
        chain_log(msg);
    }

    scan_master_presets();
    return 0;
}

int update_master_preset(int index, const char *json_str) {
    if (index < 0 || index >= master_preset_count) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Invalid master preset index: %d", index);
        chain_log(msg);
        return -1;
    }

    /* Get name from JSON or use existing */
    char name[MAX_NAME_LEN];
    if (json_get_string(json_str, "custom_name", name, sizeof(name)) != 0) {
        strncpy(name, master_preset_names[index], sizeof(name) - 1);
    }

    /* Build wrapped JSON (heap-sized; FX state can exceed fixed buffers) */
    char *final_json = build_master_preset_json(name, json_str);
    if (!final_json) {
        chain_log("Failed to build master preset JSON (out of memory)");
        return -1;
    }

    int rc = chain_write_file_atomic(master_preset_paths[index], final_json,
                                     strlen(final_json));
    free(final_json);
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to update master preset: %s",
                 master_preset_paths[index]);
        chain_log(msg);
        return -1;
    }

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Updated master preset: %s", name);
        chain_log(msg);
    }
    scan_master_presets();
    return 0;
}

int delete_master_preset(int index) {
    if (index < 0 || index >= master_preset_count) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Invalid master preset index: %d", index);
        chain_log(msg);
        return -1;
    }

    if (remove(master_preset_paths[index]) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to delete master preset: %s", master_preset_paths[index]);
        chain_log(msg);
        return -1;
    }

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Deleted master preset: %s", master_preset_names[index]);
        chain_log(msg);
    }
    scan_master_presets();
    return 0;
}

int load_master_preset_json(int index, char *buf, int buf_len) {
    if (index < 0 || index >= master_preset_count) {
        buf[0] = '\0';
        return 0;
    }

    FILE *f = fopen(master_preset_paths[index], "r");
    if (!f) {
        buf[0] = '\0';
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        buf[0] = '\0';
        return 0;
    }
    if (len >= buf_len) len = buf_len - 1;
    size_t nr = fread(buf, 1, len, f);
    buf[nr] = '\0';
    len = (long)nr;
    fclose(f);

    return (int)len;
}

/* ========== End Master Preset Functions ========== */

/* V2 parse patch file - simplified version */
int v2_parse_patch_file(chain_instance_t *inst, const char *path, patch_info_t *patch) {
    (void)inst;

    char dbgmsg[512];
    snprintf(dbgmsg, sizeof(dbgmsg), "=== Parsing: %s ===", path);
    parse_debug_log(dbgmsg);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 65536) {
        fclose(f);
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    { size_t nr = fread(json, 1, size, f); json[nr] = '\0'; }
    fclose(f);

    memset(patch, 0, sizeof(*patch));
    /* Mark channel fields as absent; json_get_int only overwrites if present. */
    patch->receive_channel = PATCH_CHANNEL_UNSET;
    patch->forward_channel = PATCH_CHANNEL_UNSET;

    /* Parse name */
    json_get_string(json, "name", patch->name, MAX_NAME_LEN);
    if (!patch->name[0]) {
        /* Use filename as name */
        const char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
        strncpy(patch->name, base, MAX_NAME_LEN - 1);
        char *dot = strrchr(patch->name, '.');
        if (dot) *dot = '\0';
    }

    /* Parse synth */
    json_get_string_in_section(json, "synth", "module", patch->synth_module, MAX_NAME_LEN);
    json_get_int_in_section(json, "synth", "preset", &patch->synth_preset);

    /* Parse synth config.state if present */
    patch->synth_state[0] = '\0';
    const char *synth_pos = strstr(json, "\"synth\"");
    if (synth_pos) {
        const char *config_pos = strstr(synth_pos, "\"config\"");
        if (config_pos) {
            const char *state_pos = strstr(config_pos, "\"state\"");
            if (state_pos) {
                const char *state_colon = strchr(state_pos, ':');
                if (state_colon) {
                    const char *sv = state_colon + 1;
                    while (*sv == ' ' || *sv == '\t' || *sv == '\n') sv++;
                    if (*sv == '{') {
                        /* Extract state as JSON object */
                        const char *end = sv + 1;
                        int depth = 1;
                        while (*end && depth > 0) {
                            if (*end == '{') depth++;
                            else if (*end == '}') depth--;
                            if (depth > 0) end++;
                        }
                        if (*end && depth == 0) {
                            int len = end - sv + 1;
                            if (len > 0 && len < MAX_SYNTH_STATE_LEN) {
                                strncpy(patch->synth_state, sv, len);
                                patch->synth_state[len] = '\0';
                            }
                        }
                    } else if (*sv == '"') {
                        /* Extract state as opaque string (non-JSON formats) */
                        const char *str_start = sv + 1;
                        const char *str_end = str_start;
                        while (*str_end && *str_end != '"') {
                            if (*str_end == '\\' && *(str_end + 1)) str_end++;
                            str_end++;
                        }
                        if (*str_end == '"') {
                            int len = str_end - str_start;
                            if (len > 0 && len < MAX_SYNTH_STATE_LEN) {
                                strncpy(patch->synth_state, str_start, len);
                                patch->synth_state[len] = '\0';
                            }
                        }
                    }
                }
            }
        }
    }

    /* Parse audio_fx array */
    const char *fx_pos = strstr(json, "\"audio_fx\"");
    if (fx_pos) {
        const char *bracket = strchr(fx_pos, '[');
        if (bracket) {
            bracket++;
            while (patch->audio_fx_count < MAX_AUDIO_FX) {
                /* Find the start of next FX object */
                const char *obj_start = strchr(bracket, '{');
                if (!obj_start) break;

                /* Find matching closing brace (handle nested objects) */
                const char *obj_end = obj_start + 1;
                int depth = 1;
                while (*obj_end && depth > 0) {
                    if (*obj_end == '{') depth++;
                    else if (*obj_end == '}') depth--;
                    if (depth > 0) obj_end++;
                }
                if (!*obj_end || depth != 0) break;

                audio_fx_config_t *cfg = &patch->audio_fx[patch->audio_fx_count];
                memset(cfg, 0, sizeof(*cfg));

                /* Parse "type" field */
                const char *type_pos = strstr(obj_start, "\"type\"");
                if (!type_pos || type_pos > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }

                const char *colon = strchr(type_pos, ':');
                if (!colon || colon > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }

                const char *quote1 = strchr(colon, '"');
                if (!quote1 || quote1 > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }
                quote1++;

                const char *quote2 = strchr(quote1, '"');
                if (!quote2 || quote2 > obj_end) {
                    bracket = obj_end + 1;
                    continue;
                }

                int len = quote2 - quote1;
                if (len > 0 && len < MAX_NAME_LEN) {
                    strncpy(cfg->module, quote1, len);
                    cfg->module[len] = '\0';
                }

                /* Parse "params" object if present */
                const char *params_pos = strstr(obj_start, "\"params\"");
                {
                    char dbg[256];
                    snprintf(dbg, sizeof(dbg), "[parse] audio_fx type='%s' params_pos=%s",
                             cfg->module, params_pos ? "found" : "null");
                    parse_debug_log(dbg);
                }
                if (params_pos && params_pos < obj_end) {
                    const char *params_obj = strchr(params_pos, '{');
                    /* Find matching closing brace for params (handle nested objects) */
                    const char *params_end = NULL;
                    if (params_obj && params_obj < obj_end) {
                        const char *p = params_obj + 1;
                        int pdepth = 1;
                        while (*p && pdepth > 0 && p < obj_end) {
                            if (*p == '{') pdepth++;
                            else if (*p == '}') pdepth--;
                            if (pdepth > 0) p++;
                        }
                        if (pdepth == 0) params_end = p;
                    }
                    {
                        char dbg[256];
                        snprintf(dbg, sizeof(dbg), "[parse] params_obj=%p params_end=%p obj_end=%p check=%s",
                                 (void*)params_obj, (void*)params_end, (void*)obj_end,
                                 (params_obj && params_end && params_end <= obj_end) ? "pass" : "fail");
                        parse_debug_log(dbg);
                    }
                    if (params_obj && params_end && params_end <= obj_end) {
                        /* Check for nested "state" object in params */
                        cfg->state[0] = '\0';
                        const char *state_key = strstr(params_obj, "\"state\"");
                        if (state_key && state_key < params_end) {
                            const char *state_colon = strchr(state_key, ':');
                            if (state_colon && state_colon < params_end) {
                                /* Skip whitespace */
                                const char *sv = state_colon + 1;
                                while (sv < params_end && (*sv == ' ' || *sv == '\t' || *sv == '\n')) sv++;
                                if (*sv == '{') {
                                    /* Extract entire state object */
                                    const char *state_start = sv;
                                    const char *se = sv + 1;
                                    int sdepth = 1;
                                    while (*se && sdepth > 0 && se < params_end) {
                                        if (*se == '{') sdepth++;
                                        else if (*se == '}') sdepth--;
                                        if (sdepth > 0) se++;
                                    }
                                    if (sdepth == 0) {
                                        int slen = se - state_start + 1;
                                        if (slen > 0 && slen < MAX_FX_STATE_LEN) {
                                            strncpy(cfg->state, state_start, slen);
                                            cfg->state[slen] = '\0';
                                            parse_debug_log("[parse] Extracted audio_fx state object");
                                        }
                                    }
                                } else if (*sv == '"') {
                                    /* Extract state as opaque string (non-JSON formats like key=val;...) */
                                    const char *str_start = sv + 1;
                                    const char *str_end = str_start;
                                    while (str_end < params_end && *str_end != '"') {
                                        if (*str_end == '\\' && *(str_end + 1)) str_end++; /* skip escaped chars */
                                        str_end++;
                                    }
                                    if (*str_end == '"') {
                                        int slen = str_end - str_start;
                                        if (slen > 0 && slen < MAX_FX_STATE_LEN) {
                                            strncpy(cfg->state, str_start, slen);
                                            cfg->state[slen] = '\0';
                                            parse_debug_log("[parse] Extracted audio_fx state string");
                                        }
                                    }
                                }
                            }
                        }
                        /* Parse key-value pairs in params object */
                        const char *scan = params_obj;
                        while (cfg->param_count < MAX_MIDI_FX_PARAMS && scan < params_end) {
                            const char *key_start = strchr(scan, '"');
                            if (!key_start || key_start >= params_end) break;

                            const char *key_end = strchr(key_start + 1, '"');
                            if (!key_end || key_end >= params_end) break;

                            int key_len = key_end - key_start - 1;
                            if (key_len <= 0 || key_len >= 32) {
                                scan = key_end + 1;
                                continue;
                            }

                            /* Skip "state" key - already extracted separately */
                            if (key_len == 5 && strncmp(key_start + 1, "state", 5) == 0) {
                                /* Skip to end of state object */
                                const char *sc = strchr(key_end, ':');
                                if (sc && sc < params_end) {
                                    const char *sv = sc + 1;
                                    while (sv < params_end && (*sv == ' ' || *sv == '\t' || *sv == '\n')) sv++;
                                    if (*sv == '{') {
                                        /* Skip object */
                                        int od = 1;
                                        sv++;
                                        while (*sv && od > 0 && sv < params_end) {
                                            if (*sv == '{') od++;
                                            else if (*sv == '}') od--;
                                            sv++;
                                        }
                                        scan = sv;
                                    } else if (*sv == '"') {
                                        /* Skip string value */
                                        sv++;
                                        while (sv < params_end && *sv != '"') {
                                            if (*sv == '\\' && *(sv + 1)) sv++;
                                            sv++;
                                        }
                                        if (*sv == '"') sv++;
                                        scan = sv;
                                    } else {
                                        scan = sv;
                                    }
                                } else {
                                    scan = key_end + 1;
                                }
                                continue;
                            }

                            /* Find value after colon */
                            const char *val_colon = strchr(key_end, ':');
                            if (!val_colon || val_colon >= params_end) {
                                scan = key_end + 1;
                                continue;
                            }

                            /* Skip whitespace */
                            const char *val_start = val_colon + 1;
                            while (val_start < params_end && (*val_start == ' ' || *val_start == '\t')) {
                                val_start++;
                            }

                            /* Skip object values (other nested objects besides state) */
                            if (*val_start == '{') {
                                int od = 1;
                                const char *ov = val_start + 1;
                                while (*ov && od > 0 && ov < params_end) {
                                    if (*ov == '{') od++;
                                    else if (*ov == '}') od--;
                                    ov++;
                                }
                                scan = ov;
                                continue;
                            }

                            char val_buf[32] = "";
                            if (*val_start == '"') {
                                /* String value */
                                val_start++;
                                const char *val_end = strchr(val_start, '"');
                                if (val_end && val_end < params_end) {
                                    int val_len = val_end - val_start;
                                    if (val_len > 31) val_len = 31;
                                    strncpy(val_buf, val_start, val_len);
                                    val_buf[val_len] = '\0';
                                    scan = val_end + 1;
                                } else {
                                    scan = val_start;
                                    continue;
                                }
                            } else {
                                /* Numeric value */
                                const char *val_end = val_start;
                                while (val_end < params_end && *val_end != ',' && *val_end != '}' &&
                                       *val_end != ' ' && *val_end != '\n' && *val_end != '\r') {
                                    val_end++;
                                }
                                int val_len = val_end - val_start;
                                if (val_len > 31) val_len = 31;
                                strncpy(val_buf, val_start, val_len);
                                val_buf[val_len] = '\0';
                                scan = val_end;
                            }

                            /* Store the param */
                            strncpy(cfg->params[cfg->param_count].key, key_start + 1, key_len);
                            cfg->params[cfg->param_count].key[key_len] = '\0';
                            strncpy(cfg->params[cfg->param_count].val, val_buf, 31);
                            cfg->params[cfg->param_count].val[31] = '\0';
                            {
                                char dbg[256];
                                snprintf(dbg, sizeof(dbg), "[parse] stored param[%d]: key='%s' val='%s'",
                                         cfg->param_count, cfg->params[cfg->param_count].key, val_buf);
                                parse_debug_log(dbg);
                            }
                            cfg->param_count++;
                        }
                    }
                }

                {
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "[parse] audio_fx[%d] param_count: %d",
                             patch->audio_fx_count, cfg->param_count);
                    parse_debug_log(dbg);
                }
                patch->audio_fx_count++;
                bracket = obj_end + 1;
            }
        }
    }

    /* Parse midi_fx array (native MIDI FX modules with params) */
    const char *midi_fx_pos = strstr(json, "\"midi_fx\"");
    if (midi_fx_pos) {
        const char *arr_start = strchr(midi_fx_pos, '[');
        const char *arr_end = arr_start ? strchr(arr_start, ']') : NULL;

        if (arr_start && arr_end) {
            const char *obj_start = arr_start;
            while (patch->midi_fx_count < MAX_MIDI_FX) {
                obj_start = strchr(obj_start + 1, '{');
                if (!obj_start || obj_start > arr_end) break;

                /* Find matching closing brace for midi_fx object (handle nested objects) */
                const char *obj_end = obj_start + 1;
                int mfx_depth = 1;
                while (*obj_end && mfx_depth > 0 && obj_end < arr_end) {
                    if (*obj_end == '{') mfx_depth++;
                    else if (*obj_end == '}') mfx_depth--;
                    if (mfx_depth > 0) obj_end++;
                }
                if (!*obj_end || mfx_depth != 0) break;

                midi_fx_config_t *cfg = &patch->midi_fx[patch->midi_fx_count];
                memset(cfg, 0, sizeof(*cfg));

                /* Parse "type" (module name) - consistent with audio_fx format */
                const char *type_pos = strstr(obj_start, "\"type\"");
                if (type_pos && type_pos < obj_end) {
                    const char *q1 = strchr(strchr(type_pos, ':'), '"');
                    if (q1 && q1 < obj_end) {
                        const char *q2 = strchr(q1 + 1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1 - 1;
                            if (len > MAX_NAME_LEN - 1) len = MAX_NAME_LEN - 1;
                            strncpy(cfg->module, q1 + 1, len);
                            cfg->module[len] = '\0';
                        }
                    }
                }

                /* Check for "params" object and extract state if present */
                cfg->state[0] = '\0';
                const char *params_pos = strstr(obj_start, "\"params\"");
                if (params_pos && params_pos < obj_end) {
                    const char *params_obj = strchr(params_pos, '{');
                    if (params_obj && params_obj < obj_end) {
                        /* Find matching closing brace for params */
                        const char *params_end = params_obj + 1;
                        int pdepth = 1;
                        while (*params_end && pdepth > 0 && params_end < obj_end) {
                            if (*params_end == '{') pdepth++;
                            else if (*params_end == '}') pdepth--;
                            if (pdepth > 0) params_end++;
                        }
                        if (pdepth == 0) {
                            /* Look for state object inside params */
                            const char *state_key = strstr(params_obj, "\"state\"");
                            if (state_key && state_key < params_end) {
                                const char *state_colon = strchr(state_key, ':');
                                if (state_colon && state_colon < params_end) {
                                    const char *sv = state_colon + 1;
                                    while (sv < params_end && (*sv == ' ' || *sv == '\t' || *sv == '\n')) sv++;
                                    if (*sv == '{') {
                                        const char *state_start = sv;
                                        const char *se = sv + 1;
                                        int sdepth = 1;
                                        while (*se && sdepth > 0 && se < params_end) {
                                            if (*se == '{') sdepth++;
                                            else if (*se == '}') sdepth--;
                                            if (sdepth > 0) se++;
                                        }
                                        if (sdepth == 0) {
                                            int slen = se - state_start + 1;
                                            if (slen > 0 && slen < MAX_FX_STATE_LEN) {
                                                strncpy(cfg->state, state_start, slen);
                                                cfg->state[slen] = '\0';
                                                parse_debug_log("[parse] Extracted midi_fx state object");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                /* Parse other fields as params (skip "type" and "params") */
                const char *scan = obj_start;
                while (cfg->param_count < MAX_MIDI_FX_PARAMS && scan < obj_end) {
                    const char *key_start = strchr(scan, '"');
                    if (!key_start || key_start >= obj_end) break;

                    const char *key_end = strchr(key_start + 1, '"');
                    if (!key_end || key_end >= obj_end) break;

                    int key_len = key_end - key_start - 1;
                    if (key_len <= 0 || key_len >= 32) {
                        scan = key_end + 1;
                        continue;
                    }

                    /* Skip "type" - already parsed as module name */
                    if (key_len == 4 && strncmp(key_start + 1, "type", 4) == 0) {
                        const char *colon = strchr(key_end, ':');
                        if (colon && colon < obj_end) {
                            const char *vq1 = strchr(colon, '"');
                            if (vq1 && vq1 < obj_end) {
                                const char *vq2 = strchr(vq1 + 1, '"');
                                scan = vq2 ? vq2 + 1 : obj_end;
                            } else {
                                scan = obj_end;
                            }
                        } else {
                            scan = key_end + 1;
                        }
                        continue;
                    }

                    /* Skip "params" - contains nested state object */
                    if (key_len == 6 && strncmp(key_start + 1, "params", 6) == 0) {
                        const char *colon = strchr(key_end, ':');
                        if (colon && colon < obj_end) {
                            const char *pv = colon + 1;
                            while (pv < obj_end && (*pv == ' ' || *pv == '\t' || *pv == '\n')) pv++;
                            if (*pv == '{') {
                                int pd = 1;
                                pv++;
                                while (*pv && pd > 0 && pv < obj_end) {
                                    if (*pv == '{') pd++;
                                    else if (*pv == '}') pd--;
                                    pv++;
                                }
                                scan = pv;
                            } else {
                                scan = pv;
                            }
                        } else {
                            scan = key_end + 1;
                        }
                        continue;
                    }

                    /* Found a param key */
                    midi_fx_param_t *p = &cfg->params[cfg->param_count];
                    strncpy(p->key, key_start + 1, key_len);
                    p->key[key_len] = '\0';

                    /* Get value */
                    const char *colon = strchr(key_end, ':');
                    if (!colon || colon >= obj_end) {
                        scan = key_end + 1;
                        continue;
                    }

                    /* Skip whitespace */
                    const char *val_start = colon + 1;
                    while (val_start < obj_end && (*val_start == ' ' || *val_start == '\t')) val_start++;

                    if (val_start >= obj_end) {
                        scan = obj_end;
                        continue;
                    }

                    if (*val_start == '"') {
                        /* String value */
                        const char *vq1 = val_start;
                        const char *vq2 = strchr(vq1 + 1, '"');
                        if (vq2 && vq2 < obj_end) {
                            int val_len = vq2 - vq1 - 1;
                            if (val_len > 31) val_len = 31;
                            strncpy(p->val, vq1 + 1, val_len);
                            p->val[val_len] = '\0';
                            cfg->param_count++;
                            scan = vq2 + 1;
                        } else {
                            scan = obj_end;
                        }
                    } else {
                        /* Numeric value */
                        const char *val_end = val_start;
                        while (val_end < obj_end && *val_end != ',' && *val_end != '}') val_end++;
                        int val_len = val_end - val_start;
                        if (val_len > 31) val_len = 31;
                        strncpy(p->val, val_start, val_len);
                        p->val[val_len] = '\0';
                        /* Trim trailing whitespace */
                        while (val_len > 0 && (p->val[val_len-1] == ' ' || p->val[val_len-1] == '\t' || p->val[val_len-1] == '\n')) {
                            p->val[--val_len] = '\0';
                        }
                        cfg->param_count++;
                        scan = val_end;
                    }
                }

                if (cfg->module[0]) {
                    patch->midi_fx_count++;
                }

                obj_start = obj_end;
            }
        }
    }

    /* Parse knob_mappings - simplified */
    const char *mappings_pos = strstr(json, "\"knob_mappings\"");
    if (mappings_pos) {
        const char *arr_start = strchr(mappings_pos, '[');
        const char *arr_end = arr_start ? strchr(arr_start, ']') : NULL;

        if (arr_start && arr_end) {
            const char *obj_start = arr_start;
            while (patch->knob_mapping_count < MAX_KNOB_MAPPINGS) {
                obj_start = strchr(obj_start + 1, '{');
                if (!obj_start || obj_start > arr_end) break;

                const char *obj_end = strchr(obj_start, '}');
                if (!obj_end || obj_end > arr_end) break;

                knob_mapping_t *m = &patch->knob_mappings[patch->knob_mapping_count];

                /* Parse cc */
                const char *cc_pos = strstr(obj_start, "\"cc\"");
                if (cc_pos && cc_pos < obj_end) {
                    const char *colon = strchr(cc_pos, ':');
                    if (colon) m->cc = atoi(colon + 1);
                }

                /* Parse target */
                const char *target_pos = strstr(obj_start, "\"target\"");
                if (target_pos && target_pos < obj_end) {
                    const char *q1 = strchr(strchr(target_pos, ':'), '"');
                    if (q1 && q1 < obj_end) {
                        const char *q2 = strchr(q1 + 1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1 - 1;
                            if (len > 15) len = 15;
                            strncpy(m->target, q1 + 1, len);
                        }
                    }
                }

                /* Parse param */
                const char *param_pos = strstr(obj_start, "\"param\"");
                if (param_pos && param_pos < obj_end) {
                    const char *q1 = strchr(strchr(param_pos, ':'), '"');
                    if (q1 && q1 < obj_end) {
                        const char *q2 = strchr(q1 + 1, '"');
                        if (q2 && q2 < obj_end) {
                            int len = q2 - q1 - 1;
                            if (len > 31) len = 31;
                            strncpy(m->param, q1 + 1, len);
                        }
                    }
                }

                /* Parse saved value if present */
                m->current_value = -999999.0f;  /* Sentinel: no saved value */
                const char *val_pos = strstr(obj_start, "\"value\"");
                if (val_pos && val_pos < obj_end) {
                    const char *colon = strchr(val_pos, ':');
                    if (colon) m->current_value = strtof(colon + 1, NULL);
                }

                if (m->cc >= KNOB_CC_START && m->cc <= KNOB_CC_END && m->param[0]) {
                    patch->knob_mapping_count++;
                }

                obj_start = obj_end;
            }
        }
    }

    /* Parse receive_channel and forward_channel (top-level in chain content) */
    json_get_int(json, "receive_channel", &patch->receive_channel);
    json_get_int(json, "forward_channel", &patch->forward_channel);

    /* Parse midi_fx_pre_mode (top-level; absence = Post). */
    json_get_int(json, "midi_fx_pre_mode", &patch->midi_fx_pre_mode);

    /* Parse LFO config: "lfos": { "lfo1": { ... }, "lfo2": ... } */
    const char *lfos_pos = strstr(json, "\"lfos\"");
    if (lfos_pos) {
        for (int i = 0; i < LFO_COUNT; i++) {
            char lfo_key[8];
            snprintf(lfo_key, sizeof(lfo_key), "\"lfo%d\"", i + 1);
            const char *lfo_pos = strstr(lfos_pos, lfo_key);
            if (!lfo_pos) continue;

            /* Check for null */
            const char *colon = strchr(lfo_pos + strlen(lfo_key), ':');
            if (!colon) continue;
            const char *val = colon + 1;
            while (*val == ' ' || *val == '\t' || *val == '\n') val++;
            if (strncmp(val, "null", 4) == 0) continue;  /* null = inactive */

            const char *obj = strchr(val, '{');
            if (!obj) continue;
            const char *obj_end = obj + 1;
            int depth = 1;
            while (*obj_end && depth > 0) {
                if (*obj_end == '{') depth++;
                else if (*obj_end == '}') depth--;
                if (depth > 0) obj_end++;
            }

            lfo_state_t *lfo = &patch->lfos[i];
            json_get_int(obj, "enabled", &lfo->enabled);
            json_get_int(obj, "shape", &lfo->shape);
            json_get_int(obj, "sync", &lfo->sync);
            json_get_int(obj, "rate_div", &lfo->rate_div);

            /* Migrate old 14-entry division table index to new 27-entry table */
            int div_version = 0;
            if (json_get_int(obj, "division_table_version", &div_version) != 0) {
                /* No version field means old format - migrate */
                lfo->rate_div = lfo_migrate_division_index(lfo->rate_div);
            }

            json_get_float(obj, "rate_hz", &lfo->rate_hz);
            json_get_float(obj, "depth", &lfo->depth);
            json_get_float(obj, "phase_offset", &lfo->phase_offset);

            json_get_string(obj, "target", lfo->target, sizeof(lfo->target));
            json_get_string(obj, "target_param", lfo->param, sizeof(lfo->param));
            json_get_int(obj, "polarity", &lfo->bipolar);
            json_get_int(obj, "retrigger", &lfo->retrigger);

            lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
        }
    }

    free(json);
    return 0;
}

/* V2 load patch */
int v2_load_from_patch_info(chain_instance_t *inst, patch_info_t *patch) {
    char msg[256];

    if (!inst || !patch) {
        return -1;
    }

    snprintf(msg, sizeof(msg), "Loading patch: %s (synth=%s, %d FX)",
             patch->name, patch->synth_module, patch->audio_fx_count);
    v2_chain_log(inst, msg);

    /* Unload existing */
    v2_synth_panic(inst);
    v2_unload_all_midi_fx(inst);
    v2_unload_all_audio_fx(inst);
    v2_unload_synth(inst);

    /* Load synth */
    if (patch->synth_module[0]) {
        if (v2_load_synth(inst, patch->synth_module) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load synth: %s", patch->synth_module);
            v2_chain_log(inst, msg);
            return -1;
        }

        /* Set preset first (state may override params set by preset) */
        char preset_str[16];
        snprintf(preset_str, sizeof(preset_str), "%d", patch->synth_preset);
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->set_param) {
            inst->synth_plugin_v2->set_param(inst->synth_instance, "preset", preset_str);
        }

        /* Reset mod wheel (CC 1) to 0 BEFORE state restore.
         * This clears stale mod wheel values from Move's track state,
         * but must happen before state restore so saved param values
         * (e.g. Braids FM which is mapped to CC 1) are not clobbered. */
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            for (int ch = 0; ch < 16; ch++) {
                uint8_t mod_reset[3] = {(uint8_t)(0xB0 | ch), 1, 0};
                inst->synth_plugin_v2->on_midi(inst->synth_instance, mod_reset, 3, MOVE_MIDI_SOURCE_HOST);
            }
        }

        /* Apply saved state if present */
        if (patch->synth_state[0] && inst->synth_plugin_v2 && inst->synth_instance) {
            snprintf(msg, sizeof(msg), "Applying synth state: %.50s...", patch->synth_state);
            v2_chain_log(inst, msg);
            inst->synth_plugin_v2->set_param(inst->synth_instance, "state", patch->synth_state);
        }
    }

    /* Load audio FX */
    for (int i = 0; i < patch->audio_fx_count; i++) {
        audio_fx_config_t *cfg = &patch->audio_fx[i];
        {
            char dbg[256];
            snprintf(dbg, sizeof(dbg), "[load] Loading audio_fx[%d]: module='%s' param_count=%d",
                     i, cfg->module, cfg->param_count);
            parse_debug_log(dbg);
        }
        if (v2_load_audio_fx(inst, cfg->module) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load FX: %s", cfg->module);
            v2_chain_log(inst, msg);
            parse_debug_log("[load] FX load failed!");
            /* Continue loading other FX */
            continue;
        }

        /* Apply audio FX parameters (e.g., plugin_id for CLAP) */
        int fx_idx = inst->fx_count - 1;  /* Just loaded */
        {
            char dbg[256];
            snprintf(dbg, sizeof(dbg), "[load] FX loaded, fx_idx=%d is_v2=%d plugins_v2=%p instances=%p",
                     fx_idx, inst->fx_is_v2[fx_idx], (void*)inst->fx_plugins_v2[fx_idx],
                     (void*)inst->fx_instances[fx_idx]);
            parse_debug_log(dbg);
        }
        if (fx_idx >= 0 && fx_idx < MAX_AUDIO_FX && cfg->param_count > 0) {
            if (inst->fx_is_v2[fx_idx] && inst->fx_plugins_v2[fx_idx] && inst->fx_instances[fx_idx]) {
                for (int p = 0; p < cfg->param_count; p++) {
                    snprintf(msg, sizeof(msg), "Setting FX%d param: %s = %s",
                             fx_idx + 1, cfg->params[p].key, cfg->params[p].val);
                    v2_chain_log(inst, msg);
                    parse_debug_log(msg);
                    inst->fx_plugins_v2[fx_idx]->set_param(inst->fx_instances[fx_idx],
                                                           cfg->params[p].key, cfg->params[p].val);
                }
            } else {
                parse_debug_log("[load] No API available for setting params!");
            }
        } else {
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "[load] Skipping params: fx_idx=%d param_count=%d", fx_idx, cfg->param_count);
            parse_debug_log(dbg);
        }

        /* Apply saved audio FX state if present */
        if (cfg->state[0] && fx_idx >= 0 && fx_idx < MAX_AUDIO_FX) {
            if (inst->fx_is_v2[fx_idx] && inst->fx_plugins_v2[fx_idx] && inst->fx_instances[fx_idx]) {
                snprintf(msg, sizeof(msg), "Applying FX%d state: %.50s...", fx_idx + 1, cfg->state);
                v2_chain_log(inst, msg);
                inst->fx_plugins_v2[fx_idx]->set_param(inst->fx_instances[fx_idx], "state", cfg->state);
            }
        }
    }

    /* Load MIDI FX */
    for (int i = 0; i < patch->midi_fx_count; i++) {
        midi_fx_config_t *cfg = &patch->midi_fx[i];
        if (v2_load_midi_fx(inst, cfg->module) != 0) {
            snprintf(msg, sizeof(msg), "Failed to load MIDI FX: %s", cfg->module);
            v2_chain_log(inst, msg);
            continue;  /* Skip params if load failed */
        }

        /* Apply MIDI FX parameters */
        int fx_idx = inst->midi_fx_count - 1;  /* Just loaded */
        if (fx_idx >= 0 && fx_idx < MAX_MIDI_FX && inst->midi_fx_plugins[fx_idx]) {
            midi_fx_api_v1_t *api = inst->midi_fx_plugins[fx_idx];
            void *instance = inst->midi_fx_instances[fx_idx];
            if (api->set_param && instance) {
                for (int p = 0; p < cfg->param_count; p++) {
                    snprintf(msg, sizeof(msg), "Setting MIDI FX param: %s = %s",
                             cfg->params[p].key, cfg->params[p].val);
                    v2_chain_log(inst, msg);
                    api->set_param(instance, cfg->params[p].key, cfg->params[p].val);
                }

                /* Apply saved MIDI FX state if present */
                if (cfg->state[0]) {
                    snprintf(msg, sizeof(msg), "Applying MIDI FX state: %.50s...", cfg->state);
                    v2_chain_log(inst, msg);
                    api->set_param(instance, "state", cfg->state);
                }
            }
        }
    }

    /* Copy knob mappings to instance and initialize current values */
    inst->knob_mapping_count = patch->knob_mapping_count;
    memcpy(inst->knob_mappings, patch->knob_mappings,
           sizeof(knob_mapping_t) * patch->knob_mapping_count);

    /* Sync knob current_value from actual DSP state.
     * The saved value may be stale if params were changed via module UI,
     * so always read the real value from the plugin after state restore. */
    for (int i = 0; i < inst->knob_mapping_count; i++) {
        const char *target = inst->knob_mappings[i].target;
        const char *param = inst->knob_mappings[i].param;

        char val_buf[64];
        int got = -1;
        if (strcmp(target, "synth") == 0 && inst->synth_plugin_v2 && inst->synth_instance) {
            got = inst->synth_plugin_v2->get_param(inst->synth_instance, param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "fx1") == 0 && inst->fx_count > 0 &&
                   inst->fx_is_v2[0] && inst->fx_plugins_v2[0] && inst->fx_instances[0]) {
            got = inst->fx_plugins_v2[0]->get_param(inst->fx_instances[0], param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "fx2") == 0 && inst->fx_count > 1 &&
                   inst->fx_is_v2[1] && inst->fx_plugins_v2[1] && inst->fx_instances[1]) {
            got = inst->fx_plugins_v2[1]->get_param(inst->fx_instances[1], param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "midi_fx1") == 0 && inst->midi_fx_count > 0 &&
                   inst->midi_fx_plugins[0] && inst->midi_fx_instances[0]) {
            got = inst->midi_fx_plugins[0]->get_param(inst->midi_fx_instances[0], param, val_buf, sizeof(val_buf));
        } else if (strcmp(target, "midi_fx2") == 0 && inst->midi_fx_count > 1 &&
                   inst->midi_fx_plugins[1] && inst->midi_fx_instances[1]) {
            got = inst->midi_fx_plugins[1]->get_param(inst->midi_fx_instances[1], param, val_buf, sizeof(val_buf));
        }

        chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
        if (got > 0) {
            inst->knob_mappings[i].current_value = dsp_value_to_float(
                val_buf, pinfo, pinfo ? (pinfo->min_val + pinfo->max_val) / 2.0f : 0.5f);
        } else if (pinfo) {
            /* No DSP read — use saved value or midpoint */
            float saved = patch->knob_mappings[i].current_value;
            if (saved > -999998.0f) {
                if (saved < pinfo->min_val) saved = pinfo->min_val;
                if (saved > pinfo->max_val) saved = pinfo->max_val;
                inst->knob_mappings[i].current_value = saved;
            } else {
                inst->knob_mappings[i].current_value = (pinfo->min_val + pinfo->max_val) / 2.0f;
            }
        } else {
            inst->knob_mappings[i].current_value = 0.5f;
        }
    }

    /* Copy other settings */
    inst->midi_input = patch->midi_input;

    /* Restore LFO config from patch (clear old sources first) */
    for (int i = 0; i < LFO_COUNT; i++) {
        char source_id[8];
        snprintf(source_id, sizeof(source_id), "lfo%d", i + 1);
        chain_mod_clear_source(inst, source_id);
        inst->lfos[i] = patch->lfos[i];
        /* Reset runtime state */
        inst->lfos[i].last_sh_value = 0.0f;
        inst->lfos[i].prev_wrap = 0;
        /* Reset phase for synced LFOs on patch load */
        if (inst->lfos[i].sync) {
            inst->lfos[i].phase = 0.0;
        }
    }

    snprintf(msg, sizeof(msg), "Patch loaded: %s", patch->name);
    v2_chain_log(inst, msg);

    return 0;
}

int v2_load_patch(chain_instance_t *inst, int patch_idx) {
    if (!inst || patch_idx < 0 || patch_idx >= inst->patch_count) {
        return -1;
    }

    int rc = v2_load_from_patch_info(inst, &inst->patches[patch_idx]);
    if (rc == 0) {
        inst->current_patch = patch_idx;
        inst->midi_fx_pre_mode = inst->patches[patch_idx].midi_fx_pre_mode ? 1 : 0;
        inst->dirty = 0;
    }
    return rc;
}


