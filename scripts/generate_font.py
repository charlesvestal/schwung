#!/usr/bin/env python3
import sys
from PIL import Image, ImageDraw

# Oled 5x7 Pixel Font (adapted from: https://github.com/noopkat/oled-font-5x7/)
# Each character is defined as a list of 7 strings (rows), each 5 characters wide.
# '#' represents a pixel, '.' a blank space.

FONT = {
    ' ': [
        '.....',
        '.....',
        '.....',
        '.....',
        '.....',
        '.....',
        '.....',
    ],
    '!': [
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '.....',
        '..#..',
    ],
    '"': [
        '.#.#.',
        '.#.#.',
        '.#.#.',
        '.....',
        '.....',
        '.....',
        '.....',
    ],
    '#': [
        '.#.#.',
        '.#.#.',
        '#####',
        '.#.#.',
        '#####',
        '.#.#.',
        '.#.#.',
    ],
    '$': [
        '..#..',
        '.####',
        '#.#..',
        '.###.',
        '..#.#',
        '####.',
        '..#..',
    ],
    '%': [
        '##..#',
        '##.#.',
        '...#.',
        '..#..',
        '.#...',
        '.#.##',
        '#..##',
    ],
    '&': [
        '.##..',
        '#..#.',
        '#.#..',
        '.#...',
        '#.#.#',
        '#..#.',
        '.##.#',
    ],
    "'": [
        '.##..',
        '..#..',
        '.#...',
        '.....',
        '.....',
        '.....',
        '.....',
    ],
    '(': [
        '...#.',
        '..#..',
        '.#...',
        '.#...',
        '.#...',
        '..#..',
        '...#.',
    ],
    ')': [
        '.#...',
        '..#..',
        '...#.',
        '...#.',
        '...#.',
        '..#..',
        '.#...',
    ],
    '*': [
        '.....',
        '.#.#.',
        '..#..',
        '#####',
        '..#..',
        '.#.#.',
        '.....',
    ],
    '+': [
        '.....',
        '..#..',
        '..#..',
        '#####',
        '..#..',
        '..#..',
        '.....',
    ],
    ',': [
        '.....',
        '.....',
        '.....',
        '.....',
        '.##..',
        '..#..',
        '.#...',
    ],
    '-': [
        '.....',
        '.....',
        '.....',
        '#####',
        '.....',
        '.....',
        '.....',
    ],
    '.': [
        '.....',
        '.....',
        '.....',
        '.....',
        '.....',
        '.##..',
        '.##..',
    ],
    '/': [
        '....#',
        '...#.',
        '...#.',
        '..#..',
        '.#...',
        '.#...',
        '#....',
    ],
    '0': [
        '.###.',
        '#...#',
        '#..##',
        '#.#.#',
        '##..#',
        '#...#',
        '.###.',
    ],
    '1': [
        '..#..',
        '.##..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '.###.',
    ],
    '2': [
        '.###.',
        '#...#',
        '....#',
        '...#.',
        '..#..',
        '.#...',
        '#####',
    ],
    '3': [
        '#####',
        '...#.',
        '..#..',
        '...#.',
        '....#',
        '#...#',
        '.###.',
    ],
    '4': [
        '...#.',
        '..##.',
        '.#.#.',
        '#..#.',
        '#####',
        '...#.',
        '...#.',
    ],
    '5': [
        '#####',
        '#....',
        '####.',
        '....#',
        '....#',
        '#...#',
        '.###.',
    ],
    '6': [
        '..##.',
        '.#...',
        '#....',
        '####.',
        '#...#',
        '#...#',
        '.###.',
    ],
    '7': [
        '#####',
        '....#',
        '...#.',
        '..#..',
        '.#...',
        '.#...',
        '.#...',
    ],
    '8': [
        '.###.',
        '#...#',
        '#...#',
        '.###.',
        '#...#',
        '#...#',
        '.###.',
    ],
    '9': [
        '.###.',
        '#...#',
        '#...#',
        '.####',
        '....#',
        '...#.',
        '.##..',
    ],
    ':': [
        '.....',
        '.##..',
        '.##..',
        '.....',
        '.##..',
        '.##..',
        '.....',
    ],
    ';': [
        '.....',
        '.##..',
        '.##..',
        '.....',
        '.##..',
        '..#..',
        '.#...',
    ],
    '<': [
        '....#',
        '...#.',
        '..#..',
        '.#...',
        '..#..',
        '...#.',
        '....#',
    ],
    '=': [
        '.....',
        '.....',
        '#####',
        '.....',
        '#####',
        '.....',
        '.....',
    ],
    '>': [
        '#....',
        '.#...',
        '..#..',
        '...#.',
        '..#..',
        '.#...',
        '#....',
    ],
    '?': [
        '.###.',
        '#...#',
        '....#',
        '...#.',
        '..#..',
        '.....',
        '..#..',
    ],
    '@': [
        '.###.',
        '#...#',
        '....#',
        '.##.#',
        '#.#.#',
        '#.#.#',
        '.###.',
    ],
    'A': [
        '.###.',
        '#...#',
        '#...#',
        '#...#',
        '#####',
        '#...#',
        '#...#',
    ],
    'B': [
        '####.',
        '#...#',
        '#...#',
        '####.',
        '#...#',
        '#...#',
        '####.',
    ],
    'C': [
        '.###.',
        '#...#',
        '#....',
        '#....',
        '#....',
        '#...#',
        '.###.',
    ],
    'D': [
        '###..',
        '#..#.',
        '#...#',
        '#...#',
        '#...#',
        '#..#.',
        '###..',
    ],
    'E': [
        '#####',
        '#....',
        '#....',
        '####.',
        '#....',
        '#....',
        '#####',
    ],
    'F': [
        '#####',
        '#....',
        '#....',
        '###..',
        '#....',
        '#....',
        '#....',
    ],
    'G': [
        '.###.',
        '#...#',
        '#....',
        '#....',
        '#..##',
        '#...#',
        '.###.',
    ],
    'H': [
        '#...#',
        '#...#',
        '#...#',
        '#####',
        '#...#',
        '#...#',
        '#...#',
    ],
    'I': [
        '.###.',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '.###.',
    ],
    'J': [
        '..###',
        '...#.',
        '...#.',
        '...#.',
        '...#.',
        '#..#.',
        '.##..',
    ],
    'K': [
        '#...#',
        '#..#.',
        '#.#..',
        '##...',
        '#.#..',
        '#..#.',
        '#...#',
    ],
    'L': [
        '#....',
        '#....',
        '#....',
        '#....',
        '#....',
        '#....',
        '#####',
    ],
    'M': [
        '#...#',
        '##.##',
        '#.#.#',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
    ],
    'N': [
        '#...#',
        '#...#',
        '##..#',
        '#.#.#',
        '#..##',
        '#...#',
        '#...#',
    ],
    'O': [
        '.###.',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '.###.',
    ],
    'P': [
        '####.',
        '#...#',
        '#...#',
        '####.',
        '#....',
        '#....',
        '#....',
    ],
    'Q': [
        '.###.',
        '#...#',
        '#...#',
        '#...#',
        '#.#.#',
        '#..#.',
        '.##.#',
    ],
    'R': [
        '####.',
        '#...#',
        '#...#',
        '####.',
        '#.#..',
        '#..#.',
        '#...#',
    ],
    'S': [
        '.####',
        '#....',
        '#....',
        '.###.',
        '....#',
        '....#',
        '####.',
    ],
    'T': [
        '#####',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
    ],
    'U': [
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '.###.',
    ],
    'V': [
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '.#.#.',
        '..#..',
    ],
    'W': [
        '#...#',
        '#...#',
        '#...#',
        '#.#.#',
        '#.#.#',
        '##.##',
        '#...#',
    ],
    'X': [
        '#...#',
        '#...#',
        '.#.#.',
        '..#..',
        '.#.#.',
        '#...#',
        '#...#',
    ],
    'Y': [
        '#...#',
        '#...#',
        '.#.#.',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
    ],
    'Z': [
        '#####',
        '....#',
        '...#.',
        '..#..',
        '.#...',
        '#....',
        '#####',
    ],
    'Ä': [
        '#..#.',
        '.##..',
        '#..#.',
        '#..#.',
        '####.',
        '#..#.',
        '#..#.',
    ],
    'Ö': [
        '#...#',
        '.###.',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '.###.',
    ],
    'Ü': [
        '#...#',
        '.....',
        '#...#',
        '#...#',
        '#...#',
        '#...#',
        '.###.',
    ],
    '[': [
        '..###',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..###',
    ],
    '\\': [
        '#....',
        '#....',
        '.#...',
        '..#..',
        '...#.',
        '....#',
        '....#',
    ],
    ']': [
        '###..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '###..',
    ],
    '^': [
        '..#..',
        '.#.#.',
        '#...#',
        '.....',
        '.....',
        '.....',
        '.....',
    ],
    '_': [
        '.....',
        '.....',
        '.....',
        '.....',
        '.....',
        '.....',
        '#####',
    ],
    '`': [
        '.#...',
        '..#..',
        '...#.',
        '.....',
        '.....',
        '.....',
        '.....',
    ],
    'a': [
        '.....',
        '.....',
        '.###.',
        '....#',
        '.####',
        '#...#',
        '.####',
    ],
    'b': [
        '#....',
        '#....',
        '#.##.',
        '##..#',
        '#...#',
        '#...#',
        '####.',
    ],
    'c': [
        '.....',
        '.....',
        '.###.',
        '#....',
        '#....',
        '#....',
        '.###.',
    ],
    'd': [
        '....#',
        '....#',
        '.##.#',
        '#..##',
        '#...#',
        '#...#',
        '.####',
    ],
    'e': [
        '.....',
        '.....',
        '.###.',
        '#...#',
        '#####',
        '#....',
        '.###.',
    ],
    'f': [
        '..##.',
        '.#..#',
        '.#...',
        '###..',
        '.#...',
        '.#...',
        '.#...',
    ],
    'g': [
        '.....',
        '.....',
        '.####',
        '#...#',
        '.####',
        '....#',
        '..##.',
    ],
    'h': [
        '#....',
        '#....',
        '#.##.',
        '##..#',
        '#...#',
        '#...#',
        '#...#',
    ],
    'i': [
        '..#..',
        '.....',
        '.##..',
        '..#..',
        '..#..',
        '..#..',
        '.###.',
    ],
    'j': [
        '...#.',
        '.....',
        '..##.',
        '...#.',
        '...#.',
        '#..#.',
        '.##..',
    ],
    'k': [
        '.#...',
        '.#...',
        '.#..#',
        '.#.#.',
        '.##..',
        '.#.#.',
        '.#..#',
    ],
    'l': [
        '.##..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '.###.',
    ],
    'm': [
        '.....',
        '.....',
        '##.#.',
        '#.#.#',
        '#.#.#',
        '#...#',
        '#...#',
    ],
    'n': [
        '.....',
        '.....',
        '#.##.',
        '##..#',
        '#...#',
        '#...#',
        '#...#',
    ],
    'o': [
        '.....',
        '.....',
        '.###.',
        '#...#',
        '#...#',
        '#...#',
        '.###.',
    ],
    'p': [
        '.....',
        '.....',
        '####.',
        '#...#',
        '####.',
        '#....',
        '#....',
    ],
    'q': [
        '.....',
        '.....',
        '.##.#',
        '#..##',
        '.####',
        '....#',
        '....#',
    ],
    'r': [
        '.....',
        '.....',
        '#.##.',
        '##..#',
        '#....',
        '#....',
        '#....',
    ],
    's': [
        '.....',
        '.....',
        '.###.',
        '#....',
        '.###.',
        '....#',
        '####.',
    ],
    't': [
        '.#...',
        '.#...',
        '###..',
        '.#...',
        '.#...',
        '.#..#',
        '..##.',
    ],
    'u': [
        '.....',
        '.....',
        '#...#',
        '#...#',
        '#...#',
        '#..##',
        '.##.#',
    ],
    'v': [
        '.....',
        '.....',
        '#...#',
        '#...#',
        '#...#',
        '.#.#.',
        '..#..',
    ],
    'w': [
        '.....',
        '.....',
        '#...#',
        '#...#',
        '#.#.#',
        '#.#.#',
        '.#.#.',
    ],
    'x': [
        '.....',
        '.....',
        '#...#',
        '.#.#.',
        '..#..',
        '.#.#.',
        '#...#',
    ],
    'y': [
        '.....',
        '.....',
        '#...#',
        '#...#',
        '.####',
        '....#',
        '.###.',
    ],
    'z': [
        '.....',
        '.....',
        '#####',
        '...#.',
        '..#..',
        '.#...',
        '#####',
    ],
    'ä': [
        '.#.#.',
        '.....',
        '.###.',
        '....#',
        '.####',
        '#...#',
        '.####',
    ],
    'ö': [
        '.....',
        '#..#.',
        '.##..',
        '#..#.',
        '#..#.',
        '#..#.',
        '.##..',
    ],
    'ü': [
        '.....',
        '#..#.',
        '.....',
        '#..#.',
        '#..#.',
        '#..#.',
        '.##..',
    ],
    '{': [
        '...#.',
        '..#..',
        '..#..',
        '.#...',
        '..#..',
        '..#..',
        '...#.',
    ],
    '|': [
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
        '..#..',
    ],
    '}': [
        '.#...',
        '..#..',
        '..#..',
        '...#.',
        '..#..',
        '..#..',
        '.#...',
    ],
    '~': [
        '.....',
        '.....',
        '.#...',
        '#.#.#',
        '...#.',
        '.....',
        '.....',
    ],
    '€': [
        '..##.',
        '.#..#',
        '###..',
        '.#...',
        '###..',
        '.#..#',
        '..##.',
    ],
    '†': [
        '.....',
        '..#..',
        '...#.',
        '#####',
        '...#.',
        '..#..',
        '.....',
    ],
    '‡': [
        '.....',
        '..#..',
        '.#...',
        '#####',
        '.#...',
        '..#..',
        '.....',
    ],
    '°': [
        '..###',
        '..#.#',
        '..###',
        '.....',
        '.....',
        '.....',
        '.....',
    ],
}

