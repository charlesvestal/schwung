/* Stem filename derivation — the string arithmetic behind "<base>_Slot1.wav".
 *
 * Small, but it has two callers in different halves of the subsystem (a
 * sampler take and a skipback save) and every one of the cases below is a real
 * path this code sees on the device: the sampler's auto-named
 * "sample_..._120bpm.wav", Song Mode's "<set name>_<timestamp>.wav" where the
 * set name is whatever the user typed, and the skipback's
 * "skipback_<timestamp>.wav" under a dated directory.
 *
 * The failure this exists to prevent is silent: a wrong split still produces a
 * filename, the WAV still writes, and it lands somewhere nobody looks. */

#include "../../src/host/sampler_stem_path.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void expect(const char *master, const char *suffix, const char *want) {
    char got[300];
    sampler_stem_path_build(got, sizeof(got), master, suffix);
    if (strcmp(got, want) != 0) {
        printf("FAIL: (\"%s\", \"%s\") -> \"%s\", expected \"%s\"\n",
               master, suffix, got, want);
        fails++;
    }
}

int main(void) {
    /* The ordinary case: the suffix goes BEFORE the extension, so the file is
     * still a .wav and every tool that dispatches on extension still opens
     * it. Appending after ("x.wav_Slot1") would produce something no sampler
     * on the far end would import. */
    expect("/a/b/sample_20260902_120bpm.wav", "Slot1",
           "/a/b/sample_20260902_120bpm_Slot1.wav");
    expect("/a/b/skipback_20260902_143012.wav", "Move",
           "/a/b/skipback_20260902_143012_Move.wav");

    /* Song Mode names the file after the SET, and a set name is user text.
     * A dot in it is not an extension boundary — splitting on the first dot
     * would file the stems under a truncated name that no longer matches the
     * master sitting beside them. */
    expect("/Recordings/my.set.v2_20260902.wav", "Slot3",
           "/Recordings/my.set.v2_20260902_Slot3.wav");

    /* No extension at all: append one rather than inventing a split. */
    expect("/Recordings/take", "Slot2", "/Recordings/take_Slot2.wav");

    /* A dot in a DIRECTORY with no extension on the file. Searching the whole
     * path for the last '.' finds the directory's and cuts the path in half,
     * writing the stem into a directory that does not exist. Which is why the
     * search starts after the last '/'. */
    expect("/data/UserData/v0.9/take", "Slot1", "/data/UserData/v0.9/take_Slot1.wav");

    /* A leading dot is a hidden file, not an extension. Treating it as one
     * leaves the name empty: "/a/_Slot1.wav". */
    expect("/a/.wav", "Slot1", "/a/.wav_Slot1.wav");

    /* Relative paths and a bare filename still work — nothing here requires a
     * leading slash. */
    expect("take.wav", "Move", "take_Move.wav");

    /* Defensive: a NULL in either position empties the buffer rather than
     * leaving whatever the caller's stack held, which would then be fopen'd. */
    {
        char got[64];
        memset(got, 'x', sizeof(got));
        sampler_stem_path_build(got, sizeof(got), NULL, "Slot1");
        if (got[0] != '\0') { printf("FAIL: NULL master did not empty the buffer\n"); fails++; }
        memset(got, 'x', sizeof(got));
        sampler_stem_path_build(got, sizeof(got), "/a/b.wav", NULL);
        if (got[0] != '\0') { printf("FAIL: NULL suffix did not empty the buffer\n"); fails++; }
    }

    /* Truncation must not run off the end of a short buffer. snprintf bounds
     * it; this asserts that rather than trusting it, because the %.*s form
     * above is the one place a length is computed by hand. */
    {
        char small[16];
        memset(small, 'x', sizeof(small));
        sampler_stem_path_build(small, sizeof(small),
                                "/a/very_long_recording_name.wav", "Slot4");
        if (small[sizeof(small) - 1] != '\0') {
            printf("FAIL: short buffer is not NUL-terminated\n");
            fails++;
        }
    }

    if (fails) return 1;
    printf("PASS: sampler stem path derivation (suffix before the extension, "
           "split after the last '/', dotted set names and dotted directories "
           "intact, bounded)\n");
    return 0;
}
