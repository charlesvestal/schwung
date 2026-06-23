/*
 * schwung_trace.c — realtime-safe span tracing, OTLP/JSON file exporter.
 * See schwung_trace.h and docs/tracing.md.
 *
 * Emission (hot path, incl. SPI core 3): per-thread SPSC ring, no alloc /
 * lock / I/O. Export: one SCHED_OTHER thread drains every thread's ring,
 * builds OTLP/JSON, appends a JSONL line to a rotating file. OFF by
 * default; enabled by the touch-file (see schwung_trace_poll_enable).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE        /* pthread affinity + CPU_SET (RT: keep core 3 free) */
#endif
#include "schwung_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "unified_log.h"   /* exporter thread is non-RT → logging is safe here */

#if defined(__linux__)
#include <sys/syscall.h>
static inline uint32_t os_tid(void) { return (uint32_t)syscall(SYS_gettid); }
#else
static inline uint32_t os_tid(void) { return (uint32_t)(uintptr_t)pthread_self(); }
#endif

/* ---- Tunables ------------------------------------------------------- */
#define TRACE_MAX_THREADS  8
#define TRACE_RING_CAP     4096            /* per thread; power of two */
#define TRACE_RING_MASK    (TRACE_RING_CAP - 1)
#define TRACE_MAX_DEPTH    32              /* per-thread span nesting */
#define TRACE_MAX_NAMES    256
#define TRACE_NAME_LIT     1               /* names are static literals */

#define TRACE_TOUCH_FILE   "/data/UserData/schwung/otlp_trace_on"
#define TRACE_DIR          "/data/UserData/schwung/traces"
/* output file: <TRACE_DIR>/<service>-YYYYMMDD-HHMMSS.otlp.jsonl (see open_outfile) */
#define TRACE_ROTATE_BYTES (8 * 1024 * 1024)
#define TRACE_MAX_FILES    16              /* keep at most this many trace files */
#define TRACE_EXPORT_SLEEP_US 50000        /* 50 ms drain cadence */

/* ---- Span record (internal; .h keeps trace_handle_t opaque) --------- */
typedef struct {
    uint32_t name_id;
    uint32_t tid;
    uint64_t t0_ns, t1_ns;
    uint64_t trace_id;
    uint64_t span_id;
    uint64_t parent_id;
} trace_rec_t;

/* ---- Per-thread SPSC ring ------------------------------------------- */
typedef struct {
    _Atomic uint64_t w;                    /* producer-only writes (RELEASE) */
    _Atomic uint64_t r;                    /* exporter-only writes; producer reads */
    _Atomic uint64_t dropped;
    trace_rec_t      buf[TRACE_RING_CAP];
} trace_ring_t;

static trace_ring_t   g_rings[TRACE_MAX_THREADS];
static _Atomic int    g_ring_count = 0;
static __thread trace_ring_t *t_ring = NULL;   /* this thread's claimed ring */

/* ---- Per-thread span stack (parent linkage + trace id) -------------- */
static __thread trace_rec_t t_stack[TRACE_MAX_DEPTH];
static __thread int          t_depth   = 0;
static __thread uint64_t      t_traceid = 0;
static __thread uint32_t      t_tid     = 0;

/* ---- Global state --------------------------------------------------- */
_Atomic int schwung_trace_on = 0;

static _Atomic(const char *) g_names[TRACE_MAX_NAMES];
static _Atomic uint32_t g_name_count = 0;

static _Atomic uint64_t g_span_seq  = 0;
static _Atomic uint64_t g_trace_seq = 0;

/* Per-PROCESS random base for span/trace ids. Span and trace ids must be unique
 * across processes (shim + shadow_ui) so a correlated trace doesn't get two
 * spans with the same id, and two unrelated traces don't share a trace_id and
 * get merged. The id is base + counter; with a 64-bit random base per process,
 * cross-process collision is negligible. Seeded in init from pid + clocks. */
static uint64_t g_span_base  = 0;
static uint64_t g_trace_base = 0;

