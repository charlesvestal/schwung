#ifndef FORWARD_CHANNEL_H
#define FORWARD_CHANNEL_H

/*
 * Resolving a slot's forward channel.
 *
 * Lives in a header so tests/host can run it: the decision is three-way and it
 * was previously spread across shadow_chain_remap_channel (which knew only
 * "auto = receive channel") and four load call sites in shadow_chain_mgmt.c
 * (which applied a module's declared default by OVERWRITING forward_channel).
 * That made Auto a one-shot conversion rather than a live answer -- a slot
 * seeded from an older module.json kept that channel forever, and a module
 * whose default mattered only on some load paths silently got the receive
 * channel. The JP-8000 is the case that surfaced it: its arpeggiator is
 * reachable only on the Remote Control Channel, so an Auto slot with receive=1
 * forwarded on ch1 and never arpeggiated.
 *
 * Precedence for AUTO: the module's declared default, then the receive
 * channel, then passthrough (receive=All).
 */

#include "shadow_chain_types.h"

/* Effective forward channel for a slot.
 *   fwd     -2 THRU, -1 AUTO, 0-15 explicit  (the user's setting)
 *   mod_def -2 THRU, 0-15, anything else = "the module said nothing"
 *   recv    0-15 receive channel, < 0 = All
 * Returns 0-15 to remap onto, or SHADOW_FORWARD_THRU to preserve the channel.
 */
static inline int forward_channel_resolve(int fwd, int mod_def, int recv)
{
    if (fwd == SHADOW_FORWARD_AUTO) {
        if (mod_def == SHADOW_FORWARD_THRU || (mod_def >= 0 && mod_def <= 15))
            fwd = mod_def;
    }
    if (fwd == SHADOW_FORWARD_THRU)
        return SHADOW_FORWARD_THRU;
    if (fwd >= 0 && fwd <= 15)
        return fwd;
    if (recv < 0)
        return SHADOW_FORWARD_THRU;   /* recv=All + auto, no module default */
    return recv;
}

#endif /* FORWARD_CHANNEL_H */
