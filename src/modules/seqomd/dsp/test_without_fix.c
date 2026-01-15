#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void mock_log(const char *msg) { (void)msg; }
static int mock_midi_send_internal(const uint8_t *msg, int len) { (void)msg; (void)len; return len; }
static int mock_midi_send_external(const uint8_t *msg, int len) { (void)msg; (void)len; return len; }

#include "host/plugin_api_v1.h"
static host_api_v1_t g_mock_host;

// Temporarily undefine to include seq_plugin with modified code
#include "seq_plugin.c"
#include "midi.c"
#include "scheduler.c"
#include "transpose.c"
#include "scale.c"
#include "arpeggiator.c"
#include "track.c"
#include "params.c"

// Override the schedule_note function to simulate the BUG
#undef schedule_note
static void schedule_note_BUGGY(uint8_t note, uint8_t velocity, uint8_t channel,
                          int swing, double on_phase, double length, int gate) {
    double swing_delay = calculate_swing_delay(swing, on_phase);
    double swung_on_phase = on_phase + swing_delay;
    double gate_mult = gate / 100.0;
    double note_duration = length * gate_mult;
    double off_phase = swung_on_phase + note_duration;
    
    int conflict_idx = find_conflicting_note(note, channel);
    if (conflict_idx >= 0) {
        scheduled_note_t *conflict = &g_scheduled_notes[conflict_idx];
        if (swung_on_phase < conflict->off_phase) {
            double early_off = swung_on_phase - 0.001;
            if (early_off > g_global_phase) {
                conflict->off_phase = early_off;
            } else {
                if (conflict->on_sent && !conflict->off_sent) {
                    send_note_off(conflict->note, conflict->channel);
                    conflict->off_sent = 1;
                    // BUG: missing conflict->active = 0; here!
                }
            }
        }
    }
    
    int slot = find_free_slot();
    if (slot < 0) {
        return;
    }
    
    scheduled_note_t *sn = &g_scheduled_notes[slot];
    sn->note = note;
    sn->channel = channel;
    sn->velocity = velocity;
    sn->on_phase = swung_on_phase;
    sn->off_phase = off_phase;
    sn->on_sent = 0;
    sn->off_sent = 0;
    sn->active = 1;
}

#define schedule_note schedule_note_BUGGY

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
    printf("TESTING WITHOUT THE FIX (simulating the bug)\n");
    printf("=============================================\n\n");
    
    g_mock_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_mock_host.sample_rate = MOVE_SAMPLE_RATE;
    g_mock_host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    g_mock_host.log = mock_log;
    g_mock_host.midi_send_internal = mock_midi_send_internal;
    g_mock_host.midi_send_external = mock_midi_send_external;
    
    plugin_api_v1_t *plugin = move_plugin_init_v1(&g_mock_host);
    plugin->on_load("/test", NULL);
    
    // Setup: repeating note (simulates Set 16)
    plugin->set_param("track_0_step_0_add_note", "60");
    plugin->set_param("track_0_step_0_length", "8");
    
    for (int i = 1; i < 8; i++) {
        char key[64];
        snprintf(key, sizeof(key), "track_0_step_%d_add_note", i);
        plugin->set_param(key, "60");
        snprintf(key, sizeof(key), "track_0_step_%d_length", i);
        plugin->set_param(key, "8");
    }
    
    plugin->set_param("playing", "1");
    
    int16_t buf[MOVE_FRAMES_PER_BLOCK * 2];
    int samples_per_step = MOVE_SAMPLE_RATE / 8;
    
    printf("Simulating Set 16 playback over 20 loops:\n\n");
    
    for (int loop = 1; loop <= 20; loop++) {
        for (int step = 0; step < 16; step++) {
            int rendered = 0;
            while (rendered < samples_per_step) {
                int frames = (samples_per_step - rendered) < MOVE_FRAMES_PER_BLOCK ? 
                             (samples_per_step - rendered) : MOVE_FRAMES_PER_BLOCK;
                plugin->render_block(buf, frames);
                rendered += frames;
            }
        }
        
        if (loop % 2 == 0 || loop == 1) {
            int active = count_active_scheduler_slots();
            printf("Loop %2d: %3d active slots", loop, active);
            if (active > 100) {
                printf(" ⚠️  CRITICALLY HIGH!");
            } else if (active > 50) {
                printf(" ⚠️  High");
            }
            printf("\n");
        }
    }
    
    int final_active = count_active_scheduler_slots();
    plugin->set_param("playing", "0");
    
    printf("\n✗ FAIL: Scheduler leaked %d slots!\n", final_active);
    printf("  Without the fix, Set 16 stops playing after ~20 loops\n");
    printf("  MAX_SCHEDULED_NOTES = 128, so scheduler would be full\n");
    
    return 1;
}