static char        g_service[64] = "schwung";
static int         g_inited = 0;        /* set in constructor before any RT thread */
static pthread_t   g_exporter;
static _Atomic int g_exporter_running = 0;
static _Atomic int g_exporter_stop = 0;

/* MONOTONIC ↔ REALTIME offset, sampled at init (ns). */
static uint64_t g_mono0_ns = 0;
static uint64_t g_real0_ns = 0;

/* ---- Clock ---------------------------------------------------------- *
 * CLOCK_MONOTONIC (not _RAW): same vDSO read, no syscall, but it shares
 * REALTIME's NTP slewing so the fixed MONOTONIC↔REALTIME offset sampled at
 * init stays accurate over long sessions (RAW is unslewed and would drift
 * the exported UnixNano). It also matches the clock the rest of the shim
 * uses for its [spi_timing] boundaries. */
static inline uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t real_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t schwung_trace_now_ns(void) { return mono_ns(); }

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* Salted, globally-unique span/trace ids (see g_span_base). Never 0. */
static inline uint64_t next_span_id(void) {
    return g_span_base + atomic_fetch_add_explicit(&g_span_seq, 1, memory_order_relaxed) + 1;
}
static inline uint64_t next_trace_id(void) {
    return g_trace_base + atomic_fetch_add_explicit(&g_trace_seq, 1, memory_order_relaxed) + 1;
}

void schwung_trace_current(uint64_t *trace_id, uint64_t *span_id) {
    if (t_depth > 0) {
        if (trace_id) *trace_id = t_stack[t_depth - 1].trace_id;
        if (span_id)  *span_id  = t_stack[t_depth - 1].span_id;
    } else {
        if (trace_id) *trace_id = 0;
        if (span_id)  *span_id  = 0;
    }
}

/* ---- Name interning: store the literal pointer, lock-free ----------- */
uint32_t schwung_trace_intern(const char *name) {
    uint32_t idx = atomic_fetch_add_explicit(&g_name_count, 1, memory_order_relaxed);
    if (idx >= TRACE_MAX_NAMES) return 1;          /* table full → bucket 1 */
    /* Release-store the literal so the exporter, which acquire-loads the slot,
     * never sees the id published with a stale/NULL pointer (ARM64 reorders). */
    atomic_store_explicit(&g_names[idx], name, memory_order_release);
    return idx + 1;                                /* ids are 1-based */
}

/* Copy-interning for non-static names (e.g. from JS). Dedups by string so a
 * repeated JS span name reuses its id. Not RT-safe (strdup). */
uint32_t schwung_trace_intern_copy(const char *name) {
    if (!name || !*name) return 1;
    uint32_t nc = atomic_load_explicit(&g_name_count, memory_order_acquire);
    if (nc > TRACE_MAX_NAMES) nc = TRACE_MAX_NAMES;
    for (uint32_t i = 0; i < nc; i++) {
        const char *p = atomic_load_explicit(&g_names[i], memory_order_acquire);
        if (p && strcmp(p, name) == 0) return i + 1;   /* already interned */
    }
    uint32_t idx = atomic_fetch_add_explicit(&g_name_count, 1, memory_order_relaxed);
    if (idx >= TRACE_MAX_NAMES) return 1;
    char *dup = strndup(name, 255);                    /* cap: a rogue JS name can't OOM;
                                                        * process-lifetime, intentional */
    atomic_store_explicit(&g_names[idx], dup ? dup : "?", memory_order_release);
    return idx + 1;
}

/* ---- Ring push (hot path; SPSC, this thread only) ------------------- */
static inline void ring_push(const trace_rec_t *rec) {
    trace_ring_t *ring = t_ring;
    if (!ring) {
        int i = atomic_fetch_add_explicit(&g_ring_count, 1, memory_order_relaxed);
        if (i >= TRACE_MAX_THREADS) return;        /* too many threads → drop */
        ring = t_ring = &g_rings[i];
    }
    uint64_t w = atomic_load_explicit(&ring->w, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ring->r, memory_order_relaxed);  /* stale ok, not torn */
    if (w - r >= TRACE_RING_CAP) {                 /* full → drop, count it */
        atomic_fetch_add_explicit(&ring->dropped, 1, memory_order_relaxed);
        return;
    }
    ring->buf[w & TRACE_RING_MASK] = *rec;
    atomic_store_explicit(&ring->w, w + 1, memory_order_release);
}

