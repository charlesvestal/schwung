# Plan: Queued Pattern Switching

## Problem

Currently, pattern switching is immediate. If a track is at step 8 of 16 and the user switches patterns, step 9 comes from the new pattern. This causes jarring musical transitions.

## Desired Behavior

OP-Z / Elektron style:
1. User selects new pattern → queued as `next_pattern`
2. Track continues playing current pattern to completion
3. At loop boundary, switch to `next_pattern` starting at its `loop_start`
4. Each track switches independently at its own loop point

## Edge Cases

**Switching to shorter pattern:**
- Pattern A: 16 steps, at step 12
- Pattern B: 8 steps
- At A's loop point → B starts at B's loop_start (0) ✓

**Switching to longer pattern:**
- Pattern A: 8 steps
- Pattern B: 16 steps
- At A's loop point → B starts at B's loop_start (0) ✓

Starting at the new pattern's loop_start avoids "step 14 in 8-step pattern" issues.

---

## Implementation

### DSP Changes (seq_plugin.c)

1. **Add field to track_t struct:**
   ```c
   typedef struct {
       // ... existing fields ...
       int8_t next_pattern;  /* Queued pattern, -1 = none */
   } track_t;
   ```

2. **Initialize in init_track():**
   ```c
   track->next_pattern = -1;
   ```

3. **Modify set_param for track_N_pattern:**
   ```c
   else if (strcmp(param, "pattern") == 0) {
       int pat = atoi(value);
       if (pat >= 0 && pat < NUM_PATTERNS) {
           if (g_playing) {
               /* Queue for next loop boundary */
               g_tracks[track].next_pattern = pat;
           } else {
               /* Immediate switch when stopped (for editing) */
               g_tracks[track].current_pattern = pat;
               g_tracks[track].next_pattern = -1;
           }
       }
   }
   ```

4. **Modify advance_step() at loop wrap:**
   ```c
   if (track->current_step >= pattern->loop_end) {
       /* Check for queued pattern change */
       if (track->next_pattern >= 0) {
           track->current_pattern = track->next_pattern;
           track->next_pattern = -1;
           pattern = get_current_pattern(track);  /* Update pointer */
       }
       track->current_step = pattern->loop_start;
       track->loop_count++;
   }
   ```

5. **Add get_param for queued pattern:**
   ```c
   else if (strcmp(param, "next_pattern") == 0) {
       return snprintf(buf, buf_len, "%d", g_tracks[track].next_pattern);
   }
   ```

### UI Changes (pattern.js)

- Show queued pattern with visual feedback
- Options:
  - Flashing/pulsing LED for queued pattern
  - Different brightness (current = bright, queued = medium)
  - Border or outline effect

### Tests (test_seq_plugin.c)

```c
TEST(pattern_switch_when_stopped) {
    /* Immediate switch when not playing */
    set_param("track_0_pattern", "5");
    char buf[32];
    get_param("track_0_pattern", buf, sizeof(buf));
    ASSERT_EQ(atoi(buf), 5);
}

TEST(pattern_switch_when_playing_queued) {
    /* Queue switch when playing */
    set_param("playing", "1");
    set_param("track_0_pattern", "3");

    char buf[32];
    get_param("track_0_pattern", buf, sizeof(buf));
    ASSERT_EQ(atoi(buf), 0);  /* Still on original */

    get_param("track_0_next_pattern", buf, sizeof(buf));
    ASSERT_EQ(atoi(buf), 3);  /* Queued */
}

TEST(pattern_switch_at_loop_boundary) {
    /* Pattern switches when loop wraps */
    set_param("track_0_loop_end", "3");  /* 4-step loop */
    set_param("track_0_step_0_add_note", "60");
    set_param("playing", "1");
    set_param("track_0_pattern", "1");  /* Queue pattern 1 */

    render_steps(4);  /* Complete the loop */

    char buf[32];
    get_param("track_0_pattern", buf, sizeof(buf));
    ASSERT_EQ(atoi(buf), 1);  /* Now on pattern 1 */

    get_param("track_0_next_pattern", buf, sizeof(buf));
    ASSERT_EQ(atoi(buf), -1);  /* Queue cleared */
}

TEST(pattern_switch_different_lengths) {
    /* Switching to pattern with different loop points */
    set_param("track_0_loop_end", "15");  /* 16-step loop */

    /* Set up pattern 1 with 8-step loop */
    set_param("track_0_pattern", "1");
    set_param("track_0_loop_end", "7");
    set_param("track_0_pattern", "0");  /* Back to pattern 0 */

    set_param("playing", "1");
    set_param("track_0_pattern", "1");  /* Queue shorter pattern */

    render_steps(16);  /* Complete pattern 0's loop */

    char buf[32];
    get_param("track_0_current_step", buf, sizeof(buf));
    ASSERT_EQ(atoi(buf), 0);  /* Started at pattern 1's loop_start */
}
```

---

## Notes

- Independent per-track switching (OP-Z style)
- When stopped: immediate switch for editing convenience
- When playing: queued until loop boundary
- New pattern always starts at its own loop_start
