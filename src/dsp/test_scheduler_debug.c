#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void mock_log(const char *msg) { (void)msg; }
static int mock_midi_send_internal(const uint8_t *msg, int len) { (void)msg; (void)len; return len; }
static int mock_midi_send_external(const uint8_t *msg, int len) { (void)msg; (void)len; return len; }

#include "host/plugin_api_v1.h"
static host_api_v1_t g_mock_host;
#include "seq_plugin.c"
#include "midi.c"
#include "scheduler.c"
#include "transpose.c"
#include "scale.c"
#include "arpeggiator.c"
#include "track.c"
#include "params.c"

static int count_active_scheduler_slots(void) {
    int count = 0;
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        if (g_scheduled_notes[i].active) {
            count++;
        }
    }
    return count;
}

int main() {
    g_mock_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_mock_host.sample_rate = MOVE_SAMPLE_RATE;
    g_mock_host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    g_mock_host.log = mock_log;
    g_mock_host.midi_send_internal = mock_midi_send_internal;
    g_mock_host.midi_send_external = mock_midi_send_external;
    
    plugin_api_v1_t *plugin = move_plugin_init_v1(&g_mock_host);
    plugin->on_load("/test", NULL);
    
    plugin->set_param("track_0_step_0_add_note", "60");
    plugin->set_param("track_0_step_0_length", "16");
    plugin->set_param("track_0_step_1_add_note", "60");
    plugin->set_param("track_0_step_1_length", "1");
    
    plugin->set_param("playing", "1");
    
    int16_t buf[MOVE_FRAMES_PER_BLOCK * 2];
    
    // Render 1 step
    int samples_per_step = MOVE_SAMPLE_RATE / 8; // 120 BPM, 4 steps/beat
    int rendered = 0;
    while (rendered < samples_per_step) {
        int frames = (samples_per_step - rendered) < MOVE_FRAMES_PER_BLOCK ? 
                     (samples_per_step - rendered) : MOVE_FRAMES_PER_BLOCK;
        plugin->render_block(buf, frames);
        rendered += frames;
    }
    
    printf("After step 0: %d active slots\n", count_active_scheduler_slots());
    
    // Render step 1
    rendered = 0;
    while (rendered < samples_per_step) {
        int frames = (samples_per_step - rendered) < MOVE_FRAMES_PER_BLOCK ? 
                     (samples_per_step - rendered) : MOVE_FRAMES_PER_BLOCK;
        plugin->render_block(buf, frames);
        rendered += frames;
    }
    
    printf("After step 1: %d active slots\n", count_active_scheduler_slots());
    
    // Print details
    for (int i = 0; i < MAX_SCHEDULED_NOTES; i++) {
        if (g_scheduled_notes[i].active) {
            printf("  Slot %d: note=%d on_sent=%d off_sent=%d on_phase=%.2f off_phase=%.2f\n",
                   i, g_scheduled_notes[i].note, g_scheduled_notes[i].on_sent,
                   g_scheduled_notes[i].off_sent, g_scheduled_notes[i].on_phase,
                   g_scheduled_notes[i].off_phase);
        }
    }
    
    return 0;
}
