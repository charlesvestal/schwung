#!/usr/bin/env python3
"""Regenerate tools/param-pages/font5x7.json from the deployed font atlas.

The headless preview harness renders text by replicating the device's
`print()` exactly, so previews are pixel-identical to the OLED rather than an
approximation. The device loads /data/UserData/schwung/host/font.png via
js_display_load_font(); this script mirrors that loader's behaviour —
auto-trim blank columns per cell, charSpacing 1, blank cells keep the full
cell width so the cursor still advances.

Run after any change to the font:

    python3 scripts/generate_font.py --deploy-png build/host/font.png
    python3 tools/param-pages/gen_font_table.py

Requires Pillow (only for this regeneration step — the harness itself is
node-only and reads the checked-in JSON).
"""
import hashlib
import json
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PNG = os.path.join(ROOT, "build", "host", "font.png")
OUT = os.path.join(ROOT, "tools", "param-pages", "font5x7.json")


def main():
    if not os.path.exists(PNG):
        sys.exit(f"{PNG} not found — run scripts/build.sh (or generate_font.py) first")

    img = Image.open(PNG).convert("RGBA")
    chars = open(PNG + ".dat", encoding="utf-8").read().rstrip("\n")
    width, height = img.size
    cell_w = width // len(chars)
    px = img.load()

    glyphs = {}
    for i, ch in enumerate(chars):
        x0 = i * cell_w
        start_x = end_x = -1
        for x in range(cell_w):
            for y in range(height):
                if px[x0 + x, y][3] > 0:
                    if start_x == -1:
                        start_x = x
                    end_x = x
                    break
        if start_x == -1:
            # Blank glyph (space): full cell width so the cursor advances.
            glyphs[ch] = {"w": cell_w, "rows": [0] * height}
            continue
        gw = end_x - start_x + 1
        rows = []
        for y in range(height):
            bits = 0
            for x in range(gw):
                if px[x0 + start_x + x, y][3] > 0:
                    bits |= 1 << x          # bit0 = leftmost pixel
            rows.append(bits)
        glyphs[ch] = {"w": gw, "rows": rows}

    out = {
        "_note": "Generated from build/host/font.png by tools/param-pages/gen_font_table.py. "
                 "Mirrors js_display_load_font's auto-trim so the harness renders text "
                 "pixel-identically to the device.",
        "height": height,
        "cellWidth": cell_w,
        "charSpacing": 1,
        "source_md5": hashlib.md5(open(PNG, "rb").read()).hexdigest(),
        "glyphs": glyphs,
    }
    with open(OUT, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    print(f"wrote {OUT} ({len(glyphs)} glyphs, cell {cell_w}x{height})")


if __name__ == "__main__":
    main()
