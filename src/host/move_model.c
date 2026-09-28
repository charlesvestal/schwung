/* move_model.c -- see move_model.h for what this is and the three rules. */
#define _GNU_SOURCE
#include "move_model.h"

#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static int vp_in(const uint64_t *v, int n, uint64_t x)
{
    for (int i = 0; i < n; i++) if (v[i] == x) return 1;
    return 0;
}

int mm_history_read(mm_read_fn rd, void *ctx, uint64_t obj, const mm_hist_vps_t *vps, move_model_t *m)
{
    m->hist_valid = 0;
    m->hist_undo_node = m->hist_undo_nbr = m->hist_redo_node = m->hist_redo_nbr = 0;
    m->hist_size = 0;
    if (!obj || !vps) return -1;
    uint64_t w[8];
    if (rd(ctx, obj, w, sizeof w)) return -1;
    if (!vp_in(vps->hist, vps->nh, w[0]) || !vp_in(vps->store, vps->ns, w[2])) return -1;
    const uint64_t sentinel = obj + 0x20;
    const uint64_t size = w[6], it_redo = w[7];
    if (size > 1000000) return -1;
    uint64_t last = w[4];                           /* sentinel.prev: the tail */
    uint64_t node[7];
    if (it_redo != sentinel) {
        if (rd(ctx, it_redo, node, sizeof node) || !vp_in(vps->tx, vps->nt, node[2])) return -1;
        m->hist_redo_node = it_redo;
        m->hist_redo_nbr = node[6];
        last = node[0];                             /* prev(_it_redo) */
    }
    if (last != sentinel) {
        if (!size || rd(ctx, last, node, sizeof node) || !vp_in(vps->tx, vps->nt, node[2])) return -1;
        m->hist_undo_node = last;
        m->hist_undo_nbr = node[6];
    }
    m->hist_size = (uint32_t)size;
    m->hist_valid = 1;
    return 0;
}

static uint32_t be32(const uint8_t *r) { return (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]; }
static uint64_t be64(const uint8_t *r) { return (uint64_t)be32(r) << 32 | be32(r + 4); }
static double be_f64(const uint8_t *r) { uint64_t u = be64(r); double d; memcpy(&d, &u, 8); return d; }

#define MM_NOTE_HEAD     29             /* pitch, start, dur, vel, offvel, flag */
#define MM_LANE_PITCH    (-2)
#define MM_LANE_PRESSURE (-1)
#define MM_PITCH_SCALE   (8191.0 / 48.0) /* 14-bit, +-48 semitones: 170.6458 per semitone */
#define MM_LANES_MAX     16
#define MM_POINTS_MAX    65536

int mm_decode_notes_buf(const uint8_t *raw, size_t len, mm_note_t *out, int max,
                        mm_expr_point_t *pool, int pool_max)
{
    if (!raw && len) return -1;
    int n = 0, used = 0;
    size_t o = 0;
    while (o < len) {
        if (len - o < MM_NOTE_HEAD + 8 || n >= max) return -1;
        const uint8_t *r = raw + o;
        mm_note_t nt;
        memset(&nt, 0, sizeof nt);
        nt.pitch = (int32_t)be32(r);
        nt.start = be_f64(r + 4);
        nt.dur = be_f64(r + 12);
        { uint32_t w = be32(r + 20); float f; memcpy(&f, &w, 4); nt.vel = f; }
        nt.pressure_first = -1;
        size_t p = o + MM_NOTE_HEAD;
        const uint32_t lanes = be32(raw + p);
        if (lanes == 0) {                                   /* a plain note: i64 id */
            nt.id = (int64_t)be64(raw + p);
            p += 8;
        } else {
            if (lanes > MM_LANES_MAX) return -1;
            p += 4;
            for (uint32_t l = 0; l < lanes; l++) {
                if (len - p < 8) return -1;
                const int32_t type = (int32_t)be32(raw + p);
                const uint32_t cnt = be32(raw + p + 4);
                p += 8;
                if (cnt > MM_POINTS_MAX || (size_t)cnt * 16u > len - p) return -1;
                if (type == MM_LANE_PITCH) {
                    nt.has_pitch = cnt > 0;
                    /* The pitch at the note's start: the point at t=0, else the first. */
                    for (uint32_t k = 0; k < cnt; k++) {
                        const uint8_t *pt = raw + p + 16u * k;
                        if (k == 0 || be_f64(pt) == 0.0) nt.pitch_offset = be_f64(pt + 8) / MM_PITCH_SCALE;
                        if (be_f64(pt) == 0.0) break;
                    }
                } else if (type == MM_LANE_PRESSURE) {
                    if (pool) {
                        if (cnt > (uint32_t)(pool_max - used)) return -1;
                        nt.pressure_first = used;
                        for (uint32_t k = 0; k < cnt; k++) {
                            pool[used + k].time = be_f64(raw + p + 16u * k);
                            pool[used + k].value = be_f64(raw + p + 16u * k + 8);
                        }
                        used += (int)cnt;
                    }
                    nt.pressure_count = (int)cnt;
                } else {
                    return -1;                              /* a lane we do not know */
                }
                p += 16u * cnt;
            }
            if (len - p < 4) return -1;
            nt.id = (int64_t)be32(raw + p);
            p += 4;
        }
        out[n++] = nt;
        o = p;
    }
    return n;                                               /* landed exactly on the end */
}

void mm_decode_live_recs(const uint8_t *raw, int n, mm_live_rec_t *out)
{
    for (int k = 0; k < n; k++) {
        const uint8_t *r = raw + (size_t)k * MM_LIVE_REC_BYTES;
        memcpy(&out[k].frame, r, 8);                   /* little-endian, native on aarch64 */
        memcpy(&out[k].ep, r + 8, 4);
        memcpy(&out[k].a, r + 16, 4);
        memcpy(&out[k].b, r + 20, 4);
        memcpy(&out[k].id, r + 24, 8);
        memcpy(&out[k].kind, r + 32, 4);
    }
}

#if defined(__linux__) && !defined(MOVE_MODEL_PURE_ONLY)
#include <elf.h>
#include "unified_log.h"
#endif

/* ====================================================================== */
/* Pure pieces                                                            */
/* ====================================================================== */

int mm_decode_member_stub(const uint32_t insn[4], uint32_t *off)
{
    uint32_t acc = 0, pend = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t w = insn[k];
        if (w == 0xd65f03c0u) { *off = acc; return 0; }              /* ret */
        if ((w & 0x7f80001fu) == 0x52800008u) {                      /* movz w8/x8,#imm16{,lsl} */
            pend = ((w >> 5) & 0xffffu) << (16 * ((w >> 21) & 3u));
            continue;
        }
        if (w == 0x8b080000u) { acc += pend; continue; }             /* add x0,x0,x8 */
        if ((w & 0xff8003ffu) == 0x91000000u) {                      /* add x0,x0,#imm{,lsl12} */
            uint32_t imm = (w >> 10) & 0xfffu;
            if (w & (1u << 22)) imm <<= 12;
            acc += imm;
            continue;
        }
        return -1;
    }
    return -1;
}

int mm_sso_string(mm_read_fn rd, void *ctx, const uint8_t raw[24], char *out, size_t cap)
{
    if (!cap) return -1;
    if (raw[0] & 1) {                         /* long: {cap|1, size, data} */
        uint64_t size, ptr;
        memcpy(&size, raw + 8, 8);
        memcpy(&ptr, raw + 16, 8);
        if (size >= cap) return -1;
        if (rd(ctx, ptr, out, size) != 0) return -1;
        out[size] = 0;
        return 0;
    }
    size_t size = raw[0] >> 1;                /* short: {size<<1, chars[23]} */
    if (size > 22 || size >= cap) return -1;
    memcpy(out, raw + 1, size);
    out[size] = 0;
    return 0;
}

static int in_img(uint64_t p, uint64_t lo, uint64_t hi) { return p >= lo && p < hi; }

static int plausible_heap_ptr(uint64_t p, uint64_t lo, uint64_t hi)
{
    return p >= 0x10000 && p < 0x8000000000ull && !(p & 7) && !in_img(p, lo, hi);
}

int mm_tree_elems(mm_read_fn rd, void *ctx, uint64_t hdr, uint64_t img_lo, uint64_t img_hi,
                  uint64_t *out, int max)
{
    uint64_t h[3];
    if (rd(ctx, hdr, h, sizeof h) != 0) return -1;
    uint64_t n = h[0], end = hdr + 8, size = h[2];
    if (size > 4096) return -1;
    int count = 0;
    for (int guard = 0; n && n != end; guard++) {
        if (guard > 4096 || !plausible_heap_ptr(n, img_lo, img_hi)) return -1;
        uint64_t node[20];   /* left,right,parent,color, then key + wrapper */
        if (rd(ctx, n, node, sizeof node) != 0) return -1;
        uint64_t elem = 0;
        for (int k = 5; k < 19; k++) {        /* k=4 is the key's own vptr */
            if (in_img(node[k], img_lo, img_hi)) { elem = node[k + 1]; break; }
        }
        if (!plausible_heap_ptr(elem, img_lo, img_hi)) return -1;
        if (count < max) out[count] = elem;
        count++;
        /* successor */
        if (node[1]) {
            n = node[1];
            for (int g = 0; g < 64; g++) {
                uint64_t l;
                if (rd(ctx, n, &l, 8) != 0) return -1;
                if (!l) break;
                n = l;
            }
        } else {
            uint64_t cur = n, p = node[2];
            for (int g = 0; g < 64 && p; g++) {
                uint64_t pl;
                if (rd(ctx, p, &pl, 8) != 0) return -1;
                if (pl == cur) break;
                cur = p;
                if (rd(ctx, cur + 16, &p, 8) != 0) return -1;
            }
            n = p;
        }
    }
    if ((uint64_t)count != size) return -1;   /* a torn walk; the caller retries */
    return count;
}

