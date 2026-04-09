# Generic Display SHM + Touch Back-Channel Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Allow arbitrary standalone applications on Move to stream full-color framebuffers of any resolution to a remote browser viewer, with a touch/click coordinate back-channel from the viewer to the application.

**Architecture:** A new shared memory segment (`/dev/shm/schwung-display-generic`) with a self-describing header (width, height, pixel format, touch fields). The existing `display_server.c` gains WebSocket support — it reads the SHM and sends raw binary frames to the browser, and receives touch coordinates back on the same WebSocket. The browser viewer dynamically adapts to the resolution and color format. Touch events are written back into the SHM header for the producer app to poll.

**Tech Stack:** C (display_server), Go (schwung-manager proxy), HTML/JS (browser viewer), POSIX shared memory

---

## Task 1: Create the Generic Display SHM Header

**Files:**
- Create: `src/host/generic_display_shm.h`

**Step 1: Write the SHM header definition**

```c
#ifndef GENERIC_DISPLAY_SHM_H
#define GENERIC_DISPLAY_SHM_H

#include <stdint.h>

#define GENERIC_DISPLAY_MAGIC      "SCHWDSP1"
#define GENERIC_DISPLAY_SHM_PATH   "/dev/shm/schwung-display-generic"

/* Pixel formats (stored in format field) */
#define GENERIC_FMT_RGB888         "rgb888"      /* 3 bytes/pixel */
#define GENERIC_FMT_RGBA8888       "rgba8888"     /* 4 bytes/pixel */
#define GENERIC_FMT_GRAY8          "gray8"        /* 1 byte/pixel  */

/* Touch states */
#define GENERIC_TOUCH_NONE         0
#define GENERIC_TOUCH_DOWN         1
#define GENERIC_TOUCH_MOVE         2
#define GENERIC_TOUCH_UP           3

/* Staleness threshold */
#define GENERIC_STALE_MS           1500

/* Fixed header size (frame data follows immediately after) */
#define GENERIC_DISPLAY_HEADER_SIZE 96

typedef struct __attribute__((packed)) {
    /* Identity (offset 0) */
    char     magic[8];            /* "SCHWDSP1" */
    char     format[16];          /* "rgb888", "rgba8888", "gray8" */

    /* Timing (offset 24) */
    uint64_t last_update_ms;      /* CLOCK_MONOTONIC ms, set by producer */

    /* Versioning (offset 32) */
    uint32_t version;             /* 1 */
    uint32_t header_size;         /* GENERIC_DISPLAY_HEADER_SIZE */

    /* Dimensions (offset 40) */
    uint32_t width;               /* pixels */
    uint32_t height;              /* pixels */
    uint32_t bytes_per_pixel;     /* 1, 3, or 4 */
    uint32_t bytes_per_frame;     /* width * height * bytes_per_pixel */

    /* Frame sequencing (offset 56) */
    uint32_t frame_counter;       /* bumped after each frame write */
    uint8_t  active;              /* 1 = live, 0 = dormant */
    uint8_t  reserved1[3];

    /* Touch back-channel (offset 64) — display_server writes, app reads */
    uint32_t touch_x;            /* pixel coordinate */
    uint32_t touch_y;            /* pixel coordinate */
    uint32_t touch_state;        /* GENERIC_TOUCH_* */
    uint32_t touch_counter;      /* bumped on each new touch event */
    uint32_t touch_id;           /* multi-touch identifier (0 for single) */
    uint32_t touch_pressure;     /* 0-65535 (0 if not available) */

    uint8_t  reserved2[8];       /* pad to 96 bytes */

    /* Frame data follows: uint8_t frame[bytes_per_frame] */
} generic_display_shm_t;

/* Compile-time size check */
typedef char generic_header_size_check[
    (sizeof(generic_display_shm_t) == GENERIC_DISPLAY_HEADER_SIZE) ? 1 : -1];

#endif /* GENERIC_DISPLAY_SHM_H */
```

**Step 2: Add SHM name to shadow_constants.h**

