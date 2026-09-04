// Throwaway probe: push 10 distinct tones into the UAC2 gadget's PCM so each
// channel is identifiable on the host. Channel N carries (N+1)*100 Hz, so
// channel 1 = 100 Hz ... channel 10 = 1000 Hz.
//
// This exists to prove the isochronous IN stream actually carries 10 channels
// of real audio under dwc2 before any SHM plumbing is built. Not shipped.

#include <alsa/asoundlib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define CHANNELS 10
#define RATE     44100
#define PERIOD   256

static volatile sig_atomic_t stop_now = 0;
static void on_sig(int s) { (void)s; stop_now = 1; }

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "hw:0,0";
    int seconds = (argc > 2) ? atoi(argv[2]) : 30;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "open %s: %s\n", dev, snd_strerror(err));
        return 1;
    }

    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             CHANNELS, RATE,
                             1,        /* allow resampling */
                             100000);  /* 100 ms latency */
    if (err < 0) {
        fprintf(stderr, "set_params (%d ch @ %d): %s\n",
                CHANNELS, RATE, snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    unsigned int rate = 0, channels = 0;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_current(pcm, hw) == 0) {
        snd_pcm_hw_params_get_rate(hw, &rate, NULL);
        snd_pcm_hw_params_get_channels(hw, &channels);
    }
    printf("negotiated: %u ch @ %u Hz, S16_LE\n", channels, rate);
    printf("channel N carries (N+1)*100 Hz: ch1=100Hz ... ch10=1000Hz\n");
    fflush(stdout);

    static int16_t buf[PERIOD * CHANNELS];
    double phase[CHANNELS] = {0};
    long total = (long)seconds * RATE;
    long done = 0;

    while (!stop_now && done < total) {
        for (int f = 0; f < PERIOD; f++) {
            for (int c = 0; c < CHANNELS; c++) {
                double freq = (c + 1) * 100.0;
                buf[f * CHANNELS + c] = (int16_t)(8000.0 * sin(phase[c]));
                phase[c] += 2.0 * M_PI * freq / RATE;
                if (phase[c] > 2.0 * M_PI) phase[c] -= 2.0 * M_PI;
            }
        }

        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, PERIOD);
        if (w < 0) {
            w = snd_pcm_recover(pcm, (int)w, 0);
            if (w < 0) {
                fprintf(stderr, "write: %s\n", snd_strerror((int)w));
                break;
            }
            continue;
        }
        done += w;

        if ((done % (RATE * 5)) < PERIOD)
            printf("  %lds streamed\n", done / RATE), fflush(stdout);
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    printf("done: %ld frames (%.1f s)\n", done, (double)done / RATE);
    return 0;
}
