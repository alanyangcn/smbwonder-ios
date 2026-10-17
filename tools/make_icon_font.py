#!/usr/bin/env python3
"""Button icon font standing in for the Switch shared font type 5 (Nintendo extension). That font lives in the system
firmware, which this offline port does not have; iOS has no face with these private-use glyphs, so text showed "?" for
every button icon. This draws the 27 private-use code points Wonder's message data uses (Mals/*.sarc.zs, round196 survey)
as plain icons. It is a functional substitute, not Nintendo's artwork (STATUS lists it as a difference).

Code points, from their use in USen text ("<stick><E092> in front of the door", "Press and hold <E0E4> + <E0E5>", ...):
  E090-E093 arrows left/right/up/down, E094-E097 small arrows up/right/left/down ("<E096><stick><E095> while crouching"),
  E0AB-E0AE direction buttons up/down/left/right, E0E0-E0E3 A B X Y, E0E4-E0E9 L R ZL ZR SL SR, E0EA D-pad,
  E0EF plus, E0F0 minus, E100-E101 control sticks.
Letters come from a TrueType face on the build Mac (Arial Bold by default); the output stays in work/.
Usage: make_icon_font.py [--letters <ttf>] --out <ttf>
"""
import argparse, math
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.ttLib import TTFont

EM, ASCENT, DESCENT = 1000, 880, -120
MID = 330                                  # icon centre height (about the middle of CJK ideographs on the baseline)

def circle(pen, cx, cy, r, clockwise=True):
    """Circle from 8 quadratic segments (TrueType outer contours run clockwise)."""
    n = 8; k = r / math.cos(math.pi / n)
    pts = [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    ctl = [(cx + k * math.cos(2 * math.pi * (i + 0.5) / n), cy + k * math.sin(2 * math.pi * (i + 0.5) / n)) for i in range(n)]
    if clockwise: pts, ctl = pts[::-1], ctl[::-1]; pts = pts[-1:] + pts[:-1]
    pen.moveTo(pts[0])
    for i in range(n): pen.qCurveTo(ctl[i], pts[(i + 1) % n])
    pen.closePath()

def rounded_rect(pen, x0, y0, x1, y1, r, clockwise=True):
    pts = [((x0 + r, y0), (x1 - r, y0), (x1, y0), (x1, y0 + r)), ((x1, y0 + r), (x1, y1 - r), (x1, y1), (x1 - r, y1)),
           ((x1 - r, y1), (x0 + r, y1), (x0, y1), (x0, y1 - r)), ((x0, y1 - r), (x0, y0 + r), (x0, y0), (x0 + r, y0))]
    path = []
    for a, b, c, d in pts: path += [('line', b), ('q', c, d)]
    if not clockwise:
        pen.moveTo((x0 + r, y0))
        for op in path: pen.lineTo(op[1]) if op[0] == 'line' else pen.qCurveTo(op[1], op[2])
        pen.closePath(); return
    rp = ReverseContourPen(pen); rp.moveTo((x0 + r, y0))
    for op in path: rp.lineTo(op[1]) if op[0] == 'line' else rp.qCurveTo(op[1], op[2])
    rp.closePath()

def polygon(pen, pts, clockwise=True):
    area = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))
    if (area < 0) != clockwise: pts = pts[::-1]
    pen.moveTo(pts[0])
    for p in pts[1:]: pen.lineTo(p)
    pen.closePath()

class Letters:
    def __init__(self, path):
        self.font = TTFont(path); self.glyphs = self.font.getGlyphSet(); self.cmap = self.font.getBestCmap()
        self.upm = self.font['head'].unitsPerEm
        self.cap = getattr(self.font['OS/2'], 'sCapHeight', 0) or self.upm * 0.716
    def width(self, text, height):
        s = height / self.cap
        return sum(self.glyphs[self.cmap[ord(ch)]].width for ch in text) * s
    def draw(self, pen, text, cx, cy, height, hole=True):
        """Text centred on (cx, cy), cap height `height`; hole=True reverses it to cut out of a filled shape."""
        s = height / self.cap; x = cx - self.width(text, height) / 2
        target = ReverseContourPen(pen) if hole else pen
        for ch in text:
            g = self.glyphs[self.cmap[ord(ch)]]
            g.draw(TransformPen(Cu2QuPen(target, 1.0, reverse_direction=False), (s, 0, 0, s, x, cy - height / 2)))
            x += g.width * s

def arrow(pen, cx, cy, size, angle):
    """Filled arrow pointing at `angle` degrees (0 = right)."""
    pts = [(0.5, 0), (0, 0.42), (0, 0.16), (-0.5, 0.16), (-0.5, -0.16), (0, -0.16), (0, -0.42)]
    a = math.radians(angle)
    polygon(pen, [(cx + size * (x * math.cos(a) - y * math.sin(a)), cy + size * (x * math.sin(a) + y * math.cos(a))) for x, y in pts])

