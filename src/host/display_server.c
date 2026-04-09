/*
 * display_server.c - Live display SSE server
 *
 * Streams Move's 128x64 1-bit OLED to a browser via Server-Sent Events.
 * Reads /dev/shm/schwung-display-live (1024 bytes, written by the shim)
 * and pushes base64-encoded frames to connected browser clients at ~30 Hz.
 *
 * Also supports WebSocket streaming of generic display frames (arbitrary
 * resolution, full-color) from shared memory, with touch back-channel.
 *
 * Usage: display-server [port]   (default port 7681)
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "generic_display_shm.h"
#include "norns_display_shm.h"
#include "unified_log.h"

/* ------------------------------------------------------------------ */
/* Minimal SHA-1 implementation (public domain, RFC 3174)              */
/* Required for WebSocket handshake (Sec-WebSocket-Accept).            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx;

static void sha1_init(sha1_ctx *c) {
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE; c->state[3] = 0x10325476;
    c->state[4] = 0xC3D2E1F0; c->count = 0;
}

#define SHA1_ROL(v,b) (((v)<<(b))|((v)>>(32-(b))))
#define SHA1_BLK(i) (block[i&15] = SHA1_ROL(block[(i+13)&15]^block[(i+8)&15]^block[(i+2)&15]^block[i&15],1))

static void sha1_transform(uint32_t state[5], const uint8_t buf[64]) {
    uint32_t a,b,c,d,e, block[16];
    for (int i=0;i<16;i++)
        block[i]=(uint32_t)buf[i*4]<<24|(uint32_t)buf[i*4+1]<<16|
                 (uint32_t)buf[i*4+2]<<8|(uint32_t)buf[i*4+3];
    a=state[0]; b=state[1]; c=state[2]; d=state[3]; e=state[4];
    for (int i=0;i<80;i++) {
        uint32_t f,k,w=(i<16)?block[i]:SHA1_BLK(i);
        if      (i<20) { f=(b&c)|((~b)&d);       k=0x5A827999; }
        else if (i<40) { f=b^c^d;                 k=0x6ED9EBA1; }
        else if (i<60) { f=(b&c)|(b&d)|(c&d);     k=0x8F1BBCDC; }
        else           { f=b^c^d;                 k=0xCA62C1D6; }
        uint32_t t=SHA1_ROL(a,5)+f+e+k+w;
        e=d; d=c; c=SHA1_ROL(b,30); b=a; a=t;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
}

static void sha1_update(sha1_ctx *c, const uint8_t *data, size_t len) {
    size_t idx = (size_t)(c->count & 63);
    c->count += len;
    for (size_t i=0; i<len; i++) {
        c->buffer[idx++] = data[i];
        if (idx==64) { sha1_transform(c->state, c->buffer); idx=0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t digest[20]) {
    size_t idx = (size_t)(c->count & 63);
    c->buffer[idx++] = 0x80;
    if (idx>56) { while(idx<64) c->buffer[idx++]=0; sha1_transform(c->state,c->buffer); idx=0; memset(c->buffer,0,64); }
    while(idx<56) c->buffer[idx++]=0;
    uint64_t bits=c->count*8;
    for (int i=7;i>=0;i--) c->buffer[56+(7-i)]=(uint8_t)(bits>>(i*8));
    sha1_transform(c->state,c->buffer);
    for (int i=0;i<5;i++) {
        digest[i*4]=(uint8_t)(c->state[i]>>24); digest[i*4+1]=(uint8_t)(c->state[i]>>16);
        digest[i*4+2]=(uint8_t)(c->state[i]>>8); digest[i*4+3]=(uint8_t)(c->state[i]);
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket constants                                                 */
/* ------------------------------------------------------------------ */
#define WS_GUID    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_OP_TEXT   0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A

