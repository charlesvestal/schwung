/*
 * protocol.c — line-based wire protocol implementation.
 * See protocol.h for the public surface and rationale for the split.
 */

#define _GNU_SOURCE

#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- transport --------------------------------------------------------- */

int protocol_read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n < cap - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) return -1;             /* peer closed */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\r') continue;            /* tolerate CRLF */
        if (c == '\n') {
            out[n] = '\0';
            return (int)n;
        }
        if (c == '\0') return -1;           /* embedded NUL: lossy parse hazard */
        out[n++] = c;
    }
    return -1;                              /* line too long */
}

int protocol_send_line(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

int protocol_reply(int fd, const char *line) {
    char buf[TESTD_LINE_MAX + 2];
    snprintf(buf, sizeof(buf), "%s\n", line);
    return protocol_send_line(fd, buf);
}

int protocol_reply_err(int fd, const char *msg) {
    char buf[TESTD_LINE_MAX];
    snprintf(buf, sizeof(buf), "ERR %s", msg);
    return protocol_reply(fd, buf);
}

/* ---- hex codec --------------------------------------------------------- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int protocol_parse_hex(const char *s, size_t len_chars, uint8_t *out) {
    if (len_chars % 2 != 0) return -1;
    for (size_t i = 0; i < len_chars; i += 2) {
        int hi = hex_nibble(s[i]);
        int lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

void protocol_format_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* ---- command line parsing ---------------------------------------------- */

void protocol_split_command(char *line, char **verb, char **args) {
    *verb = line;
    *args = NULL;
    for (char *p = line; *p; p++) {
        if (isspace((unsigned char)*p)) {
            *p = '\0';
            char *a = p + 1;
            while (*a && isspace((unsigned char)*a)) a++;
            if (*a) *args = a;  /* NULL if only trailing whitespace */
            break;
        }
    }
    /* Uppercase the verb for case-insensitive dispatch. */
    for (char *p = *verb; *p; p++) *p = (char)toupper((unsigned char)*p);
}
