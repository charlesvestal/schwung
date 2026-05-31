// SPDX-License-Identifier: MIT
//
// RFC 6455 WebSocket server — see ws_server.h for usage.
//
// Implementation notes:
//   * Single accept/poll thread services all clients via select(2).
//   * Per-client state: socket + a small recv buffer + a small send queue.
//   * Frame parsing: opcodes 0x1 (text), 0x2 (binary), 0x8 (close), 0x9 (ping),
//     0xA (pong). We don't support fragmentation — large frames are reassembled
//     up to a fixed cap (4 KiB) and dropped beyond it.
//   * The HTTP upgrade handshake speaks just enough HTTP/1.1 to satisfy
//     browsers' wscat-ish clients. No virtual hosts, no path routing.

#include "ws_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS    4
#define RECV_BUF_SIZE  4096
#define SEND_BUF_SIZE  4096
#define WS_MAGIC       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// ============================================================================
// SHA-1 (RFC 3174) — needed for the WebSocket handshake
// ============================================================================
//
// Compact public-domain-style implementation. Only used for the handshake;
// not on the hot path.

typedef struct {
    uint32_t h[5];
    uint64_t length;
    uint8_t  buffer[64];
    size_t   buffer_len;
} sha1_ctx;

static uint32_t sha1_rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->length = 0; c->buffer_len = 0;
}

static void sha1_block(sha1_ctx *c, const uint8_t *blk) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16) |
               ((uint32_t)blk[i*4+2] << 8) | (uint32_t)blk[i*4+3];
    }
    for (int i = 16; i < 80; i++) w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a=c->h[0], b=c->h[1], cc=c->h[2], d=c->h[3], e=c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | ((~b) & d);             k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                        k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);     k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                        k = 0xCA62C1D6; }
        uint32_t t = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = sha1_rotl(b, 30); b = a; a = t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e;
}

static void sha1_update(sha1_ctx *c, const uint8_t *data, size_t len) {
    c->length += len * 8;
    while (len > 0) {
        size_t take = 64 - c->buffer_len;
        if (take > len) take = len;
        memcpy(c->buffer + c->buffer_len, data, take);
        c->buffer_len += take;
        data += take; len -= take;
        if (c->buffer_len == 64) { sha1_block(c, c->buffer); c->buffer_len = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    c->buffer[c->buffer_len++] = 0x80;
    if (c->buffer_len > 56) {
        while (c->buffer_len < 64) c->buffer[c->buffer_len++] = 0;
        sha1_block(c, c->buffer); c->buffer_len = 0;
    }
    while (c->buffer_len < 56) c->buffer[c->buffer_len++] = 0;
    for (int i = 7; i >= 0; i--) c->buffer[c->buffer_len++] = (uint8_t)(c->length >> (i*8));
    sha1_block(c, c->buffer);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

// ============================================================================
// base64 encode (no padding-stripping; RFC 4648)
// ============================================================================

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t inlen, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < inlen; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16);
        if (i + 1 < inlen) v |= ((uint32_t)in[i+1] << 8);
        if (i + 2 < inlen) v |= (uint32_t)in[i+2];
        out[o++] = b64_chars[(v >> 18) & 0x3F];
        out[o++] = b64_chars[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < inlen) ? b64_chars[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < inlen) ? b64_chars[v & 0x3F]       : '=';
    }
    out[o] = '\0';
    return o;
}

// ============================================================================
// Client state
// ============================================================================

typedef struct {
    int    fd;
    int    handshaken;
    uint8_t recv_buf[RECV_BUF_SIZE];
    size_t  recv_len;
    uint8_t send_buf[SEND_BUF_SIZE];
    size_t  send_len;
} ws_client_t;

// ============================================================================
// Server state
// ============================================================================

struct ws_server {
    int             listen_fd;
    pthread_t       thread;
    int             running;          // 0/1
    ws_on_frame_fn  on_frame;
    void           *on_frame_ctx;
    pthread_mutex_t lock;             // protects clients[] for broadcast
    ws_client_t     clients[MAX_CLIENTS];
};

// ============================================================================
// Handshake
// ============================================================================

static void handshake(ws_client_t *c, const char *sec_key) {
    char concat[128];
    int n = snprintf(concat, sizeof(concat), "%s%s", sec_key, WS_MAGIC);
    if (n <= 0 || n >= (int)sizeof(concat)) return;

    sha1_ctx sc; sha1_init(&sc);
    sha1_update(&sc, (const uint8_t *)concat, (size_t)n);
    uint8_t digest[20]; sha1_final(&sc, digest);
    char accept[40]; b64_encode(digest, 20, accept);

    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (rlen > 0) {
        ssize_t w = write(c->fd, resp, (size_t)rlen);
        (void)w;
    }
    c->handshaken = 1;
}

