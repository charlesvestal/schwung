/* shadow_set_pages.c - Set page switching and per-set state management
 * Extracted from move_anything_shim.c for maintainability. */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "shadow_set_pages.h"
#include "shadow_sampler.h"  /* for SAMPLER_SETS_DIR, sampler_read_set_tempo */

/* ============================================================================
 * Globals
 * ============================================================================ */

int set_page_current = 0;            /* 0-7 */
int set_page_overlay_active = 0;
int set_page_overlay_timeout = 0;    /* Frames remaining for toast */
int set_page_loading = 0;            /* 1 = pre-restart "Loading...", 0 = post-boot */
volatile int set_page_change_in_flight = 0;  /* Guard against double-press */

/* Set tracking globals */
float sampler_set_tempo = 0.0f;              /* 0 = not yet detected */
char sampler_current_set_name[128] = "";      /* current set name */
char sampler_current_set_uuid[64] = "";       /* UUID from Sets/<UUID>/<Name>/ path */
int sampler_last_song_index = -1;             /* last seen currentSongIndex */
int sampler_pending_song_index = -1;          /* unresolved currentSongIndex without UUID dir yet */
uint32_t sampler_pending_set_seq = 0;         /* synthetic pending-set UUID sequence */

/* Xattr names to preserve when stashing/restoring set UUID dirs */
static const char *set_page_xattr_names[] = {
    "user.song-index",
    "user.song-color",
    "user.last-modified-time",
    "user.was-externally-modified",
    "user.local-cloud-state",
    NULL
};

/* ============================================================================
 * Host callbacks (set during init)
 * ============================================================================ */

static set_pages_host_t host;

void set_pages_init(const set_pages_host_t *h) {
    host = *h;
}

/* ============================================================================
 * Utility functions
 * ============================================================================ */

/* Ensure a directory exists, creating it if needed (like mkdir -p) */
void shadow_ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        const char *mkdir_argv[] = { "mkdir", "-p", dir, NULL };
        host.run_command(mkdir_argv);
    }
}

/* Copy a single file from src_path to dst_path. Returns 1 on success. */
int shadow_copy_file(const char *src_path, const char *dst_path) {
    FILE *sf = fopen(src_path, "r");
    if (!sf) return 0;
    fseek(sf, 0, SEEK_END);
    long sz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(sf); return 0; }
    char *buf = malloc(sz);
    if (!buf) { fclose(sf); return 0; }
    size_t nr = fread(buf, 1, sz, sf);
    fclose(sf);
    if (nr == 0) { free(buf); return 0; }
    FILE *df = fopen(dst_path, "w");
    if (!df) { free(buf); return 0; }
    size_t nw = fwrite(buf, 1, nr, df);
    fclose(df);
    free(buf);
    if (nw != nr) { unlink(dst_path); return 0; }
    return 1;
}

/* ============================================================================
 * Batch migration
 * ============================================================================ */

