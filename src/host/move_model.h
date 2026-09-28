/* move_model.h -- read Move's LIVE song model out of its own memory.
 *
 * Move's set is a flip document (Ableton/Ohm Force's open-source data-model
 * framework, github.com/ohmtech/flip-public; the classes are `live.Song`,
 * `live.Track`, `live.ClipSlot`, `live.Clip`, `live.ClipRegion`, ...).
 * The shim is LD_PRELOADed into MoveOriginal, so the document is in OUR
 * address space. Everything the automation lanes spent 365 lines inferring
 * from LEDs, step-strip pixels and a Song.abl that is ~10 s stale is simply
 * there, current to the frame:
 *
 *   - which clip slots hold a clip, each clip's region and loop, in beats
 *   - which slot each track is PLAYING (or, stopped, has selected), and the
 *     transport beat it started on -- the quantised launch boundary, exact
 *   - the selected track, each clip's step-editor scroll position
 *   - tempo, time signature, and (firmware-pinned) run state + beat clock
 *
 * THREE RULES THAT ARE NOT OBVIOUS
 *
 * Offsets are resolved BY NAME, at runtime. flip keeps a registry of every
 * model class (`flip::ClassBase`) with its members' names and a tiny accessor
 * stub per member (`add x0, x0, #off; ret`). We find the registry by the class
 * NAME strings and decode the stubs, so a firmware that reorders fields still
 * resolves. Only two facts are pinned to a build: the offsets of the run flag
 * and the beat clock inside the transport's control-message slot, which is a
 * Move template, not a flip member. They are gated on MoveOriginal's GNU
 * build-id and report "unknown" on any other build.
 *
 * Every pointer hop is FAULT-SAFE. Move mutates the document on its own main
 * thread while we read it from ours. All reads go through process_vm_readv()
 * on our own pid, which returns EFAULT for an unmapped address instead of
 * raising SIGSEGV inside MoveOriginal. A walk that races an edit can therefore
 * read garbage but cannot crash; a snapshot is taken twice and published only
 * when both agree.
 *
 * The FSong object SURVIVES set loads (measured: same address, contents
 * replaced in place), so the expensive part -- a heap scan for it -- runs once
 * per process. It is re-validated every tick and re-found if it ever fails.
 *
 * Not RT: the reader is its own SCHED_OTHER thread on cores 0-2. The SPI
 * callback may call move_model_get(), which is a seqlock copy -- no locks, no
 * syscalls. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MM_TRACKS 4
#define MM_SLOTS  8

typedef struct {
    uint8_t  exists;
    uint64_t clip_id;        /* flip object id of the SessionClip: unique per clip
                              * for the life of the loaded set (a copy is a new id) */
    double   region_start, region_end;   /* beats, clip time */
    double   loop_start, loop_end;
    uint8_t  loop_on;
    double   scroll;         /* step editor scroll position (beats); -1 = n/a */
    uint32_t notes_len;      /* bytes of Move's notes blob (0 = no notes / not MIDI) */
    uint32_t notes_hash;     /* FNV-1a of those bytes: a CONTENT fingerprint */
    uint32_t content_hash;   /* notes AND Move's own automation envelopes: changes on
                              * any edit Move makes to the clip, and returns to an
                              * earlier value exactly when Move's Undo restores one */
    int      n_envelopes;    /* Move's own automation lanes on this clip */
    int      ts_upper, ts_lower;
} mm_clip_t;

/* live.PlayingState.Mode, as measured: 0 = never played, 1 = a clip slot is
 * current (playing when the transport runs; SELECTED when it is stopped),
 * 2 = the track was stopped (an empty slot was launched/selected). */
typedef struct {
    uint8_t   selected;
    uint8_t   mixer_valid;
    uint8_t   muted, soloed;  /* derived: speakerOn off / solo-cue on */
    double    volume, pan;    /* the mixer Parameters' manual values, as stored (volume 0..1) */
    double    speaker_value, solo_value;
    int       mode;
    int       playing_slot;  /* 0..7, or -1 */
    double    start_beats;   /* transport beat the current clip started on */
    mm_clip_t slot[MM_SLOTS];
} mm_track_t;