In `src/host/shadow_constants.h`, add near the other SHM defines:

```c
#define SHM_DISPLAY_GENERIC "/schwung-display-generic" /* Generic app display */
```

**Step 3: Commit**

```bash
git add src/host/generic_display_shm.h src/host/shadow_constants.h
git commit -m "feat: add generic display SHM header for arbitrary app framebuffers"
```

---

## Task 2: Add WebSocket Support to display_server.c

The display server currently only speaks HTTP + SSE. We need to add a minimal WebSocket implementation for binary frame streaming and bidirectional touch events.

**Files:**
- Modify: `src/host/display_server.c`

**Step 1: Add WebSocket handshake and framing**

Add these constants and helpers after the existing base64 code:

```c
#include <openssl/sha.h>  /* For SHA-1 in WebSocket handshake */

/* Or if openssl is not available, use a minimal SHA-1 implementation.
   The WebSocket spec requires SHA-1 for the Sec-WebSocket-Accept header. */

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_OP_BINARY 0x02
#define WS_OP_TEXT   0x01
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A
```

Add a new stream mode:

```c
typedef enum {
    STREAM_MODE_NONE = 0,
    STREAM_MODE_LEGACY = 1,
    STREAM_MODE_AUTO = 2,
    STREAM_MODE_WS_GENERIC = 3,  /* WebSocket binary for generic display */
} stream_mode_t;
```

Add WebSocket state to client_t:

```c
typedef struct {
    int fd;
    stream_mode_t stream_mode;
    int needs_initial_frame;
    char buf[CLIENT_BUF_SIZE];
    int buf_len;
    /* WebSocket state */
    uint8_t ws_buf[8192];      /* reassembly buffer for incoming WS frames */
    int ws_buf_len;
} client_t;
```

**Step 2: Implement WebSocket handshake**

When the HTTP request is `GET /ws-generic`, perform the WebSocket upgrade:

```c
static int ws_handshake(int idx) {
    /* Parse Sec-WebSocket-Key from HTTP headers */
    char *key_start = strstr(clients[idx].buf, "Sec-WebSocket-Key: ");
    if (!key_start) return -1;
    key_start += 19;
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return -1;

    char ws_key[128];
    int key_len = key_end - key_start;
    if (key_len >= (int)sizeof(ws_key) - 37) return -1;
    memcpy(ws_key, key_start, key_len);
    memcpy(ws_key + key_len, WS_GUID, 36);
    ws_key[key_len + 36] = '\0';

    /* SHA-1 hash */
    uint8_t sha1[20];
    SHA1((unsigned char *)ws_key, key_len + 36, sha1);

    /* Base64 encode the SHA-1 */
    char accept[32];
    base64_encode(sha1, 20, accept);

    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);
    return write(clients[idx].fd, resp, rlen) > 0 ? 0 : -1;
}
```

**Step 3: Implement WebSocket frame send (binary, no mask)**

Server-to-client frames are unmasked per RFC 6455:

```c
static int ws_send_binary(int fd, const uint8_t *data, size_t len) {
    uint8_t header[10];
    int hlen;

    header[0] = 0x80 | WS_OP_BINARY;  /* FIN + binary opcode */
    if (len < 126) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        hlen = 4;
    } else {
        header[1] = 127;
        memset(header + 2, 0, 4);  /* high 32 bits = 0 */
        header[6] = (len >> 24) & 0xFF;
        header[7] = (len >> 16) & 0xFF;
        header[8] = (len >> 8) & 0xFF;
        header[9] = len & 0xFF;
        hlen = 10;
    }

    /* Use writev to send header + payload atomically */
    struct iovec iov[2] = {
        { header, hlen },
        { (void *)data, len }
    };
    return writev(fd, iov, 2) > 0 ? 0 : -1;
}
```

**Step 4: Implement WebSocket frame receive (for touch events)**

Client-to-server frames are masked per RFC 6455. Touch events arrive as small JSON text frames:

