/*
 * display_server.c - Live display SSE server
 *
 * Streams Move's 128x64 1-bit OLED to a browser via Server-Sent Events.
 * Reads /dev/shm/schwung-display-live (1024 bytes, written by the shim)
 * and pushes base64-encoded frames to connected browser clients at ~30 Hz.
 *
 * Usage: display-server [port]   (default port 7681)
 *
 * WHY A PAGE COULD FREEZE AND STAY FROZEN. Three ways, all fixed here and in
 * the /mirror page (schwung-manager/static/mirror.html):
 *
 *  - Frames are sent only when the screen CHANGES and the keepalive was an SSE
 *    comment, which EventSource never hands to JS. After a Wi-Fi blip that
 *    leaves a half-open TCP connection there is no FIN, so no `onerror`, and
 *    the page could not tell "still screen" from "dead link". The heartbeat is
 *    now a named event (`event: hb`) and the page rebuilds its connection when
 *    it stops arriving.
 *  - A non-200 answer (display-server restarting, slots full -> the manager's
 *    proxy says 502) makes EventSource give up PERMANENTLY -- readyState
 *    CLOSED, no retry -- while the old page still said "reconnecting". The
 *    page now reconnects itself.
 *  - Slots were refused when full, and a half-open connection keeps its slot
 *    until TCP gives up (minutes: a ping into a dead socket still "succeeds"
 *    into the kernel buffer). A new stream now EVICTS the oldest instead, so a
 *    reload always gets in.
 *
 * And every write to a stream used to be one non-blocking write(): EAGAIN
 * dropped the client, and a short write spliced half an event into the next
 * one. Output now goes through a per-client queue of WHOLE events (clients_out
 * below); a backed-up client skips frames rather than corrupting them, and is
 * resynced with a full snapshot once it drains.
 */

#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "norns_display_shm.h"
#include "e16_mirror_shm.h"
#include "surface_live_shm.h"
#include "audio_live_shm.h"
#include "unified_log.h"

#define DEFAULT_PORT       7681
#ifndef SHM_PATH
#define SHM_PATH           "/dev/shm/schwung-display-live"
#endif
#define DISPLAY_SIZE       1024
#ifndef NORNS_SHM_PATH
#define NORNS_SHM_PATH     "/dev/shm/schwung-norns-display-live"
#endif
#define MAX_CLIENTS        16
#define POLL_INTERVAL_MS   33    /* ~30 Hz */
#define SHM_RETRY_MS       2000
#define CLIENT_BUF_SIZE    4096
#define SSE_BUF_SIZE       7000
#define HEARTBEAT_MS       2000   /* `event: hb` to every stream; the page's watchdog keys on it */
#ifndef OUT_CAP
/* Per-client queue of whole events. Sized for SOUND: a listening client takes
 * ~240 KB/s of base64 PCM, so this is about a second of Wi-Fi stall before a
 * chunk is skipped. Static, and only the pages a client actually fills are
 * ever resident. */
#define OUT_CAP            262144
#endif
#define AUDIO_CHUNK_MAX    4096   /* frames per pcm event (93 ms); older is skipped */
#define AUDIO_START_BACK   1024   /* a first read starts this far behind the writer */
#ifndef STALL_DROP_MS
#define STALL_DROP_MS      15000  /* a queue that has not drained for this long is a dead peer */
#endif

#define DISPLAY_LOG_SOURCE "display_server"

