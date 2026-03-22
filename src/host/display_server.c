/*
 * display_server.c - Live display SSE server
 *
 * Streams Move's 128x64 1-bit OLED to a browser via Server-Sent Events.
 * Reads /dev/shm/schwung-display-live (1024 bytes, written by the shim)
 * and pushes base64-encoded frames to connected browser clients at ~30 Hz.
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
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "unified_log.h"

#define DEFAULT_PORT       7681
#define SHM_PATH           "/dev/shm/schwung-display-live"
#define DISPLAY_SIZE       1024
#define NORNS_SHM_PATH     "/dev/shm/schwung-norns-display-live"
#define NORNS_FRAME_SIZE   4096
#define MAX_CLIENTS        8
#define POLL_INTERVAL_MS   33    /* ~30 Hz */
#define SHM_RETRY_MS       2000
#define NORNS_STALE_MS     1500
#define CLIENT_BUF_SIZE    4096
#define SSE_BUF_SIZE       7000

#define NORNS_DISPLAY_MAGIC  "NR4SHM1"
#define NORNS_DISPLAY_FORMAT "gray4_packed"

#define DISPLAY_LOG_SOURCE "display_server"

typedef struct {
    char magic[8];
    char format[16];
    uint64_t last_update_ms;
    uint32_t version;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_frame;
    uint32_t frame_counter;
    uint32_t active;
    uint32_t reserved;
    uint8_t frame[NORNS_FRAME_SIZE];
} norns_display_shm_t;

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
} stream_mode_t;

typedef enum {
    AUTO_SOURCE_NONE = 0,
    AUTO_SOURCE_MOVE = 1,
    AUTO_SOURCE_NORNS = 2,
} auto_source_t;

typedef struct {
    int fd;
    stream_mode_t stream_mode;
    char buf[CLIENT_BUF_SIZE];
    int buf_len;
} client_t;

static client_t clients[MAX_CLIENTS];
static volatile sig_atomic_t running = 1;

static void sighandler(int sig) { (void)sig; running = 0; }

/* Embedded HTML page */
static const char HTML_PAGE[] =
    "<!DOCTYPE html>\n"
    "<html><head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>Move Display</title>\n"
    "<style>\n"
    "  body { background: #111; margin: 0; display: flex; flex-direction: column;\n"
    "         align-items: center; justify-content: center; height: 100vh; }\n"
    "  canvas { image-rendering: pixelated; image-rendering: crisp-edges;\n"
    "           width: 512px; height: 256px; border: 2px solid #333; }\n"
    "  #status { color: #888; font: 12px monospace; margin-top: 8px; }\n"
    "  #status.connected { color: #4a4; }\n"
    "</style>\n"
    "</head><body>\n"
    "<canvas id=\"c\" width=\"128\" height=\"64\"></canvas>\n"
    "<div id=\"status\">connecting...</div>\n"
    "<script>\n"
    "const canvas = document.getElementById('c');\n"
    "const ctx = canvas.getContext('2d');\n"
    "const statusEl = document.getElementById('status');\n"
    "const img = ctx.createImageData(128, 64);\n"
    "let frames = 0, lastFrame = Date.now(), lastMode = 'waiting';\n"
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
    "    if (payload.format === 'gray4_packed') {\n"
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

    if (strncmp(clients[idx].buf, "GET /stream-auto", 16) == 0) {
        const char *sse_header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        if (write(clients[idx].fd, sse_header, strlen(sse_header)) > 0) {
            clients[idx].stream_mode = STREAM_MODE_AUTO;
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

        /* Build fd_set for select */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        int maxfd = srv;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && clients[i].stream_mode == STREAM_MODE_NONE) {
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

                if (shm_ptr && memcmp(shm_ptr, last_display, DISPLAY_SIZE) != 0) {
                    memcpy(last_display, shm_ptr, DISPLAY_SIZE);
                    legacy_changed = 1;
                }

                if (norns_frame_is_live(norns_shm_ptr, now)) {
                    auto_frame = norns_shm_ptr->frame;
                    auto_frame_size = NORNS_FRAME_SIZE;
                    auto_format = NORNS_DISPLAY_FORMAT;
                    auto_source_label = "norns 4-bit";
                    auto_source = AUTO_SOURCE_NORNS;
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
    unified_log_shutdown();
    return 0;
}