```c
/* Returns opcode, fills payload/payload_len. Returns -1 on error. */
static int ws_recv_frame(client_t *c, uint8_t *payload, int *payload_len, int max_len) {
    /* Read available data into ws_buf */
    int n = read(c->fd, c->ws_buf + c->ws_buf_len,
                 sizeof(c->ws_buf) - c->ws_buf_len);
    if (n <= 0) return -1;
    c->ws_buf_len += n;

    if (c->ws_buf_len < 2) return 0;  /* need more data */

    int opcode = c->ws_buf[0] & 0x0F;
    int masked = (c->ws_buf[1] >> 7) & 1;
    uint64_t plen = c->ws_buf[1] & 0x7F;
    int hdr_len = 2;

    if (plen == 126) {
        if (c->ws_buf_len < 4) return 0;
        plen = (c->ws_buf[2] << 8) | c->ws_buf[3];
        hdr_len = 4;
    } else if (plen == 127) {
        if (c->ws_buf_len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | c->ws_buf[2 + i];
        hdr_len = 10;
    }

    int mask_len = masked ? 4 : 0;
    int total = hdr_len + mask_len + (int)plen;
    if (c->ws_buf_len < total) return 0;  /* need more data */

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) memcpy(mask, c->ws_buf + hdr_len, 4);

    int copy = ((int)plen < max_len) ? (int)plen : max_len;
    for (int i = 0; i < copy; i++)
        payload[i] = c->ws_buf[hdr_len + mask_len + i] ^ mask[i % 4];
    *payload_len = copy;

    /* Shift remaining data */
    int remain = c->ws_buf_len - total;
    if (remain > 0) memmove(c->ws_buf, c->ws_buf + total, remain);
    c->ws_buf_len = remain;

    return opcode;
}
```

**Step 5: Add generic SHM mapping and the `/ws-generic` route**

In `main()`, add generic SHM mapping alongside Move/Norns:

```c
generic_display_shm_t *generic_shm_ptr = NULL;
int generic_shm_fd = -1;
long long last_generic_shm_attempt = 0;
size_t generic_shm_total_size = 0;
```

In the SHM retry loop:

```c
if (!generic_shm_ptr) {
    long long now = now_ms();
    if (now - last_generic_shm_attempt >= SHM_RETRY_MS) {
        last_generic_shm_attempt = now;
        generic_shm_fd = open(GENERIC_DISPLAY_SHM_PATH, O_RDWR);
        if (generic_shm_fd >= 0) {
            /* First map just the header to read dimensions */
            generic_display_shm_t hdr;
            if (read(generic_shm_fd, &hdr, sizeof(hdr)) == sizeof(hdr) &&
                memcmp(hdr.magic, GENERIC_DISPLAY_MAGIC, 8) == 0) {
                generic_shm_total_size = GENERIC_DISPLAY_HEADER_SIZE + hdr.bytes_per_frame;
                generic_shm_ptr = mmap(NULL, generic_shm_total_size,
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       generic_shm_fd, 0);
                if (generic_shm_ptr == MAP_FAILED) {
                    generic_shm_ptr = NULL;
                    close(generic_shm_fd);
                    generic_shm_fd = -1;
                } else {
                    LOG_INFO(DISPLAY_LOG_SOURCE, "opened generic display: %ux%u %s",
                             generic_shm_ptr->width, generic_shm_ptr->height,
                             generic_shm_ptr->format);
                }
            } else {
                close(generic_shm_fd);
                generic_shm_fd = -1;
            }
        }
    }
}
```

In `handle_http()`, add the WebSocket upgrade route:

```c
} else if (strncmp(clients[idx].buf, "GET /ws-generic", 15) == 0) {
    if (ws_handshake(idx) == 0) {
        clients[idx].stream_mode = STREAM_MODE_WS_GENERIC;
        clients[idx].needs_initial_frame = 1;
        clients[idx].ws_buf_len = 0;
        LOG_INFO(DISPLAY_LOG_SOURCE, "WebSocket generic client connected (slot %d)", idx);
    } else {
        client_remove(idx);
    }
}
```

