/*
 * boot-select's pure-logic half, host-testable: MIDI_IN scanning with
 * press-edge detection, jog delta decoding, flat-JSON field extraction, and
 * the picker row model. No SPI, no display -- see boot_select_core.h.
 */
/* mkdtemp() is POSIX.1-2008, not C11 -- without this, glibc (Linux CI) leaves
 * it implicitly declared (int return, truncated through a pointer) and the
 * test SIGSEGVs (exit 139). Must be the FIRST line, before any include pulls
 * in <features.h>.
 *
 * Darwin's <sys/cdefs.h> derives __DARWIN_C_LEVEL from _POSIX_C_SOURCE
 * *unless* _DARWIN_C_SOURCE is also defined, and mkdtemp is gated on
 * __DARWIN_C_LEVEL >= __DARWIN_C_FULL -- so _POSIX_C_SOURCE alone fixes
 * glibc and, symmetrically, HIDES mkdtemp on macOS (verified: this file
 * fails to build there with just the one macro). _DARWIN_C_SOURCE is a
 * no-op everywhere else, so define both. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot_select_core.h"

static void mkdir_or_die(const char *path) {
    if (mkdir(path, 0755) != 0) { perror(path); exit(1); }
}

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static void put_cc(uint8_t *buf, int slot, uint8_t cc, uint8_t val) {
    uint8_t *e = buf + slot * 8;
    e[0] = 0x0B; e[1] = 0xB0; e[2] = cc; e[3] = val;
    e[4] = 1;  /* nonzero timestamp byte, realistic */
}

static void test_frame_zero_never_fires(void) {
    printf("frame 0 never fires an edge\n");
    bs_input_state_t st; memset(&st, 0, sizeof(st));
    uint8_t buf[BS_MIDI_IN_BYTES]; memset(buf, 0, sizeof(buf));
    put_cc(buf, 0, 51, 127); /* Back pressed, but this is frame 0 */

    bs_input_events_t ev;
    bs_input_scan(&st, buf, &ev);
    check(ev.back_pressed == 0, "no edge on the very first scan");
    check(st.frames_seen == 1, "frames_seen advances after the scan");

    /* A clean (empty) frame first, then the press -> edge fires. */
    bs_input_state_t st2; memset(&st2, 0, sizeof(st2));
    uint8_t empty[BS_MIDI_IN_BYTES]; memset(empty, 0, sizeof(empty));
    bs_input_events_t ev0;
    bs_input_scan(&st2, empty, &ev0);
    check(ev0.back_pressed == 0, "empty frame 0 fires nothing");

    bs_input_events_t ev1;
    bs_input_scan(&st2, buf, &ev1);
    check(ev1.back_pressed == 1, "press on frame 1 (after a clean frame 0) fires");
}

static void test_held_then_release_press(void) {
    printf("a held button fires exactly one edge; release then press fires again\n");
    bs_input_state_t st; memset(&st, 0, sizeof(st));
    uint8_t empty[BS_MIDI_IN_BYTES]; memset(empty, 0, sizeof(empty));
    uint8_t pressed[BS_MIDI_IN_BYTES]; memset(pressed, 0, sizeof(pressed));
    put_cc(pressed, 0, 51, 127);
    uint8_t released[BS_MIDI_IN_BYTES]; memset(released, 0, sizeof(released));
    put_cc(released, 0, 51, 0);

    bs_input_events_t ev;
    bs_input_scan(&st, empty, &ev);          /* frame 0: warmup */
    check(ev.back_pressed == 0, "warmup frame: no edge");

    bs_input_scan(&st, pressed, &ev);        /* frame 1: press */
    check(ev.back_pressed == 1, "first press frame: edge fires");

    bs_input_scan(&st, pressed, &ev);        /* frame 2: still held */
    check(ev.back_pressed == 0, "held second frame: no repeat edge");

    bs_input_scan(&st, released, &ev);       /* frame 3: release */
    check(ev.back_pressed == 0, "release frame: no edge");

    bs_input_scan(&st, pressed, &ev);        /* frame 4: press again */
    check(ev.back_pressed == 1, "press again after release: edge fires");
}

