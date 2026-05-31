// SPDX-License-Identifier: MIT
//
// CoreAudio backend for schwung-host on macOS.
//
// CoreAudio is the *clock master*: its render callback fires at the negotiated
// buffer size (we request 128 frames @ 44.1 kHz ≈ every 2.9 ms) and drives the
// sim tick pipe. That pulse releases the host's blocked schwung_sim_ioctl_wait,
// the host runs one frame, and we shuttle audio between CoreAudio's float
// buffers and the SPI mailbox's int16 regions.
//
// Phase 3a (this file): just gets the clock running. Render callback pulses
// the tick pipe and returns silence. Audio I/O wiring lands in Phase 3b/c.

#include "audio_backend.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "schwung_spi_lib.h"  // SCHWUNG_PAGE_SIZE, OFF_* constants
#include "sim_backend.h"      // schwung_sim_drain_midi_in_to_mailbox

// ============================================================================
// State
// ============================================================================

static struct {
    AudioUnit  unit;
    uint8_t   *mailbox;
    int        tick_fd;
    int        started;
    uint64_t   callback_count;  // diagnostic — count callbacks
} g = { .tick_fd = -1 };

// ============================================================================
// Render callback
// ============================================================================

static OSStatus render_cb(void                       *inRefCon,
                          AudioUnitRenderActionFlags *ioActionFlags,
                          const AudioTimeStamp       *inTimeStamp,
                          UInt32                      inBusNumber,
                          UInt32                      inNumberFrames,
                          AudioBufferList            *ioData)
{
    (void)inRefCon; (void)ioActionFlags; (void)inTimeStamp; (void)inBusNumber;

    // Read AUDIO OUT region (offset 256) from the mailbox: 128 frames × stereo
    // int16, interleaved. This is whatever the host wrote in its LAST tick.
    // No sync barrier yet — we accept one-frame latency. Phase 3d adds a
    // host-done semaphore for same-callback latency.
    if (ioData && ioData->mNumberBuffers >= 2 && g.mailbox) {
        const int16_t *src = (const int16_t *)(g.mailbox + SCHWUNG_OFF_OUT_AUDIO);
        float *L = (float *)ioData->mBuffers[0].mData;
        float *R = (float *)ioData->mBuffers[1].mData;
        UInt32 frames = inNumberFrames < SCHWUNG_AUDIO_FRAMES
                        ? inNumberFrames : SCHWUNG_AUDIO_FRAMES;
        for (UInt32 i = 0; i < frames; i++) {
            L[i] = src[2 * i + 0] * (1.0f / 32768.0f);
            R[i] = src[2 * i + 1] * (1.0f / 32768.0f);
        }
        // Zero any tail beyond what we have (in case CoreAudio asks for more
        // than 128 frames — shouldn't on a typical setup per preflight).
        for (UInt32 i = frames; i < inNumberFrames; i++) {
            L[i] = 0.0f;
            R[i] = 0.0f;
        }
    } else if (ioData) {
        for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
            memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }
    }

    // Drain any browser-injected MIDI into the mailbox MIDI_IN region. Must
    // happen BEFORE pulsing the tick pipe so the host sees fresh events on
    // this frame, not the next one. Cheap mutex lock — bounded operations.
    if (g.mailbox) {
        schwung_sim_drain_midi_in_to_mailbox(g.mailbox);
    }

    // Pulse the sim tick pipe — releases the host's blocked schwung_sim_ioctl_wait.
    // The host runs exactly one frame in response, writing fresh audio into the
    // mailbox for the NEXT callback to pick up.
    if (g.tick_fd >= 0) {
        char b = 1;
        ssize_t n;
        do { n = write(g.tick_fd, &b, 1); } while (n < 0 && errno == EINTR);
    }

    // Diagnostic: log every ~5 seconds so we know the clock is running.
    // 5 seconds @ 344Hz ≈ 1720 callbacks. NOT realtime-safe but writes to a
    // FILE* which is buffered; close enough for a stub. Phase 3d/e can prune.
    g.callback_count++;
    if (g.callback_count % 1720 == 1) {
        fprintf(stderr, "audio: callback %llu (frames=%u)\n",
                (unsigned long long)g.callback_count, (unsigned)inNumberFrames);
    }

    return noErr;
}

// ============================================================================
// Setup helpers
// ============================================================================