void shadow_batch_migrate_sets(void) {
    char migrated_path[256];
    snprintf(migrated_path, sizeof(migrated_path), SET_STATE_DIR "/.migrated");
    struct stat mst;
    if (stat(migrated_path, &mst) == 0) return;  /* Already migrated */

    host.log("Batch migration: seeding per-set state for all existing sets");
    shadow_ensure_dir(SET_STATE_DIR);

    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) {
        host.log("Batch migration: cannot open Sets dir, writing .migrated anyway");
        goto write_marker;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Each entry under Sets/ is a UUID directory */
        const char *uuid = entry->d_name;
        char set_dir[512];
        snprintf(set_dir, sizeof(set_dir), SET_STATE_DIR "/%s", uuid);

        /* Skip if already has state files */
        char test_path[768];
        snprintf(test_path, sizeof(test_path), "%s/slot_0.json", set_dir);
        struct stat tst;
        if (stat(test_path, &tst) == 0) continue;

        shadow_ensure_dir(set_dir);

        /* Copy slot state files from default dir */
        for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
            char src[512], dst[512];
            snprintf(src, sizeof(src), SLOT_STATE_DIR "/slot_%d.json", i);
            snprintf(dst, sizeof(dst), "%s/slot_%d.json", set_dir, i);
            shadow_copy_file(src, dst);

            snprintf(src, sizeof(src), SLOT_STATE_DIR "/master_fx_%d.json", i);
            snprintf(dst, sizeof(dst), "%s/master_fx_%d.json", set_dir, i);
            shadow_copy_file(src, dst);
        }

        /* Also copy shadow_chain_config.json if it exists */
        {
            char src[512], dst[512];
            snprintf(src, sizeof(src), "/data/UserData/move-anything/" SHADOW_CHAIN_CONFIG_FILENAME);
            snprintf(dst, sizeof(dst), "%s/" SHADOW_CHAIN_CONFIG_FILENAME, set_dir);
            shadow_copy_file(src, dst);
        }

        count++;
    }
    closedir(sets_dir);

    char m[128];
    snprintf(m, sizeof(m), "Batch migration: seeded %d sets from default slot_state", count);
    host.log(m);

write_marker:
    {
        FILE *mf = fopen(migrated_path, "w");
        if (mf) {
            fputs("1\n", mf);
            fclose(mf);
        }
    }
}

/* ============================================================================
 * Config save/load
 * ============================================================================ */

void shadow_save_config_to_dir(const char *dir) {
    shadow_ensure_dir(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/" SHADOW_CHAIN_CONFIG_FILENAME, dir);

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"slots\": [\n");
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        int display_ch = host.chain_slots[i].channel < 0
            ? 0 : host.chain_slots[i].channel + 1;
        int display_fwd = host.chain_slots[i].forward_channel >= 0
            ? host.chain_slots[i].forward_channel + 1
            : host.chain_slots[i].forward_channel;
        fprintf(f, "    {\"name\": \"%s\", \"channel\": %d, \"volume\": %.3f, \"forward_channel\": %d, \"muted\": %d, \"soloed\": %d}%s\n",
                host.chain_slots[i].patch_name, display_ch,
                host.chain_slots[i].volume, display_fwd,
                host.chain_slots[i].muted, host.chain_slots[i].soloed,
                i < SHADOW_CHAIN_INSTANCES - 1 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

int shadow_load_config_from_dir(const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/" SHADOW_CHAIN_CONFIG_FILENAME, dir);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4096) { fclose(f); return 0; }

    char *json = malloc(size + 1);
    if (!json) { fclose(f); return 0; }
    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    /* Parse slots - same logic as shadow_chain_load_config */
    char *cursor = json;
    *host.solo_count = 0;
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        char *name_pos = strstr(cursor, "\"name\"");
        if (!name_pos) break;
        char *colon = strchr(name_pos, ':');
        if (colon) {
            char *q1 = strchr(colon, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2 && q2 > q1) {
                    size_t len = (size_t)(q2 - q1);
                    if (len < sizeof(host.chain_slots[i].patch_name)) {
                        memcpy(host.chain_slots[i].patch_name, q1, len);
                        host.chain_slots[i].patch_name[len] = '\0';
                    }
                }
            }
        }
        char *chan_pos = strstr(name_pos, "\"channel\"");
        if (chan_pos) {
            char *chan_colon = strchr(chan_pos, ':');
            if (chan_colon) {
                int ch = atoi(chan_colon + 1);
                if (ch >= 0 && ch <= 16)
                    host.chain_slots[i].channel = host.chain_parse_channel(ch);
            }
            cursor = chan_pos + 8;
        } else {
            cursor = name_pos + 6;
        }
        char *vol_pos = strstr(name_pos, "\"volume\"");
        if (vol_pos) {
            char *vol_colon = strchr(vol_pos, ':');
            if (vol_colon) {
                float vol = atof(vol_colon + 1);
                if (vol >= 0.0f && vol <= 1.0f)
                    host.chain_slots[i].volume = vol;
            }
        }
        char *fwd_pos = strstr(name_pos, "\"forward_channel\"");
        if (fwd_pos) {
            char *fwd_colon = strchr(fwd_pos, ':');
            if (fwd_colon) {
                int ch = atoi(fwd_colon + 1);
                if (ch >= -2 && ch <= 16)
                    host.chain_slots[i].forward_channel = (ch > 0) ? ch - 1 : ch;
            }
        }
        char *muted_pos = strstr(name_pos, "\"muted\"");
        if (muted_pos) {
            char *muted_colon = strchr(muted_pos, ':');
            if (muted_colon) {
                host.chain_slots[i].muted = atoi(muted_colon + 1);
            }
        }
        char *soloed_pos = strstr(name_pos, "\"soloed\"");
        if (soloed_pos) {
            char *soloed_colon = strchr(soloed_pos, ':');
            if (soloed_colon) {
                host.chain_slots[i].soloed = atoi(soloed_colon + 1);
                if (host.chain_slots[i].soloed) (*host.solo_count)++;
            }
        }
    }
    free(json);
    host.ui_state_refresh();
    return 1;
}

