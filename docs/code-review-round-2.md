# Comprehensive Code Review - Round 2

**Date:** 2026-02-06
**Branch:** fix/code-review-round-2
**Base:** main (post v0.3.44 + safety fixes merge)

## Scope

Seven parallel review agents examined the entire codebase:

1. Main host runtime (`move_anything.c`)
2. LD_PRELOAD shim (`move_anything_shim.c`)
3. Shadow UI (`shadow_ui.c` + `shadow_ui.js`)
4. Host support files (`module_manager.c`, `tts_engine_flite.c`, `unified_log.c`, headers)
5. Chain DSP host (`chain_host.c`)
6. JavaScript modules and shared utilities
7. Build and deployment scripts

---

## Summary

| Area | Critical | Important | Minor |
|------|----------|-----------|-------|
| move_anything.c | 4 | 10 | 8 |
| move_anything_shim.c | 4 | 9 | 9 |
| shadow_ui.c + shadow_ui.js | 3 | 7 | 9 |
| Host support files | 2 | 8 | 10 |
| chain_host.c | 4 | 8 | 6 |
| JS modules/shared | 3 | 10 | 10 |
| Scripts | 2 | 9 | 10 |
| **Total** | **22** | **61** | **62** |

---

## CRITICAL Findings

### C1. Uninitialized `cable` variable in `onMidiMessage` (move_anything.c:762)

```c
void onMidiMessage(unsigned char midi_message[4]) {
    int cable;              // NEVER initialized
    if (cable == 0) { ... } // UB
}
```

Function appears unused (actual routing is in `main()` loop). **Fix: Delete the function or parse cable from `midi_message[0] >> 4`.**

### C2. `get_pixel()` missing bounds check (move_anything.c:231)

```c
int get_pixel(int x, int y) {
    return screen_buffer[y*128+x] > 0 ? 1 : 0;  // No bounds check!
}
```

Unlike `set_pixel()` which validates. Called from `glyph()`. **Fix: Add same bounds check as `set_pixel()`.**

### C3. `js_print()` accesses `argv[3]` without checking `argc >= 4` (move_anything.c:1075)

Checks `argc < 3` but then unconditionally accesses `argv[3]`. When `argc == 3`, this is OOB. **Fix: Guard with `if (argc >= 4)`.**

### C4. `load_font()` sprintf buffer overflow (move_anything.c:297)

```c
char charListFilename[100];
sprintf(charListFilename, "%s.dat", filename);  // No length check
```

**Fix: Use `snprintf`.**

### C5. `volatile` insufficient for cross-process atomics on ARM64 (shadow_constants.h:76-97)

All `shadow_control_t`, `shadow_param_t`, `shadow_midi_out_t`, and `shadow_screenreader_t` structures use `volatile` for cross-process IPC. `volatile` does NOT provide atomicity or memory ordering on ARM. Multi-byte fields (`uint32_t`, `float`, `uint16_t`) can tear.

**Fix: Use `__atomic_load_n` / `__atomic_store_n` for signaling fields, add `__sync_synchronize()` barriers between signal reads and payload reads.**

### C6. D-Bus thread writes to shared state without synchronization (shim:1216-1288)

`shadow_dbus_handle_text()` runs on D-Bus thread and writes to `shadow_chain_slots[].volume` (float), calls `shadow_save_state()` (file I/O), and writes to `sampler_current_set_name` -- all read by the ioctl thread without locks.

**Fix: Protect slot volume writes with spinlock or atomic float ops. Defer `shadow_save_state()` to ioctl thread via flag.**

### C7. Unaligned ARM access in D-Bus serial parsing (shim:1346)

```c
uint32_t serial = *(uint32_t*)(buf + 8);  // Potential SIGBUS on ARM
```

**Fix: Use `memcpy(&serial, buf + 8, sizeof(serial))`.**

### C8. `fork()` called from ioctl/audio context (shim:1900-1912, 6377-6392)