int mm_parse_resolution(const char *nm, double *beats, uint8_t *trip)
{
    int num = 0, den = 0;
    char t = 0;
    if (sscanf(nm, "%d/%d%c", &num, &den, &t) < 2 || num <= 0 || den <= 0) return -1;
    *trip = (t == 't');
    *beats = 4.0 * num / den * (*trip ? 2.0 / 3.0 : 1.0);
    return 0;
}


double mm_clip_position(const mm_clip_t *c, double start_beats, double song_beats)
{
    if (!c || !c->exists) return -1.0;
    double pos = c->region_start + (song_beats - start_beats);
    if (!c->loop_on) return (pos < c->region_end) ? pos : -1.0;
    double len = c->loop_end - c->loop_start;
    if (!(len > 1e-9)) return -1.0;
    if (pos >= c->loop_end) pos = c->loop_start + fmod(pos - c->loop_start, len);
    return pos;
}

#if defined(__linux__) && !defined(MOVE_MODEL_PURE_ONLY)   /* the runtime half; tests/host builds only the pure half (-DMOVE_MODEL_PURE_ONLY) */
/* ====================================================================== */
/* Runtime: memory access                                                 */
/* ====================================================================== */

static pid_t g_pid;

static int self_read(void *ctx, uint64_t addr, void *buf, size_t n)
{
    (void)ctx;
    if (!n) return 0;
    if (addr < 0x10000 || addr >= 0x8000000000ull) return -1;
    struct iovec local = { buf, n }, remote = { (void *)(uintptr_t)addr, n };
    return process_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)n ? 0 : -1;
}
#define RD(a, p, n) self_read(NULL, (a), (p), (n))

static uint64_t rq(uint64_t a) { uint64_t v = 0; if (RD(a, &v, 8)) return 0; return v; }

typedef struct { uint64_t lo, hi; } range_t;
static range_t g_img, g_heap;
/* The image's mapped SEGMENTS. The span lo..hi has unmapped gaps between them,
 * and a chunked read that straddles one fails whole -- which is exactly where
 * .data.rel.ro (the typeinfo) begins. Scans walk these, never the span. */
static range_t g_seg[16];
static int g_nseg;

static int parse_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    g_img.lo = g_img.hi = g_heap.lo = g_heap.hi = 0;
    g_nseg = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        char perm[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perm) != 3) continue;
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == ' ')) line[--L] = 0;
        if (L > 13 && strcmp(line + L - 13, "/MoveOriginal") == 0) {
            if (!g_img.lo || a < g_img.lo) g_img.lo = a;
            if (b > g_img.hi) g_img.hi = b;
            if (perm[0] == 'r' && g_nseg < 16) { g_seg[g_nseg].lo = a; g_seg[g_nseg].hi = b; g_nseg++; }
        } else if (strstr(line, "[heap]")) {
            g_heap.lo = a; g_heap.hi = b;
        }
    }
    fclose(f);
    return (g_img.lo && g_heap.lo) ? 0 : -1;
}

/* ---- build id (gates the two firmware-pinned offsets) ---------------- */

static const char *KNOWN_BUILD = "fc05b06da749387587aca5db39f24612a5c2a154"; /* Move 2.1.0 a6233f89a28a */

static int read_build_id(char *out, size_t cap)
{
    Elf64_Ehdr eh;
    if (RD(g_img.lo, &eh, sizeof eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG)) return -1;
    for (int i = 0; i < eh.e_phnum && i < 32; i++) {
        Elf64_Phdr ph;
        if (RD(g_img.lo + eh.e_phoff + (uint64_t)i * eh.e_phentsize, &ph, sizeof ph)) return -1;
        if (ph.p_type != PT_NOTE || ph.p_filesz > 4096) continue;
        uint8_t buf[4096];
        if (RD(g_img.lo + ph.p_vaddr, buf, ph.p_filesz)) continue;
        for (size_t o = 0; o + 12 <= ph.p_filesz;) {
            Elf64_Nhdr nh;
            memcpy(&nh, buf + o, sizeof nh);
            size_t np = o + 12, dp = np + ((nh.n_namesz + 3) & ~3u);
            if (nh.n_type == NT_GNU_BUILD_ID && dp + nh.n_descsz <= ph.p_filesz &&
                nh.n_descsz * 2 + 1 <= cap) {
                for (unsigned k = 0; k < nh.n_descsz; k++) sprintf(out + 2 * k, "%02x", buf[dp + k]);
                return 0;
            }
            o = dp + ((nh.n_descsz + 3) & ~3u);
        }
    }
    return -1;
}

/* ---- the flip class registry ------------------------------------------ */

/* flip::ClassBase in this build: {vptr, name, super, ?, members.begin, .end, .cap}
 * Member (48 bytes): {ClassBase *type, std::string name, Representative{vptr, stub}} */
#define CB_NAME    0x08
#define CB_SUPER   0x10
#define CB_MBEGIN  0x20
#define CB_MEND    0x28
#define MEMBER_SZ  48

enum {
    C_SONG, C_TRANSPORT, C_PARAMETER, C_TIMESIG, C_TRACKLIST, C_TRACK, C_CLIPS,
    C_PLAYSTATE, C_CLIPSLOT, C_SESSIONCLIP, C_CLIP, C_REGION, C_LOOP, C_MIDICONTENT, C_ABSDEV,
    C_MIXPARAMS, C_ENVLIST, C_ENVELOPE, C_AUTOMATION, C_MIDISTATE, C_COUNT
};
static const char *CLASS_NAMES[C_COUNT] = {
    "live.Song", "live.Transport", "live.Parameter", "live.TimeSignature", "live.TrackList",
    "live.Track", "live.Clips", "live.PlayingState", "live.ClipSlot", "live.SessionClip",
    "live.Clip", "live.ClipRegion", "live.Loop", "live.MidiClipContent", "live.AbstractDevice",
    "live.AudioMixerParameters", "live.ClipEnvelopeList", "live.ClipEnvelope", "live.Automation",
    "live.MidiTrackState",
};
static uint64_t g_cls[C_COUNT];

/* flip::EnumClass "StepEditorResolution": {vptr, name, enumerators{begin,end}},
 * each enumerator {const char *name, int64 value}. Measured on 2.1.0:
 * 0=1/8t 1=1/16 2=1/16t 3=1/32 4=1/32t 5=1/64. Resolved by name at runtime
 * like everything else, and parsed from the name, so no table is copied here. */
#define RES_MAX 16
static double g_res_beats[RES_MAX];
static uint8_t g_res_triplet[RES_MAX];

static void resolve_enum_at(uint64_t ec)
{
    uint64_t b = rq(ec + 16), e = rq(ec + 24);
    if (e < b || e - b > RES_MAX * 16) return;
    for (uint64_t a = b; a < e; a += 16) {
        char nm[16] = "";
        uint64_t np = rq(a), v = rq(a + 8);
        if (v >= RES_MAX || RD(np, nm, sizeof nm - 1)) continue;
        mm_parse_resolution(nm, &g_res_beats[v], &g_res_triplet[v]);
    }
}

typedef struct { int cls; const char *member; uint32_t off; } moff_t;
enum {
    O_SONG_TRANSPORT, O_SONG_TRACKS, O_TR_TEMPO, O_TR_TIMESIG, O_TR_CTRLMSG, O_PARAM_VALUE,
    O_TS_UPPER, O_TS_LOWER, O_TL_TRACKS, O_TRACK_COMPONENTS, O_TRACK_SELECTED, O_CLIPS_SLOTS,
    O_CLIPS_PLAYSTATE, O_PS_MODE, O_PS_SLOT, O_PS_START, O_SLOT_CLIP, O_SC_CLIP, O_CLIP_REGION,
    O_CLIP_TIMESIG, O_CLIP_CONTENT, O_RG_START, O_RG_END, O_RG_LOOP, O_LOOP_START, O_LOOP_END,
    O_LOOP_ON, O_MC_SCROLL, O_SONG_STEPRES, O_TRACK_MIXER, O_DEV_COMPONENTS,
    O_MIX_VOLUME, O_MIX_PAN, O_MIX_SOLO, O_MIX_SPEAKER, O_MC_NOTES, O_SC_ENVELOPES,
    O_ENVLIST_ENVS, O_ENV_AUTOMATION, O_AUTO_BREAKPOINTS, O_AUTO_PARAM, O_MTS_OUT_EP, O_COUNT
};
static moff_t g_off[O_COUNT] = {
    {C_SONG, "mTransport", 0}, {C_SONG, "mTracks", 0},
    {C_TRANSPORT, "mTempo", 0}, {C_TRANSPORT, "mTimeSignature", 0},
    {C_TRANSPORT, "mTransportControlMessage", 0}, {C_PARAMETER, "mManualValue", 0},
    {C_TIMESIG, "mUpper", 0}, {C_TIMESIG, "mLower", 0}, {C_TRACKLIST, "mTracks", 0},
    {C_TRACK, "mComponents", 0}, {C_TRACK, "mIsSelected", 0},
    {C_CLIPS, "mClipSlots", 0}, {C_CLIPS, "mPlayingState", 0},
    {C_PLAYSTATE, "mMode", 0}, {C_PLAYSTATE, "mPlayingClipSlot", 0},
    {C_PLAYSTATE, "mSessionClipStartTime", 0}, {C_CLIPSLOT, "mClip", 0},
    {C_SESSIONCLIP, "mClip", 0}, {C_CLIP, "mClipRegion", 0}, {C_CLIP, "mTimeSignature", 0},
    {C_CLIP, "mContent", 0}, {C_REGION, "mStart", 0}, {C_REGION, "mEnd", 0},
    {C_REGION, "mLoop", 0}, {C_LOOP, "mStart", 0}, {C_LOOP, "mEnd", 0},
    {C_LOOP, "mIsEnabled", 0}, {C_MIDICONTENT, "mStepEditorScrollPosition", 0},
    {C_SONG, "mStepEditorResolution", 0}, {C_TRACK, "mTrackMixerDevice", 0},
    {C_ABSDEV, "mComponents", 0}, {C_MIXPARAMS, "mVolume", 0}, {C_MIXPARAMS, "mPan", 0},
    {C_MIXPARAMS, "mSolo", 0}, {C_MIXPARAMS, "mSpeakerOn", 0}, {C_MIDICONTENT, "mNotes", 0},
    {C_SESSIONCLIP, "mClipEnvelopes", 0}, {C_ENVLIST, "mClipEnvelopes", 0}, {C_ENVELOPE, "mAutomation", 0},
    {C_AUTOMATION, "mBreakpoints", 0}, {C_AUTOMATION, "mpParameter", 0},
    {C_MIDISTATE, "mMidiOutputEndpoint", 0},
};

