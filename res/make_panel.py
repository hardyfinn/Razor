#!/usr/bin/env python3
# Draws res/Razor.svg. VCV Rack can't draw SVG <text>, so every label is
# turned into outline paths with fontTools. Positions are in mm and must match
# src/Razor.cpp. Label sizes follow the house standard: control labels
# 3.2 mm, jack labels 2.9 mm.
#   pip install fonttools
#   python3 res/make_panel.py
import math
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 142.24, 128.5  # 28 HP

BG = "#1E2023"
BG_LINE = "#26292D"
INK = "#EEE8DA"
SOFT = "#9A968E"
MARK = "#F2C94C"      # china-marker yellow, the colour tape editors mark cuts with
REV = "#F07A3A"
OUTBOX = "#121315"

CONTROL = 3.2
JACK = 2.9
HINT = 2.2

fonts = {}


def text(s, x, y, size, font=FONT_BOLD, fill=INK, anchor="middle", spacing=0.03):
    """Return an SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    if anchor == "middle":
        x0 = x - width * scale / 2
    elif anchor == "end":
        x0 = x - width * scale
    else:
        x0 = x
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        tp = TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y))
        gs[n].draw(tp)
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append


def tick(cx, cy, value, label, r0=7.4, r1=8.6, fill=INK, size=HINT):
    """A mark at a knob position (value 0..1 over Rack's -150..+150 degree sweep)."""
    a = math.radians(-150 + 300 * value)
    sx, sy = math.sin(a), -math.cos(a)
    add(f'<line x1="{cx + r0 * sx:.3f}" y1="{cy + r0 * sy:.3f}" x2="{cx + r1 * sx:.3f}" '
        f'y2="{cy + r1 * sy:.3f}" stroke="{fill}" stroke-width="0.4"/>')
    if label:
        rl = r1 + 2.0
        add(text(label, cx + rl * sx, cy + rl * sy + size * 0.36, size, FONT_MED, fill))


add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
# faint tape-width bands
for y in range(8, int(H), 4):
    add(f'<line x1="0" y1="{y}" x2="{W}" y2="{y}" stroke="{BG_LINE}" stroke-width="0.5"/>')

# title, with a razor-cut slash
add(text("RAZOR", 6.0, 10.6, 5.0, FONT_BOLD, INK, anchor="start", spacing=0.12))
add(f'<line x1="27.2" y1="11.2" x2="30.2" y2="5.6" stroke="{MARK}" stroke-width="0.7" stroke-linecap="round"/>')
add(text("cut  /  flip  /  splice", W - 6.0, 10.4, 2.6, FONT_MED, SOFT, anchor="end"))

# display window
add(f'<rect x="5.0" y="13.0" width="132.24" height="31.0" rx="1.6" fill="#0C0C0E"/>')

# ---- reverse buttons and their gates ----
lenX = [14.0, 31.0, 48.0, 65.0]
add(text("REVERSE THE NEXT ... BEATS", 39.5, 49.2, 2.6, FONT_BOLD, SOFT, spacing=0.06))
for x, lab in zip(lenX, ["½", "1", "2", "4"]):
    add(text(lab, x, 55.0, 4.2, FONT_BOLD, INK))
    add(text("gate", x, 81.0, JACK, FONT_BOLD, SOFT))
add(f'<line x1="6.5" y1="45.8" x2="72.5" y2="45.8" stroke="{MARK}" stroke-width="0.35"/>')

# ---- mode buttons ----
for x, lab, hints in [(90.0, "SNAP", ["cut just", "before hits"]),
                      (108.0, "BAR", ["start on", "next bar"]),
                      (126.0, "INSERT", ["add, then", "catch up"])]:
    add(text(lab, x, 55.0, CONTROL, FONT_BOLD, INK))
    for j, h in enumerate(hints):
        add(text(h, x, 69.6 + 2.6 * j, HINT, FONT_MED, SOFT))
add(f'<line x1="80.5" y1="45.8" x2="136.0" y2="45.8" stroke="{MARK}" stroke-width="0.35"/>')

# ---- knob row ----
knobY = 97.0
add(f'<line x1="6.5" y1="83.6" x2="136.0" y2="83.6" stroke="{MARK}" stroke-width="0.35"/>')
for x, lab in [(16.0, "CUT"), (40.0, "ANGLE"), (64.0, "PROB"), (86.0, "BPM")]:
    add(text(lab, x, 87.4, CONTROL, FONT_BOLD, INK))
add(text("CV", 106.0, 87.4, JACK, FONT_BOLD, INK))
add(text("CV TO", 126.0, 86.4, CONTROL, FONT_BOLD, INK))

tick(16.0, knobY, 0.0, "early")
tick(16.0, knobY, 1.0, "late")
tick(40.0, knobY, 0.0, "90°")
tick(40.0, knobY, 2.0 / 3.0, "45°", fill=MARK)
tick(40.0, knobY, 1.0, "")
tick(64.0, knobY, 0.0, "0")
tick(64.0, knobY, 1.0, "100")
tick(86.0, knobY, 0.0, "", r0=5.6, r1=6.6)
tick(86.0, knobY, 1.0, "", r0=5.6, r1=6.6)
for v, lab in [(0.0, "cut"), (0.5, "angle"), (1.0, "prob")]:
    tick(126.0, knobY, v, lab, r0=4.6, r1=5.4)
add(text("no clock only", 86.0, 105.0, HINT, FONT_MED, SOFT))

# ---- jacks ----
jackY = 118.0
add(f'<rect x="89.0" y="107.6" width="48.2" height="19.2" rx="1.2" fill="{OUTBOX}"/>')
for x, lab in [(12.0, "IN L"), (27.0, "IN R"), (45.0, "CLK"), (60.0, "RESET")]:
    add(text(lab, x, 111.6, JACK, FONT_BOLD, INK))
for x, lab in [(98.0, "OUT L"), (113.0, "OUT R"), (128.0, "REV")]:
    add(text(lab, x, 111.6, JACK, FONT_BOLD, INK))
add(text("one bar late", 105.5, 125.4, HINT, FONT_MED, SOFT))
add(text("gate", 128.0, 125.4, HINT, FONT_MED, SOFT))
add(text("mono: L only", 19.5, 125.4, HINT, FONT_MED, SOFT))

svg = (
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
    f'viewBox="0 0 {W} {H}">\n' + "\n".join(parts) + "\n</svg>\n"
)
with open(os.path.join(HERE, "Razor.svg"), "w") as fh:
    fh.write(svg)
print("wrote Razor.svg")
