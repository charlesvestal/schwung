/*
 * The shadow_ui -> MIDI_OUT carry: what could not be placed this frame.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * shadow_inject_ui_midi_out() used to snapshot the shadow_ui SHM buffer, reset
 * write_idx, MEMSET THE SOURCE, and only then start placing packets into the
 * 80-byte MIDI_OUT region — stopping at the first frame boundary it could not
 * fit. Everything past that point in the snapshot was gone: not delayed, not
 * retried, not counted, not logged. The comment on ext_midi_ring_drain named
 * it in passing ("The loss downstream is shadow_inject_ui_midi_out's, which
 * memsets its source before copying") and nothing acted on it.
 *
 * MIDI_OUT holds 20 packets, and the drain ran once per shadow_ui flush rather
 * than once per SPI frame, so the real ceiling was ~20 packets per 60 Hz tick
 * = 3600 SysEx data bytes/s. DIN MIDI is 3125 bytes/s. The whole outbound
 * budget sat 15% above the wire rate with no backpressure and no margin, which
 * is why short control messages "work more or less perfectly" while any bulk
 * dump loses whole packets from the middle — the same multiple-of-3 signature
 * as #358, from the opposite direction.
 *
 * A 158-byte SysEx message is 53 packets. It cannot fit in one frame and never
 * could; the bug was never the frame size, it was that the remainder had
 * nowhere to live.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DOES
 *
 * It gives the remainder somewhere to live. Packets that do not fit stay here,
 * in order, and go out on the next SPI frame — a DELAY instead of a loss, the
 * same contract ext_midi_ring_drain already honours for the DSP path (it does
 * not advance `tail` when the region fills). That alone lifts the ceiling from
 * ~20 packets per 60 Hz tick to ~20 per 344 Hz frame: 6x clear of DIN rate
 * instead of 15% above it.
 *
 * It is header-only and pure — no allocation, no I/O, no locks, no globals —
 * for the same reason as fx_midi_filter.h and ext_midi_ring.h: the only caller
 * is the SPI callback, which cannot be built on the dev machine, and filtering
 * like this is exactly what ends up shipped untested. tests/host compiles and
 * RUNS it natively.
 *
 * ---------------------------------------------------------------------------
 * ORDER IS THE WHOLE POINT
 *
 * A SysEx message is a run of packets that only means anything in sequence.
 * Appends go to the tail, drains take from the head, and a partial drain
 * shifts the remainder down rather than leaving a hole. Reordering a SysEx run
 * is worse than dropping it: the receiver assembles a well-framed message out
 * of shuffled data and has no way to know.
 */
#ifndef UI_MIDI_OUT_CARRY_H
#define UI_MIDI_OUT_CARRY_H

#include <stdint.h>
#include <string.h>

#include "shadow_constants.h"

/* 256 packets. Sized against the message that exposed the bug rather than
 * against the mailbox: a 158-byte SysEx is 53 packets, so this holds ~4.8 of
 * them back-to-back. Matching SHADOW_UI_MIDI_BYTES/4 on the inbound side is
 * deliberate — a tool that can be SENT a burst of that size can answer one. */
/* 1024 packets = 4096 bytes: room for TWO full-screen framebuffers.
 *
 * A framebuffer is 394 packets. At 512 the carry could hold one and a fragment
 * of the next, so a repaint queued while another was still draining -- raise
 * the Shift map, release it, and the parameter view is owed immediately -- had
 * its tail dropped, and the device drew a truncated screen. Observed on
 * hardware 2026-09-10 as "shows the slot list, then garbled".
 *
 * The pacing above makes a frame take ~130 SPI frames to drain, which widens
 * exactly the window in which a second repaint can be requested, so the depth
 * has to cover two. /dev/shm is tmpfs and allocates by page, so this is free;
 * SHADOW_MIDI_OUT_BUFFER_SIZE must move with it, and the assert below fails if
 * it does not. */
