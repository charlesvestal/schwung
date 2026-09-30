/*
 * heal_args.h — schwung-heal's whole command line, as a closed set.
 *
 * Split out from schwung-heal.c so tests/host can run it: the binary setuid(0)s
 * at startup and refuses to run unprivileged.
 *
 * Two modes, and they never mix:
 *
 *   (none) / --reboot      the historic duty: self-update, tool helpers, the
 *                          shim + entrypoint mirror, and optionally a reboot.
 *   --pause-launcher       `systemctl stop  move-launcher.service`, nothing else.
 *   --resume-launcher      `systemctl start move-launcher.service`, nothing else.
 *
 * Why the launcher verbs exist: move-launcher.service is supervised with
 * Restart=on-failure, so launch-standalone.sh killing MoveLauncher made systemd
 * bring the WHOLE stock stack back ~2 s later, alongside the standalone binary,
 * both driving /dev/ablspi0.0. The script runs as ableton and cannot stop a
 * unit. The verb picks one of two hardcoded systemctl words and the unit name
 * is a compile-time constant, so no caller-supplied string reaches execl().
 * Surfaced by dbxhost (legsmechanical), whose own helper carries the same pair.
 *
 * A launcher verb deliberately skips the mirror duties: "pause the supervisor"
 * must not also rewrite /usr/lib, and an audit of either should not need to
 * read the other.
 */
#ifndef HEAL_ARGS_H
#define HEAL_ARGS_H

#include <string.h>

#define HEAL_SYSTEMCTL      "/usr/bin/systemctl"
#define HEAL_LAUNCHER_UNIT  "move-launcher.service"

typedef enum {
    HEAL_MODE_INVALID = 0,
    HEAL_MODE_MIRROR,          /* no args, or --reboot */
    HEAL_MODE_PAUSE_LAUNCHER,
    HEAL_MODE_RESUME_LAUNCHER,
} heal_mode_t;

/* Parse argv. *do_reboot is set only in MIRROR mode. A launcher verb must be
 * the ONLY argument; anything unknown, repeated or combined is INVALID. */
static inline heal_mode_t heal_parse_args(int argc, char **argv, int *do_reboot) {
    if (do_reboot) *do_reboot = 0;
    if (argc <= 1) return HEAL_MODE_MIRROR;
    if (argc == 2 && argv[1]) {
        if (strcmp(argv[1], "--pause-launcher") == 0)  return HEAL_MODE_PAUSE_LAUNCHER;
        if (strcmp(argv[1], "--resume-launcher") == 0) return HEAL_MODE_RESUME_LAUNCHER;
    }
    int reboot = 0;
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || strcmp(argv[i], "--reboot") != 0) return HEAL_MODE_INVALID;
        reboot = 1;
    }
    if (do_reboot) *do_reboot = reboot;
    return HEAL_MODE_MIRROR;
}

/* The systemctl verb for a launcher mode, from a closed pair; NULL otherwise. */
static inline const char *heal_launcher_verb(heal_mode_t mode) {
    if (mode == HEAL_MODE_PAUSE_LAUNCHER)  return "stop";
    if (mode == HEAL_MODE_RESUME_LAUNCHER) return "start";
    return NULL;
}

#endif /* HEAL_ARGS_H */
