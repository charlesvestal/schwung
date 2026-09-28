from fx import *
import struct
def sso(a):
    b = rd(a, 24)
    if b[0] & 1:
        size, ptr = struct.unpack('<QQ', b[8:24]); return rd(ptr, size).decode('latin1')
    return b[1:1 + (b[0] >> 1)].decode('latin1')
def stub_off(fn):
    # decode leading "add x0, x0, #imm{,lsl12}" ; ret, or "ret" (offset 0)
    ws = struct.unpack('<4I', rd(fn, 16))
    off = 0
    for w in ws:
        if w == 0xd65f03c0: return off
        if (w & 0x7f80001f) == 0x52800008:  # movz w8/x8,#imm16{,lsl}
            pend = ((w >> 5) & 0xffff) << (16 * ((w >> 21) & 3)); continue
        if w == 0x8b080000:  # add x0,x0,x8
            off += pend; continue
        if (w & 0xff8003ff) == 0x91000000:  # add x0,x0,#imm
            imm = (w >> 10) & 0xfff
            if w & (1 << 22): imm <<= 12
            off += imm; continue
        return ('?', ['%08x' % x for x in ws])
    return ('?', ['%08x' % x for x in ws])
_cls_cache = {}
def cls_by_name(name):
    if name in _cls_cache: return _cls_cache[name]
    s = find_bytes(name.encode() + b'\0', text_regions(), 1)
    if not s: return None
    r = find_q(s[0], rw_regions(anon=True), 4)
    c = r[0] - 8 if r else None
    _cls_cache[name] = c; return c
def cls_name(c): return cstr(q(c + 8))
def members(c):
    b, e = q(c + 0x20), q(c + 0x28); out = []
    for m in range(b, e, 48):
        out.append((sso(m + 8), cls_name(q(m)), stub_off(q(m + 40)), q(m)))
    return out
def all_members(c):
    sup = q(c + 0x10)
    res = all_members(sup) if sup else []
    return res + members(c)
if __name__ == '__main__':
    import sys
    for nm in sys.argv[1:]:
        c = cls_by_name(nm)
        print('%s  cls=%x super=%s sz=%s' % (nm, c, cls_name(q(c+0x10))  if q(c+0x10) else None, q(c+0x18)))
        for n, t, off, _ in all_members(c):
            print('   %-30s %-24s off=%s' % (n, t, off if not isinstance(off, int) else hex(off)))
