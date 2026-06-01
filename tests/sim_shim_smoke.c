// SPDX-License-Identifier: MIT
//
// Sim B Phase 2 smoke test for schwung_spi_lib's sim mode.
//
// Verifies that under SCHWUNG_SPI_BACKEND=sim + LD_PRELOAD=libschwung_sim.so:
//   open("/dev/ablspi0.0") returns a real fd (no real device touched)
//   mmap on that fd returns a usable 4 KB shadow buffer
//   ioctl(WAIT_SEND_SIZE) blocks on the tick pipe and returns 0 once pulsed
//
// Compile + run (the trailing backslashes are ignored by GCC's preprocessor):
//   gcc -shared -fPIC -DSCHWUNG_LIB_WITH_SIM
//       src/lib/schwung_spi_lib.c src/host/sim_backend.c
//       -o /tmp/libschwung_sim.so -lpthread
//   gcc tests/sim_shim_smoke.c -o /tmp/sim_shim_smoke -lpthread
//   SCHWUNG_SPI_BACKEND=sim LD_PRELOAD=/tmp/libschwung_sim.so /tmp/sim_shim_smoke

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ABLSPI_DEV            "/dev/ablspi0.0"
#define SCHWUNG_PAGE_SIZE     4096
#define IOCTL_WAIT_SEND_SIZE  10

// Provided by libschwung_sim.so (sim_backend.c). Weak so the linker tolerates
// it being unresolved at link time; LD_PRELOAD supplies it at process start.
extern int schwung_sim_get_tick_fd(void) __attribute__((weak));

static void *pulse_thread(void *arg) {
    (void)arg;
    struct timespec t = {0, 100 * 1000 * 1000};  // 100 ms
    nanosleep(&t, NULL);
    if (!schwung_sim_get_tick_fd) {
        fprintf(stderr, "FAIL: schwung_sim_get_tick_fd not provided "
                "(is the lib LD_PRELOADed?)\n");
        return NULL;
    }
    int tick_fd = schwung_sim_get_tick_fd();
    if (tick_fd < 0) {
        fprintf(stderr, "FAIL: tick fd unavailable (%d)\n", tick_fd);
        return NULL;
    }
    char b = 1;
    if (write(tick_fd, &b, 1) != 1) {
        fprintf(stderr, "FAIL: write to tick fd: %s\n", strerror(errno));
    }
    return NULL;
}

int main(void) {
    int fd = open(ABLSPI_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open(%s) errno=%d (%s)\n",
                ABLSPI_DEV, errno, strerror(errno));
        return 1;
    }
    fprintf(stderr, "OK: open fd=%d (sim tempfile, not /dev/ablspi0.0)\n", fd);

    void *m = mmap(NULL, SCHWUNG_PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED || !m) {
        fprintf(stderr, "FAIL: mmap returned %p errno=%d (%s)\n",
                m, errno, strerror(errno));
        return 1;
    }
    fprintf(stderr, "OK: mmap shadow=%p\n", m);

    // Write a sentinel into the TX region; the lib should copy it to hw on
    // the next ioctl barrier.
    ((unsigned char *)m)[256] = 0xA5;

    pthread_t th;
    pthread_create(&th, NULL, pulse_thread, NULL);

    int r = ioctl(fd, IOCTL_WAIT_SEND_SIZE, 0);
    if (r != 0) {
        fprintf(stderr, "FAIL: ioctl returned %d errno=%d (%s)\n",
                r, errno, strerror(errno));
        return 1;
    }
    fprintf(stderr, "OK: ioctl(WAIT_SEND_SIZE) returned 0\n");

    pthread_join(th, NULL);
    close(fd);
    fprintf(stderr, "PASS\n");
    return 0;
}
