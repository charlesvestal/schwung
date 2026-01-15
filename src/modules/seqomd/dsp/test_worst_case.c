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
    printf("WORST CASE STRESS TEST\n");
    printf("======================\n");
    printf("Scenario:\n");
    printf("  - 16 tracks all playing\n");
    printf("  - 1/4 speed (0.25x)\n");
    printf("  - 7 notes per step (max)\n");
    printf("  - 16-step note length\n");
    printf("  - 100%% gate (notes last full duration)\n\n");
    
    g_mock_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_mock_host.sample_rate = MOVE_SAMPLE_RATE;
    g_mock_host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    g_mock_host.log = mock_log;
    g_mock_host.midi_send_internal = mock_midi_send_internal;
    g_mock_host.midi_send_external = mock_midi_send_external;
    
    plugin_api_v1_t *plugin = move_plugin_init_v1(&g_mock_host);
    plugin->on_load("/test", NULL);
    
    // Setup all 16 tracks with worst case settings
    for (int track = 0; track < 16; track++) {
        char key[64];
        
        // Set track to 1/4 speed
        snprintf(key, sizeof(key), "track_%d_speed", track);
        plugin->set_param(key, "0.25");
        
        // Setup step 0 with 7 notes, long length, full gate
        snprintf(key, sizeof(key), "track_%d_step_0_add_note", track);
        for (int note = 0; note < 7; note++) {
            plugin->set_param(key, "60");  // All C, different pitches don't matter
        }
        
        snprintf(key, sizeof(key), "track_%d_step_0_length", track);
        plugin->set_param(key, "16");
        
        snprintf(key, sizeof(key), "track_%d_step_0_gate", track);
        plugin->set_param(key, "100");
    }
    
    plugin->set_param("playing", "1");
    
    int16_t buf[MOVE_FRAMES_PER_BLOCK * 2];
    int samples_per_step = MOVE_SAMPLE_RATE / 8;
    
    printf("Rendering playback:\n");
    int max_active = 0;
    
    for (int step = 0; step < 32; step++) {
        int rendered = 0;
        while (rendered < samples_per_step) {
            int frames = (samples_per_step - rendered) < MOVE_FRAMES_PER_BLOCK ? 
                         (samples_per_step - rendered) : MOVE_FRAMES_PER_BLOCK;
            plugin->render_block(buf, frames);
            rendered += frames;
        }
        
        int active = count_active_scheduler_slots();
        if (active > max_active) max_active = active;
        
        if (step % 4 == 0) {
            printf("  Step %2d: %3d active slots", step, active);
            if (active > 100) {
                printf(" ⚠️  CRITICAL!");
            } else if (active > 80) {
                printf(" ⚠️  High");
            }
            printf("\n");
        }
    }
    
    plugin->set_param("playing", "0");
    
    printf("\nRESULTS:\n");
    printf("  MAX_SCHEDULED_NOTES: %d\n", MAX_SCHEDULED_NOTES);
    printf("  Peak active slots:   %d\n", max_active);
    printf("  Headroom:            %d slots\n\n", MAX_SCHEDULED_NOTES - max_active);
    
    if (max_active <= MAX_SCHEDULED_NOTES) {
        printf("✓ PASS: Scheduler can handle this load\n");
        return 0;
    } else {
        printf("✗ FAIL: Scheduler overflow!\n");
        printf("  Need to increase MAX_SCHEDULED_NOTES to at least %d\n", max_active + 20);
        return 1;
    }
}
