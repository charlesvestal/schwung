// SPDX-License-Identifier: MIT
// In-process round-trip test for src/host/sim_backend.c.
//   open → mmap → spawn ticker thread → ioctl_wait → expect PASS

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "host/sim_backend.h"

static int g_fail = 0;
static void fail(const char *r) { fprintf(stderr, "FAIL: %s\n", r); g_fail++; }

static void *ticker(void *_) {
    (void)_;
    usleep(100 * 1000);
    int fd = schwung_sim_get_tick_fd();
    if (fd < 0) { fail("get_tick_fd < 0"); return NULL; }
    const char b = 1;
    if (write(fd, &b, 1) != 1) fail("write tick != 1");
    return NULL;
}

int main(void) {
    int fd = schwung_sim_open();
    if (fd < 0) { fail("schwung_sim_open < 0"); return 1; }

    uint8_t *m = schwung_sim_mmap();
    if (!m) { fail("schwung_sim_mmap NULL"); return 1; }

    pthread_t t;
    if (pthread_create(&t, NULL, ticker, NULL) != 0) { fail("pthread_create"); return 1; }

    int rv = schwung_sim_ioctl_wait();
    if (rv != 0) fail("ioctl_wait != 0");

    pthread_join(t, NULL);
    schwung_sim_close();

    if (g_fail == 0) { printf("PASS\n"); return 0; }
    return 1;
}
