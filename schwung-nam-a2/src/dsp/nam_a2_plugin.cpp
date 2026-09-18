/*
 * Nam A2 Audio FX Plugin - Neural Amp Modeler + solo FX chain for Move Anything
 *
 * Based on the schwung-nam module (https://github.com/charlesvestal/schwung-nam)
 * by Charles Vestal, which wraps NeuralAudio (MIT, by Mike Oliphant) to run
 * .nam / .aidax neural-network guitar-amp models. This module keeps that
 * amp + cab IR core and adds:
 *
 *   - A "Quality" switch (Full / Lite). Lite halves the neural net's work by
 *     running the model at half the block's sample rate (averaging input
 *     pairs, zero-order-hold on the output) - a CPU/quality tradeoff for
 *     models too heavy to run in real time on Move's ARM core alongside the
 *     cab IR, EQ and solo FX chain below.
 *   - A 3-band EQ (low/high shelf + mid bell) sitting after the cab IR.
 *   - A "solo" FX chain - Doubler (chorus) -> Echo (filtered delay, tap
 *     tempo) -> Reverb (small room) - each independently bypassable,
 *     inspired by the layout of the Solar Guitars CHUG SOLO pedal.
 *
 * Cab IR convolution and the reverb's comb/allpass network are adapted from
 * the classic public-domain Freeverb algorithm (Jezar at Dreampoint), as
 * already used by this host's own built-in freeverb.c audio FX.
 *
 * Dependencies (all header-only / static, permissive licenses):
 *   NeuralAudio  - MIT      - Mike Oliphant
 *   Eigen        - MPL2     - Eigen contributors
 *   RTNeural     - BSD-3    - Jatin Chowdhury
 *   math_approx  - BSD-3    - Jatin Chowdhury
 *   nlohmann/json- MIT      - Niels Lohmann
 *
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 in-place.
 * NAM models are mono - we sum L+R to mono, process the amp/cab/EQ stage in
 * mono, then split to stereo for the solo FX chain.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <dirent.h>
#include <algorithm>
#include <string>
#include <atomic>
#include <pthread.h>

/* NeuralAudio */
#include "NeuralAudio/NeuralModel.h"

/* Move Anything API */
extern "C" {
#include "plugin_api_v1.h"
}

/* ======================================================================== */

#define MAX_MODELS 256
#define MAX_CABS 256
#define MAX_NAME_LEN 128
#define MAX_PATH_LEN 512
#define FRAMES_PER_BLOCK 128
#define MAX_IR_LEN 8192

#define SAMPLE_RATE 44100.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Doubler (chorus) */
#define DOUBLER_BUF_LEN 4096
#define DOUBLER_RATE_HZ 0.6f
#define DOUBLER_BASE_MS 15.0f
#define DOUBLER_DEPTH_MS 4.0f

/* Echo */
#define ECHO_MAX_SECONDS 2.0f
#define ECHO_BUF_LEN ((int)(SAMPLE_RATE * ECHO_MAX_SECONDS) + FRAMES_PER_BLOCK)
#define ECHO_TAP_MIN_MS 60.0f
#define ECHO_TAP_MAX_MS 2000.0f

/* Reverb - adapted from the public-domain Freeverb algorithm, same tuning
 * tables as this host's built-in freeverb.c audio FX. */
#define REVERB_NUM_COMBS 8
#define REVERB_NUM_ALLPASSES 4
#define REVERB_MAX_DELAY 2048

static const int reverb_comb_tuning_l[REVERB_NUM_COMBS] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const int reverb_comb_tuning_r[REVERB_NUM_COMBS] = {
    1116 + 23, 1188 + 23, 1277 + 23, 1356 + 23,
    1422 + 23, 1491 + 23, 1557 + 23, 1617 + 23
};
static const int reverb_allpass_tuning_l[REVERB_NUM_ALLPASSES] = { 556, 441, 341, 225 };
static const int reverb_allpass_tuning_r[REVERB_NUM_ALLPASSES] = {
    556 + 23, 441 + 23, 341 + 23, 225 + 23
};

static const host_api_v1_t *g_host = nullptr;

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ======================================================================== */
/* WAV reader - minimal parser for cab IR files                              */
/* ======================================================================== */

/* Read a mono float IR from a WAV file. Supports PCM16/24/32 and float32/64.
 * Returns number of samples read, or 0 on failure. */
static int load_wav_ir(const char *path, float *out, int max_samples) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char riff_id[4];
    uint32_t file_size;
    char wave_id[4];
    if (fread(riff_id, 1, 4, f) != 4 || memcmp(riff_id, "RIFF", 4) != 0) { fclose(f); return 0; }
    if (fread(&file_size, 4, 1, f) != 1) { fclose(f); return 0; }
    if (fread(wave_id, 1, 4, f) != 4 || memcmp(wave_id, "WAVE", 4) != 0) { fclose(f); return 0; }

    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0;
    bool found_fmt = false, found_data = false;

    while (!found_data) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) { fclose(f); return 0; }
            fread(&audio_format, 2, 1, f);
            fread(&num_channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            uint32_t byte_rate; fread(&byte_rate, 4, 1, f);
            uint16_t block_align; fread(&block_align, 2, 1, f);
            fread(&bits_per_sample, 2, 1, f);
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            found_fmt = true;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) { fclose(f); return 0; }
    if (audio_format != 1 && audio_format != 3) { fclose(f); return 0; }

    int bytes_per_sample = bits_per_sample / 8;
    if (bytes_per_sample <= 0 || num_channels == 0) { fclose(f); return 0; }
    int total_samples = data_size / (bytes_per_sample * num_channels);
    if (total_samples > max_samples) total_samples = max_samples;

    int read_count = 0;
    for (int i = 0; i < total_samples; i++) {
        float sample = 0.0f;
        if (audio_format == 1 && bits_per_sample == 16) {
            int16_t s; fread(&s, 2, 1, f);
            sample = s / 32768.0f;
        } else if (audio_format == 1 && bits_per_sample == 24) {
            uint8_t b[3]; fread(b, 1, 3, f);
            int32_t s = (b[0] | (b[1] << 8) | (b[2] << 16));
            if (s & 0x800000) s |= 0xFF000000;
            sample = s / 8388608.0f;
        } else if (audio_format == 1 && bits_per_sample == 32) {
            int32_t s; fread(&s, 4, 1, f);
            sample = s / 2147483648.0f;
        } else if (audio_format == 3 && bits_per_sample == 32) {
            fread(&sample, 4, 1, f);
        } else if (audio_format == 3 && bits_per_sample == 64) {
            double d; fread(&d, 8, 1, f);
            sample = (float)d;
        } else {
            fclose(f); return 0;
        }
        if (num_channels > 1) {
            fseek(f, bytes_per_sample * (num_channels - 1), SEEK_CUR);
        }
        out[read_count++] = sample;
    }

    fclose(f);

    char msg[MAX_PATH_LEN + 128];
    snprintf(msg, sizeof(msg), "Nam A2: loaded cab IR %s (%d samples, %d ch, %d bit, fmt %d)",
             path, read_count, num_channels, bits_per_sample, audio_format);
    plugin_log(msg);

    return read_count;
}