/* ============================================================================
 * Set detection
 * ============================================================================ */

/* Find Song.abl size for a given UUID by scanning its subdirectory.
 * Returns file size, or -1 if not found. */
static long shadow_get_song_abl_size(const char *uuid) {
    char uuid_path[512];
    snprintf(uuid_path, sizeof(uuid_path), "%s/%s", SAMPLER_SETS_DIR, uuid);
    DIR *d = opendir(uuid_path);
    if (!d) return -1;
    struct dirent *sub;
    long result = -1;
    while ((sub = readdir(d)) != NULL) {
        if (sub->d_name[0] == '.') continue;
        char song_path[768];
        snprintf(song_path, sizeof(song_path), "%s/%s/Song.abl", uuid_path, sub->d_name);
        struct stat st;
        if (stat(song_path, &st) == 0 && S_ISREG(st.st_mode)) {
            result = (long)st.st_size;
            break;
        }
    }
    closedir(d);
    return result;
}

/* Returns non-zero if set name indicates user asked for duplication. */
static int shadow_set_name_looks_like_copy(const char *set_name) {
    if (!set_name || !set_name[0]) return 0;
    if (strcasestr(set_name, "copy")) return 1;
    if (strcasestr(set_name, "duplicate")) return 1;
    return 0;
}

/* Detect if a new set is a copy of an existing tracked set.
 * Compares Song.abl file sizes between the new set and all sets
 * that have per-set state directories.
 * Returns 1 and fills copy_source_uuid if a likely source is found. */
static int shadow_detect_copy_source(const char *set_name, const char *new_uuid,
                                     char *copy_source_uuid, int buf_len) {
    copy_source_uuid[0] = '\0';
    if (!shadow_set_name_looks_like_copy(set_name)) {
        return 0;
    }

    /* Get new set's Song.abl size */
    long new_size = shadow_get_song_abl_size(new_uuid);
    if (new_size <= 0) return 0;

    /* Scan set_state/ for existing tracked sets */
    DIR *state_dir = opendir(SET_STATE_DIR);
    if (!state_dir) return 0;

    int match_count = 0;
    char best_uuid[64] = "";
    struct dirent *entry;
    while ((entry = readdir(state_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, new_uuid) == 0) continue;  /* Skip self */

        /* Check if this tracked set's Song.abl matches */
        long existing_size = shadow_get_song_abl_size(entry->d_name);
        if (existing_size == new_size) {
            snprintf(best_uuid, sizeof(best_uuid), "%s", entry->d_name);
            match_count++;
        }
    }
    closedir(state_dir);

    if (match_count == 1) {
        snprintf(copy_source_uuid, buf_len, "%s", best_uuid);
        return 1;
    }

    return 0;
}

