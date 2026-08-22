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
static void make_stat(char *buf, int len, int tid, const char *comm,
                      int cpu, int rtprio, int policy)
{
    int n = snprintf(buf, (size_t)len, "%d (%s) S 1 1 1 0 -1 4194560", tid, comm);
    /* fields so far: 1 pid, 2 comm, 3 state, 4-9 = six more. Next is 10. */
    for (int f = 10; f <= 38; f++)
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

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: rt thread audit — an inherited FIFO thread is caught by tid, not by name\n");
    return 0;
}