/* flip basic-type value slots, measured: Type ends at +0x64 (a 4-byte
 * modification count), Bool's value is the next byte, the 8-byte ones align
 * to +0x68. ObjectRef's Ref is {user, actor, obj} from +0x70. Type's own Ref
 * sits at +0x18, so an object's id is at +0x28. */
#define V_BOOL   0x64
#define V_WORD   0x68
#define V_REFOBJ 0x80
#define OBJ_ID   0x28

/* Firmware-pinned (KNOWN_BUILD only): inside the transport control message. */
#define CTRL_PLAYING 0xb0
#define CTRL_BEATS   0x158

static int find_member(uint64_t cls, const char *name, uint32_t *off, int depth)
{
    if (!cls || depth > 8) return -1;
    uint64_t b = rq(cls + CB_MBEGIN), e = rq(cls + CB_MEND);
    if (e < b || e - b > 64 * MEMBER_SZ) return -1;
    for (uint64_t m = b; m < e; m += MEMBER_SZ) {
        uint8_t raw[MEMBER_SZ];
        char nm[96];
        if (RD(m, raw, sizeof raw)) return -1;
        if (mm_sso_string(self_read, NULL, raw + 8, nm, sizeof nm)) continue;
        if (strcmp(nm, name)) continue;
        uint64_t stub;
        uint32_t insn[4];
        memcpy(&stub, raw + 40, 8);
        if (RD(stub, insn, sizeof insn)) return -1;
        return mm_decode_member_stub(insn, off);
    }
    return find_member(rq(cls + CB_SUPER), name, off, depth + 1);
}

/* One pass over the first 64 MB of the heap: a ClassBase is a {vptr, name}
 * pair with both words in the image and the name one of ours. */
static int resolve_classes(void)
{
    memset(g_cls, 0, sizeof g_cls);
    memset(g_res_beats, 0, sizeof g_res_beats);
    const size_t CH = 1 << 20;
    uint64_t *buf = malloc(CH + 16);
    if (!buf) return -1;
    uint64_t end = g_heap.lo + (64ull << 20);
    if (end > g_heap.hi) end = g_heap.hi;
    int found = 0;
    for (uint64_t s = g_heap.lo; s < end && found < C_COUNT; s += CH) {
        size_t n = (end - s < CH) ? (size_t)(end - s) : CH;
        if (RD(s, buf, n)) continue;
        for (size_t k = 0; k + 1 < n / 8; k++) {
            if (!in_img(buf[k], g_img.lo, g_img.hi) || !in_img(buf[k + 1], g_img.lo, g_img.hi)) continue;
            char nm[32];
            if (RD(buf[k + 1], nm, sizeof nm)) continue;
            nm[31] = 0;
            if (strcmp(nm, "StepEditorResolution") == 0) { resolve_enum_at(s + k * 8); continue; }
            if (memcmp(nm, "live.", 5)) continue;
            for (int c = 0; c < C_COUNT; c++) {
                if (!g_cls[c] && strcmp(nm, CLASS_NAMES[c]) == 0) {
                    g_cls[c] = s + k * 8;
                    found++;
                }
            }
        }
        sched_yield();
    }
    free(buf);
    if (found != C_COUNT) {
        for (int c = 0; c < C_COUNT; c++)
            if (!g_cls[c]) unified_log("move_model", LOG_LEVEL_WARN, "class %s not found", CLASS_NAMES[c]);
        return -1;
    }
    for (int o = 0; o < O_COUNT; o++) {
        if (find_member(g_cls[g_off[o].cls], g_off[o].member, &g_off[o].off, 0)) {
            unified_log("move_model", LOG_LEVEL_WARN, "member %s.%s not resolved",
                        CLASS_NAMES[g_off[o].cls], g_off[o].member);
            return -1;
        }
    }
    return 0;
}

/* ---- RTTI: a class's primary vptr from its mangled name --------------- */

static uint64_t find_word_in_image(uint64_t val, uint64_t from)
{
    const size_t CH = 1 << 20;
    uint64_t *buf = malloc(CH);
    uint64_t hit = 0;
    if (!buf) return 0;
    for (int g = 0; g < g_nseg && !hit; g++) {
        uint64_t lo = g_seg[g].lo > (from & ~7ull) ? g_seg[g].lo : (from & ~7ull);
        for (uint64_t s = lo; s < g_seg[g].hi && !hit; s += CH) {
            size_t n = (g_seg[g].hi - s < CH) ? (size_t)(g_seg[g].hi - s) : CH;
            if (RD(s, buf, n)) continue;
            for (size_t k = 0; k < n / 8; k++) if (buf[k] == val) { hit = s + k * 8; break; }
        }
    }
    free(buf);
    return hit;
}

static uint64_t find_string_in_image(const char *str)
{
    size_t L = strlen(str) + 1;
    const size_t CH = 1 << 20;
    char *buf = malloc(CH + L);
    uint64_t hit = 0;
    if (!buf) return 0;
    for (int g = 0; g < g_nseg && !hit; g++) {
        for (uint64_t s = g_seg[g].lo; s < g_seg[g].hi && !hit; s += CH) {
            size_t n = (g_seg[g].hi - s < CH + L) ? (size_t)(g_seg[g].hi - s) : CH + L;
            if (RD(s, buf, n)) continue;
            for (char *p = memmem(buf, n, str, L); p && !hit;
                 p = memmem(p + 1, n - (size_t)(p + 1 - buf), str, L))
                if (p == buf || p[-1] == 0) hit = s + (uint64_t)(p - buf);
        }
    }
    free(buf);
    return hit;
}

/* Every vtable of the class whose offset-to-top is 0 (primary, plus any
 * construction vtables): up to `max`, written to out. Returns the count. */
static int rtti_vptrs(const char *mangled, uint64_t *out, int max)
{
    int n = 0;
    uint64_t name = find_string_in_image(mangled);
    if (!name) return 0;
    /* typeinfo = {vptr, name, ...}; possibly several references to the name */
    for (uint64_t from = g_img.lo; from < g_img.hi;) {
        uint64_t at = find_word_in_image(name, from);
        if (!at) break;
        uint64_t ti = at - 8;
        for (uint64_t vf = g_img.lo; vf < g_img.hi;) {
            uint64_t v = find_word_in_image(ti, vf);
            if (!v) break;
            if (rq(v - 8) == 0 && n < max) out[n++] = v + 8;   /* offset-to-top 0 */
            vf = v + 8;
        }
        from = at + 8;
    }
    return n;
}

#define MAXVP 4
typedef struct { uint64_t v[MAXVP]; int n; } vpset_t;
static int vp_is(const vpset_t *s, uint64_t vp)
{
    for (int k = 0; k < s->n; k++) if (s->v[k] == vp) return 1;
    return 0;
}
static vpset_t g_vp_song, g_vp_clips, g_vp_midicontent, g_vp_sessionclip, g_vp_mixparams, g_vp_midistate;
static vpset_t g_vp_hist, g_vp_hstore, g_vp_tx;
static uint64_t g_hist;
static uint64_t g_song;
static int g_clock_pinned;

static int song_ok(uint64_t s)
{
    if (!s || !vp_is(&g_vp_song, rq(s))) return 0;
    double tempo;
    uint64_t tp = s + g_off[O_SONG_TRANSPORT].off + g_off[O_TR_TEMPO].off + g_off[O_PARAM_VALUE].off;
    if (RD(tp + V_WORD, &tempo, 8)) return 0;
    return tempo >= 20.0 && tempo <= 999.0;
}

