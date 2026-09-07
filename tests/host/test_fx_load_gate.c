/*
 * fx_load_gate.h — the three-state answer, and the two orderings behind it.
 *
 * The rules under test are the ones a Master FX / send FX position's editor
 * depends on now that the dlopen happens on the shim worker:
 *
 *   - LOADING, LOADED and FAILED are three distinguishable answers. A load
 *     that failed must not read as one still running: the entry gate never
 *     gives up on a hold, so the two being the same is a screen that says
 *     "Loading..." for the rest of the session.
 *   - A failure is forgotten the moment a NEWER request is made, or a retry
 *     reports the error it is in the middle of clearing.
 *   - A zeroed position is SETTLED. Everything here starts at 0 (the arrays
 *     are BSS), so a position nobody has touched must not read as loading.
 *   - The worker may not read the request payload until req_pub has caught up
 *     with req_seq — the path is written between the close and the publish,
 *     and reading it in that window is a torn dlopen argument.
 *   - A realisation whose seq is no longer the one being asked for is refused
 *     at install. That is the whole reason this is a sequence number and not a
 *     flag; see the write-up in fx_load_gate.h and in chain_bus.c.
 */
#include <stdio.h>
#include "fx_load_gate.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    /* ---- a position nobody has touched ---------------------------------- */
    CHECK(fx_load_state(0, 0, 0) == FX_LOAD_SETTLED,
          "a zeroed position must read SETTLED, not loading");

    /* ---- the ordinary life of one request -------------------------------- */
    unsigned req = fx_load_next_seq(0);              /* the close */
    CHECK(req == 1u, "the first seq must be 1, not 0");
    CHECK(fx_load_state(req, 0, 0) == FX_LOAD_LOADING,
          "closed but not installed must read LOADING");
    CHECK(fx_load_state(req, req, 0) == FX_LOAD_SETTLED,
          "installed and successful must read SETTLED");
    CHECK(fx_load_state(req, req, 1) == FX_LOAD_FAILED,
          "installed and failed must read FAILED");

    /* ---- three answers, not two ----------------------------------------- */
    CHECK(fx_load_state(req, 0, 1) == FX_LOAD_LOADING,
          "a stale failure must not outrank a newer request in flight");

    /* ---- a retry clears the report --------------------------------------- */
    unsigned again = fx_load_next_seq(req);
    CHECK(fx_load_state(again, req, 1) == FX_LOAD_LOADING,
          "asking again must read LOADING even while the last failure is recorded");

    /* ---- 0 is skipped on wrap, in both places ---------------------------- */
    CHECK(fx_load_next_seq(0xFFFFFFFFu) == 1u,
          "wrapping to 0 would make a live request read as 'never asked'");

    /* ---- the payload is not readable before the publish ------------------ */
    CHECK(!fx_load_request_readable(/*req*/2u, /*pub*/1u, /*done*/1u),
          "the worker must not read req_path while req_pub lags req_seq");
    CHECK(fx_load_request_readable(2u, 2u, 1u),
          "a published request must be readable");
    CHECK(!fx_load_request_readable(2u, 2u, 2u),
          "an already-installed request must not be picked up again");
    CHECK(!fx_load_request_readable(0u, 0u, 0u),
          "a position that has never been asked has nothing to read");

    /* ---- a stale realisation is refused at install ----------------------- */
    CHECK(fx_load_stage_current(3u, 3u), "the current realisation must install");
    CHECK(!fx_load_stage_current(3u, 4u),
          "a realisation the request has moved past must be refused, not installed");
    CHECK(!fx_load_stage_current(0u, 0u),
          "0 is not a realisation");

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: fx_load_gate three-state answer and both orderings\n");
    return 0;
}
