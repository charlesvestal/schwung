# USB-C Audio-Out Source Persistence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the user's choice of Move's USB-C audio-out source (Mic / Main Out) survive a reboot, by observing the SysEx Move emits and re-asserting it ~5 s after boot.

**Architecture:** A new `src/host/shadow_xmos_audio.{c,h}` holds three pure buffer functions — scan, build, emit — with no I/O, no allocation and no locks, so the SPI callback can call them and the host test suite can compile them directly. The SPI pre-transfer callback scans MIDI_OUT for the XMOS audio-IO envelope and publishes the observed value; the 200 ms worker thread persists it and arms a boot replay; the SPI callback emits the replay into free MIDI_OUT slots, one message per frame.

**Tech Stack:** C11, cross-compiled for ARM64 via Docker. Host unit tests are plain `cc` + a shell wrapper (`tests/host/*.sh`), which CI gates.

**User decisions (already made):**
- "Move's own Settings menu" — the setting is Move firmware's; it reverts to Mic each boot.
- "Global Schwung setting (Recommended)" — one device-wide value, not per-set.
- "I think if we set it via the right message move will show it. audio is what matters, but let's not try to do anything fancy with move's settings" — replay Move's own message; do not puppet Move's menu or inject UI events.
- Capture performed live on hardware 2026-08-18; the command bytes are known, not assumed.

**Refinement over the spec (adopted during planning):** the spec proposed persisting both 23-byte payloads verbatim and replaying them. That would also restore `37 12` bit0, the *input* route bit shared with Move's sampling page, which Move does not restore at boot — re-asserting it would silently change the recording input. Instead we persist a single preference (0 = Mic, 1 = Main Out) and, at replay time, take the `37 12` payload Move itself emitted this boot and flip only bit1. Bit0 stays authoritative from Move. `37 14` is fully synthesized since it carries nothing else.

---

### Task 1: SysEx codec module (`shadow_xmos_audio`)

**Goal:** Three pure functions — scan MIDI_OUT for the XMOS audio-IO envelope, build the replay pair, emit a message into free slots — with unit tests.

**Files:**
- Create: `src/host/shadow_xmos_audio.h`
- Create: `src/host/shadow_xmos_audio.c`
- Test: `tests/host/test_xmos_audio.c`, `tests/host/test_xmos_audio.sh`

**Acceptance Criteria:**
- [ ] Scanning a frame containing the captured Main Out pair sets `usbc_out = 1` and bumps `seq`
- [ ] Scanning the Mic pair sets `usbc_out = 0`
- [ ] Re-scanning an unchanged frame does not bump `seq` (idempotent)
- [ ] LED SysEx (`3B`) and truncated envelopes are ignored
- [ ] Fragments interleaved with non-SysEx packets still parse
- [ ] `xmos_audio_build` preserves `37 12` bit0 from the last observed route payload
- [ ] `xmos_audio_emit` never overwrites an occupied slot and returns 0 when fewer than 8 are free
- [ ] Emit → scan round-trips to the same value

**Verify:** `bash tests/host/test_xmos_audio.sh` → prints `PASS` and exits 0

**Steps:**

- [ ] **Step 1: Write the header**

Create `src/host/shadow_xmos_audio.h`:

```c
/* shadow_xmos_audio.h - Move USB-C audio-out source (Mic / Main Out)
 *
 * Pure buffer helpers for the XMOS audio-IO SysEx envelope. No I/O, no
 * allocation, no locks — safe to call from the SPI callback.
 */
#ifndef SHADOW_XMOS_AUDIO_H
#define SHADOW_XMOS_AUDIO_H

#include <stdint.h>

/* F0 00 21 1D 01 01 37 <inner[15]> F7 */
#define XMOS_AUDIO_MSG_LEN 23
/* 23 bytes fragment as 7x3-byte packets + one 2-byte end packet. */
#define XMOS_AUDIO_PACKETS 8

#define XMOS_AUDIO_KEY_ROUTE 0x12  /* routing + monitoring TLV */
#define XMOS_AUDIO_KEY_MON   0x14  /* dedicated out-source bit */

#define XMOS_AUDIO_ROUTE_BIT_USBC_IN 0x01
#define XMOS_AUDIO_ROUTE_BIT_MONITOR 0x02

typedef struct {
    uint8_t  route[XMOS_AUDIO_MSG_LEN]; /* last observed 37 12 envelope */
    uint8_t  have_route;                /* 1 once route[] is populated */
    int8_t   usbc_out;                  /* -1 unknown, 0 = Mic, 1 = Main Out */
    uint32_t seq;                       /* bumped on every usbc_out change */
} xmos_audio_state_t;

/* Scan a MIDI_OUT region (len bytes, 4-byte USB-MIDI slots) and fold any
 * audio-IO envelopes into st. Returns 1 if usbc_out changed. */
int xmos_audio_scan(const uint8_t *midi_out, int len, xmos_audio_state_t *st);

/* Build the replay pair for usbc_out (0 = Mic, 1 = Main Out). The route
 * message reuses the last payload Move sent so bit0 stays Move's. */
void xmos_audio_build(const xmos_audio_state_t *st, int usbc_out,
                      uint8_t out_route[XMOS_AUDIO_MSG_LEN],
                      uint8_t out_mon[XMOS_AUDIO_MSG_LEN]);

/* Write one message into free slots. Returns 1 on success, 0 if fewer than
 * XMOS_AUDIO_PACKETS slots are free (caller retries next frame). Never
 * overwrites an occupied slot. */
int xmos_audio_emit(uint8_t *midi_out, int len, const uint8_t *msg);

#endif /* SHADOW_XMOS_AUDIO_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_xmos_audio.c`:

```c
/* Unit test for the XMOS audio-IO SysEx codec behind USB-C out persistence.
 *
 * Wire bytes come from a live capture on hardware, 2026-08-18 (see
 * docs/superpowers/specs/2026-08-18-usbc-out-source-capture.txt):
 *   Main Out: F0 00 21 1D 01 01 37 12 02 00x12 F7  +  ... 37 14 01 ...
 *   Mic:      F0 00 21 1D 01 01 37 12 00 00x12 F7  +  ... 37 14 00 ...
 *
 * Build/run: bash tests/host/test_xmos_audio.sh
 */
#include <stdio.h>
#include <string.h>
#include "shadow_xmos_audio.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

#define MIDI_OUT_LEN 80  /* 20 slots x 4 bytes */

/* Append one 23-byte audio-IO message to buf at *slot, mirroring Move's own
 * framing: 7 packets of cin 0x4, then a cin 0x6 end packet. */
static void put_msg(uint8_t *buf, int *slot, uint8_t key, uint8_t val) {
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    memset(msg, 0, sizeof msg);
    const uint8_t hdr[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };
    memcpy(msg, hdr, sizeof hdr);
    msg[7] = key;
    msg[8] = val;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;

    int pos = 0;
    while (pos < XMOS_AUDIO_MSG_LEN) {
        int remaining = XMOS_AUDIO_MSG_LEN - pos;
        int n = remaining >= 3 ? 3 : remaining;
        buf[*slot] = (remaining > 3) ? 0x04 : (n == 3 ? 0x07 : (n == 2 ? 0x06 : 0x05));
        buf[*slot + 1] = msg[pos];
        buf[*slot + 2] = n > 1 ? msg[pos + 1] : 0;
        buf[*slot + 3] = n > 2 ? msg[pos + 2] : 0;
        pos += n;
        *slot += 4;
    }
}

/* A full frame as Move sends it: the 37 12 message then the 37 14 message. */
static void make_frame(uint8_t *buf, uint8_t route_val, uint8_t mon_val) {
    memset(buf, 0, MIDI_OUT_LEN);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, route_val);
    put_msg(buf, &slot, XMOS_AUDIO_KEY_MON, mon_val);
}

static xmos_audio_state_t fresh(void) {
    xmos_audio_state_t st;
    memset(&st, 0, sizeof st);
    st.usbc_out = -1;
    return st;
}

static void test_main_out(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x02, 0x01);
    xmos_audio_state_t st = fresh();
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 1, "main out: scan reports change");
    CHECK(st.usbc_out == 1, "main out: usbc_out == 1");
    CHECK(st.seq == 1, "main out: seq bumped once");
    CHECK(st.have_route == 1, "main out: route payload captured");
    CHECK(st.route[8] == 0x02, "main out: route value byte preserved");
}

static void test_mic(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x00, 0x00);
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 0, "mic: usbc_out == 0");
}

static void test_idempotent(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x02, 0x01);
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "idempotent: second scan reports no change");
    CHECK(st.seq == 1, "idempotent: seq not bumped twice");
}

static void test_ignores_led_sysex(void) {
    /* LED RGB SysEx is the same manufacturer envelope with cmd 0x3B. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x04; buf[1] = 0xF0; buf[2] = 0x00; buf[3] = 0x21;
    buf[4] = 0x04; buf[5] = 0x1D; buf[6] = 0x01; buf[7] = 0x01;
    buf[8] = 0x04; buf[9] = 0x3B; buf[10] = 0x00; buf[11] = 0x05;
    buf[12] = 0x06; buf[13] = 0x00; buf[14] = 0xF7; buf[15] = 0x00;
    xmos_audio_state_t st = fresh();
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "led sysex: no change reported");
    CHECK(st.usbc_out == -1, "led sysex: usbc_out untouched");
}

static void test_rejects_truncated(void) {
    /* Envelope that never reaches F7 — must not be accepted. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x04; buf[1] = 0xF0; buf[2] = 0x00; buf[3] = 0x21;
    buf[4] = 0x04; buf[5] = 0x1D; buf[6] = 0x01; buf[7] = 0x01;
    buf[8] = 0x04; buf[9] = 0x37; buf[10] = 0x14; buf[11] = 0x01;
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == -1, "truncated: usbc_out untouched");
}

static void test_interleaved_non_sysex(void) {
    /* A CC packet lands between two fragments; reassembly must survive it. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_MON, 0x01);
    /* Shift the last packet one slot later, dropping a CC into the gap. */
    memcpy(&buf[slot], &buf[slot - 4], 4);
    buf[slot - 4] = 0x0B; buf[slot - 3] = 0xB0; buf[slot - 2] = 0x4F; buf[slot - 1] = 0x40;
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1, "interleaved: still parsed");
}

static void test_build_preserves_input_route_bit(void) {
    uint8_t buf[MIDI_OUT_LEN];
    /* Move last selected USB-C *input* (bit0) with monitoring off. */
    make_frame(buf, 0x01, 0x00);
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);

    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(route[8] == 0x03, "build: monitor bit set, input route bit kept");
    CHECK(mon[7] == XMOS_AUDIO_KEY_MON && mon[8] == 0x01, "build: mon message correct");

    xmos_audio_build(&st, 0, route, mon);
    CHECK(route[8] == 0x01, "build: monitor bit cleared, input route bit kept");
    CHECK(mon[8] == 0x00, "build: mon message cleared");
}

static void test_build_without_observation(void) {
    xmos_audio_state_t st = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(route[0] == 0xF0 && route[6] == 0x37, "build cold: header present");
    CHECK(route[7] == XMOS_AUDIO_KEY_ROUTE, "build cold: route key");
    CHECK(route[8] == 0x02, "build cold: only monitor bit set");
    CHECK(route[XMOS_AUDIO_MSG_LEN - 1] == 0xF7, "build cold: terminated");
}

static void test_emit_round_trip(void) {
    xmos_audio_state_t src = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&src, 1, route, mon);

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 1, "emit: route accepted");
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, mon) == 1, "emit: mon accepted");

    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1, "round trip: emitted pair scans back as main out");
}

static void test_emit_respects_occupied_slots(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    /* Occupy the first three slots with LED-ish traffic. */
    for (int i = 0; i < 12; i += 4) {
        buf[i] = 0x0B; buf[i+1] = 0xB0; buf[i+2] = 0x10; buf[i+3] = 0x7F;
    }
    uint8_t snapshot[12];
    memcpy(snapshot, buf, sizeof snapshot);

    xmos_audio_state_t st = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 1, "emit: fits around occupied slots");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: occupied slots untouched");
}

static void test_emit_defers_when_full(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    /* Leave only 7 free slots — one short of a message. */
    for (int i = 0; i < MIDI_OUT_LEN - 28; i += 4) {
        buf[i] = 0x0B; buf[i+1] = 0xB0; buf[i+2] = 0x10; buf[i+3] = 0x7F;
    }
    uint8_t snapshot[MIDI_OUT_LEN];
    memcpy(snapshot, buf, sizeof snapshot);

    xmos_audio_state_t st = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 0, "emit: refuses when short on slots");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: buffer untouched on refusal");
}

int main(void) {
    test_main_out();
    test_mic();
    test_idempotent();
    test_ignores_led_sysex();
    test_rejects_truncated();
    test_interleaved_non_sysex();
    test_build_preserves_input_route_bit();
    test_build_without_observation();
    test_emit_round_trip();
    test_emit_respects_occupied_slots();
    test_emit_defers_when_full();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
```

