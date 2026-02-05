# TTS System Architecture: End-to-End Flow

## Overview

The Move Anything TTS system provides accessibility by speaking screen reader announcements. It intercepts screen reader D-Bus signals from the stock Move firmware, synthesizes speech using Flite, and mixes the audio into the output stream.

## System Components

```
Stock Move Firmware (D-Bus signals)
        ↓
LD_PRELOAD Shim (intercepts D-Bus)
        ↓
Debouncer (300ms buffer)
        ↓
Flite TTS Engine (text → audio)
        ↓
Ring Buffer (4 seconds)
        ↓
Audio Mixer (shadow_mix_audio)
        ↓
Hardware Output
```

## Step-by-Step Flow

### 1. Screen Reader Message Generation

**Location:** Stock Move firmware (Ableton)

When the user navigates menus or adjusts parameters, the Move's built-in screen reader announces the action via D-Bus:

```
Example: User turns "Osc 1 Shape" knob
→ D-Bus signal: com.ableton.move.ScreenReader.text
→ Argument: "Osc 1 Shape 0.52"
```

The Move sends these at **~50Hz** during knob adjustments (every 18-20ms).

### 2. D-Bus Signal Interception

**Location:** `src/move_anything_shim.c` (lines ~1163-1210)

The shim uses **LD_PRELOAD** to inject itself into the Move process and registers a D-Bus filter:

```c
/* D-Bus filter function - intercepts ALL D-Bus messages */
static DBusHandlerResult shadow_dbus_filter(DBusConnection *conn,
                                           DBusMessage *msg,
                                           void *data)
{
    // Check if this is a ScreenReader.text signal
    if (strcmp(iface, "com.ableton.move.ScreenReader") == 0 &&
        strcmp(member, "text") == 0) {

        // Extract the text string
        const char *text = ...;

        // Handle it
        shadow_dbus_handle_text(text);
    }
}
```

**Key point:** The shim sits **inside** the Move process, so it sees all D-Bus traffic in real-time.

### 3. Message Buffering and Debouncing

**Location:** `src/move_anything_shim.c:shadow_dbus_handle_text()` (lines ~1118-1136)

When a D-Bus message arrives, it's written to **shared memory** for the audio thread:

```c
static void shadow_dbus_handle_text(const char *text)
{
    // Write to shared memory (shadow_screenreader_shm)
    strncpy(shadow_screenreader_shm->text, text, 255);
    shadow_screenreader_shm->sequence++;  // Signal new message
}
```

The audio thread checks this shared memory **every audio frame** (~2.9ms):

**Location:** `src/move_anything_shim.c:shadow_check_screenreader()` (lines ~4536-4570)

```c
static void shadow_check_screenreader(void)
{
    // New message arrived?
    if (current_sequence != last_screenreader_sequence) {
        // Buffer it and start debounce timer
        strncpy(pending_tts_message, shadow_screenreader_shm->text, 255);
        last_message_time_ms = now_ms;
        has_pending_message = true;
        return;
    }

    // Has 300ms passed since last message?
    if (has_pending_message &&
        (now_ms - last_message_time_ms >= 300)) {
        // Speak the buffered message
        tts_speak(pending_tts_message);
        has_pending_message = false;
    }
}
```

**Debouncing logic:**
- Rapid messages (knob turning) → buffer latest, reset timer
- 300ms quiet → speak final value
- Result: "Osc 1 Shape 0.52, 0.53, 0.54" → speaks only "0.54"

### 4. TTS Synthesis (Flite)

**Location:** `src/host/tts_engine_flite.c:tts_speak()` (lines ~67-148)

When `tts_speak("Osc 1 Shape 0.70")` is called:

```c
bool tts_speak(const char *text)
{
    // Lazy init on first use
    if (!initialized) {
        flite_init();
        voice = register_cmu_us_kal(NULL);  // US English voice
    }

    // Synthesize text to waveform (BLOCKING, ~100-200ms)
    cst_wave *wav = flite_text_to_wave(text, voice);

    // Flite outputs: 8kHz, mono, int16 samples
    // wav->num_samples = ~21000 for 2.6 second phrase
    // wav->sample_rate = 8000
```

**Synthesis performance:**
- Text processing: ~50ms
- Audio generation: ~150ms
- Total: ~200ms blocking time

### 5. Sample Rate Conversion and Buffering

**Location:** `src/host/tts_engine_flite.c:tts_speak()` (lines ~100-140)

Flite outputs **8kHz mono**, but Move needs **44.1kHz stereo**:

```c
    // Clear ring buffer
    pthread_mutex_lock(&ring_mutex);
    ring_write_pos = 0;
    ring_read_pos = 0;

    // Upsample ratio: 44100 / 8000 = 5.5125x
    float upsample_ratio = 44100.0f / wav->sample_rate;

    // Linear interpolation upsampling
    for (int i = 0; i < flite_samples - 1; i++) {
        int16_t curr = wav->samples[i];
        int16_t next = wav->samples[i + 1];

        int repeats = 6;  // Round 5.5125 up
        for (int r = 0; r < repeats; r++) {
            // Interpolate between curr and next
            float alpha = r / 6.0f;
            int16_t sample = curr * (1-alpha) + next * alpha;

            // Write stereo (duplicate mono)
            ring_buffer[write_pos++] = sample;  // Left
            ring_buffer[write_pos++] = sample;  // Right
        }
    }

    pthread_mutex_unlock(&ring_mutex);
```

