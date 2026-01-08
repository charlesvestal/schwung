#!/usr/bin/env python3
"""
Generate a small pixel font PNG for Move Anything.
Uses a 4x6 pixel font design for 128x64 1-bit displays.
"""

from PIL import Image, ImageDraw

# Character set (must match font.png.dat)
CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,;:?!-_~+=*%$<>#\"'()[]|`/\\@"

# 4x6 pixel font definitions (each char is 4 wide x 6 tall)
# 1 = pixel on, 0 = pixel off
FONT_4X6 = {
    ' ': [
        "    ",
        "    ",
        "    ",
        "    ",
        "    ",
        "    ",
    ],
    'A': [
        " ## ",
        "#  #",
        "####",
        "#  #",
        "#  #",
        "    ",
    ],
    'B': [
        "### ",
        "#  #",
        "### ",
        "#  #",
        "### ",
        "    ",
    ],
    'C': [
        " ###",
        "#   ",
        "#   ",
        "#   ",
        " ###",
        "    ",
    ],
    'D': [
        "### ",
        "#  #",
        "#  #",
        "#  #",
        "### ",
        "    ",
    ],
    'E': [
        "####",
        "#   ",
        "### ",
        "#   ",
        "####",
        "    ",
    ],
    'F': [
        "####",
        "#   ",
        "### ",
        "#   ",
        "#   ",
        "    ",
    ],
    'G': [
        " ###",
        "#   ",
        "# ##",
        "#  #",
        " ###",
        "    ",
    ],
    'H': [
        "#  #",
        "#  #",
        "####",
        "#  #",
        "#  #",
        "    ",
    ],
    'I': [
        "### ",
        " #  ",
        " #  ",
        " #  ",
        "### ",
        "    ",
    ],
    'J': [
        "  ##",
        "   #",
        "   #",
        "#  #",
        " ## ",
        "    ",
    ],
    'K': [
        "#  #",
        "# # ",
        "##  ",
        "# # ",
        "#  #",
        "    ",
    ],
    'L': [
        "#   ",
        "#   ",
        "#   ",
        "#   ",
        "####",
        "    ",
    ],
    'M': [
        "#  #",
        "####",
        "####",
        "#  #",
        "#  #",
        "    ",
    ],
    'N': [
        "#  #",
        "## #",
        "# ##",
        "#  #",
        "#  #",
        "    ",
    ],
    'O': [
        " ## ",
        "#  #",
        "#  #",
        "#  #",
        " ## ",
        "    ",
    ],
    'P': [
        "### ",
        "#  #",
        "### ",
        "#   ",
        "#   ",
        "    ",
    ],
    'Q': [
        " ## ",
        "#  #",
        "#  #",
        "# # ",
        " # #",
        "    ",
    ],
    'R': [
        "### ",
        "#  #",
        "### ",
        "# # ",
        "#  #",
        "    ",
    ],
    'S': [
        " ###",
        "#   ",
        " ## ",
        "   #",
        "### ",
        "    ",
    ],
    'T': [
        "####",
        " #  ",
        " #  ",
        " #  ",
        " #  ",
        "    ",
    ],
    'U': [
        "#  #",
        "#  #",
        "#  #",
        "#  #",
        " ## ",
        "    ",
    ],
    'V': [
        "#  #",
        "#  #",
        "#  #",
        " ## ",
        " #  ",
        "    ",
    ],
    'W': [
        "#  #",
        "#  #",
        "####",
        "####",
        "#  #",
        "    ",
    ],
    'X': [
        "#  #",
        " ## ",
        " #  ",
        " ## ",
        "#  #",
        "    ",
    ],
    'Y': [
        "#  #",
        " ## ",
        " #  ",
        " #  ",
        " #  ",
        "    ",
    ],
    'Z': [
        "####",
        "  # ",
        " #  ",
        "#   ",
        "####",
        "    ",
    ],
    # Lowercase (same as uppercase for this tiny font)
    'a': [
        "    ",
        " ## ",
        "  ##",
        "# ##",
        " ###",
        "    ",
    ],
    'b': [
        "#   ",
        "#   ",
        "### ",
        "#  #",
        "### ",
        "    ",
    ],
    'c': [
        "    ",
        " ## ",
        "#   ",
        "#   ",
        " ## ",
        "    ",
    ],
    'd': [
        "   #",
        "   #",
        " ###",
        "#  #",
        " ###",
        "    ",
    ],
    'e': [
        "    ",
        " ## ",
        "####",
        "#   ",
        " ## ",
        "    ",
    ],
    'f': [
        "  # ",
        " #  ",
        "### ",
        " #  ",
        " #  ",
        "    ",
    ],
    'g': [
        "    ",
        " ###",
        "#  #",
        " ###",
        "### ",
    ],
    'h': [
        "#   ",
        "#   ",
        "### ",
        "#  #",
        "#  #",
        "    ",
    ],
    'i': [
        " #  ",
        "    ",
        " #  ",
        " #  ",
        " #  ",
        "    ",
    ],
    'j': [
        "  # ",
        "    ",
        "  # ",
        "  # ",
        " #  ",
        "    ",
    ],
    'k': [
        "#   ",
        "#  #",
        "##  ",
        "#  #",
        "#  #",
        "    ",
    ],
    'l': [
        " #  ",
        " #  ",
        " #  ",
        " #  ",
        "  # ",
        "    ",
    ],
    'm': [
        "    ",
        "### ",
        "####",
        "#  #",
        "#  #",
        "    ",
    ],
    'n': [
        "    ",
        "### ",
        "#  #",
        "#  #",
        "#  #",
        "    ",
    ],
    'o': [
        "    ",
        " ## ",
        "#  #",
        "#  #",
        " ## ",
        "    ",
    ],
    'p': [
        "    ",
        "### ",
        "#  #",
        "### ",
        "#   ",
    ],
    'q': [
        "    ",
        " ###",
        "#  #",
        " ###",
        "   #",
    ],
    'r': [
        "    ",
        " ###",
        "#   ",
        "#   ",
        "#   ",
        "    ",
    ],
    's': [
        "    ",
        " ###",
        "##  ",
        "  ##",
        "### ",
        "    ",
    ],
    't': [
        " #  ",
        "### ",
        " #  ",
        " #  ",
        "  # ",
        "    ",
    ],
    'u': [
        "    ",
        "#  #",
        "#  #",
        "#  #",
        " ###",
        "    ",
    ],
    'v': [
        "    ",
        "#  #",
        "#  #",
        " ## ",
        " #  ",
        "    ",
    ],
    'w': [
        "    ",
        "#  #",
        "####",
        "####",
        " ## ",
        "    ",
    ],
    'x': [
        "    ",
        "#  #",
        " ## ",
        " ## ",
        "#  #",
        "    ",
    ],
    'y': [
        "    ",
        "#  #",
        " ## ",
        " #  ",
        "#   ",
        "    ",
    ],
    'z': [
        "    ",
        "####",
        "  # ",
        " #  ",
        "####",
        "    ",
    ],
    '0': [
        " ## ",
        "#  #",
        "#  #",
        "#  #",
        " ## ",
        "    ",
    ],
    '1': [
        " #  ",
        "##  ",
        " #  ",
        " #  ",
        "### ",
        "    ",
    ],
    '2': [
        " ## ",
        "#  #",
        "  # ",
        " #  ",
        "####",
        "    ",
    ],
    '3': [
        "### ",
        "   #",
        " ## ",
        "   #",
        "### ",
        "    ",
    ],
    '4': [
        "#  #",
        "#  #",
        "####",
        "   #",
        "   #",
        "    ",
    ],
    '5': [
        "####",
        "#   ",
        "### ",
        "   #",
        "### ",
        "    ",
    ],
    '6': [
        " ## ",
        "#   ",
        "### ",
        "#  #",
        " ## ",
        "    ",
    ],
    '7': [
        "####",
        "   #",
        "  # ",
        " #  ",
        " #  ",
        "    ",
    ],
    '8': [
        " ## ",
        "#  #",
        " ## ",
        "#  #",
        " ## ",
        "    ",
    ],
    '9': [
        " ## ",
        "#  #",
        " ###",
        "   #",
        " ## ",
        "    ",
    ],
    '.': [
        "    ",
        "    ",
        "    ",
        "    ",
        " #  ",
        "    ",
    ],
    ',': [
        "    ",
        "    ",
        "    ",
        " #  ",
        "#   ",
        "    ",
    ],
    ';': [
        "    ",
        " #  ",
        "    ",
        " #  ",
        "#   ",
        "    ",
    ],
    ':': [
        "    ",
        " #  ",
        "    ",
        " #  ",
        "    ",
        "    ",
    ],
    '?': [
        " ## ",
        "#  #",
        "  # ",
        "    ",
        " #  ",
        "    ",
    ],
    '!': [
        " #  ",
        " #  ",
        " #  ",
        "    ",
        " #  ",
        "    ",
    ],
    '-': [
        "    ",
        "    ",
        "### ",
        "    ",
        "    ",
        "    ",
    ],
    '_': [
        "    ",
        "    ",
        "    ",
        "    ",
        "####",
        "    ",
    ],
    '~': [
        "    ",
        " # #",
        "# # ",
        "    ",
        "    ",
        "    ",
    ],
    '+': [
        "    ",
        " #  ",
        "### ",
        " #  ",
        "    ",
        "    ",
    ],
    '=': [
        "    ",
        "### ",
        "    ",
        "### ",
        "    ",
        "    ",
    ],
    '*': [
        "    ",
        "# # ",
        " #  ",
        "# # ",
        "    ",
        "    ",
    ],
    '%': [
        "#  #",
        "  # ",
        " #  ",
        "#   ",
        "#  #",
        "    ",
    ],
    '$': [
        " #  ",
        " ###",
        " #  ",
        "### ",
        " #  ",
        "    ",
    ],
    '<': [
        "  # ",
        " #  ",
        "#   ",
        " #  ",
        "  # ",
        "    ",
    ],
    '>': [
        "#   ",
        " #  ",
        "  # ",
        " #  ",
        "#   ",
        "    ",
    ],
    '#': [
        " # #",
        "####",
        " # #",
        "####",
        " # #",
        "    ",
    ],
    '"': [
        "# # ",
        "# # ",
        "    ",
        "    ",
        "    ",
        "    ",
    ],
    "'": [
        " #  ",
        " #  ",
        "    ",
        "    ",
        "    ",
        "    ",
    ],
    '(': [
        "  # ",
        " #  ",
        " #  ",
        " #  ",
        "  # ",
        "    ",
    ],
    ')': [
        " #  ",
        "  # ",
        "  # ",
        "  # ",
        " #  ",
        "    ",
    ],
    '[': [
        " ## ",
        " #  ",
        " #  ",
        " #  ",
        " ## ",
        "    ",
    ],
    ']': [
        " ## ",
        "  # ",
        "  # ",
        "  # ",
        " ## ",
        "    ",
    ],
    '|': [
        " #  ",
        " #  ",
        " #  ",
        " #  ",
        " #  ",
        "    ",
    ],
    '`': [
        "#   ",
        " #  ",
        "    ",
        "    ",
        "    ",
        "    ",
    ],
    '/': [
        "   #",
        "  # ",
        " #  ",
        "#   ",
        "    ",
        "    ",
    ],
    '\\': [
        "#   ",
        " #  ",
        "  # ",
        "   #",
        "    ",
        "    ",
    ],
    '@': [
        " ## ",
        "# ##",
        "# ##",
        "#   ",
        " ## ",
        "    ",
    ],
}