`launch_shadow_ui()` is called from the ioctl hook. `fork()` in a multi-threaded process copies the entire address space, causing audio dropouts.

**Fix: Spawn a dedicated helper thread for process management rather than forking from the ioctl path.**

### C9. Uninitialized JSValue when `getGlobalFunction` fails (shadow_ui.c:1245-1264)

```c
JSValue JSinit;  // uninitialized
getGlobalFunction(ctx, "init", &JSinit);  // if fails, JSinit stays garbage
callGlobalFunction(ctx, &JSinit, 0);       // UB
```

**Fix: Initialize to `JS_UNDEFINED` and guard calls behind success check.**

### C10. No memory barriers on shared memory IPC (shadow_ui.c throughout)

Same `volatile`-only issue as C5, but from the shadow_ui process side. Parameter request/response protocol has no barrier between checking `request_type` and reading `key`/`value`. MIDI output write path similarly unprotected.

**Fix: Same as C5 -- use atomic operations with acquire/release semantics.**

### C11. `getMasterFxParam` called but never defined (shadow_ui.js:779,781,4284)

```javascript
const fxError = getMasterFxParam(fxSlot, "error");  // ReferenceError!
```

Called in 3 places, never defined anywhere. Will throw at runtime when Master FX slots are accessed.

**Fix: Define the function (likely wraps `shadow_get_param`).**

### C12. TTS "ring buffer" is not actually a ring buffer (tts_engine_flite.c:102-104,396-401)

Write path always resets both `ring_write_pos` and `ring_read_pos` to 0 before writing. Read path has dead code for wrap-around computation that would produce incorrect results.

**Fix: Either make it a true ring buffer or remove the wrap-around logic and document as linear overwrite buffer.**

### C13. `system()` called for `mkdir` in chain_host.c (chain_host.c:572-574)

```c
system(mkdir_cmd);  // Forks shell in audio context
```

**Fix: Replace with `mkdir()` from `<sys/stat.h>`.**

### C14. Ring buffer data race in chain recording (chain_host.c:3090-3108)

`g_ring_write_pos` and `g_ring_read_pos` are `volatile size_t` but accessed from audio thread and writer thread without memory barriers. On ARM, this can cause torn reads. Additionally, `stop_recording()` can free `g_ring_buffer` while the audio thread is writing to it.

**Fix: Use `__atomic_load_n` / `__atomic_store_n` with appropriate memory ordering.**

### C15. Integer overflow in `send_note_to_synth` transpose (chain_host.c:2536)

Negative interval + note cast to `uint8_t` wraps to high value. Works by coincidence (fails `<= 127` check), but the V2 version correctly checks the `int` result first.

**Fix: Check `int result = (int)msg[1] + interval; if (result < 0 || result > 127) return;`**

### C16. JSON injection in `save_patch` / `save_master_preset` (chain_host.c:2035-2041, 3982)

Patch `name` and `json_data` inserted directly into JSON without escaping quotes/backslashes. Malformed names produce unparseable patch files.

**Fix: Escape special characters in `name` before JSON insertion.**

### C17. `strncpy` buffer overflows in V2 knob mapping (chain_host.c:5249,5260)

```c
strncpy(inst->knob_mappings[found].target, target, 31);  // target buffer is 16 bytes!
strncpy(inst->knob_mappings[i].param, param, 63);         // param buffer is 32 bytes!
```

**Fix: Use `sizeof(field) - 1` as the length argument.**

### C18. Off-by-one in `clearAllLEDs` (input_filter.mjs:80)

```javascript
for (let i = 0; i < 127; i++) {  // Should be i < 128
```

LED index 127 is never cleared.

**Fix: Change `i < 127` to `i < 128`.**

### C19. Missing MIDI channel masking in controller module (controller/ui.js:139-145)

```javascript
const isNote = status === MidiNoteOn;  // Only matches channel 0!
```

Should use `(data[0] & 0xF0) === MidiNoteOn`.

### C20. `install.sh` wrong shebang (install.sh:1)

`#!/usr/bin/env sh` but uses `local` keyword and other bash-isms.