static int set_stream_format(AudioUnit unit, AudioUnitScope scope,
                             AudioUnitElement element)
{
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = 44100.0;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat
                          | kAudioFormatFlagsNativeEndian
                          | kAudioFormatFlagIsPacked
                          | kAudioFormatFlagIsNonInterleaved;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerPacket   = 4;  // 1 frame × 4 bytes per non-interleaved channel
    fmt.mBytesPerFrame    = 4;

    OSStatus s = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                      scope, element, &fmt, sizeof(fmt));
    if (s != noErr) {
        fprintf(stderr, "audio: set StreamFormat (scope=%u) failed: %d\n",
                (unsigned)scope, (int)s);
        return -1;
    }
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

int schwung_audio_start(uint8_t *mailbox, int tick_fd) {
    if (g.started) return 0;
    if (!mailbox || tick_fd < 0) {
        fprintf(stderr, "audio: bad args (mailbox=%p, tick_fd=%d)\n",
                (void *)mailbox, tick_fd);
        return -1;
    }

    g.mailbox = mailbox;
    g.tick_fd = tick_fd;
    g.callback_count = 0;

    // Find the HALOutput audio component.
    AudioComponentDescription desc = {
        .componentType         = kAudioUnitType_Output,
        .componentSubType      = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "audio: HALOutput component not found\n");
        return -1;
    }

    OSStatus s = AudioComponentInstanceNew(comp, &g.unit);
    if (s != noErr) {
        fprintf(stderr, "audio: InstanceNew failed: %d\n", (int)s);
        return -1;
    }

    // For Phase 3a we only need output enabled — input wiring comes in 3c.
    // Bus 0 (output side) is enabled by default on HALOutput. Skip touching
    // bus 1 to keep things simple.

    // Bind to the default output device.
    AudioObjectID dev = 0;
    UInt32 size = sizeof(dev);
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &dev);
    AudioUnitSetProperty(g.unit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &dev, sizeof(dev));

    // Verify the device sample rate. v1 requires 44.1k — if the device is at
    // 48k/96k we'd resample (out of scope), so just fail loudly.
    Float64 rate = 0; size = sizeof(rate);
    addr.mSelector = kAudioDevicePropertyNominalSampleRate;
    AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &rate);
    if (rate != 44100.0) {
        fprintf(stderr,
                "audio: default output device runs at %.0f Hz; sim requires 44100 Hz.\n"
                "       Set it in Audio MIDI Setup (Applications > Utilities) and retry.\n",
                rate);
        AudioComponentInstanceDispose(g.unit);
        return -1;
    }

    // Request 128 frames per buffer. Preflight (Task 0.2) confirmed CoreAudio
    // honors this on a typical Mac, so we don't bother with N×128 fan-out yet.
    UInt32 frames = 128;
    AudioUnitSetProperty(g.unit, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Global, 0, &frames, sizeof(frames));

    // Stream format on output side (bus 0).
    if (set_stream_format(g.unit, kAudioUnitScope_Input, 0) < 0) {
        AudioComponentInstanceDispose(g.unit);
        return -1;
    }

    // Render callback.
    AURenderCallbackStruct cb = { .inputProc = render_cb, .inputProcRefCon = NULL };
    s = AudioUnitSetProperty(g.unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Global, 0, &cb, sizeof(cb));
    if (s != noErr) {
        fprintf(stderr, "audio: SetRenderCallback failed: %d\n", (int)s);
        AudioComponentInstanceDispose(g.unit);
        return -1;
    }

    s = AudioUnitInitialize(g.unit);
    if (s != noErr) {
        fprintf(stderr, "audio: AudioUnitInitialize failed: %d\n", (int)s);
        AudioComponentInstanceDispose(g.unit);
        return -1;
    }

    // Confirm the actual buffer size we got.
    UInt32 actual = 0; size = sizeof(actual);
    AudioUnitGetProperty(g.unit, kAudioDevicePropertyBufferFrameSize,
                        kAudioUnitScope_Global, 0, &actual, &size);
    if (actual != 128) {
        fprintf(stderr, "audio: warning — requested 128 frames, got %u\n",
                (unsigned)actual);
    }

    s = AudioOutputUnitStart(g.unit);
    if (s != noErr) {
        fprintf(stderr, "audio: AudioOutputUnitStart failed: %d\n", (int)s);
        AudioUnitUninitialize(g.unit);
        AudioComponentInstanceDispose(g.unit);
        return -1;
    }

    g.started = 1;
    fprintf(stderr, "audio: started — CoreAudio is now the clock master "
                    "(44100Hz, %u frames/buffer)\n", (unsigned)actual);
    return 0;
}

void schwung_audio_stop(void) {
    if (!g.started) return;
    AudioOutputUnitStop(g.unit);
    AudioUnitUninitialize(g.unit);
    AudioComponentInstanceDispose(g.unit);
    g.started = 0;
    g.unit = NULL;
}
