/* undo_timeline: one Undo for Move's edits and Schwung's, decided from Move's
 * own undo stack (undo_timeline.h). */
#include <stdio.h>
#include <string.h>
#include "undo_timeline.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static char log_[16][48];
static int nlog;
static void cmd(void *ctx, int slot, const char *key, const char *val)
{
    (void)ctx;
    if (nlog < 16) snprintf(log_[nlog++], sizeof log_[0], "%d %s %s", slot, key, val);
}

/* Move's stack as a list of step ids, with a cursor: undo = [0, cur), redo = [cur, n). */
static uint64_t stk[32];
static int nstk, cur;
static uint64_t next_node = 0x1000;
static ut_hist_t hist(void)
{
    ut_hist_t h = { 1, {0, 0}, {0, 0} };
    if (cur > 0) { h.undo.node = stk[cur - 1]; h.undo.nbr = stk[cur - 1] * 7; }
    if (cur < nstk) { h.redo.node = stk[cur]; h.redo.nbr = stk[cur] * 7; }
    return h;
}
static void move_edit(ut_t *u, uint64_t t) { nstk = cur; stk[nstk++] = next_node++; cur = nstk; ut_hist_t h = hist(); ut_on_history(u, &h, t, cmd, NULL); }
static void move_undo(ut_t *u, uint64_t t) { if (cur) cur--; ut_hist_t h = hist(); ut_on_history(u, &h, t, cmd, NULL); }
static void move_redo(ut_t *u, uint64_t t) { if (cur < nstk) cur++; ut_hist_t h = hist(); ut_on_history(u, &h, t, cmd, NULL); }
static void squash(ut_t *u, uint64_t t) { stk[cur - 1] = next_node++; ut_hist_t h = hist(); ut_on_history(u, &h, t, cmd, NULL); }

/* The whole press path: ours if the timeline says so, else Move's. */
static int press_undo(ut_t *u, uint64_t t)
{
    int s; uint32_t j;
    if (ut_undo_target(u, &s, &j)) return ut_take_undo(u, s, j, cmd, NULL) ? 1 : -1;
    move_undo(u, t);
    return 0;
}
static int press_redo(ut_t *u, uint64_t t)
{
    int s; uint32_t j;
    if (ut_redo_target(u, &s, &j)) return ut_take_redo(u, s, j, cmd, NULL) ? 1 : -1;
    move_redo(u, t);
    return 0;
}

