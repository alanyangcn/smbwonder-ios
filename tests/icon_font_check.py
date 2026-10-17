"""Helper for test_shared_font.py (runs in work/tools/venv): builds the icon font and prints how many of the message
data's private-use code points it covers with outlines, then the missing ones ("none")."""
import os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import make_icon_font
from fontTools.ttLib import TTFont
out = os.path.join(tempfile.mkdtemp(), 'icons.ttf')
make_icon_font.build(make_icon_font.Letters('/System/Library/Fonts/Supplemental/Arial Bold.ttf'), out)
font = TTFont(out); cmap = font.getBestCmap(); glyf = font['glyf']
want = list(range(0xE090, 0xE098)) + list(range(0xE0AB, 0xE0AF)) + list(range(0xE0E0, 0xE0EB)) + [0xE0EF, 0xE0F0, 0xE100, 0xE101]
missing = [hex(c) for c in want if c not in cmap or glyf[cmap[c]].numberOfContours <= 0]
print(len(want), ' '.join(missing) or 'none')
