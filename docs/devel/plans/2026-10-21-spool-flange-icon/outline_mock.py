import math, sys
sys.path.insert(0, '.')
import icon_mock as im
from PIL import Image, ImageDraw, ImageFont
FONT = sys.argv[2]
def fill(d): im.reel(d, 12, 12, 10, 6, 2.6, 5.0, 7.6, 2.2)
def ring(d, r_out=10, w=1.8):
    im.disc(d, 12, 12, r_out, 255); im.disc(d, 12, 12, r_out - w, 0)
def O1(d):  # rim + hub ring, slots as solid dashes floating inside
    ring(d); im.disc(d, 12, 12, 2.6 + 1.6, 255); im.disc(d, 12, 12, 2.6, 0)
    for k in range(6): im.slot(d, 12, 12, 2*math.pi*k/6 - math.pi/2, 5.4, 6.6, 1.8, 255)
def O2(d):  # rim + hub dot, solid dashes (no hub ring)
    ring(d); im.disc(d, 12, 12, 1.8, 255)
    for k in range(6): im.slot(d, 12, 12, 2*math.pi*k/6 - math.pi/2, 4.4, 6.6, 1.8, 255)
def O3(d):  # filled reel inside a separate thin rim: the flange edge, face open
    ring(d, 10, 1.6)
    im.disc(d, 12, 12, 6.8, 255); im.disc(d, 12, 12, 2.2, 0)
    for k in range(6): im.slot(d, 12, 12, 2*math.pi*k/6 - math.pi/2, 4.0, 5.2, 1.6, 0)
def O4(d):  # two rings (flange rim + filament edge) and the hub ring
    ring(d, 10, 1.8); ring(d, 6.6, 1.6); im.disc(d, 12, 12, 2.8, 255); im.disc(d, 12, 12, 1.4, 0)
OPTS = [('O1 rim + hub ring + solid slot dashes', O1), ('O2 rim + solid dashes + hub dot', O2),
        ('O3 thin rim around a small filled reel', O3), ('O4 rim, filament ring, hub', O4)]
bg = (25, 25, 28); fg = (230, 230, 230); grey = (150, 150, 155); acc = (66, 135, 245)
font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 16)
out = Image.new('RGB', (1000, 40 + len(OPTS) * 100), bg); d = ImageDraw.Draw(out)
d.text((20, 10), 'outline (big, 32, 24)      inactive navbar                active (filled)', fill=fg, font=font)
for r, (name, fn) in enumerate(OPTS):
    y = 40 + r * 100
    out.paste(im.glyph(fn, 80, fg, bg), (20, y))
    out.paste(im.glyph(fn, 32, fg, bg), (110, y + 24)); out.paste(im.glyph(fn, 24, fg, bg), (150, y + 28))
    x = 200
    for cp in (0xF02DC, 0xF062E): out.paste(im.mdi(FONT, cp, 40, grey, bg), (x, y + 20)); x += 55
    out.paste(im.glyph(fn, 40, grey, bg), (x, y + 20)); x += 55
    out.paste(im.mdi(FONT, 0xF0493, 40, grey, bg), (x, y + 20)); x += 70
    out.paste(im.glyph(fill, 46, acc, bg), (x, y + 17)); x += 80
    d.text((x, y + 30), name, fill=fg, font=font)
out.save(sys.argv[1] + '/outline-opts.png')
