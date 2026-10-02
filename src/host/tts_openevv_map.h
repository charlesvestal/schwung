/*
 * openevv (Eloquence) — the pure mappings, header-only so tests/host can run
 * them without the engine, the shim or a device.
 */

#ifndef TTS_OPENEVV_MAP_H
#define TTS_OPENEVV_MAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * The shared Speed row is a MULTIPLIER (0.5 .. 6.0, 1.0 = normal) because
 * eSpeak and Flite read it that way. ECI speed is 0..250 with 50 as every
 * enus preset's own speed, so 1.0x lands on the preset rate and the top of
 * the row reaches the engine's ceiling.
 */
static inline int tts_evv_speed_from_mult(float mult) {
    int s = (int)(mult * 50.0f + 0.5f);
    if (s < 0) s = 0;
    if (s > 250) s = 250;
    return s;
}

/* Unicode code points 0x80..0x9F that Windows-1252 places, by byte. */
static inline int tts_evv_cp1252_high(uint32_t cp) {
    switch (cp) {
    case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83;
    case 0x201E: return 0x84; case 0x2026: return 0x85; case 0x2020: return 0x86;
    case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89;
    case 0x0160: return 0x8A; case 0x2039: return 0x8B; case 0x0152: return 0x8C;
    case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
    case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95;
    case 0x2013: return 0x96; case 0x2014: return 0x97; case 0x02DC: return 0x98;
    case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B;
    case 0x0153: return 0x9C; case 0x017E: return 0x9E; case 0x0178: return 0x9F;
    }
    return -1;
}

/*
 * UTF-8 -> Windows-1252, which is what eciAddText reads. Anything 1252 cannot
 * say becomes a space, and so does a malformed sequence and every control
 * byte, so nothing reaching the engine is a byte it could misread. The
 * backtick is spoken text here, never an annotation: eciInputType stays 0.
 *
 * Always NUL-terminates when out_len > 0. Returns the bytes written, not
 * counting the terminator.
 */
static inline size_t tts_evv_utf8_to_cp1252(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p && o + 1 < out_len) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80)              { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
        else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
        else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
        else                           { cp = 0xFFFD; n = 1; }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { cp = 0xFFFD; n = i; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += n;

        int b;
        if (cp < 0x20 || cp == 0x7F)      b = ' ';
        else if (cp < 0x80)               b = (int)cp;
        else if (cp >= 0xA0 && cp <= 0xFF) b = (int)cp;
        else                              b = tts_evv_cp1252_high(cp);
        if (b < 0) b = ' ';
        out[o++] = (char)b;
    }
    out[o] = '\0';
    return o;
}

#endif /* TTS_OPENEVV_MAP_H */
