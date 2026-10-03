/*
 * schwung_port.h — the platform primitives that differ between the Move
 * device (Linux / glibc) and the desktop host (macOS).
 *
 * This header exists so that the three places the chain host touches the
 * operating system directly are named once rather than carrying an #ifdef
 * each. It is deliberately tiny: everything else in src/modules/chain/dsp/
 * is portable C and must stay that way.
 *
 * WHAT IS NOT HERE, AND WHY. dlopen/dlsym are already portable. dlinfo() is
 * not, but it has a single call site (logging a load base for crash
 * attribution) and no desktop meaning, so it is guarded in place rather than
 * given a shim that would have to invent an answer.
 */
#ifndef SCHWUNG_PORT_H
#define SCHWUNG_PORT_H

/* ------------------------------------------------------------- semaphore --
 *
 * A counting semaphore used to wake the bus worker.
 *
 * THE POST SIDE MUST NOT TAKE A LOCK. It runs from the audio callback, and a
 * lock shared with the SCHED_OTHER worker is a priority inversion with no
 * inheritance — the whole reason chain_bus.c reaches for a semaphore instead
 * of a condition variable in the first place. Any replacement has to keep
 * that property, which rules out the obvious mutex+condvar emulation.
 *
 * Linux: an unnamed POSIX semaphore, futex-backed, uncontended post is a
 * single atomic.
 *
 * macOS: sem_init() is not merely deprecated there, it FAILS — unnamed POSIX
 * semaphores are unimplemented and return ENOSYS, so the device's code path
 * would silently never start its worker. GCD's semaphore is the supported
 * equivalent and its signal path is an atomic increment with no lock in the
 * uncontended case.
 */
#if defined(__APPLE__)

#include <dispatch/dispatch.h>

typedef dispatch_semaphore_t schwung_sem_t;

static inline int schwung_sem_init(schwung_sem_t *s) {
    *s = dispatch_semaphore_create(0);
    return *s ? 0 : -1;
}
static inline void schwung_sem_post(schwung_sem_t *s) {
    dispatch_semaphore_signal(*s);
}
/* Returns 0 on success. Never reports EINTR — GCD restarts internally — but
 * the caller's retry loop is harmless and stays as written. */
static inline int schwung_sem_wait(schwung_sem_t *s) {
    dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER);
    return 0;
}
/* Only safe with no waiter outstanding: GCD traps if a semaphore is released
 * while its value is below what it was created with. Every caller joins the
 * worker first, which is the same ordering sem_destroy() already required. */
static inline void schwung_sem_destroy(schwung_sem_t *s) {
    if (*s) { dispatch_release(*s); *s = NULL; }
}

#else /* Linux and anything else with working unnamed POSIX semaphores */

#include <semaphore.h>
#include <errno.h>

typedef sem_t schwung_sem_t;

static inline int schwung_sem_init(schwung_sem_t *s)   { return sem_init(s, 0, 0); }
static inline void schwung_sem_post(schwung_sem_t *s)  { sem_post(s); }
static inline int schwung_sem_wait(schwung_sem_t *s)   { return sem_wait(s); }
static inline void schwung_sem_destroy(schwung_sem_t *s) { sem_destroy(s); }

#endif

/* ---------------------------------------------------------------- worker --
 *
 * Put the calling thread where a non-realtime worker belongs.
 *
 * On Move that is explicit: drop to SCHED_OTHER (the thread inherits the
 * audio callback's FIFO 70 otherwise, which starves Move's own Link Main at
 * 35) and pin to cores 0-2, leaving core 3 for SPI.
 *
 * On a desktop host neither half applies. There is no inherited realtime
 * priority to shed — the plugin's worker is created from an ordinary thread —
 * and macOS deliberately exposes no CPU affinity API, because the scheduler
 * owns that decision. Saying so here is the point: this is a no-op by
 * analysis, not an unported stub.
 */
#if defined(__linux__)

/* _GNU_SOURCE BEFORE <sched.h>, and it must be set here rather than left to
 * the including .c file. CPU_ZERO/CPU_SET/sched_setaffinity are glibc
 * extensions that sched.h only declares under it — chain_bus.c and
 * chain_host.c define it at the top for exactly this reason, but
 * chain_internal.h now pulls this header into six other translation units
 * that do not. Without it they compile with three IMPLICIT DECLARATIONS and
 * link clean anyway, because a shared object is permitted to carry undefined
 * symbols: the failure moves to dlopen on the device. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

static inline void schwung_thread_become_worker(void) {
    struct sched_param sp = {0};
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);   /* core 3 is SPI's */
    sched_setaffinity(0, sizeof(set), &set);
}

#else

static inline void schwung_thread_become_worker(void) { }

#endif

/* ------------------------------------------------------------------ heap --
 *
 * Hand freed arenas back to the OS after a chain teardown. glibc only; on
 * other allocators the release is automatic or unavailable, and in both cases
 * there is nothing to ask for.
 */
#if defined(__linux__) && defined(__GLIBC__)

#include <malloc.h>

static inline void schwung_malloc_trim(void) { malloc_trim(0); }

#else

static inline void schwung_malloc_trim(void) { }

#endif

#endif /* SCHWUNG_PORT_H */
