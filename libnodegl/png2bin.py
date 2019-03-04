import sys
from PIL import Image

img = Image.open(sys.argv[1])
pix = img.load()

w, h = img.size

font_h, font_w = 8, 7
for char_y in range(8):
    for char_x in range(16):
        data_char = []
        for py in range(font_h):
            char = 0
            for px in range(font_w):
                x = char_x * font_w + px
                y = char_y * font_h + py
                bit = 1 if pix[x, y] == (255, 255, 255) else 0
                char |= bit<<px
            data_char.append(char)
        print '    {%s},' % ', '.join('0x%02x' % c for c in data_char)