/* Handle a Set being loaded — called from Settings.json poll.
 * set_name: human-readable name (e.g. "My Song")
 * uuid: UUID directory name from Sets/<UUID>/<Name>/ path */
void shadow_handle_set_loaded(const char *set_name, const char *uuid) {
    if (!set_name || !set_name[0]) return;

    /* Avoid re-triggering for the same set */
    if (strcmp(sampler_current_set_name, set_name) == 0 &&
        (uuid == NULL || strcmp(sampler_current_set_uuid, uuid) == 0)) {
        return;
    }

    /* Save outgoing set's config before switching */
    if (uuid && sampler_current_set_uuid[0]) {
        char outgoing_dir[512];
        snprintf(outgoing_dir, sizeof(outgoing_dir), SET_STATE_DIR "/%s",
                 sampler_current_set_uuid);
        shadow_save_config_to_dir(outgoing_dir);
        char m[256];
        snprintf(m, sizeof(m), "Set switch: saved config to %s", outgoing_dir);
        host.log(m);
    }

    snprintf(sampler_current_set_name, sizeof(sampler_current_set_name), "%s", set_name);
    if (uuid) {
        snprintf(sampler_current_set_uuid, sizeof(sampler_current_set_uuid), "%s", uuid);
    }

    /* Write active set UUID + name to file for shadow UI and boot persistence.
     * Format: line 1 = UUID, line 2 = set name */
    if (uuid && uuid[0]) {
        FILE *af = fopen(ACTIVE_SET_PATH, "w");
        if (af) {
            fputs(uuid, af);
            fputc('\n', af);
            fputs(set_name ? set_name : "", af);
            fclose(af);
        }
        /* Ensure per-set state directory exists */
        char incoming_dir[512];
        snprintf(incoming_dir, sizeof(incoming_dir), SET_STATE_DIR "/%s", uuid);
        shadow_ensure_dir(incoming_dir);

        /* Detect if this is a copied set — write source UUID for JS copy-on-first-use */
        char copy_source_path[512];
        snprintf(copy_source_path, sizeof(copy_source_path), "%s/copy_source.txt", incoming_dir);
        {
            /* Only detect copy for sets that don't already have state */
            char test_path[512];
            snprintf(test_path, sizeof(test_path), "%s/slot_0.json", incoming_dir);
            struct stat tst;
            if (stat(test_path, &tst) != 0) {
                /* No existing state — check if this is a copy */
                char source_uuid[64];
                if (shadow_detect_copy_source(set_name, uuid, source_uuid, sizeof(source_uuid))) {
                    /* Write copy source UUID so JS can copy from the right dir */
                    FILE *csf = fopen(copy_source_path, "w");
                    if (csf) {
                        fputs(source_uuid, csf);
                        fclose(csf);
                    }
                    /* Also copy the source's chain config to the new dir */
                    {
                        char source_dir[512];
                        snprintf(source_dir, sizeof(source_dir), SET_STATE_DIR "/%s", source_uuid);
                        char src_cfg[512], dst_cfg[512];
                        snprintf(src_cfg, sizeof(src_cfg), "%s/" SHADOW_CHAIN_CONFIG_FILENAME, source_dir);
                        snprintf(dst_cfg, sizeof(dst_cfg), "%s/" SHADOW_CHAIN_CONFIG_FILENAME, incoming_dir);
                        shadow_copy_file(src_cfg, dst_cfg);
                    }
                    char m[256];
                    snprintf(m, sizeof(m), "Set copy detected: source=%s -> new=%s", source_uuid, uuid);
                    host.log(m);
                }
            }
        }

        /* Load incoming set's config (volumes, channels) */
        shadow_load_config_from_dir(incoming_dir);
    }

    /* Signal shadow UI to save outgoing state and reload from new set dir */
    if (*host.shadow_control_ptr) {
        (*host.shadow_control_ptr)->ui_flags |= SHADOW_UI_FLAG_SET_CHANGED;
    }

    sampler_set_tempo = host.read_set_tempo(set_name);
    char msg[256];
    snprintf(msg, sizeof(msg), "Set detected: \"%s\" uuid=%s tempo=%.1f",
             set_name, uuid ? uuid : "?", sampler_set_tempo);
    host.log(msg);

    /* Read initial mute and solo states from Song.abl */
    int muted[4], soloed[4];
    int n = host.read_set_mute_states(set_name, muted, soloed);
    *host.solo_count = 0;
    for (int i = 0; i < n && i < SHADOW_CHAIN_INSTANCES; i++) {
        host.chain_slots[i].muted = muted[i];
        host.chain_slots[i].soloed = soloed[i];
        if (soloed[i]) (*host.solo_count)++;
        host.ui_state_update_slot(i);
    }
    {
        char m[128];
        snprintf(m, sizeof(m), "Set load: muted=[%d,%d,%d,%d] soloed=[%d,%d,%d,%d]",
                 muted[0], muted[1], muted[2], muted[3],
                 soloed[0], soloed[1], soloed[2], soloed[3]);
        host.log(m);
    }
}