/* ---- begin / end ---------------------------------------------------- */
trace_handle_t schwung_trace_begin(uint32_t name_id) {
    if (!atomic_load_explicit(&schwung_trace_on, memory_order_relaxed))
        return (trace_handle_t){0};
    if (t_depth >= TRACE_MAX_DEPTH || t_depth < 0)
        return (trace_handle_t){0};                /* nesting overflow → skip */
    uint64_t sid = next_span_id();
    if (sid == 0)                                  /* salted base wrapped id to 0 →
                                                    * would alias the off sentinel */
        return (trace_handle_t){0};
    if (t_depth == 0)
        t_traceid = next_trace_id();
    trace_rec_t *r = &t_stack[t_depth];
    r->name_id   = name_id;
    r->span_id   = sid;
    r->parent_id = (t_depth > 0) ? t_stack[t_depth - 1].span_id : 0;
    r->trace_id  = t_traceid;
    r->t0_ns     = mono_ns();
    t_depth++;
    return (trace_handle_t){ sid };
}

void schwung_trace_end(trace_handle_t *h) {
    if (!h || h->span_id == 0) return;             /* was off at begin */
    if (t_depth <= 0) return;
    /* Normal LIFO fast path: the span being ended is the stack top — one
     * comparison, no scan. */
    int idx = t_depth - 1;
    if (__builtin_expect(t_stack[idx].span_id != h->span_id, 0)) {
        /* Out-of-order / forgotten end (e.g. a mis-paired JS-bridge span):
         * find the matching span deeper in the stack and unwind to it, so
         * subsequent spans re-parent correctly instead of inheriting a stale
         * top. Inner spans left open by the mismatch are dropped (they were
         * never properly ended). Bounded by TRACE_MAX_DEPTH; no syscall/alloc.
         * Unknown handle (already ended / never pushed) → no-op. */
        idx--;
        while (idx >= 0 && t_stack[idx].span_id != h->span_id) idx--;
        if (idx < 0) return;
    }
    trace_rec_t *r = &t_stack[idx];
    r->t1_ns = mono_ns();
    if (t_tid == 0) t_tid = os_tid();
    r->tid   = t_tid;
    t_depth  = idx;                                /* unwind to / pop the match */
    ring_push(r);
}

void schwung_trace_span_explicit(uint32_t name_id,
                                 uint64_t t0_ns, uint64_t t1_ns,
                                 uint64_t trace_id, uint64_t parent_id) {
    if (!atomic_load_explicit(&schwung_trace_on, memory_order_relaxed)) return;
    if (t_tid == 0) t_tid = os_tid();
    trace_rec_t rec = {
        .name_id = name_id, .tid = t_tid,
        .t0_ns = t0_ns, .t1_ns = t1_ns,
        .trace_id  = trace_id ? trace_id : next_trace_id(),
        .span_id   = next_span_id(),
        .parent_id = parent_id,
    };
    ring_push(&rec);
}

/* ---- Exporter: drain rings → OTLP/JSON → rotating JSONL file -------- */
static void hex16(uint64_t v, char *out) {           /* 16 hex chars */
    static const char *H = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { out[i] = H[v & 0xF]; v >>= 4; }
}
static uint64_t to_unix_ns(uint64_t mono) {
    /* real0 + (mono - mono0); guard mono < mono0 (shouldn't happen).
     * mono0/real0 are sampled together at init; CLOCK_MONOTONIC tracks
     * REALTIME's slew so this fixed offset doesn't drift. */
    return (mono >= g_mono0_ns) ? g_real0_ns + (mono - g_mono0_ns)
                                : g_real0_ns;
}

static FILE  *g_out = NULL;
static long   g_out_bytes = 0;