/* ======================================================================== */
/* 3-band EQ - RBJ Audio EQ Cookbook biquads (low/high shelf + mid bell)     */
/* ======================================================================== */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2; /* Direct Form 2 Transposed state */
} biquad_t;

static inline void biquad_reset(biquad_t *bq) {
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

static inline float biquad_process(biquad_t *bq, float x) {
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

static void biquad_set_low_shelf(biquad_t *bq, float freq, float gain_db, float sr) {
    freq = clampf(freq, 10.0f, sr * 0.45f);
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f - 1.0f) + 2.0f); /* S = 1 shelf slope */
    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + two_sqrtA_alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - two_sqrtA_alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + two_sqrtA_alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - two_sqrtA_alpha;

    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
}

static void biquad_set_high_shelf(biquad_t *bq, float freq, float gain_db, float sr) {
    freq = clampf(freq, 10.0f, sr * 0.45f);
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f - 1.0f) + 2.0f);
    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + two_sqrtA_alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - two_sqrtA_alpha);
    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + two_sqrtA_alpha;
    float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - two_sqrtA_alpha;

    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
}

/* Fixed-Q peaking (bell) filter for the mid band. */
#define EQ_MID_Q 0.9f

static void biquad_set_peak(biquad_t *bq, float freq, float gain_db, float q, float sr) {
    freq = clampf(freq, 10.0f, sr * 0.45f);
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cosw0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha / A;

    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
}

/* ======================================================================== */
/* Instance                                                                  */
/* ======================================================================== */

typedef struct {
    char module_dir[MAX_PATH_LEN];

    /* Model */
    NeuralAudio::NeuralModelLoader *loader; /* owns load-mode config; CreateFromFile is
                                              * an instance method as of NeuralAudio 0.1.x -
                                              * heap-allocated (not a struct member) because
                                              * this whole instance is calloc'd, which would
                                              * skip NeuralModelLoader's constructor and its
                                              * non-zero default member initializers. */
    NeuralAudio::NeuralModel *model;
    std::atomic<NeuralAudio::NeuralModel *> pending_model;
    std::atomic<bool> loading;
    char model_path[MAX_PATH_LEN];
    char model_name[MAX_NAME_LEN];

    int model_count;
    char model_names[MAX_MODELS][MAX_NAME_LEN];
    char model_paths[MAX_MODELS][MAX_PATH_LEN];
    int current_model_index;

    /* Cabinet IR */
    float *cab_ir;
    int cab_ir_len;
    float *cab_history;
    int cab_hist_pos;
    bool cab_bypass;
    char cab_name[MAX_NAME_LEN];

    int cab_count;
    char cab_names[MAX_CABS][MAX_NAME_LEN];
    char cab_paths[MAX_CABS][MAX_PATH_LEN];
    int current_cab_index;

    /* Input / output gain */
    float input_level;
    float output_level;
    float input_gain;
    float output_gain;

    /* Quality (0 = Full, 1 = Lite - runs the model at half rate) */
    int quality_lite;

    /* 3-band EQ (mono, post cab IR) */
    float eq_low_gain, eq_low_freq;
    float eq_mid_gain, eq_mid_freq;
    float eq_high_gain, eq_high_freq;
    biquad_t eq_low, eq_mid, eq_high;

    /* Doubler (chorus, mono -> stereo) */
    int doubler_bypass;
    float doubler_mix;
    float doubler_buf[DOUBLER_BUF_LEN];
    int doubler_pos;
    float doubler_phase;

    /* Echo (stereo, filtered feedback, tap tempo) */
    int echo_bypass;
    float echo_time_ms;
    float echo_feedback;
    float echo_filter_hz;
    float echo_mix;
    float echo_filter_coeff; /* one-pole lowpass coefficient, derived from echo_filter_hz */
    float *echo_buf_l;
    float *echo_buf_r;
    int echo_pos;
    float echo_lp_l, echo_lp_r;
    uint64_t sample_clock;
    uint64_t echo_last_tap_sample;
    int echo_has_last_tap;

    /* Reverb (stereo, Freeverb-derived comb/allpass network) */
    int reverb_bypass;
    float reverb_room;
    float reverb_damping;
    float reverb_mix;
    float reverb_feedback, reverb_damp1, reverb_damp2, reverb_wet;
    struct {
        float buffer[REVERB_MAX_DELAY];
        int bufsize;
        int bufidx;
        float filterstore;
    } reverb_comb_l[REVERB_NUM_COMBS], reverb_comb_r[REVERB_NUM_COMBS];
    struct {
        float buffer[REVERB_MAX_DELAY];
        int bufsize;
        int bufidx;
    } reverb_allpass_l[REVERB_NUM_ALLPASSES], reverb_allpass_r[REVERB_NUM_ALLPASSES];

    /* Audio buffers (avoid per-block allocation) */
    float mono_in[FRAMES_PER_BLOCK];
    float mono_out[FRAMES_PER_BLOCK];
    float mono_in_lite[FRAMES_PER_BLOCK / 2];
    float mono_out_lite[FRAMES_PER_BLOCK / 2];

} nam_a2_instance_t;

/* ======================================================================== */
/* Helpers                                                                   */
/* ======================================================================== */

/* Map 0-1 knob to dB range (-24 to +12), then to linear gain */
static float knob_to_gain(float knob) {
    float db = -24.0f + knob * 36.0f;
    return powf(10.0f, db / 20.0f);
}

static void path_to_name(const char *path, char *name, int name_len) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= name_len) len = name_len - 1;
    memcpy(name, base, len);
    name[len] = '\0';
}

static bool is_model_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".nam") == 0 ||
            strcasecmp(dot, ".json") == 0 ||
            strcasecmp(dot, ".aidax") == 0);
}

static bool is_cab_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".wav") == 0 ||
            strcasecmp(dot, ".ir") == 0);
}

static int scan_directory(const char *dir_path,
                          char names[][MAX_NAME_LEN],
                          char paths[][MAX_PATH_LEN],
                          int max_entries,
                          bool (*filter)(const char *)) {
    int count = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && count < max_entries) {
        if (entry->d_name[0] == '.') continue;
        if (!filter(entry->d_name)) continue;

        snprintf(paths[count], MAX_PATH_LEN, "%s/%s", dir_path, entry->d_name);
        path_to_name(entry->d_name, names[count], MAX_NAME_LEN);
        count++;
    }
    closedir(dir);

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(names[i], names[j]) > 0) {
                char tmp_name[MAX_NAME_LEN], tmp_path[MAX_PATH_LEN];
                memcpy(tmp_name, names[i], MAX_NAME_LEN);
                memcpy(tmp_path, paths[i], MAX_PATH_LEN);
                memcpy(names[i], names[j], MAX_NAME_LEN);
                memcpy(paths[i], paths[j], MAX_PATH_LEN);
                memcpy(names[j], tmp_name, MAX_NAME_LEN);
                memcpy(paths[j], tmp_path, MAX_PATH_LEN);
            }
        }
    }

    return count;
}