/* Poll Settings.json for currentSongIndex changes, then match via xattr.
 * Called periodically from ioctl tick (~every 5 seconds). */
void shadow_poll_current_set(void)
{
    static const char settings_path[] = "/data/UserData/settings/Settings.json";

    /* Read currentSongIndex from Settings.json */
    FILE *f = fopen(settings_path, "r");
    if (!f) return;

    int song_index = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "\"currentSongIndex\":");
        if (p) {
            p += 19;  /* skip past "currentSongIndex": */
            while (*p == ' ') p++;
            song_index = atoi(p);
            break;
        }
    }
    fclose(f);

    if (song_index < 0) return;

    /* Normal path: react when index changes.
     * Pending path: keep retrying the same unresolved index until a UUID appears. */
    if (song_index == sampler_last_song_index &&
        song_index != sampler_pending_song_index) {
        return;
    }

    int song_index_changed = (song_index != sampler_last_song_index);
    if (song_index_changed) {
        sampler_last_song_index = song_index;
    }

    /* Scan Sets directories for matching user.song-index xattr */
    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) return;

    int matched = 0;
    struct dirent *entry;
    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char uuid_path[512];
        snprintf(uuid_path, sizeof(uuid_path), "%s/%s", SAMPLER_SETS_DIR, entry->d_name);

        /* Read user.song-index xattr from UUID directory */
        char xattr_val[32] = "";
        ssize_t xlen = getxattr(uuid_path, "user.song-index", xattr_val, sizeof(xattr_val) - 1);
        if (xlen <= 0) continue;
        xattr_val[xlen] = '\0';

        int idx = atoi(xattr_val);
        if (idx != song_index) continue;

        /* Found matching UUID dir — get set name from subdirectory */
        DIR *uuid_dir = opendir(uuid_path);
        if (!uuid_dir) continue;

        int handled = 0;
        struct dirent *sub;
        while ((sub = readdir(uuid_dir)) != NULL) {
            if (sub->d_name[0] == '.') continue;
            /* This subdirectory name is the set name */
            shadow_handle_set_loaded(sub->d_name, entry->d_name);
            handled = 1;
            break;
        }
        closedir(uuid_dir);
        if (handled) {
            matched = 1;
            break;
        }
    }
    closedir(sets_dir);

    if (matched) {
        sampler_pending_song_index = -1;
        return;
    }

    /* currentSongIndex changed, but the Sets/<UUID>/ folder is not materialized yet.
     * Present an immediate blank working state in a synthetic pending namespace. */
    if (song_index_changed || song_index != sampler_pending_song_index) {
        sampler_pending_set_seq++;
        if (sampler_pending_set_seq == 0) sampler_pending_set_seq = 1;
    }
    sampler_pending_song_index = song_index;

    char pending_name[128];
    char pending_uuid[64];
    snprintf(pending_name, sizeof(pending_name), "New Set %d", song_index + 1);
    snprintf(pending_uuid, sizeof(pending_uuid), "__pending-%d-%u",
             song_index, (unsigned)sampler_pending_set_seq);
    shadow_handle_set_loaded(pending_name, pending_uuid);
}

