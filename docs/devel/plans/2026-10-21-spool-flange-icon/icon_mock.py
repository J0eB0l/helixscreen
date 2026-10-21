"""Throwaway spool-icon candidates on the 24x24 MDI grid, next to real MDI glyphs."""
import math, sys
from PIL import Image, ImageDraw, ImageFont

K = 32  # grid scale for drawing
G = 24 * K


def P(v):
    return v * K


def disc(d, cx, cy, r, fill):
    d.ellipse([P(cx - r), P(cy - r), P(cx + r), P(cy + r)], fill=fill)


def ell(d, cx, cy, rx, ry, fill):
    d.ellipse([P(cx - rx), P(cy - ry), P(cx + rx), P(cy + ry)], fill=fill)


def slot(d, cx, cy, a, r1, r2, w, fill):
    ca, sa = math.cos(a), math.sin(a)
    x1, y1, x2, y2 = cx + r1 * ca, cy + r1 * sa, cx + r2 * ca, cy + r2 * sa
    d.line([P(x1), P(y1), P(x2), P(y2)], fill=fill, width=int(P(w)))
    disc(d, x1, y1, w / 2, fill)
    disc(d, x2, y2, w / 2, fill)


def reel(d, cx, cy, r, n, hub, r1, r2, w):
    disc(d, cx, cy, r, 255)
    disc(d, cx, cy, hub, 0)
    for k in range(n):
        slot(d, cx, cy, 2 * math.pi * k / n - math.pi / 2, r1, r2, w, 0)


def i_reel6(d):
    reel(d, 12, 12, 10, 6, 2.6, 5.0, 7.6, 2.2)


def i_reel8(d):
    reel(d, 12, 12, 10, 8, 2.4, 4.8, 7.8, 1.7)


def i_reel_ring(d):
    # rim + filament ring + slotted core: the drawn spool front-on
    disc(d, 12, 12, 10, 255)
    disc(d, 12, 12, 8.6, 0)
    disc(d, 12, 12, 7.4, 255)
    disc(d, 12, 12, 2.4, 0)
    for k in range(6):
        slot(d, 12, 12, 2 * math.pi * k / 6 - math.pi / 2, 4.3, 5.4, 1.8, 0)


def i_three_quarter(d):
    # back flange, wound body, front flange with hub
    ell(d, 7.5, 12, 4.2, 10, 255)
    d.rectangle([P(7.5), P(4), P(16.5), P(20)], fill=255)
    for y in (8, 12, 16):
        d.rectangle([P(9.6), P(y - 0.45), P(13.6), P(y + 0.45)], fill=0)
    ell(d, 16.5, 12, 5.4, 11.2, 0)
    ell(d, 16.5, 12, 4.4, 10, 255)
    ell(d, 16.5, 12, 1.3, 2.6, 0)
    for k in range(6):
        a = 2 * math.pi * k / 6 - math.pi / 2
        ca, sa = math.cos(a), math.sin(a)
        x1, y1 = 16.5 + 1.95 * ca, 12 + 4.6 * sa
        x2, y2 = 16.5 + 3.2 * ca, 12 + 7.6 * sa
        d.line([P(x1), P(y1), P(x2), P(y2)], fill=0, width=int(P(1.1)))


def i_profile(d):
    # side profile: two flanges and the winding between them
    d.rounded_rectangle([P(3), P(2), P(6), P(22)], radius=P(1.2), fill=255)
    d.rounded_rectangle([P(18), P(2), P(21), P(22)], radius=P(1.2), fill=255)
    for i, y in enumerate((5, 8.5, 12, 15.5)):
        d.rounded_rectangle([P(7), P(y), P(17), P(y + 2.6)], radius=P(1.3), fill=255)
    d.rectangle([P(7), P(18.5), P(17), P(19.5)], fill=255)


def i_profile_tail(d):
    i_profile(d)
    # loose end leaving over the top
    d.line([P(12), P(5.6), P(12), P(2.2)], fill=255, width=int(P(2)))
    disc(d, 12, 2.2, 1, 255)


CANDIDATES = [('I1 reel, 6 slots', i_reel6), ('I2 reel, 8 slots', i_reel8),
              ('I3 rim + filament ring', i_reel_ring), ('I4 three-quarter', i_three_quarter),
              ('I5 side profile', i_profile), ('I6 profile + loose end', i_profile_tail)]


def glyph(fn, px, color, bg):
    m = Image.new('L', (G, G), 0)
    fn(ImageDraw.Draw(m))
    m = m.resize((px, px), Image.LANCZOS)
    im = Image.new('RGB', (px, px), bg)
    im.paste(Image.new('RGB', (px, px), color), (0, 0), m)
    return im


def mdi(font_path, cp, px, color, bg):
    f = ImageFont.truetype(font_path, px)
    im = Image.new('RGB', (px, px), bg)
    ImageDraw.Draw(im).text((0, 0), chr(cp), font=f, fill=color)
    return im


def sheet(path, mdi_font, dark):
    bg = (25, 25, 28) if dark else (240, 240, 244)
    fg = (230, 230, 230) if dark else (30, 30, 30)
    accent = (66, 135, 245)
    label = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 16)
    rows = [('MDI today: printer-3d-nozzle', None)] + CANDIDATES
    W, H = 900, 40 + len(rows) * 110
    out = Image.new('RGB', (W, H), bg)
    d = ImageDraw.Draw(out)
    d.text((20, 10), 'big            48     32    24     in a navbar row (home / tune / [icon] / cog)', fill=fg, font=label)
    for r, (name, fn) in enumerate(rows):
        y = 40 + r * 110
        g = (lambda px, c: mdi(mdi_font, 0xF0E5B, px, c, bg)) if fn is None else (lambda px, c, fn=fn: glyph(fn, px, c, bg))
        out.paste(g(96, fg), (20, y))
        out.paste(g(48, fg), (140, y + 24))
        out.paste(g(32, fg), (200, y + 32))
        out.paste(g(24, fg), (245, y + 36))
        x = 300
        for cp in (0xF02DC, 0xF062E):
            out.paste(mdi(mdi_font, cp, 40, (150, 150, 155), bg), (x, y + 28)); x += 60
        out.paste(g(40, accent), (x, y + 28)); x += 60
        out.paste(mdi(mdi_font, 0xF0493, 40, (150, 150, 155), bg), (x, y + 28)); x += 70
        d.text((x, y + 38), name, fill=fg, font=label)
    out.save(path)


if __name__ == '__main__':
    o, font = sys.argv[1], sys.argv[2]
    sheet(f'{o}/icons-dark.png', font, True)
    sheet(f'{o}/icons-light.png', font, False)