int main(void)
{
    static ut_t u;
    int s; uint32_t j;

    /* ---- interleaved: note edit N, p-lock E, note edit M ---------------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    { ut_hist_t h = hist(); ut_on_history(&u, &h, 0, cmd, NULL); }
    CHECK(!ut_undo_target(&u, &s, &j), "nothing of ours yet");
    move_edit(&u, 10);                                       /* N */
    ut_on_schwung_edit(&u, 2, 0x80000001u, UT_PLOCK, 20);   /* E */
    CHECK(ut_undo_target(&u, &s, &j) && s == 2 && j == 0x80000001u, "the p-lock is on top");
    move_edit(&u, 30);                                       /* M */
    CHECK(!ut_undo_target(&u, &s, &j), "Move's note edit came after: Undo is Move's");

    CHECK(press_undo(&u, 40) == 0 && cur == 1, "undo 1 -> Move undoes M");
    CHECK(press_undo(&u, 50) == 1 && nlog == 1 && strcmp(log_[0], "2 lanes:journal undo 2147483649") == 0,
          "undo 2 -> ours: %s", nlog ? log_[0] : "(none)");
    CHECK(press_undo(&u, 60) == 0 && cur == 0, "undo 3 -> Move undoes N");
    CHECK(press_undo(&u, 65) == 0 && cur == 0, "undo 4 -> nothing left, Move's (a no-op)");

    /* Redo walks back up in the same order: N, E, M. */
    nlog = 0;
    CHECK(press_redo(&u, 70) == 0 && cur == 1, "redo 1 -> Move redoes N");
    CHECK(press_redo(&u, 80) == 1 && nlog == 1 && strstr(log_[0], "redo 2147483649"), "redo 2 -> ours");
    CHECK(press_redo(&u, 90) == 0 && cur == 2, "redo 3 -> Move redoes M");

    /* ---- two Schwung edits in a row undo newest first ------------------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_schwung_edit(&u, 0, 0x80000001u, UT_PLOCK, 10);
    ut_on_schwung_edit(&u, 1, 0x80000001u, UT_CLEAR, 20);
    CHECK(press_undo(&u, 30) == 1 && strstr(log_[0], "1 lanes:journal undo"), "newest (slot 1) first");
    CHECK(press_undo(&u, 40) == 1 && strstr(log_[1], "0 lanes:journal undo"), "then slot 0");
    CHECK(press_redo(&u, 50) == 1 && strstr(log_[2], "0 lanes:journal redo"), "redo: last undone first");
    CHECK(press_redo(&u, 60) == 1 && strstr(log_[3], "1 lanes:journal redo"), "then the other");

    /* ---- a new edit kills the redo branch -------------------------------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_schwung_edit(&u, 0, 0x80000001u, UT_PLOCK, 10);
    press_undo(&u, 20);
    move_edit(&u, 30);                                       /* a Move edit after the undo */
    CHECK(!ut_redo_target(&u, &s, &j), "Move's new edit ended our redo");
    ut_on_schwung_edit(&u, 0, 0x80000002u, UT_PLOCK, 40);
    press_undo(&u, 50);
    ut_on_schwung_edit(&u, 0, 0x80000003u, UT_PLOCK, 60);
    CHECK(ut_undo_target(&u, &s, &j) && j == 0x80000003u, "newest live");
    CHECK(!ut_redo_target(&u, &s, &j), "and our own new edit ended our redo too");

    /* ---- a take recorded while Move recorded notes is ONE step ---------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);                                        /* something earlier */
    ut_on_arm(&u, 1, 100);
    move_edit(&u, 150);                                      /* Move's first recorded note */
    squash(&u, 300);                                         /* ...squashed as the pass goes on */
    ut_on_arm(&u, 0, 400);
    ut_on_schwung_edit(&u, 3, 0x80000001u, UT_TAKE, 410);
    CHECK(!ut_undo_target(&u, &s, &j), "linked: the press is Move's");
    CHECK(press_undo(&u, 500) == 0 && nlog == 1 && strstr(log_[0], "3 lanes:journal undo"),
          "Move undoes its notes and the take goes with them: %s", nlog ? log_[0] : "(none)");
    CHECK(press_redo(&u, 600) == 0 && nlog == 2 && strstr(log_[1], "3 lanes:journal redo"),
          "and comes back with them");

    /* Move pushes its recording just AFTER the take ended: still one step. */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_arm(&u, 1, 100);
    ut_on_arm(&u, 0, 400);
    ut_on_schwung_edit(&u, 3, 0x80000001u, UT_TAKE, 410);
    move_edit(&u, 450);
    CHECK(!ut_undo_target(&u, &s, &j), "a step inside the link window claims the take");
    CHECK(press_undo(&u, 500) == 0 && nlog == 1, "one Undo, both");

    /* An automation-only take: nothing of Move's in the window, ours alone. */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_arm(&u, 1, 100);
    ut_on_arm(&u, 0, 400);
    ut_on_schwung_edit(&u, 3, 0x80000001u, UT_TAKE, 410);
    move_edit(&u, 410 + UT_LINK_MS + 100);                   /* long after */
    CHECK(press_undo(&u, 2000) == 0, "the later edit is Move's");
    CHECK(press_undo(&u, 2100) == 1, "then the take, alone");

    /* ---- the chain's journal depth bounds what can be claimed ----------- */
    ut_reset(&u); nstk = cur = 0;
    move_edit(&u, 0);
    for (uint32_t k = 1; k <= UT_SLOT_DEPTH + 2; k++) ut_on_schwung_edit(&u, 0, 0x80000000u + k, UT_PLOCK, k);
    int claimed = 0;
    while (press_undo(&u, 100) == 1) claimed++;
    CHECK(claimed == UT_SLOT_DEPTH, "only the chain's %d are claimed, not %d", UT_SLOT_DEPTH, claimed);

    /* ---- a take split by a mirrored edit: two entries on one Move step,
     *      undone NEWEST first and redone oldest first ---------------------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_arm(&u, 1, 100);
    move_edit(&u, 150);
    ut_on_schwung_edit(&u, 3, 0x80000001u, UT_TAKE, 200);   /* part a (committed at the mirrored verb) */
    ut_on_schwung_edit(&u, 3, 0x80000002u, UT_TAKE, 400);   /* part b */
    ut_on_arm(&u, 0, 400);
    press_undo(&u, 500);
    CHECK(nlog == 2 && strstr(log_[0], "undo 2147483650") && strstr(log_[1], "undo 2147483649"),
          "newest part undone first: %s / %s", log_[0], nlog > 1 ? log_[1] : "");
    nlog = 0;
    press_redo(&u, 600);
    CHECK(nlog == 2 && strstr(log_[0], "redo 2147483649") && strstr(log_[1], "redo 2147483650"),
          "oldest part redone first");

    /* ---- nothing is claimed while a take is open ------------------------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_schwung_edit(&u, 0, 0x80000001u, UT_PLOCK, 10);
    ut_on_arm(&u, 1, 20);
    CHECK(!ut_undo_target(&u, &s, &j), "armed: Undo is Move's");
    ut_on_arm(&u, 0, 30);
    CHECK(ut_undo_target(&u, &s, &j), "disarmed: ours again");

    /* ---- a Schwung edit leaves a linked take's redo alone ---------------- */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_arm(&u, 1, 100);
    move_edit(&u, 150);
    ut_on_arm(&u, 0, 400);
    ut_on_schwung_edit(&u, 3, 0x80000001u, UT_TAKE, 410);
    press_undo(&u, 500);                                     /* Move + take undone */
    ut_on_schwung_edit(&u, 0, 0x80000001u, UT_PLOCK, 600);   /* an unrelated p-lock */
    nlog = 0;
    press_redo(&u, 700);                                     /* the p-lock is live: Redo is Move's */
    CHECK(nlog == 1 && strstr(log_[0], "3 lanes:journal redo"), "Move's redo brings the take back too: %s",
          nlog ? log_[0] : "(none)");

    /* ---- a restore voids the slot's journal ------------------------------ */
    ut_reset(&u); nstk = cur = 0; nlog = 0;
    move_edit(&u, 0);
    ut_on_schwung_edit(&u, 2, 0x80000001u, UT_PLOCK, 10);
    ut_on_schwung_edit(&u, 2, 0, UT_RESET, 20);
    CHECK(!ut_undo_target(&u, &s, &j), "restored: the old p-lock is not undoable");

    /* ---- no history, no claims ------------------------------------------ */
    ut_reset(&u);
    ut_on_schwung_edit(&u, 0, 0x80000001u, UT_PLOCK, 0);
    CHECK(!ut_undo_target(&u, &s, &j), "unknown history: the press is always Move's");

    if (fails) { printf("test_undo_timeline: %d FAILED\n", fails); return 1; }
    printf("test_undo_timeline: PASS\n");
    return 0;
}
