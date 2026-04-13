# Ensoniq SD-1 32-Voice Module Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create a Schwung sound generator module that emulates the Ensoniq SD-1 32-voice synthesizer by combining standalone chip emulators (M68000 CPU, ES-5506 wavetable, ES-5510 effects DSP) with minimal glue code — no MAME framework dependency.

**Architecture:** A standalone M68000 CPU (Moira) runs the original SD-1 firmware ROM, which programs the ES-5506 wavetable chip (VGMPlay standalone) for 32-voice synthesis and the ES-5510 effects DSP (extracted from MAME, ~750 lines of core math) for reverb/chorus/delay. A ~150-line DUART stub handles MIDI input. Audio output is resampled from the chip's native rate (~31kHz) to 44.1kHz. The whole thing wraps in a Schwung plugin_api_v2 .so.

**Tech Stack:** C/C++, Moira (MIT M68000 emulator), VGMPlay ES-5506 (standalone C), MAME ES-5510 (extracted, BSD-3-Clause), libresample (from JV-880 module)

**Source repo:** `schwung-ensoniq-sd1` (new external module repo, like `schwung-jv880`)

**License:** GPL-2.0+ (inherited from MAME patches used in ES-5510 extraction; ES-5506 VGMPlay code is also MAME-derived GPL)

---

## Reference Materials

These were cloned/fetched during investigation and should be consulted during implementation:

| Resource | Location | What it contains |
|----------|----------|-----------------|
| VGMPlay ES-5506 | `/schwung-parent/vgmplay-legacy/VGMPlay/chips/es5506.c` + `.h` | Standalone wavetable chip (~2400 lines C) |
| MAME ES-5510 | `/schwung-parent/mame-es5510-ref/es5510.cpp` + `.h` | Effects DSP to extract (~1288 lines) |
| MAME esqpump | `/schwung-parent/mame-esqpump-ref/esqpump.cpp` + `.h` | Chip interconnect reference (153 lines) |
| Ensoniq SD-1 VST | `/schwung-parent/ensoniq-sd1-source/` | Board driver, ROM defs, MIDI path reference |
| Moira M68000 | https://github.com/dirkwhoffmann/Moira | MIT C++ CPU emulator (clone at build time) |
| ES-5510 datasheet | https://ccrma.stanford.edu/~dattorro/es5510searchable.pdf | Full instruction set reference |
| ES-5506 datasheet | https://dosdays.co.uk/media/ensoniq/ES5506%20-%20OTTO%20-%20Technical%20Specification.pdf | Wavetable chip spec |
| JV-880 module | `/schwung-parent/schwung-jv880/` | Reference for module structure, build, resample |
| Virus module | `/schwung-parent/schwung-virus/` | Reference for complex emulator module |

## SD-1 32-Voice Hardware Summary

```
M68000 CPU @ 15.238 MHz (30.47618 / 2)
  ├── 0x000000-0x00FFFF  Work RAM (64KB)
  ├── 0x200000-0x20001F  ES-5505 (OTIS) registers (note: SD-1 uses ES-5505, not 5506)
  ├── 0x260000-0x2601FF  ES-5510 (ESP) registers
  ├── 0x280000-0x28001F  MC68681 DUART (MIDI + panel)
  ├── 0x2C0000-0x2C0007  WD1772 floppy (stub)
  ├── 0x2E0000-0x2FFFFF  Cartridge ROM (optional)
  ├── 0x330000-0x37FFFF  Sequencer RAM (320KB)
  ├── 0xC00000-0xC3FFFF  OS ROM (256KB)
  └── 0xFF0000-0xFFFFFF  Work RAM high (mirror)

ES-5505 OTIS @ 15.238 MHz
  - 32 voices, wavetable playback with 4-pole filters
  - Waveform ROM: Bank 0 (u34+u35, 12-bit) + Bank 1 (u37+u38, 16-bit)
  - Nibble ROM: u36 (compressed waveform data)
  - IRQ → M68000 IPL1 (autovector)
  - 8 audio output channels → pump

ES-5510 ESP @ 10 MHz
  - 24-bit fixed-point DSP, 160 instructions/sample
  - Reverb, chorus, flanging, delay effects
  - Programs loaded by firmware via host registers
  - 1MB delay line DRAM
  - HALT controlled by DUART output port bit 6

Audio path: ES-5505 voices → pump (8ch) → ESP serial in → ESP program → ESP serial out (stereo)
```

## ROM Files Required