/* Keep at most TRACE_MAX_FILES trace files: unlink the oldest before opening a
 * new one. Filenames are <service>-YYYYMMDD-HHMMSS.otlp.jsonl, so lexicographic
 * order == chronological order. Bounds disk use even if the touch-file is left
 * on for days (/data is ~49 GB but not infinite). */
static void prune_old_files(void) {
    DIR *d = opendir(TRACE_DIR);
    if (!d) return;
    /* Only our own service's files — each exporter (shim, shadow_ui) prunes its
     * own, so two exporters in the same dir never race on each other's unlinks. */
    char prefix[80];
    int plen = snprintf(prefix, sizeof(prefix), "%s-", g_service);
    if (plen < 0) { closedir(d); return; }          /* encoding error */
    if (plen >= (int)sizeof(prefix))                /* truncated: clamp so the */
        plen = (int)sizeof(prefix) - 1;             /* strncmp below stays in-bounds */
    char names[64][64];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < 64) {
        if (strncmp(de->d_name, prefix, (size_t)plen) == 0 &&
            strstr(de->d_name, ".otlp.jsonl") != NULL) {
            snprintf(names[count], sizeof(names[count]), "%s", de->d_name);
            count++;
        }
    }
    closedir(d);
    /* insertion sort ascending (oldest first) */
    for (int i = 1; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "%s", names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            snprintf(names[j + 1], sizeof(names[j + 1]), "%s", names[j]);
            j--;
        }
        snprintf(names[j + 1], sizeof(names[j + 1]), "%s", key);
    }
    int to_delete = count - (TRACE_MAX_FILES - 1);   /* leave room for the new file */
    for (int i = 0; i < to_delete && i < count; i++) {
        char p[320];
        snprintf(p, sizeof(p), "%s/%s", TRACE_DIR, names[i]);
        unlink(p);
    }
}

static void open_outfile(void) {
    if (mkdir(TRACE_DIR, 0755) != 0 && errno != EEXIST)
        unified_log("trace", LOG_LEVEL_ERROR, "mkdir %s failed: errno %d", TRACE_DIR, errno);
    prune_old_files();
    char path[320], stamp[32];
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(path, sizeof(path), "%s/%s-%s.otlp.jsonl", TRACE_DIR, g_service, stamp);
    g_out = fopen(path, "w");
    if (!g_out)
        unified_log("trace", LOG_LEVEL_ERROR, "fopen %s failed: errno %d", path, errno);
    g_out_bytes = 0;
}

/* Minimal JSON string escaper → out (capacity cap, always NUL-terminated).
 * Span names come from JS modules via host_trace_begin, so an unescaped name
 * containing '"', '\\' or a control char would otherwise corrupt or forge
 * the OTLP/JSONL output. Truncates safely if the escaped form won't fit. */
static const char *json_escape(const char *in, char *out, int cap) {
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < cap - 7; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\')  { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n')         { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r')         { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t')         { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)          { o += snprintf(out + o, cap - o, "\\u%04x", c); }
        else                        { out[o++] = (char)c; }
    }
    out[o] = '\0';
    return out;
}

