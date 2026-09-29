from fx import *
from flipcls import cls_by_name, all_members
VAL = 0x68
_off = {}
def off(cls, member):
    k = (cls, member)
    if k not in _off:
        for n, t, o, _ in all_members(cls_by_name(cls)):
            _off[(cls, n)] = o
    return _off[k]
def m(obj, cls, member): return obj + off(cls, member)
def f(a): return d(a + VAL)
def i(a): return q(a + VAL)
def b(a): return rd(a + 0x64, 1)[0]
def blob(a):
    s, e = q(a + VAL), q(a + VAL + 8)
    return rd(s, e - s) if e and s and 0 <= e - s < 1 << 20 else b''
def tree(root_hdr):
    begin, root, size = q(root_hdr), q(root_hdr + 8), q(root_hdr + 16)
    end_node = root_hdr + 8
    out = []; n = begin; guard = 0
    while n and n != end_node and guard < 100000:
        out.append(n); guard += 1
        r = q(n + 8)
        if r:
            n = r
            while q(n): n = q(n)
        else:
            p = q(n + 16)
            while p and q(p) != n:
                n = p; p = q(n + 16)
            n = p
    return out, size
def is_obj(p):
    if not p or BASE <= p < BASE + 0x2000000 or p & 7: return False
    v = q(p)
    return v is not None and BASE <= v < BASE + 0x2000000
def array(a): return collection(a)
def collection(a):
    nodes, size = tree(a + VAL)
    out = []
    for n in nodes:
        # KeyRandom size unknown: first word after the key that points at an object
        for k in range(32, 32 + 96, 8):
            p = q(n + k)
            if p and is_obj(p):
                out.append(p); break
    return out, size
def tname(p):
    try:
        vp = q(p); ti = q(vp - 8); return cstr(q(ti + 8))
    except Exception: return None
