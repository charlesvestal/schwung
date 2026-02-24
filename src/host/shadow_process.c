/* shadow_process.c - Shadow UI and Link subscriber process management
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include "shadow_process.h"
#include "shadow_link_audio.h"
#include "unified_log.h"

/* ============================================================================
 * Static host callbacks
 * ============================================================================ */

static process_host_t host;
static int process_initialized = 0;

/* ============================================================================
 * Global definitions
 * ============================================================================ */

volatile int link_sub_started = 0;
volatile pid_t link_sub_pid = -1;
volatile uint32_t link_sub_ever_received = 0;
volatile int link_sub_restart_count = 0;

/* ============================================================================
 * Internal state
 * ============================================================================ */

/* Shadow UI */
static int shadow_ui_started = 0;
static pid_t shadow_ui_pid = -1;
static const char *shadow_ui_pid_path = "/data/UserData/move-anything/shadow_ui.pid";

/* Link subscriber monitor */
static volatile int link_sub_monitor_started = 0;
static volatile int link_sub_monitor_running = 0;
static pthread_t link_sub_monitor_thread;

/* Recovery constants */
#define LINK_SUB_STALE_THRESHOLD_MS 5000
#define LINK_SUB_WAIT_MS            3000
#define LINK_SUB_COOLDOWN_MS        10000
#define LINK_SUB_ALIVE_CHECK_MS     5000
#define LINK_SUB_MONITOR_POLL_US    100000

/* ============================================================================
 * Init
 * ============================================================================ */

void process_init(const process_host_t *h) {
    host = *h;
    shadow_ui_started = 0;
    shadow_ui_pid = -1;
    link_sub_started = 0;
    link_sub_pid = -1;
    link_sub_ever_received = 0;
    link_sub_restart_count = 0;
    link_sub_monitor_started = 0;
    link_sub_monitor_running = 0;
    process_initialized = 1;
}

/* ============================================================================
 * Launch and replace (standalone mode)
 * ============================================================================ */

void launchChildAndKillThisProcess(char *pBinPath, char *pBinName, char *pArgs) {
    int pid = fork();

    if (pid < 0) {
        printf("Fork failed\n");
        exit(1);
    } else if (pid == 0) {
        setsid();
        printf("Child process running in the background...\n");
        printf("Args: %s\n", pArgs);

        printf("Closing file descriptors...\n");
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++) {
            close(i);
        }

        execl(pBinPath, pBinName, pArgs, (char *)0);
        perror("execl failed");
        _exit(1);
    } else {
        kill(getpid(), SIGINT);
    }
}

/* ============================================================================
 * Shadow UI process management
 * ============================================================================ */

static int shadow_ui_pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int rpid = 0;
    char comm[64] = {0};
    char state = 0;
    int matched = fscanf(f, "%d %63s %c", &rpid, comm, &state);
    fclose(f);
    if (matched != 3) return 0;
    if (rpid != (int)pid) return 0;
    if (state == 'Z') return 0;
    if (!strstr(comm, "shadow_ui")) return 0;
    return 1;
}

static pid_t shadow_ui_read_pid(void) {
    FILE *pid_file = fopen(shadow_ui_pid_path, "r");
    if (!pid_file) return -1;
    long pid = -1;
    if (fscanf(pid_file, "%ld", &pid) != 1) {
        pid = -1;
    }
    fclose(pid_file);
    return (pid_t)pid;
}

static void shadow_ui_refresh_pid(void) {
    if (shadow_ui_pid_alive(shadow_ui_pid)) {
        shadow_ui_started = 1;
        return;
    }
    pid_t pid = shadow_ui_read_pid();
    if (shadow_ui_pid_alive(pid)) {
        shadow_ui_pid = pid;
        shadow_ui_started = 1;
        return;
    }
    if (pid > 0) {
        unlink(shadow_ui_pid_path);
    }
    shadow_ui_pid = -1;
    shadow_ui_started = 0;
}

static void shadow_ui_reap(void) {
    if (shadow_ui_pid <= 0) return;
    int status = 0;
    pid_t res = waitpid(shadow_ui_pid, &status, WNOHANG);
    if (res == shadow_ui_pid) {
        shadow_ui_pid = -1;
        shadow_ui_started = 0;
    }
}