| Filename | Size | Purpose |
|----------|------|---------|
| `sd1_410_lo.bin` | 128KB | OS ROM low byte (v4.10) |
| `sd1_410_hi.bin` | 64KB | OS ROM high byte (v4.10) |
| `sd1_32_402_lo.bin` | 128KB | OS ROM low byte (v4.02, alternate) |
| `sd1_32_402_hi.bin` | 64KB | OS ROM high byte (v4.02, alternate) |
| `u34.bin` | 512KB | Waveform ROM bank 0 (12-bit, odd bytes) |
| `u35.bin` | 512KB | Waveform ROM bank 0 (12-bit, odd bytes) |
| `u36.bin` | 512KB | Nibble ROM (compressed waveforms) |
| `u37.bin` | 1MB | Waveform ROM bank 1 (16-bit, word-swapped) |
| `u38.bin` | 1MB | Waveform ROM bank 1 (16-bit, word-swapped) |
| `esqvfd_font_vfx.bin` | ? | VFD font (not needed for headless) |

---

## Task 1: Repository Setup and Skeleton

**Files:**
- Create: `schwung-ensoniq-sd1/` (new repo at same level as schwung-jv880)
- Create: `src/module.json`
- Create: `src/dsp/sd1_plugin.cpp` (skeleton)
- Create: `src/ui.js` (minimal sound generator UI)
- Create: `src/help.json`
- Create: `scripts/build.sh`
- Create: `scripts/Dockerfile`
- Create: `scripts/install.sh`
- Create: `release.json`
- Create: `CLAUDE.md`

**Step 1: Create repo directory structure**
```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent
mkdir -p schwung-ensoniq-sd1/{src/dsp,scripts,libs,dist/ensoniq-sd1}
cd schwung-ensoniq-sd1
git init
```

**Step 2: Create module.json**

```json
{
    "id": "ensoniq-sd1",
    "name": "Ensoniq SD-1",
    "abbrev": "SD1",
    "version": "0.1.0",
    "description": "Ensoniq SD-1 32-Voice wavetable synthesizer",
    "author": "MAME/VGMPlay (port: charlesvestal)",
    "license": "GPL-2.0+",
    "ui": "ui.js",
    "dsp": "dsp.so",
    "api_version": 2,
    "assets": {
        "path": "roms",
        "label": "ROMs",
        "extensions": [".bin"],
        "description": "Requires Ensoniq SD-1/32 ROM files (sd132 MAME romset)",
        "files": [
            { "filename": "sd1_410_lo.bin", "size": 131072, "required": true },
            { "filename": "sd1_410_hi.bin", "size": 65536, "required": true },
            { "filename": "u34.bin", "size": 524288, "required": true },
            { "filename": "u35.bin", "size": 524288, "required": true },
            { "filename": "u36.bin", "size": 524288, "required": true },
            { "filename": "u37.bin", "size": 1048576, "required": true },
            { "filename": "u38.bin", "size": 1048576, "required": true }
        ]
    },
    "capabilities": {
        "audio_out": true,
        "audio_in": false,
        "midi_in": true,
        "midi_out": false,
        "midi_channels": 12,
        "chainable": true,
        "component_type": "sound_generator"
    }
}
```

**Step 3: Create minimal ui.js**

Use the shared sound generator UI template (like the Virus module):
```javascript
import { createSoundGeneratorUI } from '/data/UserData/schwung/shared/sound_generator_ui.mjs';

const ui = createSoundGeneratorUI({
    moduleName: 'Ensoniq SD-1',
    onInit: (state) => {},
    onTick: (state) => {},
    showPolyphony: false,
    showOctave: true,
});

globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
```

**Step 4: Create skeleton plugin source**

Create `src/dsp/sd1_plugin.cpp` with the v2 API entry point, all functions stubbed to compile:

```cpp
#include "../../schwung/src/host/plugin_api_v1.h"
#include <string.h>
#include <stdlib.h>

static const host_api_v1_t *g_host = NULL;

typedef struct {
    char module_dir[512];
    char error_msg[256];
    int initialized;
} sd1_instance_t;

static void *v2_create_instance(const char *module_dir, const char *json_defaults) {
    sd1_instance_t *inst = (sd1_instance_t *)calloc(1, sizeof(sd1_instance_t));
    if (!inst) return NULL;
    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    // TODO: init chips
    return inst;
}

static void v2_destroy_instance(void *instance) {
    free(instance);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {}
static void v2_set_param(void *instance, const char *key, const char *val) {}
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) { return -1; }
static int v2_get_error(void *instance, char *buf, int buf_len) { return 0; }
static void v2_render_block(void *instance, int16_t *out, int frames) {
    memset(out, 0, frames * 2 * sizeof(int16_t));
}

static plugin_api_v2_t g_api;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_api.create_instance = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.on_midi = v2_on_midi;
    g_api.set_param = v2_set_param;
    g_api.get_param = v2_get_param;
    g_api.get_error = v2_get_error;
    g_api.render_block = v2_render_block;
    return &g_api;
}
```

**Step 5: Create build.sh, Dockerfile, install.sh**