// Try to parse and respond to the initial HTTP upgrade. Returns 1 on success
// (handshake complete), 0 if data is incomplete (wait for more), -1 on error.
static int try_handshake(ws_client_t *c) {
    // Need terminator \r\n\r\n
    if (c->recv_len < 4) return 0;
    char *blob = (char *)c->recv_buf;
    size_t maxlook = c->recv_len < RECV_BUF_SIZE - 1 ? c->recv_len : RECV_BUF_SIZE - 1;
    blob[maxlook] = '\0';
    char *end = strstr(blob, "\r\n\r\n");
    if (!end) return c->recv_len >= RECV_BUF_SIZE - 1 ? -1 : 0;

    // Find Sec-WebSocket-Key
    const char *key_hdr = strcasestr(blob, "Sec-WebSocket-Key:");
    if (!key_hdr) return -1;
    key_hdr += strlen("Sec-WebSocket-Key:");
    while (*key_hdr == ' ' || *key_hdr == '\t') key_hdr++;
    char key[128] = {0};
    int ki = 0;
    while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && ki < (int)sizeof(key) - 1) {
        key[ki++] = *key_hdr++;
    }
    key[ki] = '\0';

    handshake(c, key);

    // Consume request bytes (move any tail to start)
    size_t consumed = (size_t)(end - blob) + 4;
    if (consumed < c->recv_len) {
        memmove(c->recv_buf, c->recv_buf + consumed, c->recv_len - consumed);
        c->recv_len -= consumed;
    } else {
        c->recv_len = 0;
    }
    return 1;
}

// ============================================================================
// Frame parsing (inbound, masked client frames)
// ============================================================================

// Try to extract one frame from the recv buffer. Returns:
//   > 0 — bytes consumed (frame parsed)
//   = 0 — incomplete; need more data
//   < 0 — protocol error; close connection
static int parse_inbound_frame(ws_client_t *c, ws_on_frame_fn on_frame, void *ctx) {
    if (c->recv_len < 2) return 0;
    uint8_t b0 = c->recv_buf[0];
    uint8_t b1 = c->recv_buf[1];
    int fin     = (b0 >> 7) & 1;
    int opcode  = b0 & 0x0F;
    int masked  = (b1 >> 7) & 1;
    uint64_t plen = b1 & 0x7F;
    size_t header_len = 2;

    if (!masked) return -1;  // client→server MUST be masked
    if (!fin)    return -1;  // we don't accept fragmentation

    if (plen == 126) {
        if (c->recv_len < 4) return 0;
        plen = ((uint64_t)c->recv_buf[2] << 8) | c->recv_buf[3];
        header_len = 4;
    } else if (plen == 127) {
        if (c->recv_len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | c->recv_buf[2 + i];
        header_len = 10;
    }
    if (plen > RECV_BUF_SIZE - 14) return -1;  // too big

    if (c->recv_len < header_len + 4 + plen) return 0;
    uint8_t mask[4];
    memcpy(mask, c->recv_buf + header_len, 4);
    uint8_t *payload = c->recv_buf + header_len + 4;
    for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];

    // Dispatch on opcode
    if (opcode == 0x8) {           // close
        return -1;
    } else if (opcode == 0x9) {    // ping → reply with pong
        uint8_t hdr[2] = { 0x8A, (uint8_t)plen };
        ssize_t w = write(c->fd, hdr, 2);
        if (plen > 0) w = write(c->fd, payload, (size_t)plen);
        (void)w;
    } else if (opcode == 0xA) {    // pong — ignore
    } else if (opcode == 0x1 || opcode == 0x2) {
        if (on_frame) on_frame(ctx, payload, (size_t)plen, opcode == 0x2);
    }

    return (int)(header_len + 4 + plen);
}

// ============================================================================
// Outbound: send a binary frame, unmasked (server→client)
// ============================================================================

static void send_binary_frame(ws_client_t *c, const uint8_t *data, size_t len) {
    if (len > 65535) return;  // we don't bother with 64-bit lengths
    uint8_t hdr[4];
    size_t hlen;
    hdr[0] = 0x82;  // FIN + binary
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    }
    // Best-effort — drop if would block.
    if (c->send_len + hlen + len > SEND_BUF_SIZE) return;
    memcpy(c->send_buf + c->send_len, hdr, hlen);
    c->send_len += hlen;
    memcpy(c->send_buf + c->send_len, data, len);
    c->send_len += len;
}

// ============================================================================
// Listener thread
// ============================================================================

static void client_close(ws_client_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->handshaken = 0;
    c->recv_len = 0;
    c->send_len = 0;
}

