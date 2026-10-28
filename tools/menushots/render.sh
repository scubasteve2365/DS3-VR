#!/bin/sh
# Renders the VR menu's three tabs to docs/images/menu-*.png on Linux, from the menu's own UI code in
# src/menu.cpp (stubbed mod state, software rasterizer). Needs g++, python3, numpy and Pillow.
# Run from the repository root:  sh tools/menushots/render.sh
set -e
R=$(pwd)
T=$(mktemp -d)
python3 - "$R/src/menu.cpp" "$T" <<'PY'
import sys
src = open(sys.argv[1]).read().split('\n')
s = src.index('    ImGui::StyleColorsDark();')
e = next(i for i in range(s, len(src)) if 'ImGuiCol_CheckMark' in src[i])
open(sys.argv[2] + '/style_extract.inc', 'w').write('\n'.join(src[s:e + 1]))
a = next(i for i, l in enumerate(src) if l.startswith('int Stepper('))
a -= 1  # its comment line
b = next(i for i, l in enumerate(src) if l.startswith('void Render('))
ui = '\n'.join(src[a:b])
x = ui.index('void SetDevKeys(bool on) {'); y = ui.index('}\n', x) + 2
ui = ui[:x] + 'void SetDevKeys(bool on) { g_dev = on; }\n' + ui[y:]
open(sys.argv[2] + '/ui_extract.inc', 'w').write(ui)
PY
I="$R/third_party/imgui"
g++ -O2 -std=c++17 -I"$T" -I"$I" -I"$R/src" "$R/tools/menushots/harness.cpp" "$I/imgui.cpp" "$I/imgui_draw.cpp" \
    "$I/imgui_widgets.cpp" "$I/imgui_tables.cpp" -o "$T/harness"
(cd "$T" && ./harness)
python3 - "$T" "$R/docs/images" <<'PY'
import sys
import numpy as np
from PIL import Image
for n in ['general', 'graphics', 'developer']:
    a = np.fromfile(f'{sys.argv[1]}/{n}.rgba', dtype=np.uint8).reshape(1250, 1280, 4).astype(np.float32) / 255
    al = a[..., 3:4]
    rgb = np.where(al > 0, a[..., :3] / np.maximum(al, 1e-6), 0)
    out = np.concatenate([np.clip(rgb, 0, 1), al], -1)
    Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA').save(f'{sys.argv[2]}/menu-{n}.png', optimize=True)
    print('wrote', f'docs/images/menu-{n}.png')
PY
rm -rf "$T"
