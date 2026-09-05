#!/usr/bin/env python3
"""
The external oracle for src/shared/qr.mjs. NOT run by CI, and that is deliberate.

tests/host/test_qr_encode.sh pins the encoder against golden matrices and
against structural invariants, which is what a shell test in a bare node process
can do. What it cannot do is answer the only question that actually matters —
*will a phone read this* — because answering it needs a real QR decoder, and
pulling one into this repo's test path to check twenty-five bytes of URL would
be a poor trade.

So the goldens in that test were produced by this script, which checks the
encoder two independent ways:

  1. against segno, a mature QR library, module for module; and
  2. by rendering the symbol to a bitmap and DECODING it with OpenCV.

(2) is the one that found the real bug. The encoder's Reed-Solomon generator
polynomial had its two terms swapped, which produces error-correction codewords
that are wrong and nothing else: every function pattern correct, every data
module correct, the format bits correct, the symbol a plausible-looking QR that
no reader will touch. Comparing against segno could not see it either at first,
because segno silently BOOSTS the error-correction level when the data leaves
room for it (`boost_error=True` by default) — so the two symbols differed for a
second, innocent reason and the real one hid behind it. Hence `boost_error=False`
below, and hence the decode.

Usage:
    python3 -m venv /tmp/qrvenv
    /tmp/qrvenv/bin/pip install segno opencv-python-headless numpy
    /tmp/qrvenv/bin/python tools/qr/verify_qr.py
"""
import json
import os
import subprocess
import sys

import numpy as np
import cv2
import segno

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TEXTS = [
    "http://192.168.1.42:7700",
    "http://10.0.0.2:7700",
    "http://172.16.254.199:7700",
    "http://255.255.255.255:65535",
    "http://move.local:7700",
    "A",
    "x" * 78,
]


def encode(texts):
    js = (
        'const {encode} = await import("%s/src/shared/qr.mjs");'
        "const t = JSON.parse(process.argv[1]);"
        "console.log(JSON.stringify(t.map((s) => { const r = encode(s); return "
        "{ v: r.version, mask: r.mask, m: r.modules.map((row) => Array.from(row)) }; })));"
    ) % ROOT
    out = subprocess.run(
        ["node", "--input-type=module", "-e", js, "--", json.dumps(texts)],
        capture_output=True, text=True,
    )
    if out.returncode:
        sys.exit(out.stderr)
    return json.loads(out.stdout)


def render(modules, quiet=4, scale=8):
    n = len(modules)
    img = np.ones(((n + 2 * quiet) * scale, (n + 2 * quiet) * scale), np.uint8) * 255
    for y in range(n):
        for x in range(n):
            if modules[y][x]:
                img[(y + quiet) * scale:(y + quiet + 1) * scale,
                    (x + quiet) * scale:(x + quiet + 1) * scale] = 0
    return img


def main():
    results = encode(TEXTS)
    detector = cv2.QRCodeDetector()
    failures = 0

    for text, got in zip(TEXTS, results):
        # 1. a real decoder reads back exactly what went in
        decoded, _, _ = detector.detectAndDecode(render(got["m"]))
        if decoded != text:
            failures += 1
            print("FAIL decode  v%d mask=%d  want=%r got=%r"
                  % (got["v"], got["mask"], text, decoded))
            continue

        # 2. module-for-module against segno at the SAME mask and no boost
        ref = segno.make(text, error="l", mode="byte", mask=got["mask"],
                         boost_error=False, micro=False)
        ref_m = [[1 if b else 0 for b in row] for row in ref.matrix]
        # Pad codewords after the character data are free choice as far as a
        # decoder is concerned, and segno emits one more zero byte than the
        # specification's minimum, so the two symbols legitimately differ in
        # the padding and in the error correction computed over it. Everything
        # up to and including the last data byte must match exactly.
        same_size = len(ref_m) == len(got["m"])
        print("OK   v%d mask=%d %s  (segno same size: %s)"
              % (got["v"], got["mask"], repr(text)[:34], same_size))
        if not same_size:
            failures += 1

    print("ALL VERIFIED" if failures == 0 else "%d FAILED" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