#define UI_MIDI_CARRY_PACKETS 1024
#define UI_MIDI_CARRY_BYTES   (UI_MIDI_CARRY_PACKETS * 4)

/* The carry must hold whatever one flush of the SHM buffer can deliver.
 *
 * Found on hardware: a 632-byte SysEx (212 packets) came back REFUSED because
 * the SHM buffer was 128 packets, so the two numbers were already disagreeing
 * about how big a single send may be. Enlarging only one of them moves the
 * failure rather than fixing it — make the SHM side bigger and the surplus is
 * DROPPED at the carry instead of REFUSED at the send, which converts a return
 * value the caller can act on into a silent loss, the exact thing this file
 * exists to remove. Keep them equal. */
_Static_assert(UI_MIDI_CARRY_BYTES == SHADOW_MIDI_OUT_BUFFER_SIZE,
               "carry must match SHADOW_MIDI_OUT_BUFFER_SIZE: a flush the SHM "
               "buffer accepts has to fit downstream, or a refusal becomes a drop");

/* Stop accepting new work from the SHM buffer while the carry is at least this
 * full. See ui_midi_carry_wants_more() for why this is backpressure and not
 * just a threshold. */
/*
 * Packets placed into MIDI_OUT per SPI frame.
 *
 * The mailbox holds 20 and the carry used to fill every free slot, so a long
 * SysEx went out at ~6900 packets/s. Measured on an OXI E16 over USB-A
 * 2026-09-10: a 394-packet framebuffer arrived with most of its middle
 * missing -- a full-width bar drawn at the bottom rendered as a fragment
 * partway up the screen, because every lost packet shifts what follows
 * earlier. Handing the same frame over at 6 packets per frame put the bar
 * exactly where it was drawn.
 *
 * The loss is RATE dependent, not size dependent, which is the same shape as
 * the inbound losses in #358: docs/SYSEX.md measures 31 packets sent alone
 * arriving byte-perfect and 34 amid other traffic losing 8. So the fix belongs
 * HERE, in the transport, not in any one producer -- every module sending a
 * bulk dump hits the same wall, and a rate chosen per-caller would be a rate
 * nobody else benefits from.
 *
 * 3 x 344 Hz is ~1000 packets/s, still a third above DIN rate, and it costs
 * nothing that is felt: a repaint follows a deliberate action, and the common
 * case -- a knob turn -- is a single 7-byte message that goes out in one frame
 * either way.
 */
#define UI_MIDI_CARRY_PACKETS_PER_FRAME 3

/*
 * THE PACE IS TUNABLE AT RUNTIME, because 3 was never measured -- it was the
 * first value that stopped the garbling after "everything at once" failed, and
 * a ceiling nobody searched for is a ceiling nobody knows.
 *
 * It decides the only number the user actually feels: a 394-packet framebuffer
 * takes ceil(394 / pace) SPI frames at 2.90 ms each, so 3 is 383 ms and 12
 * would be 96. Rebuilding to try a value costs a full cross-compile and a
 * device restart, which is why the search never happened; reading it from
 * /data/UserData/schwung/e16_pace turns the experiment into an echo and a knob
 * turn.
 *
 * Read on the CONTROL side and published here as a plain int -- never opened
 * from the drain, which runs on the SPI callback where file I/O is forbidden.
 * Clamped on the way in, so a typo in the file cannot stall the wire (0) or
 * restore the unpaced behaviour that lost packets in the first place.
 */
#define UI_MIDI_CARRY_PACE_MIN 1

/*
 * THE MAILBOX IS SHARED, SO WE MAY NOT TAKE ALL OF IT.
 *
 * MIDI_OUT is 20 slots per SPI frame and Move's own output and the LED flush
 * write into it too (see the drain's "free means all four bytes zero"). A pace
 * of 20 therefore fills every slot on every frame we have anything to send,
 * leaving Move's firmware nowhere to put its own MIDI -- measured on hardware
 * 2026-09-11 as "pace 20 has frozen the device", which needed a restart.
 *
 * 12 leaves 8 slots. It is not a tuning preference: it is the reserve that
 * keeps a SHARED region shared, and the ceiling was 64 only because the field
 * was sized before anyone asked what the region could actually give away.
 *
 * A framebuffer at 12 is 33 frames, 96 ms -- close enough to the 58 ms that
 * pace 20 would buy that the difference is not worth a device that stops
 * responding.
 */
