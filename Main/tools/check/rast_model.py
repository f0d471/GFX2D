#!/usr/bin/env python3
"""R1 的 Python 整数/有理数模型；不读取 C 运算结果作为期望。"""
from fractions import Fraction
import struct

ONE = 65536
GRADS = (44, 56, 64, 96, 104, 112)


def rnd(n, d=ONE):
    assert d > 0
    return (n + d // 2) // d


def sat(n):
    return min(2147483647, max(-2147483648, n))


def div(a, b):
    if not b:
        return 2147483647 if a > 0 else -2147483648 if a < 0 else 0
    return sat(rnd(a * ONE * (1 if b > 0 else -1), abs(b)))


def color_parts(c):
    return [(c >> s) & 255 for s in (16, 8, 0, 24)]


def expand(v):
    return list(v[:5]) + color_parts(v[5])


def contract(v):
    return tuple(v[:5]) + (sum(v[k + 5] << s for k, s in enumerate((16, 8, 0, 24))),)


def clip_integer(poly, axis, plane, upper):
    result = []
    if not poly:
        return result
    a = poly[-1]
    for b in poly:
        ai = a[axis] <= plane if upper else a[axis] >= plane
        bi = b[axis] <= plane if upper else b[axis] >= plane
        if ai != bi:
            den = b[axis] - a[axis]
            num = [x * (b[axis] - plane) + y * (plane - a[axis]) for x, y in zip(a, b)]
            c = [rnd(x if den > 0 else -x, abs(den)) for x in num]
            c[axis] = plane
            result.append(c)
        if bi:
            result.append(b)
        a = b
    return result


def clip_fraction(poly, axis, plane, upper):
    # 第二实现：以带符号平面距离及 Fraction 权重做相交，不复用上面的裁剪函数。
    answer = []
    for index in range(len(poly)):
        start, end = poly[index - 1], poly[index]
        ds, de = start[axis] - plane, end[axis] - plane
        if upper:
            ds, de = -ds, -de
        if (ds >= 0) != (de >= 0):
            t = Fraction(ds, ds - de)
            values = [Fraction(x) * (1 - t) + y * t for x, y in zip(start, end)]
            answer.append([(v + Fraction(1, 2)).numerator // (v + Fraction(1, 2)).denominator for v in values])
        if de >= 0:
            answer.append(end)
    return answer


def setup(vertices, flags, alternate=False):
    clip = clip_fraction if alternate else clip_integer
    poly = [expand(v) for v in vertices]
    for axis, plane, upper in ((0, -4096, False), (0, 14335, True),
                                (1, -4096, False), (1, 11775, True)):
        poly = clip(poly, axis, plane, upper)
    result = []
    for i in range(1, len(poly) - 1):
        tri = (poly[0], poly[i], poly[i + 1])
        x, y = [v[0] for v in tri], [v[1] for v in tri]
        area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if area <= 0:
            continue
        edges = [(y[k] - y[(k + 1) % 3], x[(k + 1) % 3] - x[k],
                  y[(k + 1) % 3] * x[k] - y[k] * x[(k + 1) % 3]) for k in range(3)]
        bbox = (max(0, -(-min(x) // 16)), min(639, max(x) // 16),
                max(0, -(-min(y) // 16)), min(479, max(y) // 16))
        if bbox[0] > bbox[1] or bbox[2] > bbox[3]:
            continue
        gradients = []
        for field, scale in ((2, ONE), (3, 4096), (4, 4096), (5, ONE), (6, ONE), (7, ONE)):
            a = [v[field] * scale for v in tri]
            if alternate:
                gx = sum(Fraction(a[k] * edges[(k + 1) % 3][0] * 16, area) for k in range(3))
                gy = sum(Fraction(a[k] * edges[(k + 1) % 3][1] * 16, area) for k in range(3))
                dx, dy = (int((g + Fraction(1, 2)) // 1) for g in (gx, gy))
            else:
                dx = rnd(((a[1] - a[0]) * (y[2] - y[0]) - (a[2] - a[0]) * (y[1] - y[0])) * 16, area)
                dy = rnd(((x[1] - x[0]) * (a[2] - a[0]) - (x[2] - x[0]) * (a[1] - a[0])) * 16, area)
            gradients.append((dx, dy))
        if any(abs(g) >= 1 << 30 for pair in gradients for g in pair):
            continue
        r = bytearray(128)
        struct.pack_into('<I', r, 0, flags)
        for k, (a, b, c) in enumerate(edges):
            struct.pack_into('<iii', r, 4 + k * 12, a * 16, b * 16, c)
        struct.pack_into('<H', r, 40, tri[0][2])
        struct.pack_into('<HH', r, 52, *tri[0][3:5])
        struct.pack_into('<hhhh', r, 72, *bbox)
        for k, v in enumerate(tri):
            struct.pack_into('<I', r, 80 + 4 * k, contract(v)[5])
        for offset, pair in zip(GRADS, gradients):
            struct.pack_into('<ii', r, offset, *pair)
        struct.pack_into('<hh', r, 120, *tri[0][:2])
        result.append(bytes(r))
    return result


def view_setup(vertices, matrix, near, far, flags, alternate=False):
    clip = clip_fraction if alternate else clip_integer
    poly = [expand(v) for v in vertices]
    poly = clip(poly, 2, -near, True)
    poly = clip(poly, 2, -far, False)
    screen = []
    for v in poly:
        vec = v[:3] + [ONE]
        c = [sat(sum(sat(rnd(matrix[i * 4 + k] * vec[k])) for k in range(4))) for i in range(4)]
        assert c[3] > 0
        ndc = [div(a, c[3]) for a in c[:3]]
        x = sat(rnd((ONE + ndc[0]) * 640 * 8))
        y = sat(rnd((ONE - ndc[1]) * 480 * 8))
        z = max(0, min(65535, rnd((ndc[2] + ONE) * 65535, 2 * ONE)))
        screen.append((x, y, z, v[3], v[4], contract(v)[5]))
    return setup(screen, flags, alternate) if len(screen) >= 3 else []


def unpack(record):
    r = record
    return (struct.unpack_from('<I', r)[0],
            [struct.unpack_from('<iii', r, 4 + k * 12) for k in range(3)],
            struct.unpack_from('<hhhh', r, 72), struct.unpack_from('<hh', r, 120),
            [struct.unpack_from('<ii', r, o) for o in GRADS],
            [struct.unpack_from('<H', r, 40)[0] * ONE,
             struct.unpack_from('<H', r, 52)[0] * 4096, struct.unpack_from('<H', r, 54)[0] * 4096] +
            [c * ONE for c in color_parts(struct.unpack_from('<I', r, 80)[0])[:3]])


def pixel(decoded, x, y, old_color, old_z, texture, key):
    flags, edges, bbox, origin, gradients, starts = decoded
    for a, b, c in edges:
        e = a * x + b * y + c
        if e < 0 or e == 0 and not (a > 0 or a == 0 and b > 0):
            return old_color, old_z
    values = [rnd(start + rnd(gx * (16 * x - origin[0]) + gy * (16 * y - origin[1]), 16))
              for (gx, gy), start in zip(gradients, starts)]
    z = min(65535, max(0, values[0]))
    if z >= old_z:
        return old_color, old_z
    texel = 0xffffff
    if flags & 1:
        u, v = [min(1, max(0, a)) for a in values[1:3]]
        texel = texture[v * 2 + u]
        if flags & 4 and texel == key:
            return old_color, old_z
    color = 0
    for k, shift in enumerate((16, 8, 0)):
        c = max(0, min(255, values[k + 3])) if flags & 2 else 255
        if flags & 1:
            c = rnd(((texel >> shift) & 255) * c, 255)
        color |= c << shift
    return color, z


class XorShift:
    def __init__(self, seed):
        self.state = seed

    def next(self):
        s = self.state
        s ^= s << 13 & 0xffffffff
        s ^= s >> 17
        s ^= s << 5 & 0xffffffff
        self.state = s
        return s
