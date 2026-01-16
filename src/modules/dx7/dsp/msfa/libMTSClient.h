/*
 * Stub for MTS-ESP client - we don't need microtuning support
 */

#ifndef LIB_MTS_CLIENT_H
#define LIB_MTS_CLIENT_H

typedef void MTSClient;

static inline MTSClient* MTS_RegisterClient() { return nullptr; }
static inline void MTS_DeregisterClient(MTSClient*) {}
static inline bool MTS_HasMaster(MTSClient*) { return false; }
static inline double MTS_NoteToFrequency(MTSClient*, char, char) { return 440.0; }
static inline double MTS_RetuningInSemitones(MTSClient*, char, char) { return 0.0; }

#endif