**Step 6: Stream binary frames and handle touch in the main loop**

In the push-frames section, add generic WebSocket handling:

```c
/* Push generic display frames to WebSocket clients */
if (generic_shm_ptr && generic_shm_ptr->active &&
    memcmp(generic_shm_ptr->magic, GENERIC_DISPLAY_MAGIC, 8) == 0) {
    long long age = now - (long long)generic_shm_ptr->last_update_ms;
    if (age >= 0 && age <= GENERIC_STALE_MS) {
        static uint32_t last_generic_counter = 0;
        if (generic_shm_ptr->frame_counter != last_generic_counter) {
            /* Build a small header: 16 bytes of metadata + raw pixels */
            uint32_t w = generic_shm_ptr->width;
            uint32_t h = generic_shm_ptr->height;
            uint32_t bpp = generic_shm_ptr->bytes_per_pixel;
            uint32_t frame_size = generic_shm_ptr->bytes_per_frame;

            /* 16-byte binary header: [width:u32 LE][height:u32 LE][bpp:u32 LE][counter:u32 LE] */
            uint8_t bin_header[16];
            memcpy(bin_header + 0,  &w, 4);
            memcpy(bin_header + 4,  &h, 4);
            memcpy(bin_header + 8,  &bpp, 4);
            uint32_t ctr = generic_shm_ptr->frame_counter;
            memcpy(bin_header + 12, &ctr, 4);

            /* Send to all WS_GENERIC clients: header + frame data */
            /* Use a combined buffer or writev */
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd < 0 ||
                    clients[i].stream_mode != STREAM_MODE_WS_GENERIC) continue;

                /* Build single message: bin_header + frame pixels */
                size_t total = 16 + frame_size;
                /* For large frames, use a scratch buffer or send in two parts.
                   ws_send_binary uses writev internally, so we need a contiguous
                   buffer. For simplicity, use a static buffer up to a max size. */
                static uint8_t ws_frame_buf[2 * 1024 * 1024]; /* 2MB max */
                if (total <= sizeof(ws_frame_buf)) {
                    memcpy(ws_frame_buf, bin_header, 16);
                    memcpy(ws_frame_buf + 16,
                           ((uint8_t *)generic_shm_ptr) + GENERIC_DISPLAY_HEADER_SIZE,
                           frame_size);
                    if (ws_send_binary(clients[i].fd, ws_frame_buf, total) < 0)
                        client_remove(i);
                }
            }
            last_generic_counter = generic_shm_ptr->frame_counter;
        }
    }
}

/* Read touch events from WebSocket clients */
for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].fd < 0 ||
        clients[i].stream_mode != STREAM_MODE_WS_GENERIC) continue;

    uint8_t payload[256];
    int plen = 0;
    int op = ws_recv_frame(&clients[i], payload, &plen, sizeof(payload));
    if (op < 0) { client_remove(i); continue; }
    if (op == WS_OP_TEXT && plen > 0 && generic_shm_ptr) {
        /* Parse JSON: {"x":123,"y":456,"state":1} */
        payload[plen] = '\0';
        /* Minimal JSON parse — find x, y, state values */
        uint32_t tx = 0, ty = 0, ts = 0;
        char *p;
        if ((p = strstr((char *)payload, "\"x\":")) != NULL) tx = atoi(p + 4);
        if ((p = strstr((char *)payload, "\"y\":")) != NULL) ty = atoi(p + 4);
        if ((p = strstr((char *)payload, "\"state\":")) != NULL) ts = atoi(p + 8);
        generic_shm_ptr->touch_x = tx;
        generic_shm_ptr->touch_y = ty;
        generic_shm_ptr->touch_state = ts;
        __sync_synchronize();
        generic_shm_ptr->touch_counter++;
    } else if (op == WS_OP_PING) {
        /* Respond with pong */
        /* (simple: just echo payload with pong opcode) */
    } else if (op == WS_OP_CLOSE) {
        client_remove(i);
    }
}
```

