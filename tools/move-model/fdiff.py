import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
for k in sorted(set(a) | set(b)):
    if a.get(k) != b.get(k): print('%-90s %r -> %r' % (k, a.get(k), b.get(k)))