# Colors
MAGENTA = (255, 0, 255)  # Background
BLACK = (0, 0, 0)        # Separator between characters
WHITE = (255, 255, 255)  # Character pixels

CHAR_WIDTH = 4
CHAR_HEIGHT = 6

def generate_font():
    # Calculate image size
    # Format: 1px black separator, then char pixels, repeated
    # Each char cell is: 1 black + CHAR_WIDTH pixels
    num_chars = len(CHARS)
    img_width = num_chars * (CHAR_WIDTH + 1) + 1  # +1 for final separator
    img_height = CHAR_HEIGHT + 1  # +1 for bottom row (emptyColor detection)

    # Create image with magenta background
    img = Image.new('RGB', (img_width, img_height), MAGENTA)
    draw = ImageDraw.Draw(img)

    # Draw vertical black separators and bottom black line
    for i in range(num_chars + 1):
        x = i * (CHAR_WIDTH + 1)
        for y in range(img_height):
            img.putpixel((x, y), BLACK)

    # Bottom row is black (for emptyColor detection - but we want magenta there)
    # Actually, looking at the loader: emptyColor = data[(height-1) * width]
    # That's the bottom-left pixel. We need that to be magenta (background).
    # The black separators handle the border detection.

    for i, char in enumerate(CHARS):
        x_offset = i * (CHAR_WIDTH + 1) + 1  # +1 to skip the separator

        # Draw character if we have it
        if char in FONT_4X6:
            glyph = FONT_4X6[char]
            for y, row in enumerate(glyph):
                for x, pixel in enumerate(row):
                    if pixel == '#':
                        px = x_offset + x
                        py = y
                        if px < img_width and py < img_height:
                            img.putpixel((px, py), WHITE)

    return img

if __name__ == '__main__':
    import sys

    output_path = sys.argv[1] if len(sys.argv) > 1 else 'font.png'

    img = generate_font()
    img.save(output_path)
    print(f"Generated {output_path}: {img.width}x{img.height} pixels, {len(CHARS)} characters")
    print(f"Character cell: {CELL_WIDTH}x{CELL_HEIGHT} (4x6 glyphs with 1px border)")
