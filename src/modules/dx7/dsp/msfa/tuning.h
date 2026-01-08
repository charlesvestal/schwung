/*
 * Simplified tuning support for standalone msfa
 * Standard 12-TET only (no microtuning)
 */

#ifndef __SYNTH_TUNING_H
#define __SYNTH_TUNING_H

#include "synth.h"
#include <memory>
#include <string>
#include <cmath>

/* Log frequency calculation constants */
/* DX7 uses a 10.10 fixed-point log frequency representation */
/* Middle C (MIDI 60) = 261.63 Hz, A4 (MIDI 69) = 440 Hz */

class TuningState {
public:
    virtual ~TuningState() { }

    virtual int32_t midinote_to_logfreq(int midinote) {
        /* Convert MIDI note to msfa log frequency format */
        /* The format is roughly: logfreq = (midinote - 69) * (1 << 24) / 12 + (69 << 24) / 12 */
        /* Simplified: each semitone is (1 << 24) / 12 = 1398101 units */
        const int32_t logfreq_per_semitone = (1 << 24) / 12;
        /* A4 (440 Hz) at MIDI 69 corresponds to a specific logfreq */
        /* We use an offset so that MIDI 69 gives the correct frequency */
        const int32_t a4_logfreq = 69 * logfreq_per_semitone;
        return midinote * logfreq_per_semitone;
    }

    virtual bool is_standard_tuning() { return true; }
    virtual int scale_length() { return 12; }
    virtual std::string display_tuning_str() { return "Standard Tuning"; }
};

inline std::shared_ptr<TuningState> createStandardTuning() {
    return std::make_shared<TuningState>();
}

/* Stub functions for SCL/KBM loading - not supported */
inline std::shared_ptr<TuningState> createTuningFromSCLData(const std::string &) {
    return createStandardTuning();
}
inline std::shared_ptr<TuningState> createTuningFromKBMData(const std::string &) {
    return createStandardTuning();
}
inline std::shared_ptr<TuningState> createTuningFromSCLAndKBMData(const std::string &, const std::string &) {
    return createStandardTuning();
}

#endif
