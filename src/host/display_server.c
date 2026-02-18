/*
 * display_server.c - Live display SSE server
 *
 * Streams Move's 128x64 1-bit OLED to a browser via Server-Sent Events.
 * Reads /dev/shm/move-display-live (1024 bytes, written by the shim)
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

#define DEFAULT_PORT       7681
#define SHM_PATH           "/dev/shm/move-display-live"
#define DISPLAY_SIZE       1024
#define MAX_CLIENTS        8
#define POLL_INTERVAL_MS   33    /* ~30 Hz */
#define SHM_RETRY_MS       2000
#define CLIENT_BUF_SIZE    4096

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
typedef struct {
    int fd;
    int streaming;   /* 1 = SSE client, receiving frames */
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
    "let frames = 0, lastFrame = Date.now();\n"
    "\n"
    "function connect() {\n"
    "  const es = new EventSource('/stream');\n"
    "  es.onopen = () => { statusEl.textContent = 'connected'; statusEl.className = 'connected'; };\n"
    "  es.onerror = () => { statusEl.textContent = 'disconnected - reconnecting...';\n"
    "                        statusEl.className = ''; };\n"
    "  es.onmessage = (e) => {\n"
    "    const raw = atob(e.data);\n"
    "    const d = img.data;\n"
    "    for (let page = 0; page < 8; page++) {\n"
    "      for (let col = 0; col < 128; col++) {\n"
    "        const b = raw.charCodeAt(page * 128 + col);\n"
    "        for (let bit = 0; bit < 8; bit++) {\n"
    "          const y = page * 8 + bit;\n"
    "          const idx = (y * 128 + col) * 4;\n"
    "          const on = (b >> bit) & 1;\n"
    "          d[idx] = d[idx+1] = d[idx+2] = on ? 255 : 0;\n"
    "          d[idx+3] = 255;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    ctx.putImageData(img, 0, 0);\n"
    "    frames++;\n"
    "    const now = Date.now();\n"
    "    if (now - lastFrame > 1000) {\n"
    "      statusEl.textContent = 'connected - ' + frames + ' fps';\n"
    "      frames = 0; lastFrame = now;\n"
    "    }\n"
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

/* Close and clear a client slot */
static void client_remove(int idx) {
    if (clients[idx].fd >= 0) {
        if (clients[idx].streaming)
            printf("display: SSE client disconnected (slot %d)\n", idx);
        close(clients[idx].fd);
    }
    clients[idx].fd = -1;
    clients[idx].streaming = 0;
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

    if (strncmp(clients[idx].buf, "GET /stream", 11) == 0) {
        /* SSE endpoint */
        const char *sse_header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        if (write(clients[idx].fd, sse_header, strlen(sse_header)) > 0) {
            clients[idx].streaming = 1;
            printf("display: SSE client connected (slot %d)\n", idx);
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

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    /* Init client slots */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].streaming = 0;
        clients[i].buf_len = 0;
    }

    /* Open shared memory (retry loop) */
    uint8_t *shm_ptr = NULL;
    int shm_fd = -1;
    long long last_shm_attempt = 0;

    /* Listen socket */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("display: socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("display: bind");
        close(srv);
        return 1;
    }
    listen(srv, MAX_CLIENTS);
    fcntl(srv, F_SETFL, O_NONBLOCK);

    printf("display: server listening on port %d\n", port);

    uint8_t last_display[DISPLAY_SIZE];
    memset(last_display, 0, sizeof(last_display));
    long long last_push = 0;

    /* Base64 output buffer: 4/3 * 1024 + padding + SSE framing */
    char b64_buf[2048];
    char sse_buf[2200];

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
                        printf("display: opened %s\n", SHM_PATH);
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
            if (clients[i].fd >= 0 && !clients[i].streaming) {
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
                        clients[i].streaming = 0;
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
            if (clients[i].fd < 0 || clients[i].streaming) continue;
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
        if (shm_ptr) {
            long long now = now_ms();
            if (now - last_push >= POLL_INTERVAL_MS) {
                last_push = now;

                if (memcmp(shm_ptr, last_display, DISPLAY_SIZE) != 0) {
                    memcpy(last_display, shm_ptr, DISPLAY_SIZE);

                    /* Encode frame */
                    int b64_len = base64_encode(last_display, DISPLAY_SIZE, b64_buf);
                    int sse_len = snprintf(sse_buf, sizeof(sse_buf),
                                           "data: %s\n\n", b64_buf);

                    /* Send to all streaming clients */
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].fd < 0 || !clients[i].streaming) continue;
                        int sent = write(clients[i].fd, sse_buf, sse_len);
                        if (sent <= 0) client_remove(i);
                    }
                }
            }
        }
    }

    printf("display: shutting down\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) close(clients[i].fd);
    }
    close(srv);
    if (shm_ptr) munmap(shm_ptr, DISPLAY_SIZE);
    if (shm_fd >= 0) close(shm_fd);
    return 0;
}