Model after schwung-jv880. The Dockerfile needs:
- Ubuntu 22.04, aarch64 cross-compiler, cmake, ninja (for Moira)

**Step 6: Commit**
```bash
git add -A
git commit -m "feat: initial repo skeleton with module.json and plugin stub"
```

---

## Task 2: Integrate VGMPlay ES-5506 (OTIS)

The SD-1 actually uses the ES-5505 variant (not 5506). The VGMPlay code handles both via the `sndtype` flag — pass `clock | (0 << 31)` for ES-5505 mode.

**Files:**
- Copy: `vgmplay-legacy/VGMPlay/chips/es5506.c` → `libs/es5506/es5506.c`
- Copy: `vgmplay-legacy/VGMPlay/chips/es5506.h` → `libs/es5506/es5506.h`
- Create: `libs/es5506/mamedef.h` (minimal type shims)
- Modify: `src/dsp/sd1_plugin.cpp`

**Step 1: Copy VGMPlay ES-5506 files into libs/es5506/**

**Step 2: Create minimal mamedef.h**

```c
#ifndef MAMEDEF_H
#define MAMEDEF_H

#include <stdint.h>

typedef uint8_t  UINT8;
typedef int8_t   INT8;
typedef uint16_t UINT16;
typedef int16_t  INT16;
typedef uint32_t UINT32;
typedef int32_t  INT32;
typedef uint64_t UINT64;
typedef int64_t  INT64;
typedef UINT32   offs_t;
typedef INT32    stream_sample_t;

#define INLINE static inline
#define BYTE_XOR_BE(x) ((x) ^ 0x01)

typedef void (*SRATE_CALLBACK)(void*, UINT32);

/* Required by mamedef.h but not used by es5506 */
extern stream_sample_t* DUMMYBUF[];

#ifdef ES5506_DEBUG
#define logerror printf
#else
#define logerror(...)
#endif

#endif /* MAMEDEF_H */
```

**Step 3: Adapt es5506.c**

Key changes:
- Change `MAX_CHIPS` from 2 to 4 (support multiple instances)
- **OR better:** Refactor to heap-allocated instances (pass instance pointer instead of ChipID). This is optional for v0.1 since we likely only need 1 instance.
- Ensure the `channels` override is removed so we get the full multi-channel output needed for the pump/ESP routing
- Add IRQ callback support (the firmware needs ES-5505 IRQ to drive voice management)

**Step 4: Test standalone compilation**

Write a small test that:
1. Calls `device_start_es5506(0, clock, channels)` with ES-5505 mode
2. Loads a waveform ROM via `es5506_write_rom()`
3. Calls `es5506_update()` for a block
4. Verifies non-zero output (with ROM data loaded)

```bash
# Cross-compile test
${CROSS_PREFIX}gcc -c -fPIC libs/es5506/es5506.c -I libs/es5506/ -o build/es5506.o
```

**Step 5: Commit**
```bash
git add libs/es5506/
git commit -m "feat: add VGMPlay ES-5506 standalone wavetable chip"
```

---

## Task 3: Extract ES-5510 (ESP) from MAME

**Files:**
- Create: `libs/es5510/es5510.h` (standalone header)
- Create: `libs/es5510/es5510.c` (standalone implementation)

**Step 1: Create standalone es5510.h**

Extract from MAME's es5510.h, replacing the `cpu_device` class with a plain C struct:

```c
#ifndef ES5510_H
#define ES5510_H

#include <stdint.h>
#include <stdbool.h>

#define ES5510_ICOUNT 160    /* instructions per program */
#define ES5510_DRAM_SIZE (1 << 20)  /* 1M entries */

typedef struct {
    /* General purpose registers (192 x 24-bit, stored as int32) */
    int32_t gpr[0xC0];

    /* Instruction memory (160 x 48-bit, stored as uint64) */
    uint64_t instr[160];

    /* Delay line DRAM (1M x 16-bit) */
    int16_t *dram;

    /* Serial I/O (8 channels: 4 stereo pairs, 16-bit) */
    int16_t ser[8];

    /* MAC accumulator (48-bit) */
    int64_t machl;

    /* Address generators */
    int32_t dbase;      /* delay line base (circular pointer) */
    int32_t dlength;    /* delay line length */
    int32_t abase;      /* table A base */
    int32_t bbase;      /* table B base */

    /* Control */
    int32_t memsiz;     /* memory size config */
    int32_t sigreg;     /* signal register (controls mulshift) */
    int32_t ccr;        /* condition code register */
    int32_t cmr;        /* condition mask register */

    /* Memory addressing derived state */
    int32_t memshift;
    int32_t memmask;
    int32_t memincrement;
    int32_t mulshift;   /* 1 or 2, derived from sigreg bit 22 */

    /* DOL FIFO (2-deep) */
    int32_t dol[2];
    int dol_count;

    /* DIL (delay input latch) */
    int32_t dil;

    /* Pipeline state for RAM operations */
    struct {
        int32_t address;
        int io;         /* 0=delay, 1=table_a, 2=table_b, 3=io */
        int type;       /* 0=nop, 1=read, 2=write, 3=dump */
    } ram, ram_p, ram_pp;

    /* Host interface latches (for M68000 register access) */
    uint8_t gpr_latch;
    uint64_t instr_latch;
    int32_t dil_latch;
    int32_t dol_latch;
    int32_t dadr_latch;
    uint8_t ram_sel;
    uint32_t host_control;
    uint32_t host_serial;

    /* State */
    int pc;
    bool halted;

} es5510_state_t;