**Step 7: Update build command**

In `scripts/build.sh`, add `-lssl -lcrypto` (or alternatively bundle a minimal SHA-1) to the display-server build:

```bash
"${CROSS_PREFIX}gcc" -g -O3 \
    src/host/display_server.c \
    src/host/unified_log.c \
    -o build/display-server \
    -Isrc -Isrc/host \
    -lrt -lssl -lcrypto
```

> **Note:** If OpenSSL is not available on the Move toolchain, use a bundled minimal SHA-1 implementation instead (~40 lines of C). This avoids a heavy dependency. The implementer should check if `openssl/sha.h` is available in the Docker cross-compile environment and fall back to a bundled SHA-1 if not.

**Step 8: Commit**

```bash
git add src/host/display_server.c scripts/build.sh
git commit -m "feat: add WebSocket support to display server for generic display streaming"
```

---

## Task 3: Add Generic Display WebSocket Route to schwung-manager

The Go web server needs to proxy `/ws-generic` to the display server, with WebSocket upgrade support.

**Files:**
- Modify: `schwung-manager/main.go`

**Step 1: Add WebSocket proxy route in hostRouter**

In the `hostRouter` function, add a case for `/ws-generic` before the schwungHandler fallthrough. The existing `displayProxy` won't properly handle WebSocket upgrades because `httputil.ReverseProxy` doesn't support them natively. Use a raw TCP proxy for the WebSocket connection:

```go
// /ws-generic → display server WebSocket (raw TCP proxy for upgrade)
if r.URL.Path == "/ws-generic" {
    wsProxyHandler(displayAddr, w, r, logger)
    return
}
```

**Step 2: Implement raw WebSocket proxy**

Add a new function that hijacks the HTTP connection and does a raw TCP tunnel:

```go
func wsProxyHandler(backendAddr string, w http.ResponseWriter, r *http.Request, logger *slog.Logger) {
    // Connect to backend
    backend, err := net.DialTimeout("tcp", backendAddr, 5*time.Second)
    if err != nil {
        logger.Error("ws proxy: backend connect failed", "err", err)
        http.Error(w, "Display server unavailable", http.StatusBadGateway)
        return
    }

    // Hijack the client connection
    hj, ok := w.(http.Hijacker)
    if !ok {
        backend.Close()
        http.Error(w, "WebSocket not supported", http.StatusInternalServerError)
        return
    }
    client, _, err := hj.Hijack()
    if err != nil {
        backend.Close()
        logger.Error("ws proxy: hijack failed", "err", err)
        return
    }

    // Forward the original HTTP request to the backend
    _ = r.Write(backend)

    // Bidirectional copy
    var wg sync.WaitGroup
    wg.Add(2)
    go func() {
        defer wg.Done()
        io.Copy(client, backend)
        client.Close()
    }()
    go func() {
        defer wg.Done()
        io.Copy(backend, client)
        backend.Close()
    }()
    wg.Wait()
}
```

**Step 3: Commit**

```bash
cd schwung-manager
git add main.go
git commit -m "feat: add WebSocket proxy route for generic display streaming"
```

---

## Task 4: Create the Browser Viewer

**Files:**
- Create: `schwung-manager/static/generic-display.html`
- Create: `schwung-manager/static/generic-display.js`

**Step 1: Create the HTML page**

```html
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<title>Generic Display</title>
<style>
  body {
    background: #000; margin: 0; display: flex; flex-direction: column;
    align-items: center; justify-content: center; height: 100vh;
    height: 100dvh; touch-action: none;
    user-select: none; -webkit-user-select: none;
    -webkit-touch-callout: none; overflow: hidden;
  }
  canvas {
    image-rendering: pixelated; image-rendering: crisp-edges;
    max-width: 100vw; max-height: 90vh;
    border: 2px solid #333; cursor: crosshair;
  }
  body.fs canvas { border: none; max-height: 100vh; }
  body.fs #status { display: none; }
  #status { color: #888; font: 12px monospace; margin-top: 8px; }
  #status.connected { color: #4a4; }
</style>
</head>
<body>
<canvas id="c"></canvas>
<div id="status">connecting... (tap canvas to fullscreen)</div>
<script src="/static/generic-display.js"></script>
</body>
</html>
```

