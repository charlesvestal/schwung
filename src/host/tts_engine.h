/*
 * TTS Engine - Dual-engine dispatcher (eSpeak-NG + Flite)
 */

#ifndef TTS_ENGINE_H
#define TTS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize TTS engine with target sample rate */
bool tts_init(int sample_rate);

/* Cleanup TTS engine */
void tts_cleanup(void);

/* Speak text (non-blocking, synthesis happens in background) */
bool tts_speak(const char *text);

/* Check if TTS is currently speaking */
bool tts_is_speaking(void);

/* Get synthesized audio samples for mixing
 * Returns number of frames read (stereo pairs)
 * out_buffer: stereo interleaved int16 [L,R,L,R,...]
 * max_frames: maximum number of stereo frames to read
 */
int tts_get_audio(int16_t *out_buffer, int max_frames);

/* Set TTS volume (0-100) */
void tts_set_volume(int volume);

/* Set TTS speed (0.5 = half speed, 1.0 = normal, 2.0 = double speed) */
void tts_set_speed(float speed);

/* Set TTS pitch in Hz (range: 80-180, typical: 110) */
void tts_set_pitch(float pitch_hz);

/* Enable or disable TTS */
void tts_set_enabled(bool enabled);

/* Get TTS enabled state */
bool tts_get_enabled(void);

/* Get TTS volume */
int tts_get_volume(void);

/* Get TTS speed */
float tts_get_speed(void);

/* Get TTS pitch */
float tts_get_pitch(void);

/* Switch TTS engine: "espeak" or "flite" */
void tts_set_engine(const char *engine_name);

/* Get current TTS engine name */
const char *tts_get_engine(void);

#endif /* TTS_ENGINE_H */