/* Base64 encoding */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int len, char *out) {
    int i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = b64_table[(in[i] >> 2) & 0x3F];
        out[j++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
        out[j++] = b64_table[((in[i+1] & 0xF) << 2) | ((in[i+2] >> 6) & 0x3)];
        out[j++] = b64_table[in[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = b64_table[(in[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
            out[j++] = b64_table[((in[i+1] & 0xF) << 2)];
        } else {
            out[j++] = b64_table[(in[i] & 0x3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return j;
}

/* Client tracking */
typedef enum {
    STREAM_MODE_NONE = 0,
    STREAM_MODE_LEGACY = 1,
    STREAM_MODE_AUTO = 2,
    STREAM_MODE_E16 = 3,      /* /stream-e16: the OXI E16 mirror (e16_mirror_shm.h) */
} stream_mode_t;

typedef enum {
    AUTO_SOURCE_NONE = 0,
    AUTO_SOURCE_MOVE = 1,
    AUTO_SOURCE_NORNS = 2,
} auto_source_t;

typedef struct {
    int fd;
    stream_mode_t stream_mode;
    int needs_initial_frame;
    int extras;                    /* /stream-auto?v=2: also surface + e16 as named events */
    int audio;                     /* ...&audio=1: also `pcm` (audio_live_shm.h) */
    long long connected_ms;        /* eviction picks the oldest stream */
    char buf[CLIENT_BUF_SIZE];
    int buf_len;
    /* Whole events waiting for the socket. out_off..out_len is unsent. */
    char out[OUT_CAP];
    int out_len;
    int out_off;
    long long stalled_since;       /* 0 while the queue is empty */
    int resync;                    /* an event was skipped: send full state when drained */
} client_t;

static client_t clients[MAX_CLIENTS];
static volatile sig_atomic_t running = 1;

static void sighandler(int sig) { (void)sig; running = 0; }

/* Embedded HTML page */
static const char HTML_PAGE[] =
    "<!DOCTYPE html>\n"
    "<html><head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, "
        "maximum-scale=1, user-scalable=no\">\n"
    "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">\n"
    "<meta name=\"apple-mobile-web-app-status-bar-style\" content=\"black\">\n"
    "<title>Move Display</title>\n"
    "<style>\n"
    "  body { background: #000; margin: 0; display: flex; flex-direction: column;\n"
    "         align-items: center; justify-content: center; height: 100vh;\n"
    "         height: 100dvh; touch-action: manipulation;\n"
    "         user-select: none; -webkit-user-select: none;\n"
    "         -webkit-touch-callout: none; overflow: hidden; }\n"
    "  canvas { image-rendering: pixelated; image-rendering: crisp-edges;\n"
    "           width: 512px; height: 256px; border: 2px solid #333;\n"
    "           cursor: pointer; }\n"
    "  body.fs canvas { border: none; }\n"
    "  body.fs #status { display: none; }\n"
    "  #status { color: #888; font: 12px monospace; margin-top: 8px; }\n"
    "  #status.connected { color: #4a4; }\n"
    "  #e16 { display: none; margin-top: 18px; text-align: center; }\n"
    "  #e16.on { display: block; }\n"
    "  #e16 .label { color: #888; font: 12px monospace; margin-bottom: 6px; }\n"
    "  #e16c { width: 512px; height: 256px; }\n"
    "  #e16r { image-rendering: auto; width: 256px; height: 256px; border: none;\n"
    "          cursor: default; margin-left: 12px; }\n"
    "  body.fs #e16 { display: none; }\n"
    "</style>\n"
    "</head><body>\n"
    "<canvas id=\"c\" width=\"128\" height=\"64\"></canvas>\n"
    "<div id=\"status\">connecting... (tap to fullscreen)</div>\n"
    "<div id=\"e16\"><div class=\"label\">OXI E16 (as last sent)</div>\n"
    "<canvas id=\"e16c\" width=\"128\" height=\"64\"></canvas>"
    "<canvas id=\"e16r\" width=\"256\" height=\"256\"></canvas></div>\n"
    "<script>\n"
    "const canvas = document.getElementById('c');\n"
    "const ctx = canvas.getContext('2d');\n"
    "const statusEl = document.getElementById('status');\n"
    "const img = ctx.createImageData(128, 64);\n"
    "let frames = 0, lastFrame = Date.now(), lastMode = 'waiting';\n"
    "\n"
    "function resizeFS() {\n"
    "  if (!document.body.classList.contains('fs')) {\n"
    "    canvas.style.width = '512px'; canvas.style.height = '256px';\n"
    "    return;\n"
    "  }\n"
    "  var w = window.innerWidth, h = window.innerHeight;\n"
    "  if (w / h > 2) { canvas.style.height = h+'px'; canvas.style.width = (h*2)+'px'; }\n"
    "  else { canvas.style.width = w+'px'; canvas.style.height = (w/2)+'px'; }\n"
    "}\n"
    "canvas.addEventListener('click', function() {\n"
    "  document.body.classList.toggle('fs'); resizeFS();\n"
    "});\n"
    "window.addEventListener('resize', resizeFS);\n"
    "\n"
    "function drawMono(raw) {\n"
    "  const d = img.data;\n"
    "  for (let page = 0; page < 8; page++) {\n"
    "    for (let col = 0; col < 128; col++) {\n"
    "      const b = raw.charCodeAt(page * 128 + col);\n"
    "      for (let bit = 0; bit < 8; bit++) {\n"
    "        const y = page * 8 + bit;\n"
    "        const idx = (y * 128 + col) * 4;\n"
    "        const on = (b >> bit) & 1;\n"
    "        d[idx] = d[idx+1] = d[idx+2] = on ? 255 : 0;\n"
    "        d[idx+3] = 255;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n"
    "\n"
    "function drawGray4(raw) {\n"
    "  const d = img.data;\n"
    "  for (let y = 0; y < 64; y++) {\n"
    "    for (let x = 0; x < 128; x += 2) {\n"
    "      const b = raw.charCodeAt(y * 64 + (x >> 1));\n"
    "      const left = ((b >> 4) & 0x0f) * 17;\n"
    "      const right = (b & 0x0f) * 17;\n"
    "      let idx = (y * 128 + x) * 4;\n"
    "      d[idx] = d[idx+1] = d[idx+2] = left;\n"
    "      d[idx+3] = 255;\n"
    "      idx += 4;\n"
    "      d[idx] = d[idx+1] = d[idx+2] = right;\n"
    "      d[idx+3] = 255;\n"
    "    }\n"
    "  }\n"
    "}\n"
    "\n"
    "function updateStatus(mode) {\n"
    "  frames++;\n"
    "  lastMode = mode;\n"
    "  const now = Date.now();\n"
    "  if (now - lastFrame > 1000) {\n"
    "    statusEl.textContent = 'connected - ' + lastMode + ' - ' + frames + ' fps';\n"
    "    frames = 0;\n"
    "    lastFrame = now;\n"
    "  }\n"
    "}\n"
    "\n"
    "/* A half-open link fires no error and a non-200 closes EventSource for\n"
    "   good, so silence past 6 s (the server sends `hb` every 2 s) or a\n"
    "   CLOSED source rebuilds the connection. /mirror on the manager is the\n"
    "   full page; this one is kept for direct :7681 use. */\n"
    "let es = null, lastEvt = 0;\n"
    "setInterval(() => {\n"
    "  if (!es || es.readyState === 2 || Date.now() - lastEvt > 6000) connect();\n"
    "}, 1000);\n"
    "function connect() {\n"
    "  if (es) es.close();\n"
    "  lastEvt = Date.now();\n"
    "  es = new EventSource('/stream-auto');\n"
    "  es.addEventListener('hb', () => { lastEvt = Date.now(); });\n"
    "  es.onopen = () => {\n"
    "    lastEvt = Date.now();\n"
    "    statusEl.textContent = 'connected';\n"
    "    statusEl.className = 'connected';\n"
    "  };\n"
    "  es.onerror = () => {\n"
    "    statusEl.textContent = 'disconnected - reconnecting...';\n"
    "    statusEl.className = '';\n"
    "  };\n"
    "  es.onmessage = (e) => {\n"
    "    lastEvt = Date.now();\n"
    "    let payload;\n"
    "    try { payload = JSON.parse(e.data); } catch (_) { return; }\n"
    "    const raw = atob(payload.data || '');\n"
    "    if (payload.format === 'gray4' || payload.format === 'gray4_packed') {\n"
    "      drawGray4(raw);\n"
    "    } else {\n"
    "      drawMono(raw);\n"
    "    }\n"
    "    ctx.putImageData(img, 0, 0);\n"
    "    updateStatus(payload.source || payload.format || 'display');\n"
    "  };\n"
    "}\n"
    "connect();\n"
    "\n"
    "/* THE E16 MIRROR: the screen we last sent it and the last ring per knob,\n"
    "   from /stream-e16. Hidden while no E16 is live. */\n"
    "const e16El = document.getElementById('e16');\n"
    "const e16c = document.getElementById('e16c').getContext('2d');\n"
    "const e16r = document.getElementById('e16r').getContext('2d');\n"
    "const e16img = e16c.createImageData(128, 64);\n"
    "function e16Screen(raw) {\n"
    "  const d = e16img.data;\n"
    "  for (let page = 0; page < 8; page++) for (let col = 0; col < 128; col++) {\n"
    "    const b = raw.charCodeAt(page * 128 + col);\n"
    "    for (let bit = 0; bit < 8; bit++) {\n"
    "      const idx = ((page * 8 + bit) * 128 + col) * 4;\n"
    "      const on = (b >> bit) & 1;\n"
    "      d[idx] = d[idx + 1] = d[idx + 2] = on ? 255 : 0; d[idx + 3] = 255;\n"
    "    }\n"
    "  }\n"
    "  e16c.putImageData(e16img, 0, 0);\n"
    "}\n"
    "function e16Rings(raw) {\n"
    "  const W = 256, cell = W / 4, R = cell * 0.36;\n"
    "  e16r.fillStyle = '#000'; e16r.fillRect(0, 0, W, W);\n"
    "  for (let e = 0; e < 16; e++) {\n"
    "    const o = e * 6;\n"
    "    const r = raw.charCodeAt(o), g = raw.charCodeAt(o + 1), b = raw.charCodeAt(o + 2);\n"
    "    const amt = ((raw.charCodeAt(o + 3) << 8) | raw.charCodeAt(o + 4)) / 16383;\n"
    "    const bip = raw.charCodeAt(o + 5);\n"
    "    const cx = (e % 4) * cell + cell / 2, cy = Math.floor(e / 4) * cell + cell / 2;\n"
    "    const col = 'rgb(' + Math.min(255, r * 2) + ',' + Math.min(255, g * 2) + ',' + Math.min(255, b * 2) + ')';\n"
    "    const lit = r || g || b;\n"
    "    const start = Math.PI * 0.75, span = Math.PI * 1.5;\n"
    "    e16r.lineWidth = 6; e16r.lineCap = 'round';\n"
    "    e16r.strokeStyle = '#1a1a1a';\n"
    "    e16r.beginPath(); e16r.arc(cx, cy, R, start, start + span); e16r.stroke();\n"
    "    if (lit) {\n"
    "      e16r.strokeStyle = col;\n"
    "      e16r.beginPath();\n"
    "      if (bip) { const mid = start + span / 2, to = start + span * amt;\n"
    "        if (to >= mid) e16r.arc(cx, cy, R, mid, Math.max(mid + 0.02, to)); else e16r.arc(cx, cy, R, to, mid); }\n"
    "      else e16r.arc(cx, cy, R, start, start + Math.max(0.02, span * amt));\n"
    "      e16r.stroke();\n"
    "    }\n"
    "    e16r.fillStyle = '#555'; e16r.font = '10px monospace'; e16r.textAlign = 'center';\n"
    "    e16r.fillText(String(e + 1), cx, cy + 4);\n"
    "  }\n"
    "}\n"
    "function connectE16() {\n"
    "  const es = new EventSource('/stream-e16');\n"
    "  es.onmessage = (e) => {\n"
    "    let p; try { p = JSON.parse(e.data); } catch (_) { return; }\n"
    "    if (!p.active) { e16El.className = ''; return; }\n"
    "    e16El.className = 'on';\n"
    "    if (p.hasFrame) e16Screen(atob(p.data || ''));\n"
    "    else { e16c.fillStyle = '#000'; e16c.fillRect(0, 0, 128, 64); }\n"
    "    e16Rings(atob(p.rings || ''));\n"
    "  };\n"
    "}\n"
    "connectE16();\n"
    "</script>\n"
    "</body></html>\n";

/* Get monotonic time in milliseconds */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int norns_frame_is_live(const norns_display_shm_t *shm, long long now) {
    long long age_ms;

    if (!shm) return 0;
    if (memcmp(shm->magic, NORNS_DISPLAY_MAGIC, sizeof(shm->magic)) != 0) return 0;
    if (strncmp(shm->format, NORNS_DISPLAY_FORMAT, sizeof(shm->format)) != 0) return 0;
    if (shm->version != 1) return 0;
    if (shm->header_size != sizeof(norns_display_shm_t) - NORNS_FRAME_SIZE) return 0;
    if (shm->width != 128 || shm->height != 64) return 0;
    if (shm->bytes_per_frame != NORNS_FRAME_SIZE) return 0;
    if (shm->active != 1) return 0;
    if (shm->last_update_ms == 0) return 0;
    age_ms = now - (long long)shm->last_update_ms;
    return age_ms >= 0 && age_ms <= NORNS_STALE_MS;
}

/*
 * The E16 mirror payload: {"active":1,"hasFrame":..,"data":b64,"rings":b64}
 * while the surface is live, {"active":0} otherwise. A frame is copied only
 * between two equal, EVEN reads of the sequence counter (the writer makes it
 * odd while writing), so a half-written frame is never sent. Returns the
 * length written to `out`, or -1 when the read raced a write (try later).
 */
static int e16_mirror_payload(const e16_mirror_shm_t *m, long long now, char *out, size_t cap) {
    if (!m || memcmp(m->magic, E16_MIRROR_MAGIC, 7) != 0 || m->version != 1 || !m->active ||
        now - (long long)m->last_update_ms > E16_MIRROR_STALE_MS || now < (long long)m->last_update_ms)
        return snprintf(out, cap, "{\"active\":0}");
    static uint8_t frame[E16_MIRROR_FRAME_SIZE];
    static uint8_t rings[E16_MIRROR_RINGS * E16_MIRROR_RING_BYTES];
    static char fb64[E16_MIRROR_FRAME_SIZE * 2], rb64[E16_MIRROR_RINGS * E16_MIRROR_RING_BYTES * 2];
    uint32_t s1 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1u) return -1;
    __sync_synchronize();
    uint8_t has = m->has_frame;
    memcpy(frame, m->frame, sizeof frame);
    memcpy(rings, m->rings, sizeof rings);
    __sync_synchronize();
    if (__atomic_load_n(&m->seq, __ATOMIC_ACQUIRE) != s1) return -1;
    (void)base64_encode(frame, (int)sizeof frame, fb64);
    (void)base64_encode(rings, (int)sizeof rings, rb64);
    return snprintf(out, cap, "{\"active\":1,\"hasFrame\":%d,\"data\":\"%s\",\"rings\":\"%s\"}",
                    has ? 1 : 0, fb64, rb64);
}

/* Close and clear a client slot */
static void client_remove(int idx) {
    if (clients[idx].fd >= 0) {
        if (clients[idx].stream_mode != STREAM_MODE_NONE)
            LOG_INFO(DISPLAY_LOG_SOURCE, "SSE client disconnected (slot %d)", idx);
        close(clients[idx].fd);
    }
    clients[idx].fd = -1;
    clients[idx].stream_mode = STREAM_MODE_NONE;
    clients[idx].buf_len = 0;
    clients[idx].out_len = clients[idx].out_off = 0;
    clients[idx].stalled_since = 0;
    clients[idx].resync = 0;
    clients[idx].extras = 0;
    clients[idx].audio = 0;
}

/* Push what is queued. Returns -1 when the client was removed. */
static int client_flush(int idx, long long now) {
    client_t *c = &clients[idx];
    while (c->out_off < c->out_len) {
        ssize_t n = write(c->fd, c->out + c->out_off, (size_t)(c->out_len - c->out_off));
        if (n > 0) { c->out_off += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        client_remove(idx);
        return -1;
    }
    if (c->out_off >= c->out_len) {
        c->out_off = c->out_len = 0;
        c->stalled_since = 0;
    } else {
        if (!c->stalled_since) c->stalled_since = now;
        if (now - c->stalled_since > STALL_DROP_MS) {
            LOG_INFO(DISPLAY_LOG_SOURCE, "stream client stalled %d ms, dropping (slot %d)",
                     STALL_DROP_MS, idx);
            client_remove(idx);
            return -1;
        }
    }
    return 0;
}

/* Queue ONE whole event and try to send it. An event that does not fit behind
 * what is already queued is skipped, never truncated: every event here is a
 * complete snapshot, so the client loses a frame, and `resync` makes the next
 * drained pass send the current state. Returns -1 when the client was removed. */
static int client_send(int idx, const char *data, int len, long long now) {
    client_t *c = &clients[idx];
    if (c->fd < 0) return -1;
    if (len <= 0 || len > OUT_CAP) return 0;
    if (c->out_off > 0 && c->out_off == c->out_len) c->out_off = c->out_len = 0;
    if (c->out_off > 0 && c->out_len + len > OUT_CAP) {
        memmove(c->out, c->out + c->out_off, (size_t)(c->out_len - c->out_off));
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if (c->out_len + len > OUT_CAP) {
        c->resync = 1;
        return client_flush(idx, now);
    }
    memcpy(c->out + c->out_len, data, (size_t)len);
    c->out_len += len;
    return client_flush(idx, now);
}

/* Send a complete HTTP response and close */
static void send_response(int idx, int code, const char *ctype,
                          const char *body, int body_len) {
    const char *status = (code == 200) ? "OK" : "Not Found";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status, ctype, body_len);

    /* Best-effort send; ignore errors */
    (void)write(clients[idx].fd, header, hlen);
    (void)write(clients[idx].fd, body, body_len);
    client_remove(idx);
}

/* Handle an HTTP request */
static void handle_http(int idx) {
    clients[idx].buf[clients[idx].buf_len] = '\0';

    if (strncmp(clients[idx].buf, "GET /stream-e16", 15) == 0) {
        const char *sse_header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        if (write(clients[idx].fd, sse_header, strlen(sse_header)) > 0) {
            clients[idx].stream_mode = STREAM_MODE_E16;
            clients[idx].needs_initial_frame = 1;
            clients[idx].connected_ms = now_ms();
            LOG_INFO(DISPLAY_LOG_SOURCE, "e16 SSE client connected (slot %d)", idx);
        } else {
            client_remove(idx);
        }
    } else if (strncmp(clients[idx].buf, "GET /stream-auto", 16) == 0) {
        /* ?v=2: the /mirror page's single connection. Old clients ask for the
         * bare path and see exactly the stream they always did (plus an
         * `event: hb` they do not listen for). */
        const char *eol = strstr(clients[idx].buf, "\r\n");
        const char *v2 = strstr(clients[idx].buf, "v=2");
        clients[idx].extras = (v2 && eol && v2 < eol) ? 1 : 0;
        const char *au = strstr(clients[idx].buf, "audio=1");
        clients[idx].audio = (clients[idx].extras && au && eol && au < eol) ? 1 : 0;
        const char *sse_header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        if (write(clients[idx].fd, sse_header, strlen(sse_header)) > 0) {
            clients[idx].stream_mode = STREAM_MODE_AUTO;
            clients[idx].needs_initial_frame = 1;
            clients[idx].connected_ms = now_ms();
            LOG_INFO(DISPLAY_LOG_SOURCE, "auto SSE client connected (slot %d)", idx);
        } else {
            client_remove(idx);
        }
    } else if (strncmp(clients[idx].buf, "GET /stream", 11) == 0) {
        /* SSE endpoint */
        const char *sse_header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        if (write(clients[idx].fd, sse_header, strlen(sse_header)) > 0) {
            clients[idx].stream_mode = STREAM_MODE_LEGACY;
            clients[idx].needs_initial_frame = 1;
            clients[idx].connected_ms = now_ms();
            LOG_INFO(DISPLAY_LOG_SOURCE, "legacy SSE client connected (slot %d)", idx);
        } else {
            client_remove(idx);
        }
    } else if (strncmp(clients[idx].buf, "GET / ", 6) == 0 ||
               strncmp(clients[idx].buf, "GET /index", 10) == 0) {
        send_response(idx, 200, "text/html", HTML_PAGE, (int)sizeof(HTML_PAGE) - 1);
    } else {
        send_response(idx, 404, "text/plain", "Not Found", 9);
    }
    clients[idx].buf_len = 0;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    unified_log_init();

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    /* Init client slots */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].stream_mode = STREAM_MODE_NONE;
        clients[i].buf_len = 0;
    }

    /* Open shared memory (retry loop) */
    uint8_t *shm_ptr = NULL;
    int shm_fd = -1;
    long long last_shm_attempt = 0;
    norns_display_shm_t *norns_shm_ptr = NULL;
    int norns_shm_fd = -1;
    long long last_norns_shm_attempt = 0;
    e16_mirror_shm_t *e16_shm_ptr = NULL;
    long long last_e16_shm_attempt = 0;
    static char e16_json[4096], e16_last[4096];
    int e16_last_len = 0;
    long long hb_at = 0;
    const surface_live_shm_t *surface_ptr = NULL;
    long long last_surface_shm_attempt = 0;
    static surface_live_shm_t surface_snap, surface_last;
    int surface_have = 0;
    const audio_live_shm_t *audio_ptr = NULL;
    long long last_audio_shm_attempt = 0;
    uint64_t audio_cursor = 0;     /* next frame to send; 0 = not started */
    static int16_t audio_pcm[AUDIO_CHUNK_MAX * 2];
    static char audio_b64[AUDIO_CHUNK_MAX * 4 * 4 / 3 + 8];
    static char pcm_evt[AUDIO_CHUNK_MAX * 4 * 4 / 3 + 128];

    /* Listen socket */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        LOG_ERROR(DISPLAY_LOG_SOURCE, "socket failed: %s", strerror(errno));
        unified_log_shutdown();
        return 1;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR(DISPLAY_LOG_SOURCE, "bind failed on port %d: %s", port, strerror(errno));
        close(srv);
        unified_log_shutdown();
        return 1;
    }
    listen(srv, MAX_CLIENTS);
    fcntl(srv, F_SETFL, O_NONBLOCK);

    LOG_INFO(DISPLAY_LOG_SOURCE, "server listening on port %d", port);

    uint8_t last_display[DISPLAY_SIZE];
    memset(last_display, 0, sizeof(last_display));
    uint8_t last_auto_frame[NORNS_FRAME_SIZE];
    size_t last_auto_size = 0;
    auto_source_t last_auto_source = AUTO_SOURCE_NONE;
    long long last_push = 0;

    /* Large enough for 4096-byte base64 + JSON SSE framing. */
    static char b64_buf[SSE_BUF_SIZE];
    static char legacy_evt[1500], auto_evt[SSE_BUF_SIZE], e16_evt[4200], e16n_evt[4220];
    static char surf_evt[3300], hb_evt[96];

    while (running) {
        /* Try to open shm if not yet mapped */
        if (!shm_ptr) {
            long long now = now_ms();
            if (now - last_shm_attempt >= SHM_RETRY_MS) {
                last_shm_attempt = now;
                shm_fd = open(SHM_PATH, O_RDONLY);
                if (shm_fd >= 0) {
                    shm_ptr = mmap(NULL, DISPLAY_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
                    if (shm_ptr == MAP_FAILED) {
                        shm_ptr = NULL;
                        close(shm_fd);
                        shm_fd = -1;
                    } else {
                        LOG_INFO(DISPLAY_LOG_SOURCE, "opened %s", SHM_PATH);
                    }
                }
            }
        }
        if (!norns_shm_ptr) {
            long long now = now_ms();
            if (now - last_norns_shm_attempt >= SHM_RETRY_MS) {
                last_norns_shm_attempt = now;
                norns_shm_fd = open(NORNS_SHM_PATH, O_RDONLY);
                if (norns_shm_fd >= 0) {
                    norns_shm_ptr = mmap(NULL, sizeof(norns_display_shm_t),
                                         PROT_READ, MAP_SHARED, norns_shm_fd, 0);
                    if (norns_shm_ptr == MAP_FAILED) {
                        norns_shm_ptr = NULL;
                        close(norns_shm_fd);
                        norns_shm_fd = -1;
                    } else {
                        LOG_INFO(DISPLAY_LOG_SOURCE, "opened %s", NORNS_SHM_PATH);
                    }
                }
            }
        }

        /* The E16 mirror, made by shadow_ui only once an E16 is in use. A
         * segment shorter than the struct is refused (a mapping past its end
         * is SIGBUS), and retried. */
        if (!e16_shm_ptr) {
            long long now = now_ms();
            if (now - last_e16_shm_attempt >= SHM_RETRY_MS) {
                last_e16_shm_attempt = now;
                int fd = open(E16_MIRROR_SHM_PATH, O_RDONLY);
                if (fd >= 0) {
                    struct stat st;
                    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(e16_mirror_shm_t)) {
                        void *p = mmap(NULL, sizeof(e16_mirror_shm_t), PROT_READ, MAP_SHARED, fd, 0);
                        if (p != MAP_FAILED) {
                            e16_shm_ptr = (e16_mirror_shm_t *)p;
                            LOG_INFO(DISPLAY_LOG_SOURCE, "opened %s", E16_MIRROR_SHM_PATH);
                        }
                    }
                    close(fd);
                }
            }
        }

        /* The control surface (surface_live_shm.h), made by the shim. A
         * segment shorter than the struct is refused and retried. */
        if (!surface_ptr) {
            long long now = now_ms();
            if (now - last_surface_shm_attempt >= SHM_RETRY_MS) {
                last_surface_shm_attempt = now;
                int fd = open(SURFACE_LIVE_SHM_PATH, O_RDONLY);
                if (fd >= 0) {
                    struct stat st;
                    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(surface_live_shm_t)) {
                        void *p = mmap(NULL, sizeof(surface_live_shm_t), PROT_READ, MAP_SHARED, fd, 0);
                        if (p != MAP_FAILED) {
                            surface_ptr = (const surface_live_shm_t *)p;
                            LOG_INFO(DISPLAY_LOG_SOURCE, "opened %s", SURFACE_LIVE_SHM_PATH);
                        }
                    }
                    close(fd);
                }
            }
        }

        /* The mirror's audio ring (audio_live_shm.h), made by the shim. */
        if (!audio_ptr) {
            long long now = now_ms();
            if (now - last_audio_shm_attempt >= SHM_RETRY_MS) {
                last_audio_shm_attempt = now;
                int fd = open(AUDIO_LIVE_SHM_PATH, O_RDONLY);
                if (fd >= 0) {
                    struct stat st;
                    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(audio_live_shm_t)) {
                        void *p = mmap(NULL, sizeof(audio_live_shm_t), PROT_READ, MAP_SHARED, fd, 0);
                        if (p != MAP_FAILED) {
                            audio_ptr = (const audio_live_shm_t *)p;
                            LOG_INFO(DISPLAY_LOG_SOURCE, "opened %s", AUDIO_LIVE_SHM_PATH);
                        }
                    }
                    close(fd);
                }
            }
        }

        /* Build fd_sets for select: requests to read, queues to drain */
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(srv, &rfds);
        int maxfd = srv;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            if (clients[i].stream_mode == STREAM_MODE_NONE) FD_SET(clients[i].fd, &rfds);
            else if (clients[i].out_off < clients[i].out_len) FD_SET(clients[i].fd, &wfds);
            else continue;
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = POLL_INTERVAL_MS * 1000;
        int nready = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        long long now = now_ms();

        /* Accept new connections. When every slot is taken, the OLDEST stream
         * goes: a half-open connection holds its slot for minutes, and the
         * person reloading the page is the one who is actually there. */
        if (nready > 0 && FD_ISSET(srv, &rfds)) {
            int cfd = accept(srv, NULL, NULL);
            if (cfd >= 0) {
                fcntl(cfd, F_SETFL, O_NONBLOCK);
                int slot = -1, oldest = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) { slot = i; break; }
                    if (clients[i].stream_mode != STREAM_MODE_NONE &&
                        (oldest < 0 || clients[i].connected_ms < clients[oldest].connected_ms))
                        oldest = i;
                }
                if (slot < 0 && oldest >= 0) {
                    LOG_INFO(DISPLAY_LOG_SOURCE, "slots full, evicting oldest stream (slot %d)", oldest);
                    client_remove(oldest);
                    slot = oldest;
                }
                if (slot >= 0) {
#ifdef DISPLAY_SERVER_TEST_SNDBUF
                    /* tests only: a tiny kernel buffer, so the queue and the
                     * short-write path are exercised on loopback */
                    int sb = DISPLAY_SERVER_TEST_SNDBUF;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
#endif
                    clients[slot].fd = cfd;
                    clients[slot].stream_mode = STREAM_MODE_NONE;
                    clients[slot].buf_len = 0;
                    clients[slot].connected_ms = now;
                } else {
                    close(cfd);
                }
            }
        }

        /* Drain queues the socket has room for */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && clients[i].stream_mode != STREAM_MODE_NONE &&
                clients[i].out_off < clients[i].out_len &&
                ((nready > 0 && FD_ISSET(clients[i].fd, &wfds)) ||
                 now - clients[i].stalled_since > STALL_DROP_MS))
                (void)client_flush(i, now);
        }

        /* Read from non-streaming clients */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0 || clients[i].stream_mode != STREAM_MODE_NONE) continue;
            if (nready > 0 && FD_ISSET(clients[i].fd, &rfds)) {
                int space = CLIENT_BUF_SIZE - clients[i].buf_len - 1;
                if (space <= 0) { client_remove(i); continue; }
                int n = read(clients[i].fd, clients[i].buf + clients[i].buf_len, space);
                if (n <= 0) { client_remove(i); continue; }
                clients[i].buf_len += n;
                /* Check for complete HTTP request */
                clients[i].buf[clients[i].buf_len] = '\0';
                if (strstr(clients[i].buf, "\r\n\r\n")) {
                    handle_http(i);
                }
            }
        }

        if (now - last_push < POLL_INTERVAL_MS) continue;
        last_push = now;

        /* ---- What changed since the last pass ---- */

        /* The E16 mirror */
        int e16_changed = 0;
        {
            int n = e16_mirror_payload(e16_shm_ptr, now, e16_json, sizeof e16_json);
            if (n > 0 && n < (int)sizeof e16_json &&
                (n != e16_last_len || memcmp(e16_json, e16_last, (size_t)n) != 0)) {
                memcpy(e16_last, e16_json, (size_t)n);
                e16_last_len = n;
                e16_changed = 1;
            }
        }

        /* The control surface: copied between two equal, even sequence
         * reads. `seq` and `frame` move on every SPI frame and are not a
         * change; everything from event_count on is. */
        int surface_changed = 0;
        if (surface_ptr && memcmp(surface_ptr->magic, SURFACE_LIVE_MAGIC, 7) == 0 &&
            surface_ptr->version == SURFACE_LIVE_VERSION) {
            for (int tries = 0; tries < 4; tries++) {
                uint32_t s1 = __atomic_load_n(&surface_ptr->seq, __ATOMIC_ACQUIRE);
                if (s1 & 1u) continue;
                memcpy(&surface_snap, (const void *)surface_ptr, sizeof surface_snap);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (__atomic_load_n(&surface_ptr->seq, __ATOMIC_RELAXED) != s1) continue;
                const size_t from = offsetof(surface_live_shm_t, event_count);
                if (!surface_have ||
                    memcmp((const uint8_t *)&surface_snap + from, (const uint8_t *)&surface_last + from,
                           sizeof surface_snap - from) != 0) {
                    surface_last = surface_snap;
                    surface_have = 1;
                    surface_changed = 1;
                }
                break;
            }
        }

        /* Move's OLED (legacy stream) */
        int legacy_changed = 0;
        if (shm_ptr && memcmp(shm_ptr, last_display, DISPLAY_SIZE) != 0) {
            memcpy(last_display, shm_ptr, DISPLAY_SIZE);
            legacy_changed = 1;
        }

        /* The auto stream: a live norns frame, else Move's OLED */
        int auto_changed = 0;
        {
            static uint8_t norns_frame_copy[NORNS_FRAME_SIZE];
            const uint8_t *auto_frame = NULL;
            size_t auto_frame_size = 0;
            auto_source_t auto_source = AUTO_SOURCE_NONE;

            if (norns_frame_is_live(norns_shm_ptr, now)) {
                /* Snapshot frame_counter before and after reading frame
                 * to detect torn reads */
                uint32_t counter_before = norns_shm_ptr->frame_counter;
                __sync_synchronize(); /* memory barrier */
                memcpy(norns_frame_copy, norns_shm_ptr->frame, NORNS_FRAME_SIZE);
                __sync_synchronize();
                uint32_t counter_after = norns_shm_ptr->frame_counter;
                if (counter_before == counter_after) {
                    auto_frame = norns_frame_copy;
                    auto_frame_size = NORNS_FRAME_SIZE;
                    auto_source = AUTO_SOURCE_NORNS;
                }
            } else if (shm_ptr) {
                auto_frame = shm_ptr;
                auto_frame_size = DISPLAY_SIZE;
                auto_source = AUTO_SOURCE_MOVE;
            }
            if (auto_frame &&
                (auto_source != last_auto_source || auto_frame_size != last_auto_size ||
                 memcmp(auto_frame, last_auto_frame, auto_frame_size) != 0)) {
                memcpy(last_auto_frame, auto_frame, auto_frame_size);
                last_auto_size = auto_frame_size;
                last_auto_source = auto_source;
                auto_changed = 1;
            }
        }

        /* ---- Events, built once and only if someone will get them ---- */
        int legacy_len = 0, auto_len = 0, e16_len = 0, e16n_len = 0, surf_len = 0, hb_len = 0;
        int want_legacy = 0, want_auto = 0, want_e16 = 0, want_extras = 0, want_audio = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            if (clients[i].stream_mode == STREAM_MODE_LEGACY) want_legacy = 1;
            if (clients[i].stream_mode == STREAM_MODE_AUTO) {
                want_auto = 1;
                if (clients[i].extras) want_extras = 1;
                if (clients[i].audio) want_audio = 1;
            }
            if (clients[i].stream_mode == STREAM_MODE_E16) want_e16 = 1;
        }
        if (want_legacy && shm_ptr) {
            (void)base64_encode(last_display, DISPLAY_SIZE, b64_buf);
            legacy_len = snprintf(legacy_evt, sizeof legacy_evt, "data: %s\n\n", b64_buf);
        }
        if (want_auto && last_auto_size > 0) {
            const char *fmt = (last_auto_source == AUTO_SOURCE_NORNS) ? NORNS_DISPLAY_FORMAT : "mono1_packed";
            const char *src = (last_auto_source == AUTO_SOURCE_NORNS) ? "norns 4-bit" : "move 1-bit";
            (void)base64_encode(last_auto_frame, (int)last_auto_size, b64_buf);
            auto_len = snprintf(auto_evt, sizeof auto_evt,
                                "data: {\"format\":\"%s\",\"encoding\":\"base64\","
                                "\"width\":128,\"height\":64,\"source\":\"%s\","
                                "\"data\":\"%s\"}\n\n", fmt, src, b64_buf);
        }
        if (want_e16 || want_extras) {
            const char *body = e16_last_len > 0 ? e16_last : "{\"active\":0}";
            int blen = e16_last_len > 0 ? e16_last_len : (int)strlen(body);
            e16_len = snprintf(e16_evt, sizeof e16_evt, "data: %.*s\n\n", blen, body);
            e16n_len = snprintf(e16n_evt, sizeof e16n_evt, "event: e16\ndata: %.*s\n\n", blen, body);
        }
        if (want_extras && surface_have) {
            (void)base64_encode((const uint8_t *)&surface_last, (int)sizeof surface_last, b64_buf);
            surf_len = snprintf(surf_evt, sizeof surf_evt,
                                "event: surface\ndata: {\"v\":%d,\"frame\":%u,\"data\":\"%s\"}\n\n",
                                SURFACE_LIVE_VERSION, surface_snap.frame, b64_buf);
        }
        /* Sound: everything new since the last pass, as one event every
         * listener gets. `pos` is the frame index of the first sample, so a
         * page can tell a gap (a skipped chunk, a lapped ring) from a
         * continuation. With no listener the cursor is dropped, and the next
         * one starts near the writer instead of replaying a stale ring. */
        int pcm_len = 0;
        if (!want_audio) audio_cursor = 0;
        if (want_audio && audio_ptr && memcmp(audio_ptr->magic, AUDIO_LIVE_MAGIC, 6) == 0 &&
            audio_ptr->version == AUDIO_LIVE_VERSION) {
            uint64_t wp = __atomic_load_n(&audio_ptr->write_pos, __ATOMIC_ACQUIRE);
            if (audio_cursor == 0 || audio_cursor > wp || wp - audio_cursor > AUDIO_LIVE_FRAMES / 2)
                audio_cursor = wp > AUDIO_START_BACK ? wp - AUDIO_START_BACK : 0;
            if (wp - audio_cursor > AUDIO_CHUNK_MAX) audio_cursor = wp - AUDIO_CHUNK_MAX;
            if (wp > audio_cursor) {
                int n = audio_live_read(audio_ptr, audio_cursor, wp, audio_pcm);
                (void)base64_encode((const uint8_t *)audio_pcm, n * 4, audio_b64);
                pcm_len = snprintf(pcm_evt, sizeof pcm_evt,
                                   "event: pcm\ndata: {\"pos\":%llu,\"n\":%d,\"rate\":%u,\"d\":\"%s\"}\n\n",
                                   (unsigned long long)audio_cursor, n, audio_ptr->sample_rate, audio_b64);
                if (pcm_len >= (int)sizeof pcm_evt) pcm_len = 0;
                audio_cursor = wp;
            }
        }

        int hb_due = (now - hb_at >= HEARTBEAT_MS);
        if (hb_due) {
            hb_at = now;
            /* frame lets the page age the press log without a clock of its own */
            hb_len = snprintf(hb_evt, sizeof hb_evt, "event: hb\ndata: {\"frame\":%u}\n\n",
                              surface_have ? surface_snap.frame : 0u);
        }
        /* An event that overflowed its buffer is never sent: a truncated
         * event is worse than a missing one. */
        if (legacy_len >= (int)sizeof legacy_evt) legacy_len = 0;
        if (auto_len >= (int)sizeof auto_evt) auto_len = 0;
        if (e16_len >= (int)sizeof e16_evt) e16_len = 0;
        if (e16n_len >= (int)sizeof e16n_evt) e16n_len = 0;
        if (surf_len >= (int)sizeof surf_evt) surf_len = 0;

        /* ---- Send ---- */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0 || c->stream_mode == STREAM_MODE_NONE) continue;
            /* A new client, or one that skipped an event and has now drained,
             * gets every snapshot whether or not it changed. */
            int full = c->needs_initial_frame || (c->resync && c->out_off == c->out_len);
            if (full) { c->needs_initial_frame = 0; c->resync = 0; }
            int rc = 0;
            switch (c->stream_mode) {
            case STREAM_MODE_LEGACY:
                if ((full || legacy_changed) && legacy_len) rc = client_send(i, legacy_evt, legacy_len, now);
                break;
            case STREAM_MODE_AUTO:
                if ((full || auto_changed) && auto_len) rc = client_send(i, auto_evt, auto_len, now);
                if (rc == 0 && c->extras && (full || surface_changed) && surf_len)
                    rc = client_send(i, surf_evt, surf_len, now);
                if (rc == 0 && c->extras && (full || e16_changed) && e16n_len)
                    rc = client_send(i, e16n_evt, e16n_len, now);
                if (rc == 0 && c->audio && pcm_len)
                    rc = client_send(i, pcm_evt, pcm_len, now);
                break;
            case STREAM_MODE_E16:
                if ((full || e16_changed) && e16_len) rc = client_send(i, e16_evt, e16_len, now);
                break;
            default:
                break;
            }
            if (rc == 0 && hb_due && hb_len) (void)client_send(i, hb_evt, hb_len, now);
        }
    }

    LOG_INFO(DISPLAY_LOG_SOURCE, "shutting down");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) close(clients[i].fd);
    }
    close(srv);
    if (shm_ptr) munmap(shm_ptr, DISPLAY_SIZE);
    if (shm_fd >= 0) close(shm_fd);
    if (norns_shm_ptr) munmap(norns_shm_ptr, sizeof(norns_display_shm_t));
    if (e16_shm_ptr) munmap(e16_shm_ptr, sizeof(e16_mirror_shm_t));
    if (surface_ptr) munmap((void *)surface_ptr, sizeof(surface_live_shm_t));
    if (audio_ptr) munmap((void *)audio_ptr, sizeof(audio_live_shm_t));
    if (norns_shm_fd >= 0) close(norns_shm_fd);
    unified_log_shutdown();
    return 0;
}
