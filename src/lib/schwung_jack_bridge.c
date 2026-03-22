// schwung_jack_bridge.c — Shim-side JACK bridge
// Manages shared memory and transfers audio/MIDI/display between
// the SPI shadow buffer and the JACK shadow driver.

#include "schwung_jack_bridge.h"
#include "schwung_spi_lib.h"

#include <fcntl.h>
#include <linux/futex.h>
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
// Pre-transfer: mix JACK output into shadow before shadow→hardware copy
// ============================================================================

void schwung_jack_bridge_pre(SchwungJackShm *shm, uint8_t *shadow) {
    if (!shm || !shadow)
        return;

    if (!jack_is_active(shm))
        return;

    // --- Audio: add JACK's audio_out into shadow output buffer (saturating) ---
    int16_t *shadow_audio = (int16_t *)(shadow + SCHWUNG_OFF_OUT_AUDIO);
    const int16_t *jack_audio = shm->audio_out;
    int num_samples = SCHWUNG_AUDIO_FRAMES * 2; // stereo interleaved

    for (int i = 0; i < num_samples; i++)
        shadow_audio[i] = saturating_add_i16(shadow_audio[i], jack_audio[i]);

    // --- MIDI: drain JACK's MIDI output into shadow buffer ---
    // Send up to 20 messages per SPI frame (hardware limit).
    // Track read position so large bursts drain across multiple frames.
    {
        static uint8_t midi_read_pos = 0;
        static uint8_t midi_total = 0;
        static uint8_t ext_read_pos = 0;
        static uint8_t ext_total = 0;

        // Check for new data from JACK
        uint8_t new_count = shm->midi_from_jack_count;
        uint8_t new_ext = shm->ext_midi_from_jack_count;
        if (new_count != midi_total || new_ext != ext_total) {
            // New batch from JACK — reset read positions
            midi_read_pos = 0;
            midi_total = new_count;
            ext_read_pos = 0;
            ext_total = new_ext;
        }

        // Anything left to send?
        if (midi_read_pos < midi_total || ext_read_pos < ext_total) {
            SchwungUsbMidiMsg *out_midi = (SchwungUsbMidiMsg *)(shadow + SCHWUNG_OFF_OUT_MIDI);
            int slot = 0;

            // Send cable-0 MIDI (up to 20 per frame)
            while (midi_read_pos < midi_total && slot < SCHWUNG_MIDI_OUT_MAX) {
                SchwungJackUsbMidiMsg jmsg = shm->midi_from_jack[midi_read_pos++];
                SchwungUsbMidiMsg smsg;
                smsg.cin = jmsg.cin;
                smsg.cable = jmsg.cable;
                smsg.midi.channel = jmsg.midi.channel;
                smsg.midi.type = jmsg.midi.type;
                smsg.midi.data1 = jmsg.midi.data1;
                smsg.midi.data2 = jmsg.midi.data2;
                out_midi[slot++] = smsg;
            }

            // Send ext MIDI with remaining slots
            while (ext_read_pos < ext_total && slot < SCHWUNG_MIDI_OUT_MAX) {
                SchwungJackUsbMidiMsg jmsg = shm->ext_midi_from_jack[ext_read_pos++];
                SchwungUsbMidiMsg smsg;
                smsg.cin = jmsg.cin;
                smsg.cable = jmsg.cable;
                smsg.midi.channel = jmsg.midi.channel;
                smsg.midi.type = jmsg.midi.type;
                smsg.midi.data1 = jmsg.midi.data1;
                smsg.midi.data2 = jmsg.midi.data2;
                out_midi[slot++] = smsg;
            }

            // Zero-pad remaining
            SchwungUsbMidiMsg empty = {0};
            while (slot < SCHWUNG_MIDI_OUT_MAX) {
                out_midi[slot++] = empty;
            }
        }
    }

    // --- Display: if active, copy correct chunk to shadow ---
    // Display protocol: hardware sends index 1-6, we respond with the
    // corresponding 172-byte chunk. Index 0 = no update.
    // See ablspi.c handleDisplayOutput() for reference.
    if (shm->display_active) {
        uint32_t idx = *(uint32_t *)(shadow + SCHWUNG_OFF_IN_DISP_STAT);
        /* Debug: store display index at a known shm location */
        ((uint8_t *)shm)[4090] = (uint8_t)(idx & 0xFF);
        ((uint8_t *)shm)[4091]++; /* call counter */
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
}

// ============================================================================
// Post-transfer: copy capture data to shm, split MIDI, wake JACK
// ============================================================================

void schwung_jack_bridge_post(SchwungJackShm *shm, uint8_t *shadow,
                               const uint8_t *hw) {
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

        if (cable == 0 && c0_count < SCHWUNG_JACK_MIDI_IN_MAX) {
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

    shm->midi_to_jack_count = c0_count;
    shm->ext_midi_to_jack_count = ext_count;

    // Debug: store total event count and first event raw bytes for debugging
    // Use unused bytes at end of shm (after display_active, before page boundary)
    // Offset 2725+ is padding
    {
        uint8_t *dbg = ((uint8_t *)shm) + 2726;
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

    // --- Increment frame_counter and wake JACK via futex ---
    __atomic_add_fetch(&shm->frame_counter, 1, __ATOMIC_RELEASE);

    syscall(SYS_futex, &shm->frame_counter, FUTEX_WAKE, 1, NULL, NULL, 0);
}
