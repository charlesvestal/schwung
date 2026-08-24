/* Unit test: detect threads that inherited the SPI callback's priority.
 *
 * The failure being detected is invisible by construction. A thread created
 * from a module entry point inherits SCHED_FIFO 90 AND the parent's `comm`, so
 * it reports as "Audio Main/SPI" and looks exactly like the real SPI thread in
 * `top` or any thread listing. A name-based check would therefore pass while
 * the bug is present, which is worse than no check at all.
 *
 * So the detector is a SET DIFF over tids, and these tests are written to fail
 * if anyone ever reduces it to a name comparison.
 */
#include <stdio.h>
#include <string.h>

#include "rt_thread_audit.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

/* Build a /proc stat line: fields 1-2 then filler up to 38, then
 * processor / rt_priority / policy at 39 / 40 / 41. */
static void make_stat_cpu(char *buf, int len, int tid, const char *comm,
                          int cpu, int rtprio, int policy,
                          unsigned long utime, unsigned long stime);

static void make_stat(char *buf, int len, int tid, const char *comm,
                      int cpu, int rtprio, int policy)
{
    make_stat_cpu(buf, len, tid, comm, cpu, rtprio, policy, 0, 0);
}

/* Same, with utime/stime (proc(5) fields 14 and 15) placed exactly. */
static void make_stat_cpu(char *buf, int len, int tid, const char *comm,
                          int cpu, int rtprio, int policy,
                          unsigned long utime, unsigned long stime)
{
    int n = snprintf(buf, (size_t)len, "%d (%s) S 1 1 1 0 -1 4194560", tid, comm);
    /* fields so far: 1 pid, 2 comm, 3 state, 4-9 = six more. Next is 10. */
    for (int f = 10; f <= 13; f++)
        n += snprintf(buf + n, (size_t)(len - n), " %d", f);
    n += snprintf(buf + n, (size_t)(len - n), " %lu %lu", utime, stime);
    for (int f = 16; f <= 38; f++)
        n += snprintf(buf + n, (size_t)(len - n), " %d", f);
    snprintf(buf + n, (size_t)(len - n), " %d %d %d 0 0 0", cpu, rtprio, policy);
}

static void test_parses_the_scheduling_fields(void)
{
    char line[1024];
    rt_thread_info_t t;

    make_stat(line, sizeof(line), 22339, "Audio Main/SPI", 3, 90, RT_AUDIT_SCHED_FIFO);
    CHECK(rt_thread_parse_stat(line, &t), "a well-formed stat line parses");
    CHECK(t.tid == 22339, "tid");
    CHECK(t.policy == RT_AUDIT_SCHED_FIFO, "policy is field 41");
    CHECK(t.rtprio == 90, "rt_priority is field 40");
    CHECK(t.cpu == 3, "processor is field 39");
    CHECK(strcmp(t.comm, "Audio Main/SPI") == 0, "comm survives its embedded space");
    CHECK(rt_thread_is_realtime(&t), "FIFO 90 is realtime");
}

/* comm is bracketed and unescaped, so a name with a ')' in it will split a
 * naive tokeniser and shift every field after it — silently reporting
 * SCHED_OTHER for a thread that is actually FIFO. */
static void test_comm_with_parens_does_not_shift_the_fields(void)
{
    char line[1024];
    rt_thread_info_t t;

    make_stat(line, sizeof(line), 41, "worker (2)", 1, 70, RT_AUDIT_SCHED_FIFO);
    CHECK(rt_thread_parse_stat(line, &t), "a comm containing parens still parses");
    CHECK(strcmp(t.comm, "worker (2)") == 0, "comm delimited by the LAST ')'");
    CHECK(t.rtprio == 70 && t.policy == RT_AUDIT_SCHED_FIFO,
          "fields after a parenthesised comm are not shifted");
}

/* A truncated line must not read as a clean SCHED_OTHER — a false all-clear is
 * the one wrong answer here. */
static void test_truncated_line_is_rejected(void)
{
    rt_thread_info_t t;
    CHECK(!rt_thread_parse_stat("1234 (short) S 1 1 1", &t),
          "a line that stops before field 41 must be REJECTED, not read as OTHER");
    CHECK(!rt_thread_parse_stat("", &t), "an empty line is rejected");
    CHECK(!rt_thread_parse_stat("nonsense", &t), "a line with no brackets is rejected");
}

