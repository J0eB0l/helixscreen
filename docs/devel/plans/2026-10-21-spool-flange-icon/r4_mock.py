import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, '.')
import slots_mock as sm
import icon_mock as im

SS = 4
def flat(size, fill, color, bg, slots=True):
    # front-on: flange disc with slots + dots; the filament disc shows through
    S = size * SS
    img = np.zeros((S, S, 3), np.float32); img[:] = bg
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) + 0.5
    u, v = xx - S/2, yy - S/2
    rho = np.sqrt(u*u + v*v)
    R = S/2 - SS
    hub = S/6
    fil = hub + 2*SS + (R - 2*SS - hub - 2*SS) * fill
    flange = sm.hexc('#444444')
    t = np.clip(yy / S, 0, 1)[..., None]
    if not slots:
        m = rho <= R; img[m] = np.array(flange)[None]
        m = rho <= fil; img[m] = (np.array(sm.lighten(color, 30))*(1-t) + np.array(sm.darken(color, 30))*t)[m]
    else:
        m = rho <= R
        img[m] = (np.array(sm.lighten(flange, 45))*(1-t) + np.array(sm.darken(flange, 10))*t)[m]
        holes = np.zeros_like(m)
        n = 6; r1, r2, w = S*0.21, S*0.40, S*0.065
        for k in range(n):
            a = 2*math.pi*k/n - math.pi/2; ca, sa = math.cos(a), math.sin(a)
            p = np.clip(u*ca + v*sa, r1, r2)
            holes |= (u-p*ca)**2 + (v-p*sa)**2 <= w*w
        if size >= 96:
            pitch, dr = S*0.035, S*0.008
            gu = np.round(u/pitch)*pitch; gv = np.round(v/pitch)*pitch
            dot = ((u-gu)**2+(v-gv)**2 <= dr*dr) & (rho > S*0.2) & (rho < S*0.44)
            near = np.zeros_like(dot)
            for k in range(n):
                a = 2*math.pi*k/n - math.pi/2; ca, sa = math.cos(a), math.sin(a)
                p = np.clip(u*ca + v*sa, r1, r2)
                near |= (u-p*ca)**2 + (v-p*sa)**2 <= (w+S*0.03)**2
            holes |= dot & ~near
        holes &= m
        smoke = m & ~holes & (rho <= fil)
        col = (np.array(color)*(1-t) + np.array(sm.darken(color, 70))*t)
        img[smoke] = img[smoke]*0.55 + col[smoke]*0.45
        inside = holes & (rho <= fil)
        img[inside] = (np.array(color)*(1-t) + np.array(sm.darken(color, 70))*t)[inside]
        out_ = holes & ~inside
        img[out_] = np.array(sm.darken(flange, 45))[None]
    hm = rho <= hub
    img[hm] = (np.array(sm.hexc('#050505'))*(1-t) + np.array(sm.hexc('#404040'))*t)[hm]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize((size, size), Image.LANCZOS)

# icon variants
def ic_fill(d): im.reel(d, 12, 12, 10, 6, 2.6, 5.0, 7.6, 2.2)
def ic_outline(d):
    im.disc(d, 12, 12, 10, 255); im.disc(d, 12, 12, 8.3, 0)
    im.disc(d, 12, 12, 2.6+1.6, 255); im.disc(d, 12, 12, 2.6, 0)
    for k in range(6):
        a = 2*math.pi*k/6 - math.pi/2
        im.slot(d, 12, 12, a, 5.6, 6.6, 2.0+2.4, 255)
        im.slot(d, 12, 12, a, 5.6, 6.6, 2.0, 0)
def ic_alert(d):
    # MDI -alert shape: glyph shifted left, "!" bar at the right
    im.reel(d, 9.5, 12, 8.5, 6, 2.2, 4.2, 6.4, 1.9)
    d.rectangle([im.P(20), im.P(7), im.P(22), im.P(17)], fill=255)
    d.rectangle([im.P(20), im.P(19), im.P(22), im.P(21)], fill=255)
    # clear a gap between reel and the bar
    pass

def sheet(dark, path):
    bg = (25, 25, 28) if dark else (240, 240, 244)
    fg = (230, 230, 230) if dark else (30, 30, 30)
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 16)
    out = Image.new('RGB', (1000, 760), bg); d = ImageDraw.Draw(out)
    red, blue = sm.hexc('#d02020'), sm.hexc('#2a8adf')
    d.text((20, 10), '3D spool: 6 slots + dots at 96px and up, slots only below', fill=fg, font=font)
    x = 20
    for s in (200, 96, 64, 40, 32):
        out.paste(sm.render(s, 0.95, red, 'slots:6:dots' if s >= 96 else 'slots:6', bg), (x, 240 - s)); x += s + 20
    x += 20
    for s in (200, 64, 32):
        out.paste(sm.render(s, 0.3, blue, 'slots:6:dots' if s >= 96 else 'slots:6', bg), (x, 240 - s)); x += s + 20
    d.text((20, 270), 'Flat AMS style: today vs front-on with the same cut-outs (fill 95% / 30%)', fill=fg, font=font)
    x = 20
    for s in (96, 64, 40):
        out.paste(flat(s, 0.95, red, bg, False), (x, 400 - s)); x += s + 14
    d.text((x, 360), 'today', fill=fg, font=font); x += 70
    for fl, col in ((0.95, red), (0.3, blue)):
        for s in (96, 64, 40):
            out.paste(flat(s, fl, col, bg), (x, 400 - s)); x += s + 14
        x += 20
    d.text((20, 430), 'Icon set: filament / filament_outline (inactive navbar) / filament_alert', fill=fg, font=font)
    x = 20
    for fn in (ic_fill, ic_outline, ic_alert):
        out.paste(im.glyph(fn, 96, fg, bg), (x, 460)); out.paste(im.glyph(fn, 32, fg, bg), (x + 110, 524)); out.paste(im.glyph(fn, 24, fg, bg), (x + 150, 532)); x += 210
    # navbar strip: inactive outline grey, active filled accent
    y = 600; x = 20
    for cp in (0xF02DC, 0xF062E):
        out.paste(im.mdi(FONT, cp, 40, (150,150,155), bg), (x, y)); x += 60
    out.paste(im.glyph(ic_outline, 40, (150,150,155), bg), (x, y)); x += 60
    out.paste(im.mdi(FONT, 0xF0493, 40, (150,150,155), bg), (x, y)); x += 100
    for cp in (0xF02DC, 0xF062E):
        out.paste(im.mdi(FONT, cp, 40, (150,150,155), bg), (x, y)); x += 60
    out.paste(im.glyph(ic_fill, 40, (66,135,245), bg), (x, y)); x += 60
    out.paste(im.mdi(FONT, 0xF0493, 40, (150,150,155), bg), (x, y)); x += 40
    d.text((20, 660), 'navbar: Filament tab inactive (outline)          Filament tab active (filled, accent)', fill=fg, font=font)
    out.save(path)

FONT = sys.argv[2]
sheet(True, sys.argv[1] + '/r4-dark.png'); sheet(False, sys.argv[1] + '/r4-light.png')