#define DEFAULT_PORT       7681
#define SHM_PATH           "/dev/shm/schwung-display-live"
#define DISPLAY_SIZE       1024
#define NORNS_SHM_PATH     "/dev/shm/schwung-norns-display-live"
#define MAX_CLIENTS        8
#define POLL_INTERVAL_MS   33    /* ~30 Hz */
#define SHM_RETRY_MS       2000
#define CLIENT_BUF_SIZE    4096
#define SSE_BUF_SIZE       7000

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
    STREAM_MODE_WS_GENERIC = 3,
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
    char buf[CLIENT_BUF_SIZE];
    int buf_len;
    /* WebSocket receive state */
    uint8_t ws_buf[8192];
    int ws_buf_len;
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
    "</style>\n"
    "</head><body>\n"
    "<canvas id=\"c\" width=\"128\" height=\"64\"></canvas>\n"
    "<div id=\"status\">connecting... (tap to fullscreen)</div>\n"
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
    "function connect() {\n"
    "  const es = new EventSource('/stream-auto');\n"
    "  es.onopen = () => {\n"
    "    statusEl.textContent = 'connected';\n"
    "    statusEl.className = 'connected';\n"
    "  };\n"
    "  es.onerror = () => {\n"
    "    statusEl.textContent = 'disconnected - reconnecting...';\n"
    "    statusEl.className = '';\n"
    "  };\n"
    "  es.onmessage = (e) => {\n"
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

/* ------------------------------------------------------------------ */
/* WebSocket helpers                                                   */
/* ------------------------------------------------------------------ */

/* Perform WebSocket handshake: read Sec-WebSocket-Key from the already-
   buffered HTTP request, compute SHA-1 + base64 accept value, send 101. */
/* Case-insensitive substring search */
static char *strcasestr_local(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}

static int ws_handshake(int idx) {
    char *key_start = strcasestr_local(clients[idx].buf, "Sec-WebSocket-Key:");
    if (!key_start) return -1;
    key_start += 18; /* skip header name + colon */
    while (*key_start == ' ') key_start++;
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return -1;
    int key_len = (int)(key_end - key_start);
    if (key_len <= 0 || key_len > 64) return -1;

    /* Concatenate key + GUID */
    char concat[128];
    memcpy(concat, key_start, key_len);
    memcpy(concat + key_len, WS_GUID, 36);
    int concat_len = key_len + 36;

    /* SHA-1 hash */
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)concat, concat_len);
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    /* Base64 encode the digest */
    char accept_b64[32];
    base64_encode(digest, 20, accept_b64);

    /* Send 101 Switching Protocols */
    char resp[512];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_b64);
    if (write(clients[idx].fd, resp, resp_len) <= 0) return -1;
    return 0;
}

/* Send a binary WebSocket frame (server -> client, unmasked).
   Supports payloads up to 2MB (uses 64-bit extended length for >65535). */
static int ws_send_binary(int fd, const uint8_t *data, size_t len) {
    uint8_t hdr[10];
    int hdr_len;
    hdr[0] = 0x80 | WS_OP_BINARY; /* FIN + binary opcode */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hdr_len = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        hdr[2] = 0; hdr[3] = 0; hdr[4] = 0; hdr[5] = 0;
        hdr[6] = (uint8_t)(len >> 24);
        hdr[7] = (uint8_t)(len >> 16);
        hdr[8] = (uint8_t)(len >> 8);
        hdr[9] = (uint8_t)(len);
        hdr_len = 10;
    }
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = hdr_len;
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = len;
    ssize_t sent = writev(fd, iov, 2);
    return (sent == (ssize_t)(hdr_len + len)) ? 0 : -1;
}

/* Receive and unmask a WebSocket frame from the client's ws_buf.
   Returns the opcode on success, -1 if not enough data yet, -2 on error.
   Consumed bytes are removed from ws_buf. */
