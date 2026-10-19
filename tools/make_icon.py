#!/usr/bin/env python3
"""games/<name>/icon.png -> games/<name>/res: the Android launcher icon.

Adaptive icon (Android 8+): the art, scaled into the safe zone, on a background colour
(taken from the art's border, or --bg), so masks (circle, squircle) don't cut the picture.
Older Android: the square art at each density. app/build.gradle adds games/<name>/res
when it exists; builds with bundled game data prefer games/<name>/res-bundled.
usage: tools/make_icon.py games/claw [--src icon.png] [--res res] [--bg F2ECDC] [--nearest]
  --nearest: pixel art (small icons): whole-number nearest-neighbour scaling, kept crisp
"""
import sys, os, argparse
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('game')
ap.add_argument('--src', default='icon.png')
ap.add_argument('--res', default='res')
ap.add_argument('--bg')
ap.add_argument('--nearest', action='store_true')
a = ap.parse_args()
game = a.game
src = Image.open(os.path.join(game, a.src)).convert('RGBA')
res = os.path.join(game, a.res)


def scaled(img, size):
    """img scaled to fit size x size: nearest-neighbour by a whole factor for pixel art"""
    if a.nearest:
        k = max(1, size // max(img.size))
        return img.resize((img.size[0] * k, img.size[1] * k), Image.NEAREST)
    return img.resize((size, size), Image.LANCZOS)


# background: --bg, else the average of the outer 2% of the art
if a.bg:
    bg = tuple(int(a.bg.lstrip('#')[i:i + 2], 16) for i in (0, 2, 4))
else:
    w, h = src.size
    b = max(2, w // 50)
    px = [src.getpixel((x, y))[:3] for x in range(0, w, 4) for y in list(range(0, b)) + list(range(h - b, h))]
    bg = tuple(sum(p[i] for p in px) // len(px) for i in range(3))

# foreground: 108 dp layer, art at 72 dp (the visible part of the mask)
S = 432  # xxxhdpi
fg = Image.new('RGBA', (S, S), (0, 0, 0, 0))
# full-bleed art fills the 72 dp the masks show; art on a transparent background
# (pixel-art emblems) stays inside the 66 dp circle that every mask keeps
art = scaled(src, S * (66 if a.nearest else 72) // 108)
fg.alpha_composite(art, ((S - art.size[0]) // 2, (S - art.size[1]) // 2))
os.makedirs(os.path.join(res, 'drawable-nodpi'), exist_ok=True)
fg.save(os.path.join(res, 'drawable-nodpi', 'ic_launcher_fg.png'), optimize=True)

os.makedirs(os.path.join(res, 'drawable-anydpi-v26'), exist_ok=True)
with open(os.path.join(res, 'drawable-anydpi-v26', 'ic_launcher.xml'), 'w') as f:
    f.write('''<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_bg" />
    <foreground android:drawable="@drawable/ic_launcher_fg" />
</adaptive-icon>
''')
os.makedirs(os.path.join(res, 'values'), exist_ok=True)
with open(os.path.join(res, 'values', 'ic_launcher.xml'), 'w') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_bg">#%02X%02X%02X</color>\n</resources>\n' % bg)

# legacy (Android 7): 48 dp square art per density
for name, size in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    d = os.path.join(res, 'drawable-' + name)
    os.makedirs(d, exist_ok=True)
    tile = Image.new('RGBA', (size, size), bg + (255,))
    art = scaled(src, size)
    tile.alpha_composite(art, ((size - art.size[0]) // 2, (size - art.size[1]) // 2))
    tile.convert('RGB').save(os.path.join(d, 'ic_launcher.png'), optimize=True)
print('background #%02X%02X%02X' % bg)