static uint64_t find_song(void)
{
    const size_t CH = 1 << 20;
    uint64_t *buf = malloc(CH), hit = 0;
    if (!buf) return 0;
    for (uint64_t s = g_heap.lo; s < g_heap.hi && !hit; s += CH) {
        size_t n = (g_heap.hi - s < CH) ? (size_t)(g_heap.hi - s) : CH;
        if (RD(s, buf, n)) continue;
        for (size_t k = 0; k < n / 8; k++)
            if (vp_is(&g_vp_song, buf[k]) && song_ok(s + k * 8)) { hit = s + k * 8; break; }
        sched_yield();
    }
    free(buf);
    return hit;
}

/* MOVE'S UNDO STACK. Found once by its two vptrs (History, then its store
 * sixteen bytes in) and confirmed by a full read; every candidate is logged,
 * because a second flip document would carry a second History and which one
 * is the Song's is only knowable by watching it move. */
static mm_hist_vps_t hist_vps(void)
{
    mm_hist_vps_t v = { g_vp_hist.v, g_vp_hist.n, g_vp_hstore.v, g_vp_hstore.n, g_vp_tx.v, g_vp_tx.n };
    return v;
}
static void status(const char *fmt, ...);
static uint64_t find_history(void)
{
    if (!g_vp_hist.n || !g_vp_hstore.n || !g_vp_tx.n) return 0;
    const size_t CH = 1 << 20;
    uint64_t *buf = malloc(CH), hit = 0;
    if (!buf) return 0;
    mm_hist_vps_t v = hist_vps();
    int found = 0;
    for (uint64_t s = g_heap.lo; s < g_heap.hi; s += CH) {
        size_t n = (g_heap.hi - s < CH) ? (size_t)(g_heap.hi - s) : CH;
        if (RD(s, buf, n)) continue;
        for (size_t k = 0; k + 2 < n / 8; k++) {
            if (!vp_is(&g_vp_hist, buf[k]) || !vp_is(&g_vp_hstore, buf[k + 2])) continue;
            move_model_t t;
            const uint64_t at = s + k * 8;
            if (mm_history_read(self_read, NULL, at, &v, &t)) continue;
            status("history candidate %d at %llx size=%u", found, (unsigned long long)at, t.hist_size);
            if (!hit) hit = at;
            found++;
        }
        sched_yield();
    }
    free(buf);
    return hit;
}

#define STATUS_PATH "/data/UserData/schwung/move_model_status.txt"

/* Always written, one line per resolve attempt: the reader's state must never
 * be invisible (unified_log is best-effort and off unless armed). */