**Fix: Change to `#!/usr/bin/env bash`.**

### C21. `package.sh` lacks `set -e` (package.sh)

Can produce corrupt tarballs silently if any step fails.

**Fix: Add `set -eo pipefail`.**

### C22. `isNoiseMessage` doesn't mask channel nibble (input_filter.mjs:29-38)

`MidiChAftertouch` (0xD0) comparison only matches channel 1. Aftertouch on channels 2-16 won't be filtered.

**Fix: Use `(data[0] & 0xF0)` for channel-voice message comparisons.**

---

## IMPORTANT Findings

### Host Runtime (move_anything.c)

- **I1.** Font `charData` array not zeroed -- `malloc` without `memset`, `glyph()` checks `fc.data == NULL` but garbage may look non-NULL (line 317). **Fix: Use `calloc` or `memset`.**
- **I2.** Font memory leak -- `stbi_load` data never freed with `stbi_image_free()` (line 311). **Fix: Add `stbi_image_free(data)` before return.**
- **I3.** `load_font()` unbounded loop -- `while(data[x] == borderColor) x++;` without bounds check against `width` (line 342).
- **I4.** Negative index array access in `load_font` and `glyph` -- `char` cast to `int` produces negative index for chars >= 128 (lines 389, 479). **Fix: Cast to `unsigned char`.**
- **I5.** `exit(0)` bypasses cleanup -- `deinit_javascript` is dead code (line 2820).
- **I6.** `eval_file()` calls `exit(1)` on load failure -- hard exit without cleanup (line 913).
- **I7.** JSValue leak on menu reload -- `JSinit` obtained but never `JS_FreeValue`'d (line 2636).
- **I8.** `queueMidiSend` counter stays stuck after overflow (line 654).
- **I9.** `validate_path()` TOCTOU race and `strstr("..")` overly broad (lines 1822-1834).
- **I10.** `push_screen()` bounds fragile -- zero margin on 1024-byte buffer (lines 2418-2448).

### Shim (move_anything_shim.c)

- **I11.** File handle leaks -- ~11 debug `FILE*` handles opened but never closed (lines 138-560).
- **I12.** Shared memory segments never cleaned up -- `shm_unlink()` never called, stale segments persist across restarts.
- **I13.** `fread()` return not checked in 6+ locations (lines 3028, 3078, 3203, 3384, 3947, 4265).
- **I14.** `sampler_stop_recording()` calls `pthread_join` in ioctl path -- blocking call in audio context (line 2492).
- **I15.** Skipback writer thread uses `localtime()` (not thread-safe) and reads `skipback_write_pos` without acquire semantics (line 2662).
- **I16.** `mmap()` hook matches on size only (`length == 4096`) -- any 4096-byte mmap is intercepted (lines 6039-6100). **Very fragile.**
- **I17.** Integer overflow potential in timing calculations -- `tv_nsec` subtraction can wrap negative (lines 7756-7763).
- **I18.** `localtime()` not thread-safe -- should use `localtime_r()` in sampler (line 2398).
- **I19.** `launch_shadow_ui()` fork from ioctl path can cause audio dropouts (lines 6071, 5945).

### Shadow UI (shadow_ui.c + shadow_ui.js)

- **I20.** Resource leak on partial shm_open failure -- first mapped region not unmapped if later shm_open fails (lines 55-107).
- **I21.** MIDI output buffer `write_idx` never reset by shadow UI, no synchronization with shim reader (lines 493-501).
- **I22.** `validate_path` TOCTOU and non-existent path bypass -- `realpath()` fails for new files, falling back to weak string checks only (lines 592-603).
- **I23.** JSValue reference leaks for function handles before runtime destroy (lines 1245-1303).
- **I24.** No URL validation in `host_http_download` (lines 630-677).
- **I25.** Loaded overtake modules execute unsandboxed with full host function access (shadow_ui.js:690-747).
- **I26.** `masterFxChainConfig` referenced but should be `masterFxConfig` (shadow_ui.js:4351).