/* ============================================================================
 * Set page operations
 * ============================================================================ */

/* Save xattrs for all UUID dirs in Sets/ to stash_dir/xattrs.txt */
static void set_page_save_xattrs(const char *sets_dir, const char *stash_dir)
{
    char xattrs_path[512];
    snprintf(xattrs_path, sizeof(xattrs_path), "%s/xattrs.txt", stash_dir);

    FILE *xf = fopen(xattrs_path, "w");
    if (!xf) return;

    DIR *d = opendir(sets_dir);
    if (!d) { fclose(xf); return; }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char uuid_path[512];
        snprintf(uuid_path, sizeof(uuid_path), "%s/%s", sets_dir, entry->d_name);

        for (int i = 0; set_page_xattr_names[i]; i++) {
            char val[256] = "";
            ssize_t xlen = getxattr(uuid_path, set_page_xattr_names[i], val, sizeof(val) - 1);
            if (xlen > 0) {
                val[xlen] = '\0';
                fprintf(xf, "%s %s %s\n", entry->d_name, set_page_xattr_names[i], val);
            }
        }
    }
    closedir(d);
    fclose(xf);
}

/* Restore xattrs from stash_dir/xattrs.txt to UUID dirs in sets_dir */
static void set_page_restore_xattrs(const char *sets_dir, const char *stash_dir)
{
    char xattrs_path[512];
    snprintf(xattrs_path, sizeof(xattrs_path), "%s/xattrs.txt", stash_dir);

    FILE *xf = fopen(xattrs_path, "r");
    if (!xf) return;

    char line[512];
    while (fgets(line, sizeof(line), xf)) {
        /* Parse: "UUID attr_name attr_value\n" */
        char uuid[128], attr[128], val[256];
        if (sscanf(line, "%127s %127s %255[^\n]", uuid, attr, val) == 3) {
            char uuid_path[512];
            snprintf(uuid_path, sizeof(uuid_path), "%s/%s", sets_dir, uuid);
            struct stat st;
            if (stat(uuid_path, &st) == 0) {
                setxattr(uuid_path, attr, val, strlen(val), 0);
            }
        }
    }
    fclose(xf);
}

/* Move all UUID directories from src_dir to dst_dir */
/* Count non-dot directory entries (UUID dirs) in a path */
static int count_uuid_dirs(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            count++;
    }
    closedir(d);
    return count;
}

/* Write a recovery manifest listing UUID dirs in a page stash directory */
static void write_manifest(const char *stash_dir, int page_num)
{
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", stash_dir);

    FILE *f = fopen(manifest_path, "w");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "# Set page manifest - page %d - %s\n", page_num, timestamp);

    DIR *d = opendir(stash_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", stash_dir, entry->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                fprintf(f, "%s\n", entry->d_name);
        }
        closedir(d);
    }
    fclose(f);
}

static int set_page_move_dirs(const char *src_dir, const char *dst_dir, int *out_skipped)
{
    DIR *d = opendir(src_dir);
    if (!d) { if (out_skipped) *out_skipped = 0; return 0; }

    int moved = 0, skipped = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        /* Only move directories (UUID dirs) */
        char src_path[512], dst_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        /* Skip non-directories and xattrs.txt */
        struct stat st;
        if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);

        /* Collision check: skip if destination already exists as a directory */
        struct stat dst_st;
        if (stat(dst_path, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
            char msg[512];
            snprintf(msg, sizeof(msg), "SetPage: SKIP collision %s (already exists at dest)",
                     entry->d_name);
            host.log(msg);
            skipped++;
            continue;
        }

        if (rename(src_path, dst_path) == 0) {
            moved++;
        } else {
            char msg[512];
            snprintf(msg, sizeof(msg), "SetPage: rename failed %s -> %s: %s",
                     src_path, dst_path, strerror(errno));
            host.log(msg);
        }
    }
    closedir(d);
    if (out_skipped) *out_skipped = skipped;
    return moved;
}