def triangle(pen, cx, cy, size, angle, clockwise=True):
    a = math.radians(angle)
    pts = [(cx + size * math.cos(a + k * 2 * math.pi / 3), cy + size * math.sin(a + k * 2 * math.pi / 3)) for k in range(3)]
    polygon(pen, pts, clockwise)

def build(letters, out):
    glyphs, widths, cmap = {}, {}, {}
    def add(cp, name, width, draw):
        pen = TTGlyphPen(None); draw(pen); glyphs[name] = pen.glyph(); widths[name] = width; cmap[cp] = name
    def round_button(text):
        def d(pen): circle(pen, 500, MID, 440); letters.draw(pen, text, 500, MID, 440 if len(text) == 1 else 300)
        return d
    def shoulder(text, width):
        def d(pen): rounded_rect(pen, 40, MID - 300, width - 40, MID + 300, 150); letters.draw(pen, text, width / 2, MID, 330)
        return d
    for cp, t in zip(range(0xE0E0, 0xE0E4), 'ABXY'): add(cp, f'button{t}', 1000, round_button(t))
    for cp, t in zip(range(0xE0E4, 0xE0EA), ('L', 'R', 'ZL', 'ZR', 'SL', 'SR')): add(cp, f'shoulder{t}', 1300 if len(t) == 2 else 1100, shoulder(t, 1300 if len(t) == 2 else 1100))
    def plus_minus(sign):
        def d(pen):
            circle(pen, 500, MID, 440)
            polygon(pen, [(260, MID - 70), (740, MID - 70), (740, MID + 70), (260, MID + 70)], clockwise=False)
            if sign == '+': polygon(pen, [(430, MID - 240), (570, MID - 240), (570, MID - 70), (430, MID - 70)], clockwise=False); \
                            polygon(pen, [(430, MID + 70), (570, MID + 70), (570, MID + 240), (430, MID + 240)], clockwise=False)
        return d
    add(0xE0EF, 'buttonPlus', 1000, plus_minus('+')); add(0xE0F0, 'buttonMinus', 1000, plus_minus('-'))
    def dpad(pen):
        w, r = 150, 450
        polygon(pen, [(500 - w, MID - r), (500 + w, MID - r), (500 + w, MID - w), (500 + r, MID - w), (500 + r, MID + w), (500 + w, MID + w),
                      (500 + w, MID + r), (500 - w, MID + r), (500 - w, MID + w), (500 - r, MID + w), (500 - r, MID - w), (500 - w, MID - w)])
    add(0xE0EA, 'dpad', 1000, dpad)
    for cp, (name, angle) in zip(range(0xE0AB, 0xE0AF), (('Up', 90), ('Down', 270), ('Left', 180), ('Right', 0))):
        add(cp, f'direction{name}', 1000, lambda pen, a=angle: (circle(pen, 500, MID, 440), triangle(pen, 500, MID, 230, a, clockwise=False)))
    def stick(pen):
        circle(pen, 500, MID, 440); circle(pen, 500, MID, 330, clockwise=False); circle(pen, 500, MID, 220)
    add(0xE100, 'stickL', 1000, stick); add(0xE101, 'stickR', 1000, stick)
    for cp, (name, angle, size) in zip(range(0xE090, 0xE098), (('Left', 180, 820), ('Right', 0, 820), ('Up', 90, 820), ('Down', 270, 820),
                                                               ('SmallUp', 90, 560), ('SmallRight', 0, 560), ('SmallLeft', 180, 560), ('SmallDown', 270, 560))):
        add(cp, f'arrow{name}', 1000 if size > 600 else 700, lambda pen, a=angle, s=size: arrow(pen, (1000 if s > 600 else 700) / 2, MID, s, a))
    order = ['.notdef'] + sorted(glyphs)
    notdef = TTGlyphPen(None); glyphs['.notdef'] = notdef.glyph(); widths['.notdef'] = 500
    fb = FontBuilder(EM, isTTF=True)
    fb.setupGlyphOrder(order); fb.setupCharacterMap(cmap); fb.setupGlyf(glyphs)
    glyf = fb.font['glyf']
    for n in order: glyf[n].recalcBounds(glyf)
    fb.setupHorizontalMetrics({n: (widths[n], getattr(glyf[n], 'xMin', 0)) for n in order})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT)
    fb.setupNameTable({'familyName': 'Wonder Port Icons', 'styleName': 'Regular', 'psName': 'WonderPortIcons-Regular'})
    fb.setupOS2(sTypoAscender=ASCENT, sTypoDescender=DESCENT, usWinAscent=ASCENT, usWinDescent=-DESCENT)
    fb.setupPost(); fb.setupHead(unitsPerEm=EM)
    fb.save(out)
    return cmap

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--letters', default='/System/Library/Fonts/Supplemental/Arial Bold.ttf'); p.add_argument('--out', required=True)
    a = p.parse_args(); cmap = build(Letters(a.letters), a.out)
    print(f'{len(cmap)} icons -> {a.out}')
