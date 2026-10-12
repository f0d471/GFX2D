#!/usr/bin/env python3
# 研究轮：在冻结的 17 组固定输入上量化 PB v1 的容量、引用分布与取数账。
# 输入链：inputs/mode0（v1 直通记录流）按共享状态键合包成 v1-I 包流，
# 再按两遍构建（COUNT/SNAPSHOT、PREFIX、SCATTER）算每桶引用。
# 求交语义对齐 strip.c 即时 v2（目的矩形与 clip、屏幕、目的表面高、源有效区求交），
# 用 mode1 的 v2 条目数交叉验证：M_model + NOP 数 == M_v2（NOP 在 v2 进桶 0 占位，
# PB 只对绘制包产生引用）。
# 输出 f5_pb_stats.json 与控制台摘要；不改任何生产源码。
import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
INPUTS = HERE / "inputs"

OP_NOP, OP_FILL, OP_BLIT, OP_SET_CLIP, OP_END = 0, 1, 2, 3, 0xFF
SCREEN_W, SCREEN_H = 640, 480
STRIP_H = 16
LSTRIPS = 30
HEADER_BYTES = 512


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def load_v1(path):
    """返回 (表面表原始字列表, 记录列表)。布局：192 B 表面表 + u32 记录数 + 32 B 记录。"""
    data = path.read_bytes()
    n = struct.unpack_from("<I", data, 192)[0]
    words = struct.unpack_from("<%dI" % (n * 8), data, 196)
    recs = [list(words[i * 8:(i + 1) * 8]) for i in range(n)]
    surf = [list(struct.unpack_from("<3I", data, i * 12)) for i in range(16)]
    return surf, recs


def load_v2(path):
    """返回 (每桶计数[32], 总条目数)。布局：192 B 表面表 + u32 used +
    32 个 u32 计数 + 每桶计数条 32 B 条目（consume.c 的导出格式）。"""
    data = path.read_bytes()
    counts = list(struct.unpack_from("<32I", data, 196))
    return counts, sum(counts[:30])


def intersect(r, clip, surf_h, src_h=None):
    """按 strip.c 即时 v2 的语义求 y 覆盖，返回 (y0, y1) 或 None。
    r=(dx,dy,w,ht)；clip=[y0,y1,x0,x1] 闭区间；src_h 为源表面高（BLIT 源反推裁剪）。
    x 方向由屏幕边界覆盖（目的表面宽恒 640），y 需单独对目的表面高求交。"""
    dx, dy, w, ht = r
    y0 = max(0, dy, clip[0])
    y1 = min(dy + ht - 1, SCREEN_H - 1, surf_h - 1, clip[1])
    if src_h is not None:
        pass
    if y0 > y1:
        return None
    return y0, y1


def intersect_x(r, clip, src_w=None):
    dx, _, w, _ = r
    x0 = max(0, dx, clip[2])
    x1 = min(dx + w - 1, SCREEN_W - 1, clip[3])
    if src_w is not None:
        pass
    return (x0, x1) if x0 <= x1 else None


def src_window_y(dy, sy, src_h):
    """源有效区在目的 y 轴上的投影 [dy-sy, dy-sy+src_h-1]。"""
    return dy - sy, dy - sy + src_h - 1


def src_window_x(dx, sx, src_w):
    return dx - sx, dx - sx + src_w - 1


def pack_runs(recs):
    """F4 保守口径合包：连续 BLIT 且共享状态键相同为一个区间
    （键 = w0 全部 + key 值 w5 + 宽高 w3 + clip epoch），
    区间内源坐标全同计为 XY4 包，否则整段一个 XYUV8 包；
    非 BLIT 命令逐条成包。返回保序包列表 [(kind, 记录下标列表)]，kind: cmd/inst4/inst8。"""
    runs = []
    epoch = 0
    cur_key = None
    cur_idx = []
    for i, w in enumerate(recs):
        op = w[0] & 0xFF
        if op == OP_SET_CLIP:
            epoch += 1
        if op != OP_BLIT:
            if cur_idx:
                runs.append((cur_key, cur_idx))
                cur_key, cur_idx = None, []
            continue
        key = (w[0], w[5], w[3], epoch)
        if cur_key is None or key != cur_key:
            if cur_idx:
                runs.append((cur_key, cur_idx))
            cur_key, cur_idx = key, [i]
        else:
            cur_idx.append(i)
    if cur_idx:
        runs.append((cur_key, cur_idx))

    in_run = {}
    for _, idx in runs:
        for j in idx:
            in_run[j] = True
    packets = []
    for _, idx in runs:
        sxs = {(s16(recs[j][1] & 0xFFFF), s16(recs[j][1] >> 16)) for j in idx}
        packets.append(("inst4" if len(sxs) == 1 else "inst8", idx))
    # 混入非 BLIT 包，按输入序归位
    merged = []
    run_pos = 0
    for i, w in enumerate(recs):
        if i in in_run:
            if merged and merged[-1][0].startswith("inst") and i in merged[-1][1]:
                continue
            continue
        op = w[0] & 0xFF
        if op == OP_END:
            break
        merged.append(("cmd", [i]))
    result = []
    ri = 0
    used = set()
    for i, w in enumerate(recs):
        if i in in_run:
            if ri < len(runs) and i == runs[ri][1][0]:
                result.append(packets[ri])
                ri += 1
            continue
        if (w[0] & 0xFF) == OP_END:
            break
        result.append(("cmd", [i]))
    return result