def parse_bdf(bdf_path):
    """Parse a BDF font file and extract glyph bitmaps for ASCII 32-126.

    Returns (char_width, char_height, glyphs) where glyphs is a dict mapping
    character -> list of row strings (same format as FONT: '#' for pixel,
    '.' for blank, each row char_width characters wide).
    """
    with open(bdf_path, 'r') as f:
        lines = f.readlines()

    # Parse FONTBOUNDINGBOX for cell dimensions
    cell_w, cell_h, cell_xoff, cell_yoff = None, None, None, None
    for line in lines:
        if line.startswith('FONTBOUNDINGBOX '):
            parts = line.split()
            cell_w = int(parts[1])
            cell_h = int(parts[2])
            cell_xoff = int(parts[3])
            cell_yoff = int(parts[4])
            break

    if cell_w is None or cell_h is None or cell_xoff is None or cell_yoff is None:
        raise ValueError("No FONTBOUNDINGBOX found in BDF file")

    glyphs = {}
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith('STARTCHAR'):
            encoding = None
            bbx_w, bbx_h, bbx_xoff, bbx_yoff = 0, 0, 0, 0
            bitmap_rows = []
            i += 1
            in_bitmap = False
            while i < len(lines):
                l = lines[i].strip()
                if l.startswith('ENCODING '):
                    encoding = int(l.split()[1])
                elif l.startswith('BBX '):
                    parts = l.split()
                    bbx_w = int(parts[1])
                    bbx_h = int(parts[2])
                    bbx_xoff = int(parts[3])
                    bbx_yoff = int(parts[4])
                elif l == 'BITMAP':
                    in_bitmap = True
                elif l == 'ENDCHAR':
                    break
                elif in_bitmap:
                    bitmap_rows.append(l)
                i += 1

            if encoding is not None and 32 <= encoding <= 126:
                ch = chr(encoding)
                # Build full cell bitmap
                cell = [['.' for _ in range(cell_w)] for _ in range(cell_h)]

                # Glyph placement within cell:
                # x offset relative to cell: bbx_xoff - cell_xoff
                # y offset: glyph top row in cell
                # The glyph baseline is at (cell_h + cell_yoff) from top of cell.
                # Glyph bottom = baseline - bbx_yoff, glyph top = bottom - bbx_h
                glyph_x = bbx_xoff - cell_xoff
                baseline_from_top = cell_h + cell_yoff  # distance from top to baseline (= FONT_ASCENT)
                glyph_bottom = baseline_from_top - bbx_yoff  # row just below last glyph row
                glyph_top = glyph_bottom - bbx_h

                for row_idx, hex_str in enumerate(bitmap_rows):
                    cell_y = glyph_top + row_idx
                    if cell_y < 0 or cell_y >= cell_h:
                        continue
                    val = int(hex_str, 16)
                    # Number of bits in the hex string
                    num_bits = len(hex_str) * 4
                    for bit in range(bbx_w):
                        if val & (1 << (num_bits - 1 - bit)):
                            x = glyph_x + bit
                            if 0 <= x < cell_w:
                                cell[cell_y][x] = '#'

                glyphs[ch] = [''.join(row) for row in cell]
        i += 1

    return cell_w, cell_h, glyphs


