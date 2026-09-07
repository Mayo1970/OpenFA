import os
from PIL import Image

src = r"E:\Users\Matteo\Desktop\Ferrero\FreshAdventures\src\icon\icon.png"
out = r"E:\Users\Matteo\Desktop\Ferrero\FreshAdventures\src\icon\fa_icon_rgba.h"
S = 64
img = Image.open(src).convert("RGBA").resize((S, S), Image.LANCZOS)
data = img.tobytes()  # row-major RGBA, S*S*4 bytes

lines = []
lines.append("// Generated from src/icon/icon.png. %dx%d RGBA, top-down, straight alpha." % (S, S))
lines.append("// Regenerate: tools/gen_icon_header.py")
lines.append("#ifndef FA_ICON_RGBA_H")
lines.append("#define FA_ICON_RGBA_H")
lines.append("enum { FA_ICON_W = %d, FA_ICON_H = %d };" % (S, S))
lines.append("static const unsigned char fa_icon_rgba[%d] = {" % len(data))
row = []
for i, b in enumerate(data):
    row.append("%d," % b)
    if len(row) == 20:
        lines.append("".join(row))
        row = []
if row:
    lines.append("".join(row))
lines.append("};")
lines.append("#endif")
with open(out, "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", out, len(data), "bytes")