static void status(const char *fmt, ...)
{
    FILE *f = fopen(STATUS_PATH, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int resolve_all(void)
{
    g_pid = getpid();
    if (parse_maps()) return -1;
    char bid[64] = "";
    read_build_id(bid, sizeof bid);
    g_clock_pinned = (strcmp(bid, KNOWN_BUILD) == 0);
    if (resolve_classes()) { status("resolve: flip class registry incomplete (build %s)", bid); return -1; }
    g_vp_song.n        = rtti_vptrs("N7ableton10flip_model5FSongE", g_vp_song.v, MAXVP);
    g_vp_clips.n       = rtti_vptrs("N7ableton10flip_model6FClipsE", g_vp_clips.v, MAXVP);
    g_vp_sessionclip.n = rtti_vptrs("N7ableton10flip_model12FSessionClipE", g_vp_sessionclip.v, MAXVP);
    g_vp_midicontent.n = rtti_vptrs("N7ableton10flip_model16FMidiClipContentE", g_vp_midicontent.v, MAXVP);
    g_vp_mixparams.n   = rtti_vptrs("N7ableton10flip_model21FAudioMixerParametersE", g_vp_mixparams.v, MAXVP);
    /* Optional: a track's MIDI output (drum lanes needs its channel). */
    g_vp_midistate.n   = rtti_vptrs("N7ableton10flip_model15FMidiTrackStateE", g_vp_midistate.v, MAXVP);
    /* The MIXER is mandatory too: the model owning mute/solo while it can
     * read no mixer turns every fallback off and follows nothing. */
    if (!g_vp_song.n || !g_vp_clips.n || !g_vp_sessionclip.n || !g_vp_mixparams.n) {
        status("resolve: rtti unresolved song=%d clips=%d sessionclip=%d mixer=%d",
               g_vp_song.n, g_vp_clips.n, g_vp_sessionclip.n, g_vp_mixparams.n);
        return -1;
    }
    g_song = find_song();
    g_vp_hist.n   = rtti_vptrs("N4flip7HistoryINS_18HistoryStoreMemoryEEE", g_vp_hist.v, MAXVP);
    g_vp_hstore.n = rtti_vptrs("N4flip18HistoryStoreMemoryE", g_vp_hstore.v, MAXVP);
    g_vp_tx.n     = rtti_vptrs("N4flip11TransactionE", g_vp_tx.v, MAXVP);
    g_hist = find_history();
    status("history: vptrs hist=%d store=%d tx=%d obj=%llx", g_vp_hist.n, g_vp_hstore.n, g_vp_tx.n,
           (unsigned long long)g_hist);
    status("resolve: build=%s clock=%s song=%llx vptrs song=%d clips=%d sc=%d mc=%d", bid,
           g_clock_pinned ? "pinned" : "UNKNOWN-BUILD(no clock)", (unsigned long long)g_song,
           g_vp_song.n, g_vp_clips.n, g_vp_sessionclip.n, g_vp_midicontent.n);
    return g_song ? 0 : -1;
}

/* ====================================================================== */
/* Snapshot                                                               */
/* ====================================================================== */

#define OFF(o) (g_off[o].off)

/* THE READ PLAN. A full walk costs ~600 process_vm_readv calls on a busy set,
 * and doing it twice at 50 Hz measured 14% of a core on the device. But the
 * document's SHAPE -- which objects exist, where -- changes only on an edit.
 * So the full walk also RECORDS every leaf it read (address -> field of
 * move_model_t) and every structural word it relied on (container begin/size,
 * each element's object id, each vptr it tested) as a GUARD. A tick replays the
 * plan as ONE batched process_vm_readv; any guard that moved means the shape
 * changed and the next tick walks again. */
enum { PK_F64, PK_INT, PK_BOOL, PK_U64, PK_GUARD };
typedef struct { uint64_t addr; uint64_t expect; uint32_t dst; uint8_t kind, len; } plan_t;
#define PLAN_MAX 2048
static plan_t g_plan[PLAN_MAX];
static int g_nplan, g_plan_overflow;
static move_model_t *g_rec;         /* non-NULL while a full walk is recording */

static void rec(uint64_t addr, int kind, int len, void *dst, uint64_t expect)
{
    if (!g_rec) return;
    if (g_nplan >= PLAN_MAX) { g_plan_overflow = 1; return; }
    plan_t *p = &g_plan[g_nplan++];
    p->addr = addr; p->kind = (uint8_t)kind; p->len = (uint8_t)len; p->expect = expect;
    p->dst = dst ? (uint32_t)((uint8_t *)dst - (uint8_t *)g_rec) : 0;
}
static int f_f64(uint64_t a, double *dst)
{
    if (RD(a + V_WORD, dst, 8)) return -1;
    rec(a + V_WORD, PK_F64, 8, dst, 0);
    return 0;
}
static int f_int(uint64_t a, int *dst)
{
    int64_t v;
    if (RD(a + V_WORD, &v, 8)) return -1;
    *dst = (int)v;
    rec(a + V_WORD, PK_INT, 8, dst, 0);
    return 0;
}
static int f_bool(uint64_t a, uint8_t *dst)
{
    if (RD(a + V_BOOL, dst, 1)) return -1;
    rec(a + V_BOOL, PK_BOOL, 1, dst, 0);
    return 0;
}
/* A structural word: read it, and require it to stay put. */
static uint64_t guard(uint64_t a)
{
    uint64_t v = 0;
    if (RD(a, &v, 8)) return 0;
    rec(a, PK_GUARD, 8, NULL, v);
    return v;
}
static int walk(uint64_t hdr, uint64_t *out, int max)
{
    int n = mm_tree_elems(self_read, NULL, hdr, g_img.lo, g_img.hi, out, max);
    if (n < 0) return -1;
    guard(hdr);            /* begin node */
    guard(hdr + 16);       /* size */
    for (int k = 0; k < n && k < max; k++) guard(out[k] + OBJ_ID);
    return n;
}

/* THE CLIP BEING EDITED, watched every tick. A paste onto an occupied step
 * replaces a note without resizing Move's notes vector, so no guard moves;
 * the content is re-hashed each tick instead (a few hundred bytes) and a
 * difference forces a walk at once. Envelopes are Move's OWN automation,
 * which Move's edits and Undo change along with the notes. */
#define MM_PROBE_ENVS 16
typedef struct {
    uint64_t notes_vec;                    /* address of the Blob's {begin, end} */
    int      nenv;
    uint64_t env_vec[MM_PROBE_ENVS];
    uint64_t env_param[MM_PROBE_ENVS];
} probe_t;
static probe_t g_probe_tab[MM_TRACKS][MM_SLOTS];
static probe_t g_probe;                    /* the selected track's current clip */
static uint32_t g_probe_expect;
static int g_probe_valid;

static int hash_vec(uint64_t vec, uint32_t *h)
{
    uint64_t be[2];
    if (RD(vec, be, sizeof be)) return -1;
    if (be[1] < be[0] || be[1] - be[0] > (1u << 20)) return -1;
    uint8_t buf[4096];
    for (uint64_t at = be[0]; at < be[1]; at += sizeof buf) {
        size_t n = (be[1] - at < sizeof buf) ? (size_t)(be[1] - at) : sizeof buf;
        if (RD(at, buf, n)) return -1;
        for (size_t i = 0; i < n; i++) { *h ^= buf[i]; *h *= 16777619u; }
    }
    return 0;
}
static int probe_hash(const probe_t *p, uint32_t *out)
{
    uint32_t h = 2166136261u;
    if (p->notes_vec && hash_vec(p->notes_vec, &h)) return -1;
    for (int k = 0; k < p->nenv; k++) {
        uint64_t id = p->env_param[k];
        for (int b = 0; b < 8; b++) { h ^= (uint8_t)(id >> (8 * b)); h *= 16777619u; }
        if (hash_vec(p->env_vec[k], &h)) return -1;
    }
    *out = h;
    return 0;
}

static int read_clip(uint64_t sc, mm_clip_t *c, int t, int s)
{
    probe_t *pr = &g_probe_tab[t][s];
    memset(pr, 0, sizeof *pr);
    uint64_t clip = sc + OFF(O_SC_CLIP);
    uint64_t rg = clip + OFF(O_CLIP_REGION), lp = rg + OFF(O_RG_LOOP);
    uint64_t ts = clip + OFF(O_CLIP_TIMESIG);
    c->exists = 1;
    c->clip_id = guard(sc + OBJ_ID);
    if (f_f64(rg + OFF(O_RG_START), &c->region_start) || f_f64(rg + OFF(O_RG_END), &c->region_end) ||
        f_f64(lp + OFF(O_LOOP_START), &c->loop_start) || f_f64(lp + OFF(O_LOOP_END), &c->loop_end) ||
        f_bool(lp + OFF(O_LOOP_ON), &c->loop_on) || f_int(ts + OFF(O_TS_UPPER), &c->ts_upper) ||
        f_int(ts + OFF(O_TS_LOWER), &c->ts_lower))
        return -1;
    c->scroll = -1.0;
    uint64_t content[4];
    int nc = walk(clip + OFF(O_CLIP_CONTENT) + V_WORD, content, 4);
    if (nc < 0) return -1;
    for (int k = 0; k < nc && k < 4; k++) {
        if (!vp_is(&g_vp_midicontent, guard(content[k]))) continue;
        if (f_f64(content[k] + OFF(O_MC_SCROLL), &c->scroll)) return -1;
        /* The notes, as a CONTENT fingerprint: the Blob's bytes hashed. Its
         * vector {begin, end} is guarded, so an edit that reallocates it
         * re-walks at once; an in-place edit is picked up by the periodic
         * full walk. Decoding the notes is not needed -- equality is. */
        uint64_t nb = content[k] + OFF(O_MC_NOTES) + V_WORD;
        uint64_t b = guard(nb), e = guard(nb + 8);
        if (e < b || e - b > (1u << 20)) return -1;
        c->notes_len = (uint32_t)(e - b);
        uint32_t h = 2166136261u;
        uint8_t buf[4096];
        for (uint64_t at = b; at < e; at += sizeof buf) {
            size_t n = (e - at < sizeof buf) ? (size_t)(e - at) : sizeof buf;
            if (RD(at, buf, n)) return -1;
            for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 16777619u; }
        }
        c->notes_hash = h;
        pr->notes_vec = nb;
    }
    /* Move's own automation for this clip: SessionClip.mClipEnvelopes. */
    uint64_t envs[MM_PROBE_ENVS];
    int ne = walk(sc + OFF(O_SC_ENVELOPES) + OFF(O_ENVLIST_ENVS) + V_WORD, envs, MM_PROBE_ENVS);
    if (ne < 0) return -1;
    for (int k = 0; k < ne && k < MM_PROBE_ENVS; k++) {
        uint64_t au = envs[k] + OFF(O_ENV_AUTOMATION);
        pr->env_vec[pr->nenv] = au + OFF(O_AUTO_BREAKPOINTS) + V_WORD;
        pr->env_param[pr->nenv] = rq(au + OFF(O_AUTO_PARAM) + V_REFOBJ);
        pr->nenv++;
    }
    if (probe_hash(pr, &c->content_hash)) return -1;
    c->n_envelopes = pr->nenv;
    return 0;
}

static void derive(move_model_t *m)
{
    for (int t = 0; t < MM_TRACKS; t++) {
        mm_track_t *T = &m->track[t];
        T->muted = T->mixer_valid && T->speaker_value < 0.5;   /* speakerOn: 1 = audible */
        T->soloed = T->mixer_valid && T->solo_value > 0.5;     /* solo-cue */
    }
    m->selected_track = -1;
    for (int t = 0; t < MM_TRACKS; t++)
        if (m->track[t].selected) { m->selected_track = t; break; }
    m->clock_valid = g_clock_pinned && (m->playing == 0 || m->playing == 1) &&
                     isfinite(m->song_beats) && m->song_beats >= 0 && m->song_beats < 1e7;
    if (!m->clock_valid) { m->playing = 0; m->song_beats = 0; }
    int r = m->step_resolution;
    m->step_beats = (r >= 0 && r < RES_MAX) ? g_res_beats[r] : 0.0;   /* 0 = unknown */
    m->step_triplet = (r >= 0 && r < RES_MAX) ? g_res_triplet[r] : 0;
}

/* The full walk. Records the plan when g_rec == m. */
static int snapshot(move_model_t *m)
{
    memset(m, 0, sizeof *m);
    uint64_t S = g_song;
    if (!song_ok(S)) return -2;                              /* re-find */
    guard(S);                                                /* its vptr */
    uint64_t tr = S + OFF(O_SONG_TRANSPORT);
    if (f_f64(tr + OFF(O_TR_TEMPO) + OFF(O_PARAM_VALUE), &m->tempo) ||
        f_int(tr + OFF(O_TR_TIMESIG) + OFF(O_TS_UPPER), &m->ts_upper) ||
        f_int(tr + OFF(O_TR_TIMESIG) + OFF(O_TS_LOWER), &m->ts_lower) ||
        f_int(S + OFF(O_SONG_STEPRES), &m->step_resolution))
        return -1;
    if (g_clock_pinned) {
        uint64_t cm = tr + OFF(O_TR_CTRLMSG);
        int64_t pl = 0;
        if (RD(cm + CTRL_PLAYING, &pl, 8) || RD(cm + CTRL_BEATS, &m->song_beats, 8)) return -1;
        m->playing = (int)pl;
        rec(cm + CTRL_PLAYING, PK_INT, 8, &m->playing, 0);
        rec(cm + CTRL_BEATS, PK_F64, 8, &m->song_beats, 0);
    }
    uint64_t tracks[8];
    int nt = walk(S + OFF(O_SONG_TRACKS) + OFF(O_TL_TRACKS) + V_WORD, tracks, 8);
    /* EXACTLY four. A set load swaps the document over ~180 ms by inserting
     * the new tracks before removing the old -- measured 8 and then 12
     * elements mid-swap -- and a walk that took the first four of those would
     * publish a hybrid of two sets. */
    if (nt != MM_TRACKS) return -1;
    m->doc_id = 1469598103934665603ull;                      /* FNV-1a over the track ids */
    for (int t = 0; t < nt; t++) {
        uint64_t id = rq(tracks[t] + OBJ_ID);
        for (int b = 0; b < 8; b++) { m->doc_id ^= (id >> (8 * b)) & 0xff; m->doc_id *= 1099511628211ull; }
    }
    for (int t = 0; t < nt && t < MM_TRACKS; t++) {
        mm_track_t *T = &m->track[t];
        T->playing_slot = -1;
        if (f_bool(tracks[t] + OFF(O_TRACK_SELECTED), &T->selected)) return -1;
        {   /* the mixer: Track.mTrackMixerDevice -> its AudioMixerParameters component */
            uint64_t dcomps[8];
            int nd = walk(tracks[t] + OFF(O_TRACK_MIXER) + OFF(O_DEV_COMPONENTS) + V_WORD, dcomps, 8);
            if (nd < 0) return -1;
            for (int k = 0; k < nd && k < 8; k++) {
                if (!vp_is(&g_vp_mixparams, guard(dcomps[k]))) continue;
                uint64_t mp = dcomps[k];
                if (f_f64(mp + OFF(O_MIX_VOLUME) + OFF(O_PARAM_VALUE), &T->volume) ||
                    f_f64(mp + OFF(O_MIX_PAN) + OFF(O_PARAM_VALUE), &T->pan) ||
                    f_f64(mp + OFF(O_MIX_SOLO) + OFF(O_PARAM_VALUE), &T->solo_value) ||
                    f_f64(mp + OFF(O_MIX_SPEAKER) + OFF(O_PARAM_VALUE), &T->speaker_value))
                    return -1;
                T->mixer_valid = 1;
            }
        }
        uint64_t comps[8];
        int nc = walk(tracks[t] + OFF(O_TRACK_COMPONENTS) + V_WORD, comps, 8);
        if (nc < 0) return -1;
        uint64_t clips = 0;
        T->midi_out_ep = -1;
        for (int k = 0; k < nc && k < 8; k++) {
            const uint64_t vp = guard(comps[k]);
            if (vp_is(&g_vp_clips, vp)) clips = comps[k];
            /* Move's MIDI output for this track: an endpoint, channel = ep + 1,
             * none = -1 (the UI's "MIDI output channel" writes exactly this). */
            if (g_vp_midistate.n && vp_is(&g_vp_midistate, vp) &&
                f_int(comps[k] + OFF(O_MTS_OUT_EP), &T->midi_out_ep))
                return -1;
        }
        if (!clips) continue;                                 /* an audio-only shape, say */
        uint64_t ps = clips + OFF(O_CLIPS_PLAYSTATE);
        if (f_int(ps + OFF(O_PS_MODE), &T->mode) || f_f64(ps + OFF(O_PS_START), &T->start_beats))
            return -1;
        uint64_t ref = guard(ps + OFF(O_PS_SLOT) + V_REFOBJ);  /* a launch re-walks: rare */
        uint64_t slots[MM_SLOTS + 8];
        int ns = walk(clips + OFF(O_CLIPS_SLOTS) + V_WORD, slots, MM_SLOTS + 8);
        if (ns < 0) return -1;
        for (int s = 0; s < ns && s < MM_SLOTS; s++) {
            if (ref && rq(slots[s] + OBJ_ID) == ref) T->playing_slot = s;
            uint64_t sc[2];
            int n1 = walk(slots[s] + OFF(O_SLOT_CLIP) + V_WORD, sc, 2);
            if (n1 < 0) return -1;
            if (n1 >= 1 && vp_is(&g_vp_sessionclip, guard(sc[0])))
                if (read_clip(sc[0], &T->slot[s], t, s)) return -1;
        }
    }
    derive(m);
    m->valid = 1;
    return 0;
}

/* ---- the edited clip's notes, decoded (model thread only) -------------- */

#define MM_NOTES_MAX 1024
#define MM_NOTES_RAW_MAX (256 * 1024)   /* bytes: records are variable-length */
#define MM_PRESS_MAX 8192
static mm_note_t g_notes[2][MM_NOTES_MAX];   /* [0] now, [1] the previous state */
static mm_expr_point_t g_press[2][MM_PRESS_MAX];
static int g_nnotes[2];
static mm_clip_ref_t g_notes_ref[2];

static int decode_notes(uint64_t vec, mm_note_t *out, int max, mm_expr_point_t *pool, int pool_max)
{
    uint64_t be[2];
    if (!vec || RD(vec, be, sizeof be) || be[1] < be[0] || be[1] - be[0] > MM_NOTES_RAW_MAX) return -1;
    static uint8_t raw[MM_NOTES_RAW_MAX];
    size_t len = (size_t)(be[1] - be[0]);
    if (len && RD(be[0], raw, len)) return -1;
    return mm_decode_notes_buf(raw, len, out, max, pool, pool_max);
}

/* After a published walk: point the probe at the selected track's current
 * clip, and when that clip's content moved, decode it (previous kept). */
static void edited_clip_update(const move_model_t *m)
{
    g_probe_valid = 0;
    if (m->selected_track < 0) return;
    const mm_track_t *T = &m->track[m->selected_track];
    const int cs = (T->mode == 1) ? T->playing_slot : -1;
    if (cs < 0 || cs >= MM_SLOTS || !T->slot[cs].exists) return;
    const mm_clip_t *c = &T->slot[cs];
    g_probe = g_probe_tab[m->selected_track][cs];
    g_probe_expect = c->content_hash;
    g_probe_valid = 1;
    if (g_notes_ref[0].clip_id == c->clip_id && g_notes_ref[0].content_hash == c->content_hash) return;
    memcpy(g_notes[1], g_notes[0], sizeof(mm_note_t) * (size_t)g_nnotes[0]);
    memcpy(g_press[1], g_press[0], sizeof g_press[0]);
    g_nnotes[1] = g_nnotes[0];
    g_notes_ref[1] = g_notes_ref[0];
    int n = decode_notes(g_probe.notes_vec, g_notes[0], MM_NOTES_MAX, g_press[0], MM_PRESS_MAX);
    g_nnotes[0] = n < 0 ? 0 : n;
    g_notes_ref[0].track = m->selected_track;
    g_notes_ref[0].slot = cs;
    g_notes_ref[0].clip_id = c->clip_id;
    g_notes_ref[0].content_hash = c->content_hash;
    g_notes_ref[0].valid = n >= 0;
}

int move_model_edited_pressure(int previous, const mm_expr_point_t **pts)
{
    if (pts) *pts = g_press[previous ? 1 : 0];
    return MM_PRESS_MAX;
}

int move_model_edited_notes(int previous, const mm_note_t **notes, mm_clip_ref_t *ref)
{
    const int k = previous ? 1 : 0;
    if (notes) *notes = g_notes[k];
    if (ref) *ref = g_notes_ref[k];
    return g_notes_ref[k].valid ? g_nnotes[k] : -1;
}

/* Replay the plan into a copy of `skel` (the walk that recorded it).
 * 0 = read, 1 = a guard moved (re-walk), -1 = the read failed (re-walk).
 *
 * Fields are COALESCED into spans before reading: process_vm_readv's cost is
 * per iovec, not per byte, and a clip's fields sit a few hundred bytes apart.
 * 450 single-field iovecs measured 3.5% of a core at 50 Hz. */
#define SPAN_GAP 512
#define SPAN_MAX 4096
static struct iovec g_riov[PLAN_MAX];
static int g_nspan;
static uint32_t g_rawoff[PLAN_MAX];         /* each plan entry's offset into g_raw */
static uint8_t *g_raw;
static size_t g_rawcap;
static int g_order[PLAN_MAX];

static int cmp_plan_addr(const void *x, const void *y)
{
    uint64_t a = g_plan[*(const int *)x].addr, b = g_plan[*(const int *)y].addr;
    return a < b ? -1 : a > b;
}

/* After a walk: sort by address, merge neighbours into spans. */
static int plan_compile(void)
{
    for (int k = 0; k < g_nplan; k++) g_order[k] = k;
    qsort(g_order, (size_t)g_nplan, sizeof g_order[0], cmp_plan_addr);
    size_t total = 0;
    g_nspan = 0;
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < g_nplan; i++) {
        const plan_t *e = &g_plan[g_order[i]];
        if (g_nspan && e->addr >= lo && e->addr - hi <= SPAN_GAP && e->addr + e->len - lo <= SPAN_MAX) {
            if (e->addr + e->len > hi) hi = e->addr + e->len;
        } else {
            if (g_nspan) { g_riov[g_nspan - 1].iov_len = hi - lo; total += hi - lo; }
            lo = e->addr; hi = e->addr + e->len;
            g_riov[g_nspan].iov_base = (void *)(uintptr_t)lo;
            g_nspan++;
        }
        g_rawoff[g_order[i]] = (uint32_t)(total + (e->addr - lo));
    }
    if (g_nspan) { g_riov[g_nspan - 1].iov_len = hi - lo; total += hi - lo; }
    if (total > g_rawcap) {
        uint8_t *nb = realloc(g_raw, total);
        if (!nb) return -1;
        g_raw = nb; g_rawcap = total;
    }
    return 0;
}