static void scan_models(nam_a2_instance_t *inst) {
    char models_dir[MAX_PATH_LEN];
    snprintf(models_dir, sizeof(models_dir), "%s/models", inst->module_dir);
    inst->model_count = scan_directory(models_dir, inst->model_names, inst->model_paths,
                                       MAX_MODELS, is_model_file);
}

static void scan_cabs(nam_a2_instance_t *inst) {
    char cabs_dir[MAX_PATH_LEN];
    snprintf(cabs_dir, sizeof(cabs_dir), "%s/cabs", inst->module_dir);
    inst->cab_count = scan_directory(cabs_dir, inst->cab_names, inst->cab_paths,
                                     MAX_CABS, is_cab_file);
}

static void load_cab(nam_a2_instance_t *inst, int index) {
    if (index < 0 || index >= inst->cab_count) return;

    float *new_ir = (float *)calloc(MAX_IR_LEN, sizeof(float));
    if (!new_ir) return;

    int ir_len = load_wav_ir(inst->cab_paths[index], new_ir, MAX_IR_LEN);
    if (ir_len <= 0) {
        free(new_ir);
        char msg[MAX_PATH_LEN + 64];
        snprintf(msg, sizeof(msg), "Nam A2: failed to load cab IR %s", inst->cab_paths[index]);
        plugin_log(msg);
        return;
    }

    float *old_ir = inst->cab_ir;
    float *old_hist = inst->cab_history;

    inst->cab_ir = new_ir;
    inst->cab_ir_len = ir_len;
    inst->current_cab_index = index;
    path_to_name(inst->cab_paths[index], inst->cab_name, MAX_NAME_LEN);

    inst->cab_history = (float *)calloc(ir_len + FRAMES_PER_BLOCK, sizeof(float));
    inst->cab_hist_pos = 0;

    free(old_ir);
    free(old_hist);

    char msg[MAX_PATH_LEN + 64];
    snprintf(msg, sizeof(msg), "Nam A2: loaded cab IR '%s' (%d samples)", inst->cab_name, ir_len);
    plugin_log(msg);
}

/* Direct time-domain overlap-save convolution via a circular history buffer. */
static void apply_cab_ir(nam_a2_instance_t *inst, float *audio, int frames) {
    if (!inst->cab_ir || inst->cab_ir_len <= 0 || !inst->cab_history) return;

    const float *ir = inst->cab_ir;
    const int ir_len = inst->cab_ir_len;
    float *hist = inst->cab_history;
    const int hist_len = ir_len + FRAMES_PER_BLOCK;
    int pos = inst->cab_hist_pos;

    for (int i = 0; i < frames; i++) {
        hist[pos] = audio[i];

        float sum = 0.0f;
        int p = pos;
        for (int k = 0; k < ir_len; k++) {
            sum += ir[k] * hist[p];
            if (--p < 0) p = hist_len - 1;
        }

        audio[i] = sum;
        if (++pos >= hist_len) pos = 0;
    }

    inst->cab_hist_pos = pos;
}

static void *model_loader_thread(void *arg) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)arg;

    char msg[MAX_PATH_LEN + 64];
    snprintf(msg, sizeof(msg), "Nam A2: loading model %s", inst->model_path);
    plugin_log(msg);

    NeuralAudio::NeuralModel *new_model = inst->loader->CreateFromFile(inst->model_path);

    if (new_model) {
        snprintf(msg, sizeof(msg), "Nam A2: model loaded successfully (sample_rate=%.0f)",
                 new_model->GetSampleRate());
        plugin_log(msg);
    } else {
        snprintf(msg, sizeof(msg), "Nam A2: failed to load model %s", inst->model_path);
        plugin_log(msg);
    }

    inst->pending_model.store(new_model, std::memory_order_release);
    inst->loading.store(false, std::memory_order_release);

    return nullptr;
}

static void load_model_async(nam_a2_instance_t *inst, const char *path) {
    if (inst->loading.load(std::memory_order_acquire)) {
        plugin_log("Nam A2: already loading a model, skipping");
        return;
    }

    strncpy(inst->model_path, path, MAX_PATH_LEN - 1);
    inst->model_path[MAX_PATH_LEN - 1] = '\0';
    path_to_name(path, inst->model_name, MAX_NAME_LEN);

    inst->loading.store(true, std::memory_order_release);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, model_loader_thread, inst);
    pthread_attr_destroy(&attr);
}

/* --- EQ --- */

static void update_eq(nam_a2_instance_t *inst) {
    biquad_set_low_shelf(&inst->eq_low, inst->eq_low_freq, inst->eq_low_gain, SAMPLE_RATE);
    biquad_set_peak(&inst->eq_mid, inst->eq_mid_freq, inst->eq_mid_gain, EQ_MID_Q, SAMPLE_RATE);
    biquad_set_high_shelf(&inst->eq_high, inst->eq_high_freq, inst->eq_high_gain, SAMPLE_RATE);
}

static inline float eq_process(nam_a2_instance_t *inst, float x) {
    float y = biquad_process(&inst->eq_low, x);
    y = biquad_process(&inst->eq_mid, y);
    y = biquad_process(&inst->eq_high, y);
    return y;
}

/* --- Doubler (chorus) --- */

static inline float doubler_read_interp(nam_a2_instance_t *inst, float delay_samples) {
    float read_pos = (float)inst->doubler_pos - delay_samples;
    while (read_pos < 0.0f) read_pos += DOUBLER_BUF_LEN;
    int i0 = (int)read_pos;
    float frac = read_pos - (float)i0;
    int i1 = i0 + 1;
    if (i0 >= DOUBLER_BUF_LEN) i0 -= DOUBLER_BUF_LEN;
    if (i1 >= DOUBLER_BUF_LEN) i1 -= DOUBLER_BUF_LEN;
    return inst->doubler_buf[i0] * (1.0f - frac) + inst->doubler_buf[i1] * frac;
}

