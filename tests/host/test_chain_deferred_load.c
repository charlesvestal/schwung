/*
 * Does a module get built OFF the SPI callback — and does a thread the module
 * spawns inherit normal priority as a result?
 *
 * That second question is the fleetwide bug. `create_instance` used to run on
 * the SPI callback (SCHED_FIFO 70 on core 3), and POSIX defaults to
 * PTHREAD_INHERIT_SCHED, so a plugin calling pthread_create from create_instance
 * got a worker at FIFO 70 — outranking Move's own Link Audio publisher, which
 * is FIFO 35. Measured on hardware 2026-08-22: five modules do exactly this.
 *
 * The fix is not visible in any one module. It is that create runs somewhere
 * else now. So the assertion has to be about WHERE the work happens, which is
 * what this file measures:
 *
 *   - stage runs on a thread that is NOT the caller's        (inheritance source)
 *   - that thread is BELOW Link Main's priority              (what is inherited)
 *   - a thread spawned FROM stage is below it too            (the actual harm)
 *   - the request returns long before the load finishes      (the 673 ms stall)
 *
 * "Below Link Main", not "SCHED_OTHER": demoting that far was measured on
 * hardware to break modules silently relying on inherited realtime to keep up.
 * See CHAIN_LOADER_RT_PRIORITY in chain_loader.h.
 *
 * `chain_synth_stage` / `chain_synth_commit` / `chain_synth_destroy_triple` are
 * stubbed here rather than linked from chain_host.c. That is deliberate: the
 * real ones drag in six translation units and a dlopen, and none of that
 * decides the question. What IS faithful is that the stub does the two things a
 * real plugin does that make this dangerous — it sleeps, and it spawns a
 * thread — so the properties above are measured against the same shape.
 *
 * The wiring (that `synth:module` reaches the loader at all) is pinned
 * separately by test_chain_deferred_load.sh, because a perfect loader that
 * nothing calls would pass every assertion in this file.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chain_loader.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
    else         { printf("ok: ");   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------ observations */

static pthread_t g_main_thread;

static pthread_t g_stage_thread;
static int       g_stage_policy      = -1;
static int       g_stage_prio        = -1;
static int       g_worker_policy     = -1;
static int       g_worker_prio       = -1;
static int       g_stage_calls       = 0;
static int       g_destroy_calls     = 0;
static int       g_commit_calls      = 0;
static char      g_last_committed[64];
static int       g_stage_sleep_ms    = 200;

static pthread_mutex_t g_obs = PTHREAD_MUTEX_INITIALIZER;

static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Priority of Move's Link Audio publisher. The loader — and therefore every
 * plugin thread born from create_instance — must stay BELOW it. */
#define LINK_MAIN_PRIORITY 35

static int self_priority(void) {
    int policy = -1;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) return -1;
    return sp.sched_priority;
}

static int self_policy(void) {
#ifdef __linux__
    return sched_getscheduler(0);
#else
    int policy = -1;
    struct sched_param sp;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) return -1;
    return policy;
#endif
}

/* Stands in for the seven-odd fleet plugins that pthread_create inside create.
 * Records what policy it was BORN with — which is inherited from whoever
 * called create_instance, and is the whole point. */
static void *plugin_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_obs);
    g_worker_policy = self_policy();
    g_worker_prio   = self_priority();
    pthread_mutex_unlock(&g_obs);
    return NULL;
}

/* ------------------------------------------------------- stubbed chain_host */

void chain_synth_stage(chain_instance_t *inst, const char *module_name,
                       chain_staged_synth_t *out) {
    (void)inst;
    pthread_mutex_lock(&g_obs);
    g_stage_calls++;
    g_stage_thread = pthread_self();
    g_stage_policy = self_policy();
    g_stage_prio   = self_priority();
    pthread_mutex_unlock(&g_obs);

    /* A real create_instance spawns its worker here. */
    pthread_t w;
    if (pthread_create(&w, NULL, plugin_worker, NULL) == 0) pthread_join(w, NULL);

    /* And it is SLOW — 672.9 ms for minijv on hardware. */
    nap(g_stage_sleep_ms);

    snprintf(out->module_name, sizeof(out->module_name), "%s", module_name);
    out->handle   = (void *)0x1;      /* non-NULL sentinels; nothing dereferences */
    out->api      = NULL;
    out->instance = (void *)0x2;
    out->ok       = 1;
}