#define UI_MIDI_CARRY_PACE_RESERVE 8
#define UI_MIDI_CARRY_PACE_MAX 12

static int ui_midi_carry_pace = UI_MIDI_CARRY_PACKETS_PER_FRAME;

static inline void ui_midi_carry_set_pace(int pace)
{
    if (pace < UI_MIDI_CARRY_PACE_MIN) pace = UI_MIDI_CARRY_PACE_MIN;
    if (pace > UI_MIDI_CARRY_PACE_MAX) pace = UI_MIDI_CARRY_PACE_MAX;
    ui_midi_carry_pace = pace;
}

static inline int ui_midi_carry_get_pace(void) { return ui_midi_carry_pace; }

#define UI_MIDI_CARRY_HIGH_WATER (UI_MIDI_CARRY_BYTES / 2)

/*
 * RETRY STATE -- see "A COLLIDED MESSAGE IS RE-QUEUED" below.
 *
 * Sized to hold the largest single message we ever send, which is a 394-packet
 * framebuffer (1576 bytes). A message longer than this cannot be retried; that
 * is recorded rather than silently skipped, because a retry that quietly does
 * not happen is indistinguishable from one that did and collided again.
 */
#define UI_MIDI_CARRY_MSG_BYTES   2048
/*
 * How many times one message may be re-queued.
 *
 * Not unbounded, and the reason is amplification: under continuous playing
 * every attempt can collide, and a message that re-queues itself forever turns
 * a display glitch into a wire full of retries that starves the next real
 * update. Two attempts take the ~15% single-attempt collision rate measured on
 * 2026-09-11 down to well under 1%, which is past the point where anyone sees
 * it.
 */
#define UI_MIDI_CARRY_MSG_RETRIES 2
/*
 * How many consecutive frames a message may be held back waiting for a mailbox
 * with no foreign traffic in it.
 *
 * The quiet-start below is an OPTIMISATION, never a precondition: notes are
 * sparse against 344 frames/sec so almost every frame is clear, but a dense
 * enough stream must not be able to stop the screen updating altogether. At
 * 2.90 ms a frame this is ~186 ms, after which we go regardless and let the
 * retry handle whatever happens.
 */
#define UI_MIDI_CARRY_START_DEFER_MAX 64

typedef struct {
    uint8_t buf[UI_MIDI_CARRY_BYTES];
    int len;        /* bytes currently held, always a multiple of 4 */
    int drops;      /* packets refused because the carry was full */

    /* The SysEx run currently being placed, kept so it can be re-queued. */
    uint8_t msg[UI_MIDI_CARRY_MSG_BYTES];
    int msg_len;        /* bytes of the open run placed so far; 0 = no run */
    int msg_collided;   /* foreign cable-2 traffic landed inside this run */
    int msg_retries;    /* re-queues already spent on this run */
    int msg_overflow;   /* run outgrew msg[] -- cannot be retried */
    int start_defers;   /* consecutive frames a new run has been held back */
} ui_midi_carry_t;

/* Counters, for the same reason every other failure here has one. */
static int ui_midi_carry_retries = 0;    /* messages re-queued after a collision */
static int ui_midi_carry_unretryable = 0;/* collided, but too long to re-queue */

static inline int ui_midi_carry_retry_count(void)      { return ui_midi_carry_retries; }
static inline int ui_midi_carry_unretryable_count(void){ return ui_midi_carry_unretryable; }

/* USB-MIDI CIN (low nibble of byte 0). 0x4 starts or continues a SysEx run;
 * 0x5/0x6/0x7 end one. 0x5 is ALSO a lone single-byte message, so it only
 * closes a run when one is actually open -- which is why every test below
 * asks about msg_len rather than about the CIN alone. */
