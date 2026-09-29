/*
 * test_heal_args — schwung-heal's command line is a CLOSED set.
 *
 * The binary setuid(0)s and bails unprivileged, so its argument policy is only
 * testable from host/heal_args.h. What matters: a launcher verb is accepted
 * only ALONE (it must never ride along with a mirror or a reboot), anything
 * unknown is refused, and the systemctl verb comes from a fixed pair.
 */
#include <stdio.h>
#include <string.h>

#include "heal_args.h"

static int failures = 0;

static void expect(int argc, const char *a1, const char *a2,
                   heal_mode_t want, int want_reboot, const char *why) {
    char *argv[4] = { (char *)"schwung-heal", (char *)a1, (char *)a2, NULL };
    int reboot = -1;
    heal_mode_t got = heal_parse_args(argc, argv, &reboot);
    if (got != want || (want != HEAL_MODE_INVALID && reboot != want_reboot)) {
        printf("FAIL: %s: mode=%d reboot=%d, want mode=%d reboot=%d\n",
               why, (int)got, reboot, (int)want, want_reboot);
        failures++;
    }
}

int main(void) {
    expect(1, NULL, NULL, HEAL_MODE_MIRROR, 0, "no args is the historic mirror");
    expect(2, "--reboot", NULL, HEAL_MODE_MIRROR, 1, "--reboot");
    expect(3, "--reboot", "--reboot", HEAL_MODE_MIRROR, 1, "repeated --reboot is harmless");
    expect(2, "--pause-launcher", NULL, HEAL_MODE_PAUSE_LAUNCHER, 0, "pause alone");
    expect(2, "--resume-launcher", NULL, HEAL_MODE_RESUME_LAUNCHER, 0, "resume alone");
    expect(3, "--pause-launcher", "--reboot", HEAL_MODE_INVALID, 0, "a verb never combines");
    expect(3, "--reboot", "--resume-launcher", HEAL_MODE_INVALID, 0, "a verb never combines");
    expect(3, "--pause-launcher", "--resume-launcher", HEAL_MODE_INVALID, 0, "two verbs");
    expect(2, "--pause-launcher=other.service", NULL, HEAL_MODE_INVALID, 0, "no unit argument");
    expect(2, "stop", NULL, HEAL_MODE_INVALID, 0, "a bare systemctl word is not a verb");
    expect(2, "", NULL, HEAL_MODE_INVALID, 0, "empty arg");

    if (strcmp(heal_launcher_verb(HEAL_MODE_PAUSE_LAUNCHER), "stop") != 0) { printf("FAIL: pause -> stop\n"); failures++; }
    if (strcmp(heal_launcher_verb(HEAL_MODE_RESUME_LAUNCHER), "start") != 0) { printf("FAIL: resume -> start\n"); failures++; }
    if (heal_launcher_verb(HEAL_MODE_MIRROR) != NULL) { printf("FAIL: mirror has no verb\n"); failures++; }
    if (heal_launcher_verb(HEAL_MODE_INVALID) != NULL) { printf("FAIL: invalid has no verb\n"); failures++; }
    if (strcmp(HEAL_LAUNCHER_UNIT, "move-launcher.service") != 0) { printf("FAIL: unit name\n"); failures++; }

    if (failures) return 1;
    printf("PASS: test_heal_args\n");
    return 0;
}