**Step 2: Create the JavaScript**

```javascript
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
const statusEl = document.getElementById('status');

let ws = null;
let imgData = null;
let currentWidth = 0;
let currentHeight = 0;
let frames = 0;
let lastFpsTime = Date.now();

function resizeFS() {
  if (!document.body.classList.contains('fs')) {
    canvas.style.width = '';
    canvas.style.height = '';
    return;
  }
  const w = window.innerWidth, h = window.innerHeight;
  const aspect = currentWidth / currentHeight;
  if (w / h > aspect) {
    canvas.style.height = h + 'px';
    canvas.style.width = (h * aspect) + 'px';
  } else {
    canvas.style.width = w + 'px';
    canvas.style.height = (w / aspect) + 'px';
  }
}

canvas.addEventListener('click', function() {
  document.body.classList.toggle('fs');
  resizeFS();
});
window.addEventListener('resize', resizeFS);

/* --- Touch / Mouse → WebSocket --- */

function canvasCoords(e) {
  const rect = canvas.getBoundingClientRect();
  const scaleX = currentWidth / rect.width;
  const scaleY = currentHeight / rect.height;
  let clientX, clientY;
  if (e.touches) {
    clientX = e.touches[0].clientX;
    clientY = e.touches[0].clientY;
  } else {
    clientX = e.clientX;
    clientY = e.clientY;
  }
  return {
    x: Math.round((clientX - rect.left) * scaleX),
    y: Math.round((clientY - rect.top) * scaleY)
  };
}

function sendTouch(e, state) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  if (state === 2 && !e.touches && !(e.buttons & 1)) return; // ignore move without button
  const c = canvasCoords(e);
  ws.send(JSON.stringify({ x: c.x, y: c.y, state: state }));
  e.preventDefault();
}

canvas.addEventListener('mousedown',  e => sendTouch(e, 1));
canvas.addEventListener('mousemove',  e => sendTouch(e, 2));
canvas.addEventListener('mouseup',    e => sendTouch(e, 3));
canvas.addEventListener('touchstart', e => sendTouch(e, 1));
canvas.addEventListener('touchmove',  e => sendTouch(e, 2));
canvas.addEventListener('touchend',   e => { sendTouch(e, 3); });

/* --- WebSocket Frame Receive --- */

function handleFrame(arrayBuf) {
  const view = new DataView(arrayBuf);
  const w = view.getUint32(0, true);   // little-endian
  const h = view.getUint32(4, true);
  const bpp = view.getUint32(8, true);
  // const counter = view.getUint32(12, true);

  if (w !== currentWidth || h !== currentHeight) {
    currentWidth = w;
    currentHeight = h;
    canvas.width = w;
    canvas.height = h;
    imgData = ctx.createImageData(w, h);
    resizeFS();
  }

  const pixels = new Uint8Array(arrayBuf, 16);
  const d = imgData.data;

  if (bpp === 3) {
    // RGB888
    for (let i = 0, j = 0; i < w * h; i++, j += 3) {
      const di = i * 4;
      d[di]     = pixels[j];
      d[di + 1] = pixels[j + 1];
      d[di + 2] = pixels[j + 2];
      d[di + 3] = 255;
    }
  } else if (bpp === 4) {
    // RGBA8888 — direct copy
    d.set(pixels.subarray(0, w * h * 4));
  } else if (bpp === 1) {
    // Grayscale
    for (let i = 0; i < w * h; i++) {
      const di = i * 4;
      d[di] = d[di + 1] = d[di + 2] = pixels[i];
      d[di + 3] = 255;
    }
  }

  ctx.putImageData(imgData, 0, 0);

  // FPS counter
  frames++;
  const now = Date.now();
  if (now - lastFpsTime > 1000) {
    statusEl.textContent = `connected - ${w}x${h} ${bpp === 3 ? 'RGB' : bpp === 4 ? 'RGBA' : 'Gray'} - ${frames} fps`;
    frames = 0;
    lastFpsTime = now;
  }
}

/* --- Connect --- */

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws-generic`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    statusEl.textContent = 'connected';
    statusEl.className = 'connected';
  };

  ws.onmessage = (e) => {
    if (e.data instanceof ArrayBuffer) {
      handleFrame(e.data);
    }
  };

  ws.onclose = () => {
    statusEl.textContent = 'disconnected - reconnecting...';
    statusEl.className = '';
    setTimeout(connect, 2000);
  };

  ws.onerror = () => {
    ws.close();
  };
}

