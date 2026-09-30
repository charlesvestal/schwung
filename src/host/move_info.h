/*
 * move_info.h -- what Move's own set says, for modules. PUBLIC: copy this one
 * file into a module; it needs nothing else from Schwung.
 *
 * Schwung reads Move's song document live (docs/MOVE_MODEL.md) and publishes
 * the set-wide settings below. A module never does memory introspection of
 * its own -- it asks for a move_info_t:
 *
 *     move_info_t mi;
 *     if (move_info_read(&mi) && mi.valid) { ... mi.tempo, mi.root_note ... }
 *
 * WHERE IT COMES FROM. One source of truth, the shared-memory segment
 * /schwung-move-info, written by Schwung's model reader (not the audio
 * thread) whenever anything in it changes, at most every 20 ms. Two ways in,
 * and move_info_read() picks:
 *   - inside MoveOriginal (chain synths and FX, Master FX, overtake DSP) it
 *     calls the shim's exported schwung_move_info(), found once by dlsym and
 *     cached: a copy out of an already-mapped page, NO SYSCALLS, safe on the
 *     audio thread. Make the FIRST call in create_instance, not in
 *     render_block: the dlsym takes the loader's lock once;
 *   - anywhere else (a fork-parallel module's child, a tool) it maps the
 *     segment itself on first use. That first call opens a file, so make it
 *     outside the audio path.
 * JS modules use host_get_move_info() instead, which returns the same fields.
 *
 * move_info_read() uses dlsym and shm_open: compile with -D_GNU_SOURCE (or
 * define it before any #include), and link -ldl on older glibc. Define
 * MOVE_INFO_NO_READER to take the types and move_info_copy() only.
 *
 * COMPATIBILITY. The struct begins with its own size and version, and fields
 * are only ever APPENDED. A reader copies min(its size, the writer's size) and
 * zero-fills the rest, so an old module on a new Schwung and a new module on
 * an old Schwung both work: a field the host does not know reads as its
 * UNKNOWN value. Check `size` (or the field's unknown value) before trusting a
 * field added after MOVE_INFO_VERSION 1.
 *
 * UNKNOWN is always explicit: -1 for ints, 255 for flags, a negative float,
 * "" for text. `valid` 0 means Move's document is not being read at all right
 * now (an unrecognised firmware, or a set load in progress): treat every field
 * as unknown then. Values are Move's own -- no Schwung settings are mixed in.
 */
#ifndef MOVE_INFO_H
#define MOVE_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MOVE_INFO_VERSION   1
#define MOVE_INFO_SHM_NAME  "/schwung-move-info"
#define MOVE_INFO_MAGIC     "MOVEINF"
#define MOVE_INFO_TRACKS    4

typedef struct {
    char    name[32];       /* the track's name as typed on Move; "" = unnamed */
    int16_t color_id;       /* Move's palette index for the track colour; -1 unknown */
    int8_t  type;           /* 0 master, 1 player (the four tracks), 2 return; -1 unknown */
    uint8_t muted;          /* 0/1, 255 unknown */
    uint8_t soloed;         /* 0/1, 255 unknown */
    uint8_t selected;       /* 0/1, 255 unknown */
    uint8_t _pad[2];
    float   volume_db;      /* the track's volume fader, dB (0 = unity); <= -1000 unknown */
} move_info_track_t;

