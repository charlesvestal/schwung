/*
 * schwung_trace.h — realtime-safe span tracing with OTLP export.
 *
 * Design + rationale: docs/tracing.md
 *
 * One-line instrumentation, no begin/end boilerplate:
 *
 *     void shim_post_transfer(...) {
 *         TRACE_SCOPE("spi.callback");           // root span for this frame
 *         ...
 *         { TRACE_SCOPE("spi.mix_audio"); mix_audio(); }   // child span
 *         ...
 *     }                                          // span auto-closes here
 *
 * For spans that don't match a lexical block:
 *
 *     trace_handle_t h = TRACE_BEGIN("param.serve");
 *     ...
 *     TRACE_END(h);
 *
 * COST WHEN DISABLED: tracing is OFF BY DEFAULT (no touch-file). Begin
 * is then a single atomic-load + branch; end is a no-op. Build with
 * -DSCHWUNG_TRACE_DISABLED to compile every macro to absolute zero.
 *
 * REALTIME SAFETY (emission): no alloc, no locks, no I/O. Begin/end only
 * read CLOCK_MONOTONIC (vDSO, no syscall) and append a fixed record to a
 * preallocated lock-free ring; full ring drops (never blocks). The sole
 * syscall is one cached gettid() per thread on its first traced span (only
 * when tracing is enabled); steady state is syscall-free. Export (serialize
 * + file/HTTP) runs on a separate SCHED_OTHER thread, never core 3.
 */
#ifndef SCHWUNG_TRACE_H
#define SCHWUNG_TRACE_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by begin, consumed by end. Opaque-ish; treat as a token. */
typedef struct {
    uint64_t span_id;   /* 0 when tracing was off at begin → end no-ops */
} trace_handle_t;

/* ---- Lifecycle (call from the owning process, e.g. the shim) -------- *
 *
 * init: allocate the ring + name table, record the MONOTONIC↔REALTIME
 *       offset, and (if the touch-file is present) start the exporter
 *       thread. Safe to call once at startup.
 * poll_enable: re-check the touch-file; flips tracing on/off and
 *       lazily starts/parks the exporter. Call from a slow timer.
 * shutdown: flush + stop the exporter (best-effort; optional).
 */
void schwung_trace_init(const char *service_name);
void schwung_trace_poll_enable(void);
void schwung_trace_shutdown(void);

/* Runtime gate. 0 = off (the default). Read by the macros. */
extern _Atomic int schwung_trace_on;

/* Intern a span-name string literal → stable id (>= 1). Called once per
 * call site via the macros; not on the hot path after the first hit. */
uint32_t schwung_trace_intern(const char *name);

/* Hot-path primitives. begin returns {0} immediately when off. */
trace_handle_t schwung_trace_begin(uint32_t name_id);
void           schwung_trace_end(trace_handle_t *h);

/* ---- Phase 2: cross-process / JS bridge (declared, not yet wired) --- *
 * A JS (QuickJS) binding will push a pre-timed span into the shared ring
 * so shadow_ui / ion spans correlate with the shim's. */
void schwung_trace_span_explicit(uint32_t name_id,
                                 uint64_t t0_ns, uint64_t t1_ns,
                                 uint64_t trace_id, uint64_t parent_id);

/* ===================================================================== */
/*  Macros — the only thing call sites should use.                       */
/* ===================================================================== */

#ifdef SCHWUNG_TRACE_DISABLED

#define TRACE_SCOPE(name_lit)   ((void)0)
#define TRACE_BEGIN(name_lit)   ((trace_handle_t){0})
#define TRACE_END(h)            ((void)(h))

#else  /* tracing compiled in, runtime-gated */

/* Cache the interned id per call site in a static; first hit interns. */
#define SCHWUNG_TRACE__NID(name_lit)                                       \
    ({ static uint32_t _schw_nid = 0;                                      \
       if (__builtin_expect(_schw_nid == 0, 0))                            \
           _schw_nid = schwung_trace_intern(name_lit);                     \
       _schw_nid; })

#define SCHWUNG_TRACE__CAT2(a, b) a##b
#define SCHWUNG_TRACE__CAT(a, b)  SCHWUNG_TRACE__CAT2(a, b)

static inline void schwung_trace__cleanup(trace_handle_t *h) {
    schwung_trace_end(h);
}

/* Scoped span: closes automatically at end of the enclosing block. */
#define TRACE_SCOPE(name_lit)                                              \
    trace_handle_t SCHWUNG_TRACE__CAT(_schw_span_, __LINE__)               \
        __attribute__((cleanup(schwung_trace__cleanup))) =                 \
        schwung_trace_begin(SCHWUNG_TRACE__NID(name_lit))

/* Manual begin/end for non-lexical spans. */
#define TRACE_BEGIN(name_lit) schwung_trace_begin(SCHWUNG_TRACE__NID(name_lit))
#define TRACE_END(h)          schwung_trace_end(&(h))

#endif /* SCHWUNG_TRACE_DISABLED */

#ifdef __cplusplus
}
#endif

#endif /* SCHWUNG_TRACE_H */