static void test_only_nonzero_fifo_rr_counts_as_realtime(void)
{
    rt_thread_info_t t = { 0 };
    t.policy = RT_AUDIT_SCHED_OTHER; t.rtprio = 0;
    CHECK(!rt_thread_is_realtime(&t), "SCHED_OTHER is not realtime");
    t.policy = RT_AUDIT_SCHED_FIFO; t.rtprio = 0;
    CHECK(!rt_thread_is_realtime(&t), "FIFO at priority 0 is not realtime");
    t.policy = RT_AUDIT_SCHED_RR; t.rtprio = 35;
    CHECK(rt_thread_is_realtime(&t), "RR 35 is realtime");
    t.policy = RT_AUDIT_SCHED_FIFO; t.rtprio = 90;
    CHECK(rt_thread_is_realtime(&t), "FIFO 90 is realtime");
}

/* THE CASE. A module spawns a worker from create_instance; it is born FIFO 90
 * and wearing its parent's name. Two threads now claim to be "Audio Main/SPI"
 * and nothing about either one distinguishes it. Only the diff sees it. */
static void test_an_inherited_thread_wearing_the_spi_name_is_caught(void)
{
    rt_thread_info_t before[2], after[3], found[8];

    before[0] = (rt_thread_info_t){ .tid = 900, .policy = RT_AUDIT_SCHED_FIFO,
                                    .rtprio = 90, .cpu = 3, .comm = "Audio Main/SPI" };
    before[1] = (rt_thread_info_t){ .tid = 901, .policy = RT_AUDIT_SCHED_OTHER,
                                    .rtprio = 0, .cpu = 0, .comm = "shim-worker" };

    after[0] = before[0];
    after[1] = before[1];
    after[2] = (rt_thread_info_t){ .tid = 22339, .policy = RT_AUDIT_SCHED_FIFO,
                                   .rtprio = 90, .cpu = 3, .comm = "Audio Main/SPI" };

    int n = rt_thread_new_realtime(before, 2, after, 3, found, 8);
    CHECK(n == 1, "exactly one new realtime thread");
    CHECK(found[0].tid == 22339, "and it is the inherited one, found by tid not name");
    CHECK(strcmp(found[0].comm, "Audio Main/SPI") == 0,
          "which is indistinguishable from the SPI thread by name — the point of the diff");

    /* The message has to say so, or the next reader dismisses it. */
    char buf[256];
    rt_thread_format(&found[0], "tablor", buf, sizeof(buf));
    CHECK(strstr(buf, "tablor") != NULL, "the finding names the module that was loading");
    CHECK(strstr(buf, "shared name") != NULL,
          "and flags the name as shared rather than concluding anything from it");
    CHECK(strstr(buf, "NOT the SPI thread") == NULL,
          "it must NOT conclude a thread is inherited from its name — measurement "
          "showed Move runs six of its own threads under exactly this name");
}

static void test_a_properly_spawned_worker_is_not_reported(void)
{
    rt_thread_info_t before[1], after[2], found[8];

    before[0] = (rt_thread_info_t){ .tid = 900, .policy = RT_AUDIT_SCHED_FIFO,
                                    .rtprio = 90, .cpu = 3, .comm = "Audio Main/SPI" };
    after[0] = before[0];
    /* What tablor now does: SCHED_OTHER on the attributes, and named. */
    after[1] = (rt_thread_info_t){ .tid = 22400, .policy = RT_AUDIT_SCHED_OTHER,
                                   .rtprio = 0, .cpu = 1, .comm = "tablor-wtload" };

    CHECK(rt_thread_new_realtime(before, 1, after, 2, found, 8) == 0,
          "a worker spawned SCHED_OTHER is not a finding");
}

/* A thread that exits and one that starts must not cancel out. */
static void test_churn_does_not_mask_a_new_rt_thread(void)
{
    rt_thread_info_t before[2], after[2], found[8];

    before[0] = (rt_thread_info_t){ .tid = 900, .policy = RT_AUDIT_SCHED_FIFO,
                                    .rtprio = 90, .cpu = 3, .comm = "Audio Main/SPI" };
    before[1] = (rt_thread_info_t){ .tid = 905, .policy = RT_AUDIT_SCHED_FIFO,
                                    .rtprio = 70, .cpu = 2, .comm = "old-worker" };
    after[0] = before[0];
    after[1] = (rt_thread_info_t){ .tid = 906, .policy = RT_AUDIT_SCHED_FIFO,
                                   .rtprio = 70, .cpu = 2, .comm = "old-worker" };

    int n = rt_thread_new_realtime(before, 2, after, 2, found, 8);
    CHECK(n == 1 && found[0].tid == 906,
          "same count and same name, different tid — still a finding");
}

