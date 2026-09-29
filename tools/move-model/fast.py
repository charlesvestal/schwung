import json, struct, os, sys, time
C = json.load(open('/data/UserData/schwung/offs.json'))
PID = int(os.popen('pidof MoveOriginal').read().split()[0]); assert PID == C['pid'], 'Move restarted: rerun cache.py'
MEM = open('/proc/%d/mem' % PID, 'rb', 0); BASE = C['base']; O = C['offs']; VAL = 0x68
def rd(a, n): MEM.seek(a); return MEM.read(n)
def q(a): return struct.unpack('<Q', rd(a, 8))[0]
def m(o, key): return o + O[key]
def f(a): return struct.unpack('<d', rd(a + VAL, 8))[0]
def i(a): return q(a + VAL)
def b(a): return rd(a + 0x64, 1)[0]
def refv(a): return q(a + VAL + 8 + 16)          # ObjectRef value obj_id
def objid(o): return q(o + 0x28)
def is_obj(p):
    if not p or BASE <= p < BASE + 0x2000000 or p & 7 or p < 0x1000000 or p >= 0x8000000000: return False
    try: v = q(p)
    except OSError: return False
    return BASE <= v < BASE + 0x2000000
def elems(a):
    hdr = a + VAL; begin, size = q(hdr), q(hdr + 16); end = hdr + 8
    out = []; n = begin; g = 0
    while n and n != end and g < 100000:
        for k in range(32, 128, 8):
            p = q(n + k)
            if is_obj(p): out.append(p); break
        g += 1; r = q(n + 8)
        if r:
            n = r
            while q(n): n = q(n)
        else:
            p = q(n + 16)
            while p and q(p) != n: n = p; p = q(n + 16)
            n = p
    return out
def tname(p):
    vp = q(p); ti = q(vp - 8); s = rd(q(ti + 8), 64); return s.split(b'\0')[0].decode()
def snapshot():
    S = C['song']; res = {}
    tl = m(S, 'live.Song.mTracks')
    for ti, t in enumerate(elems(m(tl, 'live.TrackList.mTracks'))):
        tr = {'sel': b(m(t, 'live.Track.mIsSelected'))}
        clips = [c for c in elems(m(t, 'live.Track.mComponents')) if tname(c).endswith('6FClipsE')]
        if clips:
            Cc = clips[0]; ps = m(Cc, 'live.Clips.mPlayingState')
            tr['mode'] = i(m(ps, 'live.PlayingState.mMode'))
            tr['playing_ref'] = refv(m(ps, 'live.PlayingState.mPlayingClipSlot'))
            tr['start'] = f(m(ps, 'live.PlayingState.mSessionClipStartTime'))
            slots = []
            for si, s in enumerate(elems(m(Cc, 'live.Clips.mClipSlots'))):
                sc = elems(m(s, 'live.ClipSlot.mClip'))
                if not sc: continue
                cl = m(sc[0], 'live.SessionClip.mClip'); rg = m(cl, 'live.Clip.mClipRegion'); lp = m(rg, 'live.ClipRegion.mLoop')
                slots.append((si + 1, objid(s) == tr['playing_ref'], f(m(rg, 'live.ClipRegion.mStart')), f(m(rg, 'live.ClipRegion.mEnd')),
                              f(m(lp, 'live.Loop.mStart')), f(m(lp, 'live.Loop.mEnd')), b(m(lp, 'live.Loop.mIsEnabled'))))
            tr['clips'] = slots
        res['T%d' % (ti + 1)] = tr
    return res
def fmt(s):
    out = []
    for k, t in s.items():
        cl = ' '.join('s%d%s[%g..%g|L%g..%g]' % (c[0], '*' if c[1] else '', c[2], c[3], c[4], c[5]) for c in t.get('clips', []))
        out.append('%s%s m%d st=%g %s' % (k, '<' if t['sel'] else ' ', t.get('mode', -1), t.get('start', -1), cl))
    return ' | '.join(out)
if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'watch':
        dur = float(sys.argv[2]) if len(sys.argv) > 2 else 20; t0 = time.time(); last = None
        while time.time() - t0 < dur:
            s = fmt(snapshot())
            if s != last: print('%6.2f %s' % (time.time() - t0, s), flush=True); last = s
            time.sleep(0.05)
    else:
        t0 = time.time(); s = snapshot(); print('%.1f ms' % ((time.time() - t0) * 1000)); print(fmt(s))
