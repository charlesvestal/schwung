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
        /* Convert MIDI note to msfa log frequency format.
         * msfa logfreq = log2(frequency) * (1 << 24)
         *
         * For standard 12-TET tuning:
         *   freq = 440 * 2^((midinote - 69) / 12)
         *   log2(freq) = log2(440) + (midinote - 69) / 12
         *   logfreq = log2(440) * (1 << 24) + (midinote - 69) * (1 << 24) / 12
         */
        const int32_t per_semitone = (1 << 24) / 12;  /* 1,398,101 */
        /* log2(440) * (1 << 24) = 8.78135... * 16777216 ≈ 147,318,855 */
        const int32_t a4_logfreq = 147318855;
        return a4_logfreq + (midinote - 69) * per_semitone;
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