def build_pb(recs, surf, strip_h=STRIP_H, lstrips=LSTRIPS):
    """两遍构建的计数遍（COUNT/SNAPSHOT）模型，同时保留每桶引用的包序
    （SCATTER 按相同输入序重放得到同一序列）。返回统计 dict。"""
    packets = pack_runs(recs)
    clip = [0, SCREEN_H - 1, 0, SCREEN_W - 1]
    counts = [0] * 32
    bucket_seq = [[] for _ in range(lstrips)]
    m_refs = 0
    empty_insts = 0
    u_slots = 0
    n_setclip = 0
    n_nop = 0
    for pi, (kind, idx) in enumerate(packets):
        u_slots += 1
        head = recs[idx[0]]
        op = head[0] & 0xFF
        if op == OP_SET_CLIP:
            n_setclip += 1
            x = s16(head[1] & 0xFFFF)
            y = s16(head[1] >> 16)
            clip = [max(0, y), min(SCREEN_H - 1, y + (head[3] >> 16) - 1),
                    max(0, x), min(SCREEN_W - 1, x + (head[3] & 0xFFFF) - 1)]
            if clip[0] > clip[1] or clip[2] > clip[3]:
                clip = [1, 0, 1, 0]  # 空框规范化 (0,0,-1,-1)
            continue
        if op == OP_NOP:
            n_nop += 1
            continue
        w = head[3] & 0xFFFF
        ht = head[3] >> 16
        if not w or not ht:
            continue  # 零尺寸实例包：包检查后计零引用
        dst_id = (head[0] >> 20) & 0xF
        src_id = (head[0] >> 16) & 0xF
        surf_h = surf[dst_id][1] >> 16
        for j in (idx if kind.startswith("inst") else [-1]):
            rec = recs[j] if kind.startswith("inst") else head
            dx, dy = s16(rec[2] & 0xFFFF), s16(rec[2] >> 16)
            r = (dx, dy, w, ht)
            if op == OP_BLIT:
                sx, sy = s16(rec[1] & 0xFFFF), s16(rec[1] >> 16)
                src_w = surf[src_id][1] & 0xFFFF
                src_h = surf[src_id][1] >> 16
                symin, symax = src_window_y(dy, sy, src_h)
                sxmin, sxmax = src_window_x(dx, sx, src_w)
            else:
                symin, symax, sxmin, sxmax = -1 << 30, 1 << 30, -1 << 30, 1 << 30
            y0 = max(0, dy, clip[0], symin)
            y1 = min(dy + ht - 1, SCREEN_H - 1, surf_h - 1, clip[1], symax)
            x0 = max(0, dx, clip[2], sxmin)
            x1 = min(dx + w - 1, SCREEN_W - 1, clip[3], sxmax)
            if y0 > y1 or x0 > x1:
                empty_insts += 1
                continue
            for s in range(y0 // strip_h, y1 // strip_h + 1):
                counts[s] += 1
                if not bucket_seq[s] or bucket_seq[s][-1][0] != pi:
                    bucket_seq[s].append([pi, 0])
                bucket_seq[s][-1][1] += 1
            m_refs += 1
    return {
        "packets": packets,
        "u_slots": u_slots,
        "m_refs": m_refs,
        "m_total": sum(counts[:lstrips]),
        "counts": counts[:lstrips],
        "bucket_seq": bucket_seq,
        "n_setclip": n_setclip,
        "n_nop": n_nop,
        "empty_insts": empty_insts,
    }


def align16(n):
    return (n + 15) // 16 * 16


def scene_stats(entry, strip_h=STRIP_H, lstrips=None):
    if lstrips is None:
        lstrips = SCREEN_H // strip_h
    p0 = INPUTS / ("scene%02d_mode0.bin" % entry["scene"])
    p1 = INPUTS / ("scene%02d_mode1.bin" % entry["scene"])
    surf, recs = load_v1(p0)
    v2_counts, m_v2 = load_v2(p1)
    ops = [w[0] & 0xFF for w in recs]
    n_blit = ops.count(OP_BLIT)
    n_nop = ops.count(OP_NOP)
    n_setclip = ops.count(OP_SET_CLIP)
    assert len(recs) == entry["records"], p0

    pb = build_pb(recs, surf, strip_h=strip_h, lstrips=lstrips)
    packets = pb["packets"]
    u = len(packets)
    inst_total = sum(len(idx) for k, idx in packets if k.startswith("inst"))
    inst_bytes = sum(len(idx) * (4 if k == "inst4" else 8)
                     for k, idx in packets if k.startswith("inst"))
    counts = pb["counts"]
    m = pb["m_refs"]
    peak = max(counts)
    empty_buckets = sum(1 for c in counts if c == 0)
    ref_bytes = sum(align16(c * 8) for c in counts)
    pb_bytes = HEADER_BYTES + 64 * u + ref_bytes
    pb_wr_beats = pb_bytes // 16
    v2_bytes = 128 + 32 * m_v2
    v2_wr_beats = v2_bytes // 16

    # 构建读账（COUNT 与 SCATTER 各一遍）：每包 32 B 包流（2 拍）+ 数组字节
    flow_beats = 2 * u
    build_rd_beats = 2 * (flow_beats + align16(inst_bytes) // 16)
    # 执行读账：每桶 = 引用拍 + 不同包 metadata 段（64 B）+ 实例区间（对齐）
    ex_ref_beats = ref_bytes // 16
    ex_meta_segs = 0
    ex_inst_bytes = 0
    for s in range(lstrips):
        ex_meta_segs += len(pb["bucket_seq"][s])
        for pi, n in pb["bucket_seq"][s]:
            kind, idx = packets[pi]
            if kind.startswith("inst"):
                ex_inst_bytes += align16(n * (4 if kind == "inst4" else 8))
    ex_meta_bytes = 64 * ex_meta_segs
    ex_rd_beats = ex_ref_beats + ex_meta_bytes // 16 + ex_inst_bytes // 16

    return {
        "scene": entry["scene"],
        "records": entry["records"],
        "blits": n_blit,
        "nops": n_nop,
        "setclips": n_setclip,
        "u_packets": u,
        "inst_total": inst_total,
        "inst_bytes": inst_bytes,
        "m_refs": m,
        "m_total": pb["m_total"],
        "m_v2": m_v2,
        "m_check": (strip_h == STRIP_H and pb["m_total"] + n_nop == m_v2),
        "strip_h": strip_h,
        "lstrips": lstrips,
        "m_v2_peak": max(v2_counts[:30]),
        "v2_empty": sum(1 for c in v2_counts[:30] if c == 0),
        "pb_peak": peak,
        "pb_empty": empty_buckets,
        "pb_bytes": pb_bytes,
        "v2_bytes": v2_bytes,
        "pb_wr_beats": pb_wr_beats,
        "v2_wr_beats": v2_wr_beats,
        "build_rd_beats": build_rd_beats,
        "ex_rd_beats": ex_rd_beats,
        "ex_meta_segs": ex_meta_segs,
        "ex_inst_bytes": ex_inst_bytes,
        "empty_insts": pb["empty_insts"],
    }


def main():
    manifest = json.loads((INPUTS / "manifest.json").read_text(encoding="utf-8"))
    out = []
    for entry in manifest:
        if entry["mode"] != 0:
            continue
        out.append(scene_stats(entry))
    (HERE / "f5_pb_stats.json").write_text(
        json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")

    hdr = ("scene recs | U inst Mref(Mv2 ok) | peak/empty PB vs v2 | "
           "PBKB v2KB | wrPB wrV2 | rdBuild rdExec metaSeg instKB")
    print(hdr)
    for s in out:
        print("%5d %4d | %3d %5d %5d(%5d %s) | %4d/%3d vs %4d/%3d | "
              "%6.2f %6.2f | %5d %5d | %6d %6d %6d %6.2f" % (
                  s["scene"], s["records"], s["u_packets"], s["inst_total"],
                  s["m_total"], s["m_v2"], "OK" if s["m_check"] else "MISMATCH",
                  s["pb_peak"], s["pb_empty"], s["m_v2_peak"], s["v2_empty"],
                  s["pb_bytes"] / 1024.0, s["v2_bytes"] / 1024.0,
                  s["pb_wr_beats"], s["v2_wr_beats"],
                  s["build_rd_beats"], s["ex_rd_beats"], s["ex_meta_segs"],
                  s["ex_inst_bytes"] / 1024.0))
    bad = [s["scene"] for s in out if not s["m_check"]]
    print("H=16 M 交叉验证：" + ("全部通过" if not bad else "失败场景 %s" % bad))

    out32 = [scene_stats(e, strip_h=32) for e in manifest if e["mode"] == 0]
    print()
    print("scene | H=32: S Mref peak/empty PBKB wrPB rdBuild rdExec")
    for s in out32:
        print("%5d | %2d %5d %4d/%3d %6.2f %5d %6d %6d" % (
            s["scene"], s["lstrips"], s["m_total"],
            s["pb_peak"], s["pb_empty"], s["pb_bytes"] / 1024.0,
            s["pb_wr_beats"], s["build_rd_beats"], s["ex_rd_beats"]))
    print("H=32 无 v2 直接对照（mode1 表按 16 行生成）")


if __name__ == "__main__":
    main()