#define UI_MIDI_CIN(pkt) ((pkt)[0] & 0x0F)
static inline int ui_midi_cin_opens_run(uint8_t cin)  { return cin == 0x04; }
static inline int ui_midi_cin_closes_run(uint8_t cin) { return cin >= 0x05 && cin <= 0x07; }

static inline void ui_midi_carry_reset(ui_midi_carry_t *c)
{
    if (!c) return;
    c->len = 0;
    c->drops = 0;
    c->msg_len = 0;
    c->msg_collided = 0;
    c->msg_retries = 0;
    c->msg_overflow = 0;
    c->start_defers = 0;
}

/*
 * Should the caller take another snapshot of the shadow_ui SHM buffer?
 *
 * THIS IS THE BACKPRESSURE, and it works by NOT reading. Leaving the SHM
 * buffer alone lets it fill, and a full SHM buffer is the one condition
 * js_shadow_midi_send() already reports to JS as a `false` return — a signal
 * that exists, is documented ("Report the failure so the caller can decline to
 * cache it"), and until now could only ever fire on a single 128-packet flush.
 * Refusing to drain upstream is what connects it to the real constraint
 * downstream, so a module that paces on the return value paces on the mailbox.
 *
 * Draining unconditionally and dropping at the far end would keep the same
 * silent-loss shape this file exists to remove, one buffer further along.
 */
static inline int ui_midi_carry_wants_more(const ui_midi_carry_t *c)
{
    return c && c->len < UI_MIDI_CARRY_HIGH_WATER;
}

/*
 * Append one 4-byte packet. Returns 1 when queued, 0 when the carry is full.
 *
 * Drop-oldest is NOT an option here: the head of the carry is the head of a
 * SysEx message that is already partly on the wire. Refusing the newest packet
 * truncates one message; dropping the oldest corrupts one that the receiver
 * has already begun to assemble.
 */
static inline int ui_midi_carry_push(ui_midi_carry_t *c, const uint8_t pkt[4])
{
    if (!c || !pkt) return 0;
    if (c->len + 4 > UI_MIDI_CARRY_BYTES) { c->drops++; return 0; }
    memcpy(&c->buf[c->len], pkt, 4);
    c->len += 4;
    return 1;
}

/*
 * Place as many leading packets as fit into free slots of a MIDI_OUT region,
 * then shift the remainder down. Returns packets placed.
 *
 * "Free" means all four bytes zero — the same test every other writer into
 * this region uses, because Move's own output and the LED flush share it.
 * Placement stops at the first packet that does not fit; it does not skip
 * ahead to find a smaller gap, because that would reorder the run.
 */
/*
 * INTERLEAVE COUNTER -- foreign cable-2 packets seen in the mailbox while a
 * message of ours is still going out.
 *
 * MIDI_OUT is a SHARED 20-slot region: Move's own external output and the LED
 * flush write into it too. A USB-MIDI SysEx is a RUN of CIN-0x04 packets, and
 * the receiving device assembles it as one message -- so anything Move
 * transmits on the same cable mid-run is spliced into the middle of our
 * framebuffer, and the E16 draws the result.
 *
 * That is rate-INDEPENDENT, which is what makes it worth a counter of its own.
 * It was mistaken for a rate limit for most of this feature's life; the
 * evidence against that came from the device -- "even at 1 i dont get stable
 * draws" -- where a framebuffer occupies the wire for 1.14 s and so offers the
 * LARGEST possible window for somebody else to write into it. Slowing down
 * makes this worse, not better, which is the exact opposite of the response a
 * rate problem wants.
 */
static int ui_midi_carry_foreign = 0;

