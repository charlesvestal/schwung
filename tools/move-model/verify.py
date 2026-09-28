# compare in-memory model with a Song.abl on disk: per clip region/loop/scroll/notes
import json, sys, struct, re
d = json.load(open('z.json')); song = json.load(open(sys.argv[1]))
def mk(ti, si, rest):
    ks = [k for k in d if re.match(r'song\.mTracks\.mTracks\[%d\]\.mComponents\[\d\]\.mClipSlots\[%d\]\.mClip\[0\]\.mClip\.%s$' % (ti, si, re.escape(rest)), k)]
    return d[ks[0]] if ks else None
def notes_from_blob(h):
    b = bytes.fromhex(h); out = []
    # record: int32 note, int32 pad, f64 start, f64 dur, f32 vel, f32 offvel, i64 id  (40 bytes) -- guessed, verified below
    for o in range(0, len(b) // 40 * 40, 40):
        n, _, st, du, ve, ov, idn = struct.unpack('>iiddffq', b[o:o + 40])
        out.append((n, round(st, 6), round(du, 6), round(ve, 3)))
    return sorted(out)
ok = bad = 0
for ti, t in enumerate(song['tracks']):
    for si, cs in enumerate(t['clipSlots']):
        c = cs.get('clip')
        got = mk(ti, si, 'mClipRegion.mEnd')
        if not c:
            if got is not None: print('EXTRA in memory T%d s%d' % (ti + 1, si + 1)); bad += 1
            continue
        r = c['region']
        pairs = [('region.start', r['start'], mk(ti, si, 'mClipRegion.mStart')), ('region.end', r['end'], got),
                 ('loop.start', r['loop']['start'], mk(ti, si, 'mClipRegion.mLoop.mStart')),
                 ('loop.end', r['loop']['end'], mk(ti, si, 'mClipRegion.mLoop.mEnd')),
                 ('scroll', c.get('stepEditorScrollPosition'), mk(ti, si, 'mContent[0].mStepEditorScrollPosition'))]
        blob = mk(ti, si, 'mContent[0].mNotes')
        fn = sorted((n['noteNumber'], round(n['startTime'], 6), round(n['duration'], 6), round(n['velocity'], 3)) for n in c['notes'])
        pairs.append(('notes', fn, notes_from_blob(blob) if blob else []))
        for name, want, have in pairs:
            if (want == have) or (isinstance(want, float) and have is not None and abs(want - have) < 1e-6): ok += 1
            else:
                bad += 1; print('T%d s%d %s file=%r mem=%r' % (ti + 1, si + 1, name, str(want)[:80], str(have)[:80]))
print('ok', ok, 'bad', bad)