/* Persist current page number to disk */
static void set_page_persist(int page)
{
    shadow_ensure_dir(SET_PAGES_DIR);
    FILE *f = fopen(SET_PAGES_CURRENT_PATH, "w");
    if (f) {
        fprintf(f, "%d\n", page);
        fclose(f);
    }
}

/* Read current page from disk (returns 0 if not found) */
int set_page_read_persisted(void)
{
    FILE *f = fopen(SET_PAGES_CURRENT_PATH, "r");
    if (!f) return 0;
    int page = 0;
    if (fscanf(f, "%d", &page) != 1) page = 0;
    fclose(f);
    if (page < 0 || page >= SET_PAGES_TOTAL) page = 0;
    return page;
}

/* Background thread args for set page change */
typedef struct {
    int old_page;
    int new_page;
} set_page_change_args_t;

/* Fire-and-forget dbus-send (fork without waitpid) */
static void set_page_dbus_fire_and_forget(const char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stderr to /dev/null, exec */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Parent: don't wait - child will be reaped by init */
}

/* Update currentSongIndex in Settings.json (simple sed-like in-place edit) */
static void set_page_update_song_index(int index)
{
    const char *path = "/data/UserData/settings/Settings.json";
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) { fclose(f); return; }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    /* Find and replace currentSongIndex value */
    char *p = strstr(buf, "\"currentSongIndex\":");
    if (p) {
        char *val_start = p + 19;  /* skip "currentSongIndex": */
        while (*val_start == ' ') val_start++;
        char *val_end = val_start;
        if (*val_end == '-') val_end++;  /* skip negative sign */
        while (*val_end >= '0' && *val_end <= '9') val_end++;

        /* Build new file content */
        char new_val[16];
        snprintf(new_val, sizeof(new_val), "%d", index);

        FILE *out = fopen(path, "w");
        if (out) {
            fwrite(buf, 1, val_start - buf, out);
            fputs(new_val, out);
            fputs(val_end, out);
            fclose(out);
        }
    }
    free(buf);
}

