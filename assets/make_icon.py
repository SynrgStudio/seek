# Generates assets/seek.ico and assets/seek-256.png: a magnifying glass with Jolt's lightning bolt in the lens.
# Requires Pillow.   python -I assets/make_icon.py
import math, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512]
SS = 8  # supersampling

BOLT = [(150, 72), (84, 152), (124, 152), (106, 216), (178, 128), (138, 128)]  # Jolt's, in 0..256 units
BOLT_CENTER = (131, 144)


def gradient(n):
    a, b = (0x6E, 0xEB, 0xFF), (0x3A, 0x6A, 0xFF)  # cyan to blue, top left to bottom right
    t = Image.linear_gradient("L").resize((n, n))
    mix = Image.blend(t, t.rotate(90), 0.5)
    return Image.merge("RGB", [mix.point(lambda v, c0=a[i], c1=b[i]: int(c0 + (c1 - c0) * v / 255)) for i in range(3)])


def draw(n):
    N = n * SS
    s = N / 256
    small = n <= 24
    cx, cy, r = (100, 100, 86) if small else (102, 102, 80)
    handle_w = 54 if small else 42
    bolt_k = 0.66 if small else 0.6
    shape = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(shape)
    d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=255)
    # the handle: a thick round-ended stroke out of the lens, toward the bottom right
    a = math.radians(45)
    x0, y0 = cx + (r - 10) * math.cos(a), cy + (r - 10) * math.sin(a)
    x1, y1 = 232, 232
    d.line([(x0 * s, y0 * s), (x1 * s, y1 * s)], fill=255, width=int(handle_w * s))
    rr = handle_w / 2
    d.ellipse([(x1 - rr) * s, (y1 - rr) * s, (x1 + rr) * s, (y1 + rr) * s], fill=255)
    # a dark gap between lens and handle, so the two read apart even small
    gap = Image.new("L", (N, N), 0)
    gd = ImageDraw.Draw(gap)
    gr = r + (9 if small else 7)
    gd.ellipse([(cx - gr) * s, (cy - gr) * s, (cx + gr) * s, (cy + gr) * s], outline=255, width=int((5 if small else 6) * s))
    # the bolt, centered in the lens
    bx, by = BOLT_CENTER
    bolt = [((cx + (x - bx) * bolt_k) * s, (cy + 4 + (y - by) * bolt_k) * s) for x, y in BOLT]
    hole = Image.new("L", (N, N), 0)
    ImageDraw.Draw(hole).polygon(bolt, fill=255)
    img = gradient(N).convert("RGBA")
    dark = Image.new("RGBA", (N, N), (0x17, 0x18, 0x1F, 255))
    img = Image.composite(dark, img, hole)
    alpha = Image.composite(Image.new("L", (N, N), 0), shape, gap)
    img.putalpha(alpha)
    return img.resize((n, n), Image.LANCZOS)


if __name__ == "__main__":
    imgs = {n: draw(n) for n in SIZES}
    imgs[256].save(os.path.join(HERE, "seek-256.png"))
    imgs[512].save(os.path.join(HERE, "seek-512.png"))
    ico = [imgs[n] for n in SIZES if n <= 256]
    ico[-1].save(os.path.join(HERE, "seek.ico"), sizes=[(i.width, i.height) for i in ico], append_images=ico[:-1])
    print("assets/seek.ico, seek-256.png, seek-512.png")