static void test_counting_and_bounds(void)
{
    rt_thread_info_t t[3] = {
        { .tid = 1, .policy = RT_AUDIT_SCHED_FIFO,  .rtprio = 90 },
        { .tid = 2, .policy = RT_AUDIT_SCHED_OTHER, .rtprio = 0  },
        { .tid = 3, .policy = RT_AUDIT_SCHED_RR,    .rtprio = 35 },
    };
    CHECK(rt_thread_count_realtime(t, 3) == 2, "two of three are realtime");

    rt_thread_info_t one[1];
    rt_thread_info_t empty[1] = {{ .tid = 0 }};
    CHECK(rt_thread_new_realtime(empty, 0, t, 3, one, 1) == 1,
          "out_max is honoured rather than overrun");
    CHECK(rt_thread_new_realtime(NULL, 0, t, 3, one, 1) == 1,
          "a NULL previous snapshot means everything realtime is new");
}

static void test_format_never_overruns(void)
{
    rt_thread_info_t t = { .tid = 22339, .policy = RT_AUDIT_SCHED_FIFO,
                           .rtprio = 90, .cpu = 3, .comm = "Audio Main/SPI" };
    char tiny[8];
    memset(tiny, 'X', sizeof(tiny));
    int n = rt_thread_format(&t, "some-very-long-module-id", tiny, sizeof(tiny));
    CHECK(n < (int)sizeof(tiny), "returns a length inside the buffer");
    CHECK(tiny[sizeof(tiny) - 1] == '\0', "always NUL-terminates");
}


/* ---- CPU burn: the number that actually maps onto the dropouts ---------- */

static rt_thread_info_t mk(int tid, int prio, unsigned long ut, unsigned long st)
{
    rt_thread_info_t t = { 0 };
    t.tid = tid; t.policy = RT_AUDIT_SCHED_FIFO; t.rtprio = prio;
    t.utime = ut; t.stime = st;
    snprintf(t.comm, sizeof(t.comm), "Audio Main/SPI");
    return t;
}

static void test_utime_stime_are_parsed_from_fields_14_15(void)
{
    char line[1024];
    rt_thread_info_t t;
    make_stat_cpu(line, sizeof(line), 22339, "Audio Main/SPI", 3, 70,
                  RT_AUDIT_SCHED_FIFO, 1234, 56);
    CHECK(rt_thread_parse_stat(line, &t), "a line with CPU fields parses");
    CHECK(t.utime == 1234, "utime is field 14");
    CHECK(t.stime == 56, "stime is field 15");
    CHECK(t.rtprio == 70 && t.cpu == 3,
          "adding the CPU fields did not shift the scheduling fields");
}

/* A parked thread is NOT the finding. This is the whole point of measuring
 * burn instead of counting threads: po32-drum's render worker and fork's I/O
 * thread sit on a condvar at FIFO 70 and starve nobody. */
static void test_a_parked_realtime_thread_is_not_a_burner(void)
{
    rt_thread_info_t base[1] = { mk(900, 90, 0, 0) };
    rt_thread_info_t prev[2] = { mk(900, 90, 100, 0), mk(1000, 70, 5, 0) };
    rt_thread_info_t cur[2]  = { mk(900, 90, 200, 0), mk(1000, 70, 5, 0) };
    rt_thread_burn_t out[8];

    CHECK(rt_thread_burners(base, 1, prev, 2, cur, 2, 100, 20, out, 8) == 0,
          "a module thread that consumed no CPU is not reported");
}

static void test_a_busy_module_thread_is_reported_with_its_ms(void)
{
    rt_thread_info_t base[1] = { mk(900, 90, 0, 0) };
    rt_thread_info_t prev[2] = { mk(900, 90, 100, 0), mk(1000, 70, 0, 0) };
    /* 18 ticks user + 4 system at 100 Hz = 220 ms. */
    rt_thread_info_t cur[2]  = { mk(900, 90, 200, 0), mk(1000, 70, 18, 4) };
    rt_thread_burn_t out[8];

    int n = rt_thread_burners(base, 1, prev, 2, cur, 2, 100, 20, out, 8);
    CHECK(n == 1, "the busy module thread is reported");
    CHECK(out[0].thread.tid == 1000, "and it is the right one");
    CHECK(out[0].cpu_ms == 220, "220 ms from 22 ticks at 100 Hz");

    char buf[256];
    rt_thread_format_burn(&out[0], "sfz", 1000, buf, sizeof(buf));
    CHECK(strstr(buf, "BURNED 220 ms") != NULL, "the message states the cost");
    CHECK(strstr(buf, "22%") != NULL, "and it as a share of the window");
    CHECK(strstr(buf, "sfz") != NULL, "and names the module");
}