connect();
```

**Step 3: Serve the page from schwung-manager**

In `schwung-manager/main.go`, add a route in the schwung mux (near the other static/template routes):

```go
// Generic display viewer
mux.HandleFunc("/generic-display", func(w http.ResponseWriter, r *http.Request) {
    http.ServeFileFS(w, r, staticFS, "static/generic-display.html")
})
```

**Step 4: Add nav link**

In `schwung-manager/templates/base.html`, add near the existing "Screen Mirroring" link:

```html
<li><a href="/generic-display" target="_blank" rel="noopener">Generic Display</a></li>
```

**Step 5: Commit**

```bash
git add schwung-manager/static/generic-display.html schwung-manager/static/generic-display.js
git add schwung-manager/main.go schwung-manager/templates/base.html
git commit -m "feat: add browser viewer for generic display with touch input"
```

---

## Task 5: Create a Test Producer Application

A minimal C program that creates the SHM segment and writes test frames, to validate the pipeline end-to-end without needing the real standalone app.

**Files:**
- Create: `tools/generic-display-test.c`

**Step 1: Write the test producer**

```c
/*
 * generic-display-test.c - Test producer for generic display SHM
 *
 * Writes a moving gradient pattern at the specified resolution.
 * Usage: generic-display-test [width] [height] [fps]
 *        Defaults: 480 320 30
 *
 * Also monitors touch_counter and prints touch events to stdout.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "generic_display_shm.h"

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char *argv[]) {
    uint32_t width  = argc > 1 ? atoi(argv[1]) : 480;
    uint32_t height = argc > 2 ? atoi(argv[2]) : 320;
    int fps         = argc > 3 ? atoi(argv[3]) : 30;

    uint32_t bpp = 3;  /* RGB888 */
    uint32_t frame_size = width * height * bpp;
    size_t total_size = GENERIC_DISPLAY_HEADER_SIZE + frame_size;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Create SHM */
    int fd = open(GENERIC_DISPLAY_SHM_PATH, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        /* Try shm_open style path */
        fd = open("/dev/shm/schwung-display-generic", O_CREAT | O_RDWR, 0666);
    }
    if (fd < 0) { perror("open shm"); return 1; }
    if (ftruncate(fd, total_size) < 0) { perror("ftruncate"); return 1; }

    generic_display_shm_t *shm = mmap(NULL, total_size,
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    /* Init header */
    memcpy(shm->magic, GENERIC_DISPLAY_MAGIC, 8);
    strncpy(shm->format, GENERIC_FMT_RGB888, 16);
    shm->version = 1;
    shm->header_size = GENERIC_DISPLAY_HEADER_SIZE;
    shm->width = width;
    shm->height = height;
    shm->bytes_per_pixel = bpp;
    shm->bytes_per_frame = frame_size;
    shm->frame_counter = 0;
    shm->touch_counter = 0;
    shm->active = 1;

    uint8_t *frame = ((uint8_t *)shm) + GENERIC_DISPLAY_HEADER_SIZE;
    uint32_t last_touch = 0;
    int offset = 0;
    long long interval_ms = 1000 / fps;

    printf("Streaming %ux%u RGB888 @ %d fps (%zu bytes/frame)\n",
           width, height, fps, (size_t)frame_size);
    printf("SHM: %s\n", GENERIC_DISPLAY_SHM_PATH);

    while (running) {
        long long start = now_ms();

        /* Draw a moving gradient */
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t idx = (y * width + x) * 3;
                frame[idx + 0] = (x + offset) & 0xFF;       /* R */
                frame[idx + 1] = (y + offset / 2) & 0xFF;   /* G */
                frame[idx + 2] = (x + y + offset) & 0xFF;   /* B */
            }
        }
        __sync_synchronize();
        shm->frame_counter++;
        shm->last_update_ms = (uint64_t)now_ms();
        offset += 2;

        /* Check for touch events */
        if (shm->touch_counter != last_touch) {
            printf("Touch: x=%u y=%u state=%u\n",
                   shm->touch_x, shm->touch_y, shm->touch_state);
            last_touch = shm->touch_counter;
        }

        /* Sleep for remainder of frame interval */
        long long elapsed = now_ms() - start;
        if (elapsed < interval_ms)
            usleep((interval_ms - elapsed) * 1000);
    }

    shm->active = 0;
    munmap(shm, total_size);
    close(fd);
    unlink(GENERIC_DISPLAY_SHM_PATH);
    printf("Cleaned up.\n");
    return 0;
}
```

**Step 2: Add build command to build.sh**

In `scripts/build.sh`, after the display server build:

```bash
# Build generic display test tool
if needs_rebuild build/generic-display-test \
    tools/generic-display-test.c src/host/generic_display_shm.h; then
    echo "Building generic display test..."
    "${CROSS_PREFIX}gcc" -g -O2 \
        tools/generic-display-test.c \
        -o build/generic-display-test \
        -Isrc/host \
        -lrt
