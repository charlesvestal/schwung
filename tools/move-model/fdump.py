# generic flip-tree dumper: path -> value, for diffing
import json, re, sys, time
from fast import *
CL = json.load(open('/data/UserData/schwung/classes.json'))
def members_of(cn):
    out = []
    while cn and cn in CL:
        out = CL[cn]['members'] + out; cn = CL[cn]['super']
    return out
def flipname(p):
    try: t = tname(p)
    except Exception: return None
    mm = re.match(r'N7ableton10flip_model\d+F(\w+)E$', t)
    return 'live.' + mm.group(1) if mm else t
def dump(o, cn, path, out, depth=0):
    if depth > 14: return
    seen = set()
    for mn, t, off in members_of(cn):
        if off is None or (mn, off) in seen: continue
        seen.add((mn, off)); a = o + off; p = path + '.' + mn
        try:
            if t == 'flip.Float': out[p] = round(f(a), 6)
            elif t in ('flip.Int', 'flip.Enum'): out[p] = struct.unpack('<q', rd(a + VAL, 8))[0]
            elif t == 'flip.Bool': out[p] = b(a)
            elif t == 'flip.Blob':
                s, e = q(a + VAL), q(a + VAL + 8)
                v = rd(s, e - s) if s and 0 <= e - s < 65536 else b''
                out[p] = v.decode('latin1') if v and all(32 <= c < 127 or c in (9, 10, 13) for c in v) else v.hex()
            elif t == 'flip.ObjectRef': out[p] = 'ref:%x' % q(a + VAL + 24)
            elif t in ('flip.Array', 'flip.Collection'):
                for k, e in enumerate(elems(a)):
                    en = flipname(e)
                    out[p + '[%d]' % k] = en + ' #%x' % objid(e)
                    if en in CL: dump(e, en, p + '[%d]' % k, out, depth + 1)
            elif t == 'flip.Message': pass
            elif t in CL: dump(a, t, p, out, depth + 1)
            else: out[p] = '?' + t
        except Exception as ex:
            out[p] = 'ERR %s' % ex
    return out
def song():
    return dump(C['song'], 'live.Song', 'song', {})
if __name__ == '__main__':
    t0 = time.time(); d = song()
    json.dump(d, open(sys.argv[1] if len(sys.argv) > 1 else 'dump.json', 'w'), indent=0)
    print(len(d), 'keys', '%.0f ms' % ((time.time() - t0) * 1000))