/* Lifecycle */
void es5510_init(es5510_state_t *chip);
void es5510_reset(es5510_state_t *chip);
void es5510_free(es5510_state_t *chip);

/* Run one complete program pass (one audio sample period) */
void es5510_run_once(es5510_state_t *chip);

/* Host register access (M68000 reads/writes to 0x260000-0x2601FF) */
uint8_t es5510_host_r(es5510_state_t *chip, uint32_t offset);
void es5510_host_w(es5510_state_t *chip, uint32_t offset, uint8_t data);

/* Serial audio I/O */
int16_t es5510_ser_r(es5510_state_t *chip, int offset);
void es5510_ser_w(es5510_state_t *chip, int offset, int16_t data);

/* Halt control */
void es5510_set_halted(es5510_state_t *chip, bool halted);

#endif /* ES5510_H */
```

**Step 2: Create standalone es5510.c**

Port from MAME's es5510.cpp. Keep:
- `execute_run()` → `es5510_run_once()` (set icount=160, run loop, halt at END)
- `alu_operation()` (all 16 ALU ops)
- `alu_operation_end()` (END instruction, DBASE update)
- `read_reg()` / `write_reg()` (register file access with special register mapping)
- `write_to_dol()` (DOL FIFO management)
- `host_r()` / `host_w()` (program/coefficient loading from M68000)
- `ser_r()` / `ser_w()` (audio sample I/O)
- Helper functions: `add()`, `saturate()`, `negate()`, `asl()`, flag ops
- Static tables: `ALU_OPS`, `OPERAND_SELECT`, `RAM_CONTROL`

Remove:
- All MAME includes and macros (`emu.h`, `DEFINE_DEVICE_TYPE`, `LOG`, etc.)
- `save_item()` / `save_pointer()` / `state_add()` calls
- `memory_space_config()`, `execute_clocks_to_cycles()`, etc.
- `create_disassembler()`, `list_program()`, all `DESCRIBE_*` functions
- `cpu_device` constructor chain

Replace:
- `util::sext(val, bits)` → inline `((int32_t)(val << (32 - bits))) >> (32 - bits)`
- `mul_32x32(a, b)` → `(int64_t)a * (int64_t)b`
- `BIT(val, bit)` → `((val >> bit) & 1)`
- `icount` mechanism → simple loop counter
- `logerror(...)` → conditional printf or no-op

**Step 3: Test standalone compilation**

```bash
${CROSS_PREFIX}gcc -c -fPIC libs/es5510/es5510.c -o build/es5510.o
```

**Step 4: Commit**
```bash
git add libs/es5510/
git commit -m "feat: extract ES-5510 ESP from MAME as standalone C"
```

---

## Task 4: Integrate Moira M68000

**Files:**
- Create: `libs/moira/` (git submodule or vendored copy)
- Create: `src/dsp/sd1_cpu.h` (SD-1 CPU wrapper)
- Create: `src/dsp/sd1_cpu.cpp` (memory map + device dispatch)

**Step 1: Add Moira as a dependency**

```bash
git submodule add https://github.com/dirkwhoffmann/Moira.git libs/moira
```

**Step 2: Create SD-1 CPU wrapper**

`src/dsp/sd1_cpu.h`:
```cpp
#pragma once
#include "../libs/moira/Moira/Moira.h"

// Forward declarations for chip state
struct es5510_state_t;

typedef struct {
    /* Memory */
    uint8_t *os_rom;        /* 256KB OS ROM (interleaved hi/lo) */
    uint8_t *work_ram;      /* 64KB work RAM */
    uint8_t *seq_ram;       /* 320KB sequencer RAM */

    /* Chip pointers (owned by sd1_instance, not here) */
    void *otis;             /* ES-5505 chip state (VGMPlay) */
    es5510_state_t *esp;    /* ES-5510 chip state */
    void *duart;            /* DUART state */

    /* Interrupt state */
    int pending_irq;        /* Highest pending IRQ level (0=none) */
} sd1_hw_t;

class SD1CPU : public moira::Moira {
public:
    sd1_hw_t *hw;