static int ws_recv_frame(client_t *c, uint8_t *payload, int *payload_len, int max_len) {
    if (c->ws_buf_len < 2) return -1;
    int offset = 0;
    uint8_t b0 = c->ws_buf[0], b1 = c->ws_buf[1];
    int opcode = b0 & 0x0F;
    int masked = (b1 >> 7) & 1;
    uint64_t plen = b1 & 0x7F;
    offset = 2;
    if (plen == 126) {
        if (c->ws_buf_len < 4) return -1;
        plen = ((uint64_t)c->ws_buf[2] << 8) | c->ws_buf[3];
        offset = 4;
    } else if (plen == 127) {
        if (c->ws_buf_len < 10) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | c->ws_buf[2 + i];
        offset = 10;
    }
    int mask_offset = offset;
    if (masked) offset += 4;
    if ((uint64_t)c->ws_buf_len < offset + plen) return -1;
    if (plen > (uint64_t)max_len) return -2; /* frame too large */

    /* Unmask */
    uint8_t *src = c->ws_buf + offset;
    if (masked) {
        uint8_t *mask = c->ws_buf + mask_offset;
        for (uint64_t i = 0; i < plen; i++)
            payload[i] = src[i] ^ mask[i & 3];
    } else {
        memcpy(payload, src, (size_t)plen);
    }
    *payload_len = (int)plen;

    /* Remove consumed bytes */
    int consumed = offset + (int)plen;
    c->ws_buf_len -= consumed;
    if (c->ws_buf_len > 0)
        memmove(c->ws_buf, c->ws_buf + consumed, c->ws_buf_len);
    return opcode;
}

/* Send a WebSocket pong frame with the given payload */
static void ws_send_pong(int fd, const uint8_t *data, int len) {
    uint8_t hdr[2];
    hdr[0] = 0x80 | WS_OP_PONG;
    hdr[1] = (len < 126) ? (uint8_t)len : 0;
    (void)write(fd, hdr, 2);
    if (len > 0 && len < 126)
        (void)write(fd, data, len);
}