static int plan_replay(const move_model_t *skel, move_model_t *m)
{
    size_t at = 0;
    for (int k = 0; k < g_nspan; k += 1024) {                /* IOV_MAX is 1024 */
        int n = g_nspan - k > 1024 ? 1024 : g_nspan - k;
        size_t want = 0;
        for (int j = 0; j < n; j++) want += g_riov[k + j].iov_len;
        struct iovec local = { g_raw + at, want };
        if (process_vm_readv(g_pid, &local, 1, &g_riov[k], (unsigned long)n, 0) != (ssize_t)want) return -1;
        at += want;
    }
    *m = *skel;
    uint8_t *base = (uint8_t *)m;
    for (int k = 0; k < g_nplan; k++) {
        const plan_t *e = &g_plan[k];
        uint64_t u = 0;
        memcpy(&u, g_raw + g_rawoff[k], e->len);
        switch (e->kind) {
        case PK_GUARD: if (u != e->expect) return 1; break;
        case PK_F64:   memcpy(base + e->dst, &u, 8); break;
        case PK_INT:   { int v = (int)(int64_t)u; memcpy(base + e->dst, &v, sizeof v); } break;
        case PK_BOOL:  base[e->dst] = (uint8_t)u; break;
        case PK_U64:   memcpy(base + e->dst, &u, 8); break;
        }
    }
    derive(m);
    return 0;
}

/* ====================================================================== */
/* Publication (seqlock) + diagnostics + thread                           */
/* ====================================================================== */

static move_model_t g_pub;
static move_model_listener_fn g_listener;
void move_model_set_listener(move_model_listener_fn fn) { g_listener = fn; }
static move_model_tick_fn g_tick_hook;
void move_model_set_tick_hook(move_model_tick_fn fn) { g_tick_hook = fn; }
static atomic_uint g_seq;          /* odd while writing */
static atomic_uint g_changes;

/* A torn read leaves `out` UNTOUCHED and returns 0: a caller keeping a static
 * copy then still holds the last good snapshot. Zeroing it made one preempted
 * publish read as "no clip, phase unknown" -- every lane released for a block
 * and a recording pass ended. */
int move_model_get(move_model_t *out)
{
    static __thread move_model_t tmp;
    for (int tries = 0; tries < 8; tries++) {
        unsigned a = atomic_load_explicit(&g_seq, memory_order_acquire);
        if (a & 1) continue;
        memcpy(&tmp, &g_pub, sizeof tmp);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_seq, memory_order_relaxed) == a) {
            memcpy(out, &tmp, sizeof *out);
            return out->valid;
        }
    }
    return 0;
}

static atomic_ullong g_last_publish_ms;
uint64_t move_model_last_publish_ms(void) { return atomic_load(&g_last_publish_ms); }

