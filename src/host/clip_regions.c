/* clip_regions.c — see clip_regions.h.
 *
 * A targeted scanner, not a JSON library. It walks brace depth and only
 * accepts a key at the exact depth it belongs to, which is what keeps the
 * `notes` array -- objects carrying "startTime"/"duration", tens of thousands
 * of them -- from being mistaken for clip geometry. A substring search for
 * "start" over this file finds thousands of hits and none of them are the
 * loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clip_regions.h"

#define D_UNSET (-1)

static int key_is(const char *p, const char *end, const char *key)
{
    size_t n = strlen(key);
    if ((size_t)(end - p) < n + 2) return 0;
    if (p[0] != '"') return 0;
    if (strncmp(p + 1, key, n) != 0) return 0;
    return p[1 + n] == '"';
}

/* Advance past a JSON string starting at *p == '"'. Handles escapes. */
static const char *skip_string(const char *p, const char *end)
{
    p++;
    while (p < end) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return end;
}

static double read_number_after_colon(const char *p, const char *end)
{
    while (p < end && *p != ':') p++;
    if (p >= end) return 0.0;
    p++;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
    return strtod(p, NULL);
}

static int read_bool_after_colon(const char *p, const char *end)
{
    while (p < end && *p != ':') p++;
    if (p >= end) return 0;
    p++;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
    return (end - p >= 4) && strncmp(p, "true", 4) == 0;
}

