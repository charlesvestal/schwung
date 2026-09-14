/*
 * schwung_desktop.h — the Schwung chain host, driven from a desktop audio
 * callback instead of from the SPI transfer.
 *
 * This is the layer that src/schwung_shim.c occupies on the device, and it is
 * the only layer the Live-plugin port has to write: everything above it (the
 * chain, the slots, the LFOs, the buses) is the device's own code, dlopen'd
 * from chain/dsp.so exactly as the shim dlopens it.
 *
 * See docs/plans/2026-09-13-schwung-vst-port.md.
 *
 * THE BLOCK SIZE IS NOT NEGOTIABLE HERE. The chain and every module below it
 * assume 44100 Hz and a 128-frame block — sixteen hardcodes in the chain host
 * alone, and every module's filter coefficients besides. This layer therefore
 * always calls render_block() with exactly SCHWUNG_BLOCK frames at
 * SCHWUNG_RATE, and the caller is responsible for converting its own rate and
 * buffer size to that (see rate_bridge.h). Making the fleet rate-aware is not
 * a project that finishes.
 */
#ifndef SCHWUNG_DESKTOP_H
#define SCHWUNG_DESKTOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHWUNG_RATE  44100
#define SCHWUNG_BLOCK 128

typedef struct schwung_desktop schwung_desktop_t;

/*
 * Bring up one chain.
 *
 * module_root is the directory holding the device's module tree — the one
 * whose children are chain/, sound_generators/ and audio_fx/. The chain host
 * builds every sub-module path relative to its own module_dir ("%s/../
 * sound_generators/%s/dsp.so"), so the layout has to mirror the device's.
 *
 * NOTE ON THE EXTENSION: those paths end in ".so" in the chain host's own
 * source, and this port does not change that. dlopen on macOS does not care
 * what a Mach-O file is called, so the desktop module tree simply names its
 * dylibs dsp.so too. Adding a platform suffix would mean touching every path
 * construction in chain_host.c and chain_bus.c to buy tidier filenames, and
 * the two platforms never share an install tree.
 *
 * Returns NULL on failure; reason is written to the log callback.
 */
schwung_desktop_t *schwung_desktop_create(const char *module_root,
                                          void (*log)(const char *));
void schwung_desktop_destroy(schwung_desktop_t *sd);

/* Parameter channel — the same string keys the shadow UI and schwung-manager
 * use ("synth:module", "fx1:module", "slot:volume", ...).
 *
 * get_param returns the number of bytes written, or -1. A -1 is NOT "empty":
 * see the three-answer rule in CLAUDE.md. Callers must not turn a failed read
 * into a default. */
void schwung_desktop_set_param(schwung_desktop_t *sd, const char *key, const char *val);
int  schwung_desktop_get_param(schwung_desktop_t *sd, const char *key, char *buf, int buf_len);

/* A 3-byte MIDI message, as it would have arrived from the hardware. */
void schwung_desktop_midi(schwung_desktop_t *sd, const uint8_t *msg, int len);

/* Render exactly SCHWUNG_BLOCK frames of interleaved stereo int16. */
void schwung_desktop_render(schwung_desktop_t *sd, int16_t *out_lr);

/*
 * Publish one block of INPUT audio for the block that is about to render.
 *
 * A module that consumes line input does not receive it as an argument -- it
 * reads it out of the SPI mailbox at host->audio_in_offset, because on the
 * device that is where the codec puts it. So the desktop host has to fill the
 * same region, in the same layout (SCHWUNG_BLOCK interleaved stereo int16),
 * immediately before each render_block.
 *
 * in_lr may be NULL, which writes silence -- the correct answer for a plugin
 * with nothing routed in, and the reason vocoder reads as "loads but is deaf"
 * rather than crashing.
 */
void schwung_desktop_set_audio_in(schwung_desktop_t *sd, const int16_t *in_lr);

/* Transport, read by the chain's LFOs and any sync-aware module through
 * get_bpm()/get_beat_position(). beat < 0 means "no transport running", which
 * is what makes an LFO free-run rather than snap to an invented grid. */
void schwung_desktop_set_transport(schwung_desktop_t *sd, double bpm,
                                   double beat_position, int running);

#ifdef __cplusplus
}
#endif

#endif /* SCHWUNG_DESKTOP_H */
