# Per-Module Overtake Exit Hooks

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Prevent exiting one overtake module from killing another module's background processes (e.g., exiting Performance FX kills suspended RNBO Runner's JACK session).

**Architecture:** Replace the single global exit hook (`overtake-exit.sh`) with per-module hooks identified by module ID. The shadow UI writes the active module ID to a file before exit. The shim reads it and runs only the matching hook. Modules install hooks with their own ID in the filename.

**Tech Stack:** C (shim), JavaScript (shadow UI), shell (exit hooks)

---

### Task 1: Shadow UI writes active module ID on exit

**Files:**
- Modify: `src/shadow/shadow_ui.js` — `exitOvertakeMode()`, `loadOvertakeModule()`, `exitToolOvertake()`

**Step 1: Track the active overtake module ID**

Near the existing `overtakeModulePath` declaration (~line 463), add:

```javascript
let overtakeModuleId = "";         // ID of loaded overtake module (for per-module exit hooks)
```

**Step 2: Set the ID when loading a module**

In `loadOvertakeModule()` (~line 2748), after `overtakeModulePath = moduleInfo.uiPath;` add:

```javascript
overtakeModuleId = moduleInfo.id || "";
```

**Step 3: Clear the ID in all exit/cleanup paths**

In `exitOvertakeMode()`, `suspendOvertakeMode()`, `exitToolOvertake()`, and the error cleanup at ~line 2480, alongside `overtakeModulePath = "";` add:

```javascript
overtakeModuleId = "";
```

**Step 4: Write the module ID to a file before exit triggers overtake_mode transition**

In `exitOvertakeMode()`, before `overtakeExitPending = true;` (~line 2538), add:

```javascript
/* Write exiting module ID so shim runs the correct per-module hook */
if (overtakeModuleId && typeof host_write_file === "function") {
    host_write_file("/data/UserData/schwung/hooks/.exiting-module-id", overtakeModuleId);
}
```

In `exitToolOvertake()`, add the same before its exit path.

Do NOT write the file in `suspendOvertakeMode()` — suspend already skips hooks via the `suspend_overtake` flag.

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: write overtake module ID to file on exit for per-module hooks"
```

---

### Task 2: Shim reads module ID and runs per-module hook

**Files:**
- Modify: `src/schwung_shim.c` — overtake exit hook logic (~line 4053-4065)

**Step 1: Replace global hook with per-module hook lookup**

Replace the current hook execution block:

```c
} else {
    system("sh -c 'test -x /data/UserData/schwung/hooks/overtake-exit.sh && "
           "/data/UserData/schwung/hooks/overtake-exit.sh' &");
    /* Clear JACK LED cache on clean exit */
    led_queue_clear_jack_cache();
}
```

With:

```c
} else {
    /* Read exiting module ID and run per-module hook if it exists,
     * otherwise fall back to the global hook for backward compat. */
    char module_id[64] = {0};
    FILE *f = fopen("/data/UserData/schwung/hooks/.exiting-module-id", "r");
    if (f) {
        if (fgets(module_id, sizeof(module_id), f)) {
            /* Strip newline */
            char *nl = strchr(module_id, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
        unlink("/data/UserData/schwung/hooks/.exiting-module-id");
    }

    char hook_path[256];
    if (module_id[0]) {
        snprintf(hook_path, sizeof(hook_path),
                 "/data/UserData/schwung/hooks/overtake-exit-%s.sh", module_id);
    } else {
        hook_path[0] = '\0';
    }

    /* Try per-module hook first, fall back to global */
    char cmd[512];
    if (hook_path[0] && access(hook_path, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "sh -c '%s' &", hook_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "sh -c 'test -x /data/UserData/schwung/hooks/overtake-exit.sh && "
                 "/data/UserData/schwung/hooks/overtake-exit.sh' &");
    }
    system(cmd);
    led_queue_clear_jack_cache();
}
```

**Step 2: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: shim runs per-module exit hook, falls back to global"
```

---

### Task 3: RNBO Runner installs per-module hook

**Files:**
- Modify: `src/modules/overtake/rnbo-runner/ui.js` — hook installation
- Create: `src/modules/overtake/rnbo-runner/exit-hook.sh` (already exists, just rename target)

**Step 1: Change hook installation path**

In `src/modules/overtake/rnbo-runner/ui.js`, find where the exit hook is installed (look for `overtake-exit.sh`). Change the install target from:

```javascript
shadow_set_param(0, "system_cmd", "cp " + basePath + "/exit-hook.sh /data/UserData/schwung/hooks/overtake-exit.sh && chmod +x /data/UserData/schwung/hooks/overtake-exit.sh");
```

To:

```javascript
shadow_set_param(0, "system_cmd", "cp " + basePath + "/exit-hook.sh /data/UserData/schwung/hooks/overtake-exit-rnbo-runner.sh && chmod +x /data/UserData/schwung/hooks/overtake-exit-rnbo-runner.sh");
```

Also keep installing the global hook as a fallback for older host versions:

```javascript
shadow_set_param(0, "system_cmd", "cp " + basePath + "/exit-hook.sh /data/UserData/schwung/hooks/overtake-exit.sh && chmod +x /data/UserData/schwung/hooks/overtake-exit.sh");
```

**Step 2: Update suspendOvertakeMode hook removal**

In `suspendOvertakeMode()` in shadow_ui.js, the line that removes the exit hook:

```javascript
shadow_set_param(0, "system_cmd", "rm -f /data/UserData/schwung/hooks/overtake-exit.sh");
```

Should also remove the per-module hook:

```javascript
shadow_set_param(0, "system_cmd", "rm -f /data/UserData/schwung/hooks/overtake-exit.sh /data/UserData/schwung/hooks/overtake-exit-rnbo-runner.sh");
```

**Step 3: Commit**

```bash
git add src/modules/overtake/rnbo-runner/ui.js src/shadow/shadow_ui.js
git commit -m "feat: RNBO runner installs per-module exit hook"
```

---

### Task 4: Verify and test

**Test scenario:**
1. Launch RNBO Runner from overtake menu
2. Suspend it (Shift+Vol+Back) — JACK stays running
3. Open Performance FX (another overtake module)
4. Exit Performance FX normally
5. Verify JACK is still running: `ssh ableton@move.local "ps | grep jackd"`
6. Re-open RNBO Runner — should resume with audio intact

**Verification:**
- `ls /data/UserData/schwung/hooks/` should show `overtake-exit-rnbo-runner.sh` (not just the global one)
- After exiting Performance FX, `.exiting-module-id` should be consumed (deleted)
- `dmesg | grep oom` should show no new kills

**Step 5: Commit**

```bash
git commit -m "test: verify per-module exit hooks preserve suspended RNBO"
```