static inline void doubler_process(nam_a2_instance_t *inst, float in, float *out_l, float *out_r) {
    inst->doubler_buf[inst->doubler_pos] = in;

    float lfo_l = sinf(inst->doubler_phase);
    float lfo_r = sinf(inst->doubler_phase + (float)M_PI * 0.5f);
    float delay_l = (DOUBLER_BASE_MS + DOUBLER_DEPTH_MS * lfo_l) * 0.001f * SAMPLE_RATE;
    float delay_r = (DOUBLER_BASE_MS + DOUBLER_DEPTH_MS * lfo_r) * 0.001f * SAMPLE_RATE;

    float wet_l = doubler_read_interp(inst, delay_l);
    float wet_r = doubler_read_interp(inst, delay_r);

    if (++inst->doubler_pos >= DOUBLER_BUF_LEN) inst->doubler_pos = 0;
    inst->doubler_phase += 2.0f * (float)M_PI * DOUBLER_RATE_HZ / SAMPLE_RATE;
    if (inst->doubler_phase > 2.0f * (float)M_PI) inst->doubler_phase -= 2.0f * (float)M_PI;

    float mix = inst->doubler_mix;
    *out_l = in * (1.0f - mix) + wet_l * mix;
    *out_r = in * (1.0f - mix) + wet_r * mix;
}

/* --- Echo --- */

static void update_echo_filter(nam_a2_instance_t *inst) {
    float hz = clampf(inst->echo_filter_hz, 20.0f, SAMPLE_RATE * 0.45f);
    /* One-pole lowpass coefficient, alpha = 1 - exp(-2*pi*fc/fs). */
    inst->echo_filter_coeff = 1.0f - expf(-2.0f * (float)M_PI * hz / SAMPLE_RATE);
}

static inline int echo_delay_samples(nam_a2_instance_t *inst) {
    float ms = clampf(inst->echo_time_ms, 1.0f, ECHO_MAX_SECONDS * 1000.0f);
    int samples = (int)(ms * 0.001f * SAMPLE_RATE);
    if (samples < 1) samples = 1;
    if (samples > ECHO_BUF_LEN - FRAMES_PER_BLOCK - 1) samples = ECHO_BUF_LEN - FRAMES_PER_BLOCK - 1;
    return samples;
}

static inline float echo_process_channel(float in, float *buf, int pos, int delay,
                                         float feedback, float filter_coeff, float *lp_state) {
    int read_idx = pos - delay;
    if (read_idx < 0) read_idx += ECHO_BUF_LEN;

    float delayed = buf[read_idx];
    *lp_state += filter_coeff * (delayed - *lp_state);

    buf[pos] = in + (*lp_state) * feedback;
    return delayed;
}

static inline void echo_process(nam_a2_instance_t *inst, float in_l, float in_r,
                                float *out_l, float *out_r) {
    int delay = echo_delay_samples(inst);

    float delayed_l = echo_process_channel(in_l, inst->echo_buf_l, inst->echo_pos, delay,
                                           inst->echo_feedback, inst->echo_filter_coeff,
                                           &inst->echo_lp_l);
    float delayed_r = echo_process_channel(in_r, inst->echo_buf_r, inst->echo_pos, delay,
                                           inst->echo_feedback, inst->echo_filter_coeff,
                                           &inst->echo_lp_r);

    if (++inst->echo_pos >= ECHO_BUF_LEN) inst->echo_pos = 0;

    float mix = inst->echo_mix;
    *out_l = in_l * (1.0f - mix) + delayed_l * mix;
    *out_r = in_r * (1.0f - mix) + delayed_r * mix;
}

/* Registers one tap; two or more taps sets echo_time_ms from the interval. */
static void echo_register_tap(nam_a2_instance_t *inst) {
    uint64_t now = inst->sample_clock;

    if (inst->echo_has_last_tap) {
        uint64_t delta_samples = now - inst->echo_last_tap_sample;
        float delta_ms = (float)delta_samples / SAMPLE_RATE * 1000.0f;
        if (delta_ms >= ECHO_TAP_MIN_MS && delta_ms <= ECHO_TAP_MAX_MS) {
            inst->echo_time_ms = delta_ms;
            char msg[96];
            snprintf(msg, sizeof(msg), "Nam A2: tap tempo set echo time to %.0f ms", delta_ms);
            plugin_log(msg);
        }
    }

    inst->echo_last_tap_sample = now;
    inst->echo_has_last_tap = 1;
}

/* --- Reverb (Freeverb-derived) --- */

static void reverb_comb_init(void *comb_v, int size) {
    struct comb_s { float buffer[REVERB_MAX_DELAY]; int bufsize; int bufidx; float filterstore; };
    comb_s *c = (comb_s *)comb_v;
    memset(c->buffer, 0, sizeof(c->buffer));
    c->bufsize = (size < REVERB_MAX_DELAY) ? size : REVERB_MAX_DELAY;
    c->bufidx = 0;
    c->filterstore = 0.0f;
}

static void reverb_allpass_init(void *ap_v, int size) {
    struct ap_s { float buffer[REVERB_MAX_DELAY]; int bufsize; int bufidx; };
    ap_s *a = (ap_s *)ap_v;
    memset(a->buffer, 0, sizeof(a->buffer));
    a->bufsize = (size < REVERB_MAX_DELAY) ? size : REVERB_MAX_DELAY;
    a->bufidx = 0;
}

template <typename Comb>
static inline float reverb_comb_process(Comb *c, float input, float feedback, float damp1, float damp2) {
    float output = c->buffer[c->bufidx];
    c->filterstore = (output * damp2) + (c->filterstore * damp1);
    c->buffer[c->bufidx] = input + (c->filterstore * feedback);
    if (++c->bufidx >= c->bufsize) c->bufidx = 0;
    return output;
}

template <typename Allpass>
static inline float reverb_allpass_process(Allpass *a, float input) {
    float bufout = a->buffer[a->bufidx];
    float output = -input + bufout;
    a->buffer[a->bufidx] = input + (bufout * 0.5f);
    if (++a->bufidx >= a->bufsize) a->bufidx = 0;
    return output;
}

static void update_reverb(nam_a2_instance_t *inst) {
    inst->reverb_feedback = inst->reverb_room * 0.28f + 0.7f;
    inst->reverb_damp1 = inst->reverb_damping * 0.4f;
    inst->reverb_damp2 = 1.0f - inst->reverb_damp1;
    inst->reverb_wet = inst->reverb_mix;
}

static inline void reverb_process(nam_a2_instance_t *inst, float in_l, float in_r,
                                  float *out_l, float *out_r) {
    float wet_l = 0.0f, wet_r = 0.0f;

    for (int c = 0; c < REVERB_NUM_COMBS; c++) {
        wet_l += reverb_comb_process(&inst->reverb_comb_l[c], in_l, inst->reverb_feedback,
                                     inst->reverb_damp1, inst->reverb_damp2);
        wet_r += reverb_comb_process(&inst->reverb_comb_r[c], in_r, inst->reverb_feedback,
                                     inst->reverb_damp1, inst->reverb_damp2);
    }
    wet_l *= 0.125f;
    wet_r *= 0.125f;

    for (int a = 0; a < REVERB_NUM_ALLPASSES; a++) {
        wet_l = reverb_allpass_process(&inst->reverb_allpass_l[a], wet_l);
        wet_r = reverb_allpass_process(&inst->reverb_allpass_r[a], wet_r);
    }

    float dry = 1.0f - inst->reverb_wet;
    *out_l = in_l * dry + wet_l * inst->reverb_wet;
    *out_r = in_r * dry + wet_r * inst->reverb_wet;
}