uint32_t move_model_seq(void) { return atomic_load(&g_changes); }

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- every track's PLAYING clip, for the audio thread (move_model.h) ---- */

static mm_play_clip_t g_play[MM_TRACKS][2];
static int g_play_cur[MM_TRACKS];            /* the index the RT side reads */
static uint64_t g_play_flip_ms[MM_TRACKS];
static const mm_play_clip_t g_play_none;

const mm_play_clip_t *move_model_playing_notes(int track)
{
    if (track < 0 || track >= MM_TRACKS) return &g_play_none;
    return &g_play[track][__atomic_load_n(&g_play_cur[track], __ATOMIC_ACQUIRE)];
}

static void play_flip(int t, uint64_t now)
{
    __atomic_store_n(&g_play_cur[t], 1 - g_play_cur[t], __ATOMIC_RELEASE);
    g_play_flip_ms[t] = now;
}

/* Decode each track's playing clip when {slot, clip_id, notes_hash} moves.
 * The bytes decoded are re-hashed and must equal the snapshot's notes_hash:
 * Move edits the blob on its own thread, and a buffer read mid-edit decodes
 * as a plausible wrong clip at least as often as it fails. A mismatch keeps
 * the published clip and retries next tick. */
static void playing_clips_update(const move_model_t *m)
{
    static uint8_t raw[MM_NOTES_RAW_MAX];
    static mm_note_t scratch[MM_NOTES_MAX];
    const uint64_t now = mono_ms();
    for (int t = 0; t < MM_TRACKS; t++) {
        const mm_track_t *T = &m->track[t];
        const int cs = (T->mode == 1) ? T->playing_slot : -1;
        const mm_clip_t *c = (cs >= 0 && cs < MM_SLOTS && T->slot[cs].exists) ? &T->slot[cs] : NULL;
        const mm_play_clip_t *cur = &g_play[t][g_play_cur[t]];
        if (!c || !c->notes_len) {
            if (cur->valid || cur->clip_id) {          /* nothing playing: publish "unknown" */
                if (now - g_play_flip_ms[t] < MM_PLAY_REUSE_MS) continue;
                memset(&g_play[t][1 - g_play_cur[t]], 0, sizeof(mm_play_clip_t));
                play_flip(t, now);
            }
            continue;
        }
        /* Same key = the same bytes (only hash-verified reads are published),
         * so the same answer -- including "unknown": no need to decode again. */
        if (cur->clip_id == c->clip_id && cur->slot == cs && cur->notes_hash == c->notes_hash)
            continue;
        if (now - g_play_flip_ms[t] < MM_PLAY_REUSE_MS) continue;   /* the RT side may still hold it */

        const uint64_t vec = g_probe_tab[t][cs].notes_vec;
        uint64_t be[2];
        if (!vec || RD(vec, be, sizeof be) || be[1] < be[0] || be[1] - be[0] > MM_NOTES_RAW_MAX) continue;
        const size_t len = (size_t)(be[1] - be[0]);
        if (len && RD(be[0], raw, len)) continue;
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < len; i++) { h ^= raw[i]; h *= 16777619u; }
        if (h != c->notes_hash) continue;                            /* torn, or the snapshot is behind */

        mm_play_clip_t *d = &g_play[t][1 - g_play_cur[t]];
        const int n = mm_decode_notes_buf(raw, len, scratch, MM_NOTES_MAX, NULL, 0);
        d->slot = cs;
        d->clip_id = c->clip_id;
        d->notes_hash = c->notes_hash;
        /* All or nothing: more notes than fit is "unknown", never a truncated clip. */
        d->valid = (n >= 0 && n <= MM_PLAY_NOTES_MAX);
        d->n = d->valid ? n : 0;
        for (int i = 0; i < d->n; i++)
            d->note[i] = (mm_play_note_t){ scratch[i].pitch, (float)scratch[i].pitch_offset,
                                           scratch[i].has_pitch, scratch[i].vel, scratch[i].start,
                                           scratch[i].dur };
        play_flip(t, now);
    }
}

/* ---- LIVE notes: Move's engine EventBuffers (move_model.h) ------------- */

/* The shared_ptr control block that owns each buffer -- its vtable is the
 * handle, resolved from this RTTI name once per process. Layout after the
 * vptr (measured, 2.1.0): +0x18 capacity (256), +0x20 count (per block),
 * +0x28 the record array. */
static const char *EB_CLASS =
    "NSt3__120__shared_ptr_emplaceIN7ableton6engine11EventBufferINS1_4midi21EndpointedMidiMessageENS1_"
    "8datatype8DistanceINS6_4UnitINS1_4time5units9FrameKindENS_5ratioILl1ELl1EEEEEdEENS2_19MidiOverflow"
    "HandlerIS5_SF_EEEENS_9allocatorISI_EEEE";
#define EB_MAX 32
#define EB_CAP 256
static vpset_t g_vp_eb;
static int g_eb_resolved;
static uint64_t g_eb_cb[EB_MAX];
static int g_neb;
static uint64_t g_live_data;                 /* published: the record array, 0 = none */

static int eb_valid(uint64_t cb, uint64_t *data)
{
    uint64_t w[6];
    if (RD(cb, w, sizeof w) || !vp_is(&g_vp_eb, w[0]) || w[3] != EB_CAP) return 0;
    if (w[5] < g_heap.lo || w[5] >= g_heap.hi) return 0;
    if (data) *data = w[5];
    return 1;
}

/* Every EventBuffer control block in the heap. Heavy (a pass over the heap,
 * reader thread only), so it runs at start and when the published buffer
 * dies -- a set load rebuilds Move's graph. */
static void eb_scan(void)
{
    g_neb = 0;
    if (!g_eb_resolved) {
        g_vp_eb.n = rtti_vptrs(EB_CLASS, g_vp_eb.v, MAXVP);
        g_eb_resolved = 1;
        status("live: EventBuffer vtables %d", g_vp_eb.n);
    }
    if (!g_vp_eb.n || !g_heap.lo) return;
    const size_t CH = 1 << 20;
    uint64_t *buf = malloc(CH);
    if (!buf) return;
    for (uint64_t s = g_heap.lo; s < g_heap.hi && g_neb < EB_MAX; s += CH) {
        size_t n = (g_heap.hi - s < CH) ? (size_t)(g_heap.hi - s) : CH;
        if (RD(s, buf, n)) continue;
        for (size_t k = 0; k < n / 8 && g_neb < EB_MAX; k++)
            if (vp_is(&g_vp_eb, buf[k]) && eb_valid(s + 8 * k, NULL)) g_eb_cb[g_neb++] = s + 8 * k;
    }
    free(buf);
    status("live: %d EventBuffers", g_neb);
}

/* Every tick: keep the published buffer only while its owner is alive, and
 * publish the first buffer whose records carry the live-input endpoint.
 * Its records persist between blocks, so one press after a scan is enough
 * to classify it. */
static void live_update(unsigned tick)
{
    static unsigned next_scan;
    uint64_t data = 0;
    int have = 0;
    for (int i = 0; i < g_neb && !have; i++) {
        if (!eb_valid(g_eb_cb[i], &data)) continue;
        uint8_t raw[MM_LIVE_REC_BYTES * 4];
        if (RD(data, raw, sizeof raw)) continue;
        mm_live_rec_t r[4];
        mm_decode_live_recs(raw, 4, r);
        for (int k = 0; k < 4; k++)
            if (r[k].ep == MM_LIVE_EP_INPUT && r[k].kind <= MM_LIVE_KIND_PNCC && r[k].id > 0) { have = 1; break; }
    }
    __atomic_store_n(&g_live_data, have ? data : 0, __ATOMIC_RELEASE);
    /* Nothing live: rescan, backing off -- a fresh graph, or no press yet. */
    if (!have && tick >= next_scan) {
        if (parse_maps() == 0) eb_scan();
        next_scan = tick + (g_neb ? 500u : 3000u);       /* ~10 s, or ~60 s */
    }
}

int move_model_live_read(mm_live_rec_t *out, int max)
{
    const uint64_t data = __atomic_load_n(&g_live_data, __ATOMIC_ACQUIRE);
    if (!data || max <= 0) return -1;
    if (max > 32) max = 32;
    uint8_t raw[MM_LIVE_REC_BYTES * 32];
    if (RD(data, raw, (size_t)max * MM_LIVE_REC_BYTES)) return -1;
    mm_decode_live_recs(raw, max, out);
    return max;
}

static void publish(const move_model_t *m)
{
    atomic_fetch_add_explicit(&g_seq, 1, memory_order_acq_rel);
    memcpy(&g_pub, m, sizeof g_pub);
    atomic_fetch_add_explicit(&g_seq, 1, memory_order_release);
    if (m->valid) atomic_store(&g_last_publish_ms, mono_ms());
}

/* Structure equality: everything but the clock, which moves every read. */
static int same_shape(const move_model_t *a, const move_model_t *b)
{
    move_model_t x = *a, y = *b;
    x.song_beats = y.song_beats = 0;
    x.playing = y.playing = 0;
    x.doc_gen = y.doc_gen = 0;
    return memcmp(&x, &y, sizeof x) == 0;
}

#define DIAG_FLAG "/data/UserData/schwung/move_model_on"
#define DIAG_JSON "/data/UserData/schwung/move_model.json"
#define DIAG_LOG  "/data/UserData/schwung/move_model.log"