static void test_terminator_hides_events_behind_it(void) {
    printf("a zeroed slot 0 terminates the scan -- slot 1 is invisible\n");
    bs_input_state_t st; memset(&st, 0, sizeof(st));
    uint8_t empty[BS_MIDI_IN_BYTES]; memset(empty, 0, sizeof(empty));
    bs_input_events_t ev;
    bs_input_scan(&st, empty, &ev); /* warmup */

    uint8_t buf[BS_MIDI_IN_BYTES]; memset(buf, 0, sizeof(buf));
    /* slot 0 stays all-zero (terminator); slot 1 carries a Back press */
    put_cc(buf, 1, 51, 127);

    bs_input_scan(&st, buf, &ev);
    check(ev.back_pressed == 0, "event behind the terminator never fires");
}

static void test_cable_2_ignored(void) {
    printf("cable-2 (external USB) copies of a control CC are ignored\n");
    bs_input_state_t st; memset(&st, 0, sizeof(st));
    uint8_t empty[BS_MIDI_IN_BYTES]; memset(empty, 0, sizeof(empty));
    bs_input_events_t ev;
    bs_input_scan(&st, empty, &ev); /* warmup */

    uint8_t buf[BS_MIDI_IN_BYTES]; memset(buf, 0, sizeof(buf));
    uint8_t *e = buf + 0;
    e[0] = 0x2B; /* cable 2, CIN 0xB */
    e[1] = 0xB0; e[2] = 51; e[3] = 127;
    e[4] = 1;

    bs_input_scan(&st, buf, &ev);
    check(ev.back_pressed == 0, "cable-2 Back press does not fire");
}

static void test_jog_delta(void) {
    printf("jog delta sums signed detents within a frame\n");
    bs_input_state_t st; memset(&st, 0, sizeof(st));
    uint8_t empty[BS_MIDI_IN_BYTES]; memset(empty, 0, sizeof(empty));
    bs_input_events_t ev;
    bs_input_scan(&st, empty, &ev); /* warmup */

    uint8_t buf[BS_MIDI_IN_BYTES]; memset(buf, 0, sizeof(buf));
    put_cc(buf, 0, 14, 1);
    put_cc(buf, 1, 14, 1);
    put_cc(buf, 2, 14, 1);
    bs_input_scan(&st, buf, &ev);
    check(ev.jog_delta == 3, "three +1 detents sum to +3");

    uint8_t buf2[BS_MIDI_IN_BYTES]; memset(buf2, 0, sizeof(buf2));
    put_cc(buf2, 0, 14, 127);
    bs_input_scan(&st, buf2, &ev);
    check(ev.jog_delta == -1, "value 127 decodes to -1");

    /* Contract formula: delta += (v < 64 ? v : (int)v - 128). v == 64 is not
     * a dead zone -- it decodes to -64, same branch as 65..127. */
    uint8_t buf3[BS_MIDI_IN_BYTES]; memset(buf3, 0, sizeof(buf3));
    put_cc(buf3, 0, 14, 64);
    bs_input_scan(&st, buf3, &ev);
    check(ev.jog_delta == -64, "value 64 decodes to -64, per the contract formula");
}

/* Wrapping struct with a canary right after the exact 248-byte hardware
 * region, so an out-of-bounds walk (e.g. a 4-byte-stride bug, or reading
 * MIDI_BUFFER_SIZE == 256 instead of 31*8 == 248) is caught even though the
 * buffer lives on the stack.
 *
 * The canary bytes are a VALID cable-0 CC14 jog-turn event (val 1, i.e. a
 * +1 detent), not 0xAA filler. A canary the scanner would never parse as an
 * event only detects a WRITE past the boundary (a stray store), not a READ
 * -- mutating the loop bound (31 -> 32) drove jog_delta to 32 while an 0xAA
 * canary sat there untouched and unread, so that mutation survived the
 * original suite. With a real event sitting there, an over-read is caught by
 * the delta itself (32 instead of 31), and by the slot going untouched (it
 * is never zeroed by an in-bounds scan). */
typedef struct {
    uint8_t midi_in[BS_MIDI_IN_BYTES];
    uint8_t canary[8];
} canary_frame_t;

