# Generic Display Protocol

Stream arbitrary full-color framebuffers from standalone applications running on Move to a remote browser viewer, with touch/click input back from the viewer to the application.

Unlike Display Mirror (which streams Move's 128x64 1-bit OLED), Generic Display supports any resolution and full RGB color.

## Architecture

```
Standalone App → SHM (/dev/shm/schwung-display-generic) → display_server → WebSocket → Browser
Browser touch events → WebSocket → display_server → SHM touch fields → App polls
```

### Key Files

| File | Purpose |
|------|---------|
| `src/host/generic_display_shm.h` | SHM header struct definition (96 bytes + frame data) |
| `src/host/display_server.c` | WebSocket streaming + touch receive |
| `schwung-manager/main.go` | WebSocket proxy (`/ws-generic` route) |
| `schwung-manager/static/generic-display.html` | Browser viewer page |
| `schwung-manager/static/generic-display.js` | WebSocket client, canvas renderer, touch input |
| `tools/generic-display-test.c` | Test producer (moving gradient pattern) |

## SHM Protocol

The producer creates `/dev/shm/schwung-display-generic` and writes a `generic_display_shm_t` header (96 bytes) followed by raw pixel data.

### Header Layout

```c
typedef struct __attribute__((packed)) {
    char     magic[8];            // "SCHWDSP1"
    char     format[16];          // "rgb888", "rgba8888", or "gray8"
    uint64_t last_update_ms;      // CLOCK_MONOTONIC ms
    uint32_t version;             // 1
    uint32_t header_size;         // 96
    uint32_t width;               // pixels
    uint32_t height;              // pixels
    uint32_t bytes_per_pixel;     // 1, 3, or 4
    uint32_t bytes_per_frame;     // width * height * bytes_per_pixel
    uint32_t frame_counter;       // bumped after each frame write
    uint8_t  active;              // 1 = live, 0 = dormant
    uint8_t  reserved1[3];
    // Touch back-channel (display_server writes, app reads)
    uint32_t touch_x;            // pixel coordinate
    uint32_t touch_y;            // pixel coordinate
    uint32_t touch_state;        // 0=none, 1=down, 2=move, 3=up
    uint32_t touch_counter;      // bumped on each new touch event
    uint32_t touch_id;           // multi-touch identifier (0 for single)
    uint32_t touch_pressure;     // 0-65535 (0 if not available)
    uint8_t  reserved2[8];
    // Frame data follows: uint8_t frame[bytes_per_frame]
} generic_display_shm_t;
```

### Supported Formats

| Format | `bytes_per_pixel` | Description |
|--------|-------------------|-------------|
| `rgb888` | 3 | Red, Green, Blue (8 bits each) |
| `rgba8888` | 4 | Red, Green, Blue, Alpha (8 bits each) |
| `gray8` | 1 | Grayscale (8 bits) |

### Producer Lifecycle

```c
#include "generic_display_shm.h"

// 1. Create and map SHM
int fd = open(GENERIC_DISPLAY_SHM_PATH, O_CREAT | O_RDWR, 0666);
size_t total = GENERIC_DISPLAY_HEADER_SIZE + (width * height * bpp);
ftruncate(fd, total);
generic_display_shm_t *shm = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

// 2. Initialize header
memcpy(shm->magic, GENERIC_DISPLAY_MAGIC, 8);
strncpy(shm->format, GENERIC_FMT_RGB888, 16);
shm->version = 1;
shm->header_size = GENERIC_DISPLAY_HEADER_SIZE;
shm->width = width;
shm->height = height;
shm->bytes_per_pixel = 3;
shm->bytes_per_frame = width * height * 3;
shm->active = 1;

// 3. Each frame:
uint8_t *frame = ((uint8_t *)shm) + GENERIC_DISPLAY_HEADER_SIZE;
memcpy(frame, my_pixels, shm->bytes_per_frame);
__sync_synchronize();
shm->frame_counter++;
shm->last_update_ms = now_ms();  // CLOCK_MONOTONIC

// 4. Read touch events:
if (shm->touch_counter != last_touch_counter) {
    handle_touch(shm->touch_x, shm->touch_y, shm->touch_state);
    last_touch_counter = shm->touch_counter;
}

// 5. Cleanup
shm->active = 0;
munmap(shm, total);
close(fd);
unlink(GENERIC_DISPLAY_SHM_PATH);
```

### Touch States

| Value | Constant | Description |
|-------|----------|-------------|
| 0 | `GENERIC_TOUCH_NONE` | No event |
| 1 | `GENERIC_TOUCH_DOWN` | Finger/mouse down |
| 2 | `GENERIC_TOUCH_MOVE` | Drag/move |
| 3 | `GENERIC_TOUCH_UP` | Finger/mouse up |

## WebSocket Wire Format

The display server streams frames to the browser as binary WebSocket messages.

### Server → Browser (binary)

Each message is a 16-byte header followed by raw pixel data:

```
[width:u32 LE][height:u32 LE][bpp:u32 LE][counter:u32 LE][pixel data...]
```

Total message size: `16 + width * height * bpp` bytes.

### Browser → Server (text JSON)

Touch events are sent as JSON text frames:

```json
{"x": 123, "y": 456, "state": 1}
```

Where `x` and `y` are pixel coordinates and `state` is a touch state value (1=down, 2=move, 3=up).

## Browser Viewer

Available at `http://schwung.local/generic-display`.

- Auto-connects via WebSocket and adapts to any resolution
- Click/tap on canvas sends touch coordinates to the application
- Click canvas to toggle fullscreen mode
- Status bar shows resolution, color format, and FPS

## Staleness and Remapping

The display server detects stale SHM segments (no updates for >3 seconds) and automatically unmaps and re-discovers. This handles producer restarts transparently — the viewer will briefly show no frames during the gap, then resume when the new producer starts writing.

## Test Producer

```bash
generic-display-test [width] [height] [fps]
# Defaults: 480 320 30
```

Writes a moving RGB gradient pattern and prints touch events to stdout. Useful for validating the full pipeline.

## Bandwidth

Raw frame sizes at 30 fps over LAN:

| Resolution | BPP | Frame Size | Bandwidth |
|-----------|-----|-----------|-----------|
| 480x320 | 3 | 450 KB | 13.5 MB/s |
| 640x480 | 3 | 900 KB | 27 MB/s |
| 480x320 | 4 | 600 KB | 18 MB/s |

No compression is used — frames are sent as raw pixels. For higher resolutions, consider adding JPEG compression server-side.