/* ======================================================================== */
/* audio_fx_api_v2 implementation                                            */
/* ======================================================================== */

#define AUDIO_FX_API_VERSION_2 2
#define AUDIO_FX_INIT_V2_SYMBOL "move_audio_fx_init_v2"

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

typedef audio_fx_api_v2_t* (*audio_fx_init_v2_fn)(const host_api_v1_t *host);

/* --- create_instance --- */
static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;
    plugin_log("Nam A2: creating instance");

    nam_a2_instance_t *inst = (nam_a2_instance_t *)calloc(1, sizeof(nam_a2_instance_t));
    if (!inst) return nullptr;

    inst->loader = new NeuralAudio::NeuralModelLoader();
    inst->loader->SetDefaultMaxAudioBufferSize(FRAMES_PER_BLOCK);

    strncpy(inst->module_dir, module_dir, MAX_PATH_LEN - 1);
    inst->model = nullptr;
    inst->pending_model.store(nullptr);
    inst->loading.store(false);
    inst->current_model_index = -1;

    /* Cabinet IR defaults */
    inst->cab_ir = nullptr;
    inst->cab_ir_len = 0;
    inst->cab_history = nullptr;
    inst->cab_hist_pos = 0;
    inst->cab_bypass = false;
    inst->cab_name[0] = '\0';
    inst->current_cab_index = -1;

    /* Input/output/quality defaults */
    inst->input_level = 0.5f;
    inst->output_level = 0.5f;
    inst->input_gain = knob_to_gain(0.5f);
    inst->output_gain = knob_to_gain(0.5f);
    inst->quality_lite = 0;

    /* EQ defaults (flat) */
    inst->eq_low_gain = 0.0f;  inst->eq_low_freq = 100.0f;
    inst->eq_mid_gain = 0.0f;  inst->eq_mid_freq = 800.0f;
    inst->eq_high_gain = 0.0f; inst->eq_high_freq = 3000.0f;
    update_eq(inst);

    /* Doubler defaults - off until the user opts in */
    inst->doubler_bypass = 1;
    inst->doubler_mix = 0.35f;
    inst->doubler_pos = 0;
    inst->doubler_phase = 0.0f;

    /* Echo defaults */
    inst->echo_bypass = 1;
    inst->echo_time_ms = 350.0f;
    inst->echo_feedback = 0.35f;
    inst->echo_filter_hz = 3000.0f;
    inst->echo_mix = 0.3f;
    update_echo_filter(inst);
    inst->echo_buf_l = (float *)calloc(ECHO_BUF_LEN, sizeof(float));
    inst->echo_buf_r = (float *)calloc(ECHO_BUF_LEN, sizeof(float));
    inst->echo_pos = 0;
    inst->echo_lp_l = 0.0f;
    inst->echo_lp_r = 0.0f;
    inst->sample_clock = 0;
    inst->echo_has_last_tap = 0;

    /* Reverb defaults - off until the user opts in */
    inst->reverb_bypass = 1;
    inst->reverb_room = 0.5f;
    inst->reverb_damping = 0.5f;
    inst->reverb_mix = 0.3f;
    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        reverb_comb_init(&inst->reverb_comb_l[i], reverb_comb_tuning_l[i]);
        reverb_comb_init(&inst->reverb_comb_r[i], reverb_comb_tuning_r[i]);
    }
    for (int i = 0; i < REVERB_NUM_ALLPASSES; i++) {
        reverb_allpass_init(&inst->reverb_allpass_l[i], reverb_allpass_tuning_l[i]);
        reverb_allpass_init(&inst->reverb_allpass_r[i], reverb_allpass_tuning_r[i]);
    }
    update_reverb(inst);

    if (!inst->echo_buf_l || !inst->echo_buf_r) {
        plugin_log("Nam A2: failed to allocate echo buffers");
    }

    /* Scan for model/cab files and load the first of each */
    scan_models(inst);
    scan_cabs(inst);

    if (inst->model_count > 0) {
        inst->current_model_index = 0;
        load_model_async(inst, inst->model_paths[0]);
    }
    if (inst->cab_count > 0) {
        load_cab(inst, 0);
    }

    return inst;
}

/* --- destroy_instance --- */
static void v2_destroy_instance(void *instance) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst) return;

    while (inst->loading.load(std::memory_order_acquire)) {
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, nullptr);
    }

    NeuralAudio::NeuralModel *pending = inst->pending_model.load(std::memory_order_acquire);
    if (pending) delete pending;
    if (inst->model) delete inst->model;
    delete inst->loader;

    free(inst->cab_ir);
    free(inst->cab_history);
    free(inst->echo_buf_l);
    free(inst->echo_buf_r);

    free(inst);
    plugin_log("Nam A2: instance destroyed");
}