static void test_all_31_slots_no_oob(void) {
    printf("31 filled slots: delta == 31, no out-of-bounds read past byte 248\n");
    canary_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    /* slot 31 (bytes 248..255) would be the 32nd MIDI_IN slot if the walk
     * over-read -- plant a jog event there so an over-read is visible in
     * jog_delta rather than needing a separate detector. */
    put_cc(frame.canary, 0, 14, 1);

    bs_input_state_t st; memset(&st, 0, sizeof(st));
    bs_input_events_t ev;
    /* warmup with an all-zero midi_in (canary intact either way) */
    bs_input_scan(&st, frame.midi_in, &ev);

    for (int slot = 0; slot < 31; slot++)
        put_cc(frame.midi_in, slot, 14, 1);

    bs_input_scan(&st, frame.midi_in, &ev);
    check(ev.jog_delta == 31, "all 31 slots contribute +1 each -- the canary slot is NOT counted");

    int canary_intact = 1;
    uint8_t expect[8] = {0x0B, 0xB0, 14, 1, 1, 0, 0, 0};
    for (int i = 0; i < 8; i++)
        if (frame.canary[i] != expect[i]) canary_intact = 0;
    check(canary_intact, "canary past the 248-byte region is untouched (not written to)");
}

static void test_json_field(void) {
    printf("bs_json_field extracts a flat string field\n");
    const char *json = "{\n  \"name\": \"V\",\n  \"exec\": \"/data/x/entry.sh\"\n}\n";
    char out[128];

    int ok = bs_json_field(json, "exec", out, sizeof(out));
    check(ok == 1, "exec field found");
    check(strcmp(out, "/data/x/entry.sh") == 0, "exec value extracted exactly");

    ok = bs_json_field(json, "name", out, sizeof(out));
    check(ok == 1, "name field found");
    check(strcmp(out, "V") == 0, "name value extracted exactly");

    ok = bs_json_field(json, "missing", out, sizeof(out));
    check(ok == 0, "absent key returns 0");

    char small[4];
    ok = bs_json_field(json, "exec", small, sizeof(small));
    check(ok == 1, "truncated read still reports success (value was present)");
    check(strlen(small) == 3, "truncated to outlen-1 chars");
    check(small[3] == '\0', "always NUL-terminated even when truncated");

    /* The quoted key text can appear as a VALUE first ("desc": "name"),
     * which is not the key -- the first strstr hit must be skipped in
     * favor of the real "name": "Foo" that follows. */
    const char *collide = "{\"desc\": \"name\", \"name\": \"Foo\"}";
    char out2[128];
    int ok2 = bs_json_field(collide, "name", out2, sizeof(out2));
    check(ok2 == 1, "key found despite a same-text VALUE occurrence earlier in the buffer");
    check(strcmp(out2, "Foo") == 0, "value extracted is the real key's, not the value collision");
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
}