def generate_from_bdf(bdf_path, output_path):
    """Generate a deployment PNG + .dat from a BDF font file.

    Output format is identical to generate_deployment_png(): RGBA image with
    transparent background, white foreground, characters tiled left-to-right.
    """
    char_w, char_h, glyphs = parse_bdf(bdf_path)

    # ASCII 32-126
    char_list = [chr(c) for c in range(32, 127)]
    num_chars = len(char_list)
    img_w = num_chars * char_w
    img_h = char_h

    img = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))

    for col_idx, ch in enumerate(char_list):
        x_off = col_idx * char_w
        rows = glyphs.get(ch)
        if rows:
            for r_idx, row_str in enumerate(rows):
                if r_idx >= char_h:
                    break
                for c_idx in range(char_w):
                    if c_idx < len(row_str) and row_str[c_idx] == '#':
                        img.putpixel((x_off + c_idx, r_idx), (255, 255, 255, 255))

    img.save(output_path)
    print(f"Saved BDF font PNG to {output_path} ({num_chars} chars, {img_w}x{img_h}, cell {char_w}x{char_h})")

    dat_path = output_path + ".dat"
    with open(dat_path, 'w', encoding='utf-8') as f:
        f.write("".join(char_list) + "\n")
    print(f"Saved font char list to {dat_path}")