    SD1CPU();

    u8  read8(u32 addr) const override;
    u16 read16(u32 addr) const override;
    void write8(u32 addr, u8 val) const override;
    void write16(u32 addr, u16 val) const override;

    void sync(int cycles) override;
};
```

**Step 3: Implement memory map dispatch**

`src/dsp/sd1_cpu.cpp` — implement read8/16 and write8/16 based on the SD-1 32 memory map:

```
0x000000-0x00FFFF → work_ram (with FC check for ROM overlay in low 16KB)
0x200000-0x20001F → es5505 register read/write (es550x_r/es550x_w)
0x260000-0x2601FF → es5510 host_r/host_w
0x280000-0x28001F → duart read/write
0x2C0000-0x2C0007 → floppy stub (return 0)
0x2E0000-0x2FFFFF → cartridge stub (return 0xFF)
0x330000-0x37FFFF → seq_ram
0xC00000-0xC3FFFF → os_rom
0xFF0000-0xFFFFFF → work_ram (mirror of 0x000000)
```

**Step 4: Test compilation**
```bash
${CROSS_PREFIX}g++ -c -fPIC -std=c++17 src/dsp/sd1_cpu.cpp -I libs/moira/Moira/ -o build/sd1_cpu.o
```

**Step 5: Commit**
```bash
git add libs/moira src/dsp/sd1_cpu.*
git commit -m "feat: add Moira M68000 with SD-1 memory map"
```

---

## Task 5: MC68681 DUART Stub

**Files:**
- Create: `src/dsp/duart.h`
- Create: `src/dsp/duart.c`

**Step 1: Implement minimal DUART**

Only needs:
- Channel A RX FIFO (3 bytes) for MIDI input
- Status register A (RxRDY bit)
- Interrupt status/mask registers
- Interrupt vector register
- Output port register (for ESP halt control, analog mux)
- Command register A (RX enable/disable)
- Mode registers (absorb writes)
- Clock select (absorb writes)

```c
typedef struct {
    /* Channel A RX FIFO */
    uint8_t rx_fifo[3];
    uint8_t rx_count;
    uint8_t rx_enabled;

    /* Mode register pointer and storage */
    uint8_t mr_ptr;
    uint8_t mr[2];

    /* Interrupt */
    uint8_t imr;    /* interrupt mask */
    uint8_t ivr;    /* interrupt vector */

    /* Output port */
    uint8_t opr;

    /* Analog mux (low 3 bits of OPR) */
    uint8_t analog_sel;

} duart_state_t;
```

~150 lines covering read/write dispatch, `duart_rx_byte()`, `duart_irq_pending()`.

**Step 2: Wire to interrupt system**

When `duart_irq_pending()` returns true, assert M68000 IRQ level 3.
The firmware reads IVR (0x0C) during interrupt acknowledge to get vector 0x40.

**Step 3: Wire ESP halt**

OPR bit 6 controls ESP halt. In `duart_write()` for OPR set/reset, call `es5510_set_halted(esp, opr & 0x40)`.

**Step 4: Commit**
```bash
git add src/dsp/duart.*
git commit -m "feat: add MC68681 DUART stub for MIDI input"
```

---

## Task 6: Audio Pump (Chip Interconnect)

**Files:**
- Create: `src/dsp/sd1_pump.h`
- Create: `src/dsp/sd1_pump.c`

**Step 1: Implement the pump**

The pump is the audio glue between ES-5505 and ES-5510. Per sample:

```c
void sd1_pump_process_sample(sd1_pump_t *pump,
                             int32_t *otis_outputs,  /* 8 channels from ES-5505 */
                             es5510_state_t *esp,
                             bool esp_halted,
                             int16_t *out_main_lr,   /* stereo main output */
                             int16_t *out_aux_lr)    /* stereo aux (dry) output */
{
    /* Aux bypass: channels 0-1 straight through */
    out_aux_lr[0] = clamp16(otis_outputs[0]);
    out_aux_lr[1] = clamp16(otis_outputs[1]);

    /* Feed channels 2-7 into ESP serial inputs */
    es5510_ser_w(esp, 0, (int16_t)(otis_outputs[2] >> 0));  /* FX1 R */
    es5510_ser_w(esp, 1, (int16_t)(otis_outputs[3] >> 0));  /* FX1 L */
    es5510_ser_w(esp, 2, (int16_t)(otis_outputs[4] >> 0));  /* FX2 R */
    es5510_ser_w(esp, 3, (int16_t)(otis_outputs[5] >> 0));  /* FX2 L */
    es5510_ser_w(esp, 4, (int16_t)(otis_outputs[6] >> 0));  /* DRY R */
    es5510_ser_w(esp, 5, (int16_t)(otis_outputs[7] >> 0));  /* DRY L */

    /* Run ESP program */
    if (!esp_halted) {
        es5510_run_once(esp);
    }

    /* Read processed stereo from ESP serial outputs 6-7 */
    out_main_lr[0] = es5510_ser_r(esp, 6);
    out_main_lr[1] = es5510_ser_r(esp, 7);
}
```

~80 lines total.

**Step 2: Commit**
```bash
git add src/dsp/sd1_pump.*
git commit -m "feat: add audio pump connecting ES-5505 to ES-5510"
```

---

## Task 7: ROM Loading

**Files:**
- Create: `src/dsp/sd1_rom.h`
- Create: `src/dsp/sd1_rom.c`

**Step 1: Implement ROM loading**

Load and interleave ROMs per the MAME ROM definitions:

```c
/* OS ROM: 16-bit big-endian, interleaved hi/lo byte files */
/* sd1_410_hi.bin → even bytes (64KB) */
/* sd1_410_lo.bin → odd bytes (128KB, but only 64KB used for interleave) */
/* Result: 256KB (0x40000) 16-bit ROM */