void chain_synth_commit(chain_instance_t *inst, chain_staged_synth_t *staged,
                        chain_retired_module_t *retired_out) {
    (void)inst;
    pthread_mutex_lock(&g_obs);
    g_commit_calls++;
    snprintf(g_last_committed, sizeof(g_last_committed), "%s", staged->module_name);
    pthread_mutex_unlock(&g_obs);

    if (retired_out) {
        retired_out->handle   = NULL;
        retired_out->api      = NULL;
        retired_out->instance = NULL;
    }
    staged->handle   = NULL;
    staged->instance = NULL;
}

void chain_synth_destroy_triple(chain_retired_module_t *r) {
    if (!r) return;
    if (r->handle || r->instance) {
        pthread_mutex_lock(&g_obs);
        g_destroy_calls++;
        pthread_mutex_unlock(&g_obs);
    }
    r->handle = NULL; r->api = NULL; r->instance = NULL;
}

void chain_loader_note_retire_overflow(chain_instance_t *inst) { (void)inst; }

/* The demotion-verify path calls this when it cannot get below Link Main.
 * It lives inside `#ifdef __linux__` in chain_loader.c, so a macOS build
 * compiles the call away and links happily while Linux fails to link — which
 * is exactly how it shipped: the call arrived with the demotion verifier and
 * the stub did not, and only CI could see it. */
void chain_loader_note_demote_failed(const char *msg) { (void)msg; }

/* ------------------------------------------------------------------- tests */

static chain_instance_t *fresh_instance(void) {
    chain_instance_t *inst = (chain_instance_t *)calloc(1, sizeof(chain_instance_t));
    if (!inst) { printf("FAIL: out of memory\n"); exit(1); }
    if (!chain_alloc_position_storage(inst)) { printf("FAIL: storage\n"); exit(1); }
    return inst;
}

static void reset_obs(void) {
    pthread_mutex_lock(&g_obs);
    g_stage_calls = g_destroy_calls = g_commit_calls = 0;
    g_stage_policy = g_worker_policy = -1;
    g_stage_prio = g_worker_prio = -1;
    g_last_committed[0] = '\0';
    pthread_mutex_unlock(&g_obs);
}

/* Spin render_block's commit point until the load lands, or give up. */
static int pump_until_committed(chain_instance_t *inst, int timeout_ms) {
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (now_ms() < deadline) {
        chain_loader_commit(inst);
        pthread_mutex_lock(&g_obs);
        int done = g_commit_calls;
        pthread_mutex_unlock(&g_obs);
        if (done) return 1;
        nap(5);
    }
    return 0;
}

