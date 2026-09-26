/* Boot-time read of Move's track mute/solo (src/host/song_abl_mix.h).
 *
 * Shaped from real Song.abl files (firmware 2.1.x, verified against
 * JSON.parse on five sets). The cases that matter are the ones a key search
 * gets wrong: speakerOn's OBJECT form, and the same keys on a drum cell's
 * mixer deeper inside the same track.
 */
#include <stdio.h>
#include <string.h>
#include "song_abl_mix.h"

static int failures = 0;

static void expect(const char *what, const char *json, int want_n,
                   const int want_m[4], const int want_s[4])
{
    int m[4], s[4];
    int n = song_abl_mix_parse(json, m, s);
    int ok = (n == want_n);
    for (int i = 0; ok && want_n == 4 && i < 4; i++)
        ok = (m[i] == want_m[i] && s[i] == want_s[i]);
    if (!ok) {
        printf("FAIL: %s: n=%d m=[%d,%d,%d,%d] s=[%d,%d,%d,%d]\n", what, n,
               m[0], m[1], m[2], m[3], s[0], s[1], s[2], s[3]);
        failures++;
    }
}

#define MIX(on, solo) "\"mixer\": {\"pan\": 0.0, \"solo-cue\": " solo ", \"speakerOn\": " on ", \"volume\": 0.0, \"sends\": []}"
#define TRACK(name, mixer) "{\"name\": \"" name "\", \"devices\": [], " mixer "}"

int main(void)
{
    static const int z[4] = {0, 0, 0, 0};

    expect("pretty, track 1 muted (Set 5)",
        "{\n  \"tempo\": 120.0,\n  \"tracks\": [\n"
        "    " TRACK("", MIX("false", "false")) ",\n"
        "    " TRACK("", MIX("true", "false")) ",\n"
        "    " TRACK("", MIX("true", "false")) ",\n"
        "    " TRACK("", MIX("true", "false")) "\n  ],\n"
        "  \"returnTracks\": [{\"mixer\": {\"speakerOn\": false, \"solo-cue\": true}}]\n}",
        4, (const int[]){1, 0, 0, 0}, z);

    expect("track 1 soloed (Set 3)",
        "{\"tracks\": [" TRACK("", MIX("true", "true")) "," TRACK("", MIX("true", "false")) ","
                          TRACK("", MIX("true", "false")) "," TRACK("", MIX("true", "false")) "]}",
        4, z, (const int[]){1, 0, 0, 0});

    expect("speakerOn object form is muted",
        "{\"tracks\": [" TRACK("", MIX("true", "false")) "," TRACK("", MIX("true", "false")) ","
            TRACK("", MIX("{\"value\": false, \"presetValue\": true}", "false")) ","
            TRACK("", MIX("true", "false")) "]}",
        4, (const int[]){0, 0, 1, 0}, z);

    expect("drum-cell mixer deeper in the track is not the track",
        "{\"tracks\": ["
        "{\"name\": \"Drums\", \"devices\": [{\"chains\": [{\"devices\": [{\"chains\": ["
            "{\"mixer\": {\"speakerOn\": {\"value\": false, \"presetValue\": true}, \"solo-cue\": true}}"
        "]}]}]}], " MIX("true", "false") "},"
        TRACK("", MIX("true", "false")) "," TRACK("", MIX("true", "false")) ","
        TRACK("", MIX("true", "false")) "]}",
        4, z, z);

    expect("keys inside a string value are text",
        "{\"tracks\": [" TRACK("\\\"speakerOn\\\": false {[", MIX("true", "false")) ","
            TRACK("", MIX("true", "false")) "," TRACK("", MIX("true", "false")) ","
            TRACK("", MIX("true", "false")) "]}",
        4, z, z);

    expect("a set-level key named tracks deeper down is not the tracks",
        "{\"scenes\": [{\"tracks\": [" TRACK("", MIX("false", "true")) "]}], \"tracks\": ["
            TRACK("", MIX("true", "false")) "," TRACK("", MIX("true", "false")) ","
            TRACK("", MIX("true", "false")) "," TRACK("", MIX("true", "false")) "]}",
        4, z, z);

    expect("three tracks is not a whole answer",
        "{\"tracks\": [" TRACK("", MIX("false", "false")) "," TRACK("", MIX("true", "false")) ","
                          TRACK("", MIX("true", "false")) "]}",
        3, z, z);

    expect("no tracks", "{\"tempo\": 120}", 0, z, z);
    expect("empty", "", 0, z, z);
    expect("null", NULL, 0, z, z);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_song_abl_mix: all passed\n");
    return 0;
}