/* Waveform ROM Bank 0: 12-bit, loaded as ROM_LOAD16_BYTE */
/* u34.bin → odd bytes at offset 0x000001, 512KB */
/* u35.bin → odd bytes at offset 0x100001, 512KB */
/* Total: 2MB region */

/* Waveform ROM Bank 1: 16-bit word-swapped */
/* u38.bin → word-swapped at offset 0x000000, 1MB */
/* u37.bin → word-swapped at offset 0x100000, 1MB */
/* Total: 2MB region */

/* Nibble ROM: u36.bin, 512KB, direct load */
```

Key: The VGMPlay ES-5506 uses `es5506_write_rom()` to load waveform data into its internal banks. We need to map the MAME ROM layout to VGMPlay's bank/region system.

**Step 2: Add ROM verification**

Check file existence and sizes. Set error message if ROMs are missing.

**Step 3: Commit**
```bash
git add src/dsp/sd1_rom.*
git commit -m "feat: add ROM loading and interleaving"
```

---

## Task 8: Resampler Integration

**Files:**
- Copy: `schwung-jv880/src/dsp/resample/` → `libs/resample/`
- Modify: `src/dsp/sd1_plugin.cpp`

**Step 1: Copy libresample from JV-880 module**

The ES-5505 native sample rate depends on the active voice count:
`sample_rate = master_clock / (16 * (active_voices + 1))`

For SD-1 32-voice: `15238090 / (16 * 33) = ~28,864 Hz`

This needs resampling to 44,100 Hz. Use the same libresample as JV-880 (which resamples from 64kHz).

**Step 2: Set up SRATE_CALLBACK**

The VGMPlay ES-5506 calls `SRATE_CALLBACK` when the active voice count changes. Use this to update the resample ratio dynamically.

**Step 3: Commit**
```bash
git add libs/resample/
git commit -m "feat: add libresample for native→44.1kHz conversion"
```

---

## Task 9: Full Plugin Integration (render_block)

**Files:**
- Modify: `src/dsp/sd1_plugin.cpp` (major rewrite)

**Step 1: Define the instance structure**

```cpp
typedef struct {
    /* Module path */
    char module_dir[512];
    char error_msg[256];

    /* CPU */
    SD1CPU *cpu;
    sd1_hw_t hw;

    /* Chips */
    uint8_t otis_chip_id;   /* VGMPlay chip ID (0) */
    es5510_state_t esp;
    duart_state_t duart;

    /* Memory */
    uint8_t os_rom[0x40000];      /* 256KB OS ROM */
    uint8_t work_ram[0x10000];    /* 64KB work RAM */
    uint8_t seq_ram[0x50000];     /* 320KB sequencer RAM */

    /* Audio */
    void *resample_state;
    int native_sample_rate;
    int16_t native_buf[4096];     /* native-rate render buffer */
    int native_buf_pos;
    int native_buf_avail;

    /* State */
    int initialized;
    int cycles_per_sample;        /* M68000 cycles per native audio sample */

} sd1_instance_t;
```

**Step 2: Implement create_instance**

1. Allocate instance
2. Load and interleave ROMs
3. Init ES-5505 via `device_start_es5506()`
4. Load waveform ROMs via `es5506_write_rom()`
5. Init ES-5510 via `es5510_init()`
6. Init DUART via `duart_init()`
7. Create Moira CPU, wire memory map to chips
8. Load OS ROM into memory, reset CPU
9. Init resampler
10. Run boot sequence (execute CPU for ~1 second of emulated time to let firmware initialize)

**Step 3: Implement render_block**

Per block (128 frames at 44100 Hz):
1. Calculate how many native samples we need (based on resample ratio)
2. For each native sample:
   a. Run M68000 for `cycles_per_sample` cycles
   b. Call `es5506_update()` for 1 sample → get 8-channel output
   c. Feed through pump → ESP → get stereo output
   d. Store in native buffer
3. Resample native buffer → 44100 Hz → output int16 interleaved stereo

**Step 4: Implement on_midi**

Convert 3-byte MIDI messages to individual bytes and feed to DUART:
```cpp
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    sd1_instance_t *inst = (sd1_instance_t *)instance;
    for (int i = 0; i < len; i++) {
        duart_rx_byte(&inst->duart, msg[i]);
    }
    /* Update IRQ state */
    if (duart_irq_pending(&inst->duart)) {
        inst->cpu->setIPL(3);
    }
}
```

**Step 5: Commit**
```bash
git add src/dsp/sd1_plugin.cpp
git commit -m "feat: full plugin integration with render_block"
```

---

## Task 10: Build System

**Files:**
- Modify: `scripts/build.sh`
- Modify: `scripts/Dockerfile`

**Step 1: Create Dockerfile**

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    make \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-
WORKDIR /build
```