static void test_load_happens_off_the_caller_thread(void) {
    printf("\n-- a load runs off the calling thread, below Link Main --\n");
    chain_instance_t *inst = fresh_instance();
    reset_obs();

    uint64_t t0 = now_ms();
    int rc = chain_loader_request_synth(inst, "fixture-synth");
    uint64_t elapsed = now_ms() - t0;

    CHECK(rc == 0, "request accepted");

    /*
     * THE 673 ms FIX. The stub sleeps 200 ms; if the request blocked for it,
     * we are still loading on the audio thread and nothing has been fixed.
     */
    CHECK(elapsed < 50, "request returned in %llums, without waiting for the %dms load",
          (unsigned long long)elapsed, g_stage_sleep_ms);

    CHECK(chain_loader_synth_busy(inst) == 1, "is_loading reports 1 while loading");

    CHECK(pump_until_committed(inst, 5000), "the load committed");

    pthread_mutex_lock(&g_obs);
    int    calls  = g_stage_calls;
    int    spol   = g_stage_policy;
    int    sprio  = g_stage_prio;
    int    wpol   = g_worker_policy;
    int    wprio  = g_worker_prio;
    pthread_t st  = g_stage_thread;
    pthread_mutex_unlock(&g_obs);

    CHECK(calls == 1, "stage ran exactly once");

    /*
     * THE INHERITANCE FIX. A worker inherits the policy AND PRIORITY of
     * whoever created it, so what the loader runs at is what the whole fleet
     * runs at.
     *
     * The invariant is NOT "SCHED_OTHER". Demoting that far was measured on
     * hardware 2026-08-27 to break modules that were silently relying on
     * inherited realtime to keep up — osirus's forked DSP child underran. What
     * must hold is that nothing lands at or above Move's Link Audio publisher,
     * because that is what starves the device.
     *
     * Both answers are legitimate here: on the device the loader asks for
     * SCHED_FIFO 20, and on a dev machine or in CI the request is usually
     * refused for lack of privilege and it falls back to SCHED_OTHER.
     */
    CHECK(!pthread_equal(st, g_main_thread),
          "create_instance ran on a DIFFERENT thread from the caller");
    CHECK(spol == SCHED_OTHER || sprio < LINK_MAIN_PRIORITY,
          "the loader is below Link Main (policy %d prio %d)", spol, sprio);
    CHECK(wpol == SCHED_OTHER || wprio < LINK_MAIN_PRIORITY,
          "a thread spawned FROM create_instance is below Link Main "
          "(policy %d prio %d)", wpol, wprio);

    CHECK(chain_loader_synth_busy(inst) == 0, "is_loading reports 0 once committed");

    /* A second commit must not republish. */
    int before = g_commit_calls;
    chain_loader_commit(inst);
    CHECK(g_commit_calls == before, "commit is idempotent — publishes once only");

    chain_loader_shutdown(inst);
    chain_free_position_storage(inst);
    free(inst);
}

static void test_supersede_keeps_only_the_last(void) {
    printf("\n-- spinning the picker loads the last pick, and drops the rest --\n");
    chain_instance_t *inst = fresh_instance();
    reset_obs();

    /*
     * Three picks, as a jog through a module list — SPACED so that each one
     * lands while its predecessor is mid-load. Firing them back to back would
     * be a weaker test: all three would arrive before the loader's first poll,
     * only the last would ever be staged, and the supersede-a-running-load path
     * this exists to cover would never execute.
     */
    CHECK(chain_loader_request_synth(inst, "alpha") == 0, "request alpha");
    nap(60);   /* alpha is now inside its 200ms stage */
    CHECK(chain_loader_request_synth(inst, "beta")  == 0, "request beta while alpha loads");
    nap(60);
    CHECK(chain_loader_request_synth(inst, "gamma") == 0, "request gamma while beta is queued");

    CHECK(pump_until_committed(inst, 8000), "a load committed");

    /* Let any superseded work finish being reaped. */
    for (int i = 0; i < 200; i++) { chain_loader_commit(inst); nap(5); }

    pthread_mutex_lock(&g_obs);
    int commits  = g_commit_calls;
    int staged   = g_stage_calls;
    int destroys = g_destroy_calls;
    char last[64]; snprintf(last, sizeof(last), "%s", g_last_committed);
    pthread_mutex_unlock(&g_obs);

    CHECK(commits == 1, "exactly one module was published, not three (got %d)", commits);
    CHECK(strcmp(last, "gamma") == 0, "the module published is the LAST one picked (got '%s')", last);

    /* The point of the spacing: at least one load really was superseded, and
     * what it built was destroyed rather than leaked or published. */
    CHECK(staged >= 2, "more than one load was actually started (got %d)", staged);
    CHECK(destroys >= staged - 1,
          "every superseded load was destroyed (%d staged, %d destroyed)", staged, destroys);

    chain_loader_shutdown(inst);
    chain_free_position_storage(inst);
    free(inst);
}

/*
 * The window that had TWO bugs, and neither was reachable from the tests
 * above: a load completes and is staged, and a NEW request arrives before any
 * render ran, so commit is the thing that discovers it was superseded.
 *
 *  - the first version nulled the loader's reusable parameter block here, and
 *    the loader's next pass memset() through that NULL. A segfault, on the
 *    audio path's peer thread, only on a fast second pick.
 *  - it also treated the request as resolved, so the module the user actually
 *    chose was never loaded and is_loading lied about it. Silent: no error,
 *    no log, just a position that stays on the old module.
 *
 * Reproduced by staging one load to completion WITHOUT calling commit (which
 * is what a render not happening for 300 ms looks like), then picking again.
 */