static void *ws_thread(void *arg) {
    ws_server_t *s = (ws_server_t *)arg;
    while (s->running) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        FD_SET(s->listen_fd, &rfds);
        int maxfd = s->listen_fd;
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s->clients[i].fd >= 0) {
                FD_SET(s->clients[i].fd, &rfds);
                if (s->clients[i].send_len > 0) FD_SET(s->clients[i].fd, &wfds);
                if (s->clients[i].fd > maxfd) maxfd = s->clients[i].fd;
            }
        }
        pthread_mutex_unlock(&s->lock);

        struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
        int n = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Accept
        if (n > 0 && FD_ISSET(s->listen_fd, &rfds)) {
            int cfd = accept(s->listen_fd, NULL, NULL);
            if (cfd >= 0) {
                int slot = -1;
                pthread_mutex_lock(&s->lock);
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (s->clients[i].fd < 0) { slot = i; break; }
                }
                if (slot < 0) {
                    pthread_mutex_unlock(&s->lock);
                    close(cfd);
                } else {
                    int yes = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    fcntl(cfd, F_SETFL, O_NONBLOCK);
                    s->clients[slot].fd = cfd;
                    s->clients[slot].handshaken = 0;
                    s->clients[slot].recv_len = 0;
                    s->clients[slot].send_len = 0;
                    pthread_mutex_unlock(&s->lock);
                }
            }
        }

        // Per-client I/O
        for (int i = 0; i < MAX_CLIENTS; i++) {
            ws_client_t *c = &s->clients[i];
            if (c->fd < 0) continue;

            if (FD_ISSET(c->fd, &rfds)) {
                ssize_t r = recv(c->fd,
                                 c->recv_buf + c->recv_len,
                                 RECV_BUF_SIZE - c->recv_len, 0);
                if (r <= 0) {
                    pthread_mutex_lock(&s->lock);
                    client_close(c);
                    pthread_mutex_unlock(&s->lock);
                    continue;
                }
                c->recv_len += (size_t)r;

                if (!c->handshaken) {
                    int hs = try_handshake(c);
                    if (hs < 0) {
                        pthread_mutex_lock(&s->lock);
                        client_close(c);
                        pthread_mutex_unlock(&s->lock);
                        continue;
                    }
                }

                while (c->handshaken) {
                    int consumed = parse_inbound_frame(c, s->on_frame, s->on_frame_ctx);
                    if (consumed < 0) {
                        pthread_mutex_lock(&s->lock);
                        client_close(c);
                        pthread_mutex_unlock(&s->lock);
                        break;
                    }
                    if (consumed == 0) break;
                    if ((size_t)consumed < c->recv_len) {
                        memmove(c->recv_buf, c->recv_buf + consumed,
                                c->recv_len - consumed);
                    }
                    c->recv_len -= (size_t)consumed;
                }
            }

            if (c->fd >= 0 && c->send_len > 0 && FD_ISSET(c->fd, &wfds)) {
                ssize_t w = send(c->fd, c->send_buf, c->send_len, 0);
                if (w > 0) {
                    if ((size_t)w < c->send_len) {
                        memmove(c->send_buf, c->send_buf + w, c->send_len - w);
                    }
                    c->send_len -= (size_t)w;
                } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    pthread_mutex_lock(&s->lock);
                    client_close(c);
                    pthread_mutex_unlock(&s->lock);
                }
            }
        }
    }
    return NULL;
}

// ============================================================================
// Public API
// ============================================================================

ws_server_t *ws_server_start(int port, ws_on_frame_fn on_frame, void *ctx) {
    ws_server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->on_frame = on_frame;
    s->on_frame_ctx = ctx;
    pthread_mutex_init(&s->lock, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) s->clients[i].fd = -1;

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) { free(s); return NULL; }

    int yes = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
    addr.sin_port = htons((uint16_t)port);

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ws_server: bind :%d failed: %s\n", port, strerror(errno));
        close(s->listen_fd); free(s); return NULL;
    }
    if (listen(s->listen_fd, MAX_CLIENTS) < 0) {
        close(s->listen_fd); free(s); return NULL;
    }
    fcntl(s->listen_fd, F_SETFL, O_NONBLOCK);

    s->running = 1;
    if (pthread_create(&s->thread, NULL, ws_thread, s) != 0) {
        close(s->listen_fd); free(s); return NULL;
    }
    fprintf(stderr, "ws_server: listening on :%d\n", port);
    return s;
}

void ws_server_stop(ws_server_t *s) {
    if (!s) return;
    s->running = 0;
    pthread_join(s->thread, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s->clients[i].fd >= 0) close(s->clients[i].fd);
    }
    close(s->listen_fd);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void ws_server_broadcast(ws_server_t *s, const uint8_t *data, size_t len) {
    if (!s || !data || len == 0) return;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s->clients[i].fd >= 0 && s->clients[i].handshaken) {
            send_binary_frame(&s->clients[i], data, len);
        }
    }
    pthread_mutex_unlock(&s->lock);
}
