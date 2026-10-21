"""Throwaway mockups of #1817 flange-hole styles, mirroring render_spool_pixels geometry."""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

SKEW, FLANGE_R, SPOOL_W, HUB_R = 0.45, 0.42, 0.35, 0.10
SS = 4  # supersample


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def lighten(c, amt):
    return tuple(min(255, v + amt) for v in c)


def darken(c, amt):
    return tuple(max(0, v - amt) for v in c)


def hexc(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def render(size, fill, color, style, bg):
    S = size * SS
    img = np.zeros((S, S, 3), dtype=np.float32)
    img[:] = bg
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) + 0.5
    cy = S / 2
    fry = S * FLANGE_R
    frx = fry * SKEW
    hry = S * HUB_R
    hrx = hry * SKEW
    sw = S * SPOOL_W
    lx, rx_ = S / 2 - sw / 2, S / 2 + sw / 2
    fil_ry = hry + (fry * 0.85 - hry) * fill
    fil_rx = fil_ry * SKEW
    body, shade = hexc('#666666'), hexc('#444444')
    t = np.clip((yy - (cy - fry)) / (2 * fry), 0, 1)[..., None]

    def grad(top, bot):
        return np.array(top) * (1 - t) + np.array(bot) * t

    def ell(cx, rx, ry):
        return ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2 <= 1

    # back flange
    m = ell(lx, frx, fry)
    img[m] = grad(lighten(shade, 40), darken(shade, 25))[m]
    edge = m & ~ell(lx + 2 * SS, frx, fry)
    img[edge] = grad(lighten(shade, 100), darken(shade, 40))[edge]
    # filament cylinder
    if fill > 0.01:
        fl = grad(lighten(color, 50), darken(color, 50))
        m = ell(lx, fil_rx, fil_ry) | ((xx >= lx) & (xx <= rx_) & (abs(yy - cy) <= fil_ry))
        img[m] = fl[m]
    # front flange
    fm = ell(rx_, frx, fry)
    img[fm] = grad(lighten(body, 40), darken(body, 25))[fm]
    edge = fm & ~ell(rx_ + 2 * SS, frx, fry)
    img[edge] = grad(lighten(body, 100), darken(body, 40))[edge]

    # face coordinates of the front flange (unskewed): u right, v down, in S px
    u = (xx - rx_) / SKEW
    v = yy - cy
    rho = np.sqrt(u * u + v * v)
    ang = np.arctan2(v, u)

    holes = np.zeros_like(rho, dtype=bool)
    if style in ('dark', 'peek'):
        ring, hr, n = S * 0.25, S * 0.055, 6
        for k in range(n):
            a = 2 * math.pi * k / n + math.pi / 6
            hu, hv = ring * math.cos(a), ring * math.sin(a)
            holes |= (u - hu) ** 2 + (v - hv) ** 2 <= hr * hr
    elif style == 'window':
        inner, outer, n, gap = S * 0.17, S * 0.33, 4, 0.55
        seg = 2 * math.pi / n
        a = np.mod(ang + math.pi / 4, seg)
        holes = (rho >= inner) & (rho <= outer) & (a > gap / 2) & (a < seg - gap / 2)
    elif style == 'spokes':
        inner, outer, n, spoke = S * 0.15, S * 0.36, 5, S * 0.035
        seg = 2 * math.pi / n
        a = np.mod(ang + math.pi / 2, seg)
        # spoke half-width in angle at radius rho
        half = spoke / np.maximum(rho, 1)
        holes = (rho >= inner) & (rho <= outer) & (a > half) & (a < seg - half)
    elif style.startswith('slots'):
        # radial stadium slots, as on vented reusable spools
        n = int(style.split(':')[1]); dots = style.endswith('dots')
        r1, r2, w = S*0.165, S*0.335, S*(0.05 if n <= 6 else 0.038)
        for k in range(n):
            a = 2*math.pi*k/n - math.pi/2
            ca, sa = math.cos(a), math.sin(a)
            p = np.clip(u*ca + v*sa, r1, r2)
            holes |= (u - p*ca)**2 + (v - p*sa)**2 <= w*w
        if dots and size >= 96:
            pitch, dr = S*0.03, S*0.007
            gu = np.round(u/pitch)*pitch; gv = np.round(v/pitch)*pitch
            dot = ((u-gu)**2 + (v-gv)**2 <= dr*dr) & (rho > S*0.15) & (rho < S*0.39)
            near = np.zeros_like(dot)
            for k in range(n):
                a = 2*math.pi*k/n - math.pi/2
                ca, sa = math.cos(a), math.sin(a)
                p = np.clip(u*ca + v*sa, r1, r2)
                near |= (u - p*ca)**2 + (v - p*sa)**2 <= (w+S*0.025)**2
            holes |= dot & ~near
    holes &= fm

    if style == 'dark':
        img[holes] = grad(hexc('#050505'), hexc('#2a2a2a'))[holes]
    else:
        # what is behind: wound filament (shadowed) inside its radius, else the back flange's inner face
        behind_fil = holes & (rho <= fil_ry) & (fill > 0.01)
        behind_back = holes & ~behind_fil
        img[behind_fil] = grad(darken(color, 30), darken(color, 110))[behind_fil]
        img[behind_back] = grad(darken(shade, 20), darken(shade, 50))[behind_back]
        # flange thickness: shadow lip along the upper edge of each cut
        lip = holes & ~np.roll(holes, 3 * SS, axis=0)
        img[lip] = img[lip] * 0.55
    # hub
    hm = ell(rx_, hrx, hry)
    img[hm] = grad(hexc('#050505'), hexc('#404040'))[hm]

    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    return im.resize((size, size), Image.LANCZOS)


def sheet(bg_hex, path):
    bg = hexc(bg_hex)
    fg = (230, 230, 230) if sum(bg) < 300 else (30, 30, 30)
    styles = [('window', 'C  (last round)'), ('slots:6', 'C2  6 radial slots'), ('slots:8', 'C3  8 radial slots'),
              ('slots:8:dots', 'C4  8 slots + vent dots'), ('slots:10', 'C5  10 radial slots')]
    big, mids = 200, [64, 40]
    colw = 470
    W, H = colw * 2 + 20, len(styles) * (big + 20) + 20
    out = Image.new('RGB', (W, H), bg)
    d = ImageDraw.Draw(out)
    try:
        font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 18)
    except Exception:
        font = ImageFont.load_default()
    for r, (st, label) in enumerate(styles):
        y = 20 + r * (big + 20)
        for c, fill in enumerate((0.95, 0.3)):
            x = 10 + c * colw
            col = hexc('#d02020') if c == 0 else hexc('#2a8adf')
            out.paste(render(big, fill, col, st, bg), (x, y))
            xm = x + big + 10
            for s in mids:
                out.paste(render(s, fill, col, st, bg), (xm, y + big - s - 30))
                xm += s + 10
            d.text((x + big + 10, y + 10), label if c == 0 else '', fill=fg, font=font)
            d.text((x + big + 10, y + 36), f'{int(fill * 100)}% full', fill=fg, font=font)
    out.save(path)


if __name__ == '__main__':
    o = sys.argv[1]
    sheet('#19191C', f'{o}/slots-dark.png')
    sheet('#F0F0F4', f'{o}/slots-light.png')
