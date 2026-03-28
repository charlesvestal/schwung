/* display_ctl — set display_active byte in JACK shadow driver shm.
 * Usage: display_ctl 1  (enable)  or  display_ctl 0  (disable)
 * Uses offsetof(SchwungJackShm, display_active) so it stays correct
 * when the struct layout changes. */

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "lib/schwung_jack_shm.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: display_ctl 0|1\n");
        return 1;
    }

    int fd = shm_open(SCHWUNG_JACK_SHM_PATH, O_RDWR, 0);
    if (fd < 0) {
        perror("shm_open");
        return 1;
    }

    void *ptr = mmap(NULL, SCHWUNG_JACK_SHM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    SchwungJackShm *shm = (SchwungJackShm *)ptr;
    shm->display_active = (argv[1][0] == '1') ? 1 : 0;

    munmap(ptr, SCHWUNG_JACK_SHM_SIZE);
    return 0;
}
