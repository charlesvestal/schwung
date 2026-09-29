# one pass: find every flip ClassBase (vptr, name_ptr->"live.*"/"flip.*") in the first 64MB of heap
import json, struct
from fx import *
from flipcls import members, cls_name
heap = [(a, b) for a, b, perm, off, name in MAPS if name == '[heap]'][0]
lo, hi = BASE, BASE + 0x2000000
buf = rd(heap[0], min(64 << 20, heap[1] - heap[0]))
vals = struct.unpack('<%dQ' % (len(buf) // 8), buf[:len(buf) // 8 * 8])
cands = {}
for k in range(1, len(vals) - 8):
    p = vals[k]
    if lo <= p < hi and lo <= vals[k - 1] < hi:
        cands[heap[0] + (k - 1) * 8] = p
out = {}
for c, sp in cands.items():
    s = cstr(sp, 80)
    if not s or not (s.startswith('live.') or s.startswith('flip.')) or ' ' in s: continue
    try:
        sup = q(c + 0x10)
        mem = members(c)
        out[s] = {'addr': c, 'super': cls_name(sup) if sup else None,
                  'members': [(mn, t, o if isinstance(o, int) else None) for mn, t, o, _ in mem]}
    except Exception as e:
        pass
json.dump(out, open('classes.json', 'w'), indent=0)
print(len(out), sorted(out)[:10])
