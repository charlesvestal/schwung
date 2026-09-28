from fx import *
def relro():
    for a, b, perm, off, name in MAPS:
        if name.endswith('MoveOriginal') and 'x' not in perm and off > 0x1000000: yield a, b
def vptrs(mangled):
    s = find_bytes(mangled.encode() + b'\0', text_regions(), 3)
    out = []
    for sa in s:
        if rd(sa - 1, 1) not in (b'\0',): continue
        for ti_name in find_q(sa, relro(), 10):
            ti = ti_name - 8
            for h in find_q(ti, relro(), 20):
                out.append(h + 8)
    return out
def instances(mangled, limit=200):
    res = {}
    for vp in vptrs(mangled):
        res[vp] = find_q(vp, rw_regions(anon=True), limit)
    return res
if __name__ == '__main__':
    import sys
    for m in sys.argv[1:]:
        for vp, inst in instances(m).items():
            print(m, 'vptr=img+%x' % (vp - BASE), 'n=%d' % len(inst), [hex(x) for x in inst[:6]])