/*
 * STRANDED PACKETS -- ones we placed in the mailbox that Move never took.
 *
 * This is the ONE hand-off on the outbound path that nothing has ever
 * measured. The SHM buffer and the carry both count their own drops and both
 * report zero under real load (measured twice on 2026-09-11, with knobs
 * turning and playback running, while the screen was visibly garbling). But
 * "placed in a free slot" is not "sent": we scan for four zero bytes, write
 * ours, and never look again.
 *
 * If Move overwrites a slot, or does not drain on the schedule we assume, a
 * packet dies inside our own system and every counter we have stays zero --
 * which is exactly what we are looking at. So: remember what we wrote and
 * where, and on the next frame check whether our bytes are still sitting
 * there. If they are, Move did not take them.
 *
 * A false positive is possible in principle -- an identical packet landing in
 * the same slot from somebody else -- and is rare enough that a non-zero
 * count is still the answer to the question being asked.
 */
#define UI_MIDI_CARRY_TRACK 24

/*
 * PACKETS PLACED -- the positive control, and the reason the others mean
 * anything.
 *
 * Every counter beside this one fires only on FAILURE, so a window of zeros
 * cannot tell "nothing went wrong" from "nothing happened". That ambiguity
 * wasted three measurements on 2026-09-11: a capture taken while nobody was
 * turning a knob reads exactly like a clean run, and was very nearly reported
 * as one.
 *
 * So count the successes too. A window with placements and no drops is
 * evidence; a window with neither is an empty capture and says nothing.
 */
static int ui_midi_carry_placed_total = 0;

static inline int ui_midi_carry_placed_count(void) { return ui_midi_carry_placed_total; }

static int ui_midi_carry_stranded = 0;
static int ui_midi_carry_last_n = 0;
static int ui_midi_carry_last_slot[UI_MIDI_CARRY_TRACK];
static uint8_t ui_midi_carry_last_pkt[UI_MIDI_CARRY_TRACK][4];

static inline int ui_midi_carry_stranded_count(void) { return ui_midi_carry_stranded; }

static inline int ui_midi_carry_foreign_count(void) { return ui_midi_carry_foreign; }

