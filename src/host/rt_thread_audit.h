/* rt_thread_audit.h — find threads that inherited the SPI callback's priority.
 *
 * Module entry points ARE the SPI callback (SCHED_FIFO 90, core 3). POSIX
 * `PTHREAD_INHERIT_SCHED` is the default, so a `pthread_create` from
 * `create_instance` / `set_param` / `render_block` produces a worker born at
 * FIFO 90 — which starves Move's own `Link Main` (FIFO 35) for as long as it
 * runs. A 2026-08 source audit put this at seven plugins.
 *
 * **You cannot identify such a thread by name.** A child inherits the parent's
 * `comm`, so an inherited worker reports as "Audio Main/SPI" — indistinguishable
 * from the real SPI thread in `top`, in a thread list, or to its own author.
 * That invisibility is the whole reason the seven exist. tablor's worker ran
 * unnamed for months and only surfaced because someone went looking at the
 * per-thread `stat` files under /proc directly.
 *
 * So this does not match names. It watches the SET of realtime threads and
 * reports what is NEW since the last look, alongside the module most recently
 * loaded. An inherited thread cannot hide from that, whatever it calls itself.
 *
 * Everything here except `rt_thread_audit_scan` is pure — no I/O, no
 * allocation, no locks — and is unit-tested on the dev host (see
 * tests/host/test_rt_thread_audit.c). `rt_thread_audit_scan` reads /proc and
 * is therefore WORKER-ONLY: never call it from an SPI callback.
 */

#ifndef RT_THREAD_AUDIT_H
#define RT_THREAD_AUDIT_H

/* Move runs a handful of threads and a loaded fleet adds more; 64 is well
 * clear of both and bounds every loop here. A scan that fills the array
 * reports it rather than silently truncating. */
#define RT_AUDIT_MAX_THREADS 64

/* /proc caps `comm` at 15 chars + NUL. */
#define RT_AUDIT_COMM_LEN 16

/* Linux scheduling policies, spelled out so the parser needs no headers and
 * the tests need no Linux. */
#define RT_AUDIT_SCHED_OTHER 0
#define RT_AUDIT_SCHED_FIFO  1
#define RT_AUDIT_SCHED_RR    2

typedef struct {
    int  tid;
    int  policy;   /* RT_AUDIT_SCHED_* */
    int  rtprio;   /* 0 for SCHED_OTHER; 1-99 for FIFO/RR */
    int  cpu;      /* last CPU it ran on — core 3 is meant to be SPI's alone */
    char comm[RT_AUDIT_COMM_LEN];
} rt_thread_info_t;

/* Parse one line of /proc/<pid>/task/<tid>/stat. Returns 1 on success, 0 if
 * the line is malformed or truncated.
 *
 * The comm field is bracketed and may itself contain spaces and parentheses
 * ("Audio Main/SPI" has a space; a name like "worker (2)" has both), so it is
 * delimited by the LAST ')' rather than tokenised. Field numbers after it are
 * 1-based as in proc(5): 39 processor, 40 rt_priority, 41 policy. */
int rt_thread_parse_stat(const char *line, rt_thread_info_t *out);

/* Is this thread realtime-scheduled (FIFO or RR at a nonzero priority)? */
int rt_thread_is_realtime(const rt_thread_info_t *t);

/* Threads present in `cur` but not in `prev` (matched by tid) AND realtime.
 * Returns how many were written to `out`, capped at `out_max`.
 *
 * Deliberately NOT a name comparison — see the header comment. A tid is only
 * reused after wraparound, which is far longer than the interval between two
 * scans, so tid identity is sound here. */
int rt_thread_new_realtime(const rt_thread_info_t *prev, int prev_n,
                           const rt_thread_info_t *cur, int cur_n,
                           rt_thread_info_t *out, int out_max);

/* How many of these threads are realtime. */
int rt_thread_count_realtime(const rt_thread_info_t *t, int n);

/* Format one finding as a single log line. Writes at most `buf_len` bytes
 * including the NUL and always terminates. `module` may be NULL. */
int rt_thread_format(const rt_thread_info_t *t, const char *module,
                     char *buf, int buf_len);

/* Read /proc/self/task into `out`. Returns the number of threads written, or
 * -1 if /proc could not be read. Fills at most `out_max`.
 *
 * FILE I/O — worker thread only, NEVER the SPI callback. */
int rt_thread_audit_scan(rt_thread_info_t *out, int out_max);

#endif /* RT_THREAD_AUDIT_H */