static void fmt_line(const move_model_t *m, char *buf, size_t cap)
{
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "%s %7.3f %.1fbpm |", m->clock_valid ? (m->playing ? "PLAY" : "stop") : "?",
                          m->song_beats, m->tempo);
    for (int t = 0; t < MM_TRACKS && o < cap; t++) {
        const mm_track_t *T = &m->track[t];
        o += (size_t)snprintf(buf + o, cap - o, " T%d%s%s%s m%d", t + 1, T->selected ? "<" : "",
                              T->muted ? " MUTE" : "", T->soloed ? " SOLO" : "", T->mode);
        if (T->playing_slot >= 0) o += (size_t)snprintf(buf + o, cap - o, " s%d@%g", T->playing_slot + 1, T->start_beats);
        for (int s = 0; s < MM_SLOTS && o < cap; s++) {
            const mm_clip_t *c = &T->slot[s];
            if (!c->exists) continue;
            o += (size_t)snprintf(buf + o, cap - o, " [%d:%g-%g L%g-%g%s sc%g]", s + 1, c->region_start,
                                  c->region_end, c->loop_start, c->loop_end, c->loop_on ? "" : " off", c->scroll);
        }
        o += (size_t)snprintf(buf + o, cap - o, " |");
    }
}

static void write_json(const move_model_t *m)
{
    FILE *f = fopen(DIAG_JSON ".tmp", "w");
    if (!f) return;
    fprintf(f, "{\"valid\":%d,\"doc_gen\":%u,\"clock_valid\":%d,\"playing\":%d,\"song_beats\":%.4f,\"tempo\":%.3f,"
               "\"ts\":[%d,%d],\"step_resolution\":%d,\"step_beats\":%.5f,\"step_triplet\":%d,\"selected_track\":%d,\"tracks\":[",
            m->valid, m->doc_gen, m->clock_valid, m->playing, m->song_beats, m->tempo, m->ts_upper, m->ts_lower,
            m->step_resolution, m->step_beats, m->step_triplet, m->selected_track);
    for (int t = 0; t < MM_TRACKS; t++) {
        const mm_track_t *T = &m->track[t];
        double pos = (T->playing_slot >= 0 && m->clock_valid)
                         ? mm_clip_position(&T->slot[T->playing_slot], T->start_beats, m->song_beats) : -1;
        fprintf(f, "%s{\"selected\":%d,\"muted\":%d,\"soloed\":%d,\"volume\":%.4f,\"pan\":%.4f,\"mode\":%d,"
                   "\"playing_slot\":%d,\"start_beats\":%.4f,\"clip_pos\":%.4f,\"slots\":[",
                t ? "," : "", T->selected, T->muted, T->soloed, T->volume, T->pan, T->mode, T->playing_slot,
                T->start_beats, pos);
        int first = 1;
        for (int s = 0; s < MM_SLOTS; s++) {
            const mm_clip_t *c = &T->slot[s];
            if (!c->exists) continue;
            fprintf(f, "%s{\"slot\":%d,\"id\":%llu,\"region\":[%.4f,%.4f],\"loop\":[%.4f,%.4f],\"loop_on\":%d,"
                       "\"scroll\":%.4f,\"ts\":[%d,%d]}",
                    first ? "" : ",", s, (unsigned long long)c->clip_id, c->region_start, c->region_end,
                    c->loop_start, c->loop_end, c->loop_on, c->scroll, c->ts_upper, c->ts_lower);
            first = 0;
        }
        fprintf(f, "]}");
    }
    fprintf(f, "],\"history\":{\"valid\":%d,\"size\":%u,\"undo\":[%llu,%llu],\"redo\":[%llu,%llu]}}\n",
            m->hist_valid, m->hist_size, (unsigned long long)m->hist_undo_node, (unsigned long long)m->hist_undo_nbr,
            (unsigned long long)m->hist_redo_node, (unsigned long long)m->hist_redo_nbr);
    fclose(f);
    rename(DIAG_JSON ".tmp", DIAG_JSON);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double thread_cpu_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void *reader_main(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 0 };
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask); CPU_SET(1, &mask); CPU_SET(2, &mask);
    pthread_setaffinity_np(pthread_self(), sizeof mask, &mask);

    sleep(8);                        /* let Move build its document first */
    /* BOUNDED. Each attempt scans the heap and the image; an unrecognised
     * firmware would otherwise pay that every 32 s for the life of the
     * process and grow the status file without end. The fallbacks (D-Bus
     * mute follow, Song.abl, the file poll) are in charge meanwhile. */
    int backoff = 1, attempts = 0;
    while (resolve_all() != 0) {
        if (++attempts >= 8) {
            status("resolve: giving up after %d attempts; the model stays off this boot", attempts);
            return NULL;
        }
        sleep(backoff);
        if (backoff < 30) backoff *= 2;
    }

    static move_model_t prev, a, b, skel;
    memset(&prev, 0, sizeof prev);
    int diag = 0, torn = 0, refinds = 0, walks = 0, have_plan = 0;
    uint32_t doc_gen = 0;
    unsigned tick = 0;
    double t0 = now_s(), last_json = 0, cpu_t = now_s(), cpu_c = thread_cpu_s(), cpu_pct = 0;
    for (;; tick++) {
        usleep(20 * 1000);
        if (tick % 50 == 0) diag = (access(DIAG_FLAG, F_OK) == 0);
        if (tick % 250 == 0 && tick) {            /* the reader's own cost, every ~5 s */
            double tn = now_s(), cn = thread_cpu_s();
            cpu_pct = 100.0 * (cn - cpu_c) / (tn - cpu_t);
            cpu_t = tn; cpu_c = cn;
            if (diag) status("cpu %.2f%% of a core, walks=%d torn=%d plan=%d spans=%d", cpu_pct, walks, torn, g_nplan, g_nspan);
        }
        int ok = 0;
        if (have_plan && tick % 500 != 0) {       /* a full walk every ~10 s regardless */
            int r1 = plan_replay(&skel, &a);
            int r2 = r1 ? r1 : plan_replay(&skel, &b);
            if (r1 == 0 && r2 == 0 && same_shape(&a, &b)) ok = 1;
            else if (r1 == 0 && r2 == 0) { torn++; continue; }
            else have_plan = 0;                    /* the shape moved: walk now */
        }
        if (ok && g_probe_valid) {                 /* the edited clip, in place */
            uint32_t h = 0;
            if (probe_hash(&g_probe, &h) || h != g_probe_expect) ok = 0;
        }
        if (!ok) {
            g_rec = &a; g_nplan = 0; g_plan_overflow = 0;
            int ra = snapshot(&a);
            g_rec = NULL;
            if (ra == -2) {                                  /* the Song moved: find it again */
                refinds++;
                if (parse_maps() == 0) g_song = find_song();
                status("song object invalid; re-found at %llx", (unsigned long long)g_song);
                /* A heap scan is not a 50 Hz thing, nor a 2 s one forever:
                 * back off to a minute. The model is reported stale
                 * meanwhile (move_model_last_publish_ms), so the fallbacks
                 * take over rather than following a frozen document. */
                static unsigned refind_wait = 2;
                if (!g_song) { sleep(refind_wait); if (refind_wait < 60) refind_wait *= 2; }
                else refind_wait = 2;
                continue;
            }
            if (ra || snapshot(&b) || !same_shape(&a, &b)) { torn++; have_plan = 0; continue; }
            walks++;
            skel = a;
            have_plan = !g_plan_overflow && plan_compile() == 0;
        }
        {   /* Move's undo stack: three small reads, every tick. A failed
             * read re-finds the object, at most every ~5 s. */
            static unsigned hist_retry;
            mm_hist_vps_t hv = hist_vps();
            if (mm_history_read(self_read, NULL, g_hist, &hv, &b) != 0 && tick - hist_retry > (g_hist ? 250u : 3000u)) {
                hist_retry = tick;
                if (parse_maps() == 0) g_hist = find_history();
                mm_history_read(self_read, NULL, g_hist, &hv, &b);
            }
        }
        if (b.doc_id != prev.doc_id) doc_gen++;         /* a different document: a set load */
        b.doc_gen = doc_gen;
        edited_clip_update(&b);
        playing_clips_update(&b);
        live_update(tick);
        int changed = !same_shape(&b, &prev) || prev.valid != b.valid || prev.playing != b.playing;
        publish(&b);
        if (changed && g_listener) g_listener(&b, &prev);
        if (g_tick_hook) g_tick_hook(&b);
        if (changed) atomic_fetch_add(&g_changes, 1);
        if (diag) {
            double t = now_s();
            if (changed) {
                char line[2048];
                fmt_line(&b, line, sizeof line);
                FILE *f = fopen(DIAG_LOG, "a");
                if (f) {
                    fprintf(f, "%9.3f %s torn=%d refind=%d walks=%d plan=%d\n", t - t0, line, torn, refinds,
                            walks, g_nplan);
                    fclose(f);
                }
            }
            if (changed || t - last_json > 0.5) { write_json(&b); last_json = t; }
        }
        prev = b;
    }
    return NULL;
}

void move_model_start(void)
{
    static atomic_int started;
    if (atomic_exchange(&started, 1)) return;
    pthread_t tid;
    if (pthread_create(&tid, NULL, reader_main, NULL) != 0) {
        atomic_store(&started, 0);
        unified_log("move_model", LOG_LEVEL_ERROR, "pthread_create failed");
        return;
    }
    pthread_detach(tid);
}
#endif /* __linux__ */