def generate_c_array():
    out = ["static const uint8_t overlay_font_5x7[96][7] = {"]
    for ascii_code in range(32, 128):
        c_bytes = [0]*7
        char_str = chr(ascii_code)
        
        rows = FONT.get(char_str)
            
        if rows:
            for r_idx, row_str in enumerate(rows):
                if r_idx >= 7: break
                val = 0
                for c_idx in range(5):
                    if c_idx < len(row_str) and row_str[c_idx] == '#':
                        val |= (1 << (4 - c_idx))
                c_bytes[r_idx] = val
        else:
            c_bytes = [0x0E, 0x11, 0x02, 0x04, 0x00, 0x04, 0x00] # ? shape
            if ascii_code == 127: # DEL
                c_bytes = [0x1F]*7
                
        hex_str = ",".join([f"0x{b:02X}" for b in c_bytes])
        char_rep = chr(ascii_code) if ascii_code != 127 else "DEL"
        # Sanitize char_rep for comment
        if char_rep == "*/": char_rep = "* /"
        out.append(f"    {{{hex_str}}}, /* {ascii_code:3} {char_rep} */")
    out.append("};")
    return "\n".join(out)

def generate_png(output_path):
    CHAR_W, CHAR_H = 5, 7
    COLS, ROWS = 16, 6
    img_w = COLS * (CHAR_W + 1) + 1
    img_h = ROWS * (CHAR_H + 1) + 1
    
    img = Image.new('RGB', (img_w, img_h), (255, 0, 255)) # MAGENTA background
    draw = ImageDraw.Draw(img)
    
    # Draw grid lines
    for r in range(ROWS + 1):
        y = r * (CHAR_H + 1)
        draw.line([(0, y), (img_w, y)], fill=(0, 0, 0))
    for c in range(COLS + 1):
        x = c * (CHAR_W + 1)
        draw.line([(x, 0), (x, img_h)], fill=(0, 0, 0))
        
    for ascii_code in range(32, 128):
        idx = ascii_code - 32
        row = idx // COLS
        col = idx % COLS
        x_off = col * (CHAR_W + 1) + 1
        y_off = row * (CHAR_H + 1) + 1
        
        char_str = chr(ascii_code)
        if char_str in FONT:
            rows = FONT[char_str]
            r_idx = 0
            for r in rows:
                if r_idx >= CHAR_H: break
                for c_idx in range(5):
                    if c_idx < len(r) and r[c_idx] == '#':
                        img.putpixel((x_off + c_idx, y_off + r_idx), (255, 255, 255))
                r_idx += 1
                
    img.save(output_path)
    print(f"Saved font preview to {output_path}")