/* --- process_block --- */
static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst) return;

    /* Check for newly loaded model (lock-free swap) */
    NeuralAudio::NeuralModel *pending = inst->pending_model.load(std::memory_order_acquire);
    if (pending) {
        NeuralAudio::NeuralModel *old = inst->model;
        inst->model = pending;
        inst->pending_model.store(nullptr, std::memory_order_release);
        if (old) delete old;
    }

    /* No model loaded - pass through untouched (matches schwung-nam) */
    if (!inst->model) return;

    int n = (frames > FRAMES_PER_BLOCK) ? FRAMES_PER_BLOCK : frames;
    inst->sample_clock += (uint64_t)n;

    /* Deinterleave stereo int16 -> mono float, with input gain */
    float ig = inst->input_gain;
    for (int i = 0; i < n; i++) {
        float l = audio_inout[i * 2]     / 32768.0f;
        float r = audio_inout[i * 2 + 1] / 32768.0f;
        inst->mono_in[i] = (l + r) * 0.5f * ig;
    }

    /* NAM model - Full runs every sample; Lite halves the neural net's work
     * by averaging input pairs, running the model at half rate, and holding
     * each output sample for two frames. */
    if (inst->quality_lite && n >= 2) {
        int half = n / 2;
        for (int i = 0; i < half; i++) {
            inst->mono_in_lite[i] = 0.5f * (inst->mono_in[2 * i] + inst->mono_in[2 * i + 1]);
        }
        inst->model->Process(inst->mono_in_lite, inst->mono_out_lite, (size_t)half);
        for (int i = 0; i < half; i++) {
            inst->mono_out[2 * i] = inst->mono_out_lite[i];
            inst->mono_out[2 * i + 1] = inst->mono_out_lite[i];
        }
        if (n & 1) {
            /* Odd leftover sample (block sizes are always even in practice) -
             * process it directly so nothing is dropped. */
            inst->model->Process(&inst->mono_in[n - 1], &inst->mono_out[n - 1], 1);
        }
    } else {
        inst->model->Process(inst->mono_in, inst->mono_out, (size_t)n);
    }

    /* Cab IR convolution */
    if (!inst->cab_bypass && inst->cab_ir) {
        apply_cab_ir(inst, inst->mono_out, n);
    }

    /* 3-band EQ (mono, in place) */
    for (int i = 0; i < n; i++) {
        inst->mono_out[i] = eq_process(inst, inst->mono_out[i]);
    }

    /* Solo FX chain: Doubler -> Echo -> Reverb, per sample, mono -> stereo */
    float og = inst->output_gain;
    for (int i = 0; i < n; i++) {
        float src = inst->mono_out[i];

        float dl, dr;
        if (inst->doubler_bypass) { dl = src; dr = src; }
        else doubler_process(inst, src, &dl, &dr);

        float el, er;
        if (inst->echo_bypass) { el = dl; er = dr; }
        else echo_process(inst, dl, dr, &el, &er);

        float rl, rr;
        if (inst->reverb_bypass) { rl = el; rr = er; }
        else reverb_process(inst, el, er, &rl, &rr);

        float out_l = clampf(rl * og, -1.0f, 1.0f);
        float out_r = clampf(rr * og, -1.0f, 1.0f);
        audio_inout[i * 2]     = (int16_t)(out_l * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(out_r * 32767.0f);
    }
}

/* Minimal JSON field readers for state restore — encoder writes a flat
 * {"key":value,...} object so strstr + atof/sscanf is enough. */
static int json_get_float(const char *json, const char *key, float *out) {
    if (!json || !key || !out) return -1;
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(search)) return -1;
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return -1;
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(search)) return -1;
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len <= 0) return -1;
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\":\"", key);
    if (n <= 0 || n >= (int)sizeof(search)) return -1;
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += n;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; continue; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i;
}

static int nam_find_name(char names[][MAX_NAME_LEN], int count, const char *target) {
    if (!target || !target[0]) return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], target) == 0) return i;
    }
    return -1;
}

/* --- set_param --- */
static void v2_set_param(void *instance, const char *key, const char *val) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* Bulk state restore from get_param("state"). Match model/cab by saved
     * name first (survives reordering after rescans), fall back to index. */
    if (strcmp(key, "state") == 0) {
        float f; int i; char name[MAX_NAME_LEN];

        if (json_get_float(val, "input_level", &f) == 0) {
            inst->input_level = clampf(f, 0.0f, 1.0f);
            inst->input_gain  = knob_to_gain(inst->input_level);
        }
        if (json_get_float(val, "output_level", &f) == 0) {
            inst->output_level = clampf(f, 0.0f, 1.0f);
            inst->output_gain  = knob_to_gain(inst->output_level);
        }
        if (json_get_int(val, "quality", &i) == 0) inst->quality_lite = (i != 0);

        int target_model = -1;
        if (json_get_string(val, "model_name", name, sizeof(name)) > 0)
            target_model = nam_find_name(inst->model_names, inst->model_count, name);
        if (target_model < 0 && json_get_int(val, "model_index", &i) == 0)
            if (i >= 0 && i < inst->model_count) target_model = i;
        if (target_model >= 0 && target_model != inst->current_model_index) {
            inst->current_model_index = target_model;
            load_model_async(inst, inst->model_paths[target_model]);
        }

        int target_cab = -1;
        if (json_get_string(val, "cab_name", name, sizeof(name)) > 0)
            target_cab = nam_find_name(inst->cab_names, inst->cab_count, name);
        if (target_cab < 0 && json_get_int(val, "cab_index", &i) == 0)
            if (i >= 0 && i < inst->cab_count) target_cab = i;
        if (target_cab >= 0 && target_cab != inst->current_cab_index)
            load_cab(inst, target_cab);
        if (json_get_int(val, "cab_bypass", &i) == 0) inst->cab_bypass = (i != 0);

        if (json_get_float(val, "eq_low_gain", &f) == 0) inst->eq_low_gain = clampf(f, -15.0f, 15.0f);
        if (json_get_float(val, "eq_low_freq", &f) == 0) inst->eq_low_freq = clampf(f, 40.0f, 500.0f);
        if (json_get_float(val, "eq_mid_gain", &f) == 0) inst->eq_mid_gain = clampf(f, -15.0f, 15.0f);
        if (json_get_float(val, "eq_mid_freq", &f) == 0) inst->eq_mid_freq = clampf(f, 200.0f, 4000.0f);
        if (json_get_float(val, "eq_high_gain", &f) == 0) inst->eq_high_gain = clampf(f, -15.0f, 15.0f);
        if (json_get_float(val, "eq_high_freq", &f) == 0) inst->eq_high_freq = clampf(f, 1000.0f, 10000.0f);
        update_eq(inst);

        if (json_get_int(val, "doubler_bypass", &i) == 0) inst->doubler_bypass = (i != 0);
        if (json_get_float(val, "doubler_mix", &f) == 0) inst->doubler_mix = clampf(f, 0.0f, 1.0f);

        if (json_get_int(val, "echo_bypass", &i) == 0) inst->echo_bypass = (i != 0);
        if (json_get_float(val, "echo_time_ms", &f) == 0)
            inst->echo_time_ms = clampf(f, 50.0f, ECHO_MAX_SECONDS * 1000.0f);
        if (json_get_float(val, "echo_feedback", &f) == 0) inst->echo_feedback = clampf(f, 0.0f, 0.9f);
        if (json_get_float(val, "echo_filter_hz", &f) == 0) inst->echo_filter_hz = clampf(f, 400.0f, 8000.0f);
        if (json_get_float(val, "echo_mix", &f) == 0) inst->echo_mix = clampf(f, 0.0f, 1.0f);
        update_echo_filter(inst);

        if (json_get_int(val, "reverb_bypass", &i) == 0) inst->reverb_bypass = (i != 0);
        if (json_get_float(val, "reverb_room", &f) == 0) inst->reverb_room = clampf(f, 0.0f, 1.0f);
        if (json_get_float(val, "reverb_damping", &f) == 0) inst->reverb_damping = clampf(f, 0.0f, 1.0f);
        if (json_get_float(val, "reverb_mix", &f) == 0) inst->reverb_mix = clampf(f, 0.0f, 1.0f);
        update_reverb(inst);

        return;
    }

    if (strcmp(key, "input_level") == 0) {
        inst->input_level = clampf(atof(val), 0.0f, 1.0f);
        inst->input_gain = knob_to_gain(inst->input_level);
    } else if (strcmp(key, "output_level") == 0) {
        inst->output_level = clampf(atof(val), 0.0f, 1.0f);
        inst->output_gain = knob_to_gain(inst->output_level);
    } else if (strcmp(key, "quality") == 0) {
        inst->quality_lite = (atoi(val) != 0);
    } else if (strcmp(key, "model_index") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->model_count && idx != inst->current_model_index) {
            inst->current_model_index = idx;
            load_model_async(inst, inst->model_paths[idx]);
        }
    } else if (strcmp(key, "model") == 0) {
        load_model_async(inst, val);
    } else if (strcmp(key, "cab_index") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->cab_count && idx != inst->current_cab_index) {
            load_cab(inst, idx);
        }
    } else if (strcmp(key, "cab_bypass") == 0) {
        inst->cab_bypass = (atoi(val) != 0);
    } else if (strcmp(key, "eq_low_gain") == 0) {
        inst->eq_low_gain = clampf(atof(val), -15.0f, 15.0f); update_eq(inst);
    } else if (strcmp(key, "eq_low_freq") == 0) {
        inst->eq_low_freq = clampf(atof(val), 40.0f, 500.0f); update_eq(inst);
    } else if (strcmp(key, "eq_mid_gain") == 0) {
        inst->eq_mid_gain = clampf(atof(val), -15.0f, 15.0f); update_eq(inst);
    } else if (strcmp(key, "eq_mid_freq") == 0) {
        inst->eq_mid_freq = clampf(atof(val), 200.0f, 4000.0f); update_eq(inst);
    } else if (strcmp(key, "eq_high_gain") == 0) {
        inst->eq_high_gain = clampf(atof(val), -15.0f, 15.0f); update_eq(inst);
    } else if (strcmp(key, "eq_high_freq") == 0) {
        inst->eq_high_freq = clampf(atof(val), 1000.0f, 10000.0f); update_eq(inst);
    } else if (strcmp(key, "doubler_bypass") == 0) {
        inst->doubler_bypass = (atoi(val) != 0);
    } else if (strcmp(key, "doubler_mix") == 0) {
        inst->doubler_mix = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "echo_bypass") == 0) {
        inst->echo_bypass = (atoi(val) != 0);
    } else if (strcmp(key, "echo_time_ms") == 0) {
        inst->echo_time_ms = clampf(atof(val), 50.0f, ECHO_MAX_SECONDS * 1000.0f);
    } else if (strcmp(key, "echo_feedback") == 0) {
        inst->echo_feedback = clampf(atof(val), 0.0f, 0.9f);
    } else if (strcmp(key, "echo_filter_hz") == 0) {
        inst->echo_filter_hz = clampf(atof(val), 400.0f, 8000.0f); update_echo_filter(inst);
    } else if (strcmp(key, "echo_mix") == 0) {
        inst->echo_mix = clampf(atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "echo_tap") == 0) {
        /* Momentary: any write to this key is a tap, the value itself is not
         * meaningful state and is never persisted. */
        echo_register_tap(inst);
    } else if (strcmp(key, "reverb_bypass") == 0) {
        inst->reverb_bypass = (atoi(val) != 0);
    } else if (strcmp(key, "reverb_room") == 0) {
        inst->reverb_room = clampf(atof(val), 0.0f, 1.0f); update_reverb(inst);
    } else if (strcmp(key, "reverb_damping") == 0) {
        inst->reverb_damping = clampf(atof(val), 0.0f, 1.0f); update_reverb(inst);
    } else if (strcmp(key, "reverb_mix") == 0) {
        inst->reverb_mix = clampf(atof(val), 0.0f, 1.0f); update_reverb(inst);
    }
}

