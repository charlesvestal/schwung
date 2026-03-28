/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004 Grame
Copyright (C) 2025 Cycling '74 - Adapted for Move
Copyright (C) 2026 Schwung - Shadow driver (shared memory)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef __JackShadowDriver__
#define __JackShadowDriver__

#include "../linux/driver.h"
#include "JackAudioDriver.h"
#include "JackThreadedDriver.h"
#include "JackTime.h"
#include "JackMidiAsyncQueue.h"
#include "schwung_jack_shm.h"
#include <array>

namespace Jack
{

/*!
\brief The Shadow driver - shared memory interface to Move shim.
*/

class JackShadowDriver : public JackAudioDriver
{

    private:

        SchwungJackShm* fShm;
        uint32_t fLastFrameCounter;

        jack_default_audio_sample_t fInputBuffer[2][SCHWUNG_JACK_AUDIO_FRAMES];
        jack_default_audio_sample_t fOutputBuffer[2][SCHWUNG_JACK_AUDIO_FRAMES];

        jack_port_id_t fMIDICaptureId;
        jack_port_id_t fMIDIPlaybackId;

        jack_port_id_t fExternalMIDICaptureId;
        jack_port_id_t fExternalMIDIPlaybackId;

        jack_port_id_t fDisplayId;

        /* Larger queue (16KB/4096 msgs) to absorb LED burst on startup.
         * Default 4KB overflows when rnbomovecontrol sends all pad colors
         * before the driver's Process() loop starts draining. */
        JackMidiAsyncQueue fMIDIReadQueue{16384, 4096};
        JackMidiAsyncQueue fMIDIWriteQueue;

        JackMidiAsyncQueue fExternalMIDIReadQueue;
        JackMidiAsyncQueue fExternalMIDIWriteQueue;

        //for sysex accumulation
        std::array<SchwungJackUsbMidiMsg, SCHWUNG_JACK_MIDI_OUT_MAX> mPendingMIDIQueue;
        size_t mPendingMIDIEventsStart = 0;
        size_t mPendingMIDIEventsEnd = 0;
        bool mSysexActive = false;

    public:

        JackShadowDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
          : JackAudioDriver(name, alias, engine, table), fShm(NULL), fLastFrameCounter(0)
        {}
        virtual ~JackShadowDriver()
        {}

        int Open(jack_nframes_t buffer_size,
                        jack_nframes_t samplerate,
                        bool capturing,
                        bool playing,
                        int inchannels,
                        int outchannels,
                        bool monitor,
                        const char* capture_driver_name,
                        const char* playback_driver_name,
                        jack_nframes_t capture_latency,
                        jack_nframes_t playback_latency);

        int Attach();
        int Close();

        int Process();
        int Read();
        int Write();

        // BufferSize can not be changed
        bool IsFixedBufferSize()
        {
            return true;
        }

        // JACK API emulation for the midi driver
        int is_realtime() const;

};

} // end of namespace

#endif
