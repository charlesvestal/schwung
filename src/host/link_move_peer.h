/* link_move_peer.h -- which Link Audio peer is THIS device's Move.
 *
 * Move announces itself to Link under a name derived from its HOSTNAME, not a
 * constant: "move" announces as "Move", "move3" as "Move3". The sidecar used
 * to accept a channel only from a peer named exactly "Move", so any Move with
 * another hostname -- one renamed in Move's own settings, or one Avahi
 * renamed to "move-2" because a second Move is on the network -- was
 * subscribed to and then dropped on the floor: no slot, no audio in
 * /schwung-link-in, and the shim silently fell back to Move's own mix.
 * Move->Schwung did nothing, slot FX and Master FX never touched Move's
 * tracks, and nothing on the device said why. Measured 2026-11-14 by renaming
 * a working Move to move3: the peer became "Move3" and slot 0 went from ~2320
 * callbacks per 5 s to 32.
 *
 * The match is now against the device's own hostname, NORMALISED (lowercase,
 * letters and digits only) on both sides, so "move-2" matches "Move 2",
 * "Move-2" or "move2" -- whichever form Move announces. That also picks the
 * LOCAL Move out of a network with two, which the old substring filter
 * ("Move" anywhere in the name) could not: it subscribed to both.
 *
 * Fallback, when no peer matches the hostname (a name Move transforms in a way
 * the normalisation does not undo): if exactly ONE peer publishes Move-shaped
 * track channels, it is ours. With two such peers and no hostname match there
 * is no safe answer, so nothing is chosen rather than another device's tracks.
 *
 * Pure C, no allocation, so tests/host can drive it.
 */
#ifndef LINK_MOVE_PEER_H
#define LINK_MOVE_PEER_H

#include <stddef.h>
#include <string.h>

/* Lowercase letters and digits only; everything else dropped. */
static inline void lmp_norm(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) return;
    for (; in && *in && n + 1 < cap; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = c;
    }
    out[n] = '\0';
}

/* 1 when peer_name is the Move announced by a host called hostname. An empty
 * or missing hostname falls back to "move", the factory name. */
static inline int lmp_peer_is_host(const char *peer_name, const char *hostname)
{
    char a[96], b[96];
    lmp_norm(peer_name, a, sizeof(a));
    lmp_norm((hostname && *hostname) ? hostname : "move", b, sizeof(b));
    return a[0] != '\0' && strcmp(a, b) == 0;
}

/* Slot index (0-3) of a Move track channel: "1-MIDI", "2-Audio", ...
 * -1 for anything else, including "Main". */
static inline int lmp_track_slot(const char *channel_name)
{
    if (!channel_name) return -1;
    char d = channel_name[0];
    if (d >= '1' && d <= '4' && channel_name[1] == '-') return d - '1';
    return -1;
}

/* Given every channel in the session (parallel arrays, n entries), return the
 * index of one channel belonging to the peer that is this device's Move, or -1
 * when there is none / it is ambiguous. Callers then keep every channel whose
 * peer_names[i] equals peer_names[result]. */
static inline int lmp_choose_peer(const char *const *peer_names,
                                  const char *const *channel_names,
                                  int n, const char *hostname)
{
    for (int i = 0; i < n; i++)
        if (lmp_peer_is_host(peer_names[i], hostname)) return i;

    int found = -1;
    for (int i = 0; i < n; i++) {
        if (lmp_track_slot(channel_names[i]) < 0) continue;
        if (found < 0) { found = i; continue; }
        if (strcmp(peer_names[i], peer_names[found]) != 0) return -1; /* two Moves */
    }
    return found;
}

#endif /* LINK_MOVE_PEER_H */
