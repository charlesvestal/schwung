/*
 * `ui_flags` ran out of bits, and widening it is NOT the free move it looks
 * like.
 *
 * This is the same defect class as the `write_idx` uint8_t: a field too narrow
 * to address its own range, where nothing at the site says so. `ui_flags` is a
 * uint8_t holding eight defined flags, so a ninth flag written as 0x100 is
 * truncated to 0 on store — the shim would set nothing, shadow_ui would see
 * nothing, and the gesture would simply never fire with no error anywhere.
 *
 * The obvious fix — make `ui_flags` a uint16_t — is worse. `ui_patch_index`
 * (uint16_t) sits IMMEDIATELY after it at offset 8 with no padding to absorb
 * the growth, so widening moves every field behind it and changes
 * sizeof(shadow_control_t). The shim and shadow_ui are separate binaries
 * mapping one SHM segment; a layout change that reaches one before the other
 * is silent corruption of unrelated fields, not a build error. Install order
 * is not something a struct definition gets to assume.
 *
 * So flags 0x0100+ live in `ui_flags_ext`, which was `reserved16` — already at
 * that offset, already the right width, referenced nowhere. This test pins the
 * three things that have to stay true:
 *
 *   1. every defined flag fits the field it is routed to
 *   2. the two fields do not overlap in the flat space JS sees
 *   3. the layout around them did not move
 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "shadow_constants.h"

#define LOW_FLAGS (SHADOW_UI_FLAG_JUMP_TO_SLOT | SHADOW_UI_FLAG_JUMP_TO_MASTER_FX | \
                   SHADOW_UI_FLAG_JUMP_TO_OVERTAKE | SHADOW_UI_FLAG_SAVE_STATE | \
                   SHADOW_UI_FLAG_JUMP_TO_SCREENREADER | SHADOW_UI_FLAG_SET_CHANGED | \
                   SHADOW_UI_FLAG_JUMP_TO_SETTINGS | SHADOW_UI_FLAG_JUMP_TO_TOOLS)

#define EXT_FLAGS (SHADOW_UI_FLAG_SNAPSHOT_TAKE | SHADOW_UI_FLAG_SNAPSHOT_RECALL)

int main(void) {
    shadow_control_t c;

    /* 1. Each flag fits the field it is routed to. A low flag that grew past
     *    0xFF, or an ext flag written below the shift, silently stores 0. */
    assert(LOW_FLAGS <= 0xFF);
    assert((EXT_FLAGS & 0xFF) == 0);
    assert((EXT_FLAGS >> SHADOW_UI_FLAG_EXT_SHIFT) <=
           (int)((uint16_t)~(uint16_t)0));

    /* 2. The two spaces are disjoint, so js_shadow_get_ui_flags can OR them
     *    into one word and js_shadow_clear_ui_flags can split a mask back
     *    apart without either losing a flag to the other. */
    assert((LOW_FLAGS & EXT_FLAGS) == 0);

    /* 3. The field widths themselves. `ui_flags` staying 1 byte is what makes
     *    the layout claim below true; `ui_flags_ext` being 2 is what gives the
     *    ext space 16 flags rather than 8. */
    assert(sizeof(c.ui_flags) == 1);
    assert(sizeof(c.ui_flags_ext) == 2);

    /* 3b. The shift is exactly the WIDTH of the low field, not a number that
     *     happens to be big enough today. At any smaller shift the ext space
     *     aliases onto low flags in the flat word — shift 4 would put
     *     SNAPSHOT_TAKE on top of JUMP_TO_SCREENREADER — and a clear of one
     *     would wipe the other. Deriving it from sizeof is what makes the
     *     disjointness in (2) hold for flags nobody has added yet, rather than
     *     only for the ones enumerated above. */
    assert(SHADOW_UI_FLAG_EXT_SHIFT == 8 * (int)sizeof(c.ui_flags));

    /* 4. The layout did not move. These are the offsets the struct has had all
     *    along; ui_flags_ext took the slot `reserved16` was already occupying.
     *    If a future edit widens ui_flags after all, this is what fails —
     *    loudly, on the dev machine, instead of quietly on a half-updated
     *    device. */
    assert(offsetof(shadow_control_t, ui_flags) == 7);
    assert(offsetof(shadow_control_t, ui_patch_index) == 8);
    assert(offsetof(shadow_control_t, ui_flags_ext) == 10);
    assert(offsetof(shadow_control_t, ui_request_id) == 12);

    /* 4b. The Recall Quantize REGISTER round-trips through ext space.
     *
     *     Its mask and shift are declared FLAT (0x3000, 12) while ui_flags_ext
     *     holds bits 8+ already shifted down, so both must come down by
     *     EXT_SHIFT before touching the field. Masking the field with the flat
     *     0x3000 reads bits that live at 0x30 — always zero, so the setting
     *     silently never applies. Written wrong first time; this is the check
     *     that would have caught it. */
    assert((SHADOW_UI_RECALL_Q_MASK & LOW_FLAGS) == 0);
    assert((SHADOW_UI_RECALL_Q_MASK & EXT_FLAGS) == 0);
    for (int v = 0; v <= 3; v++) {
        c.ui_flags_ext = (uint16_t)(v << (SHADOW_UI_RECALL_Q_SHIFT - SHADOW_UI_FLAG_EXT_SHIFT));
        int back = (c.ui_flags_ext
                    & (SHADOW_UI_RECALL_Q_MASK >> SHADOW_UI_FLAG_EXT_SHIFT))
                   >> (SHADOW_UI_RECALL_Q_SHIFT - SHADOW_UI_FLAG_EXT_SHIFT);
        assert(back == v);
    }
    /* The register must not collide with the event flags that share the field. */
    c.ui_flags_ext = (uint16_t)((SHADOW_UI_FLAG_SNAPSHOT_QUEUED >> SHADOW_UI_FLAG_EXT_SHIFT)
                                | (3 << (SHADOW_UI_RECALL_Q_SHIFT - SHADOW_UI_FLAG_EXT_SHIFT)));
    assert((c.ui_flags_ext & (SHADOW_UI_FLAG_SNAPSHOT_QUEUED >> SHADOW_UI_FLAG_EXT_SHIFT)) != 0);
    assert(((c.ui_flags_ext & (SHADOW_UI_RECALL_Q_MASK >> SHADOW_UI_FLAG_EXT_SHIFT))
            >> (SHADOW_UI_RECALL_Q_SHIFT - SHADOW_UI_FLAG_EXT_SHIFT)) == 3);
    /* Pulse mapping: off / beat / bar / two bars at 24 PPQN. */
    assert(SHADOW_UI_RECALL_Q_PULSES(0) == 0);
    assert(SHADOW_UI_RECALL_Q_PULSES(1) == 24);
    assert(SHADOW_UI_RECALL_Q_PULSES(2) == 96);
    assert(SHADOW_UI_RECALL_Q_PULSES(3) == 192);

    /* 5. The struct is EXACTLY its buffer — shadow_constants.h asserts `==`,
     *    not `<=`, so the SHM size is a fixed contract between two binaries
     *    and not merely an upper bound. Restated here so a failure names the
     *    reason instead of a numbered typedef. */
    assert(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE);

    printf("PASS test_ui_flags_layout (control=%zu bytes, ext space holds 16 flags)\n",
           sizeof(shadow_control_t));
    return 0;
}
