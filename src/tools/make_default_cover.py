"""生成播放页默认封面（程序化抽象图，无版权争议、无 AI 水印）。

思路：深色底 + 若干条高斯模糊的彩色飘带（screen 叠加）+ 中心柔光 + 散景光点 + 轻噪声 + 暗角。
颜色取自应用主题（青 / 蓝紫 / 品红），浅色与深色皮肤都能融。
运行：python src/tools/make_default_cover.py
"""
import math
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter

W = H = 1200
SCALE = 2
w, h = W * SCALE, H * SCALE

random.seed(20261109)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def screen(a, b):
    return ImageChops.screen(a, b)


# ---------- 底：斜向渐变 ----------
base = Image.new("RGB", (w, h))
d = ImageDraw.Draw(base)
top, mid, bot = (9, 12, 26), (14, 15, 34), (24, 13, 30)
for y in range(h):
    t = y / h
    c = lerp(top, mid, t / 0.5) if t < 0.5 else lerp(mid, bot, (t - 0.5) / 0.5)
    d.line([(0, y), (w, y)], fill=c)
canvas = base

# ---------- 飘带（细而多，向右下汇聚） ----------
RIBBONS = [
    ((43, 217, 196), 26, 0.26, 0.26, 0.9),
    ((122, 107, 255), 34, 0.30, 0.52, -1.1),
    ((255, 111, 165), 22, 0.24, 0.74, 0.45),
    ((89, 194, 255), 18, 0.22, 0.40, 1.5),
    ((43, 217, 196), 14, 0.20, 0.62, -0.4),
    ((150, 120, 255), 16, 0.18, 0.86, 1.9),
]


def ribbon_mask(width, amp, y0, phase):
    layer = Image.new("L", (w, h), 0)
    dd = ImageDraw.Draw(layer)
    top_pts, bot_pts = [], []
    steps = 180
    for i in range(steps + 1):
        u = i / steps
        x = w * u
        y = h * y0 + h * amp * math.sin(u * math.pi * 1.5 + phase) \
            + h * amp * 0.28 * math.sin(u * math.pi * 3.7 + phase * 1.7)
        half = width * SCALE * (0.55 + 0.55 * math.sin(u * math.pi * 2.4 + phase))
        top_pts.append((x, y - half))
        bot_pts.append((x, y + half))
    dd.polygon(top_pts + bot_pts[::-1], fill=255)
    return layer


for color, width, amp, y0, phase in RIBBONS:
    mask = ribbon_mask(width, amp, y0, phase)
    core = mask.filter(ImageFilter.GaussianBlur(width * SCALE * 0.40))
    glow = mask.filter(ImageFilter.GaussianBlur(width * SCALE * 1.8)).point(lambda v: int(v * 0.42))
    tint = Image.new("RGB", (w, h), color)
    canvas = screen(canvas, Image.composite(tint, Image.new("RGB", (w, h), (0, 0, 0)), glow))
    canvas = screen(canvas, Image.composite(tint, Image.new("RGB", (w, h), (0, 0, 0)), core))

# ---------- 中心偏左柔光 ----------
glow = Image.new("L", (w, h), 0)
gd = ImageDraw.Draw(glow)
cx, cy, r = int(w * 0.42), int(h * 0.46), int(w * 0.42)
gd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=70)
glow = glow.filter(ImageFilter.GaussianBlur(w * 0.12))
canvas = screen(canvas, Image.composite(Image.new("RGB", (w, h), (96, 130, 190)),
                                        Image.new("RGB", (w, h), (0, 0, 0)), glow))

# ---------- 散景 ----------
bokeh = Image.new("RGB", (w, h), (0, 0, 0))
bd = ImageDraw.Draw(bokeh)
for _ in range(34):
    rad = random.randint(14, 70) * SCALE // 2
    x, y = random.randint(0, w), random.randint(0, h)
    col = random.choice([(43, 217, 196), (122, 107, 255), (255, 111, 165), (210, 225, 255)])
    a = random.randint(8, 30)
    bd.ellipse([x - rad, y - rad, x + rad, y + rad], fill=tuple(int(c * a / 255) for c in col))
bokeh = bokeh.filter(ImageFilter.GaussianBlur(16))
canvas = screen(canvas, bokeh)

# ---------- 暗角（按到中心的归一化距离压暗，四角最暗） ----------
import numpy as np

yy, xx = np.mgrid[0:h, 0:w]
dist = np.sqrt(((xx - w / 2) / (w / 2)) ** 2 + ((yy - h / 2) / (h / 2)) ** 2) / math.sqrt(2)
fall = np.clip(1.0 - 0.85 * np.clip(dist, 0, 1) ** 2.2, 0, 1)
vig = Image.fromarray((fall * 255).astype("uint8"), "L").filter(ImageFilter.GaussianBlur(w // 40))
dark = Image.new("RGB", (w, h), (5, 7, 15))
canvas = Image.composite(canvas, dark, vig)

# ---------- 噪声 ----------
noise = Image.effect_noise((w, h), 10).convert("L")
canvas = Image.blend(canvas, Image.merge("RGB", (noise, noise, noise)), 0.03)

canvas = canvas.resize((W, H), Image.LANCZOS)
canvas.save("src/ui/art/default_cover.jpg", quality=90, optimize=True)
print("written src/ui/art/default_cover.jpg")