**Step 2: Create build.sh**

Compile order:
1. `libs/es5506/es5506.c` → `es5506.o` (C, -Ofast)
2. `libs/es5510/es5510.c` → `es5510.o` (C, -Ofast)
3. `libs/resample/*.c` → `resample.o` etc. (C, -Ofast)
4. `src/dsp/duart.c` → `duart.o` (C, -Ofast)
5. `src/dsp/sd1_pump.c` → `pump.o` (C, -Ofast)
6. `src/dsp/sd1_rom.c` → `rom.o` (C, -Ofast)
7. Moira: compile `Moira.cpp` → `moira.o` (C++17, -Ofast)
8. `src/dsp/sd1_cpu.cpp` → `sd1_cpu.o` (C++17, -Ofast)
9. `src/dsp/sd1_plugin.cpp` → link all .o files into `dsp.so` (shared, -fPIC)

Flags: `-march=armv8-a -mtune=cortex-a72 -fno-exceptions -fno-rtti -DNDEBUG`

Package:
```bash
mkdir -p dist/ensoniq-sd1
cp src/module.json dist/ensoniq-sd1/
cp src/ui.js dist/ensoniq-sd1/
cp build/dsp.so dist/ensoniq-sd1/
mkdir -p dist/ensoniq-sd1/roms
cd dist && tar -czvf ensoniq-sd1-module.tar.gz ensoniq-sd1/ && cd ..
```

**Step 3: Test build**
```bash
./scripts/build.sh
```

**Step 4: Commit**
```bash
git add scripts/
git commit -m "feat: Docker cross-compile build system"
```

---

## Task 11: Boot Sequence and Timing

**Files:**
- Modify: `src/dsp/sd1_plugin.cpp`

This is the hardest part. The SD-1 firmware needs to boot and initialize all its subsystems before it can respond to MIDI. In the original hardware this takes a few seconds.

**Step 1: Implement boot pre-roll**

During `create_instance()`, after loading ROMs and resetting the CPU:
- Run the M68000 for ~2-3 seconds of emulated time
- This lets the firmware:
  - Initialize its internal data structures
  - Program the ES-5505 voice registers
  - Load the ESP effects program
  - Set up MIDI handlers
  - Load default preset bank

The boot must execute ES-5505 and ESP updates too (firmware writes to their registers), but we discard the audio output.

**Step 2: Handle the VFD panel**

The firmware sends panel display data via DUART Channel B. Our stub ignores Channel B, but the firmware might poll for panel responses. If boot hangs, we may need to stub panel ACK responses on Channel B.

Check the MAME esqpanel device to understand the expected serial protocol.

**Step 3: Handle timer/counter**

The DUART counter/timer may be used for timing during boot. If firmware reads counter registers (0x06/0x07), we may need to return incrementing values. Start with stubs returning 0; add timing if boot fails.

**Step 4: Verify boot success**

After pre-roll, verify:
- ES-5505 has voices programmed (read register state)
- ESP is not halted (DUART OPR bit 6 cleared)
- MIDI causes note output (inject a note-on, check for non-silent audio)

**Step 5: Commit**
```bash
git commit -m "feat: firmware boot sequence and pre-roll"
```

---

## Task 12: Chain Integration (ui_hierarchy + chain_params)

**Files:**
- Modify: `src/dsp/sd1_plugin.cpp` (get_param)
- Modify: `src/module.json` (capabilities)

**Step 1: Implement basic chain_params**

The SD-1 firmware handles presets internally — we can't easily expose individual synth parameters. Start with basic chain params:

```json
[
    {"key": "volume", "name": "Volume", "type": "int", "min": 0, "max": 127},
    {"key": "octave_transpose", "name": "Octave", "type": "int", "min": -3, "max": 3}
]
```

