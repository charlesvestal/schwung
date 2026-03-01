/*
 * web_shim.c - Tiny LD_PRELOAD shim for MoveWebService
 *
 * Hooks recv/recvfrom/read to detect PIN challenge requests.
 * Sets a flag in shared memory so the main shim can read
 * the PIN from the display and speak it via TTS.
 *
 * Build: gcc -shared -fPIC -o web_shim.so web_shim.c -ldl -lrt
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include "shadow_constants.h"
#include "unified_log.h"

static ssize_t (*real_recvfrom)(int, void *, size_t, int,
                                struct sockaddr *, socklen_t *) = NULL;
static ssize_t (*real_recv)(int, void *, size_t, int) = NULL;
static ssize_t (*real_read)(int, void *, size_t) = NULL;
static shadow_control_t *ctrl = NULL;

#define WEB_SHIM_LOG_SOURCE "web_shim"

static void dbg_log(const char *msg)
{
    if (!msg) return;
    unified_log(WEB_SHIM_LOG_SOURCE, LOG_LEVEL_DEBUG, "%s", msg);
}

static void init_shm(void)
{
    int fd = shm_open(SHM_SHADOW_CONTROL, O_RDWR, 0666);
    if (fd < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "shm_open failed: errno=%d", errno);
        dbg_log(buf);
        return;
    }

    void *p = mmap(NULL, CONTROL_BUFFER_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (p == MAP_FAILED) {
        char buf[128];
        snprintf(buf, sizeof(buf), "mmap failed: errno=%d", errno);
        dbg_log(buf);
        return;
    }

    ctrl = (shadow_control_t *)p;
    dbg_log("web_shim: shm mapped OK");
}

/* Constructor runs when .so is loaded - before main() */
__attribute__((constructor))
static void web_shim_init(void)
{
    unified_log_init();
    dbg_log("web_shim: constructor called - .so loaded!");

    real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
    real_recv = dlsym(RTLD_NEXT, "recv");
    real_read = dlsym(RTLD_NEXT, "read");

    char buf[256];
    snprintf(buf, sizeof(buf), "web_shim: recvfrom=%p recv=%p read=%p",
             (void *)real_recvfrom, (void *)real_recv, (void *)real_read);
    dbg_log(buf);

    init_shm();
}

/* Scan buffer for challenge endpoints */
static void scan_for_challenge(const void *buf, ssize_t n)
{
    if (n <= 0) return;
    /* Lazy retry shm_open if it wasn't available at constructor time */
    if (!ctrl) init_shm();
    if (!ctrl) return;

    const char *haystack = (const char *)buf;
    size_t scan_len = (size_t)n < 512 ? (size_t)n : 512;

    for (size_t i = 0; i + 20 <= scan_len; i++) {
        if (haystack[i] != '/') continue;

        if (i + 26 <= scan_len &&
            memcmp(haystack + i, "/api/v1/challenge-response", 26) == 0) {
            ctrl->pin_challenge_active = 2;
            dbg_log("web_shim: challenge-response detected");
            return;
        }
        if (i + 17 <= scan_len &&
            memcmp(haystack + i, "/api/v1/challenge", 17) == 0) {
            char after = (i + 17 < scan_len) ? haystack[i + 17] : '\0';
            if (after != '-') {
                ctrl->pin_challenge_active = 1;
                dbg_log("web_shim: challenge detected!");
                return;
            }
        }
    }
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    ssize_t n = real_recvfrom(fd, buf, len, flags, src_addr, addrlen);
    scan_for_challenge(buf, n);
    return n;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    ssize_t n = real_recv(sockfd, buf, len, flags);
    scan_for_challenge(buf, n);
    return n;
}

ssize_t read(int fd, void *buf, size_t count)
{
    ssize_t n = real_read(fd, buf, count);
    /* Only scan fds that could be sockets (fd > 2 to skip stdin/out/err) */
    if (fd > 2) scan_for_challenge(buf, n);
    return n;
}