/* Close and clear a client slot */
static void client_remove(int idx) {
    if (clients[idx].fd >= 0) {
        if (clients[idx].stream_mode == STREAM_MODE_WS_GENERIC)
            LOG_INFO(DISPLAY_LOG_SOURCE, "WS client disconnected (slot %d)", idx);
        else if (clients[idx].stream_mode != STREAM_MODE_NONE)
            LOG_INFO(DISPLAY_LOG_SOURCE, "SSE client disconnected (slot %d)", idx);
        close(clients[idx].fd);
    }
    clients[idx].fd = -1;
    clients[idx].stream_mode = STREAM_MODE_NONE;
    clients[idx].buf_len = 0;
    clients[idx].ws_buf_len = 0;
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

    if (strncmp(clients[idx].buf, "GET /ws-generic", 15) == 0) {
        /* WebSocket upgrade for generic display */
        if (ws_handshake(idx) == 0) {
            clients[idx].stream_mode = STREAM_MODE_WS_GENERIC;
            clients[idx].ws_buf_len = 0;
            LOG_INFO(DISPLAY_LOG_SOURCE, "WS generic client connected (slot %d)", idx);
        } else {
            send_response(idx, 400, "text/plain", "Bad Request", 11);
        }
    } else if (strncmp(clients[idx].buf, "GET /stream-auto", 16) == 0) {
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

    /* Generic display SHM */
    generic_display_shm_t *generic_shm_ptr = NULL;
    int generic_shm_fd = -1;
    size_t generic_shm_size = 0;
    long long last_generic_shm_attempt = 0;
    uint32_t last_generic_frame_counter = 0;

    /* Scratch buffer for WS frame assembly (up to 2MB) */
    uint8_t *ws_scratch = malloc(2 * 1024 * 1024);
    if (!ws_scratch) {
        LOG_ERROR(DISPLAY_LOG_SOURCE, "failed to allocate WS scratch buffer");
        unified_log_shutdown();
        return 1;
    }

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
    char b64_buf[SSE_BUF_SIZE];
    char sse_buf[SSE_BUF_SIZE];

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
        /* Try to open generic display SHM (O_RDWR for touch back-channel) */
        if (!generic_shm_ptr) {
            long long now = now_ms();
            if (now - last_generic_shm_attempt >= SHM_RETRY_MS) {
                last_generic_shm_attempt = now;
                generic_shm_fd = open(GENERIC_DISPLAY_SHM_PATH, O_RDWR);
                if (generic_shm_fd >= 0) {
                    /* First map just the header to read dimensions */
                    generic_display_shm_t *hdr = mmap(NULL, GENERIC_DISPLAY_HEADER_SIZE,
                                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                                       generic_shm_fd, 0);
                    if (hdr == MAP_FAILED) {
                        close(generic_shm_fd);
                        generic_shm_fd = -1;
                    } else if (memcmp(hdr->magic, GENERIC_DISPLAY_MAGIC, 8) != 0 ||
                               hdr->version != 1 || hdr->bytes_per_frame == 0) {
                        munmap(hdr, GENERIC_DISPLAY_HEADER_SIZE);
                        close(generic_shm_fd);
                        generic_shm_fd = -1;
                    } else {
                        /* Remap with full size (header + frame data) */
                        size_t total = GENERIC_DISPLAY_HEADER_SIZE + hdr->bytes_per_frame;
                        munmap(hdr, GENERIC_DISPLAY_HEADER_SIZE);
                        generic_shm_ptr = mmap(NULL, total, PROT_READ | PROT_WRITE,
                                               MAP_SHARED, generic_shm_fd, 0);
                        if (generic_shm_ptr == MAP_FAILED) {
                            generic_shm_ptr = NULL;
                            close(generic_shm_fd);
                            generic_shm_fd = -1;
                        } else {
                            generic_shm_size = total;
                            last_generic_frame_counter = generic_shm_ptr->frame_counter;
                            LOG_INFO(DISPLAY_LOG_SOURCE, "opened %s (%ux%u, %u bpp)",
                                     GENERIC_DISPLAY_SHM_PATH,
                                     generic_shm_ptr->width, generic_shm_ptr->height,
                                     generic_shm_ptr->bytes_per_pixel);
                        }
                    }
                }
            }
        }

        /* Build fd_set for select */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        int maxfd = srv;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 &&
                (clients[i].stream_mode == STREAM_MODE_NONE ||
                 clients[i].stream_mode == STREAM_MODE_WS_GENERIC)) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = POLL_INTERVAL_MS * 1000;
        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        /* Accept new connections */
        if (nready > 0 && FD_ISSET(srv, &rfds)) {
            int cfd = accept(srv, NULL, NULL);
            if (cfd >= 0) {
                fcntl(cfd, F_SETFL, O_NONBLOCK);
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = cfd;
                        clients[i].stream_mode = STREAM_MODE_NONE;
                        clients[i].buf_len = 0;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) close(cfd);
            }
        }

        /* Read from non-streaming clients (HTTP) */
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

        /* Read from WebSocket clients (touch events, ping/pong, close) */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0 || clients[i].stream_mode != STREAM_MODE_WS_GENERIC) continue;
            if (nready > 0 && FD_ISSET(clients[i].fd, &rfds)) {
                int space = (int)sizeof(clients[i].ws_buf) - clients[i].ws_buf_len;
                if (space <= 0) { client_remove(i); continue; }
                int n = read(clients[i].fd, clients[i].ws_buf + clients[i].ws_buf_len, space);
                if (n == 0) { client_remove(i); continue; }  /* EOF */
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue; /* no data yet */
                    client_remove(i); continue;  /* real error */
                }
                clients[i].ws_buf_len += n;

                /* Process all complete frames in buffer */
                uint8_t ws_payload[1024];
                int ws_plen;
                int opcode;
                while ((opcode = ws_recv_frame(&clients[i], ws_payload, &ws_plen, (int)sizeof(ws_payload))) >= 0) {
                    if (opcode == WS_OP_CLOSE) {
                        client_remove(i);
                        break;
                    } else if (opcode == WS_OP_PING) {
                        ws_send_pong(clients[i].fd, ws_payload, ws_plen);
                    } else if (opcode == WS_OP_TEXT && generic_shm_ptr) {
                        /* Parse touch JSON: {"x":N,"y":N,"state":N} */
                        ws_payload[ws_plen] = '\0';
                        char *px = strstr((char *)ws_payload, "\"x\":");
                        char *py = strstr((char *)ws_payload, "\"y\":");
                        char *ps = strstr((char *)ws_payload, "\"state\":");
                        if (px && py && ps) {
                            uint32_t tx = (uint32_t)atoi(px + 4);
                            uint32_t ty = (uint32_t)atoi(py + 4);
                            uint32_t ts = (uint32_t)atoi(ps + 8);
                            generic_shm_ptr->touch_x = tx;
                            generic_shm_ptr->touch_y = ty;
                            generic_shm_ptr->touch_state = ts;
                            __sync_synchronize();
                            generic_shm_ptr->touch_counter++;
                            __sync_synchronize();
                        }
                    }
                }
            }
        }

        /* Push display frames to SSE clients */
        {
            long long now = now_ms();
            if (now - last_push >= POLL_INTERVAL_MS) {
                int legacy_changed = 0;
                const uint8_t *auto_frame = NULL;
                size_t auto_frame_size = 0;
                const char *auto_format = NULL;
                const char *auto_source_label = NULL;
                auto_source_t auto_source = AUTO_SOURCE_NONE;

                last_push = now;

                /* Send initial frame to newly connected clients */
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0 || !clients[i].needs_initial_frame) continue;
                    clients[i].needs_initial_frame = 0;

                    if (clients[i].stream_mode == STREAM_MODE_LEGACY) {
                        /* Send cached mono display if available */
                        if (shm_ptr) {
                            int sse_len;
                            (void)base64_encode(shm_ptr, DISPLAY_SIZE, b64_buf);
                            sse_len = snprintf(sse_buf, sizeof(sse_buf), "data: %s\n\n", b64_buf);
                            if (write(clients[i].fd, sse_buf, sse_len) <= 0)
                                client_remove(i);
                        }
                    } else if (clients[i].stream_mode == STREAM_MODE_AUTO) {
                        /* Send cached auto frame (norns or move) */
                        if (last_auto_size > 0) {
                            int sse_len;
                            const char *fmt = (last_auto_source == AUTO_SOURCE_NORNS)
                                ? NORNS_DISPLAY_FORMAT : "mono1_packed";
                            const char *src = (last_auto_source == AUTO_SOURCE_NORNS)
                                ? "norns 4-bit" : "move 1-bit";
                            (void)base64_encode(last_auto_frame, (int)last_auto_size, b64_buf);
                            sse_len = snprintf(sse_buf, sizeof(sse_buf),
                                "data: {\"format\":\"%s\",\"encoding\":\"base64\","
                                "\"width\":128,\"height\":64,\"source\":\"%s\","
                                "\"data\":\"%s\"}\n\n",
                                fmt, src, b64_buf);
                            if (sse_len < (int)sizeof(sse_buf)) {
                                if (write(clients[i].fd, sse_buf, sse_len) <= 0)
                                    client_remove(i);
                            }
                        } else if (shm_ptr) {
                            /* No auto frame cached yet, fall back to mono */
                            int sse_len;
                            (void)base64_encode(shm_ptr, DISPLAY_SIZE, b64_buf);
                            sse_len = snprintf(sse_buf, sizeof(sse_buf),
                                "data: {\"format\":\"mono1_packed\",\"encoding\":\"base64\","
                                "\"width\":128,\"height\":64,\"source\":\"move 1-bit\","
                                "\"data\":\"%s\"}\n\n",
                                b64_buf);
                            if (sse_len < (int)sizeof(sse_buf)) {
                                if (write(clients[i].fd, sse_buf, sse_len) <= 0)
                                    client_remove(i);
                            }
                        }
                    }
                }

                if (shm_ptr && memcmp(shm_ptr, last_display, DISPLAY_SIZE) != 0) {
                    memcpy(last_display, shm_ptr, DISPLAY_SIZE);
                    legacy_changed = 1;
                }

                static uint8_t norns_frame_copy[NORNS_FRAME_SIZE];
                int norns_torn_read = 0;

                if (norns_frame_is_live(norns_shm_ptr, now)) {
                    /* Snapshot frame_counter before and after reading frame
                     * to detect torn reads */
                    uint32_t counter_before = norns_shm_ptr->frame_counter;
                    __sync_synchronize(); /* memory barrier */
                    memcpy(norns_frame_copy, norns_shm_ptr->frame, NORNS_FRAME_SIZE);
                    __sync_synchronize();
                    uint32_t counter_after = norns_shm_ptr->frame_counter;
                    if (counter_before != counter_after) {
                        /* Frame was being written during our read - skip */
                        norns_torn_read = 1;
                    }
                    if (!norns_torn_read) {
                        auto_frame = norns_frame_copy;
                        auto_frame_size = NORNS_FRAME_SIZE;
                        auto_format = NORNS_DISPLAY_FORMAT;
                        auto_source_label = "norns 4-bit";
                        auto_source = AUTO_SOURCE_NORNS;
                    }
                } else if (shm_ptr) {
                    auto_frame = shm_ptr;
                    auto_frame_size = DISPLAY_SIZE;
                    auto_format = "mono1_packed";
                    auto_source_label = "move 1-bit";
                    auto_source = AUTO_SOURCE_MOVE;
                }

                if (legacy_changed) {
                    int sse_len;
                    (void)base64_encode(last_display, DISPLAY_SIZE, b64_buf);
                    sse_len = snprintf(sse_buf, sizeof(sse_buf), "data: %s\n\n", b64_buf);
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].fd < 0 || clients[i].stream_mode != STREAM_MODE_LEGACY) continue;
                        if (write(clients[i].fd, sse_buf, sse_len) <= 0) client_remove(i);
                    }
                }

                if (auto_frame) {
                    int auto_changed =
                        (auto_source != last_auto_source) ||
                        (auto_frame_size != last_auto_size) ||
                        (memcmp(auto_frame, last_auto_frame, auto_frame_size) != 0);
                    if (auto_changed) {
                        int sse_len;
                        (void)base64_encode(auto_frame, (int)auto_frame_size, b64_buf);
                        sse_len = snprintf(sse_buf, sizeof(sse_buf),
                                           "data: {\"format\":\"%s\",\"encoding\":\"base64\","
                                           "\"width\":128,\"height\":64,\"source\":\"%s\","
                                           "\"data\":\"%s\"}\n\n",
                                           auto_format, auto_source_label, b64_buf);
                        if (sse_len < (int)sizeof(sse_buf)) {
                            for (int i = 0; i < MAX_CLIENTS; i++) {
                                if (clients[i].fd < 0 || clients[i].stream_mode != STREAM_MODE_AUTO) continue;
                                if (write(clients[i].fd, sse_buf, sse_len) <= 0) client_remove(i);
                            }
                            memcpy(last_auto_frame, auto_frame, auto_frame_size);
                            last_auto_size = auto_frame_size;
                            last_auto_source = auto_source;
                        }
                    }
                }

                /* Detect stale generic SHM and remap if producer restarted */
                if (generic_shm_ptr) {
                    long long stale_age = now - (long long)generic_shm_ptr->last_update_ms;
                    if (stale_age > GENERIC_STALE_MS * 2 ||
                        !generic_shm_ptr->active ||
                        memcmp(generic_shm_ptr->magic, GENERIC_DISPLAY_MAGIC, 8) != 0) {
                        LOG_INFO(DISPLAY_LOG_SOURCE, "generic SHM stale, unmapping for re-discovery");
                        munmap(generic_shm_ptr, generic_shm_size);
                        generic_shm_ptr = NULL;
                        if (generic_shm_fd >= 0) { close(generic_shm_fd); generic_shm_fd = -1; }
                        last_generic_shm_attempt = 0; /* retry immediately */
                    }
                }

                /* Push generic display frames via WebSocket */
                if (generic_shm_ptr &&
                    generic_shm_ptr->active &&
                    memcmp(generic_shm_ptr->magic, GENERIC_DISPLAY_MAGIC, 8) == 0) {
                    /* Check staleness */
                    long long age = now - (long long)generic_shm_ptr->last_update_ms;
                    if (age >= 0 && age <= GENERIC_STALE_MS &&
                        generic_shm_ptr->frame_counter != last_generic_frame_counter) {
                        uint32_t w = generic_shm_ptr->width;
                        uint32_t h = generic_shm_ptr->height;
                        uint32_t bpp = generic_shm_ptr->bytes_per_pixel;
                        uint32_t fc = generic_shm_ptr->frame_counter;
                        uint32_t frame_bytes = generic_shm_ptr->bytes_per_frame;

                        /* Sanity check: frame must fit in scratch buffer */
                        size_t total_ws = 16 + frame_bytes; /* 16-byte header + pixels */
                        if (total_ws <= 2 * 1024 * 1024 && frame_bytes > 0) {
                            /* Build 16-byte binary header (all LE) */
                            ws_scratch[0]  = (uint8_t)(w);
                            ws_scratch[1]  = (uint8_t)(w >> 8);
                            ws_scratch[2]  = (uint8_t)(w >> 16);
                            ws_scratch[3]  = (uint8_t)(w >> 24);
                            ws_scratch[4]  = (uint8_t)(h);
                            ws_scratch[5]  = (uint8_t)(h >> 8);
                            ws_scratch[6]  = (uint8_t)(h >> 16);
                            ws_scratch[7]  = (uint8_t)(h >> 24);
                            ws_scratch[8]  = (uint8_t)(bpp);
                            ws_scratch[9]  = (uint8_t)(bpp >> 8);
                            ws_scratch[10] = (uint8_t)(bpp >> 16);
                            ws_scratch[11] = (uint8_t)(bpp >> 24);
                            ws_scratch[12] = (uint8_t)(fc);
                            ws_scratch[13] = (uint8_t)(fc >> 8);
                            ws_scratch[14] = (uint8_t)(fc >> 16);
                            ws_scratch[15] = (uint8_t)(fc >> 24);

                            /* Copy frame pixels */
                            const uint8_t *frame_data =
                                (const uint8_t *)generic_shm_ptr + GENERIC_DISPLAY_HEADER_SIZE;
                            memcpy(ws_scratch + 16, frame_data, frame_bytes);

                            /* Send to all WS_GENERIC clients */
                            for (int i = 0; i < MAX_CLIENTS; i++) {
                                if (clients[i].fd < 0 ||
                                    clients[i].stream_mode != STREAM_MODE_WS_GENERIC) continue;
                                if (ws_send_binary(clients[i].fd, ws_scratch, total_ws) != 0)
                                    client_remove(i);
                            }
                            last_generic_frame_counter = fc;
                        }
                    }
                }
            }
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
    if (norns_shm_fd >= 0) close(norns_shm_fd);
    if (generic_shm_ptr) munmap(generic_shm_ptr, generic_shm_size);
    if (generic_shm_fd >= 0) close(generic_shm_fd);
    free(ws_scratch);
    unified_log_shutdown();
    return 0;
}