**Step 2: Implement ui_hierarchy for Shadow UI**

```json
{
    "levels": {
        "root": {
            "label": "SD-1",
            "knobs": ["volume"],
            "params": [
                {"key": "volume", "label": "Volume"},
                {"key": "octave_transpose", "label": "Octave"}
            ]
        }
    }
}
```

**Step 3: Implement set_param/get_param**

- `volume`: Send MIDI CC 7 to DUART
- `octave_transpose`: Offset note values in on_midi before feeding to DUART
- `all_notes_off`: Send CC 123 to DUART (for patch changes)

**Step 4: Commit**
```bash
git commit -m "feat: chain integration with ui_hierarchy and chain_params"
```

---

## Task 13: Testing and Iteration

**Step 1: Deploy to device**
```bash
./scripts/install.sh
```

**Step 2: Test on hardware**
- Load module via Module Store or manual install
- Check logs: `ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"`
- Play notes via MIDI
- Verify audio output
- Test in Signal Chain

**Step 3: Debug common issues**

Likely issues and fixes:
- **Boot hangs**: Panel serial protocol needs stubbing on DUART Channel B
- **No audio**: Check ROM interleaving, ES-5505 bank configuration, ESP halt state
- **Clicks/glitches**: Resample ratio wrong, or CPU cycle budget too low/high
- **Wrong pitch**: Clock frequency or voice count configuration
- **CPU overload**: Profile and optimize hot paths. Consider reducing M68000 execution frequency (skip cycles that aren't near audio events)

**Step 4: Performance optimization if needed**

If CPU usage is too high:
- Only run M68000 when there's pending work (MIDI input, timer IRQ, OTIS IRQ)
- Batch ES-5505 updates (render N samples at once instead of 1)
- Profile ES-5510: if the effects program is simple, it should be fast
- Consider child process isolation (like the Virus module) as last resort

---

## Task 14: Module Catalog and Release

**Files:**
- Modify: `schwung/module-catalog.json`
- Create: `release.json`
- Create: `.github/workflows/release.yml`

**Step 1: Add to module catalog**

```json
{
    "id": "ensoniq-sd1",
    "name": "Ensoniq SD-1",
    "description": "Ensoniq SD-1 32-Voice wavetable synthesizer",
    "author": "MAME/VGMPlay (port: charlesvestal)",
    "component_type": "sound_generator",
    "github_repo": "charlesvestal/schwung-ensoniq-sd1",
    "default_branch": "main",
    "asset_name": "ensoniq-sd1-module.tar.gz",
    "min_host_version": "0.3.11",
    "requires": "Ensoniq SD-1/32 ROM files (sd132 MAME romset)"
}
```

**Step 2: Create release.json**
```json
{
    "version": "0.1.0",
    "download_url": "https://github.com/charlesvestal/schwung-ensoniq-sd1/releases/download/v0.1.0/ensoniq-sd1-module.tar.gz"
}
```

**Step 3: Create release workflow**

Model after schwung-jv880's `.github/workflows/release.yml`.

**Step 4: Tag and release**
```bash
git tag v0.1.0
git push --tags
```

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Boot hangs (panel protocol) | Medium | Study MAME esqpanel, add Channel B stub responses |
| CPU too slow on Move | Low-Medium | Profile, batch rendering, reduce M68000 cycles |
| ROM interleaving wrong | Medium | Compare against MAME's ROM_LOAD macros carefully |
| ES-5505 IRQ timing wrong | Medium | Study MAME's IRQ handling, compare with VGMPlay callback |
| ES-5510 extraction bugs | Low | Test against MAME reference, compare output |
| Moira cycle accuracy issues | Very Low | Moira is well-tested, matches Musashi |
| Resample artifacts | Low | Use proven libresample from JV-880 |

## Estimated Effort

| Task | Effort |
|------|--------|
| Task 1: Repo skeleton | 1 hour |
| Task 2: ES-5506 integration | 3-4 hours |
| Task 3: ES-5510 extraction | 6-8 hours |
| Task 4: Moira + memory map | 3-4 hours |
| Task 5: DUART stub | 2-3 hours |
| Task 6: Audio pump | 1-2 hours |
| Task 7: ROM loading | 2-3 hours |
| Task 8: Resampler | 1-2 hours |
| Task 9: Plugin integration | 4-6 hours |
| Task 10: Build system | 2-3 hours |
| Task 11: Boot sequence | 4-8 hours (debugging) |
| Task 12: Chain integration | 2-3 hours |
| Task 13: Testing/debug | 8-16 hours |
| Task 14: Release | 1-2 hours |
| **Total** | **~35-65 hours** |

The wide range is mainly driven by Task 11 (boot debugging) and Task 13 (hardware testing). The chip emulations themselves are well-understood; the unknowns are in firmware behavior during boot and the panel serial protocol.