typedef struct {
    uint32_t size;          /* bytes the WRITER filled: sizeof(move_info_t) of the host */
    uint32_t version;       /* MOVE_INFO_VERSION of the host */
    uint32_t changes;       /* bumps whenever any field below changes */
    uint8_t  valid;         /* 1 = Move's document is being read live */
    uint8_t  playing;       /* transport running; 255 unknown */
    uint8_t  metronome_on;  /* 0/1, 255 unknown */
    uint8_t  midi_clock_sync; /* Move follows external MIDI clock: 0/1, 255 unknown */
    uint8_t  input_monitoring; /* audio input monitoring: 0/1, 255 unknown */
    int8_t   root_note;     /* 0 = C .. 11 = B; -1 unknown */
    int8_t   selected_track; /* 0..3; -1 unknown */
    int8_t   global_quant;  /* launch quantization, raw; -1 unknown (see global_quant_name) */
    uint8_t  ts_upper, ts_lower; /* time signature; 0 unknown */
    uint8_t  _pad[2];
    float    tempo;         /* BPM; <= 0 unknown */
    float    groove;        /* groove amount, Move's 0..1 value; < 0 unknown */
    float    master_db;     /* master volume knob, dB (-70 = the knob's bottom); <= -1000 unknown */
    char     scale[24];     /* Move's scale name, as Move spells it ("Major"); "" unknown */
    char     global_quant_name[24]; /* "none" "eightBars" .. "bar" .. "sixteenth" ..; "" unknown */
    double   song_beats;    /* transport position, beats since Play; < 0 unknown */
    move_info_track_t track[MOVE_INFO_TRACKS];
} move_info_t;

/* The segment: a seqlocked copy. `seq` is odd while the writer is inside. */
typedef struct {
    char        magic[8];
    uint32_t    seq;
    uint32_t    reserved;
    move_info_t info;
} move_info_shm_t;

/* Copy a consistent snapshot out of `shm` into `out` (out->size is ignored on
 * input; `cap` is the caller's sizeof). Returns 1 on success, 0 if no
 * consistent copy could be taken or the segment is not a move-info segment. */
static inline int move_info_copy(const volatile move_info_shm_t *shm, move_info_t *out, size_t cap)
{
    if (!shm || !out || cap < 8) return 0;
    if (memcmp((const void *)shm->magic, MOVE_INFO_MAGIC, sizeof MOVE_INFO_MAGIC) != 0) return 0;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t s0 = __atomic_load_n(&shm->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        size_t have = shm->info.size;
        if (have < 8 || have > 65536) return 0;
        size_t n = have < cap ? have : cap;
        memcpy(out, (const void *)&shm->info, n);
        if (n < cap) memset((char *)out + n, 0, cap - n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        /* out->size stays the WRITER's: fields past it are the host's
         * unknowns, which the caller tells from real values by that size. */
        if (__atomic_load_n(&shm->seq, __ATOMIC_RELAXED) == s0) return 1;
    }
    return 0;
}

#ifndef MOVE_INFO_NO_READER
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef RTLD_DEFAULT                 /* without _GNU_SOURCE; glibc's value */
#define RTLD_DEFAULT ((void *)0)
#endif

/* The shim's export: copies the current snapshot. Returns 1 on success. */
typedef int (*move_info_fn)(move_info_t *out, size_t cap);

/* Read the current snapshot. Returns 1 when `out` was filled (check
 * out->valid), 0 when this Schwung does not provide it. The first call may
 * open a file when not running inside MoveOriginal; see the header comment. */
static inline int move_info_read(move_info_t *out)
{
    static move_info_fn fn;
    static const volatile move_info_shm_t *shm;
    static int state;                 /* 0 untried, 1 export, 2 shm, 3 none */
    if (!out) return 0;
    if (state == 0) {
        fn = (move_info_fn)dlsym(RTLD_DEFAULT, "schwung_move_info");
        if (fn) state = 1;
        else {
            int fd = shm_open(MOVE_INFO_SHM_NAME, O_RDONLY, 0);
            if (fd >= 0) {
                void *p = mmap(NULL, sizeof(move_info_shm_t), PROT_READ, MAP_SHARED, fd, 0);
                close(fd);
                if (p != MAP_FAILED) { shm = (const volatile move_info_shm_t *)p; state = 2; }
            }
            if (state == 0) state = 3;
        }
    }
    if (state == 1) return fn(out, sizeof *out);
    if (state == 2) return move_info_copy(shm, out, sizeof *out);
    return 0;
}
#endif

#endif /* MOVE_INFO_H */