Create `tests/host/test_xmos_audio.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_xmos_audio"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc/host \
  tests/host/test_xmos_audio.c \
  src/host/shadow_xmos_audio.c \
  -o "$bin"

"$bin"
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
chmod +x tests/host/test_xmos_audio.sh
bash tests/host/test_xmos_audio.sh
```

Expected: compile error — `src/host/shadow_xmos_audio.c: No such file or directory`.

- [ ] **Step 4: Write the implementation**

Create `src/host/shadow_xmos_audio.c`:

```c
/* shadow_xmos_audio.c - Move USB-C audio-out source (Mic / Main Out)
 *
 * Move's Settings menu exposes a USB-C audio-out source, but its firmware does
 * not persist it: every boot it reverts to Mic. Picking a value emits a pair of
 * XMOS audio-IO SysEx messages on MIDI_OUT within one SPI frame (captured on
 * hardware 2026-08-18):
 *
 *   Main Out:  F0 00 21 1D 01 01 37 12 02 00x12 F7
 *              F0 00 21 1D 01 01 37 14 01 00x12 F7
 *   Mic:       ...37 12 00...            ...37 14 00...
 *
 * `37 12` is the shared routing/monitoring TLV — bit0 selects the USB-C *input*
 * (owned by Move's sampling page, never ours to change), bit1 is monitoring.
 * `37 14` is the dedicated out-source bit. We observe both, persist the
 * preference, and re-assert it after boot.
 *
 * Everything here is pure buffer work — no I/O, no allocation, no locks — so
 * the SPI callback can call it and the host suite can compile it directly.
 */
#include <string.h>
#include "shadow_xmos_audio.h"

static const uint8_t XMOS_AUDIO_HDR[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };

/* USB-MIDI CIN -> SysEx payload byte count. 0 = not a SysEx packet. */
static int cin_payload_len(uint8_t cin) {
    switch (cin & 0x0F) {
    case 0x4: return 3;  /* start / continue */
    case 0x5: return 1;  /* end, 1 byte */
    case 0x6: return 2;  /* end, 2 bytes */
    case 0x7: return 3;  /* end, 3 bytes */
    default:  return 0;
    }
}

static int cin_is_end(uint8_t cin) {
    uint8_t c = cin & 0x0F;
    return c == 0x5 || c == 0x6 || c == 0x7;
}

static int envelope_valid(const uint8_t *buf, int len) {
    if (len != XMOS_AUDIO_MSG_LEN) return 0;
    if (memcmp(buf, XMOS_AUDIO_HDR, sizeof XMOS_AUDIO_HDR) != 0) return 0;
    return buf[XMOS_AUDIO_MSG_LEN - 1] == 0xF7;
}

int xmos_audio_scan(const uint8_t *midi_out, int len, xmos_audio_state_t *st) {
    /* Slightly oversized so an over-long SysEx overflows past 23 and is
     * rejected by the length check rather than aliasing onto a valid one. */
    uint8_t buf[XMOS_AUDIO_MSG_LEN + 8];
    int blen = 0;
    int active = 0;
    int changed = 0;

    for (int i = 0; i + 4 <= len; i += 4) {
        int n = cin_payload_len(midi_out[i]);
        if (n == 0) continue;  /* non-SysEx slot: skip without resetting */

        const uint8_t *p = &midi_out[i + 1];
        if (p[0] == 0xF0) { blen = 0; active = 1; }  /* data bytes are < 0x80 */
        if (!active) continue;

        for (int k = 0; k < n; k++)
            if (blen < (int)sizeof buf) buf[blen++] = p[k];

        if (!cin_is_end(midi_out[i])) continue;
        active = 0;
        if (!envelope_valid(buf, blen)) continue;

        if (buf[7] == XMOS_AUDIO_KEY_ROUTE) {
            memcpy(st->route, buf, XMOS_AUDIO_MSG_LEN);
            st->have_route = 1;
        } else if (buf[7] == XMOS_AUDIO_KEY_MON) {
            int8_t out = (buf[8] & 0x01) ? 1 : 0;
            if (st->usbc_out != out) {
                st->usbc_out = out;
                st->seq++;
                changed = 1;
            }
        }
    }
    return changed;
}

void xmos_audio_build(const xmos_audio_state_t *st, int usbc_out,
                      uint8_t out_route[XMOS_AUDIO_MSG_LEN],
                      uint8_t out_mon[XMOS_AUDIO_MSG_LEN]) {
    /* Reuse the route payload Move itself sent this boot so bit0 (USB-C input
     * select) stays whatever Move wants; flip only the monitoring bit. Fall
     * back to a bare envelope if Move hasn't spoken yet. */
    if (st->have_route) {
        memcpy(out_route, st->route, XMOS_AUDIO_MSG_LEN);
    } else {
        memset(out_route, 0, XMOS_AUDIO_MSG_LEN);
        memcpy(out_route, XMOS_AUDIO_HDR, sizeof XMOS_AUDIO_HDR);
        out_route[7] = XMOS_AUDIO_KEY_ROUTE;
        out_route[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
    }
    if (usbc_out) out_route[8] |= XMOS_AUDIO_ROUTE_BIT_MONITOR;
    else          out_route[8] &= (uint8_t)~XMOS_AUDIO_ROUTE_BIT_MONITOR;

    memset(out_mon, 0, XMOS_AUDIO_MSG_LEN);
    memcpy(out_mon, XMOS_AUDIO_HDR, sizeof XMOS_AUDIO_HDR);
    out_mon[7] = XMOS_AUDIO_KEY_MON;
    out_mon[8] = usbc_out ? 1 : 0;
    out_mon[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
}

int xmos_audio_emit(uint8_t *midi_out, int len, const uint8_t *msg) {
    int free_slots = 0;
    for (int i = 0; i + 4 <= len; i += 4)
        if (!midi_out[i] && !midi_out[i+1] && !midi_out[i+2] && !midi_out[i+3])
            free_slots++;
    if (free_slots < XMOS_AUDIO_PACKETS) return 0;

    int pos = 0, slot = 0;
    while (pos < XMOS_AUDIO_MSG_LEN) {
        while (slot + 4 <= len &&
               (midi_out[slot] || midi_out[slot+1] || midi_out[slot+2] || midi_out[slot+3]))
            slot += 4;
        if (slot + 4 > len) return 0;  /* unreachable after the count above */

        int remaining = XMOS_AUDIO_MSG_LEN - pos;
        uint8_t cin;
        int n;
        if (remaining > 3)       { cin = 0x04; n = 3; }
        else if (remaining == 3) { cin = 0x07; n = 3; }
        else if (remaining == 2) { cin = 0x06; n = 2; }
        else                     { cin = 0x05; n = 1; }

        midi_out[slot]     = cin;  /* cable 0, matching Move's own framing */
        midi_out[slot + 1] = msg[pos];
        midi_out[slot + 2] = n > 1 ? msg[pos + 1] : 0;
        midi_out[slot + 3] = n > 2 ? msg[pos + 2] : 0;
        pos += n;
        slot += 4;
    }
    return 1;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
bash tests/host/test_xmos_audio.sh
```