static void test_supersede_discovered_at_commit(void) {
    printf("\n-- a second pick lands after the first finished but before it committed --\n");
    chain_instance_t *inst = fresh_instance();
    reset_obs();

    CHECK(chain_loader_request_synth(inst, "first") == 0, "request first");

    /* Deliberately no commit: let "first" finish staging and sit unclaimed. */
    nap(400);
    pthread_mutex_lock(&g_obs);
    int staged_before = g_stage_calls;
    pthread_mutex_unlock(&g_obs);
    CHECK(staged_before == 1, "first finished staging while nothing rendered");
    CHECK(chain_loader_synth_busy(inst) == 1,
          "is_loading is still 1 — staged is not the same as committed");

    CHECK(chain_loader_request_synth(inst, "second") == 0, "request second");

    /* Now start rendering again. The first commit sees a stale staged record. */
    CHECK(pump_until_committed(inst, 8000), "a load committed");
    for (int i = 0; i < 200; i++) { chain_loader_commit(inst); nap(5); }

    pthread_mutex_lock(&g_obs);
    int  commits = g_commit_calls;
    char last[64]; snprintf(last, sizeof(last), "%s", g_last_committed);
    pthread_mutex_unlock(&g_obs);

    /* The request must NOT have been swallowed by the stale-record branch. */
    CHECK(strcmp(last, "second") == 0,
          "the module the user actually picked was loaded (got '%s')", last);
    CHECK(commits == 1, "the stale staged record was not published (commits=%d)", commits);
    CHECK(chain_loader_synth_busy(inst) == 0, "is_loading returns to 0");

    chain_loader_shutdown(inst);
    chain_free_position_storage(inst);
    free(inst);
}

static void test_shutdown_with_a_load_in_flight(void) {
    printf("\n-- destroying an instance mid-load joins cleanly --\n");
    chain_instance_t *inst = fresh_instance();
    reset_obs();
    g_stage_sleep_ms = 400;

    CHECK(chain_loader_request_synth(inst, "slow-one") == 0, "request accepted");
    nap(30);   /* let the loader pick it up and get into the long part */

    /*
     * No commit ever happens. Shutdown has to join the loader and destroy what
     * it staged — if it detached instead, the free below would race a thread
     * still writing into it. Under ASan this is where a use-after-free shows.
     */
    chain_loader_shutdown(inst);

    pthread_mutex_lock(&g_obs);
    int destroys = g_destroy_calls;
    int commits  = g_commit_calls;
    pthread_mutex_unlock(&g_obs);

    CHECK(commits == 0, "nothing was published");
    CHECK(destroys >= 1, "the staged-but-uncommitted module was destroyed, not leaked");

    chain_free_position_storage(inst);
    free(inst);
    g_stage_sleep_ms = 200;
}

static void test_no_thread_until_a_load_is_asked_for(void) {
    printf("\n-- a chain that never swaps a module never spawns a thread --\n");
    chain_instance_t *inst = fresh_instance();

    /* Cheap, and it is the reason this is lazy: at most five of these exist,
     * and a chain that never loads anything should not hold one. */
    CHECK(chain_loader_synth_busy(inst) == 0, "not busy before any request");
    chain_loader_commit(inst);   /* must be a no-op, not a crash */
    CHECK(1, "commit with no loader is a no-op");

    chain_loader_shutdown(inst);  /* must tolerate never having started */
    chain_free_position_storage(inst);
    free(inst);
}

int main(void) {
    g_main_thread = pthread_self();
    printf("caller policy: %d (SCHED_OTHER=%d)\n", self_policy(), SCHED_OTHER);

    test_no_thread_until_a_load_is_asked_for();
    test_load_happens_off_the_caller_thread();
    test_supersede_keeps_only_the_last();
    test_supersede_discovered_at_commit();
    test_shutdown_with_a_load_in_flight();

    printf("\n%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
