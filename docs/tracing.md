# Schwung tracing

Realtime-safe span tracing for the Schwung shim and the `shadow_ui` process,
exported as OTLP/JSON for Grafana Tempo / Jaeger. It answers "where does the
time go" perf questions — a slow JS tick, an over-long SPI frame — with
parent/child spans and cross-process correlation, instead of ad-hoc
`Date.now()` / `clock_gettime` instrumentation.

**Off by default, zero hot-path cost when off**, and enabled live on a shipped
device via a touch-file (no rebuild, no restart). Source:
`src/host/schwung_trace.{c,h}`.

> **Scope — part 1 of 2.** This PR adds the **shim** tracing core (the ring,
> exporter, OTLP/JSON writer, macros, and the shim's own spans). The
> `shadow_ui` / JavaScript-bridge / cross-process pieces described below
> (marked **⏳ Part 2**) are implemented and validated on a fork but are **not
> in this PR** — they follow as a second PR if this one is approved, to keep
> the review focused. In this PR the shim writes a single
> `schwung-<ts>.otlp.jsonl` and `param.serve` is a plain child of `spi.pre`.

## Using it (on device)

```bash
# enable
ssh ableton@move.local "touch /data/UserData/schwung/otlp_trace_on"
# ... reproduce the scenario you want to profile ...
# disable
ssh ableton@move.local "rm /data/UserData/schwung/otlp_trace_on"
# pull the traces
scp 'ableton@move.local:/data/UserData/schwung/traces/*.otlp.jsonl' .
```

The touch-file gates every process (polled within ~5 s, so toggling is live).
Output lands in `/data/UserData/schwung/traces/`, one file per service:

- `schwung-shim-<ts>.otlp.jsonl` — the LD_PRELOAD shim (SPI callback path)
- `schwung-shadow-ui-<ts>.otlp.jsonl` — the QuickJS UI process

Each line is one OTLP `ExportTraceServiceRequest` (JSONL) — replay it to a
collector or import directly into Tempo/Jaeger.

## What gets traced

**Shim** (`schwung-shim`), per SPI frame:

- `spi.pre` / `spi.post` — roots (one trace each per frame); the two halves of
  the SPI callback around the hardware ioctl.
- `shadow.mix_audio`, `midi.process` — children of `spi.pre`.
- `param.serve` — the shim servicing a `shadow_param` request (a child of
  `spi.pre`). **⏳ Part 2** re-parents it cross-process to the JS `param.get`
  span (see [Cross-process correlation](#cross-process-correlation)).

**shadow_ui** (`schwung-shadow-ui`), per UI tick — **⏳ Part 2** (not in this PR):

- `js.tick` — root around the JS `tick()` call.
- `param.get` — child around the synchronous `shadow_get_param` round-trip (the
  JS side busy-waits for the shim to service it once per SPI frame).

JS modules (overtake/chain, e.g. ion) add their own spans under `js.tick` — see
[Adding spans](#adding-spans).

## How it works

### Emission (hot path, RT-safe)

`TRACE_SCOPE("name")` (or manual `TRACE_BEGIN`/`TRACE_END`) opens a span. When
tracing is off it is a single atomic-load + branch; when on it stamps
`CLOCK_MONOTONIC` and pushes a fixed 48-byte record into a lock-free ring.
The emission path is realtime-safe — usable from the SPI callback (SCHED_FIFO
90, core 3, ~900 µs budget):

- no allocation (ring, name table, and per-thread span stack are preallocated/fixed);
- no locks (atomics only);
- no syscalls in steady state except the vDSO clock read; the one exception
  is a single `gettid()` per thread on its first traced span (cached
  thereafter), and only while tracing is enabled;
- O(1) per begin; end is O(1) for the normal LIFO case (one comparison),
  O(depth) only when unwinding a mismatched/forgotten end; drop-on-full, never blocks.

**Ring** — one SPSC ring per producer thread (`g_rings[]`, claimed lazily on
first push and cached in a thread-local). The producer owns `w` (release on
publish), the exporter owns `r`; single-producer-per-ring keeps it tear-free
via `w` release/acquire with no locks. On overrun the oldest spans are dropped
and counted.

**Name interning** — span names become an int once per call site; the hot path
stores only the id. Static C literals are stored by pointer
(`schwung_trace_intern`); JS-supplied names are copied + deduped
(`schwung_trace_intern_copy`, capped at 255 chars).

**Parent linkage** — a per-thread span stack. A `begin` at depth 0 mints a new
`trace_id` (a root); nested `begin`s inherit the trace and parent to the stack
top. So `spi.pre` is the root of one trace per frame and its phases are
children — a clean per-frame flamegraph. `end` matches the handle's
`span_id` against the stack top (the normal LIFO case) and, if they differ,
unwinds down to the matching span — so a forgotten or out-of-order `end()`
(e.g. from the JS bridge) re-parents subsequent spans correctly instead of
corrupting the stack. Spans left open by such a mismatch are dropped.

### Export (off the hot path)

A dedicated exporter thread (SCHED_OTHER, pinned to cores 0–2, never core 3)
drains the rings, builds OTLP/JSON, and appends a JSONL line per batch to the
current trace file. It starts lazily when the touch-file appears. Files rotate
at 8 MB and are pruned to the newest 16 per service (≤128 MB/service), so a
left-on session cannot fill the disk.

### Cross-process correlation — ⏳ Part 2 (not in this PR)

The shim and `shadow_ui` are separate processes with separate
rings/exporters/files — yet their spans stitch into **one** trace
(`js.tick → param.get → param.serve`). Two mechanisms:

1. **Globally-unique ids.** Span/trace ids are `per-process-random-base +
   counter` (a `splitmix64` of pid + clocks, seeded at init). Without the
   per-process base the two processes' counters would collide — two spans
   sharing an id in one trace, or two unrelated traces sharing a `trace_id` and
   getting merged.
2. **Context propagation through IPC.** `shadow_ui` stamps the open `param.get`
   span's `trace_id`/`span_id` into the `shadow_param_t` request
   (`trace_id` / `parent_span_id`, released before `request_type`); the shim
   emits `param.serve` as that span's child via `schwung_trace_span_explicit`.

The per-process UnixNano stamps line up to <1 ms because `CLOCK_MONOTONIC`
is system-wide and each process samples the same MONOTONIC↔REALTIME offset at
init. (`CLOCK_MONOTONIC` rather than `_RAW`: it shares REALTIME's NTP slew, so
the fixed offset doesn't drift the exported UnixNano over a long session.)
Tempo/Jaeger merge the two files by `trace_id`.

## Adding spans

**C** (anywhere, including the RT path):

```c
#include "host/schwung_trace.h"

void f(void) {
    TRACE_SCOPE("my.phase");                 // closes at end of the C block
    ...
    { TRACE_SCOPE("my.subphase"); work(); }  // nested child
}
```

`TRACE_BEGIN("name")` / `TRACE_END(h)` cover spans that don't match a lexical
block. Build with `-DSCHWUNG_TRACE_DISABLED` to compile every macro to zero.

**JavaScript** (in a `shadow_ui` module — overtake/chain) — **⏳ Part 2** (not in this PR):

```js
const h = host_trace_begin("ion.computeFrame");  // 0 when tracing is off
...
host_trace_end(h);
```

JS spans nest under the C `js.tick` root for that tick. Use a fixed set of
names — each distinct name takes a table slot (256 per process).

## OTLP/JSON output

One `ExportTraceServiceRequest` per drained batch, newline-delimited:

```json
{"resourceSpans":[{"resource":{"attributes":[
  {"key":"service.name","value":{"stringValue":"schwung-shim"}}]},
  "scopeSpans":[{"scope":{"name":"shim"},"spans":[
    {"traceId":"<32hex>","spanId":"<16hex>","parentSpanId":"<16hex>",
     "name":"spi.pre","kind":1,
     "startTimeUnixNano":"...","endTimeUnixNano":"..."}]}]}]}
```

Span times are converted to UnixNano using the MONOTONIC↔REALTIME offset
sampled at exporter start. The same bytes are what an HTTP OTLP push would POST;
only the file exporter is wired today (replay from file, or `curl` it yourself).

## Limits

- 256-entry name table per process (shared by C + JS spans).
- 8 producer rings per process; 32 levels of span nesting per thread.
  More than 8 producer threads → the extras' spans drop, logged once by the
  exporter (`thread-ring table exhausted`).
- 16 JS spans open simultaneously per tick (`host_trace_begin` returns 0 past
  that, logged once).
- Trace files: 8 MB rotation, newest 16 per service retained.