**Result:**
- Input: 21,000 samples @ 8kHz (2.6 seconds)
- Output: ~230,000 samples @ 44.1kHz stereo
- Written to `ring_buffer[353KB]` (4 seconds capacity)

### 6. Audio Playback Retrieval

**Location:** `src/host/tts_engine_flite.c:tts_get_audio()` (lines ~152-186)

The audio mixing thread calls this **every audio frame** (128 frames @ 44.1kHz = 2.9ms):

```c
int tts_get_audio(int16_t *out_buffer, int max_frames)
{
    pthread_mutex_lock(&ring_mutex);

    // Calculate available frames
    int frames_available = (write_pos - read_pos) / 2;
    int frames_to_read = min(frames_available, max_frames);

    // Read and apply volume
    for (int i = 0; i < frames_to_read * 2; i++) {
        int32_t sample = ring_buffer[read_pos];
        sample *= (tts_volume / 100.0f);  // Volume scaling

        // Clamp to int16
        out_buffer[i] = clamp(sample, -32768, 32767);
        read_pos++;
    }

    pthread_mutex_unlock(&ring_mutex);
    return frames_to_read;  // Typically 128
}
```

**Performance:** ~0.5% CPU (simple memory read + multiply)

### 7. Audio Mixing

**Location:** `src/move_anything_shim.c:shadow_mix_audio()` (lines ~4616-4630)

This runs in the **Move's audio callback** (called by hardware every ~2.9ms):

```c
static void shadow_mix_audio(void)
{
    // Get Move's current audio frame (128 stereo samples)
    int16_t *mailbox_audio = global_mmap_addr + AUDIO_OUT_OFFSET;

    // Check for pending TTS
    if (tts_is_speaking()) {
        static int16_t tts_buffer[128 * 2];  // 128 frames stereo

        // Get TTS audio
        int frames_read = tts_get_audio(tts_buffer, 128);

        // Mix with Move's audio (simple addition)
        for (int i = 0; i < frames_read * 2; i++) {
            int32_t mixed = mailbox_audio[i] + tts_buffer[i];

            // Clamp to prevent clipping
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;

            mailbox_audio[i] = (int16_t)mixed;
        }
    }
}
```

**Result:** TTS audio is **added** to Move's audio output (both play simultaneously)

### 8. Hardware Output

**Location:** Move's audio hardware (via ioctl)

The mixed audio buffer flows to hardware:

```
mailbox_audio[512 bytes]
    ↓
Move's SPI audio controller (/dev/ablspi0.0)
    ↓
DAC (digital-to-analog converter)
    ↓
Headphones/Speakers
```

The shim doesn't intercept this - it just modifies the buffer before the Move's normal audio output path.

## Memory Layout

**Shared Memory:**
```
/move-shadow-screenreader (256 bytes)
├── text[252]         // Screen reader message
└── sequence          // Incremented on new message
```

**Ring Buffer:**
```
ring_buffer[352,800 samples = 706KB]
├── write_pos         // Where synthesis writes
└── read_pos          // Where mixer reads
```

**Threading:**
- **D-Bus thread:** Receives messages, writes to shared memory
- **Audio thread:** Runs at 44.1kHz, reads shared memory, calls TTS, mixes audio
- **Mutex:** Protects ring buffer during read/write

## Performance Characteristics

**CPU Usage:**
- Idle: 0% (lazy init, no overhead)
- Synthesis: 8% for 200ms (text → audio)
- Playback: 0.5% continuous (mixing)

**Memory:**
- Ring buffer: 706KB allocated
- Flite libraries: 410KB (shared)
- Voice data: In library
- Total: ~900KB when active

**Latency:**
- D-Bus → Debounce: 300ms (intentional)
- Synthesis: 200ms
- First audio: ~500ms after user stops adjusting
- Playback: Real-time (no additional latency)

## Error Handling

**Buffer overflow:**
```c
if (total_output_samples > RING_BUFFER_SIZE) {
    unified_log("tts_engine", LOG_LEVEL_ERROR,
                "TTS audio too long (%d samples, buffer=%d)",
                total_output_samples, RING_BUFFER_SIZE);
    return false;  // Drop message
}
```

**Synthesis failure:**
```c
cst_wave *wav = flite_text_to_wave(text, voice);
if (!wav) {
    unified_log("tts_engine", LOG_LEVEL_ERROR, "Flite synthesis failed");
    return false;
}
```

## Key Design Decisions

1. **Debouncing over rate limiting:** Speaks final value instead of first
2. **Lazy initialization:** TTS loads only when needed (prevents boot crash)
3. **Ring buffer over queue:** Simple, fixed memory, no dynamic allocation
4. **Linear interpolation:** Better quality than sample repetition
5. **Synchronous synthesis:** Simple, but blocks briefly (acceptable for <1s phrases)
6. **Additive mixing:** TTS + Move audio both audible (not ducking)

## Future Improvements

1. **Background synthesis thread:** Prevent audio thread blocking
2. **Smarter debouncing:** Detect value vs. navigation (different timings)
3. **Audio ducking:** Lower Move volume when TTS speaks
4. **Voice customization:** Speed, pitch, different voices
5. **SSML support:** Pronunciation hints, pauses

This architecture provides robust, accessible TTS with minimal overhead and complexity.
