# Host-side unit tests

These tests run on the dev machine (Mac/Linux), not on the Move device.
They cover small, well-isolated pieces of code where end-to-end testing
on hardware is overkill — typically pure data-structure helpers that are
worth pounding with multi-threaded stress before they ship into the shim
or a host process.

## Running

```sh
make -C tests/host test
```

Builds artifacts in `build/tests/host/` and runs each as a separate
binary. A passing run prints `ALL PASS`; any assertion failure aborts.

## What's here

- **`test_midi_inject_writer`** — exercises `shadow_midi_inject_push()`
  (in `src/host/shadow_midi_inject_writer.h`), the lock-free MPSC helper
  that all producers must use to write into `/schwung-midi-inject`.
  Stack-allocates a `shadow_midi_inject_t`, runs single-thread happy
  path, full-buffer rejection, and 8-thread concurrent stress (50
  iterations) to validate that no two writers ever clobber the same
  slot.

## When to add a test here

Add a host-side unit test when:

- The code under test is **pure** (no SHM, no `/dev/ablspi0.0`, no
  QuickJS) — or can be exercised through a tiny shim that fakes those.
- You want **deterministic** coverage of edge cases (boundary values,
  concurrency interleavings) that are awkward to reproduce on hardware.
- The cost of a hardware round-trip per change is too high relative to
  the change rate.

For anything that requires a live shim, Move firmware, or LED feedback,
test on hardware against a deployed build.
