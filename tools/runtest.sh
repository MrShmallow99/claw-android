#!/bin/bash
# usage: PORT_TEST_GAME=<game dir> tools/runtest.sh <seconds> [script]  -- headless run under Xvfb with frame dumps
# -> run/sheet.png (contact sheet), run/save/port.log.  Extra PORT_* variables pass through.
cd "$(dirname "$0")/.."
rm -rf run/frames && mkdir -p run/frames run/save
[ -n "$KEEP_SAVE" ] || { rm -rf run/save; mkdir -p run/save; }
PORT_HEADLESS=1 SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} PORT_DUMP_FRAMES=run/frames PORT_SCRIPT="$2" \
  xvfb-run -a -s "-screen 0 1280x1024x24" timeout -k 5 -s TERM "$1" ./build-host/gameport "${PORT_TEST_GAME:?set PORT_TEST_GAME to the game folder}" run/save \
  > run/stdout.txt 2>&1
tools/venv/bin/python - <<'PYEOF'
from PIL import Image
import glob
fs = sorted(glob.glob('run/frames/*.bmp'))
if fs:
    n = min(len(fs), 12)
    sel = [fs[int(i * (len(fs) - 1) / max(n - 1, 1))] for i in range(n)]
    sheet = Image.new('RGB', (320 * 4, 240 * ((n + 3) // 4)))
    for k, f in enumerate(sel):
        sheet.paste(Image.open(f).convert('RGB').resize((320, 240)), ((k % 4) * 320, (k // 4) * 240))
    sheet.save('run/sheet.png')
    print(len(fs), 'frames;', ' '.join(f[-9:-4] for f in sel))
PYEOF
grep -E "FATAL|FAULT|perf:" run/save/port.log | tail -3
