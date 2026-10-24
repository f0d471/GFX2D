#!/usr/bin/env python3
"""R1 acceptance：三方逐字段/逐像素、两层定种子 fuzz、位宽与注错。"""
import ctypes as ct
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

from check_run import ROOT, IncompleteCheck, command, session, write_json
import rast_model as model


class Record(ct.Structure):
    _fields_ = [('bytes', ct.c_uint8 * 128)]


class Vertex(ct.Structure):
    _fields_ = [('x', ct.c_int32), ('y', ct.c_int32), ('z', ct.c_uint16),
                ('u', ct.c_uint16), ('v', ct.c_uint16), ('color', ct.c_uint32)]


class ViewVertex(ct.Structure):
    _fields_ = [('x', ct.c_int32), ('y', ct.c_int32), ('z', ct.c_int32),
                ('u', ct.c_uint16), ('v', ct.c_uint16), ('color', ct.c_uint32)]


class Matrix(ct.Structure):
    _fields_ = [('v', ct.c_int32 * 16)]


class Viewport(ct.Structure):
    _fields_ = [('x', ct.c_int16), ('y', ct.c_int16),
                ('w', ct.c_uint16), ('h', ct.c_uint16)]


class Target(ct.Structure):
    _fields_ = [('color', ct.POINTER(ct.c_uint32)), ('depth', ct.POINTER(ct.c_uint16)),
                ('stride', ct.c_uint), ('strip', ct.c_uint), ('texture', ct.POINTER(ct.c_uint32)),
                ('tex_width', ct.c_uint), ('tex_height', ct.c_uint), ('tex_stride', ct.c_uint),
                ('key', ct.c_uint32)]


SOURCES = ['Main/sw/lib/core/fixmath.c', 'Main/sw/lib/core/sin_tab.c',
           'Main/sw/lib/gfx/rast_setup.c', 'Main/sw/lib/gfx/rast_ref.c',
           'Main/sw/lib/test/rast_ref2.c']
DIRECTED = ['Main/tools/check/rast_check.c', 'Main/sw/lib/test/test_rast.c']


def load(path):
    lib = ct.CDLL(str(path))
    lib.rast_setup_polygon.argtypes = [ct.POINTER(Vertex), ct.c_uint, ct.c_uint,
                                       ct.POINTER(Record), ct.c_uint, ct.c_void_p]
    lib.rast_setup_view.argtypes = [ct.POINTER(ViewVertex), ct.POINTER(Matrix), ct.c_int32,
                                    ct.c_int32, ct.c_uint, ct.POINTER(Record), ct.c_uint, ct.c_void_p]
    lib.rast_setup_polygon_vp.argtypes = [ct.POINTER(Vertex), ct.c_uint, ct.POINTER(Viewport),
                                          ct.c_uint, ct.POINTER(Record), ct.c_uint, ct.c_void_p]
    lib.rast_setup_view_vp.argtypes = [ct.POINTER(ViewVertex), ct.c_uint, ct.POINTER(Matrix), ct.c_int32,
                                       ct.c_int32, ct.POINTER(Viewport), ct.c_uint,
                                       ct.POINTER(Record), ct.c_uint, ct.c_void_p]
    for name in ('rast_ref_run', 'rast_ref2_run'):
        getattr(lib, name).argtypes = [ct.POINTER(Record), ct.c_uint, ct.POINTER(Target)]
    lib.rast_record_valid.argtypes = [ct.POINTER(Record)]
    lib.rast_record_valid.restype = ct.c_int
    for name in ('fx16_mul', 'fx16_div'):
        getattr(lib, name).argtypes = [ct.c_int32, ct.c_int32]
    lib.fx16_recip.argtypes = [ct.c_int32]
    lib.fx16_round_div.argtypes = [ct.c_int64, ct.c_int64]
    lib.fx16_round_div.restype = ct.c_int64
    lib.fx16_perspective.argtypes = [ct.POINTER(Matrix), ct.c_uint16, ct.c_int32, ct.c_int32, ct.c_int32]
    return lib