static void test_build_rows(void) {
    printf("bs_build_rows: schwung first, then alpha, stock always last\n");
    char tmpl[] = "/tmp/bs_core_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); exit(1); }

    char path[512];
    snprintf(path, sizeof(path), "%s/schwung", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/schwung/boot.json", dir);
    write_file(path, "{\"name\": \"Schwung\"}\n");

    snprintf(path, sizeof(path), "%s/v", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/v/boot.json", dir);
    write_file(path, "{\"name\": \"Vintage\"}\n");

    /* stray regular file at top level: must not become a row */
    snprintf(path, sizeof(path), "%s/README.txt", dir);
    write_file(path, "not a boot target\n");

    bs_row_t rows[8];
    int n = bs_build_rows(dir, rows, 8);
    check(n == 3, "exactly 3 rows: schwung, v, stock");
    if (n == 3) {
        check(strcmp(rows[0].id, "schwung") == 0, "row 0 is schwung");
        check(strcmp(rows[0].name, "Schwung") == 0, "row 0 name from boot.json");
        check(strcmp(rows[1].id, "v") == 0, "row 1 is v");
        check(strcmp(rows[1].name, "Vintage") == 0, "row 1 name from boot.json");
        check(strcmp(rows[2].id, "stock") == 0, "row 2 is stock");
        check(strcmp(rows[2].name, "Stock Move") == 0, "stock row name is Stock Move");
    }
}

static void test_build_rows_skips_stock_and_no_json_dirs(void) {
    printf("bs_build_rows: a subdir named stock is skipped as SOURCE; a subdir without boot.json is excluded; missing \"name\" falls back to the dirname\n");
    char tmpl[] = "/tmp/bs_core_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); exit(1); }

    char path[512];

    /* A real "stock" subdirectory on disk -- must never become its own row
     * (the synthesized stock row is appended separately, always last). */
    snprintf(path, sizeof(path), "%s/stock", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/stock/boot.json", dir);
    write_file(path, "{\"name\": \"Should Never Appear\"}\n");

    /* A subdir with no boot.json at all -- excluded entirely, not even as
     * a fallback-named row. */
    snprintf(path, sizeof(path), "%s/no-json", dir);
    mkdir_or_die(path);

    /* A subdir whose boot.json has no "name" field -- falls back to the
     * dirname for its display name. */
    snprintf(path, sizeof(path), "%s/nameless", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/nameless/boot.json", dir);
    write_file(path, "{\"exec\": \"/data/x/entry.sh\"}\n");

    bs_row_t rows[8];
    int n = bs_build_rows(dir, rows, 8);
    check(n == 2, "exactly 2 rows: nameless, stock (no-json excluded, real stock/ dir not a source row)");
    if (n == 2) {
        check(strcmp(rows[0].id, "nameless") == 0, "row 0 is the nameless-name dir, sorted alphabetically");
        check(strcmp(rows[0].name, "nameless") == 0, "boot.json without \"name\" falls back to the dirname");
        check(strcmp(rows[1].id, "stock") == 0, "row 1 is the synthesized stock row");
        check(strcmp(rows[1].name, "Stock Move") == 0, "synthesized stock row name is Stock Move, not the on-disk stock/boot.json's name");
    }
}

static void test_build_rows_max_constrained(void) {
    printf("bs_build_rows: max caps the row count, stock is ALWAYS last regardless\n");
    char tmpl[] = "/tmp/bs_core_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); exit(1); }

    char path[512];
    snprintf(path, sizeof(path), "%s/schwung", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/schwung/boot.json", dir);
    write_file(path, "{\"name\": \"Schwung\"}\n");

    snprintf(path, sizeof(path), "%s/a", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/a/boot.json", dir);
    write_file(path, "{\"name\": \"A\"}\n");

    snprintf(path, sizeof(path), "%s/b", dir);
    mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/b/boot.json", dir);
    write_file(path, "{\"name\": \"B\"}\n");

    /* max = 3: room for schwung + others is (max - 1) = 2, so schwung and
     * "a" (alphabetically first) fill it; "b" is dropped; stock still gets
     * its always-reserved final slot. */
    bs_row_t rows3[8];
    int n3 = bs_build_rows(dir, rows3, 3);
    check(n3 == 3, "max=3 yields exactly 3 rows");
    if (n3 == 3) {
        check(strcmp(rows3[0].id, "schwung") == 0, "row 0 is schwung");
        check(strcmp(rows3[1].id, "a") == 0, "row 1 is a (b dropped for lack of room)");
        check(strcmp(rows3[2].id, "stock") == 0, "row 2 is stock, still last");
    }

    /* max = 1: the whole reserved-room budget for schwung + others is
     * (max - 1) = 0, so ONLY the always-appended stock row survives. */
    bs_row_t rows1[8];
    int n1 = bs_build_rows(dir, rows1, 1);
    check(n1 == 1, "max=1 yields exactly 1 row");
    if (n1 == 1)
        check(strcmp(rows1[0].id, "stock") == 0, "the one row is stock");
}

int main(void) {
    test_frame_zero_never_fires();
    test_held_then_release_press();
    test_terminator_hides_events_behind_it();
    test_cable_2_ignored();
    test_jog_delta();
    test_all_31_slots_no_oob();
    test_json_field();
    test_build_rows();
    test_build_rows_skips_stock_and_no_json_dirs();
    test_build_rows_max_constrained();

    if (failures == 0)
        printf("ALL PASS\n");
    else
        printf("%d FAILURE(S)\n", failures);
    return failures;
}