int clip_regions_parse(const char *json, size_t len, clip_regions_t *out)
{
    if (!json || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->step_resolution = 0.25;   /* 1/16, Move's default */

    const char *p = json, *end = json + len;
    int depth = 0;

    int d_tracks     = D_UNSET;  /* objects at this depth are TRACKS      */
    int d_clipslots  = D_UNSET;  /* objects at this depth are CLIP SLOTS  */
    int d_clip       = D_UNSET;  /* the clip object itself                */
    int d_region     = D_UNSET;
    int d_loop       = D_UNSET;

    int track = -1, slot = -1;
    double region_start = 0, region_end = 0;
    double loop_start = 0, loop_end = 0, loop_enabled = 0;
    int have_clip = 0;

    while (p < end) {
        char c = *p;

        if (c == '"') {
            /* Keys we care about, each pinned to its own depth. */
            if (d_tracks == D_UNSET && depth == 1 && key_is(p, end, "tracks")) {
                /* +2, not +1: the value is an ARRAY, so a track object sits
                 * one level below the array, two below the key. */
                d_tracks = depth + 2;
            } else if (depth == 1 && key_is(p, end, "stepEditorResolution")) {
                const char *q = p;
                while (q < end && *q != ':') q++;
                while (q < end && *q != '"') q++;
                if (q < end) {
                    int num = 0, den = 0;
                    if (sscanf(q, "\"%d/%d\"", &num, &den) == 2 && den > 0)
                        out->step_resolution = 4.0 * (double)num / (double)den;
                }
            } else if (d_tracks != D_UNSET && depth == d_tracks &&
                       key_is(p, end, "clipSlots")) {
                d_clipslots = depth + 2;   /* array-wrapped, as above */
                slot = -1;
            } else if (d_clipslots != D_UNSET && depth == d_clipslots &&
                       key_is(p, end, "clip")) {
                /* "clip": null is an empty slot; "clip": { starts one. */
                const char *q = p;
                while (q < end && *q != ':') q++;
                q++;
                while (q < end && (*q==' '||*q=='\n'||*q=='\t'||*q=='\r')) q++;
                if (q < end && *q == '{') {
                    d_clip = depth + 1;
                    have_clip = 1;
                    region_start = region_end = 0;
                    loop_start = loop_end = 0; loop_enabled = 0;
                }
            } else if (d_clip != D_UNSET && depth == d_clip &&
                       key_is(p, end, "isPlaying")) {
                if (track >= 0 && track < CLIP_TRACKS &&
                    slot >= 0 && slot < CLIP_SLOTS)
                    out->slots[track][slot].is_playing =
                        read_bool_after_colon(p, end);
            } else if (d_clip != D_UNSET && depth == d_clip &&
                       key_is(p, end, "region")) {
                d_region = depth + 1;
            } else if (d_region != D_UNSET && depth == d_region &&
                       key_is(p, end, "loop")) {
                d_loop = depth + 1;
            } else if (d_loop != D_UNSET && depth == d_loop) {
                if (key_is(p, end, "start"))     loop_start = read_number_after_colon(p, end);
                else if (key_is(p, end, "end"))  loop_end   = read_number_after_colon(p, end);
                else if (key_is(p, end, "isEnabled")) loop_enabled = read_bool_after_colon(p, end);
            } else if (d_region != D_UNSET && depth == d_region) {
                if (key_is(p, end, "start"))     region_start = read_number_after_colon(p, end);
                else if (key_is(p, end, "end"))  region_end   = read_number_after_colon(p, end);
            }
            p = skip_string(p, end);
            continue;
        }

        if (c == '{') {
            depth++;
            if (d_tracks != D_UNSET && depth == d_tracks) {
                track++;
                d_clipslots = D_UNSET;
            } else if (d_clipslots != D_UNSET && depth == d_clipslots) {
                slot++;
            }
            p++;
            continue;
        }

        if (c == '}') {
            if (d_loop != D_UNSET && depth == d_loop)   d_loop = D_UNSET;
            else if (d_region != D_UNSET && depth == d_region) d_region = D_UNSET;
            else if (d_clip != D_UNSET && depth == d_clip) {
                /* Closing the clip: commit. song-mode uses the loop when it is
                 * enabled and the region otherwise; match it. */
                if (have_clip && track >= 0 && track < CLIP_TRACKS &&
                    slot >= 0 && slot < CLIP_SLOTS) {
                    clip_region_t *r = &out->slots[track][slot];
                    r->exists = 1;
                    if (loop_enabled && loop_end > loop_start) {
                        r->loop_start = loop_start;
                        r->loop_len   = loop_end - loop_start;
                    } else if (region_end > region_start) {
                        r->loop_start = region_start;
                        r->loop_len   = region_end - region_start;
                    }
                }
                have_clip = 0;
                d_clip = D_UNSET;
            }
            depth--;
            p++;
            continue;
        }

        if (c == '[') { depth++; p++; continue; }
        if (c == ']') {
            /* The array sits one level ABOVE the objects it holds. */
            if (d_clipslots != D_UNSET && depth == d_clipslots - 1) d_clipslots = D_UNSET;
            else if (d_tracks != D_UNSET && depth == d_tracks - 1) d_tracks = D_UNSET;
            depth--; p++; continue;
        }
        p++;
    }

    out->valid = (track >= 0);
    return out->valid;
}

int clip_regions_parse_file(const char *path, clip_regions_t *out)
{
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    /* Song.abl runs past 1 MB. Cap it so a corrupt or absurd file cannot
     * take the worker's memory with it. */
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return 0; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    /* A short read is a FAILURE, not a smaller document. Parsing a truncated
     * file yields a confident, wrong answer for every clip after the cut. */
    if (got != (size_t)sz) { free(buf); return 0; }
    int ok = clip_regions_parse(buf, got, out);
    free(buf);
    return ok;
}

void clip_regions_seed_state(const clip_regions_t *rg, clip_state_t *st)
{
    if (!rg || !st || !rg->valid) return;
    for (int t = 0; t < CLIP_TRACKS; t++) {
        /* Never overwrite what the LED stream told us. The file is save-time
         * state; an observation is now. */
        if (st->tracks[t].identity_valid) continue;
        for (int s = 0; s < CLIP_SLOTS; s++) {
            if (!rg->slots[t][s].exists || !rg->slots[t][s].is_playing) continue;
            st->tracks[t].identity_valid = 1;
            st->tracks[t].clip_slot = s;
            /* No anchor. The file says WHAT is selected, never WHEN it
             * started -- and a Start arriving after this will anchor it to 0,
             * which is the whole point of seeding before 0xFA. */
            break;
        }
    }
}

void clip_regions_forget_deleted(const clip_regions_t *before,
                                 const clip_regions_t *after,
                                 clip_state_t *st)
{
    if (!before || !after || !st) return;
    if (!before->valid || !after->valid) return;   /* nothing to compare */
    for (int t = 0; t < CLIP_TRACKS; t++) {
        clip_track_state_t *tr = &st->tracks[t];
        if (!tr->identity_valid || tr->clip_slot < 0) continue;
        int s = tr->clip_slot;
        if (before->slots[t][s].exists && !after->slots[t][s].exists) {
            /* It was there, now it is not. Whatever we believed about this
             * track is stale -- including the anchor, which describes a clip
             * that no longer exists. */
            tr->clip_slot = -1;
            tr->anchor_valid = 0;
        }
    }
}