def c_setup(lib, vertices, flags, projection=None):
    out = (Record * 12)()
    if projection is None:
        array = (Vertex * len(vertices))(*(Vertex(*v) for v in vertices))
        n = lib.rast_setup_polygon(array, len(vertices), flags, out, 12, None)
    else:
        array = (ViewVertex * 3)(*(ViewVertex(*v) for v in vertices))
        n = lib.rast_setup_view(array, ct.byref(projection), 65536, 9 * 65536, flags, out, 12, None)
    assert n >= 0, ('C setup error', n, vertices)
    return [bytes(r.bytes) for r in out[:n]]


def c_setup_vp(lib, vertices, vp, flags, projection=None):
    out = (Record * 16)()
    vpa = Viewport(*vp)
    if projection is None:
        array = (Vertex * len(vertices))(*(Vertex(*v) for v in vertices))
        n = lib.rast_setup_polygon_vp(array, len(vertices), ct.byref(vpa), flags, out, 16, None)
    else:
        array = (ViewVertex * len(vertices))(*(ViewVertex(*v) for v in vertices))
        n = lib.rast_setup_view_vp(array, len(vertices), ct.byref(projection), 65536, 9 * 65536,
                                   ct.byref(vpa), flags, out, 16, None)
    assert n >= 0, ('C vp setup error', n, vertices, vp)
    return [bytes(r.bytes) for r in out[:n]]


def pixel_set(records, texture, key):
    """按记录与逐点模型收集覆盖点（z<65535），与 hand_checks 的用法一致。"""
    covered = set()
    for record in records:
        decoded = model.unpack(record)
        x0, x1, y0, y1 = decoded[2]
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                _, z = model.pixel(decoded, x, y, 0x12345678, 65535, texture, key)
                if z != 65535:
                    covered.add((x, y))
    return covered


