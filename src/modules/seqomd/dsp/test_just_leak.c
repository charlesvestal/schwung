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
    
    // Setup: repeating note with long length (like Set 16)
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
    int samples_per_step = MOVE_SAMPLE_RATE / 8; // 120 BPM
    
    // Render 2 loops
    for (int loop = 0; loop < 2; loop++) {
        for (int step = 0; step < 16; step++) {
            int rendered = 0;
            while (rendered < samples_per_step) {
                int frames = (samples_per_step - rendered) < MOVE_FRAMES_PER_BLOCK ? 
                             (samples_per_step - rendered) : MOVE_FRAMES_PER_BLOCK;
                plugin->render_block(buf, frames);
                rendered += frames;
            }
        }
    }
    int after_2_loops = count_active_scheduler_slots();
    
    // Render 8 more loops
    for (int loop = 0; loop < 8; loop++) {
        for (int step = 0; step < 16; step++) {
            int rendered = 0;
            while (rendered < samples_per_step) {
                int frames = (samples_per_step - rendered) < MOVE_FRAMES_PER_BLOCK ? 
                             (samples_per_step - rendered) : MOVE_FRAMES_PER_BLOCK;
                plugin->render_block(buf, frames);
                rendered += frames;
            }
        }
    }
    int after_10_loops = count_active_scheduler_slots();
    
    plugin->set_param("playing", "0");
    
    printf("SCHEDULER LEAK TEST (Set 16 Regression)\n");
    printf("=========================================\n");
    printf("Active slots after 2 loops:  %d\n", after_2_loops);
    printf("Active slots after 10 loops: %d\n", after_10_loops);
    printf("Slot growth: %d\n\n", after_10_loops - after_2_loops);
    
    if (after_2_loops < 20 && after_10_loops < 20 && (after_10_loops - after_2_loops) < 10) {
        printf("✓ PASS: No scheduler leak detected\n");
        printf("  Slots remain bounded over many loops\n");
        printf("  Set 16 pattern should play indefinitely\n");
        return 0;
    } else {
        printf("✗ FAIL: Scheduler leak detected!\n");
        printf("  Slots are accumulating over loops\n");
        printf("  This would cause Set 16 to stop playing\n");
        return 1;
    }
}
