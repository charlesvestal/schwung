/*
Copyright (C) 2026 Charles Vestal
License: MIT

JackShadowDriver — JACK audio driver that shares audio/MIDI/display with
Move's firmware via shared memory (/dev/shm/schwung_jack).
Based on JackMoveDriver by Cycling '74 (GPL-2.0).
*/

#define __STDC_FORMAT_MACROS
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <limits>
#include <algorithm>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <fcntl.h>

#include "jack.h"
#include "JackShadowDriver.h"
#include "JackEngineControl.h"
#include "JackClientControl.h"
#include "JackPort.h"
#include "JackGraphManager.h"
#include "JackLockedEngine.h"
#include "JackPosixThread.h"
#include "JackCompilerDeps.h"
#include "JackServerGlobals.h"
#include "JackMidiBufferReadQueue.h"
#include "JackMidiBufferWriteQueue.h"

namespace Jack
{

static const char * DISPLAY_HEADER = "MOVEDISP";
static const size_t DISPLAY_HEADER_LEN = 8;
static const size_t DISPLAY_DATA_LEN = DISPLAY_HEADER_LEN + SCHWUNG_JACK_DISPLAY_SIZE;

static const jack_default_audio_sample_t mul =
    std::min(static_cast<jack_default_audio_sample_t>(std::numeric_limits<int16_t>::max()),
             static_cast<jack_default_audio_sample_t>(std::abs(std::numeric_limits<int16_t>::min())));

static inline int16_t toint(jack_default_audio_sample_t v) {
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    return static_cast<int16_t>(v * mul);
}

static inline jack_default_audio_sample_t fromint(int16_t v) {
    return static_cast<jack_default_audio_sample_t>(v) / mul;
}

static inline void futex_wait(uint32_t *addr, uint32_t expected, const struct timespec *timeout) {
    syscall(SYS_futex, addr, FUTEX_WAIT, expected, timeout, NULL, 0);
}

/* ---- Open / Close ---- */

int JackShadowDriver::Open(jack_nframes_t nframes, jack_nframes_t samplerate,
    bool capturing, bool playing, int inchannels, int outchannels,
    bool monitor, const char* capture_driver_name,
    const char* playback_driver_name,
    jack_nframes_t capture_latency, jack_nframes_t playback_latency)
{
    if (JackAudioDriver::Open(nframes, samplerate, capturing, playing,
                              inchannels, outchannels, monitor,
                              capture_driver_name, playback_driver_name,
                              capture_latency, playback_latency) != 0) {
        return -1;
    }

    memset(fInputBuffer[0], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);
    memset(fInputBuffer[1], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);
    memset(fOutputBuffer[0], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);
    memset(fOutputBuffer[1], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);

    return 0;
}

int JackShadowDriver::Attach() {
    JackPort* port;
    jack_port_id_t port_index;
    char name[REAL_JACK_PORT_NAME_SIZE+1];
    char alias[REAL_JACK_PORT_NAME_SIZE+1];

    jack_log("JackShadowDriver::Attach fBufferSize = %ld fSampleRate = %ld",
             fEngineControl->fBufferSize, fEngineControl->fSampleRate);

    /* Audio capture ports */
    for (int i = 0; i < fCaptureChannels; i++) {
        snprintf(alias, sizeof(alias), "move:out%d", i + 1);
        snprintf(name, sizeof(name), "%s:capture_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name,
                JACK_DEFAULT_AUDIO_TYPE, CaptureDriverFlags,
                fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("driver: cannot register port for %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        port->SetAlias(alias);
        fCapturePortList[i] = port_index;
    }

    /* Audio playback ports */
    for (int i = 0; i < fPlaybackChannels; i++) {
        snprintf(alias, sizeof(alias), "move:in%d", i + 1);
        snprintf(name, sizeof(name), "%s:playback_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name,
                JACK_DEFAULT_AUDIO_TYPE, PlaybackDriverFlags,
                fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("driver: cannot register port for %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        port->SetAlias(alias);
        fPlaybackPortList[i] = port_index;
    }

    jack_nframes_t latency = SCHWUNG_JACK_AUDIO_FRAMES;
    jack_latency_range_t latency_range;
    latency_range.max = latency;
    latency_range.min = latency;

    /* MIDI ports */
    {
        if (fEngine->PortRegister(fClientControl.fRefNum, "system:midi_playback",
                JACK_DEFAULT_MIDI_TYPE, PlaybackDriverFlags,
                fEngineControl->fBufferSize, &fMIDIPlaybackId) < 0) return -1;
        fGraphManager->GetPort(fMIDIPlaybackId)->SetLatencyRange(JackPlaybackLatency, &latency_range);
    }
    {
        if (fEngine->PortRegister(fClientControl.fRefNum, "system:midi_capture",
                JACK_DEFAULT_MIDI_TYPE, CaptureDriverFlags,
                fEngineControl->fBufferSize, &fMIDICaptureId) < 0) return -1;
        fGraphManager->GetPort(fMIDICaptureId)->SetLatencyRange(JackCaptureLatency, &latency_range);
    }
    {
        if (fEngine->PortRegister(fClientControl.fRefNum, "system:midi_playback_ext",
                JACK_DEFAULT_MIDI_TYPE, PlaybackDriverFlags,
                fEngineControl->fBufferSize, &fExternalMIDIPlaybackId) < 0) return -1;
        fGraphManager->GetPort(fExternalMIDIPlaybackId)->SetLatencyRange(JackPlaybackLatency, &latency_range);
    }
    {
        if (fEngine->PortRegister(fClientControl.fRefNum, "system:midi_capture_ext",
                JACK_DEFAULT_MIDI_TYPE, CaptureDriverFlags,
                fEngineControl->fBufferSize, &fExternalMIDICaptureId) < 0) return -1;
        fGraphManager->GetPort(fExternalMIDICaptureId)->SetLatencyRange(JackCaptureLatency, &latency_range);
    }
    {
        if (fEngine->PortRegister(fClientControl.fRefNum, "system:display",
                JACK_DEFAULT_MIDI_TYPE, PlaybackDriverFlags,
                fEngineControl->fBufferSize, &fDisplayId) < 0) return -1;
    }

    UpdateLatencies();

    /* Open shared memory */
    int fd = shm_open(SCHWUNG_JACK_SHM_PATH, O_RDWR, 0666);
    if (fd < 0) {
        jack_error("JackShadowDriver::Attach: shm_open(%s) failed: %s",
                   SCHWUNG_JACK_SHM_PATH, strerror(errno));
        return -1;
    }
    fShm = (SchwungJackShm *)mmap(NULL, SCHWUNG_JACK_SHM_SIZE,
                                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (fShm == MAP_FAILED) {
        jack_error("JackShadowDriver: mmap failed");
        fShm = NULL;
        return -1;
    }
    if (fShm->magic != SCHWUNG_JACK_MAGIC || fShm->version != SCHWUNG_JACK_VERSION) {
        jack_error("JackShadowDriver: bad magic/version (got 0x%x v%d)",
                   fShm->magic, fShm->version);
        munmap(fShm, SCHWUNG_JACK_SHM_SIZE);
        fShm = NULL;
        return -1;
    }

    fLastFrameCounter = fShm->frame_counter;
    jack_info("JackShadowDriver: attached to shared memory, frame_counter=%u", fLastFrameCounter);

    return 0;
}

int JackShadowDriver::Close() {
    if (fShm) {
        munmap(fShm, SCHWUNG_JACK_SHM_SIZE);
        fShm = NULL;
    }
    return JackAudioDriver::Close();
}

/* ---- Process ----
 * Matches JackMoveDriver::Process() pattern:
 * 1. Drain MIDI queues from previous cycle → write to shm
 * 2. Read audio output from previous cycle → write to shm
 * 3. Block until next frame (futex wait)
 * 4. Run the JACK graph (Read → client processing → Write)
 *
 * Key insight: Write() enqueues MIDI to async queues. Process() drains
 * those queues to shm BEFORE blocking. This matches how JackMoveDriver
 * uses provideMidi() inside ablspiSendAndReceiveAudioAndMidi().
 */
int JackShadowDriver::Process()
{
    if (!fShm) return -1;

    /* Drain MIDI playback queues → shm (from previous cycle's Write()).
     * Uses the same encoding logic as JackMoveDriver::provideMidi():
     * - Channel 16 messages → rewrite to channel 0
     * - Sysex messages → fragment into 3-byte USB-MIDI packets with proper CIN
     * - System messages (reset, sysex start/end) → pass through */
    {
        jack_midi_event_t *first = fMIDIReadQueue.DequeueEvent();
        if (!first) {
            fShm->midi_from_jack_count = 0;
        } else {
            int count = 0;
            const int MAX_PER_CYCLE = 20; /* Match SPI hardware limit */

            /* Encode JACK MIDI → USB-MIDI, matching JackMoveDriver::provideMidi().
             * JACK splits sysex into 3-byte events. We must track sysex state
             * across events (not just within one event).
             *
             * Important: check count AFTER processing each event, BEFORE
             * dequeuing the next. This avoids losing the event that was
             * already dequeued by the while-condition. */
            jack_midi_event_t *e = first;
            for (;;) {
                if (!e || e->size == 0) goto next_event;

                {
                uint8_t *data = e->buffer;
                size_t size = e->size;
                uint8_t status = data[0];
                uint8_t chan = status & 0x0F;
                uint8_t masked = status & 0xF0;

                /* Filter: only pass ch16 channel msgs, sysex, system, and data bytes */
                if ((masked == 0x80 || masked == 0x90 || masked == 0xB0)) {
                    if (chan != 15) goto next_event;
                    data[0] = masked; /* rewrite to ch0 */
                } else if (status == 0xFF || status == 0xF0 || status == 0xF7 || (status & 0x80) == 0) {
                    /* sysex start/end, reset, or data continuation — pass through */
                } else {
                    goto next_event;
                }

                if (size <= 3) {
                    SchwungJackUsbMidiMsg m;
                    m.cable = 0;
                    m.midi.channel = data[0] & 0x0F;
                    m.midi.type = (data[0] & 0xF0) >> 4;
                    m.midi.data1 = size > 1 ? data[1] : 0;
                    m.midi.data2 = size > 2 ? data[2] : 0;

                    /* Determine CIN based on sysex state (matching provideMidi) */
                    if (data[0] == 0xF0) {
                        /* Sysex start */
                        m.cin = 0x4;
                        if (size > 2 && data[2] == 0xF7) m.cin = 0x7;
                        else if (size > 1 && data[1] == 0xF7) m.cin = 0x6;
                        else if (size == 1 && data[0] == 0xF7) m.cin = 0x5;
                    } else if (data[0] == 0xF7) {
                        /* Sysex end (single byte) */
                        m.cin = 0x5;
                    } else if ((data[0] & 0x80) == 0) {
                        /* Data bytes — sysex continuation */
                        m.cin = 0x4;
                        /* Check for F7 ending */
                        if (size >= 3 && data[2] == 0xF7) m.cin = 0x7;
                        else if (size >= 2 && data[1] == 0xF7) m.cin = 0x6;
                        else if (data[0] == 0xF7) m.cin = 0x5;
                    } else {
                        /* Normal channel message */
                        m.cin = masked >> 4;
                    }

                    if (count < SCHWUNG_JACK_MIDI_OUT_MAX)
                        fShm->midi_from_jack[count++] = m;
                } else {
                    /* Long sysex (>3 bytes): fragment */
                    for (size_t i = 0; i < size && count < SCHWUNG_JACK_MIDI_OUT_MAX; i += 3) {
                        size_t len = std::min(size - i, (size_t)3);
                        SchwungJackUsbMidiMsg m;
                        m.cable = 0;
                        m.midi.channel = data[i] & 0x0F;
                        m.midi.type = (data[i] & 0xF0) >> 4;
                        m.midi.data1 = len > 1 ? data[i + 1] : 0;
                        m.midi.data2 = len > 2 ? data[i + 2] : 0;

                        uint8_t cin = 0x4; /* sysex continue */
                        if (i + len >= size) {
                            /* Last packet */
                            switch (len) {
                                case 1: cin = 0x5; break;
                                case 2: cin = 0x6; break;
                                case 3: cin = (data[i+2] == 0xF7) ? 0x7 : 0x4; break;
                            }
                        }
                        m.cin = cin;
                        fShm->midi_from_jack[count++] = m;
                    }
                }
                }

            next_event:
                /* Stop dequeuing once we've filled the SPI limit.
                 * Remaining events stay in the ringbuffer for next cycle. */
                if (count >= MAX_PER_CYCLE) break;
                e = fMIDIReadQueue.DequeueEvent();
                if (!e) break;
            }

            fShm->midi_from_jack_count = count;
        }
    }
    /* External MIDI — simpler, no channel filter */
    {
        jack_midi_event_t *first = fExternalMIDIReadQueue.DequeueEvent();
        if (first) {
            int count = 0;
            const int EXT_MAX = 20;
            jack_midi_event_t *e = first;
            do {
                if (!e || e->size == 0 || e->size > 3) continue;
                if (count >= EXT_MAX) break;
                uint8_t *data = e->buffer;
                SchwungJackUsbMidiMsg m;
                m.cable = 2;
                m.cin = (data[0] >> 4) & 0x0F;
                m.midi.channel = data[0] & 0x0F;
                m.midi.type = (data[0] >> 4) & 0x0F;
                m.midi.data1 = e->size > 1 ? data[1] : 0;
                m.midi.data2 = e->size > 2 ? data[2] : 0;
                fShm->ext_midi_from_jack[count++] = m;
            } while ((e = fExternalMIDIReadQueue.DequeueEvent()));
            fShm->ext_midi_from_jack_count = count;
        }
    }

    /* Block until shim signals next frame */
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 10000000; // 10ms timeout

    futex_wait(&fShm->frame_counter, fLastFrameCounter, &timeout);
    fLastFrameCounter = fShm->frame_counter;

    /* Audio: write IMMEDIATELY after wake, before the graph runs.
     * Then signal jack_frame_done so the bridge's busywait can proceed. */
    {
        int16_t* audio_out = fShm->audio_out;
        for (int i = 0; i < SCHWUNG_JACK_AUDIO_FRAMES; i++) {
            audio_out[i * 2]     = toint(fOutputBuffer[0][i]);
            audio_out[i * 2 + 1] = toint(fOutputBuffer[1][i]);
        }
    }
    /* Signal audio is written — bridge_pre busywaits on this */
    __sync_synchronize();
    *(volatile uint8_t *)(((uint8_t *)fShm) + 3940) = 1;
    fShm->jack_frame_done = fLastFrameCounter;

    JackDriver::CycleTakeBeginTime();
    int r = JackAudioDriver::Process();
    JackDriver::CycleTakeEndTime();

    return r;
}

/* ---- Read: shm → JACK port buffers ---- */

int JackShadowDriver::Read()
{
    if (!fShm) return -1;

    /* Audio: deinterleave int16 → float */
    const int16_t* audio_in = fShm->audio_in;
    for (int i = 0; i < SCHWUNG_JACK_AUDIO_FRAMES; i++) {
        fInputBuffer[0][i] = fromint(audio_in[i * 2]);
        fInputBuffer[1][i] = fromint(audio_in[i * 2 + 1]);
    }
    for (int i = 0; i < fCaptureChannels; i++) {
        memcpy(GetInputBuffer(i), fInputBuffer[i],
               sizeof(jack_default_audio_sample_t) * fEngineControl->fBufferSize);
    }

    /* MIDI capture cable 0 */
    {
        JackMidiBuffer *buf = reinterpret_cast<JackMidiBuffer *>(
            fGraphManager->GetBuffer(fMIDICaptureId, fEngineControl->fBufferSize));
        JackMidiBufferWriteQueue wq;
        wq.ResetMidiBuffer(buf, fEngineControl->fBufferSize);

        uint8_t count = fShm->midi_to_jack_count;
        for (uint8_t i = 0; i < count; i++) {
            auto &ev = fShm->midi_to_jack[i];
            auto &midi = ev.message.midi;

            /* Rewrite cable 0 to channel 16 (same as JackMoveDriver) */
            uint8_t data[3];
            switch (midi.type) {
                case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE:
                    data[0] = static_cast<uint8_t>(15 | (midi.type << 4));
                    break;
                default:
                    data[0] = static_cast<uint8_t>((midi.type << 4) | midi.channel);
                    break;
            }
            data[1] = midi.data1;
            data[2] = midi.data2;

            size_t size = 3;
            switch (ev.message.cin) {
                case 0x5: case 0xF: size = 1; break;
                case 0x2: case 0x6: case 0xC: case 0xD: size = 2; break;
                default: break;
            }

            jack_midi_event_t e;
            e.time = 0; /* all events at start of buffer */
            e.size = size;
            e.buffer = data;
            wq.EnqueueEvent(&e);
        }
    }

    /* MIDI capture external */
    {
        JackMidiBuffer *buf = reinterpret_cast<JackMidiBuffer *>(
            fGraphManager->GetBuffer(fExternalMIDICaptureId, fEngineControl->fBufferSize));
        JackMidiBufferWriteQueue wq;
        wq.ResetMidiBuffer(buf, fEngineControl->fBufferSize);

        uint8_t count = fShm->ext_midi_to_jack_count;
        for (uint8_t i = 0; i < count; i++) {
            auto &ev = fShm->ext_midi_to_jack[i];
            auto &midi = ev.message.midi;

            uint8_t data[3] = {
                static_cast<uint8_t>((midi.type << 4) | midi.channel),
                midi.data1, midi.data2
            };
            size_t size = 3;
            switch (ev.message.cin) {
                case 0x5: case 0xF: size = 1; break;
                case 0x2: case 0x6: case 0xC: case 0xD: size = 2; break;
                default: break;
            }

            jack_midi_event_t e;
            e.time = 0;
            e.size = size;
            e.buffer = data;
            wq.EnqueueEvent(&e);
        }
    }

    /* Display: read from system:display port, write to shm */
    {
        JackMidiBuffer *buf = reinterpret_cast<JackMidiBuffer *>(
            fGraphManager->GetBuffer(fDisplayId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue rq;
        rq.ResetMidiBuffer(buf);
        while (auto e = rq.DequeueEvent()) {
            if (e->size == DISPLAY_DATA_LEN) {
                memcpy(fShm->display_data, e->buffer + DISPLAY_HEADER_LEN,
                       SCHWUNG_JACK_DISPLAY_SIZE);
            }
        }
    }

    return 0;
}

/* ---- Write: JACK port buffers → async queues ----
 * Matches JackMoveDriver::Write() pattern: read playback buffers into
 * async queues. Process() drains them to shm on the next cycle.
 */
int JackShadowDriver::Write()
{
    if (!fShm) return -1;

    /* MIDI: read playback buffers → async queues (drained in Process()) */
    {
        JackMidiBuffer *buf = reinterpret_cast<JackMidiBuffer *>(
            fGraphManager->GetBuffer(fMIDIPlaybackId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue rq;
        rq.ResetMidiBuffer(buf);
        int enqueued = 0, dropped = 0;
        while (auto e = rq.DequeueEvent()) {
            auto result = fMIDIReadQueue.EnqueueEvent(e);
            if (result == JackMidiWriteQueue::OK) enqueued++;
            else dropped++;
        }
        /* Debug counters at shm offsets 3904-3911 */
        if (fShm) {
            uint8_t *dbg = reinterpret_cast<uint8_t*>(fShm) + 3904;
            /* Cumulative enqueued (32-bit LE) */
            uint32_t *p_enq = reinterpret_cast<uint32_t*>(dbg);
            uint32_t *p_drop = reinterpret_cast<uint32_t*>(dbg + 4);
            *p_enq += enqueued;
            *p_drop += dropped;
        }
    }
    {
        JackMidiBuffer *buf = reinterpret_cast<JackMidiBuffer *>(
            fGraphManager->GetBuffer(fExternalMIDIPlaybackId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue rq;
        rq.ResetMidiBuffer(buf);
        while (auto e = rq.DequeueEvent()) {
            fExternalMIDIReadQueue.EnqueueEvent(e);
        }
    }

    /* Audio: copy output buffers (converted to shm in Process()) */
    for (int i = 0; i < fPlaybackChannels; i++) {
        memcpy(fOutputBuffer[i], GetOutputBuffer(i),
               sizeof(jack_default_audio_sample_t) * fEngineControl->fBufferSize);
    }

    return 0;
}

int JackShadowDriver::is_realtime() const { return 1; }

} // end of namespace


/* ---- Driver C entry points ---- */
#ifdef __cplusplus
extern "C" {
#endif

SERVER_EXPORT const jack_driver_desc_t* driver_get_descriptor() {
    jack_driver_desc_t *desc;
    jack_driver_desc_filler_t filler;
    desc = jack_driver_descriptor_construct("shadow",
        JackDriverMaster,
        "Schwung shadow driver (shared memory with Move shim)",
        &filler);
    return desc;
}

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(
    Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    jack_nframes_t nframes = SCHWUNG_JACK_AUDIO_FRAMES;
    jack_nframes_t samplerate = 44100;

    auto *driver = new Jack::JackShadowDriver("system", "shadow", engine, table);
    auto *threaded = new Jack::JackThreadedDriver(driver);

    if (driver->Open(nframes, samplerate, true, true, 2, 2, false,
                     "shadow", "shadow", 0, 0) == 0) {
        return threaded;
    } else {
        delete threaded;
        return NULL;
    }
}

#ifdef __cplusplus
}
#endif