void launch_shadow_ui(void) {
    if (shadow_ui_started && shadow_ui_pid > 0) return;
    shadow_ui_reap();
    shadow_ui_refresh_pid();
    if (shadow_ui_started && shadow_ui_pid > 0) return;
    if (access("/data/UserData/move-anything/shadow/shadow_ui", X_OK) != 0) {
        return;
    }

    int pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        setsid();
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++) {
            close(i);
        }
        execl("/data/UserData/move-anything/shadow/shadow_ui", "shadow_ui", (char *)0);
        _exit(1);
    }
    shadow_ui_started = 1;
    shadow_ui_pid = pid;
}

/* ============================================================================
 * Link subscriber process management
 * ============================================================================ */

static int link_sub_pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int rpid = 0;
    char comm[64] = {0};
    char state = 0;
    int matched = fscanf(f, "%d %63s %c", &rpid, comm, &state);
    fclose(f);
    if (matched != 3) return 0;
    if (rpid != (int)pid) return 0;
    if (state == 'Z') return 0;
    if (!strstr(comm, "link-sub")) return 0;
    return 1;
}

static void link_sub_reap(void) {
    if (link_sub_pid <= 0) return;
    int status = 0;
    pid_t res = waitpid(link_sub_pid, &status, WNOHANG);
    if (res == link_sub_pid) {
        link_sub_pid = -1;
        link_sub_started = 0;
    }
}

void link_sub_kill(void) {
    if (link_sub_pid > 0) {
        kill(link_sub_pid, SIGTERM);
    }
}

static void link_sub_kill_orphans(void) {
    DIR *dp = opendir("/proc");
    if (!dp) return;
    struct dirent *ent;
    pid_t my_pid = getpid();
    while ((ent = readdir(dp)) != NULL) {
        int pid = atoi(ent->d_name);
        if (pid <= 1) continue;
        if (pid == my_pid) continue;
        if (pid == link_sub_pid) continue;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int rpid = 0;
        char comm[64] = {0};
        char state = 0;
        int matched = fscanf(f, "%d %63s %c", &rpid, comm, &state);
        fclose(f);
        if (matched != 3) continue;
        if (state == 'Z') continue;
        if (!strstr(comm, "link-sub")) continue;

        unified_log("shim", LOG_LEVEL_INFO,
                    "Killing orphaned link-subscriber pid=%d", pid);
        kill(pid, SIGTERM);
        usleep(50000);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }
    closedir(dp);
}

void launch_link_subscriber(void) {
    if (link_sub_started && link_sub_pid > 0) return;
    link_sub_reap();
    if (link_sub_started && link_sub_pid > 0) return;

    link_sub_kill_orphans();

    const char *sub_path = "/data/UserData/move-anything/link-subscriber";
    if (access(sub_path, X_OK) != 0) return;

    /* Write current tempo to file so subscriber uses it instead of 120 */
    if (host.get_bpm) {
        float bpm = host.get_bpm(NULL);
        FILE *fp = fopen("/tmp/link-tempo", "w");
        if (fp) {
            fprintf(fp, "%.1f\n", bpm);
            fclose(fp);
        }
    }

    int pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        freopen("/tmp/link-subscriber.log", "w", stdout);
        freopen("/tmp/link-subscriber.log", "a", stderr);
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++) {
            close(i);
        }
        unsetenv("LD_PRELOAD");
        execl(sub_path, "link-subscriber", (char *)0);
        _exit(1);
    }
    link_sub_started = 1;
    link_sub_pid = pid;
    unified_log("shim", LOG_LEVEL_INFO,
                "Link subscriber launched: pid=%d", pid);
}

/* ============================================================================
 * Link subscriber monitor thread
 * ============================================================================ */

