#!/usr/bin/env python3
"""Makes the apps' icon files from tools/icons.js's PNGs (build/icons):

  windows/res/dmodem.ico   16, 32 and 48 in 256 colours (Windows 9x) and
                           in 32-bit colour with alpha (XP on), 256 as PNG
  macos/.../AppIcon.appiconset   the sizes an Asset Catalog wants

The .ico is written by hand: Windows 95 reads only BMP entries."""
import io
import json
import os
import struct

from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), '..')
SRC = os.path.join(ROOT, 'build', 'icons')


def png(size):
    return Image.open(os.path.join(SRC, f'{size}.png')).convert('RGBA')


def and_mask(img):
    w, h = img.size
    stride = ((w + 31) // 32) * 4
    rows = []
    for y in range(h - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(w):
            if img.getpixel((x, y))[3] < 128:
                bits[x // 8] |= 0x80 >> (x % 8)
        rows.append(bytes(bits))
    return b''.join(rows)


def dib_32(img):
    w, h = img.size
    header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            px += bytes((b, g, r, a))
    return header + bytes(px) + and_mask(img)


def dib_8(img):
    w, h = img.size
    # 255 colours of the opaque pixels, on black where it is clear
    flat = Image.new('RGB', img.size, (0, 0, 0))
    flat.paste(img, mask=img.split()[3])
    q = flat.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
    pal = q.getpalette()[:256 * 3]
    pal += [0] * (256 * 3 - len(pal))
    header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 8, 0, 0, 0, 0, 256, 0)
    table = bytearray()
    for i in range(256):
        r, g, b = pal[i * 3:i * 3 + 3]
        table += bytes((b, g, r, 0))
    stride = (w + 3) & ~3
    px = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytes(q.getpixel((x, y)) for x in range(w))
        px += row + bytes(stride - w)
    return header + bytes(table) + bytes(px) + and_mask(img)


def ico(path):
    entries = []
    for size in (16, 32, 48):
        img = png(size)
        entries.append((size, 8, 256, dib_8(img)))
    for size in (16, 32, 48):
        entries.append((size, 32, 0, dib_32(png(size))))
    b = io.BytesIO()
    png(256).save(b, 'PNG')
    entries.append((256, 32, 0, b.getvalue()))
    head = struct.pack('<HHH', 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    dirs, data = b'', b''
    for size, bpp, colours, blob in entries:
        dirs += struct.pack('<BBBBHHII', size % 256, size % 256,
                            colours % 256, 0, 1, bpp, len(blob), offset)
        offset += len(blob)
        data += blob
    with open(path, 'wb') as f:
        f.write(head + dirs + data)


def appiconset(dir):
    os.makedirs(dir, exist_ok=True)
    images = []
    for pt in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = pt * scale
            name = f'icon_{pt}x{pt}{"@2x" if scale == 2 else ""}.png'
            png(px).save(os.path.join(dir, name))
            images.append({'idiom': 'mac', 'size': f'{pt}x{pt}',
                           'scale': f'{scale}x', 'filename': name})
    with open(os.path.join(dir, 'Contents.json'), 'w') as f:
        json.dump({'images': images, 'info': {'version': 1, 'author': 'xcode'}},
                  f, indent=2)
        f.write('\n')


if __name__ == '__main__':
    ico(os.path.join(ROOT, 'windows', 'res', 'dmodem.ico'))
    appiconset(os.path.join(ROOT, 'macos', 'DModem', 'Assets.xcassets',
                            'AppIcon.appiconset'))
