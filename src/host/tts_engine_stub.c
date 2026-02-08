/*
 * TTS stub implementation for builds without screen reader dependencies.
 */

#include "tts_engine.h"

bool tts_init(int sample_rate)
{
    (void)sample_rate;
    return true;
}

void tts_cleanup(void)
{
}

bool tts_speak(const char *text)
{
    (void)text;
    return false;
}

bool tts_is_speaking(void)
{
    return false;
}

int tts_get_audio(int16_t *out_buffer, int max_frames)
{
    (void)out_buffer;
    (void)max_frames;
    return 0;
}

void tts_set_volume(int volume)
{
    (void)volume;
}

void tts_set_speed(float speed)
{
    (void)speed;
}

void tts_set_pitch(float pitch_hz)
{
    (void)pitch_hz;
}

void tts_set_enabled(bool enabled)
{
    (void)enabled;
}

bool tts_get_enabled(void)
{
    return false;
}

int tts_get_volume(void)
{
    return 70;
}

float tts_get_speed(void)
{
    return 1.0f;
}

float tts_get_pitch(void)
{
    return 110.0f;
}
