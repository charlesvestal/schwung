/* link_move_peer.h: the sidecar finds THIS device's Move by its hostname, so a
 * renamed Move ("move3" announces as "Move3", "move-2" after an mDNS clash)
 * still feeds Move->Schwung, and a second Move on the network is never
 * mistaken for ours. Before this, only a peer named exactly "Move" got a slot. */
#include <stdio.h>
#include "link_move_peer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

int main(void)
{
    /* The factory name, and the renamed forms measured or plausible. */
    CHECK(lmp_peer_is_host("Move", "move"));
    CHECK(lmp_peer_is_host("Move3", "move3"));         /* measured 2026-11-14 */
    CHECK(lmp_peer_is_host("Move 2", "move-2"));
    CHECK(lmp_peer_is_host("Move-2", "move-2"));
    CHECK(lmp_peer_is_host("Move", ""));               /* no hostname: factory */
    CHECK(lmp_peer_is_host("Move", NULL));
    CHECK(!lmp_peer_is_host("Move", "move3"));         /* the OTHER Move */
    CHECK(!lmp_peer_is_host("Move3", "move"));
    CHECK(!lmp_peer_is_host("", "move"));

    CHECK(lmp_track_slot("1-MIDI") == 0);
    CHECK(lmp_track_slot("4-Audio") == 3);
    CHECK(lmp_track_slot("Main") == -1);
    CHECK(lmp_track_slot("5-MIDI") == -1);
    CHECK(lmp_track_slot("ME-1") == -1);
    CHECK(lmp_track_slot("") == -1);

    /* The bug: a renamed Move, alone. The old rule chose nothing. */
    { const char *p[] = {"Move3", "Move3", "Move3", "Move3", "Move3"};
      const char *c[] = {"1-MIDI", "2-MIDI", "3-MIDI", "4-MIDI", "Main"};
      int i = lmp_choose_peer(p, c, N(p), "move3");
      CHECK(i >= 0 && !strcmp(p[i], "Move3")); }

    /* Two Moves on one network: ours wins by hostname, either way round. */
    { const char *p[] = {"Move", "Move", "Move 2", "Move 2"};
      const char *c[] = {"1-MIDI", "2-MIDI", "1-MIDI", "2-MIDI"};
      int i = lmp_choose_peer(p, c, N(p), "move-2");
      CHECK(i >= 0 && !strcmp(p[i], "Move 2"));
      i = lmp_choose_peer(p, c, N(p), "move");
      CHECK(i >= 0 && !strcmp(p[i], "Move")); }

    /* No hostname match, one Move-shaped peer: it is ours. */
    { const char *p[] = {"Studio Box", "Studio Box", "Live"};
      const char *c[] = {"1-MIDI", "Main", "Master"};
      int i = lmp_choose_peer(p, c, N(p), "studio-mv");
      CHECK(i >= 0 && !strcmp(p[i], "Studio Box")); }

    /* No hostname match, TWO Move-shaped peers: refuse rather than guess. */
    { const char *p[] = {"A", "B"};
      const char *c[] = {"1-MIDI", "1-MIDI"};
      CHECK(lmp_choose_peer(p, c, N(p), "zzz") == -1); }

    /* Nothing Move-shaped at all (e.g. only Live on the network). */
    { const char *p[] = {"Live"};
      const char *c[] = {"Master"};
      CHECK(lmp_choose_peer(p, c, N(p), "move") == -1); }
    CHECK(lmp_choose_peer(NULL, NULL, 0, "move") == -1);

    if (failures) { printf("test_link_move_peer: %d failure(s)\n", failures); return 1; }
    printf("test_link_move_peer: PASS\n");
    return 0;
}
