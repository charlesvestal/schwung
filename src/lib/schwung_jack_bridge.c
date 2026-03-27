// schwung_jack_bridge.c — Shim-side JACK bridge
// Manages shared memory and transfers audio/MIDI/display between
// the SPI shadow buffer and the JACK shadow driver.

#include "schwung_jack_bridge.h"
#include "schwung_spi_lib.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// ============================================================================
// Create / Destroy
// ============================================================================

SchwungJackShm *schwung_jack_bridge_create(void) {
    int fd = shm_open(SCHWUNG_JACK_SHM_PATH, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return NULL;

    if (ftruncate(fd, SCHWUNG_JACK_SHM_SIZE) < 0) {
        close(fd);
        return NULL;
    }

    void *ptr = mmap(NULL, SCHWUNG_JACK_SHM_SIZE,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED)
        return NULL;

    SchwungJackShm *shm = (SchwungJackShm *)ptr;
    memset(shm, 0, sizeof(*shm));
    __atomic_store_n(&shm->magic, SCHWUNG_JACK_MAGIC, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->version, SCHWUNG_JACK_VERSION, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->frame_counter, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->jack_frame_done, 0, __ATOMIC_RELEASE);

    return shm;
}

void schwung_jack_bridge_destroy(SchwungJackShm *shm) {
    if (!shm)
        return;
    munmap(shm, SCHWUNG_JACK_SHM_SIZE);
    shm_unlink(SCHWUNG_JACK_SHM_PATH);
}

// ============================================================================
// Helpers
// ============================================================================

static inline int16_t saturating_add_i16(int16_t a, int16_t b) {
    int32_t sum = (int32_t)a + (int32_t)b;
    if (sum > 32767)  return 32767;
    if (sum < -32768) return -32768;
    return (int16_t)sum;
}

static inline int jack_is_active(SchwungJackShm *shm) {
    uint32_t fc = __atomic_load_n(&shm->frame_counter, __ATOMIC_ACQUIRE);
    uint32_t jd = __atomic_load_n(&shm->jack_frame_done, __ATOMIC_ACQUIRE);
    // JACK sets jd = fc after processing. By the time bridge_pre runs
    // next frame, fc has been incremented, so jd == fc-1 is normal.
    // Consider JACK active if it processed within the last 2 frames.
    return (fc - jd) <= 2;
}

// ============================================================================
// Phase 1: Wake JACK — called early in pre-ioctl so JACK computes audio
// in parallel with the shim's DSP render.
// ============================================================================

void schwung_jack_bridge_wake(SchwungJackShm *shm) {
    if (!shm) return;
    if (!jack_is_active(shm)) return;

    volatile uint8_t *audio_ready = (volatile uint8_t *)(((uint8_t *)shm) + 3940);
    *audio_ready = 0;
    __sync_synchronize();

    __atomic_add_fetch(&shm->frame_counter, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &shm->frame_counter, FUTEX_WAKE, 1, NULL, NULL, 0);
}

// ============================================================================
// Phase 2: Read JACK audio — called inside mix_from_buffer.  Waits for JACK
// to finish writing, returns pointer to 128-frame stereo interleaved int16.
// Returns NULL if JACK is not active.
// ============================================================================

const int16_t *schwung_jack_bridge_read_audio(SchwungJackShm *shm) {
    if (!shm) return NULL;
    if (!jack_is_active(shm)) return NULL;

    volatile uint8_t *audio_ready = (volatile uint8_t *)(((uint8_t *)shm) + 3940);
    int spins = 0;
    while (!*audio_ready && spins < 2000000) {
        spins++;
    }

    return shm->audio_out;
}

// ============================================================================
// Phase 3: Display — called late in pre-ioctl.  Audio is handled above.
// ============================================================================

int schwung_jack_bridge_pre(SchwungJackShm *shm, uint8_t *shadow) {
    if (!shm || !shadow)
        return 0;

    if (!jack_is_active(shm))
        return 0;

    if (shm->display_active) {
        uint32_t idx = *(uint32_t *)(shadow + SCHWUNG_OFF_IN_DISP_STAT);
        ((uint8_t *)shm)[4090] = (uint8_t)(idx & 0xFF);
        ((uint8_t *)shm)[4091]++;
        if (idx >= 1 && idx <= 5) {
            memcpy(shadow + SCHWUNG_OFF_OUT_DISP_DATA,
                   shm->display_data + (idx - 1) * SCHWUNG_OUT_DISP_CHUNK_LEN,
                   SCHWUNG_OUT_DISP_CHUNK_LEN);
        } else if (idx == 6) {
            memcpy(shadow + SCHWUNG_OFF_OUT_DISP_DATA,
                   shm->display_data + 5 * SCHWUNG_OUT_DISP_CHUNK_LEN,
                   SCHWUNG_JACK_DISPLAY_SIZE - 5 * SCHWUNG_OUT_DISP_CHUNK_LEN);
        }
    }
    return 1;
}

// ============================================================================
// MIDI_OUT stash — pre-transfer buffer for sequencer notes
// ============================================================================

/* MIDI_OUT is consumed by SPI ioctl, so we stash cable-2 events pre-transfer
 * and the bridge picks them up post-transfer. */
static SchwungJackMidiEvent g_midi_out_stash[SCHWUNG_JACK_MIDI_IN_MAX];
static uint8_t g_midi_out_stash_count = 0;

static int g_midi_out_stash_log_count = 0;

void schwung_jack_bridge_stash_midi_out(const uint8_t *midi_out_buf, int overtake_mode) {
    (void)overtake_mode;  /* Always stash — JACK may be running while suspended */
    g_midi_out_stash_count = 0;

    for (int i = 0; i < 256 && g_midi_out_stash_count < SCHWUNG_JACK_MIDI_IN_MAX; i += 4) {
        uint8_t p0 = midi_out_buf[i], p1 = midi_out_buf[i+1];
        uint8_t p2 = midi_out_buf[i+2], p3 = midi_out_buf[i+3];
        if (!p0 && !p1 && !p2 && !p3) continue;
        uint8_t cable = (p0 >> 4) & 0x0F;
        if (cable != 2) continue;
        uint8_t cin = p0 & 0x0F;
        if (cin < 0x8 || cin > 0xE) continue;
        /* Ch16 (0xF) → ch1 (0x0) */
        uint8_t ch = p1 & 0x0F;
        SchwungJackMidiEvent jev;
        jev.message.cin = cin;
        jev.message.cable = cable;
        jev.message.midi.type = (p1 >> 4) & 0x0F;
        jev.message.midi.channel = (ch == 0xF) ? 0x0 : ch;
        jev.message.midi.data1 = p2;
        jev.message.midi.data2 = p3;
        jev.timestamp = 0;
        g_midi_out_stash[g_midi_out_stash_count++] = jev;
    }
    /* Debug: log first few stash events */
    if (g_midi_out_stash_count > 0 && g_midi_out_stash_log_count < 50) {
        FILE *f = fopen("/data/UserData/schwung/debug.log", "a");
        if (f) {
            fprintf(f, "[DEBUG] [jack_bridge] stash: %d events, first: type=0x%X ch=%d d1=%d d2=%d\n",
                g_midi_out_stash_count,
                g_midi_out_stash[0].message.midi.type,
                g_midi_out_stash[0].message.midi.channel,
                g_midi_out_stash[0].message.midi.data1,
                g_midi_out_stash[0].message.midi.data2);
            fclose(f);
        }
        g_midi_out_stash_log_count++;
    }
}

// ============================================================================
// Post-transfer: copy capture data to shm, split MIDI, wake JACK
// ============================================================================

void schwung_jack_bridge_post(SchwungJackShm *shm, uint8_t *shadow,
                               const uint8_t *hw,
                               const volatile uint8_t *overtake_mode_ptr,
                               const volatile uint8_t *shift_held_ptr) {
    if (!shm || !shadow)
        return;

    // --- Audio: copy captured audio from shadow input to shm ---
    const int16_t *capture = (const int16_t *)(shadow + SCHWUNG_OFF_IN_AUDIO);
    memcpy(shm->audio_in, capture, SCHWUNG_JACK_AUDIO_FRAMES * 2 * sizeof(int16_t));

    // --- MIDI: scan input events from RAW HARDWARE buffer, split by cable ---
    // Use hw (not shadow) so JACK gets raw pad MIDI before Move's scale mapping.
    // This is critical for rnbomovecontrol which expects raw chromatic pad notes.
    const SchwungMidiEvent *in_events =
        (const SchwungMidiEvent *)(hw + SCHWUNG_OFF_IN_MIDI);

    uint8_t c0_count = 0;
    uint8_t ext_count = 0;

    for (int i = 0; i < SCHWUNG_MIDI_IN_MAX; i++) {
        SchwungMidiEvent ev = in_events[i];
        if (schwung_usb_midi_msg_is_empty(ev.message))
            break;

        uint8_t cable = ev.message.cable;

        if (cable == 0 && c0_count < SCHWUNG_JACK_MIDI_IN_MAX
            && overtake_mode_ptr && *overtake_mode_ptr >= 2) {
            /* When Shift is held, block system shortcut events from reaching
             * JACK/RNBO. These are Shift+Vol combos (suspend, exit, slot
             * select, etc.) — not intended for the module. Passing volume
             * touch causes RNBO to hide knob LEDs; passing Back/Jog causes
             * RNBO to navigate when it should just be suspending/exiting. */
            if (shift_held_ptr && *shift_held_ptr) {
                uint8_t msg_type = ev.message.midi.type;   /* upper nibble of status */
                uint8_t d1 = ev.message.midi.data1;
                /* Note 8 = volume touch (type 0x9=NoteOn, 0x8=NoteOff) */
                if ((msg_type == 0x9 || msg_type == 0x8) && d1 == 8) continue;
                if (msg_type == 0xB) {
                    /* CC 79 = volume knob value */
                    if (d1 == 79) continue;
                    /* CC 51 = back button (Shift+Vol+Back = suspend) */
                    if (d1 == 51) continue;
                    /* CC 3 = jog click (Shift+Vol+Jog = exit overtake) */
                    if (d1 == 3) continue;
                    /* CC 40-43 = track buttons (Shift+Vol+Track = slot select) */
                    if (d1 >= 40 && d1 <= 43) continue;
                }
            }
            SchwungJackMidiEvent jev;
            jev.message.cin = ev.message.cin;
            jev.message.cable = ev.message.cable;
            jev.message.midi.channel = ev.message.midi.channel;
            jev.message.midi.type = ev.message.midi.type;
            jev.message.midi.data1 = ev.message.midi.data1;
            jev.message.midi.data2 = ev.message.midi.data2;
            jev.timestamp = ev.timestamp;
            shm->midi_to_jack[c0_count++] = jev;
        } else if (cable >= 2 && ext_count < SCHWUNG_JACK_MIDI_IN_MAX) {
            SchwungJackMidiEvent jev;
            jev.message.cin = ev.message.cin;
            jev.message.cable = ev.message.cable;
            jev.message.midi.channel = ev.message.midi.channel;
            jev.message.midi.type = ev.message.midi.type;
            jev.message.midi.data1 = ev.message.midi.data1;
            jev.message.midi.data2 = ev.message.midi.data2;
            jev.timestamp = ev.timestamp;
            shm->ext_midi_to_jack[ext_count++] = jev;
        }
    }

    /* Append any buffered MIDI_OUT events (stashed in pre-transfer).
     * MIDI_OUT is consumed by SPI ioctl, so post-transfer it's empty — the shim
     * calls schwung_jack_bridge_stash_midi_out() before transfer. */
    for (uint8_t i = 0; i < g_midi_out_stash_count && ext_count < SCHWUNG_JACK_MIDI_IN_MAX; i++) {
        shm->ext_midi_to_jack[ext_count++] = g_midi_out_stash[i];
    }
    g_midi_out_stash_count = 0;

    shm->midi_to_jack_count = c0_count;
    shm->ext_midi_to_jack_count = ext_count;

    // Debug: store total event count and first event raw bytes for debugging
    // Use bytes after display_active (end of struct), before page boundary.
    // display_data ends at offsetof(display_active)+1; safe area starts ~3080.
    {
        uint8_t *dbg = ((uint8_t *)shm) + 3080;
        int total = 0;
        for (int i = 0; i < SCHWUNG_MIDI_IN_MAX; i++) {
            SchwungMidiEvent ev = in_events[i];
            if (schwung_usb_midi_msg_is_empty(ev.message)) break;
            total++;
        }
        dbg[0] = (uint8_t)total;     // total events seen
        dbg[1] = (uint8_t)c0_count;  // cable 0 count
        dbg[2] = (uint8_t)ext_count; // ext count
        // First event raw bytes
        if (total > 0) {
            const uint8_t *raw = (const uint8_t *)&in_events[0];
            dbg[3] = raw[0]; dbg[4] = raw[1]; dbg[5] = raw[2]; dbg[6] = raw[3];
        }
    }

    // --- frame_counter increment and futex wake moved to bridge_pre ---
    // (audio is now synchronous: wake → wait for write → mix → SPI send)
}