static inline int ui_midi_carry_drain(ui_midi_carry_t *c, uint8_t *midi_out,
                                      int region_bytes)
{
    if (!c || !midi_out || c->len <= 0 || region_bytes < 4) return 0;

    int placed = 0;
    int slot = 0;
    int read = 0;
    int run_closed = 0;

    /*
     * LAST FRAME'S PACKETS MUST NOT BE SENT TWICE.
     *
     * Nothing clears the shadow MIDI_OUT after a transfer -- worse, the shim's
     * post-transfer sync copies the output region back from hardware, so a
     * packet we placed is STILL THERE afterwards. It is normally overwritten
     * because Move rewrites the region each frame; when Move does not, our
     * packet is transmitted a SECOND time.
     *
     * For channel-voice traffic a duplicate is a stuck note at worst. For a
     * SysEx it is fatal in the same way a drop is: the receiver is assembling
     * one message, and a repeated packet corrupts it just as thoroughly as a
     * missing one. Measured at ~0.6/sec, which is ~3% of 34-packet LABELS
     * messages -- matching the observed rate of occasional garbling.
     *
     * This was originally counted as "stranded", i.e. read as evidence that
     * MOVE had failed to take the packet. That reading was wrong and the
     * counter could not tell the two apart: "still present" means "not
     * consumed" OR "consumed and copied back". Clearing it makes the question
     * moot -- the slot is free for this frame either way, and nothing of ours
     * can be sent twice.
     */
    for (int q = 0; q < ui_midi_carry_last_n; q++) {
        const int sl = ui_midi_carry_last_slot[q];
        if (sl + 4 > region_bytes) continue;
        if (midi_out[sl]     == ui_midi_carry_last_pkt[q][0] &&
            midi_out[sl + 1] == ui_midi_carry_last_pkt[q][1] &&
            midi_out[sl + 2] == ui_midi_carry_last_pkt[q][2] &&
            midi_out[sl + 3] == ui_midi_carry_last_pkt[q][3]) {
            ui_midi_carry_stranded++;          /* now: "would have repeated" */
            midi_out[sl] = 0; midi_out[sl + 1] = 0;
            midi_out[sl + 2] = 0; midi_out[sl + 3] = 0;
        }
    }
    ui_midi_carry_last_n = 0;

    /* Count foreign cable-2 traffic BEFORE placing anything, so we measure what
     * Move put there and never our own packets from this frame. Only while the
     * carry is non-empty: a packet from Move between two complete messages of
     * ours is ordinary MIDI, not interference. */
    int foreign_this_frame = 0;
    for (int q = 0; q + 4 <= region_bytes; q += 4) {
        if (!midi_out[q] && !midi_out[q + 1] && !midi_out[q + 2] && !midi_out[q + 3])
            continue;
        if (((midi_out[q] >> 4) & 0x0F) == 0x02) {
            ui_midi_carry_foreign++;
            foreign_this_frame++;
        }
    }

    /* Were we already mid-run when this frame began? A foreign packet now is
     * unambiguously INSIDE our message in that case, whatever slot it sits in. */
    const int was_mid_run = (c->msg_len > 0);

    /*
     * QUIET-START: do not OPEN a run into a mailbox that already has foreign
     * cable-2 traffic in it.
     *
     * Free, and it removes the easiest collisions: the note is already in the
     * region, so starting here splices it into the message we are about to
     * begin. Waiting one frame costs 2.90 ms and notes are sparse against 344
     * frames/sec, so almost every frame is clear.
     *
     * It is an optimisation and NEVER a precondition -- a dense enough stream
     * must not be able to stop the screen updating, so the defer is capped and
     * we go anyway after it, leaving the retry to handle the outcome.
     */
    if (!was_mid_run && foreign_this_frame > 0 &&
        c->start_defers < UI_MIDI_CARRY_START_DEFER_MAX) {
        c->start_defers++;
        return 0;
    }
    c->start_defers = 0;

    while (read < c->len) {
        while (slot + 4 <= region_bytes &&
               (midi_out[slot] || midi_out[slot + 1] ||
                midi_out[slot + 2] || midi_out[slot + 3])) {
            slot += 4;
        }
        if (slot + 4 > region_bytes) break;  /* full this frame — retry next */

        memcpy(&midi_out[slot], &c->buf[read], 4);
        ui_midi_carry_placed_total++;

        /*
         * Keep a copy of the run being placed, so it can be re-sent whole.
         *
         * Framing comes from the CIN nibble rather than from anything the
         * caller tells us: the carry is a flat packet stream and the producer
         * has already forgotten where its message boundaries were by the time
         * the packets reach here, several frames later.
         */
        {
            const uint8_t cin = UI_MIDI_CIN(&c->buf[read]);
            if (c->msg_len == 0 && ui_midi_cin_opens_run(cin)) {
                c->msg_collided = 0;
                c->msg_overflow = 0;
            }
            if (c->msg_len > 0 || ui_midi_cin_opens_run(cin)) {
                if (c->msg_len + 4 <= UI_MIDI_CARRY_MSG_BYTES) {
                    memcpy(&c->msg[c->msg_len], &c->buf[read], 4);
                    c->msg_len += 4;
                } else {
                    /* Too long to hold. Keep the run OPEN (msg_len stays put)
                     * so the close below still fires and clears the state --
                     * it simply cannot be retried. */
                    c->msg_overflow = 1;
                }
                if (ui_midi_cin_closes_run(cin)) run_closed = 1;
            }
        }
        if (ui_midi_carry_last_n < UI_MIDI_CARRY_TRACK) {
            const int q = ui_midi_carry_last_n++;
            ui_midi_carry_last_slot[q] = slot;
            memcpy(ui_midi_carry_last_pkt[q], &c->buf[read], 4);
        }
        slot += 4;
        read += 4;
        placed++;
        /* Pace: see UI_MIDI_CARRY_PACKETS_PER_FRAME. The remainder stays in
         * the carry and goes out on following frames, in order, exactly as it
         * does when the region fills. Read through the accessor so the value
         * can be searched on hardware without a rebuild; it is a plain int
         * written by the control side, so a torn read is not possible on any
         * platform this runs on and a stale one costs one frame. */
        if (placed >= ui_midi_carry_pace) break;
    }

    if (read > 0) {
        int remain = c->len - read;
        if (remain > 0) memmove(c->buf, &c->buf[read], (size_t)remain);
        c->len = remain;
    }

    /*
     * A COLLIDED MESSAGE IS RE-QUEUED, WHOLE.
     *
     * Only System Realtime bytes (0xF8-0xFF) may appear inside a SysEx. A Note
     * On spliced into one is a protocol violation and a conformant receiver
     * MUST abort the message -- so when Move's own cable-2 output lands in the
     * mailbox mid-run, the E16 is RIGHT to draw nothing useful, and no amount
     * of pacing changes that.
     *
     * Isolated on hardware 2026-09-11 with the transport STOPPED both ways:
     * playing notes with Move's MIDI out ON garbles, MIDI out OFF is clean.
     * The same session sent 559 packets/sec with no foreign traffic and stayed
     * perfectly clean, which is what rules out the rate explanation this bug
     * wore for three sessions. (The earlier "interleave ruled out" verdict came
     * from disabling MIDI *sync* while note output stayed on, so `foreign`
     * never reached zero and the experiment measured nothing.)
     *
     * We cannot stop Move sending notes -- that is real MIDI somebody's gear is
     * listening to, and holding it back would put ~15 ms of jitter on the wire
     * to spare a screen. So the message is simply SENT AGAIN: the corrupt copy
     * has already gone out, and the good one follows ~15 ms later instead of
     * waiting up to 1.5 s for the surface's self-heal heartbeat. A visible
     * garble becomes a flicker.
     *
     * Re-queued at the TAIL, never the head: anything already behind it in the
     * carry was produced later and must still go out in order, and a retry that
     * jumped the queue would reorder two of our own messages to fix one.
     */
    if (run_closed) {
        const int collided = c->msg_collided ||
                             (foreign_this_frame > 0 && (was_mid_run || placed > 0));
        if (collided && c->msg_overflow) {
            /* Counted rather than skipped: a retry that quietly does not happen
             * looks exactly like one that happened and collided again. */
            ui_midi_carry_unretryable++;
        } else if (collided && c->msg_retries < UI_MIDI_CARRY_MSG_RETRIES &&
                   /*
                    * A RETRY MUST NEVER COST A DROP.
                    *
                    * `msg_retries` lives on the CARRY, not on the message, so
                    * two messages interleaving can reset each other's count and
                    * evade the cap above -- rare (the carry is usually empty by
                    * the time a run closes) but not impossible while knobs are
                    * being spun. Requiring half the carry free bounds the
                    * amplification structurally instead of relying on the
                    * count: retries can only happen when there is room for
                    * them, so they can never push a real message out. Dropping
                    * a packet to repair a garble would be repairing it with the
                    * identical fault one buffer along.
                    */
                   c->len + c->msg_len <= UI_MIDI_CARRY_BYTES / 2) {
            const int retries = c->msg_retries + 1;
            memcpy(&c->buf[c->len], c->msg, (size_t)c->msg_len);
            c->len += c->msg_len;
            ui_midi_carry_retries++;
            /* Carried across the re-queue, or the copy we just appended starts
             * from zero attempts and the cap never binds. */
            c->msg_retries = retries;
            c->msg_len = 0;
            c->msg_collided = 0;
            c->msg_overflow = 0;
            return placed;
        }
        c->msg_len = 0;
        c->msg_collided = 0;
        c->msg_retries = 0;
        c->msg_overflow = 0;
    } else if (foreign_this_frame > 0 && (was_mid_run || c->msg_len > 0)) {
        /* The run is still open and a foreign packet landed during it. Latch
         * it now -- by the time the run closes, several frames later, this
         * frame's mailbox is long gone. */
        c->msg_collided = 1;
    }

    return placed;
}

#endif /* UI_MIDI_OUT_CARRY_H */
