# Disable USB-C Output Persistence Hotfix Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Ship a 1.3.2 hotfix that cannot replay Schwung's saved USB-C Main Out state and therefore cannot mute the built-in speaker at boot.

**Architecture:** Keep the existing XMOS codec and saved state file untouched, but remove the user-facing control and force the compatibility parameter off. Stop configuration loading from enabling the feature. This is a reversible behavioral disable, not a destructive migration.

**Tech Stack:** C, JavaScript ES modules, POSIX shell regression tests.

### Task 1: Pin the disabled behavior

**Files:**
- Create: `tests/host/test_usbc_out_persist_disabled.sh`
- Modify: `tests/host/test_global_settings_contract.sh`

1. Add assertions that the Global Settings contract no longer exposes `usbc_out_persist`.
2. Add source-contract assertions that the runtime default is off, saved JSON cannot enable it, and compatibility SET requests cannot enable it.
3. Run both tests and verify they fail against 1.3.1 for the expected reasons.

### Task 2: Disable replay and hide the control

**Files:**
- Modify: `src/host/shadow_resample.c`
- Modify: `src/schwung_shim.c`
- Modify: `src/shadow/shadow_ui_global_grid.mjs`

1. Force `usbc_out_persist_enabled` to zero and stop parsing the saved key.
2. Keep the shim parameter for compatibility, but make SET a no-op and GET report zero.
3. Remove the USB-C Persist row from Global Settings.
4. Run the focused tests and verify they pass.

### Task 3: Document and version the hotfix

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/SPI_PROTOCOL.md`
- Modify: `src/host/version.txt`
- Modify: `module-catalog.json`

1. Document that persistence is disabled in 1.3.2 because Main Out replay can leave speakers muted.
2. Bump the host version and catalog URL to 1.3.2.
3. Run the complete host test suite and build/package verification.