### Host Support Files

- **I27.** JSON key substring matching false positives in module_manager.c -- searching for `"id"` also matches `"grid"` (lines 14-36).
- **I28.** No synchronization between audio render and module load/unload in module_manager.c (lines 448-504).
- **I29.** Path traversal possible via module directory names or module.json paths (module_manager.c:226-227).
- **I30.** `synth_thread_running` not atomic or volatile in tts_engine_flite.c (lines 58, 285, 304).
- **I31.** Voice features modified without synthesis mutex in tts_engine_flite.c (lines 448-451, 477-479).
- **I32.** `tts_enabled` / `tts_disabling` flags accessed without synchronization (lines 38-40, 332-394, 501-535).
- **I33.** `localtime()` should be `localtime_r()` in unified_log.c (line 76).
- **I34.** Double mutex acquisition per log call in unified_log.c (lines 65-68).
- **I35.** Struct padding depends on compiler in shadow_constants.h -- shared across separate binaries (lines 76-97). **Fix: Consider `__attribute__((packed))`.**

### Chain DSP Host (chain_host.c)

- **I36.** Division by zero in `next_patch`/`prev_patch` -- modulo before `g_patch_count > 0` check (lines 2776-2783).
- **I37.** `strncpy` without NUL termination in multiple locations (throughout).
- **I38.** `fread()` return unchecked at lines 765, 1426, 1639, 3538, 4155.
- **I39.** `ftell()` can return -1 (error) -- not all locations check `size <= 0` (line 1420 only checks upper bound).
- **I40.** Path traversal in module loading -- `fx_name` and `synth_module` from patch files used directly in `dlopen` paths (lines 974, 1073, 2273).
- **I41.** Unbounded `strcat` in V1 `plugin_get_param` JSON building -- `knob_json[1024]` can overflow with 8 entries (lines 2889-2913).
- **I42.** `component_ui_mode` array access without bounds check (line 2839).
- **I43.** Global state shared between V1 and V2 -- recording uses globals even in V2 (lines 292-418).

### JavaScript Modules/Shared

- **I44.** Missing null guard in `menu_nav.mjs` `handleMenuInput` when items list is empty (line 46).
- **I45.** Screen reader announcement state leaks across menus -- module-level globals `lastAnnouncedIndex`/`lastAnnouncedLabel` (menu_layout.mjs:29-30).
- **I46.** `parseInt(bankIdx) || 0` treats legitimate value 0 same as error (sound_generator_ui.mjs).
- **I47.** Path construction in store_utils.mjs doesn't sanitize `mod.id` for path traversal (lines 70, 233, 267-268).
- **I48.** Duplicate `Blue` constant shadows `PureBlue` (constants.mjs:204,217).
- **I49.** `host_extract_tar_strip` called in JS but never registered in C host (shadow_ui.js:2586).

### Scripts

- **I50.** Command-in-variable anti-pattern in install.sh and uninstall.sh -- `$ssh_ableton` expanded via word splitting (lines 351-353).
- **I51.** Unsanitized catalog-derived variables in remote shell commands (install.sh:745).
- **I52.** `rm -rf ~/move-anything` in uninstall.sh deletes all user data without warning about data loss (line 48).
- **I53.** `chmod u+s` on .so file has no effect on Linux -- SUID only works on executables (install.sh:608).
- **I54.** Inconsistent error handling across scripts -- `package.sh` has no `set -e`, `clean.sh` has none.
- **I55.** Flite library symlinks not cleaned up in uninstall.sh.

---

## MINOR Findings

### Host Runtime (move_anything.c)

- m1. Unused variable `entry` in `js_move_midi_send` (line 1126)
- m2. `module` parameter misuse -- using `-1` as boolean for module flag (line 2489)
- m3. `clock_mode` array access without bounds check (line 1614)
- m4. Missing NULL check on `mapped_memory` in multiple functions
- m5. Global state pollution -- ~30 global variables
- m6. `kickM8()` copies 23 bytes from 24-byte array without comment (line 841)
- m7. Unchecked `fread` return in `load_ttf_font` (line 417)
- m8. `js_host_load_ui_module()` no path validation (line 1372)

