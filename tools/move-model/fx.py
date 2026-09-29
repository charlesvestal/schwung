import os, struct, re, sys
def pid():
    return int(os.popen('pidof MoveOriginal').read().split()[0])
PID = pid()
MEM = open('/proc/%d/mem' % PID, 'rb', 0)
def maps():
    out = []
    for l in open('/proc/%d/maps' % PID):
        p = l.split()
        a, b = [int(x, 16) for x in p[0].split('-')]
        out.append((a, b, p[1], int(p[2], 16), p[5] if len(p) > 5 else ''))
    return out
MAPS = maps()
BASE = min(a for a, b, perm, off, name in MAPS if name.endswith('MoveOriginal'))
def rd(addr, n):
    try:
        MEM.seek(addr); return MEM.read(n)
    except Exception:
        return None
def q(addr):
    b = rd(addr, 8); return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None
def d(addr):
    b = rd(addr, 8); return struct.unpack('<d', b)[0] if b and len(b) == 8 else None
def cstr(addr, n=128):
    b = rd(addr, n)
    if not b: return None
    return b.split(b'\0')[0].decode('latin1')
def rw_regions(heap=True, bss=True, anon=False):
    for a, b, perm, off, name in MAPS:
        if 'rw' not in perm: continue
        if name.endswith('MoveOriginal') and bss: yield a, b
        elif name == '[heap]' and heap: yield a, b
        elif anon and name == '' : yield a, b
    # bss continuation (anon right after MoveOriginal rw)
    if bss:
        prev = None
        for a, b, perm, off, name in MAPS:
            if prev and prev[4].endswith('MoveOriginal') and name == '' and a == prev[1] and 'rw' in perm:
                yield a, b
            prev = (a, b, perm, off, name)
def find_q(val, regions, limit=1000, align=8):
    pat = struct.pack('<Q', val); hits = []
    for a, b in regions:
        CH = 1 << 24
        for s in range(a, b, CH):
            buf = rd(s, min(CH, b - s))
            if not buf: continue
            i = buf.find(pat)
            while i >= 0:
                if (s + i) % align == 0:
                    hits.append(s + i)
                    if len(hits) >= limit: return hits
                i = buf.find(pat, i + 1)
    return hits
def find_bytes(pat, regions, limit=100):
    hits = []
    for a, b in regions:
        CH = 1 << 24
        for s in range(a, b, CH):
            buf = rd(s, min(CH, b - s) + len(pat))
            if not buf: continue
            i = buf.find(pat)
            while i >= 0:
                hits.append(s + i)
                if len(hits) >= limit: return hits
                i = buf.find(pat, i + 1)
    return hits
def text_regions():
    for a, b, perm, off, name in MAPS:
        if name.endswith('MoveOriginal') and 'r' in perm: yield a, b