/* --- get_param --- */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst || !key || !buf) return -1;

    /* Bulk serialization for slot autosave. echo_tap is momentary and is
     * deliberately never included - it has no meaningful saved state. */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"input_level\":%.4f,\"output_level\":%.4f,\"quality\":%d,"
            "\"model_index\":%d,\"model_name\":\"%s\","
            "\"cab_index\":%d,\"cab_name\":\"%s\",\"cab_bypass\":%d,"
            "\"eq_low_gain\":%.2f,\"eq_low_freq\":%.1f,"
            "\"eq_mid_gain\":%.2f,\"eq_mid_freq\":%.1f,"
            "\"eq_high_gain\":%.2f,\"eq_high_freq\":%.1f,"
            "\"doubler_bypass\":%d,\"doubler_mix\":%.3f,"
            "\"echo_bypass\":%d,\"echo_time_ms\":%.1f,\"echo_feedback\":%.3f,"
            "\"echo_filter_hz\":%.1f,\"echo_mix\":%.3f,"
            "\"reverb_bypass\":%d,\"reverb_room\":%.3f,\"reverb_damping\":%.3f,\"reverb_mix\":%.3f}",
            inst->input_level, inst->output_level, inst->quality_lite,
            inst->current_model_index, inst->model_name,
            inst->current_cab_index, inst->cab_name, inst->cab_bypass ? 1 : 0,
            inst->eq_low_gain, inst->eq_low_freq,
            inst->eq_mid_gain, inst->eq_mid_freq,
            inst->eq_high_gain, inst->eq_high_freq,
            inst->doubler_bypass ? 1 : 0, inst->doubler_mix,
            inst->echo_bypass ? 1 : 0, inst->echo_time_ms, inst->echo_feedback,
            inst->echo_filter_hz, inst->echo_mix,
            inst->reverb_bypass ? 1 : 0, inst->reverb_room, inst->reverb_damping, inst->reverb_mix);
    }

    if (strcmp(key, "input_level") == 0) return snprintf(buf, buf_len, "%.2f", inst->input_level);
    if (strcmp(key, "output_level") == 0) return snprintf(buf, buf_len, "%.2f", inst->output_level);
    if (strcmp(key, "quality") == 0) return snprintf(buf, buf_len, "%d", inst->quality_lite);

    if (strcmp(key, "model_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->model_name[0] ? inst->model_name : "(none)");
    if (strcmp(key, "model_count") == 0) return snprintf(buf, buf_len, "%d", inst->model_count);
    if (strcmp(key, "model_index") == 0) return snprintf(buf, buf_len, "%d", inst->current_model_index);

    if (strcmp(key, "model_list") == 0) {
        scan_models(inst);
        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->model_count && written < buf_len - 40; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}", inst->model_names[i], i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }

    if (strcmp(key, "loading") == 0)
        return snprintf(buf, buf_len, "%d", inst->loading.load(std::memory_order_acquire) ? 1 : 0);

    if (strcmp(key, "cab_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->cab_name[0] ? inst->cab_name : "(none)");
    if (strcmp(key, "cab_count") == 0) return snprintf(buf, buf_len, "%d", inst->cab_count);
    if (strcmp(key, "cab_index") == 0) return snprintf(buf, buf_len, "%d", inst->current_cab_index);
    if (strcmp(key, "cab_bypass") == 0) return snprintf(buf, buf_len, "%d", inst->cab_bypass ? 1 : 0);

    if (strcmp(key, "cab_list") == 0) {
        scan_cabs(inst);
        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->cab_count && written < buf_len - 40; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}", inst->cab_names[i], i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }

    if (strcmp(key, "eq_low_gain") == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_low_gain);
    if (strcmp(key, "eq_low_freq") == 0) return snprintf(buf, buf_len, "%.1f", inst->eq_low_freq);
    if (strcmp(key, "eq_mid_gain") == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_mid_gain);
    if (strcmp(key, "eq_mid_freq") == 0) return snprintf(buf, buf_len, "%.1f", inst->eq_mid_freq);
    if (strcmp(key, "eq_high_gain") == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_high_gain);
    if (strcmp(key, "eq_high_freq") == 0) return snprintf(buf, buf_len, "%.1f", inst->eq_high_freq);

    if (strcmp(key, "doubler_bypass") == 0) return snprintf(buf, buf_len, "%d", inst->doubler_bypass ? 1 : 0);
    if (strcmp(key, "doubler_mix") == 0) return snprintf(buf, buf_len, "%.3f", inst->doubler_mix);

    if (strcmp(key, "echo_bypass") == 0) return snprintf(buf, buf_len, "%d", inst->echo_bypass ? 1 : 0);
    if (strcmp(key, "echo_time_ms") == 0) return snprintf(buf, buf_len, "%.1f", inst->echo_time_ms);
    if (strcmp(key, "echo_feedback") == 0) return snprintf(buf, buf_len, "%.3f", inst->echo_feedback);
    if (strcmp(key, "echo_filter_hz") == 0) return snprintf(buf, buf_len, "%.1f", inst->echo_filter_hz);
    if (strcmp(key, "echo_mix") == 0) return snprintf(buf, buf_len, "%.3f", inst->echo_mix);
    if (strcmp(key, "echo_tap") == 0) return snprintf(buf, buf_len, "0");

    if (strcmp(key, "reverb_bypass") == 0) return snprintf(buf, buf_len, "%d", inst->reverb_bypass ? 1 : 0);
    if (strcmp(key, "reverb_room") == 0) return snprintf(buf, buf_len, "%.3f", inst->reverb_room);
    if (strcmp(key, "reverb_damping") == 0) return snprintf(buf, buf_len, "%.3f", inst->reverb_damping);
    if (strcmp(key, "reverb_mix") == 0) return snprintf(buf, buf_len, "%.3f", inst->reverb_mix);

    /* ui_hierarchy - returned dynamically (static shape, but kept alongside
     * the rest of the dynamic get_param handling for a single source of
     * truth with module.json). */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"label\":\"Nam A2\","
                    "\"children\":null,"
                    "\"knobs\":[\"input_level\",\"output_level\",\"quality\"],"
                    "\"params\":["
                        "{\"key\":\"input_level\",\"label\":\"Input\"},"
                        "{\"key\":\"output_level\",\"label\":\"Output\"},"
                        "{\"key\":\"quality\",\"label\":\"Quality\"},"
                        "{\"key\":\"cab_bypass\",\"label\":\"Cab Bypass\"},"
                        "{\"level\":\"models\",\"label\":\"Choose Model\"},"
                        "{\"level\":\"cabs\",\"label\":\"Choose Cabinet\"},"
                        "{\"level\":\"eq\",\"label\":\"3-Band EQ\"},"
                        "{\"level\":\"doubler\",\"label\":\"Doubler\"},"
                        "{\"level\":\"echo\",\"label\":\"Echo\"},"
                        "{\"level\":\"reverb\",\"label\":\"Reverb\"}"
                    "]"
                "},"
                "\"models\":{"
                    "\"label\":\"Model\","
                    "\"items_param\":\"model_list\","
                    "\"select_param\":\"model_index\","
                    "\"children\":null,\"knobs\":[],\"params\":[]"
                "},"
                "\"cabs\":{"
                    "\"label\":\"Cabinet\","
                    "\"items_param\":\"cab_list\","
                    "\"select_param\":\"cab_index\","
                    "\"children\":null,\"knobs\":[],\"params\":[]"
                "},"
                "\"eq\":{"
                    "\"label\":\"3-Band EQ\","
                    "\"children\":null,"
                    "\"knobs\":[\"eq_low_gain\",\"eq_low_freq\",\"eq_mid_gain\","
                               "\"eq_mid_freq\",\"eq_high_gain\",\"eq_high_freq\"],"
                    "\"params\":[\"eq_low_gain\",\"eq_low_freq\",\"eq_mid_gain\","
                                "\"eq_mid_freq\",\"eq_high_gain\",\"eq_high_freq\"]"
                "},"
                "\"doubler\":{"
                    "\"label\":\"Doubler\","
                    "\"children\":null,"
                    "\"knobs\":[\"doubler_mix\"],"
                    "\"params\":["
                        "{\"key\":\"doubler_bypass\",\"label\":\"Doubler Bypass\"},"
                        "\"doubler_mix\""
                    "]"
                "},"
                "\"echo\":{"
                    "\"label\":\"Echo\","
                    "\"children\":null,"
                    "\"knobs\":[\"echo_time_ms\",\"echo_feedback\",\"echo_filter_hz\",\"echo_mix\"],"
                    "\"params\":["
                        "{\"key\":\"echo_bypass\",\"label\":\"Echo Bypass\"},"
                        "\"echo_time_ms\",\"echo_feedback\",\"echo_filter_hz\",\"echo_mix\","
                        "{\"key\":\"echo_tap\",\"label\":\"Tap Tempo\"}"
                    "]"
                "},"
                "\"reverb\":{"
                    "\"label\":\"Reverb\","
                    "\"children\":null,"
                    "\"knobs\":[\"reverb_room\",\"reverb_damping\",\"reverb_mix\"],"
                    "\"params\":["
                        "{\"key\":\"reverb_bypass\",\"label\":\"Reverb Bypass\"},"
                        "\"reverb_room\",\"reverb_damping\",\"reverb_mix\""
                    "]"
                "}"
            "}"
        "}";
        return snprintf(buf, buf_len, "%s", hierarchy);
    }

    return -1;
}

/* ======================================================================== */
/* Entry point                                                               */
/* ======================================================================== */

static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block    = v2_process_block;
    g_fx_api_v2.set_param        = v2_set_param;
    g_fx_api_v2.get_param        = v2_get_param;
    g_fx_api_v2.on_midi          = nullptr; /* No MIDI handling needed */

    plugin_log("Nam A2: audio FX plugin initialized (NeuralAudio by Mike Oliphant)");

    return &g_fx_api_v2;
}
