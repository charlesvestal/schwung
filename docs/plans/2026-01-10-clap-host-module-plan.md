# CLAP Host Module Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a new external `clap` module that can host arbitrary CLAP plugins in-process, selectable from the UI, and usable as a sound generator and audio FX inside Signal Chain.

**Architecture:** Create a new `move-anything-clap` repo with a plugin_api_v1 DSP wrapper and a CLAP host core. Add an audio_fx_api_v1 wrapper installed into the chain module’s `audio_fx` directory to support CLAP audio FX in patches. Both wrappers share the same CLAP host core and plugin discovery path `/data/UserData/move-anything/modules/clap/plugins`.

**Tech Stack:** C/C++, dlopen, CLAP headers, QuickJS UI, Move Anything plugin APIs (`plugin_api_v1`, `audio_fx_api_v1`).

---

### Task 1: Create repo skeleton and shared CLAP headers

**Files:**
- Create: `move-anything-clap/README.md`
- Create: `move-anything-clap/LICENSE`
- Create: `move-anything-clap/src/ui.js`
- Create: `move-anything-clap/src/module.json`
- Create: `move-anything-clap/src/dsp/` (directory)
- Create: `move-anything-clap/src/chain_audio_fx/` (directory)
- Create: `move-anything-clap/scripts/build.sh`
- Create: `move-anything-clap/scripts/install.sh`
- Create: `move-anything-clap/third_party/clap/include/` (CLAP headers)

**Step 1: Write the failing test**

Create `move-anything-clap/tests/test_clap_headers.c`:
```c
#include "clap/clap.h"
int main(void) { return 0; }
```

**Step 2: Run test to verify it fails**

Run: `cc tests/test_clap_headers.c -Ithird_party/clap/include -o /tmp/clap_header_test`
Expected: FAIL with "clap/clap.h: No such file or directory".

**Step 3: Write minimal implementation**

Add CLAP headers into `third_party/clap/include` (vendor the upstream `clap/include` directory). Do not change header names or layout.

**Step 4: Run test to verify it passes**

Run: `cc tests/test_clap_headers.c -Ithird_party/clap/include -o /tmp/clap_header_test`
Expected: PASS (no output, exit 0).

**Step 5: Commit**

```
git add third_party/clap/include tests/test_clap_headers.c

git commit -m "chore: vendor CLAP headers"
```

---

### Task 2: Implement CLAP discovery + port classification (shared core)

**Files:**
- Create: `move-anything-clap/src/dsp/clap_host.h`
- Create: `move-anything-clap/src/dsp/clap_host.c`
- Create: `move-anything-clap/tests/test_clap_scan.c`

**Step 1: Write the failing test**