Expected: `PASS`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/host/shadow_xmos_audio.c src/host/shadow_xmos_audio.h \
        tests/host/test_xmos_audio.c tests/host/test_xmos_audio.sh
git commit -m "feat(xmos): SysEx codec for Move's USB-C audio-out source"
```

---

### Task 2: Observe and persist the setting

**Goal:** The shim notices when the user changes the setting in Move's menu and writes the preference to disk.

**Files:**
- Modify: `src/host/shim_worker.h` (add `shim_usbc_out_persist` extern)
- Modify: `src/host/shim_worker.c:27-45` (add read/write helpers), `:231-253` (persist in the loop)
- Modify: `src/schwung_shim.c:40-51` (include), and the pre-transfer callback right after the XMOS logger block ending at `:5035`
- Modify: `scripts/build.sh:238-243` (rebuild inputs), `:260-277` (compile list)

**Acceptance Criteria:**
- [ ] `./scripts/build.sh` compiles the shim with the new source, no warnings
- [ ] Toggling the setting on hardware writes `/data/UserData/schwung/usbc_out_state`
- [ ] The file contains `1` after selecting Main Out, `0` after selecting Mic
- [ ] No file I/O is added to the SPI callback path

**Verify:** `./scripts/build.sh` succeeds, then on device: toggle the setting and `ssh ableton@move.local 'cat /data/UserData/schwung/usbc_out_state'` → `1`

**Steps:**

- [ ] **Step 1: Add the source to the build**

In `scripts/build.sh`, in the `needs_rebuild build/schwung-shim.so` argument list, change the line reading:

```
    src/host/shadow_led_queue.c src/host/shadow_state.c \
