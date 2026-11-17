#!/usr/bin/env python3
"""PNG previews of the screen: tyr8.svg with the matrices and lines the host test recorded (frames.json).
    python3 render_frames.py tyr8.svg frames.json OUTDIR
A plain software rasteriser (standard library only): good for checking layout and motion, not the OP-1's pixels."""
import json, math, re, struct, sys, zlib
import xml.etree.ElementTree as ET

W, H, Z = 320, 160, 3                       # Z: preview zoom
NS = '{http://www.w3.org/2000/svg}'


def shapes(svg):
    """-> [(node id or None, [(x0, y0, x1, y1), ...], width, (r, g, b), fill (r, g, b) or None)]"""
    out = []
    def walk(el, gid):
        for c in el:
            tag = c.tag.replace(NS, '')
            if tag == 'g':
                walk(c, c.get('id') or gid)
                continue
            col = c.get('stroke', '#FFFFFF')
            rgb = tuple(int(col[i:i + 2], 16) for i in (1, 3, 5))
            w = float(c.get('stroke-width', 1))
            if tag == 'line':
                segs = [tuple(float(c.get(k)) for k in ('x1', 'y1', 'x2', 'y2'))]
            elif tag == 'circle':
                cx, cy, r = (float(c.get(k)) for k in ('cx', 'cy', 'r'))
                n = max(12, int(r * 1.5))
                p = [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]
                segs = [(*p[i], *p[i + 1]) for i in range(n)]
            elif tag == 'polyline':
                v = [float(x) for x in c.get('points').replace(',', ' ').split()]
                p = list(zip(v[0::2], v[1::2]))
                segs = [(*p[i], *p[i + 1]) for i in range(len(p) - 1)]
            elif tag == 'path':                   # M, then cubics, Z: flattened
                v = [float(x) for x in re.findall(r'-?\d+(?:\.\d+)?', c.get('d'))]
                p, cur = [(v[0], v[1])], (v[0], v[1])
                for i in range(2, len(v), 6):
                    b = [cur, (v[i], v[i + 1]), (v[i + 2], v[i + 3]), (v[i + 4], v[i + 5])]
                    for t in (j / 12 for j in range(1, 13)):
                        u = 1 - t
                        p.append(tuple(u ** 3 * b[0][k] + 3 * u * u * t * b[1][k] + 3 * u * t * t * b[2][k] + t ** 3 * b[3][k] for k in (0, 1)))
                    cur = b[3]
                segs = [(*p[i], *p[i + 1]) for i in range(len(p) - 1)]
            else:
                continue
            fc = c.get('fill', 'none')
            fill = None if fc == 'none' else tuple(int(fc[i:i + 2], 16) for i in (1, 3, 5))
            out.append((c.get('id') or gid, segs, w, rgb, fill))
    walk(ET.parse(svg).getroot(), None)
    return out


def draw(img, x0, y0, x1, y1, w, rgb):
    x0, y0, x1, y1, r = x0 * Z, y0 * Z, x1 * Z, y1 * Z, max(w * Z / 2, 0.6)
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    for py in range(max(0, int(min(y0, y1) - r - 1)), min(H * Z, int(max(y0, y1) + r + 2))):
        for px in range(max(0, int(min(x0, x1) - r - 1)), min(W * Z, int(max(x0, x1) + r + 2))):
            t = 0 if L2 == 0 else max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / L2))
            d = math.hypot(px - x0 - t * dx, py - y0 - t * dy)
            a = max(0.0, min(1.0, r + 0.5 - d))
            if a > 0:
                o = (py * W * Z + px) * 3
                for k in range(3):
                    img[o + k] = int(img[o + k] * (1 - a) + rgb[k] * a)


def fill_poly(img, pts, rgb):
    """even-odd scanline fill of a closed polygon (screen px)"""
    pts = [(x * Z, y * Z) for x, y in pts]
    for py in range(max(0, int(min(y for _, y in pts))), min(H * Z, int(max(y for _, y in pts)) + 1)):
        xs = sorted(x0 + (py + 0.5 - y0) * (x1 - x0) / (y1 - y0) for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1])
                    if (y0 <= py + 0.5) != (y1 <= py + 0.5))
        for xa, xb in zip(xs[0::2], xs[1::2]):
            for px in range(max(0, int(xa + 0.5)), min(W * Z, int(xb + 0.5))):
                o = (py * W * Z + px) * 3
                img[o:o + 3] = bytes(rgb)


def png(path, img):
    raw = b''.join(b'\0' + bytes(img[y * W * Z * 3:(y + 1) * W * Z * 3]) for y in range(H * Z))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * Z, H * Z, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main(svg, frames, outdir):
    art, seen, n = shapes(svg), set(), 0
    for fr in json.load(open(frames)):
        if fr['label'] in seen:
            continue
        seen.add(fr['label'])
        img = bytearray(W * Z * H * Z * 3)
        for gid, segs, w, rgb, fill in art:
            m = fr['nodes'].get(gid)
            if m:                               # row vectors, Q10: x' = x a + y c + tx
                segs = [((x0 * m[0] + y0 * m[3] + m[6]) / 1024, (x0 * m[1] + y0 * m[4] + m[7]) / 1024,
                         (x1 * m[0] + y1 * m[3] + m[6]) / 1024, (x1 * m[1] + y1 * m[4] + m[7]) / 1024) for x0, y0, x1, y1 in segs]
            if fill is not None:
                fill_poly(img, [(x0, y0) for x0, y0, _, _ in segs], fill)
            for x0, y0, x1, y1 in segs:
                draw(img, x0, y0, x1, y1, w, rgb)
        for x0, y0, x1, y1, w, r, g, b in fr['lines']:
            draw(img, x0 / 1024, y0 / 1024, x1 / 1024, y1 / 1024, w / 1024, (r, g, b))
        name = f'{outdir}/screen_{n:02d}.png'
        png(name, img)
        print(name, '-', fr['label'])
        n += 1


if __name__ == '__main__':
    main(*sys.argv[1:4])