/* Build + write one ExportTraceServiceRequest (JSONL) for `spans`. */
static void write_batch(const trace_rec_t *spans, int n) {
    if (!g_out || n <= 0) return;
    char sid[17] = {0}, pid[17] = {0};
    char svc_esc[160];
    fprintf(g_out,
        "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
        "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}}]},"
        /* InstrumentationScope = the producing library, same for both services
         * (the service is distinguished by service.name above). */
        "\"scopeSpans\":[{\"scope\":{\"name\":\"schwung-trace\"},\"spans\":[",
        json_escape(g_service, svc_esc, sizeof(svc_esc)));
    for (int i = 0; i < n; i++) {
        const trace_rec_t *s = &spans[i];
        uint32_t nc = atomic_load_explicit(&g_name_count, memory_order_acquire);
        const char *nm_p = (s->name_id >= 1 && s->name_id <= nc)
            ? atomic_load_explicit(&g_names[s->name_id - 1], memory_order_acquire) : NULL;
        const char *nm = nm_p ? nm_p : "?";
        char nm_esc[1600];                  /* 255-char name × ~6 worst-case escape */
        nm = json_escape(nm, nm_esc, sizeof(nm_esc));
        /* 128-bit OTLP traceId: high 64 bits zero, low 64 = our trace_id. */
        char traceid32[33];
        memset(traceid32, '0', 16);
        hex16(s->trace_id, traceid32 + 16);
        traceid32[32] = 0;
        hex16(s->span_id, sid);
        hex16(s->parent_id, pid);
        fprintf(g_out,
            "%s{\"traceId\":\"%s\",\"spanId\":\"%s\",%s%s%s"
            "\"name\":\"%s\",\"kind\":1,"
            "\"startTimeUnixNano\":\"%llu\",\"endTimeUnixNano\":\"%llu\","
            "\"attributes\":[{\"key\":\"tid\",\"value\":{\"intValue\":\"%u\"}}]}",
            (i ? "," : ""), traceid32, sid,
            (s->parent_id ? "\"parentSpanId\":\"" : ""),
            (s->parent_id ? pid : ""),
            (s->parent_id ? "\"," : ""),
            nm,
            (unsigned long long)to_unix_ns(s->t0_ns),
            (unsigned long long)to_unix_ns(s->t1_ns),
            s->tid);
    }
    fputs("]}]}]}\n", g_out);
    fflush(g_out);
    long pos = ftell(g_out);              /* true file size incl. JSON envelope */
    if (pos >= 0) g_out_bytes = pos;
    if (g_out_bytes >= TRACE_ROTATE_BYTES) {
        fclose(g_out); g_out = NULL; open_outfile();
    }
}

static void *exporter_main(void *arg) {
    (void)arg;
    open_outfile();
    int overflow_logged = 0;
    uint64_t last_dropped = 0;          /* cumulative dropped spans last surfaced */
    int      dropped_pass = 0;          /* cadence counter for the dropped-count log */
    /* small staging buffer drained from the rings each pass */
    static trace_rec_t batch[1024];
    while (!atomic_load_explicit(&g_exporter_stop, memory_order_relaxed)) {
        int n = 0;
        int rc = atomic_load_explicit(&g_ring_count, memory_order_relaxed);
        /* More producer threads than rings → the extras drop silently on the
         * RT path (can't log there). Surface it once from here (non-RT). */
        if (rc > TRACE_MAX_THREADS && !overflow_logged) {
            unified_log("trace", LOG_LEVEL_WARN,
                "thread-ring table exhausted (%d producers > %d rings); "
                "spans from extra threads dropped — raise TRACE_MAX_THREADS",
                rc, TRACE_MAX_THREADS);
            overflow_logged = 1;
        }
        if (rc > TRACE_MAX_THREADS) rc = TRACE_MAX_THREADS;
        for (int i = 0; i < rc; i++) {
            trace_ring_t *ring = &g_rings[i];
            uint64_t w = atomic_load_explicit(&ring->w, memory_order_acquire);
            uint64_t r = atomic_load_explicit(&ring->r, memory_order_relaxed);
            if (w - r > TRACE_RING_CAP) {            /* producer lapped us */
                uint64_t skipped = (w - TRACE_RING_CAP) - r;
                atomic_fetch_add_explicit(&ring->dropped, skipped, memory_order_relaxed);
                r = w - TRACE_RING_CAP;             /* skip the overwritten span(s) */
            }
            while (r < w && n < (int)(sizeof(batch)/sizeof(batch[0]))) {
                batch[n++] = ring->buf[r & TRACE_RING_MASK];
                r++;
            }
            atomic_store_explicit(&ring->r, r, memory_order_relaxed);
        }
        if (n > 0) write_batch(batch, n);
        /* Surface ring overruns. Each ring's `dropped` is bumped on the RT path
         * (producer lapped the exporter) and in the drain above, but is otherwise
         * invisible when analyzing a trace. Sum it across rings and log from here
         * (non-RT) at most once per ~5 s, and only when it grows — so overruns are
         * diagnosable without spamming. Mirrors the thread-table-exhausted warning. */
        if (++dropped_pass >= 100) {            /* 100 * 50 ms ≈ 5 s */
            dropped_pass = 0;
            uint64_t total = 0;
            for (int i = 0; i < rc; i++)
                total += atomic_load_explicit(&g_rings[i].dropped, memory_order_relaxed);
            if (total != last_dropped) {
                unified_log("trace", LOG_LEVEL_WARN,
                    "%llu span(s) dropped (ring overrun, cap %d) — "
                    "raise TRACE_RING_CAP or lower span rate",
                    (unsigned long long)total, TRACE_RING_CAP);
                last_dropped = total;
            }
        }
        /* Always sleep — cap exporter I/O to one burst per cadence even when
         * the ring keeps producing (otherwise this spins on cores 0-2). */
        usleep(TRACE_EXPORT_SLEEP_US);
    }
    if (g_out) { fclose(g_out); g_out = NULL; }
    return NULL;
}