def generate_deployment_png(output_path):
    """Generate a font PNG for deployment with the host's bitmap font loader.

    Format: RGBA image, transparent background, white foreground pixels.
    Layout: characters tiled left-to-right, each CHAR_W pixels wide.
      - image width  = numChars * CHAR_W
      - image height = CHAR_H (7 pixels)
      - character i starts at x = i * CHAR_W
    The .dat sidecar file lists characters in the same order (UTF-8, one line).
    """
    CHAR_W, CHAR_H = 5, 7

    # Build ordered character list: ASCII printable first, then extended chars
    char_list = [chr(c) for c in range(32, 128)]
    for ch in FONT:
        if len(ch) == 1 and ord(ch) >= 128:
            char_list.append(ch)


    num_chars = len(char_list)
    img_w = num_chars * CHAR_W
    img_h = CHAR_H

    img = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))  # fully transparent

    for col_idx, ch in enumerate(char_list):
        x_off = col_idx * CHAR_W
        rows = FONT.get(ch)
        if rows:
            for r_idx, row_str in enumerate(rows):
                if r_idx >= CHAR_H:
                    break
                for c_idx in range(CHAR_W):
                    if c_idx < len(row_str) and row_str[c_idx] == '#':
                        img.putpixel((x_off + c_idx, r_idx), (255, 255, 255, 255))
        elif ch == chr(127):  # DEL — solid block fallback
            for r_idx in range(CHAR_H):
                for c_idx in range(CHAR_W):
                    img.putpixel((x_off + c_idx, r_idx), (255, 255, 255, 255))
        # else: leave transparent (blank glyph)

    img.save(output_path)
    print(f"Saved deployment font PNG to {output_path} ({num_chars} chars, {img_w}x{img_h})")

    # Write companion .dat file — character list as UTF-8, newline-terminated
    dat_path = output_path + ".dat"
    with open(dat_path, 'w', encoding='utf-8') as f:
        f.write("".join(char_list) + "\n")
    print(f"Saved font char list to {dat_path}")




if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Generate OLED Font")
    parser.append = parser.add_argument
    parser.append("--c-array", action="store_true", help="Print the C array instead of writing PNG")
    parser.append("--png", type=str, metavar="FILE", help="Generate preview PNG (magenta grid, for visual inspection)")
    parser.append("--deploy-png", type=str, metavar="FILE", help="Generate deployment PNG for host (RGBA, alpha-based, for js_display_load_font)")
    parser.append("--bdf", type=str, metavar="FILE", help="Use BDF font file as source (with --deploy-png)")
    args = parser.parse_args()

    if args.c_array:
        print(generate_c_array())
    if args.png:
        generate_png(args.png)
    if args.deploy_png:
        if args.bdf:
            generate_from_bdf(args.bdf, args.deploy_png)
        else:
            generate_deployment_png(args.deploy_png)
    if not len(sys.argv) > 1:
        print("Usage: python3 scripts/generate_font.py [--c-array] [--png preview.png] [--deploy-png build/host/font.png] [--bdf font.bdf]")