else
    echo "Skipping generic display test (up to date)"
fi
```

**Step 3: Commit**

```bash
git add tools/generic-display-test.c scripts/build.sh
git commit -m "feat: add test producer for generic display SHM"
```

---

## Task 6: End-to-End Test on Device

**Step 1: Build everything**

```bash
./scripts/build.sh
```

Verify no compilation errors.

**Step 2: Deploy to device**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Run the test producer on device**

```bash
ssh ableton@move.local "/data/UserData/schwung/generic-display-test 480 320 30"
```

**Step 4: Open the viewer in a browser**

Navigate to `http://schwung.local/generic-display`. Verify:
- Moving gradient pattern is visible
- Canvas resizes to 480x320
- FPS counter shows ~30fps
- Status shows "connected - 480x320 RGB"

**Step 5: Test touch back-channel**

Click/tap on the canvas. Verify the test producer prints touch events to stdout with correct pixel coordinates.

**Step 6: Test different resolutions**

```bash
# Kill previous, try larger
ssh ableton@move.local "/data/UserData/schwung/generic-display-test 640 480 20"
```

Verify the viewer adapts to the new resolution automatically.

**Step 7: Commit any fixes**

```bash
git add -A
git commit -m "fix: address issues found during end-to-end testing"
```

---

## Summary

| Task | What | Key files |
|------|------|-----------|
| 1 | SHM header definition | `src/host/generic_display_shm.h` |
| 2 | WebSocket in display_server | `src/host/display_server.c` |
| 3 | WS proxy in schwung-manager | `schwung-manager/main.go` |
| 4 | Browser viewer | `schwung-manager/static/generic-display.{html,js}` |
| 5 | Test producer | `tools/generic-display-test.c` |
| 6 | End-to-end test | Deploy + verify on hardware |

## Future Enhancements (Not In Scope)

- **JPEG compression**: If bandwidth becomes an issue at higher resolutions, add optional server-side JPEG encoding with a quality parameter
- **Multiple named displays**: Support multiple SHM segments with app-specific names
- **Multi-touch**: Extend touch back-channel with a ring buffer of touch events instead of single-slot
- **Dirty rectangles**: Only send changed regions to reduce bandwidth