`move-anything-clap/tests/test_clap_scan.c`:
```c
#include <assert.h>
#include "dsp/clap_host.h"

int main(void) {
    clap_host_list_t list = {0};
    int rc = clap_scan_plugins("tests/fixtures/clap", &list);
    assert(rc == 0);
    assert(list.count == 2);
    assert(list.items[0].has_audio_out == 1);
    assert(list.items[1].has_audio_in == 1);
    clap_free_plugin_list(&list);
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `cc tests/test_clap_scan.c -Ithird_party/clap/include -Isrc -o /tmp/test_clap_scan`
Expected: FAIL with undefined reference to `clap_scan_plugins`.

**Step 3: Write minimal implementation**

Implement `clap_host.h/.c` with:
- `clap_host_list_t` struct (array of plugin metadata: id, name, vendor, flags)
- `clap_scan_plugins(const char *dir, clap_host_list_t *out)` that:
  - iterates `.clap` files (dlopen)
  - reads plugin descriptor and audio/midi port counts
  - fills `has_audio_in`, `has_audio_out`, `has_midi_in`, `has_midi_out`
- `clap_free_plugin_list` to free allocated strings.

Add a `tests/fixtures/clap/` directory with two tiny CLAP test plugins (built as `.so` renamed to `.clap`) that expose different port configs.

**Step 4: Run test to verify it passes**

Run: `cc tests/fixtures/clap/*.c -shared -fPIC -Ithird_party/clap/include -o tests/fixtures/clap/test_a.clap`
Run: `cc tests/fixtures/clap/*.c -shared -fPIC -Ithird_party/clap/include -o tests/fixtures/clap/test_b.clap`
Run: `cc tests/test_clap_scan.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_clap_scan && /tmp/test_clap_scan`
Expected: PASS (exit 0).

**Step 5: Commit**

```
git add src/dsp/clap_host.* tests/test_clap_scan.c tests/fixtures/clap

git commit -m "test: add CLAP discovery and classification"
```

---

### Task 3: Implement CLAP process wrapper for Move plugin_api_v1

**Files:**
- Create: `move-anything-clap/src/dsp/clap_plugin.cpp`
- Modify: `move-anything-clap/src/dsp/clap_host.c`
- Create: `move-anything-clap/tests/test_clap_process.c`

**Step 1: Write the failing test**

`move-anything-clap/tests/test_clap_process.c`:
```c
#include <assert.h>
#include "dsp/clap_host.h"

int main(void) {
    clap_instance_t inst = {0};
    int rc = clap_load_plugin("tests/fixtures/clap/test_a.clap", 0, &inst);
    assert(rc == 0);
    float out[128 * 2] = {0};
    rc = clap_process_block(&inst, NULL, out, 128);
    assert(rc == 0);
    clap_unload_plugin(&inst);
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `cc tests/test_clap_process.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_clap_process`
Expected: FAIL with undefined references to `clap_load_plugin` / `clap_process_block`.

**Step 3: Write minimal implementation**

Add to `clap_host.c`:
- `clap_load_plugin(path, index, clap_instance_t *out)`
- `clap_process_block` that wraps `clap_plugin->process`
- `clap_unload_plugin`
Implement float input/output buffer bridging and minimal event lists.

**Step 4: Run test to verify it passes**

Run: `cc tests/test_clap_process.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_clap_process && /tmp/test_clap_process`
Expected: PASS (exit 0).

**Step 5: Commit**

```
git add src/dsp/clap_host.* tests/test_clap_process.c

git commit -m "test: add CLAP load/process helpers"
```

---

### Task 4: Build Move Anything DSP wrapper (sound generator)

**Files:**
- Modify: `move-anything-clap/src/dsp/clap_plugin.cpp`
- Modify: `move-anything-clap/src/module.json`
- Modify: `move-anything-clap/src/ui.js`
- Create: `move-anything-clap/tests/test_params.c`

**Step 1: Write the failing test**

`move-anything-clap/tests/test_params.c`:
```c
#include <assert.h>
#include "dsp/clap_host.h"

int main(void) {
    clap_instance_t inst = {0};
    int rc = clap_load_plugin("tests/fixtures/clap/test_param.clap", 0, &inst);
    assert(rc == 0);
    assert(clap_param_count(&inst) > 0);
    clap_unload_plugin(&inst);
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `cc tests/test_params.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_params`
Expected: FAIL with undefined reference to `clap_param_count`.

**Step 3: Write minimal implementation**

In `clap_host.c`, add:
- `clap_param_count`, `clap_param_info`, `clap_param_set`, `clap_param_get`
- map params to string keys `param_<index>`

In `clap_plugin.cpp`, implement `plugin_api_v1` hooks:
- `on_load`: scan plugins, select default or stored plugin
- `set_param`: handles `selected_plugin`, `param_*`, and `refresh`
- `get_param`: exposes `plugin_count`, `plugin_name_<i>`, `param_name_<i>`, `param_value_<i>`
- `render_block`: uses `clap_process_block` and converts float->int16
- `on_midi`: forwards to CLAP event input

Update `module.json` to declare chainable capability and default path.
Update `ui.js` to provide a list browser + parameter bank UI.

**Step 4: Run test to verify it passes**

Run: `cc tests/test_params.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_params && /tmp/test_params`
Expected: PASS (exit 0).

**Step 5: Commit**

```
git add src/dsp/clap_plugin.cpp src/module.json src/ui.js src/dsp/clap_host.* tests/test_params.c

git commit -m "feat: add CLAP sound generator module"
```

---

### Task 5: Add CLAP audio FX wrapper for Signal Chain

**Files:**
- Create: `move-anything-clap/src/chain_audio_fx/clap_fx.cpp`
- Create: `move-anything-clap/src/chain_audio_fx/audio_fx_api_v1.h` (copy from Move host)
- Create: `move-anything-clap/tests/test_audio_fx_process.c`

**Step 1: Write the failing test**

`move-anything-clap/tests/test_audio_fx_process.c`:
```c
#include <assert.h>
#include "dsp/clap_host.h"

int main(void) {
    clap_instance_t inst = {0};
    int rc = clap_load_plugin("tests/fixtures/clap/test_fx.clap", 0, &inst);
    assert(rc == 0);
    float in[128 * 2] = {0};
    float out[128 * 2] = {0};
    rc = clap_process_block(&inst, in, out, 128);
    assert(rc == 0);
    clap_unload_plugin(&inst);
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `cc tests/test_audio_fx_process.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_audio_fx_process`
Expected: FAIL with undefined reference to `clap_process_block` if not in scope for FX path or missing fixture.

**Step 3: Write minimal implementation**

Implement `clap_fx.cpp` with `audio_fx_api_v1`:
- `on_load` loads selected CLAP FX plugin
- `process_block` wraps CLAP process and supports audio input/output
- `set_param/get_param` re-use `clap_param_*` helpers

**Step 4: Run test to verify it passes**

Run: `cc tests/test_audio_fx_process.c -Ithird_party/clap/include -Isrc -ldl -o /tmp/test_audio_fx_process && /tmp/test_audio_fx_process`
Expected: PASS (exit 0).

**Step 5: Commit**

```
git add src/chain_audio_fx/clap_fx.cpp src/chain_audio_fx/audio_fx_api_v1.h tests/test_audio_fx_process.c

git commit -m "feat: add CLAP audio FX wrapper"
```

---

### Task 6: Build scripts, install flow, and chain patches

**Files:**
- Modify: `move-anything-clap/scripts/build.sh`
- Modify: `move-anything-clap/scripts/install.sh`
- Create: `move-anything-clap/src/chain_patches/clap_reverb.json`
- Modify: `move-anything-clap/README.md`

**Step 1: Write the failing test**

Create `move-anything-clap/tests/test_build_outputs.sh`:
```sh
#!/bin/sh
set -e
./scripts/build.sh
[ -f dist/clap/dsp.so ]
[ -f dist/clap/module.json ]
[ -f dist/chain_audio_fx/clap.so ]
```

**Step 2: Run test to verify it fails**

Run: `sh tests/test_build_outputs.sh`
Expected: FAIL because build outputs are not produced yet.

**Step 3: Write minimal implementation**

Update `build.sh` to compile:
- `dist/clap/dsp.so` from `src/dsp/clap_plugin.cpp` + `clap_host.c`
- `dist/chain_audio_fx/clap.so` from `src/chain_audio_fx/clap_fx.cpp` + `clap_host.c`

Update `install.sh` to:
- Copy `dist/clap/` to `/data/UserData/move-anything/modules/clap/`
- Copy `dist/chain_audio_fx/clap.so` to `/data/UserData/move-anything/modules/chain/audio_fx/clap/clap.so`
- Copy any chain patches to `/data/UserData/move-anything/modules/chain/patches/`

Add a minimal patch JSON that uses `clap` as a synth or FX.

**Step 4: Run test to verify it passes**

Run: `sh tests/test_build_outputs.sh`
Expected: PASS (outputs exist).

**Step 5: Commit**

```
git add scripts/build.sh scripts/install.sh src/chain_patches/clap_reverb.json tests/test_build_outputs.sh README.md

git commit -m "feat: add build/install and chain patch"
```

---

### Task 7: Update Move Anything docs to mention CLAP module

**Files:**
- Modify: `move-anything/docs/MODULES.md`

**Step 1: Write the failing test**

Create `move-anything/tests/test_docs_clap.sh`:
```sh
#!/bin/sh
set -e
rg -n "CLAP" docs/MODULES.md
```

**Step 2: Run test to verify it fails**

Run: `sh move-anything/tests/test_docs_clap.sh`
Expected: FAIL (no CLAP mention yet).

**Step 3: Write minimal implementation**

Add a short note in `docs/MODULES.md` about the CLAP module and plugin directory path.

**Step 4: Run test to verify it passes**

Run: `sh move-anything/tests/test_docs_clap.sh`
Expected: PASS (match found).

**Step 5: Commit**

```
git add docs/MODULES.md tests/test_docs_clap.sh

git commit -m "docs: mention CLAP module"
```

---

## Branching

- Create a new branch in `move-anything-clap` for module work (e.g., `feat/clap-module`).
- If modifying `move-anything`, use a separate branch there (e.g., `docs/clap-module`).

## Testing Summary

- `cc tests/test_clap_headers.c -Ithird_party/clap/include -o /tmp/clap_header_test`
- `cc tests/test_clap_scan.c ... && /tmp/test_clap_scan`
- `cc tests/test_clap_process.c ... && /tmp/test_clap_process`
- `cc tests/test_params.c ... && /tmp/test_params`
- `cc tests/test_audio_fx_process.c ... && /tmp/test_audio_fx_process`
- `sh tests/test_build_outputs.sh`
- `sh move-anything/tests/test_docs_clap.sh`