static uint64_t link_sub_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void *link_sub_monitor_main(void *arg) {
    (void)arg;

    if (!host.link_audio) return NULL;

    uint32_t last_packets = host.link_audio->packets_intercepted;
    uint64_t last_packet_ms = link_sub_now_ms();
    uint64_t cooldown_until_ms = 0;
    uint64_t kill_deadline_ms = 0;
    uint64_t next_alive_check_ms = last_packet_ms + LINK_SUB_ALIVE_CHECK_MS;
    int kill_pending = 0;

    if (last_packets > link_sub_ever_received) {
        link_sub_ever_received = last_packets;
    }

    while (link_sub_monitor_running) {
        uint64_t now_ms = link_sub_now_ms();

        if (!host.link_audio->enabled) {
            usleep(LINK_SUB_MONITOR_POLL_US);
            continue;
        }

        uint32_t packets_now = host.link_audio->packets_intercepted;
        if (packets_now != last_packets) {
            last_packets = packets_now;
            last_packet_ms = now_ms;
            if (packets_now > link_sub_ever_received) {
                link_sub_ever_received = packets_now;
            }
        }

        if (kill_pending) {
            if (now_ms >= kill_deadline_ms) {
                link_sub_reap();
                pid_t pid = link_sub_pid;
                if (pid > 0) {
                    kill(pid, SIGKILL);
                    waitpid(pid, NULL, 0);
                    link_sub_pid = -1;
                    link_sub_started = 0;
                }
                kill_pending = 0;
                link_sub_reset_state();
                launch_link_subscriber();
                link_sub_restart_count++;
                cooldown_until_ms = now_ms + LINK_SUB_COOLDOWN_MS;
                last_packets = host.link_audio->packets_intercepted;
                last_packet_ms = now_ms;
                next_alive_check_ms = now_ms + LINK_SUB_ALIVE_CHECK_MS;
                unified_log("shim", LOG_LEVEL_INFO,
                            "Link subscriber restarted after stale detection (restart #%d)",
                            (int)link_sub_restart_count);
            }
            usleep(LINK_SUB_MONITOR_POLL_US);
            continue;
        }

        if (link_sub_ever_received > 0 &&
            now_ms > last_packet_ms + LINK_SUB_STALE_THRESHOLD_MS &&
            now_ms >= cooldown_until_ms) {
            pid_t pid = link_sub_pid;
            unified_log("shim", LOG_LEVEL_INFO,
                        "Link audio stale detected: la_ever=%u, killing subscriber pid=%d",
                        link_sub_ever_received, (int)pid);
            link_sub_kill();
            kill_pending = 1;
            kill_deadline_ms = now_ms + LINK_SUB_WAIT_MS;
            usleep(LINK_SUB_MONITOR_POLL_US);
            continue;
        }

        if (now_ms >= next_alive_check_ms) {
            next_alive_check_ms = now_ms + LINK_SUB_ALIVE_CHECK_MS;
            link_sub_reap();
            pid_t pid = link_sub_pid;
            if (link_sub_started && !link_sub_pid_alive(pid) &&
                now_ms >= cooldown_until_ms) {
                unified_log("shim", LOG_LEVEL_INFO,
                            "Link subscriber died (pid=%d), restarting",
                            (int)pid);
                link_sub_pid = -1;
                link_sub_started = 0;
                link_sub_reset_state();
                launch_link_subscriber();
                link_sub_restart_count++;
                cooldown_until_ms = now_ms + LINK_SUB_COOLDOWN_MS;
                last_packets = host.link_audio->packets_intercepted;
                last_packet_ms = now_ms;
            }
        }

        usleep(LINK_SUB_MONITOR_POLL_US);
    }

    return NULL;
}

void start_link_sub_monitor(void) {
    if (link_sub_monitor_started) return;

    link_sub_monitor_running = 1;
    int rc = pthread_create(&link_sub_monitor_thread, NULL, link_sub_monitor_main, NULL);
    if (rc != 0) {
        link_sub_monitor_running = 0;
        unified_log("shim", LOG_LEVEL_WARN,
                    "Link subscriber monitor start failed: %s",
                    strerror(rc));
        return;
    }

    pthread_detach(link_sub_monitor_thread);
    link_sub_monitor_started = 1;
    unified_log("shim", LOG_LEVEL_INFO, "Link subscriber monitor started");
}

void link_sub_reset_state(void) {
    /* Delegate to link audio module for protocol state reset */
    link_audio_reset_state();
    link_sub_ever_received = 0;
}
