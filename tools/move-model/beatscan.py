import struct, time, array, sys
from fx import *
regs = [(a, b) for a, b, perm, off, name in MAPS if 'rw' in perm and (name in ('', '[heap]') or name.endswith('MoveOriginal'))]
bps = float(sys.argv[1]) / 60.0
cands = []
CH = 16 << 20
for a, b in regs:
    for s in range(a, b, CH):
        n = min(CH, b - s)
        ta = time.time(); x = rd(s, n); time.sleep(0.2); tb = time.time(); y = rd(s, n)
        if not x or not y or len(x) != len(y): continue
        want = (tb - ta) * bps
        X = array.array('d'); X.frombytes(x[:len(x) // 8 * 8]); Y = array.array('d'); Y.frombytes(y[:len(y) // 8 * 8])
        for k in range(len(X)):
            if X[k] != Y[k]:
                u, v = X[k], Y[k]
                if 0.0 <= u < 100000.0 and 0.0 < v < 100000.0:
                    dv = v - u
                    if want * 0.6 < dv < want * 1.4: cands.append((s + 8 * k, u, v, dv / want))
print(len(cands), flush=True)
for c in cands[:80]: print('%x %.4f -> %.4f ratio %.3f' % c)