/* Move's own audio threads are permanently busy at FIFO 70. If they were not
 * excluded they would drown every real finding on every tick. */
static void test_moves_own_threads_are_excluded_by_the_baseline(void)
{
    rt_thread_info_t base[2] = { mk(900, 90, 0, 0), mk(976, 70, 0, 0) };
    rt_thread_info_t prev[2] = { mk(900, 90, 100, 0), mk(976, 70, 500, 0) };
    rt_thread_info_t cur[2]  = { mk(900, 90, 300, 0), mk(976, 70, 900, 0) };
    rt_thread_burn_t out[8];

    CHECK(rt_thread_burners(base, 2, prev, 2, cur, 2, 100, 20, out, 8) == 0,
          "threads present before any module loaded are the environment");
}

/* A thread that appears AND does its damage inside one sample window must not
 * be missed for having no previous reading. */
static void test_a_brand_new_thread_counts_its_whole_lifetime(void)
{
    rt_thread_info_t base[1] = { mk(900, 90, 0, 0) };
    rt_thread_info_t prev[1] = { mk(900, 90, 100, 0) };
    rt_thread_info_t cur[2]  = { mk(900, 90, 200, 0), mk(1100, 70, 21, 0) };
    rt_thread_burn_t out[8];

    int n = rt_thread_burners(base, 1, prev, 1, cur, 2, 100, 20, out, 8);
    CHECK(n == 1 && out[0].cpu_ms == 210,
          "a thread absent from prev is measured from zero, not skipped");
}

static void test_burners_are_ordered_worst_first_and_bounded(void)
{
    rt_thread_info_t base[1] = { mk(900, 90, 0, 0) };
    rt_thread_info_t prev[4] = { mk(900,90,0,0), mk(1,70,0,0), mk(2,70,0,0), mk(3,70,0,0) };
    rt_thread_info_t cur[4]  = { mk(900,90,0,0), mk(1,70,5,0), mk(2,70,50,0), mk(3,70,20,0) };
    rt_thread_burn_t out[8];

    int n = rt_thread_burners(base, 1, prev, 4, cur, 4, 100, 20, out, 8);
    CHECK(n == 3, "three burners over the floor");
    CHECK(out[0].thread.tid == 2 && out[1].thread.tid == 3 && out[2].thread.tid == 1,
          "ordered worst first");

    rt_thread_burn_t one[1];
    CHECK(rt_thread_burners(base, 1, prev, 4, cur, 4, 100, 20, one, 1) >= 1,
          "out_max is honoured");
    CHECK(one[0].thread.tid == 2, "and the ONE kept is the worst, not the first seen");
}

/* Counters only go up. A tid reused by a new thread reads as a huge negative
 * delta; guessing at it would invent a finding. */
static void test_tid_reuse_is_not_guessed_at(void)
{
    rt_thread_info_t base[1] = { mk(900, 90, 0, 0) };
    rt_thread_info_t prev[2] = { mk(900,90,0,0), mk(1000, 70, 5000, 0) };
    rt_thread_info_t cur[2]  = { mk(900,90,0,0), mk(1000, 70, 3, 0) };
    rt_thread_burn_t out[8];

    CHECK(rt_thread_burners(base, 1, prev, 2, cur, 2, 100, 20, out, 8) == 0,
          "a counter that went backwards is dropped, not reported");
}

int main(void)
{
    test_parses_the_scheduling_fields();
    test_comm_with_parens_does_not_shift_the_fields();
    test_truncated_line_is_rejected();
    test_only_nonzero_fifo_rr_counts_as_realtime();
    test_an_inherited_thread_wearing_the_spi_name_is_caught();
    test_a_properly_spawned_worker_is_not_reported();
    test_churn_does_not_mask_a_new_rt_thread();
    test_counting_and_bounds();
    test_format_never_overruns();
    test_utime_stime_are_parsed_from_fields_14_15();
    test_a_parked_realtime_thread_is_not_a_burner();
    test_a_busy_module_thread_is_reported_with_its_ms();
    test_moves_own_threads_are_excluded_by_the_baseline();
    test_a_brand_new_thread_counts_its_whole_lifetime();
    test_burners_are_ordered_worst_first_and_bounded();
    test_tid_reuse_is_not_guessed_at();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: rt thread audit — an inherited FIFO thread is caught by tid, not by name\n");
    return 0;
}
