#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_r4_models_obj.py —— Kenney Space Kit (CC0) OBJ → R4 低模头文件

来源 https://opengameart.org/content/space-kit （Kenney.nl,CC0,署名可选）:
解包目录 --src 下取 spaceCraft5.obj（自机）与 spaceCraft1.obj（敌机）。

流水线:居中 → 定标 → 顶点聚类减面(网格从粗到细试到面数达标) → 面去重 →
面法线(Q1.14) → 材质分桶(按法线方向+亮度,4 桶) → 生成 r4_models.h。
同一输入同一像素;--check 与入库文件逐字比。

产物:sw/apps/game/gfx2dGame/src/r4_models.h
    R4_SHIP_*   自机(原接口不变,r4_scene 的横滚/平移变换继续适用)
    R4_ENEMY_*  敌机(r4_scene 近景替换原六顶点八面体)
    r4_cup_normals 茶杯法线(沿用 cup_model.h)

用法:python3 Main/tools/assets/gen_r4_models_obj.py --src <解包目录>
"""
import argparse
import math
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DEST = ROOT / 'Main/sw/apps/game/gfx2dGame/src/r4_models.h'
CUP_H = ROOT / 'Main/sw/apps/game/cup3d/src/cup_model.h'

R4_ONE = 65536
SHIP_SRC = 'spaceCraft5.obj'
ENEMY_SRC = 'spaceCraft1.obj'
SHIP_FACES_MAX = 160
ENEMY_FACES_MAX = 100

MATERIALS = [0x004f7fa8, 0x00162a44, 0x00bfe8ff, 0x00ff4fd8]
MAT_HULL, MAT_DARK, MAT_GLASS, MAT_ACCENT = 0, 1, 2, 3


def load_obj(path):
    verts, faces, mat_of_face = [], [], []
    cur = 0
    mtl = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == 'v':
                verts.append([float(t[1]), float(t[2]), float(t[3])])
            elif t[0] == 'f':
                idx = [int(p.split('/')[0]) - 1 for p in t[1:]]
                for k in range(1, len(idx) - 1):
                    faces.append((idx[0], idx[k], idx[k + 1]))
                    mat_of_face.append(cur)
            elif t[0] == 'usemtl':
                cur = len(mtl)
                mtl.setdefault(t[1], cur)
    return verts, faces, mat_of_face


def bbox_center_scale(verts, target):
    xs, ys, zs = zip(*verts)
    cx, cy, cz = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2
    size = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
    s = target / size
    return [[(v[0] - cx) * s, (v[1] - cy) * s, (v[2] - cz) * s] for v in verts]


def auto_orient(verts, faces):
    """最长轴转成前进轴,R4 机头朝 -z;Kenney 模型多为 Y-up。
    鼻锥=横截面较小的一端;翻 z 时交换面索引保持绕向。"""
    xs, ys, zs = zip(*verts)
    ext = [max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)]
    axis = ext.index(max(ext))
    out = []
    for v in verts:
        if axis == 0:
            w = [v[1], v[2], v[0]]       # x 长循环保向
        elif axis == 1:
            w = [v[0], v[2], -v[1]]      # y 长(Kenney 常见)映射保向
        else:
            w = [v[0], v[1], v[2]]
        out.append(w)
    zs2 = [w[2] for w in out]
    mid = (min(zs2) + max(zs2)) / 2

    def spread(front):
        pts = [w for w in out if (w[2] < mid) == front]
        if not pts:
            return 0.0
        px = [p[0] for p in pts]
        py = [p[1] for p in pts]
        return (max(px) - min(px)) * (max(py) - min(py))

    if spread(False) < spread(True):     # +z 端更瘦=鼻锥 → 翻到 -z
        out = [[w[0], w[1], -w[2]] for w in out]
        faces = [(a, c, b) for a, b, c in faces]
    return out, faces


def decimate(verts, faces, mat_of_face, max_faces):
    """顶点聚类:网格从细到粗,首个面数达标的网格即定。"""
    xs, ys, zs = zip(*verts)
    span = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
    for cells in range(48, 3, -1):
        cell = span / cells
        def key(v):
            return (round(v[0] / cell), round(v[1] / cell), round(v[2] / cell))
        table, remap = {}, []
        for v in verts:
            k = key(v)
            if k not in table:
                table[k] = len(table)
            remap.append(table[k])
        seen, nf, nm = set(), [], []
        for f, m in zip(faces, mat_of_face):
            a, b, c = remap[f[0]], remap[f[1]], remap[f[2]]
            if a == b or b == c or a == c:
                continue
            k = tuple(sorted((a, b, c)))
            if k in seen:
                continue
            seen.add(k)
            nf.append((a, b, c))
            nm.append(m)
        if len(nf) <= max_faces:
            verts2 = [[0.0, 0.0, 0.0] for _ in range(len(table))]
            for v, k in zip(verts, map(key, verts)):
                for d in range(3):
                    verts2[table[k]][d] = v[d]
            return verts2, nf, nm
    return verts, faces, mat_of_face


def face_normals(verts, faces):
    out = []
    for a, b, c in faces:
        va, vb, vc = verts[a], verts[b], verts[c]
        u = [vb[i] - va[i] for i in range(3)]
        v = [vc[i] - va[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        ln = math.sqrt(sum(x * x for x in n)) or 1e-9
        out.append([x / ln for x in n])
    return out


def assign_materials(normals):
    """按朝向+倾角分四桶:上表面亮壳、下表面暗壳、前向玻璃青、侧带品红点缀。"""
    out = []
    for n in normals:
        nx, ny, nz = n
        if nz < -0.55:
            out.append(MAT_GLASS)
        elif ny > 0.45:
            out.append(MAT_HULL)
        elif ny < -0.45:
            out.append(MAT_DARK)
        elif abs(nx) > 0.6 and (nx * 37 + nz * 17) % 5 == 0:
            out.append(MAT_ACCENT)
        else:
            out.append(MAT_HULL if ny >= 0 else MAT_DARK)
    return out


def smooth_normals(verts, faces):
    total = [[0.0, 0.0, 0.0] for _ in verts]
    for f, n in zip(faces, face_normals(verts, faces)):
        for i in f:
            for k in range(3):
                total[i][k] += n[k]
    out = []
    for n in total:
        ln = math.sqrt(sum(x * x for x in n)) or 1.0
        out.append(tuple(max(-16384, min(16384, int(x / ln * 16384))) for x in n))
    return out


def to_q16(verts):
    return [[max(-32768, min(32767, int(round(c * R4_ONE)))) for c in v] for v in verts]


def array(name, data, ctype, width):
    return ('static const ' + ctype + ' ' + name + '[' + str(len(data)) + '][' +
            str(width) + '] = {\n' +
            ''.join('    {' + ','.join(map(str, row)) + '},\n' for row in data) + '};\n')


def model_block(prefix, verts, faces, mats):
    nv, nf = len(verts), len(faces)
    text = '#define R4_' + prefix + '_VERTICES ' + str(nv) + 'u\n'
    text += '#define R4_' + prefix + '_FACES ' + str(nf) + 'u\n'
    text += array('r4_' + prefix.lower() + '_vertices', to_q16(verts), 'int16_t', 3)
    idx_t = 'uint8_t' if nv <= 255 else 'uint16_t'
    text += array('r4_' + prefix.lower() + '_faces', faces, idx_t, 3)
    text += 'static const uint8_t r4_' + prefix.lower() + '_material[] = {' + ','.join(map(str, mats)) + '};\n'
    text += array('r4_' + prefix.lower() + '_normals', smooth_normals(verts, faces), 'int16_t', 3)
    return text


def build(src):
    text = ('/* Generated by tools/assets/gen_r4_models_obj.py from Kenney Space Kit (CC0);\n'
            '   do not hand edit. 自机=spaceCraft5, 敌机=spaceCraft1, 均经聚类减面。 */\n'
            '#ifndef GFX2D_R4_MODELS_H\n#define GFX2D_R4_MODELS_H\n#include <stdint.h>\n')
    for prefix, obj, target, fmax in (('SHIP', SHIP_SRC, 0.52, SHIP_FACES_MAX),
                                      ('ENEMY', ENEMY_SRC, 0.95, ENEMY_FACES_MAX)):
        verts, faces, mof = load_obj(os.path.join(src, 'Models', obj))
        verts = bbox_center_scale(verts, target)
        verts, faces = auto_orient(verts, faces)
        verts, faces, mof = decimate(verts, faces, mof, fmax)
        mats = assign_materials(face_normals(verts, faces))
        sys.stderr.write('%s: %d verts %d faces\n' % (prefix, len(verts), len(faces)))
        text += model_block(prefix, verts, faces, mats)
    cup = CUP_H.read_text()
    import re
    cverts = [int(x) for x in re.findall(r'-?\d+', cup.split('cupVertices')[1].split('{', 1)[1].split('}', 1)[0])]
    cfaces = [int(x) for x in re.findall(r'\d+', cup.split('cupTriangles')[1].split('{', 1)[1].split('}', 1)[0])]
    cverts = list(zip(*[iter(cverts)] * 3))
    cfaces = list(zip(*[iter(cfaces)] * 3))
    text += array('r4_cup_normals', smooth_normals(cverts, cfaces), 'int16_t', 3)
    text += '#endif\n'
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--src', required=True)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    text = build(args.src)
    if args.check:
        if not DEST.exists() or DEST.read_text() != text:
            raise SystemExit('R4 MODELS OBJ CHECK FAIL')
        print('R4 MODELS OBJ CHECK PASS')
    else:
        DEST.write_text(text, encoding='utf-8')
        print(DEST)


if __name__ == '__main__':
    main()
