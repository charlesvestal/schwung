// boot_select_core.c — pure logic for boot-select, unit-tested on the host.
// No SPI, no display, no device dependencies: this file exists so the
// press-edge / jog-decode / JSON-field / row-model logic can be verified
// with a plain `cc` build instead of only ever being exercised on hardware.
#include "boot_select_core.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define BS_CC_BACK  51
#define BS_CC_CLICK 3
#define BS_CC_JOG   14

void bs_input_scan(bs_input_state_t *st, const uint8_t *src,
                    bs_input_events_t *out) {
    out->back_pressed = 0;
    out->click_pressed = 0;
    out->jog_delta = 0;

    for (int slot = 0; slot < 31; slot++) {
        const uint8_t *e = src + slot * 8;

        /* Terminator: the first all-zero event (bytes 0..3) ends the list
         * for this frame. Everything behind it is invisible. */
        if (e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0)
            break;

        uint8_t cable = (uint8_t)(e[0] >> 4);
        if (cable != 0)
            continue; /* only Move hardware controls */

        uint8_t status = e[1];
        if ((status & 0xF0) != 0xB0)
            continue; /* CC only */

        uint8_t cc = e[2];
        uint8_t val = e[3];

        if (cc == BS_CC_BACK) {
            int down = (val > 0);
            if (down && !st->back_down && st->frames_seen > 0)
                out->back_pressed = 1;
            st->back_down = down;
        } else if (cc == BS_CC_CLICK) {
            int down = (val > 0);
            if (down && !st->click_down && st->frames_seen > 0)
                out->click_pressed = 1;
            st->click_down = down;
        } else if (cc == BS_CC_JOG) {
            if (st->frames_seen > 0) {
                if (val >= 1 && val <= 63)
                    out->jog_delta += val;
                else if (val >= 65 && val <= 127)
                    out->jog_delta -= (128 - val);
                /* val == 0 or 64: no motion, ignore */
            }
        }
    }

    st->frames_seen++;
}

int bs_json_field(const char *buf, const char *key, char *out, size_t outlen) {
    if (!buf || !key || !out || outlen == 0)
        return 0;

    char needle[80];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle))
        return 0;

    const char *p = strstr(buf, needle);
    if (!p)
        return 0;

    p += n;
    /* skip whitespace up to ':' */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return 0;
    p++;

    size_t i = 0;
    while (*p != '"' && *p != '\0') {
        if (i + 1 < outlen)
            out[i++] = *p;
        p++;
    }
    out[i < outlen ? i : outlen - 1] = '\0';

    if (*p != '"')
        return 0; /* unterminated string: malformed */

    return 1;
}

/* Insertion sort by id, ascending. Small N (a handful of boot targets), so
 * this is plenty and needs no library dependency. */
static void bs_row_insert_sorted(bs_row_t *rows, int *count, int max,
                                  const bs_row_t *row) {
    if (*count >= max)
        return;
    int i = *count;
    while (i > 0 && strcmp(rows[i - 1].id, row->id) > 0) {
        rows[i] = rows[i - 1];
        i--;
    }
    rows[i] = *row;
    (*count)++;
}

int bs_build_rows(const char *dir, bs_row_t *rows, int max) {
    if (!dir || !rows || max <= 0)
        return 0;

    int count = 0;
    bs_row_t schwung_row;
    int have_schwung = 0;
    bs_row_t others[64];
    int other_count = 0;

    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            if (strcmp(ent->d_name, "stock") == 0)
                continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

            struct stat st_buf;
            if (stat(path, &st_buf) != 0 || !S_ISDIR(st_buf.st_mode))
                continue;

            char boot_json_path[560];
            snprintf(boot_json_path, sizeof(boot_json_path), "%s/boot.json", path);

            FILE *f = fopen(boot_json_path, "rb");
            if (!f)
                continue;

            char json_buf[4097];
            size_t got = fread(json_buf, 1, sizeof(json_buf) - 1, f);
            fclose(f);
            json_buf[got] = '\0';

            bs_row_t row;
            memset(&row, 0, sizeof(row));
            snprintf(row.id, sizeof(row.id), "%s", ent->d_name);

            char name_buf[64];
            if (bs_json_field(json_buf, "name", name_buf, sizeof(name_buf))) {
                snprintf(row.name, sizeof(row.name), "%s", name_buf);
            } else {
                snprintf(row.name, sizeof(row.name), "%s", ent->d_name);
            }

            if (strcmp(ent->d_name, "schwung") == 0) {
                schwung_row = row;
                have_schwung = 1;
            } else if (other_count < (int)(sizeof(others) / sizeof(others[0]))) {
                others[other_count++] = row;
            }
        }
        closedir(d);
    }

    /* Order: schwung first, then the rest alphabetically, then stock last,
     * ALWAYS -- one slot is reserved for stock up front so hitting `max`
     * never bumps it off the end. */
    int room = max - 1; /* capacity for schwung + others; may be 0 */
    if (room < 0)
        room = 0;

    if (have_schwung && count < room)
        rows[count++] = schwung_row;

    /* Sort `others` by id via insertion into a scratch array, then append
     * within the remaining reserved room. */
    bs_row_t sorted[64];
    int sorted_count = 0;
    for (int i = 0; i < other_count; i++)
        bs_row_insert_sorted(sorted, &sorted_count,
                              (int)(sizeof(sorted) / sizeof(sorted[0])),
                              &others[i]);

    for (int i = 0; i < sorted_count && count < room; i++)
        rows[count++] = sorted[i];

    /* Stock always last. */
    bs_row_t stock_row;
    memset(&stock_row, 0, sizeof(stock_row));
    snprintf(stock_row.id, sizeof(stock_row.id), "stock");
    snprintf(stock_row.name, sizeof(stock_row.name), "Stock Move");
    rows[count++] = stock_row;

    return count;
}
