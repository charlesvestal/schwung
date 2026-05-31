// SPDX-License-Identifier: MIT
//
// Thin platform-aware SPI wrappers for schwung-host.
//
// On Linux/Move device: real /dev/ablspi0.0 syscalls.
// On macOS (SCHWUNG_SIM_BACKEND): in-process sim backend (src/host/sim_backend.c).
//
// schwung-host calls these instead of bare libc so the host source has a
// single #ifdef boundary instead of dozens at every ioctl/open/mmap site.

#ifndef SCHWUNG_HOST_SPI_IO_H
#define SCHWUNG_HOST_SPI_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __APPLE__

#include "host/sim_backend.h"

static inline int spi_open_device(void) {
    return schwung_sim_open();
}

static inline void *spi_mmap_device(int fd, size_t length) {
    (void)fd; (void)length;
    return (void *)schwung_sim_mmap();
}

// One SPI frame: schwung-host loop calls this each tick.
// In sim mode this blocks on the tick pipe; the sim daemon pulses it.
static inline int spi_wait_send_message(int fd) {
    (void)fd;
    return schwung_sim_ioctl_wait();
}

// No-op in sim: no SPI peripheral to configure.
static inline int spi_set_speed(int fd, int hz) {
    (void)fd; (void)hz;
    return 0;
}

#else  // Linux / Move device

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static inline int spi_open_device(void) {
    return open("/dev/ablspi0.0", O_RDWR);
}

static inline void *spi_mmap_device(int fd, size_t length) {
    return mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

static inline int spi_wait_send_message(int fd) {
    return ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
}

static inline int spi_set_speed(int fd, int hz) {
    return ioctl(fd, _IOC(_IOC_NONE, 0, 0xb, 0), hz);
}

#endif

#endif // SCHWUNG_HOST_SPI_IO_H
