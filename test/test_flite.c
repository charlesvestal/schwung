/* Simple Flite test */
#include <stdio.h>
#include <flite/flite.h>

extern cst_voice *register_cmu_us_kal(const char *voxdir);

int main(int argc, char **argv) {
    printf("Initializing Flite...\n");
    fflush(stdout);

    flite_init();
    cst_voice *voice = register_cmu_us_kal(NULL);

    if (!voice) {
        printf("ERROR: Failed to register voice\n");
        return 1;
    }

    printf("SUCCESS: Flite initialized\n");

    printf("Synthesizing test phrase...\n");
    fflush(stdout);

    cst_wave *wav = flite_text_to_wave("Text to speech initialized", voice);

    if (!wav) {
        printf("ERROR: Synthesis failed\n");
        return 1;
    }

    printf("SUCCESS: Synthesized %d samples at %d Hz\n", wav->num_samples, wav->sample_rate);
    delete_wave(wav);

    return 0;
}
