import time, sys
from fast import *
S = C['song']
TM = m(m(S, 'live.Song.mTransport'), 'live.Transport.mTransportControlMessage')
tl = m(S, 'live.Song.mTracks'); tracks = elems(m(tl, 'live.TrackList.mTracks'))
pss = []
for t in tracks[:2]:
    cl = [c for c in elems(m(t, 'live.Track.mComponents')) if tname(c).endswith('6FClipsE')][0]
    pss.append(m(cl, 'live.Clips.mPlayingState'))
t0 = time.time(); last = None; dur = float(sys.argv[1])
while time.time() - t0 < dur:
    play = q(TM + 0xb0); st = struct.unpack('<d', rd(TM + 0x158, 8))[0]
    s = tuple((i(m(p, 'live.PlayingState.mMode')), refv(m(p, 'live.PlayingState.mPlayingClipSlot')) & 0xffff, f(m(p, 'live.PlayingState.mSessionClipStartTime'))) for p in pss)
    if (s, play) != last: print('%6.3f play=%d song=%8.4f %s' % (time.time() - t0, play, st, s), flush=True); last = (s, play)
    time.sleep(0.002)