```

to:

```
    src/host/shadow_led_queue.c src/host/shadow_state.c \
    src/host/shadow_xmos_audio.c src/host/shadow_xmos_audio.h \
```

In the same list, after `src/host/shadow_led_queue.h src/host/shadow_state.h \` no change is needed (the header is already listed above).

Then in the `"${CROSS_PREFIX}gcc"` invocation below it, after the line:

```
        src/host/shadow_state.c \
```

add:

```
        src/host/shadow_xmos_audio.c \
```

- [ ] **Step 2: Add the persistence helpers to the worker**

In `src/host/shim_worker.c`, after the `jack_state_write` function (ends at `:45`), add:

```c
/* USB-C audio-out source (0 = Mic, 1 = Main Out). Move's firmware forgets this
 * across reboots; we observe it on the wire and re-assert it after boot. */
#define USBC_OUT_STATE_PATH "/data/UserData/schwung/usbc_out_state"

static int usbc_out_state_read(void) {
    FILE *f = fopen(USBC_OUT_STATE_PATH, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v != 0 && v != 1) return -1;
    return v;
}

static void usbc_out_state_write(int v) {
    FILE *f = fopen(USBC_OUT_STATE_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}
```

Add the shared global near `shim_inject_boot_jack` at `:20`:

```c
volatile int shim_usbc_out_persist = -1;
```

- [ ] **Step 3: Declare it in the worker header**

In `src/host/shim_worker.h`, after the `shim_jack_persist` extern block, add:

```c
/* Last USB-C audio-out source seen by the RT path (0 = Mic, 1 = Main Out),
 * -1 until observed. Worker persists it on change and re-asserts it at boot —
 * Move's firmware reverts this to Mic on every reboot. */
extern volatile int shim_usbc_out_persist;
```

- [ ] **Step 4: Persist it in the worker loop**

In `src/host/shim_worker.c`, inside `worker_main`'s `for (;;)` loop, directly after the existing jack-persist block that ends with `jack_state_write(jp);` and its closing brace, add:

```c
        /* Persist the USB-C audio-out source when the RT path reports a change. */
        int up = shim_usbc_out_persist;
        if (up >= 0 && up != last_usbc_out) {
            last_usbc_out = up;
            usbc_out_state_write(up);
        }
```

And declare `last_usbc_out` alongside `last_persisted` above the loop (near `:227`):

```c
    int boot_usbc_out = usbc_out_state_read();  /* -1 if never persisted */
    int last_usbc_out = boot_usbc_out;
```

- [ ] **Step 5: Scan MIDI_OUT from the SPI callback**

In `src/schwung_shim.c`, add the include after `#include "host/shadow_state.h"` in the block at `:40-51`:

```c
#include "host/shadow_xmos_audio.h"
```

Then, in `shim_pre_transfer`, immediately after the closing brace of the `/* Log any MIDI packets on cable >= 3 ... */` block (which ends at `:5085`), add:

```c
    /* Observe Move's USB-C audio-out source (Mic / Main Out) so the worker can
     * persist it. Move's firmware forgets this across reboots; Task 3 replays
     * it. Pure buffer scan — no I/O, safe on the SPI thread. */
    if (xmos_audio_scan(shadow + MIDI_OUT_OFFSET, 80, &xmos_audio_observed))
        shim_usbc_out_persist = xmos_audio_observed.usbc_out;
```

The state lives at file scope (not inside this block) because Task 3's replay
also reads it, to preserve `37 12` bit0. Add it near the other shadow statics at
`:172-175`:

```c
/* Last-observed XMOS audio-IO state (USB-C out source + route payload).
 * Written only by the SPI callback. */
static xmos_audio_state_t xmos_audio_observed = { .usbc_out = -1 };
```

- [ ] **Step 6: Build and deploy**

```bash
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

Expected: build succeeds with no new warnings; install completes and restarts the service.

- [ ] **Step 7: Verify on hardware**

Toggle the setting on the Move to Main Out, then:

```bash
ssh ableton@move.local 'cat /data/UserData/schwung/usbc_out_state'
```

Expected: `1`. Switch to Mic and repeat — expected `0`.

- [ ] **Step 8: Commit**

```bash
git add src/schwung_shim.c src/host/shim_worker.c src/host/shim_worker.h scripts/build.sh
git commit -m "feat(xmos): observe and persist Move's USB-C audio-out source"
```

---

### Task 3: Replay at boot, and make the debug injector safe

**Goal:** Re-assert the persisted setting ~5 s after boot, and route the pre-existing `spi_sysex_inject` debug trigger through the same slot-safe emitter.

**Files:**
- Modify: `src/host/shim_worker.h` (add `shim_usbc_out_replay` extern)
- Modify: `src/host/shim_worker.c` (arm the replay in the boot block near `:246-251`)
- Modify: `src/schwung_shim.c:4921-4955` (replace the blind-write injector), plus the emit consumer in the pre-transfer callback

**Acceptance Criteria:**
- [ ] A device whose stored value is Mic emits nothing at boot
- [ ] A device whose stored value is Main Out emits both messages, one per frame, ~5 s after start
- [ ] The old `out[0..31]` blind write is gone; `spi_sysex_inject` uses `xmos_audio_emit`
- [ ] Emission defers rather than overwriting when MIDI_OUT is busy

**Verify:** `./scripts/build.sh` succeeds; on device with Main Out stored, `grep "USB-C out" /data/UserData/schwung/debug.log` shows the replay line after a reboot

**Steps:**

- [ ] **Step 1: Declare the replay handoff**

In `src/host/shim_worker.h`, after the `shim_usbc_out_persist` extern, add:

```c
/* Boot re-assert of the USB-C audio-out source: worker sets this to 0 or 1
 * ~5 s after start (Move's firmware is up and has sent its own default by
 * then); the RT consumer emits the SysEx pair and swaps it back to -1. */
extern volatile int shim_usbc_out_replay;
```

And define it in `src/host/shim_worker.c` beside the other globals near `:20`:

```c
volatile int shim_usbc_out_replay = -1;
```

- [ ] **Step 2: Arm the replay in the worker**

In `src/host/shim_worker.c`, inside the existing `if (!boot_reasserted && tick >= 25) { ... }` block, after the line `if (v >= 0) shim_inject_boot_jack = v;`, add:

```c
            /* Re-assert the USB-C audio-out source too. Skip entirely when the
             * stored value is Mic — that's Move's own boot default, so there is
             * nothing to correct and no reason to put SysEx on the wire. */
            if (boot_usbc_out == 1) shim_usbc_out_replay = 1;
```

- [ ] **Step 3: Replace the blind-write injector**

In `src/schwung_shim.c`, replace the entire block starting at the comment `/* SPI SysEx injection: send audio source change command to XMOS.` and ending with the closing brace after `shadow_log("SPI SysEx inject: audio source change sent");` (`:4921-4955`) with:

```c
    /* XMOS audio-IO SysEx emission — two producers, one slot-safe path.
     *
     * 1. Boot replay of the USB-C audio-out source (worker arms it ~5 s in).
     * 2. The spi_sysex_inject debug trigger (file content = 37 12 value byte).
     *
     * Both go through xmos_audio_emit, which only ever writes free MIDI_OUT
     * slots. The previous implementation blind-wrote out[0..31] regardless of
     * what Move had queued there; per docs, a stuck injection like that
     * hard-powered-off the device twice. One message per frame keeps at most 8
     * of the 20 slots busy, leaving headroom for Move's LED and knob traffic. */
    {
        static uint8_t pending[2][XMOS_AUDIO_MSG_LEN];
        static int pending_count = 0;  /* messages still to send */
        static int pending_next = 0;   /* index of the next one */

        if (pending_count == 0) {
            int replay = shim_usbc_out_replay;
            if (replay >= 0) {
                shim_usbc_out_replay = -1;
                xmos_audio_build(&xmos_audio_observed, replay, pending[0], pending[1]);
                pending_count = 2;
                pending_next = 0;
                shadow_log(replay ? "USB-C out: boot re-assert Main Out"
                                  : "USB-C out: boot re-assert Mic");
            } else if (shim_pending_sysex_inject >= 0) {
                int val_byte = shim_pending_sysex_inject;
                shim_pending_sysex_inject = -1;
                memset(pending[0], 0, XMOS_AUDIO_MSG_LEN);
                pending[0][0] = 0xF0; pending[0][1] = 0x00; pending[0][2] = 0x21;
                pending[0][3] = 0x1D; pending[0][4] = 0x01; pending[0][5] = 0x01;
                pending[0][6] = 0x37;
                pending[0][7] = XMOS_AUDIO_KEY_ROUTE;
                pending[0][8] = (uint8_t)val_byte;
                pending[0][XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
                pending_count = 1;
                pending_next = 0;
                shadow_log("SPI SysEx inject: audio source change queued");
            }
        }

        /* One message per frame; retry next frame if MIDI_OUT is too busy. */
        if (pending_count > 0 &&
            xmos_audio_emit(shadow + MIDI_OUT_OFFSET, 80, pending[pending_next])) {
            pending_next++;
            pending_count--;
        }
    }
```

- [ ] **Step 4: Build and deploy**

```bash
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

Expected: build succeeds, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add src/schwung_shim.c src/host/shim_worker.c src/host/shim_worker.h
git commit -m "feat(xmos): re-assert USB-C audio-out source at boot

Also replaces the spi_sysex_inject blind write to out[0..31] with the
slot-safe emitter — a stuck injection on that path previously
hard-powered-off the device."
```

---

### Task 4: Hardware verification

**Goal:** Prove the setting survives a real reboot, and that the replay lands after Move's own boot assert without disturbing anything else.

**Files:** none (verification only)

**Acceptance Criteria:**
- [ ] With Main Out stored, USB-C carries main out after a cold boot without touching Move's menu
- [ ] The captured boot log shows our pair *after* Move's `37 12` / `37 14` boot assert
- [ ] With Mic stored, no audio-IO SysEx is emitted by Schwung at boot
- [ ] Move's own sampling-page input source still toggles normally afterwards

**Verify:** boot capture in `/data/UserData/schwung/xmos_sysex.txt` shows Move's assert, then Schwung's pair, and audio is audibly correct over USB-C

**Steps:**

- [ ] **Step 1: Arm the boot capture**

```bash
ssh ableton@move.local 'cd /data/UserData/schwung && rm -f xmos_sysex.txt && touch log_xmos_sysex_on'
```

- [ ] **Step 2: Set Main Out and reboot**

On the Move: set USB-C out to Main Out. Confirm it persisted:

```bash
ssh ableton@move.local 'cat /data/UserData/schwung/usbc_out_state'
```

Expected `1`. Then power-cycle the Move (a full cycle, not `restart-move.sh` — per
`docs/plans/2026-06-11-codebase-cleanup-review.md` that script can wedge the SPI hook).

- [ ] **Step 3: Read the boot capture**

```bash
ssh ableton@move.local 'cat /data/UserData/schwung/xmos_sysex.txt'
```

Expected: Move's own `37 12 00` / `37 14 00` early (~0.6 s), then Schwung's
`37 12 02` / `37 14 01` in two consecutive frames around the 5 s mark, each in
its own frame.

- [ ] **Step 4: Confirm the audio**

Connect the Move over USB-C and confirm the computer receives main out, not the
mic, without opening Move's Settings menu. Note whether Move's own Settings
screen reads Main Out or Mic and record the answer in the plan — it decides
whether Task 5's UI item is worth building.

- [ ] **Step 5: Confirm the Mic path stays quiet**

Set the value to Mic, reboot, and re-read the capture. Expected: no `37 12` /
`37 14` from Schwung at all — only Move's own boot assert.

- [ ] **Step 6: Confirm no regression to input routing**

Open Move's sampling page and toggle the input source between analog and USB-C.
Expected: it still switches (our replay preserved bit0 rather than forcing it).

- [ ] **Step 7: Disarm and record results**

```bash
ssh ableton@move.local 'rm -f /data/UserData/schwung/log_xmos_sysex_on'
```

Append the boot capture to the spec's capture file and commit:

```bash
git add docs/superpowers/specs/2026-08-18-usbc-out-source-capture.txt
git commit -m "docs(xmos): add boot-replay capture from hardware verification"
```

---

### Deferred (not in this plan): Global Settings row

If a Schwung-side row is added later, it must **not** be a second Mic / Main Out
control. Two controls for one piece of state can disagree, and the user's
instruction was explicitly "let's not try to do anything fancy with move's
settings". Move's menu stays the only place the value is chosen.

The row is a switch over the *persistence behavior*, with the observed value as
a read-only annotation:

```
Persist USB-C Out    On     (currently: Main Out)
```

`On` / `Off` governs whether the boot replay fires at all; the parenthetical
just reports what was last seen on the wire. Nothing about it can contradict
Move.

Deferred because `GLOBAL_SETTINGS_SECTIONS` in `src/shadow/shadow_ui.js:939` is
a declarative schema (`bool`, `enum`, `float`, `int`) with no row type that
carries an annotation, and the Off case needs a settings-to-shim propagation
path that has not been traced. Task 4 Step 4 also records whether Move's own
Settings screen already reflects the replayed value, which affects whether the
annotation earns its place.

Open question if revisited: how a `GLOBAL_SETTINGS_SECTIONS` change reaches the
shim at runtime.

---

### Task 5: Documentation and PR

**Goal:** Update the docs the release checklist requires, feed the protocol finding back upstream, and open the PR.

**Files:**
- Modify: `CLAUDE.md`
- Modify: `../schwung-catalog-site/manual.html`
- Modify: `docs/superpowers/specs/2026-08-18-usbc-out-source-persist-design.md` (record the bit0-preservation refinement)

**Acceptance Criteria:**
- [ ] `CLAUDE.md` documents the behavior and the `usbc_out_state` file
- [ ] `manual.html` explains it in user-facing terms
- [ ] The spec records the refinement actually implemented, not the original payload-verbatim plan
- [ ] All three CI checks pass on the PR

**Verify:** `gh pr checks` → `host-tests`, `go`, `cross-compile` all green

**Steps:**

- [ ] **Step 1: Update CLAUDE.md**

Under the Shadow Mode section, add a subsection:

```markdown
### USB-C Audio-Out Source

Move's Settings menu picks what the computer receives over USB-C (Mic or Main
Out), but its firmware forgets the choice on every reboot. Schwung observes the
XMOS audio-IO SysEx Move emits (`F0 00 21 1D 01 01 37 12 <bits> …` plus
`37 14 <bit>`), persists it to `/data/UserData/schwung/usbc_out_state`, and
re-asserts it ~5 s after boot — skipping the emission entirely when the stored
value is Mic, which is Move's own default.

`37 12` bit0 is the USB-C *input* select, owned by Move's sampling page. The
replay reuses the payload Move sent this boot and flips only bit1, so the input
route is never changed behind Move's back. Impl: `src/host/shadow_xmos_audio.c`
(pure codec, unit-tested in `tests/host/test_xmos_audio.sh`), observed in
`schwung_shim.c`'s pre-transfer callback, persisted and armed in
`src/host/shim_worker.c`.
```

- [ ] **Step 2: Update the user manual**

In `../schwung-catalog-site/manual.html`, add a short paragraph in the audio
section: Move forgets the USB-C out source on reboot; Schwung remembers the last
value chosen in Move's Settings and restores it a few seconds after startup.

- [ ] **Step 3: Update the spec**

Add a note to the design doc's Design section recording that the implementation
persists a single preference and preserves `37 12` bit0 from the live payload,
rather than replaying both payloads verbatim, and why.

- [ ] **Step 4: Run the full local gate**

```bash
make -C tests/host test
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```

Expected: all green. (`tests/{shadow,store,build}` are not CI-gated and carry
~20 known-stale failures — do not treat those as regressions.)

- [ ] **Step 5: Commit and open the PR**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-08-18-usbc-out-source-persist-design.md
git commit -m "docs: USB-C audio-out source persistence"
git push -u origin usbc-out-persist
gh pr create --title "Persist Move's USB-C audio-out source across reboots" --body "$(cat <<'EOF'
Move's Settings menu picks what the computer receives over USB-C (Mic or Main
Out), but its firmware forgets the choice on every reboot. This observes the
SysEx Move emits, persists it, and re-asserts it ~5 s after boot.

The command was identified by live capture on hardware (bytes and method in
`docs/superpowers/specs/2026-08-18-usbc-out-source-capture.txt`). "Main Out" is
`37 12` bit1 plus `37 14` bit0 — the same bits vimana2-rust concluded were
unusable because they mute the speakers. That muting is the feature, not a side
effect: routing main out to USB-C engages the monitor path and the XMOS mutes
the speakers to prevent feedback. It also answers movesniff's open question Q2
on the meaning of sub `0x14`.

Also replaces the `spi_sysex_inject` debug path's blind write to `out[0..31]`
with a slot-safe emitter — a stuck injection on that path previously
hard-powered-off the device twice.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 6: Feed the finding upstream**

Note in the PR description (or as a follow-up) that
`movesniff-withdocs/docs/protocol-table.md` lists `0x14` as unreversed and
`findings-2026-04-09-sampling-page.md` Q2 as open, and that both are now
answered. Also worth telling the vimana2-rust side that
`schwung_ui.rs:356`'s "no known SysEx" comment and the `set_usbc_output` no-op
can be revisited.

---

## Notes for the implementer

- **Never write to `/tmp` on the device.** Root FS is ~463 MB and usually full. Everything goes under `/data/UserData/`.
- **Never scp individual files.** Deploy only via `./scripts/install.sh local --skip-modules --skip-confirmation`; it handles setuid, symlinks and the service restart.
- **Nothing new on the SPI callback path**: no `fopen`, no `unified_log`, no allocation. `shadow_log` is already used there and is safe; all file I/O belongs on the worker.
- **`main` is branch-protected.** Work stays on `usbc-out-persist`; merging requires all three CI checks.
- **Don't brute-force XMOS commands.** Everything emitted here is a payload Move itself produced or a single documented bit flip. Per `memory/hollow_audio_jack_boot.md`, guessing command bytes hard-powered-off the device twice.