typedef struct {
    int      valid;          /* the document was resolved and read */
    uint64_t doc_id;         /* hash of the four track object ids: new on every set load */
    uint32_t doc_gen;        /* bumps when doc_id changes -- the set-load edge */
    int      clock_valid;    /* playing/song_beats are known (build-pinned) */
    int      playing;
    double   song_beats;     /* transport clock: beats since Play, 0 on every Play */
    double   tempo;
    int      ts_upper, ts_lower;
    int      step_resolution; /* live.Song.mStepEditorResolution, raw enum */
    double   step_beats;      /* one step button, in beats (1/16 = 0.25); 0 = unknown */
    uint8_t  step_triplet;    /* triplet grid: 12 steps per page, every 4th button dead */
    int      selected_track; /* 0..3, or -1 */
    mm_track_t track[MM_TRACKS];
} move_model_t;

/* ---- runtime ---------------------------------------------------------- */

void move_model_start(void);              /* spawns the reader thread, once */
int  move_model_get(move_model_t *out);   /* seqlock copy; returns out->valid */
uint32_t move_model_seq(void);            /* bumps on every published change */

/* Called on the READER thread (SCHED_OTHER, may do file I/O, must not block
 * for long) after every published change, with the new and previous snapshot.
 * One listener; set it before or after start. */
typedef void (*move_model_listener_fn)(const move_model_t *now, const move_model_t *prev);
void move_model_set_listener(move_model_listener_fn fn);
/* Also on the reader thread, after EVERY tick (changed or not). */
typedef void (*move_model_tick_fn)(const move_model_t *now);
void move_model_set_tick_hook(move_model_tick_fn fn);

/* Where the current clip of a track is, in CLIP time (beats), given the
 * transport clock. Plays region_start..loop_end once, then wraps inside the
 * loop. Returns -1 when it cannot say (no clip, degenerate loop). */
double mm_clip_position(const mm_clip_t *c, double start_beats, double song_beats);

/* A clip's whole editable STATE: its content (notes + Move's envelopes) and
 * its loop geometry. Move's Undo returns a clip to an earlier state exactly,
 * and a Double Loop on an empty clip changes only the geometry -- so this,
 * not content_hash alone, is what undo/redo are matched on. */
static inline uint32_t mm_clip_state_hash(const mm_clip_t *c)
{
    uint32_t h = c->content_hash ^ 0x9e3779b9u;
    const double g[5] = { c->region_start, c->region_end, c->loop_start, c->loop_end, (double)c->loop_on };
    const unsigned char *b = (const unsigned char *)g;
    for (unsigned i = 0; i < sizeof g; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ---- the clip being edited (model thread only: call from the listener) --- */

typedef struct { int pitch; double start, dur; float vel; int64_t id; } mm_note_t;
typedef struct { int valid, track, slot; uint64_t clip_id; uint32_t content_hash; } mm_clip_ref_t;

/* The selected track's current clip, decoded from Move's notes blob: its
 * notes NOW (previous = 0) or in its previous content state (1). Returns the
 * count, or -1 when unknown. The pointer stays valid until the next change. */
int move_model_edited_notes(int previous, const mm_note_t **notes, mm_clip_ref_t *ref);

/* ---- pure pieces, exported for tests/host ---------------------------- */

/* "1/16" -> 0.25 beats, "1/8t" -> 1/3 and *trip = 1. 0 on success. */
int mm_parse_resolution(const char *name, double *beats, uint8_t *trip);

typedef int (*mm_read_fn)(void *ctx, uint64_t addr, void *buf, size_t n); /* 0 = ok */

/* Decode a flip member-accessor stub. Two shapes are emitted:
 *   add x0, x0, #imm[, lsl #12] ; ret
 *   mov w8, #imm16              ; add x0, x0, x8 ; ret
 * (and a bare `ret` for offset 0). Returns 0 and writes *off, or -1. */
int mm_decode_member_stub(const uint32_t insn[4], uint32_t *off);

/* libc++ std::string (24 bytes, little-endian short/long layout). */
int mm_sso_string(mm_read_fn rd, void *ctx, const uint8_t raw[24], char *out, size_t cap);

/* In-order walk of a libc++ std::map whose header ({begin, root, size}) is at
 * `hdr`, returning each flip element pointer. A flip container node is
 * {left, right, parent, color, key..., ObjectWrapper{vptr, T*, ...}}; the key
 * differs by container (KeyFloat / KeyRandom), so the element is taken as the
 * word after the first IMAGE pointer following the key's own vptr -- the
 * wrapper's vtable. Returns the count, or -1 on a malformed tree. */
int mm_tree_elems(mm_read_fn rd, void *ctx, uint64_t hdr, uint64_t img_lo, uint64_t img_hi,
                  uint64_t *out, int max);
