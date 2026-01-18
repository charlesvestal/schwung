# Overlapping Duration-Based Overrides

## Problem

Several parameters in SEQOMD use "duration-based overrides" where a step's setting applies for `step->length` steps. Currently affected:

- **Gate override**: Step gate applies for step's length duration
- **CC1 override**: Step CC1 applies for step's length duration
- **CC2 override**: Step CC2 applies for step's length duration

Potentially affected in future:
- Arp settings (mode, speed, octave) if they get duration-based behavior

## Current Implementation (Remaining Counter)

```c
// In track_t:
uint8_t gate_override;
uint8_t gate_steps_remaining;

// In trigger_track_step():
if (step->gate > 0) {
    track->gate_override = step->gate;
    track->gate_steps_remaining = step->length;
}

// Each step:
if (track->gate_steps_remaining > 0) track->gate_steps_remaining--;
```

## The Overlap Problem

Example:
- Step 5: gate 50%, length 9 (should affect steps 5-13)
- Step 9: gate 25%, length 2 (should affect steps 9-10)

**Expected behavior:**
| Step | Gate | Reason |
|------|------|--------|
| 5-8  | 50%  | Step 5's override |
| 9-10 | 25%  | Step 9's override (takes priority) |
| 11-13| 50%  | Step 5's override **resumes** |
| 14+  | default | Both overrides ended |

**Current behavior:**
| Step | Gate | Reason |
|------|------|--------|
| 5-8  | 50%  | Step 5's override |
| 9-10 | 25%  | Step 9's override |
| 11-13| **default** | WRONG - step 5's override forgotten |
| 14+  | default | |

The problem: When step 9's override starts, it overwrites step 5's state. When step 9's ends, we've "forgotten" that step 5's was still active.

## Proposed Solution: Stack-Based Overrides

Instead of a single override value, use a small stack:

```c
#define MAX_OVERRIDE_STACK 8

typedef struct {
    uint8_t value;      // Override value (gate %, CC value, etc.)
    uint8_t end_step;   // Absolute step when this override ends
} override_entry_t;

// In track_t:
override_entry_t gate_stack[MAX_OVERRIDE_STACK];
uint8_t gate_stack_count;
```

### Push (when step with override is reached)

```c
void push_override(track_t *track, uint8_t value, uint8_t current_step, uint8_t length) {
    if (track->gate_stack_count < MAX_OVERRIDE_STACK) {
        uint8_t end_step = current_step + length;
        track->gate_stack[track->gate_stack_count].value = value;
        track->gate_stack[track->gate_stack_count].end_step = end_step;
        track->gate_stack_count++;
    }
}
```

### Get Current Value (pop expired, return top)

```c
uint8_t get_active_override(track_t *track, uint8_t current_step, uint8_t default_value) {
    // Pop expired entries from top of stack
    while (track->gate_stack_count > 0 &&
           current_step >= track->gate_stack[track->gate_stack_count - 1].end_step) {
        track->gate_stack_count--;  // "pop"
    }

    // Return top of stack, or default if empty
    if (track->gate_stack_count > 0) {
        return track->gate_stack[track->gate_stack_count - 1].value;
    }
    return default_value;
}
```

### How It Works

Example execution:

```
Step 5: push {50%, end:14}
  Stack: [{50%,14}]

Step 9: push {25%, end:11}
  Stack: [{50%,14}, {25%,11}]

Step 10: get_active_override()
  Top is {25%,11}, current_step(10) < end_step(11), not expired
  Return 25% ✓

Step 11: get_active_override()
  Top is {25%,11}, current_step(11) >= end_step(11), expired → pop
  Stack: [{50%,14}]
  Top is {50%,14}, current_step(11) < end_step(14), not expired
  Return 50% ✓ (resumed!)

Step 14: get_active_override()
  Top is {50%,14}, current_step(14) >= end_step(14), expired → pop
  Stack: []
  Empty → return default ✓
```

## Implementation Scope

To fully implement this, we'd need to update:

1. **track_t structure** - Replace single override fields with stacks
2. **init_track()** - Initialize stack counts to 0
3. **trigger_track_step()** - Push instead of overwrite
4. **get_step_gate()** - Use stack-based lookup
5. **CC handling** - Same pattern for CC1 and CC2

## Edge Cases to Consider

1. **Track looping**: When track loops, need to handle wrap-around of end_step values
2. **Stack overflow**: Max 8 entries should be plenty for 16-64 steps
3. **Clear on stop**: Should stack be cleared when playback stops?
4. **Pattern change**: Should stack be cleared when pattern changes?

## Status

**Parked** - Current simple implementation works for non-overlapping overrides. Stack approach documented for future implementation when needed.