/* Background thread: does the heavy I/O for page change, then restarts Move */
static void *set_page_change_thread(void *arg)
{
    set_page_change_args_t *a = (set_page_change_args_t *)arg;
    int old_page = a->old_page;
    int new_page = a->new_page;
    free(a);

    /* 1. Save song if dirty via dbus (blocking - we're on a background thread) */
    {
        const char *argv[] = {
            "dbus-send", "--system", "--print-reply",
            "--dest=com.ableton.move",
            "/com/ableton/move/browser",
            "com.ableton.move.Browser.saveSongIfDirty",
            "string:",
            NULL
        };
        host.run_command(argv);
    }

    /* 1b. Sync + poll: wait for save to materialize on disk */
    {
        sync();
        int prev_count = count_uuid_dirs(SAMPLER_SETS_DIR);
        for (int attempt = 0; attempt < 6; attempt++) {
            usleep(500000); /* 500ms */
            sync();
            int cur = count_uuid_dirs(SAMPLER_SETS_DIR);
            if (cur == prev_count) break; /* stable */
            prev_count = cur;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "SetPage: post-save sync: %d sets in Sets/", prev_count);
        host.log(msg);
    }

    /* 2. Save xattrs for current sets */
    char current_stash[512];
    snprintf(current_stash, sizeof(current_stash), SET_PAGES_DIR "/page_%d", old_page);
    shadow_ensure_dir(current_stash);
    set_page_save_xattrs(SAMPLER_SETS_DIR, current_stash);

    /* Pre-flight inventory */
    int pre_count = count_uuid_dirs(SAMPLER_SETS_DIR);
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "SetPage: pre-flight: %d sets in Sets/", pre_count);
        host.log(msg);
    }

    /* 3. Move current sets to stash */
    int stash_skipped = 0;
    int stashed = set_page_move_dirs(SAMPLER_SETS_DIR, current_stash, &stash_skipped);

    /* Post-stash inventory */
    {
        int remaining = count_uuid_dirs(SAMPLER_SETS_DIR);
        char msg[256];
        snprintf(msg, sizeof(msg), "SetPage: stashed %d (skipped %d), %d remaining in Sets/",
                 stashed, stash_skipped, remaining);
        host.log(msg);
        if (remaining > 0) {
            host.log("SetPage: WARNING - sets still in Sets/ after stash!");
        }
    }

    /* Write recovery manifest for the stash */
    write_manifest(current_stash, old_page);

    /* 4. Move target page sets from stash to Sets/ */
    char target_stash[512];
    snprintf(target_stash, sizeof(target_stash), SET_PAGES_DIR "/page_%d", new_page);
    shadow_ensure_dir(target_stash);
    int restore_skipped = 0;
    int restored = set_page_move_dirs(target_stash, SAMPLER_SETS_DIR, &restore_skipped);

    /* Post-restore inventory */
    {
        int now_in_sets = count_uuid_dirs(SAMPLER_SETS_DIR);
        char msg[256];
        snprintf(msg, sizeof(msg), "SetPage: restored %d from page_%d (skipped %d), %d now in Sets/",
                 restored, new_page, restore_skipped, now_in_sets);
        host.log(msg);
    }

    /* 5. Restore xattrs for target page */
    set_page_restore_xattrs(SAMPLER_SETS_DIR, target_stash);

    /* 6. Update currentSongIndex to 0 so Move loads the first set on new page */
    set_page_update_song_index(0);

    /* 7. Persist page number */
    set_page_persist(new_page);

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "SetPage: now on page %d (%d sets restored), restarting Move",
                 new_page + 1, restored);
        host.log(msg);
    }

    /* 8. Save shadow state before restart */
    host.save_state();

    /* 9. Trigger restart via the existing mechanism */
    host.log("SetPage: triggering restart");
    system("/data/UserData/move-anything/restart-move.sh");

    return NULL;
}

/* Change to a new set page (non-blocking: spawns background thread for I/O) */
void shadow_change_set_page(int new_page)
{
    if (new_page < 0 || new_page >= SET_PAGES_TOTAL) return;
    if (new_page == set_page_current) return;
    if (set_page_change_in_flight) return;

    int old_page = set_page_current;

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "SetPage: switching from page %d to page %d",
                 old_page + 1, new_page + 1);
        host.log(msg);
    }

    /* Update state and show "Loading..." toast immediately (before I/O) */
    set_page_current = new_page;
    set_page_loading = 1;
    set_page_overlay_active = 1;
    set_page_overlay_timeout = SET_PAGE_OVERLAY_FRAMES;
    host.overlay_sync();

    /* TTS announcement */
    {
        char sr_buf[128];
        snprintf(sr_buf, sizeof(sr_buf), "Page %d of %d", new_page + 1, SET_PAGES_TOTAL);
        host.announce(sr_buf);
    }

    /* Spawn background thread for heavy I/O */
    set_page_change_in_flight = 1;
    set_page_change_args_t *args = malloc(sizeof(set_page_change_args_t));
    if (!args) return;
    args->old_page = old_page;
    args->new_page = new_page;

    pthread_t tid;
    if (pthread_create(&tid, NULL, set_page_change_thread, args) == 0) {
        pthread_detach(tid);
    } else {
        free(args);
        host.log("SetPage: failed to create background thread");
    }
}
