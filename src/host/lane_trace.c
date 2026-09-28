/* lane_trace.c — the ring's one owner, and the arm the callback reads.
 *
 * Deliberately tiny and deliberately NOT in the header: the producer (SPI
 * callback, via shadow_lanes_publish_driving) and the consumer (the worker)
 * are in different translation units, and a `static` in a header would give
 * each of them its OWN ring -- the writer filling one and the reader draining
 * another, forever empty, with nothing to say so. That exact defect is on
 * record in this project ("`static` in a header is per-TU").
 */
#include "lane_trace.h"

static lane_trace_t g_lane_trace;
static volatile int g_lane_trace_armed;

lane_trace_t *lane_trace_ring(void) { return &g_lane_trace; }

int lane_trace_armed(void) {
    return __atomic_load_n(&g_lane_trace_armed, __ATOMIC_RELAXED);
}

void lane_trace_set_armed(int on) {
    __atomic_store_n(&g_lane_trace_armed, on ? 1 : 0, __ATOMIC_RELAXED);
}