### Shim (move_anything_shim.c)

- m9. Duplicate `#include <unistd.h>` (lines 11, 15)
- m10. Unused variable `ret` in `launchChildAndKillThisProcess` -- control flow bug after `execl()` (line 6295)
- m11. Extra semicolon `};;}` in struct declaration (line 1040)
- m12. `recording_flash_counter` never resets (line 2816)
- m13. Magic numbers throughout for display coordinates, timing, etc.
- m14. Shadowed variable `dbg` (lines 3896, 3967)
- m15. `shadow_midi_out_log_enabled()` checks filesystem on every call without caching (lines 3285-3299)
- m16. Large stack allocations in ioctl path (256+256+1024 bytes)
- m17. `shadow_dbus_stop()` joins thread without timeout (line 1728)

### Shadow UI

- m18. `read_json_string` may match wrong key via substring (shadow_ui.c:881-909)
- m19. Integer overflow in `refresh_counter` after ~414 days (shadow_ui.c:1289)
- m20. Missing `global_obj` free in `init_javascript` (shadow_ui.c:1135)
- m21. Global state not fully reset on overtake module exit (shadow_ui.js:1052-1079)
- m22. Inconsistent `shadow_get_param` usage -- sometimes with try/catch, sometimes without (shadow_ui.js)
- m23. `storePickerSelectedIndex` edge case with empty list (shadow_ui.js:2528-2533)
- m24. JSON.parse without try/catch in some locations (shadow_ui.js)
- m25. Blocking network ops freeze entire UI during store operations (shadow_ui.js)
- m26. `colourNames` object has gap at index 103 (constants.mjs)

### Host Support Files

- m27. JSON key search buffer 125-char limit (module_manager.c)
- m28. No JSON escape sequence handling in module_manager.c
- m29. Brace counting in defaults ignores string contents (module_manager.c)
- m30. `atoi` returns 0 silently on non-numeric input (module_manager.c)
- m31. Off-by-one in ring buffer boundary check (tts_engine_flite.c:115) -- safe but misleading
- m32. 2MB static ring buffer on embedded device (tts_engine_flite.c:32)
- m33. Lazy init race with cleanup in tts_engine_flite.c (lines 337-342)
- m34. Log file grows unbounded (unified_log.c:19)
- m35. Header file named `plugin_api_v1.h` but contains both v1 and v2 APIs
- m36. No `get_error` function in audio FX API
- m37. Data fields not volatile in `shadow_param_t` and `shadow_screenreader_t`

### Chain DSP Host (chain_host.c)

- m38. Debug logging to hardcoded `/tmp/` paths in render-adjacent code (lines 3957, 4122)
- m39. Large code duplication between V1 and V2 implementations
- m40. Fragile hand-rolled JSON parser throughout
- m41. V1 recording guard checks `g_synth_plugin` but not V2 synths (line 705)
- m42. `ftell` not checked for error in `parse_chain_params` (line 1415)
- m43. `localtime()` not thread-safe in chain_host.c (line 578)

### JavaScript

- m44. Unused variable `isNote` in menu_ui.js (line 316)
- m45. Debug `console.log` calls left in menu_nav.mjs (lines 72-74, 154-168)
- m46. Integer division used where bitwise shift intended in controller (line 132)
- m47. Duplicate `truncateText` function in menu_layout.mjs and chain_ui_views.mjs
- m48. Duplicate arrow drawing functions in menu_layout.mjs and scrollable_text.mjs
- m49. Duplicate `drawRect` functions in menu_layout.mjs and text_entry.mjs
- m50. Version comparison doesn't handle pre-release versions (store_utils.mjs)
- m51. `const` in switch cases without block scoping (menu_items.mjs)
- m52. `os.sleep` calls block event loop in store UI

### Scripts

