import sys
from fx import *
from flipcls import sso
name = sys.argv[1]
s = find_bytes(b'\0' + name.encode() + b'\0', text_regions(), 3)
for sa in s:
    sa += 1
    refs = find_q(sa, rw_regions(anon=False), 5)
    print('str', hex(sa), 'refs', [hex(r) for r in refs])
    for r in refs:
        for i in range(-2, 10): 
            v = q(r + 8 * i); print('   +%d %016x %s' % (8 * i, v, cstr(v, 40) if v and BASE <= v < BASE + 0x2000000 else ''))
