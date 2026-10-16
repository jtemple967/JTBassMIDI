#!/usr/bin/env python3
"""Generate the MOD GUI image assets for JTBassMIDI.

Writes into ../modgui/:
  panel.png        pedal face (enclosure, silkscreen, footswitch, LED bezel)
  knob.png         64-frame sprite, large knob (56 px frames)
  button.png       2-frame sprite, Hold / Release push button (off, on; 40 px frames)
  toggle.png       2-frame sprite, Latch bat toggle (off, on; 40 px frames)
  screenshot-jtbassmidi.png / thumbnail-jtbassmidi.png

Same drawing style as JTBassSynth (knobs, screws, footswitch), with a navy
enclosure and amber accent so the two read as a family on a pedalboard.

Requires Pillow. Layout constants below must match stylesheet-jtbassmidi.css.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "modgui"))
S = 3  # supersampling factor

W, H = 360, 600
ARC = 135.0  # knob travel: -135 .. +135 degrees

BIG = 56
KNOB_FRAMES = 64

#        symbol         label      cx   cy   min    max   default
KNOBS = [
    ("sensitivity", "GATE",      52,  70, -80.0, -10.0, -48.0),
    ("dynamics",    "DYNAMICS", 136,  70,   0.0,   1.0,   0.7),
    ("bend_range",  "BEND",     224,  70,   0.0,  24.0,   2.0),
    ("thru",        "THRU",     308,  70,   0.0,   1.0,   1.0),
    ("channel",     "CHANNEL",  136, 172,   1.0,  16.0,   1.0),
    ("transpose",   "TRANSPOSE", 224, 172, -24.0, 24.0,   0.0),
]
#          symbol     label      cx   cy
BUTTONS = [("hold",    "HOLD",    52, 172),
           ("release", "RELEASE", 308, 172)]
BTN = 40
TOGGLE = (180, 284)   # Latch switch centre; 40 px frames like the buttons
LED = (60, 530)
FOOTSWITCH = (180, 530, 30)

ENCLOSURE = (22, 28, 44)
INK = (238, 236, 226)
ACCENT = (255, 176, 59)


def font(size, names=("arialbd.ttf", "DejaVuSans-Bold.ttf", "Arial Bold.ttf")):
    dirs = ["C:/Windows/Fonts", "/usr/share/fonts/truetype/dejavu",
            "/Library/Fonts", "/System/Library/Fonts/Supplemental"]
    for n in names:
        for d in dirs:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default(size)


def logo_font(size):
    return font(size, ("impact.ttf", "Impact.ttf", "DejaVuSans-Bold.ttf"))


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.sin(a), cy - r * math.cos(a)


def lerp(c1, c2, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c1, c2))


def shaded_disc(d, cx, cy, r, light, dark, steps=40):
    """Disc with a soft highlight toward the top-left."""
    for i in range(steps):
        t = i / (steps - 1)
        rr = r * (1.0 - 0.55 * t)
        ox, oy = -0.25 * r * t, -0.3 * r * t
        d.ellipse([cx + ox - rr, cy + oy - rr, cx + ox + rr, cy + oy + rr],
                  fill=lerp(dark, light, t))


def drop_shadow(size, cx, cy, r, offset, blur, alpha=150):
    sh = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - r, cy - r + offset, cx + r, cy + r + offset],
                               fill=(0, 0, 0, alpha))
    return sh.filter(ImageFilter.GaussianBlur(blur))


# ------------------------------------------------------------------ knobs

def knob_frame(size, angle):
    n = size * S
    c = n / 2
    img = drop_shadow((n, n), c, c, n * 0.44, n * 0.03, n * 0.03)
    d = ImageDraw.Draw(img)

    # knurled skirt
    r = n * 0.45
    d.ellipse([c - r, c - r, c + r, c + r], fill=(12, 12, 14))
    for k in range(36):
        a = angle + k * 10.0
        x1, y1 = polar(c, c, r * 0.80, a)
        x2, y2 = polar(c, c, r * 0.98, a)
        d.line([x1, y1, x2, y2], fill=(46, 46, 52), width=max(1, int(n * 0.018)))

    # cap
    shaded_disc(d, c, c, n * 0.33, (78, 80, 88), (16, 16, 19))

    # pointer
    w = max(2, int(n * 0.055))
    x1, y1 = polar(c, c, n * 0.10, angle)
    x2, y2 = polar(c, c, n * 0.43, angle)
    d.line([x1, y1, x2, y2], fill=INK, width=w)
    for x, y in ((x1, y1), (x2, y2)):
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=INK)
    return img.resize((size, size), Image.LANCZOS)


def button_frame(size, lit):
    """Round push button in a chrome bezel; the 'on' frame lights an amber ring
    and sits the cap slightly lower, as if pressed."""
    n = size * S
    c = n / 2
    img = drop_shadow((n, n), c, c, n * 0.42, n * 0.03, n * 0.03)
    d = ImageDraw.Draw(img)
    shaded_disc(d, c, c, n * 0.44, (215, 218, 224), (105, 108, 116), 24)
    r = n * 0.34
    if lit:
        glow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse([c - r * 1.08, c - r * 1.08, c + r * 1.08, c + r * 1.08],
                                     fill=ACCENT + (230,))
        img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(n * 0.02)))
        d = ImageDraw.Draw(img)
    else:
        d.ellipse([c - r * 1.06, c - r * 1.06, c + r * 1.06, c + r * 1.06], fill=(40, 34, 26))
    oy = n * 0.015 if lit else 0
    shaded_disc(d, c, c + oy, r * 0.88, (74, 76, 84) if not lit else (60, 62, 70), (14, 14, 17))
    return img.resize((size, size), Image.LANCZOS)


def toggle_frame(size, right):
    """Mini bat-handle toggle seen from above: hex nut, chrome bushing and a
    lever pointing left (Note Change) or right (Latch)."""
    n = size * S
    c = n / 2
    img = drop_shadow((n, n), c, c, n * 0.30, n * 0.03, n * 0.03)
    d = ImageDraw.Draw(img)
    hexagon = [polar(c, c, n * 0.30, 30 + 60 * k) for k in range(6)]
    d.polygon(hexagon, fill=(150, 153, 160), outline=(70, 72, 78))
    shaded_disc(d, c, c, n * 0.19, (225, 228, 234), (110, 113, 120), 20)
    ang = 90 if right else -90
    tip = polar(c, c, n * 0.46, ang)
    w = n * 0.11
    d.line([(c, c), tip], fill=(205, 208, 214), width=int(w))
    d.ellipse([tip[0] - w * 0.75, tip[1] - w * 0.75, tip[0] + w * 0.75, tip[1] + w * 0.75],
              fill=(235, 237, 242), outline=(90, 92, 98))
    d.ellipse([c - n * 0.07, c - n * 0.07, c + n * 0.07, c + n * 0.07], fill=(60, 62, 68))
    return img.resize((size, size), Image.LANCZOS)


def sprite(frames):
    fw, fh = frames[0].size
    out = Image.new("RGBA", (fw * len(frames), fh), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        out.paste(f, (i * fw, 0), f)
    return out


# ------------------------------------------------------------------ panel

def panel():
    n_w, n_h = W * S, H * S
    img = Image.new("RGBA", (n_w, n_h), (0, 0, 0, 0))
    rad = 16 * S

    mask = Image.new("L", (n_w, n_h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, n_w - 1, n_h - 1], rad, fill=255)

    # textured (wrinkle-finish) enclosure
    base = Image.new("RGB", (n_w, n_h), ENCLOSURE)
    noise = Image.effect_noise((n_w // 2, n_h // 2), 26).resize((n_w, n_h)).convert("RGB")
    base = Image.blend(base, noise, 0.06)
    shade = Image.linear_gradient("L").resize((n_w, n_h))
    base = Image.composite(base, Image.new("RGB", (n_w, n_h), (8, 10, 16)),
                           shade.point(lambda v: 255 - v // 3))
    img.paste(base, (0, 0), mask)

    d = ImageDraw.Draw(img)
    d.rounded_rectangle([S, S, n_w - 1 - S, n_h - 1 - S], rad, outline=(62, 70, 92), width=2 * S)
    d.rounded_rectangle([4 * S, 4 * S, n_w - 1 - 4 * S, n_h - 1 - 4 * S], rad - 3 * S,
                        outline=(10, 10, 14), width=S)

    # corner screws
    for sx, sy in ((14, 14), (W - 14, 14), (14, H - 14), (W - 14, H - 14)):
        x, y, r = sx * S, sy * S, 5 * S
        shaded_disc(d, x, y, r, (200, 202, 208), (90, 92, 98), 16)
        d.line([x - r * 0.7, y - r * 0.7, x + r * 0.7, y + r * 0.7], fill=(50, 50, 55), width=S)

    f_lbl = font(11 * S)
    f_sm = font(9 * S)

    # knob ticks + labels
    for _, label, cx, cy, *_ in KNOBS:
        x, y = cx * S, cy * S
        r_tick = (BIG / 2 + 5) * S
        for k in range(11):
            a = -ARC + k * (2 * ARC / 10)
            px, py = polar(x, y, r_tick, a)
            rr = (1.6 if k in (0, 10) else 1.0) * S
            d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=INK)
        d.text((x, y + (BIG / 2 + 16) * S), label, font=f_lbl, fill=INK, anchor="mm")

    # end-of-travel legends where the numbers matter
    def legend(cx, cy, lo, hi):
        for a, txt in ((-ARC, lo), (ARC, hi)):
            px, py = polar(cx * S, cy * S, (BIG / 2 + 13) * S, a)
            d.text((px, py), txt, font=f_sm, fill=ACCENT, anchor="mm")
    legend(224, 70, "OFF", "24")
    legend(136, 172, "1", "16")
    legend(224, 172, "-24", "+24")

    # push-button labels (buttons are 40 px; labels sit where knob labels do)
    for _, label, cx, cy in BUTTONS:
        d.text((cx * S, (cy + BIG / 2 + 16) * S), label, font=f_lbl, fill=INK, anchor="mm")

    # Latch switch legends
    tx, ty = TOGGLE
    d.text(((tx - 26) * S, ty * S), "OFF", font=f_sm, fill=INK, anchor="rm")
    d.text(((tx + 26) * S, ty * S), "ON", font=f_sm, fill=INK, anchor="lm")
    d.text((tx * S, (ty + 30) * S), "LATCH", font=f_lbl, fill=ACCENT, anchor="mm")

    # divider rules
    for yy in (238, 448):
        d.line([24 * S, yy * S, (W - 24) * S, yy * S], fill=(50, 58, 78), width=S)

    # logo
    d.text((W / 2 * S, 376 * S), "J T", font=font(13 * S), fill=ACCENT, anchor="mm")
    d.text((W / 2 * S, 408 * S), "BASS MIDI", font=logo_font(38 * S), fill=INK, anchor="mm")

    # LED bezel
    lx, ly = LED[0] * S, LED[1] * S
    shaded_disc(d, lx, ly, 8 * S, (210, 212, 218), (100, 102, 108), 16)
    d.ellipse([lx - 5 * S, ly - 5 * S, lx + 5 * S, ly + 5 * S], fill=(40, 6, 6))
    d.text((lx, ly + 18 * S), "ON", font=f_sm, fill=INK, anchor="mm")

    # footswitch: hex nut + domed cap
    fx, fy, fr = (v * S for v in FOOTSWITCH)
    img.alpha_composite(drop_shadow((n_w, n_h), fx, fy, fr, 3 * S, 4 * S, 170))
    d = ImageDraw.Draw(img)
    hexagon = [polar(fx, fy, fr, 30 + 60 * k) for k in range(6)]
    d.polygon(hexagon, fill=(150, 153, 160), outline=(70, 72, 78))
    shaded_disc(d, fx, fy, fr * 0.66, (235, 237, 242), (110, 113, 120))
    d.ellipse([fx - fr * 0.66, fy - fr * 0.66, fx + fr * 0.66, fy + fr * 0.66],
              outline=(80, 82, 88), width=S)

    return img.resize((W, H), Image.LANCZOS)


def frame_for(value, lo, hi, frames):
    t = (value - lo) / (hi - lo)
    return int(round(max(0.0, min(1.0, t)) * (frames - 1)))


def main():
    os.makedirs(OUT, exist_ok=True)

    angles = [-ARC + k * (2 * ARC / (KNOB_FRAMES - 1)) for k in range(KNOB_FRAMES)]
    big = sprite([knob_frame(BIG, a) for a in angles])
    btn = sprite([button_frame(BTN, False), button_frame(BTN, True)])
    tog = sprite([toggle_frame(BTN, False), toggle_frame(BTN, True)])
    face = panel()

    big.save(os.path.join(OUT, "knob.png"), optimize=True)
    btn.save(os.path.join(OUT, "button.png"), optimize=True)
    tog.save(os.path.join(OUT, "toggle.png"), optimize=True)
    face.save(os.path.join(OUT, "panel.png"), optimize=True)

    # screenshot: face with every control at its default value, LED lit
    shot = face.copy()
    for _, _, cx, cy, lo, hi, dflt in KNOBS:
        f = frame_for(dflt, lo, hi, KNOB_FRAMES)
        tile = big.crop((f * BIG, 0, (f + 1) * BIG, BIG))
        shot.alpha_composite(tile, (cx - BIG // 2, cy - BIG // 2))
    for _, _, cx, cy in BUTTONS:  # both default to off
        shot.alpha_composite(btn.crop((0, 0, BTN, BTN)), (cx - BTN // 2, cy - BTN // 2))
    shot.alpha_composite(tog.crop((0, 0, BTN, BTN)), (TOGGLE[0] - BTN // 2, TOGGLE[1] - BTN // 2))

    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    lx, ly = LED
    gd.ellipse([lx - 9, ly - 9, lx + 9, ly + 9], fill=(255, 40, 30, 120))
    glow = glow.filter(ImageFilter.GaussianBlur(4))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([lx - 5, ly - 5, lx + 5, ly + 5], fill=(255, 60, 45, 255))
    gd.ellipse([lx - 2.5, ly - 3, lx + 1, ly], fill=(255, 190, 180, 255))
    shot.alpha_composite(glow)

    shot.save(os.path.join(OUT, "screenshot-jtbassmidi.png"), optimize=True)
    th_w = 64
    shot.resize((th_w, round(H * th_w / W)), Image.LANCZOS).save(
        os.path.join(OUT, "thumbnail-jtbassmidi.png"), optimize=True)
    print("wrote assets to", OUT)


if __name__ == "__main__":
    main()
