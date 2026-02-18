/*
 * Stub for libpcaudio symbols referenced by libespeak-ng.
 *
 * eSpeak-NG links against libpcaudio for audio playback, but we use
 * AUDIO_OUTPUT_RETRIEVAL mode (callback-based), so audio playback functions
 * are never called. This stub satisfies the dynamic linker without pulling
 * in libpcaudio → libpulse → libX11 dependency chain.
 */

#include <stddef.h>

void *create_audio_device_object(const char *device, const char *app, const char *desc) {
    (void)device; (void)app; (void)desc;
    return NULL;
}

void audio_object_destroy(void *obj) { (void)obj; }
int audio_object_open(void *obj, int format, int rate, int channels) {
    (void)obj; (void)format; (void)rate; (void)channels;
    return -1;
}
void audio_object_close(void *obj) { (void)obj; }
int audio_object_write(void *obj, const char *data, int len) {
    (void)obj; (void)data; (void)len;
    return -1;
}
int audio_object_drain(void *obj) { (void)obj; return -1; }
int audio_object_flush(void *obj) { (void)obj; return -1; }
const char *audio_object_strerror(void *obj, int err) {
    (void)obj; (void)err;
    return "pcaudio stub";
}