- m53. `find | while read` should use `read -r` (build.sh:198)
- m54. Silent glob failure on `*.mjs` (build.sh:180)
- m55. SSH polling loop creates 15 connections instead of one (install.sh:942-949)
- m56. No `trap` cleanup handlers in any scripts
- m57. `StrictHostKeyChecking=accept-new` weakens first-connection MITM protection
- m58. Relative path `cd ./build` in package.sh (line 12)
- m59. Hardcoded file list in package.sh without existence checks (line 15)
- m60. `killall` may not exist on minimal Linux (uninstall.sh:40)
- m61. Race condition between process kill and binary restore in uninstall.sh (line 44)
- m62. `xargs` used to trim whitespace -- has side effects (install.sh:666)

---

## Prioritized Fix Recommendations

### Priority 1: Safety-Critical (audio glitches, crashes, data corruption)

1. **C3** `js_print()` argv OOB -- trivial 1-line fix
2. **C2** `get_pixel()` bounds check -- trivial 3-line fix
3. **C4** sprintf overflow in `load_font()` -- trivial snprintf swap
4. **C17** strncpy buffer overflows in V2 knob mapping -- use `sizeof(field)`
5. **C15** Integer overflow in `send_note_to_synth` -- check before cast
6. **C13** `system()` in chain_host.c -- replace with `mkdir()`
7. **I36** Division by zero in `next_patch`/`prev_patch` -- move guard before modulo
8. **I1** Font `charData` not zeroed -- calloc or memset
9. **I4** Negative index from signed char cast -- cast to unsigned char

### Priority 2: Correctness (wrong behavior at runtime)

10. **C11** `getMasterFxParam` undefined -- define the function
11. **C18** `clearAllLEDs` off-by-one -- change `< 127` to `< 128`
12. **C19** Controller MIDI channel masking -- use `& 0xF0`
13. **C22** `isNoiseMessage` channel masking -- use `& 0xF0`
14. **C9** Uninitialized JSValue -- initialize to `JS_UNDEFINED`
15. **I26** `masterFxChainConfig` should be `masterFxConfig`
16. **I41** Unbounded `strcat` in V1 JSON building -- use snprintf with offset

### Priority 3: Thread Safety (potential races, not yet observed as crashes)

17. **C5/C10** volatile insufficient on ARM64 -- add memory barriers to shared memory IPC
18. **C6** D-Bus thread data race on slot volume -- protect with lock or atomic
19. **C7** Unaligned ARM access -- use memcpy
20. **C14** Ring buffer race in chain recording -- use atomic operations
21. **I30-32** TTS thread safety issues -- protect flags with atomics/mutex
22. **I33-34** localtime_r and double mutex in unified_log
23. **I28** Module load/unload vs audio thread synchronization

### Priority 4: Robustness (error handling, resource cleanup)

24. **I13** fread return checks in shim (6+ locations)
25. **I38** fread return checks in chain_host (5+ locations)
26. **I11** File handle leaks in debug logging
27. **I12** Shared memory segments never unlinked
28. **I2** Font memory leak (stbi_image_free)
29. **I7** JSValue leak on menu reload
30. **C20-21** Script shebang and set -e fixes

### Priority 5: Security Hardening (low risk given trusted environment)

31. **C16** JSON injection in patch names -- escape special chars
32. **I29/I40** Path traversal in module/plugin loading -- validate names
33. **I47** Path traversal in store module ID -- validate characters
34. **I16** mmap hook matches on size only -- add fd/flag checks
35. **I24** URL validation in http_download

---

## Notes

- The target platform is an Ableton Move embedded ARM64 device running a custom Linux
- The application is single-process (move_anything.c) or two-process (shim + shadow_ui)
- Real-time audio constraint: 128 frames/block at 44100 Hz = ~2.9ms per block
- Security model assumes trusted local modules (installed via Module Store from GitHub)
- Many "critical" thread safety issues haven't manifested as crashes because ARM64 stores are naturally aligned and coherent for small types, but they are technically UB