/* ---- Lifecycle ------------------------------------------------------ */
void schwung_trace_init(const char *service_name) {
    if (g_inited) return;
    if (service_name && *service_name) {
        snprintf(g_service, sizeof(g_service), "%s", service_name);
    }
    g_mono0_ns = mono_ns();
    g_real0_ns = real_ns();
    /* Per-process id bases (see g_span_base). Mix pid + both clocks so the shim
     * and shadow_ui — started at different times — get distinct id spaces. */
    uint64_t seed = g_real0_ns ^ (g_mono0_ns << 1) ^ ((uint64_t)getpid() << 32);
    g_span_base  = splitmix64(seed) | 1ull;                 /* non-zero */
    g_trace_base = splitmix64(seed ^ 0xD1B54A32D192ED03ull) | 1ull;
    g_inited = 1;
    /* Flush + join the exporter on normal process exit. No-op when tracing
     * was never enabled (exporter not running). Best-effort: skipped on
     * SIGKILL, but then /data is reclaimed by the OS anyway. */
    atexit(schwung_trace_shutdown);
    schwung_trace_poll_enable();
}

static int touch_file_present(void) {
    struct stat st;
    return stat(TRACE_TOUCH_FILE, &st) == 0;
}

/* Spawn the exporter with an EXPLICIT non-RT schedule. The shim runs inside
 * MoveOriginal's SCHED_FIFO-70 threads, and the poll that triggers this may
 * itself be on such a thread; the default PTHREAD_INHERIT_SCHED would hand the
 * exporter FIFO priority and possibly core 3 (the SPI callback's core). Force
 * SCHED_OTHER and pin to cores 0-2 so it never preempts the RT path. */
static int start_exporter(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    struct sched_param sp = { .sched_priority = 0 };
    pthread_attr_setschedparam(&attr, &sp);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
    pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);  /* best-effort */
    int rc = pthread_create(&g_exporter, &attr, exporter_main, NULL);
    pthread_attr_destroy(&attr);
    return rc;
}

void schwung_trace_poll_enable(void) {
    if (!g_inited) return;
    int want = touch_file_present();
    int have = atomic_load_explicit(&schwung_trace_on, memory_order_relaxed);
    if (want && !have) {
        if (!g_exporter_running) {
            atomic_store_explicit(&g_exporter_stop, 0, memory_order_relaxed);
            if (start_exporter() == 0)
                g_exporter_running = 1;
        }
        atomic_store_explicit(&schwung_trace_on, 1, memory_order_release);
    } else if (!want && have) {
        atomic_store_explicit(&schwung_trace_on, 0, memory_order_release);
        /* leave the exporter running to flush residual; it idles. */
    }
}

void schwung_trace_shutdown(void) {
    atomic_store_explicit(&schwung_trace_on, 0, memory_order_release);
    if (g_exporter_running) {
        atomic_store_explicit(&g_exporter_stop, 1, memory_order_release);
        pthread_join(g_exporter, NULL);
        g_exporter_running = 0;
    }
}
