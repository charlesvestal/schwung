/* Compatibility shims for running on Move's glibc (2.34).
 * Move's glibc doesn't have arc4random (2.36) or _dl_find_object (2.35).
 * These are provided as direct symbols via -Wl,--defsym won't work for
 * functions, so we use --wrap for arc4random and provide _dl_find_object
 * directly (it's weak in libgcc). */
#include <stdint.h>
#include <stdio.h>

/* arc4random was added in glibc 2.36 — used by Link SDK for random IDs */
uint32_t __wrap_arc4random(void)
{
    uint32_t val = 0;
    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        fread(&val, sizeof(val), 1, f);
        fclose(f);
    }
    return val;
}