def mathematical_checks(lib):
    rng = model.XorShift(0x16f00d01)
    # 每条式子覆盖完整 16 位余数域，含正负半值。分母 16/65536/255/65535。
    for i in range(65536):
        for d in (16, 65536, 255, 65535, 131072):
            for n in (i, -i):
                assert lib.fx16_round_div(n, d) == model.rnd(n, d), (n, d)
        # 窗口深度的乘法和偏置一起覆盖；裁剪/梯度的可变正分母覆盖全部 16 位值。
        numerator = (i - 32768 + 65536) * 65535
        assert lib.fx16_round_div(numerator, 131072) == model.rnd(numerator, 131072)
        d = i + 1
        for n in (3 * d + d // 2, -3 * d - d // 2):
            assert lib.fx16_round_div(n, d) == model.rnd(n, d)
    for i in range(10000):
        a, b = ct.c_int32(rng.next()).value, ct.c_int32(rng.next()).value
        assert lib.fx16_mul(a, b) == model.sat(model.rnd(a * b))
        assert lib.fx16_div(a, b) == model.div(a, b)
        expected_recip = model.div(65536, a) if a else 2147483647
        assert lib.fx16_recip(a) == expected_recip
    sine = (ct.c_int16 * 1280).in_dll(lib, 'fx_sin_tab')
    for i in range(1000):
        fov = 128 + rng.next() % (32768 - 128)
        aspect = 32768 + rng.next() % 131072
        near = 1 + rng.next() % 131072
        far = near + 1 + rng.next() % 1048576
        index = ((fov // 2 + 32) >> 6) & 1023
        cot = model.div(sine[index + 256], sine[index])
        expected = [0] * 16
        expected[0], expected[5] = model.div(cot, aspect), cot
        expected[10] = model.sat(model.rnd(-(far + near) * 65536, far - near))
        expected[11] = model.sat(model.rnd(-2 * far * near, far - near))
        expected[14] = -65536
        result = Matrix()
        assert lib.fx16_perspective(ct.byref(result), fov, aspect, near, far) == 0
        assert list(result.v) == expected
    print('ROUND PASS 16-bit domains x5 x2 + depth map + variable denominators; MUL/DIV/RECIP PASS 10000', flush=True)


def hand_checks(lib, logs):
    def vertex(x, y, z=100, u=0, v=0, color=0xff804020):
        return (x * 16, y * 16, z, u, v, color)

    cases = [
        ('top_left_right', '3: top/left own E=0, right does not',
         [vertex(0, 0), vertex(4, 0), vertex(0, 4)], {(x, y) for y in range(4) for x in range(4 - y)}),
        ('flat_bottom', '3: bottom does not own E=0',
         [vertex(0, 0), vertex(4, 4), vertex(0, 4)], {(x, y) for y in range(4) for x in range(y)}),
        ('shared_edge_second', '3: neighboring diagonal owns opposite tie',
         [vertex(4, 0), vertex(4, 4), vertex(0, 4)], {(x, y) for y in range(4) for x in range(4) if x + y >= 4}),
        ('cross_strip', '3: frame bbox intersects strips without changing planes',
         [vertex(0, 15), vertex(4, 15), vertex(0, 19)], {(x, y + 15) for y in range(4) for x in range(4 - y)}),
        ('degenerate', '3: area=0 emits no record', [vertex(0, 0), vertex(4, 4), vertex(8, 8)], set()),
        ('backface', '3: area<0 emits no record', [vertex(0, 0), vertex(0, 4), vertex(4, 0)], set()),
    ]
    fixtures, masks = [], {}
    texture = [0xff000000, 0xff80ff40, 0xff4080ff, 0xffffffff]
    for name, clause, vertices, coverage in cases:
        records = c_setup(lib, vertices, 2)
        assert records == model.setup(vertices, 2) == model.setup(vertices, 2, True)
        actual = set()
        expected_pixels = []
        for record in records:
            decoded = model.unpack(record)
            x0, x1, y0, y1 = decoded[2]
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    c, z = model.pixel(decoded, x, y, 0x12345678, 65535, texture, texture[0])
                    if z != 65535:
                        actual.add((x, y))
                    expected_pixels.append((x, y, c, z))
        assert actual == coverage, (name, actual, coverage)
        masks[name] = actual
        fixtures.append({'name': name, 'clause': clause, 'vertices': vertices, 'flags': 2,
                         'records': [r.hex() for r in records], 'expected_points': expected_pixels})
    assert not masks['top_left_right'] & masks['shared_edge_second']
    assert masks['top_left_right'] | masks['shared_edge_second'] == {(x, y) for x in range(4) for y in range(4)}
    for name, uv, flags, old_z, want in [
        ('key_hit', (0, 0), 7, 65535, (0x12345678, 65535)),
        ('key_miss', (16, 0), 5, 65535, (0x0080ff40, 100)),
        ('key_off', (0, 0), 3, 65535, (0, 100)),
        ('less_equal', (16, 0), 5, 100, (0x12345678, 100)),
        ('uv_half_below', (7, 0), 1, 65535, (0, 100)),
        ('uv_half_tie', (8, 0), 1, 65535, (0x0080ff40, 100)),
        ('uv_upper_clamp', (65535, 65535), 1, 65535, (0xffffff, 100)),
    ]:
        vertices = [vertex(x, y, u=uv[0], v=uv[1]) for x, y in ((0, 0), (4, 0), (0, 4))]
        r = c_setup(lib, vertices, flags)[0]
        assert model.pixel(model.unpack(r), 0, 0, 0x12345678, old_z, texture, texture[0]) == want
        fixtures.append({'name': name, 'clause': '4: LESS/UV nearest CLAMP/key before modulation',
                         'records': [r.hex()], 'initial': [0, 0, 0x12345678, old_z], 'expected': want})
    # 边界恰好位于 guard band 的最大/最小格点，极大外部坐标必须裁剪后才能发射。
    for name, positions in [('guard_exact', [(-4096, -4096), (14335, -4096), (-4096, 11775)]),
                             ('guard_fan', [(-60000, -60000), (60000, -60000), (0, 60000)])]:
        vertices = [(x, y, 100, 0, 0, 0xffffffff) for x, y in positions]
        records = c_setup(lib, vertices, 0)
        assert records == model.setup(vertices, 0) == model.setup(vertices, 0, True) and records
        fixtures.append({'name': name, 'clause': '2/3: half-open guard and s32 bound',
                         'vertices': vertices, 'records': [r.hex() for r in records]})
    write_json(logs / 'hand-vectors.json', {'abi_bytes': 128, 'texture': texture, 'key': texture[0],
                                          'vectors': fixtures})
    print('HAND PASS', len(fixtures), 'named clauses', flush=True)


def geometry_checks(lib, count, logdir):
    rng = model.XorShift(0x313d2026)
    crc = [0, 0, 0]
    projection = Matrix()
    assert lib.fx16_perspective(ct.byref(projection), 16384, 65536, 65536, 9 * 65536) == 0
    fixtures = []
    for i in range(count):
        flags = rng.next() & 7
        vertices = []
        view = i % 2
        for k in range(3):
            if view:
                xyz = ((rng.next() % 32 - 16) * 32768,
                       (rng.next() % 24 - 12) * 32768,
                       -(rng.next() % 24) * 32768)
            else:
                xyz = (int(rng.next() % 45000) - 15000,
                       int(rng.next() % 40000) - 12000, rng.next() & 65535)
            vertices.append((*xyz, rng.next() & 65535, rng.next() & 65535, rng.next()))
        # 保留反绕序、退化和裁剪空的样本；其余随机顶点自然产生两种绕序。
        if i % 19 == 0:
            vertices[2] = vertices[1]
        got = c_setup(lib, vertices, flags, projection if view else None)
        if view:
            expected = model.view_setup(vertices, list(projection.v), 65536, 9 * 65536, flags)
            third = model.view_setup(vertices, list(projection.v), 65536, 9 * 65536, flags, True)
        else:
            expected = model.setup(vertices, flags)
            third = model.setup(vertices, flags, True)
        assert got == expected == third, ('geometry mismatch', i, vertices, [r.hex() for r in got],
                                          [r.hex() for r in expected], [r.hex() for r in third])
        for k, records in enumerate((got, expected, third)):
            crc[k] = zlib.crc32(struct.pack('<I', len(records)) + b''.join(records), crc[k])
        if i < 20:
            fixtures.append({'case': i, 'layer': 'view' if view else 'screen', 'flags': flags,
                             'vertices': vertices, 'records': [r.hex() for r in got]})
    write_json(logdir / 'geometry-vectors.json', {'seed': '313d2026', 'projection': list(projection.v), 'vectors': fixtures})
    print('GEOMETRY PASS', count, 'CRCs', [f'{v:08x}' for v in crc], flush=True)
    return crc


def viewport_checks(lib, count, logdir):
    """子视口入口：定向四边/近远面组合 + 定种子模糊，三方对拍（C、整数模型、Fraction 路径）。"""
    rng = model.XorShift(0x32760423)
    projection = Matrix()
    assert lib.fx16_perspective(ct.byref(projection), 16384, 65536, 65536, 9 * 65536) == 0
    texture = [0xff000000, 0xff80ff40, 0xff4080ff, 0xffffffff]
    fixtures = []
    crc = [0, 0, 0]

    def case(name, vp, vertices, use_view, flags, clause):
        if use_view:
            got = c_setup_vp(lib, vertices, vp, flags, projection)
            expected = model.view_setup_vp(vertices, list(projection.v), 65536, 9 * 65536, vp, flags)
            third = model.view_setup_vp(vertices, list(projection.v), 65536, 9 * 65536, vp, flags, True)
        else:
            got = c_setup_vp(lib, vertices, vp, flags)
            expected = model.setup_vp(vertices, vp, flags)
            third = model.setup_vp(vertices, vp, flags, True)
        assert got == expected == third, (name, [r.hex() for r in got], expected, third)
        covered = pixel_set(got, texture, texture[0])
        x, y, w, h = vp
        assert all(x <= px < x + w and y <= py < y + h for px, py in covered), (name, sorted(covered))
        for record in got:
            r = Record.from_buffer_copy(record)
            assert lib.rast_record_valid(ct.byref(r)) == 1, (name, record.hex())
            decoded = model.unpack(record)
            bx0, bx1, by0, by1 = decoded[2]
            assert x <= bx0 and bx1 < x + w and y <= by0 and by1 < y + h, (name, decoded[2], vp)
        return got, covered

    vx0, vy0, vw0, vh0 = 2, 3, 4, 4
    px = lambda x, y, **kw: (x * 16, y * 16, kw.get('z', 100), kw.get('u', 0),
                             kw.get('v', 0), kw.get('c', 0xff804020))
    # 四边不动点：完全在视口内的三角形原样通过。
    case('vp_noop', (vx0, vy0, vw0, vh0),
         [px(vx0, vy0), px(vx0 + 3, vy0), px(vx0, vy0 + 3)], False, 2, '2.1: inside untouched')
    # 右边界的最大格点拥有、越界格点被裁。
    got, covered = case('vp_edge_right', (vx0, vy0, vw0, vh0),
                        [px(vx0 + vw0 - 2, vy0), px(vx0 + vw0 + 4, vy0), px(vx0 + vw0 - 2, vy0 + 4)],
                        False, 2, '2.1: half-open right edge')
    assert (vx0 + vw0 - 1, vy0) in covered and not any(p[0] >= vx0 + vw0 for p in covered)
    # 整棵在视口外：两个入口都发 0 条。
    for name, verts in [('vp_outside_left', [px(0, 3), px(1, 3), px(0, 7)]),
                        ('vp_outside_below', [px(2, 7), px(6, 7), px(2, 11)])]:
        got, covered = case(name, (vx0, vy0, vw0, vh0), verts, False, 2, '2.1: outside emits none')
        assert not got and not covered, name
    # 跨左/上两边的扇形拆分：多记录且 bbox 全在视口内。
    got, covered = case('vp_straddle', (vx0, vy0, vw0, vh0),
                        [px(-2, 0), px(vx0 + vw0 + 3, 2), px(0, 8)], False, 2,
                        '2.1: two-edge fan split')
    assert len(got) >= 2, 'vp_straddle'
    # 视空间入口：近面交点与视口左右边同时裁。
    view_tri = [(-8 * 32768, 32768, -131072, 0, 0, 0xffffffff),
                (8 * 32768, -32768, -131072, 64, 0, 0xff4080ff),
                (0, -8 * 32768, -49152, 0, 64, 0xff80ff40)]
    got, covered = case('vp_near_view', (160, 120, 320, 240), view_tri, True, 3,
                        '2.1: near plane and viewport together')
    assert got, 'vp_near_view'
    # Eight convex view vertices can become ten after both depth planes.
    # Keep this regression outside the historical random-stream CRC.
    octagon=[(0,65536),(46340,46340),(65536,0),(46340,-46340),
             (0,-65536),(-46340,-46340),(-65536,0),(-46340,46340)]
    verts=[(x,y,-5*65536+5*x,0,0,0xffffffff) for x,y in octagon]
    projected=model.view_screen(verts,list(projection.v),65536,9*65536,
                                160,120,320,240)
    assert len(projected)==10
    got,_=case('vp_octagon_depth_growth',(160,120,320,240),verts,True,2,
               '2.1: eight view vertices grow to ten before viewport')
    assert got
    fixtures = []
    for i in range(count):
        w = 1 + rng.next() % 320
        h = 1 + rng.next() % 240
        x = rng.next() % (640 - w + 1)
        y = rng.next() % (480 - h + 1)
        vp = (0, 0, 640, 480) if i % 17 == 0 else (x, y, w, h)
        flags = rng.next() & 7
        view = i % 2
        nv = 4 if i % 3 == 0 else 3      # 四边形：R4 走廊的实际输入形态
        vertices = []
        for k in range(nv):
            if view:
                xyz = ((rng.next() % 32 - 16) * 32768,
                       (rng.next() % 24 - 12) * 32768,
                       -(rng.next() % 24) * 32768)
            else:
                xyz = (int(rng.next() % 45000) - 15000,
                       int(rng.next() % 40000) - 12000, rng.next() & 65535)
            vertices.append((*xyz, rng.next() & 65535, rng.next() & 65535, rng.next()))
        if i % 19 == 0:
            vertices[-1] = vertices[-2]
        got, _ = case(f'fuzz{i}', vp, vertices, view, flags, '2.1 fuzz')
        crc[0] = zlib.crc32(struct.pack('<HHHH', *vp)
                            + b''.join(got), crc[0])
        if i < 20:
            fixtures.append({'case': i, 'layer': 'view' if view else 'screen', 'vp': list(vp),
                             'flags': flags, 'vertices': vertices,
                             'records': [r.hex() for r in got]})
    write_json(logdir / 'viewport-vectors.json', {'seed': '32760423',
                                                  'projection': list(projection.v), 'vectors': fixtures})
    print('VIEWPORT PASS', count, 'CRC', f'{crc[0]:08x}', flush=True)
    return crc[0]


def raster_checks(lib, count, logdir):
    rng = model.XorShift(0x72312026)
    colors = [(ct.c_uint32 * (640 * 16))() for _ in range(2)]
    depths = [(ct.c_uint16 * (640 * 16))() for _ in range(2)]
    texture_values = [0xff000000, 0xff80ff40, 0xff4080ff, 0xffffffff]
    texture = (ct.c_uint32 * 4)(*texture_values)
    targets = [Target(colors[k], depths[k], 640, 0, texture, 2, 2, 2, texture_values[0]) for k in range(2)]
    crc = [0, 0, 0]
    fixtures = []
    for i in range(count):
        x, y = rng.next() % 634, rng.next() % 474
        w, h = 1 + rng.next() % 5, 1 + rng.next() % 5
        sub = rng.next() & 15
        vs = [(x * 16 - sub, y * 16, rng.next() & 65535, rng.next() & 63, rng.next() & 63, rng.next()),
              ((x + w) * 16, y * 16 + sub, rng.next() & 65535, rng.next() & 63, rng.next() & 63, rng.next()),
              (x * 16, (y + h) * 16, rng.next() & 65535, rng.next() & 63, rng.next() & 63, rng.next())]
        records = model.setup(vs, rng.next() & 7)
        if not records:  # 极细条梯度超限时使用确定的合法大面积记录，保持记录数精确。
            vs = [(x * 16, y * 16, 100, 0, 0, 0xff804020),
                  ((x + 4) * 16, y * 16, 120, 16, 0, 0xffff8040),
                  (x * 16, (y + 4) * 16, 110, 0, 16, 0xff4080ff)]
            records = model.setup(vs, i & 7)
        r = records[0]
        # 独立随机记录流：属性梯度不局限于 setup 产生的平面，覆盖负 UV 和大累加值。
        if i % 4 == 0:
            raw = bytearray(r)
            for off in model.GRADS:
                struct.pack_into('<ii', raw, off, int(rng.next() % (1 << 29)) - (1 << 28),
                                 int(rng.next() % (1 << 29)) - (1 << 28))
            r = bytes(raw)
        decoded = model.unpack(r)
        x0, x1, y0, y1 = decoded[2]
        record = Record.from_buffer_copy(r)
        points = []
        for py in range(y0, y1 + 1):
            for px in range(x0, x1 + 1):
                old_z = (65535, 0, decoded[5][0] // 65536, rng.next() & 65535)[i % 4]
                old_color = rng.next()
                at = (py % 16) * 640 + px
                for k in range(2):
                    colors[k][at] = old_color
                    depths[k][at] = old_z
                points.append((px, py, old_color, old_z))
        for strip in range(y0 // 16, y1 // 16 + 1):
            for k, name in enumerate(('rast_ref_run', 'rast_ref2_run')):
                targets[k].strip = strip
                assert getattr(lib, name)(ct.byref(record), 1, ct.byref(targets[k])) == 0
        results = [bytearray(), bytearray(), bytearray()]
        for px, py, old_color, old_z in points:
            at = (py % 16) * 640 + px
            expected = model.pixel(decoded, px, py, old_color, old_z, texture_values, texture_values[0])
            got = [(colors[k][at], depths[k][at]) for k in range(2)]
            assert got[0] == got[1] == expected, ('raster mismatch', i, px, py, got, expected, r.hex())
            for k, value in enumerate(got + [expected]):
                results[k] += struct.pack('<IH', *value)
        for k in range(3):
            crc[k] = zlib.crc32(r + results[k], crc[k])
        if i < 16:
            fixtures.append({'case': i, 'record': r.hex(), 'initial': points,
                             'expected': results[2].hex()})
        if i and i % 25000 == 0:
            print('RASTER PROGRESS', i, flush=True)
    write_json(logdir / 'raster-vectors.json', {'seed': '72312026', 'texture': texture_values,
                                              'key': texture_values[0], 'vectors': fixtures})
    print('RASTER PASS', count, 'CRCs', [f'{v:08x}' for v in crc], flush=True)
    return crc


def main():
    current = session()
    build, logs = current.build('rast'), current.logs('rast')
    cc = os.environ.get('HOST_CC', 'gcc')
    flags = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Werror=double-promotion', '-Werror=float-conversion', '-IMain/sw/lib']
    command([sys.executable, 'Main/tools/check/gen_recip_tab.py'], logs / 'recip-table.log')
    library = build / 'rast.so'
    command([cc, *flags, '-fPIC', '-shared', *SOURCES, '-o', library], logs / 'shared-build.log')
    lib = load(library)
    mathematical_checks(lib)
    hand_checks(lib, logs)
    raster_n, geometry_n = (100000, 10000) if current.profile != 'quick' else (1000, 100)
    viewport_n = 5000 if current.profile != 'quick' else 400
    geometry = geometry_checks(lib, geometry_n, logs)
    viewport = viewport_checks(lib, viewport_n, logs)
    raster = raster_checks(lib, raster_n, logs)
    legacy = f'geometry 313d2026 {geometry_n} {geometry[0]:08x}\nraster 72312026 {raster_n} {raster[0]:08x}\n'
    actual = legacy + f'viewport 32760423 {viewport_n} {viewport:08x}\n'
    (logs / 'golden-observed.txt').write_text(actual, encoding='utf-8')
    golden = ROOT / 'Main/sw/lib/test/rast_golden.txt'
    golden_vp = ROOT / 'Main/sw/lib/test/rast_golden_vp.txt'
    if current.profile != 'quick':
        assert golden.read_text(encoding='utf-8') == legacy, ('golden differs', legacy)
        if not golden_vp.exists():
            raise IncompleteCheck('rast_golden_vp.txt 缺失：先在 acceptance 档核对一次'
                                  'viewport CRC，再把 observed 值入库')
        assert golden_vp.read_text(encoding='utf-8') == \
            f'viewport 32760423 {viewport_n} {viewport:08x}\n', ('viewport golden differs', viewport)
    # 主机正常单元测试由聚合入口 lib 去重执行；这里仅 sanitizer 和本专项独立变异。
    executable = build / 'sanitize'
    command([cc, *flags, '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             *SOURCES, *DIRECTED, '-o', executable], logs / 'sanitize-build.log')
    command([executable], logs / 'sanitize.log')
    for mutation, witness in [('RAST_ALL_EDGES', 'top/left included and right excluded'),
                               ('RAST_NO_16', 'top/left included and right excluded'),
                               ('RAST_NO_GUARD', 'guard prevents s32 overflow')]:
        executable = build / mutation
        command([cc, *flags, '-DINJECT_' + mutation, *SOURCES, *DIRECTED, '-o', executable],
                logs / (mutation + '-build.log'))
        p = subprocess.run([executable], cwd=ROOT, capture_output=True, text=True)
        (logs / (mutation + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
        assert p.returncode == 1 and 'RAST FAIL ' + witness in p.stdout, (mutation, p.returncode, p.stdout)
        print('EXPECTED_RED', mutation, flush=True)
    prefix = os.environ.get('RISCV_BIN')
    if not prefix:
        raise IncompleteCheck('RISCV_BIN 未配置，target.mk 必须构建')
    app = ROOT / 'Main/sw/apps/test/rastTest'
    for platform in ('host', 'target'):
        command(['make', '-C', app, 'PLATFORM=' + platform, 'RISCV_BIN=' + prefix,
                 'OBJDIR=' + str(build / platform)], logs / (platform + '-build.log'))
    command([build / 'host/rastTest'], logs / 'host-run.log', env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'})
    objdump = prefix + 'objdump'
    assembly = command([objdump, '-d', build / 'target/rastTest-O2.elf'], logs / 'target-disassembly.log')
    assert '__addsf3' not in assembly and '__mulsf3' not in assembly and '__divdf3' not in assembly
    write_json(logs / 'summary.json', {'status': 'PASS', 'profile': current.profile,
               'geometry': {'count': geometry_n, 'seed': '313d2026', 'crc32': [f'{c:08x}' for c in geometry]},
               'viewport': {'count': viewport_n, 'seed': '32760423', 'crc32': f'{viewport:08x}'},
               'raster': {'count': raster_n, 'seed': '72312026', 'crc32': [f'{c:08x}' for c in raster]},
               'board_run': 'NOT_APPLICABLE: R3', 'abi_bytes': 128})
    print('RAST PASS scope=' + current.profile, flush=True)


if __name__ == '__main__':
    try:
        main()
    except IncompleteCheck as error:
        print('INCOMPLETE ' + str(error), file=sys.stderr)
        sys.exit(3)
