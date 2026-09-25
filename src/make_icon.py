#!/usr/bin/env python3
"""生成启动器图标 app.ico（蓝底白放大镜）"""
from PIL import Image, ImageDraw
import os, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'app.ico'
base = 256
im = Image.new('RGBA', (base, base), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle([0, 0, base - 1, base - 1], radius=int(base * 0.19), fill=(31, 78, 121, 255))
w = int(base * 0.075)
cx, cy, rad = base * 0.42, base * 0.40, base * 0.245
d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], outline=(255, 255, 255, 255), width=w)
d.line([cx + rad * 0.72, cy + rad * 0.72, base * 0.80, base * 0.80], fill=(255, 255, 255, 255), width=w)
# 右下角一个红色小方块（对角标记）
s2 = int(base * 0.20)
d.rounded_rectangle([base - s2 - int(base * 0.09), base - s2 - int(base * 0.09),
                     base - int(base * 0.09), base - int(base * 0.09)],
                    radius=int(base * 0.04), fill=(220, 60, 60, 255))
im.save(out, sizes=[(256, 256), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
print('icon ->', out, os.path.getsize(out), 'bytes')
