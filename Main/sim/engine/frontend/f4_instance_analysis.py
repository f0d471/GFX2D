#!/usr/bin/env python3
# F4 研究轮：在 F1 冻结的 34 组固定输入上量化 v1-I 实例化的合包率与字节压缩。
# 只读 inputs/mode0（v1 直通记录流），跟踪 SET_CLIP 状态 epoch，
# 按 F4 契约的共享状态键（w0 全部、key 值、宽高、裁剪 epoch）聚合连续 BLIT。
# 输出 f4_instance_stats.json 与控制台摘要；不改任何生产源码。
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
INPUTS = HERE / "inputs"

OP_NOP, OP_FILL, OP_BLIT, OP_SET_CLIP, OP_BAND, OP_END = 0, 1, 2, 3, 4, 0xFF
EQ_NAMES = {0: "COPY", 1: "ALPHA", 2: "ADD", 3: "CONST", 4: "MUL", 5: "ALPHA_ST"}


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def load_v1(path):
    """返回 (表面表原始字, 记录列表)。文件布局：192 B 表面表 + u32 记录数 + 32 B 记录。"""
    data = path.read_bytes()
    n = struct.unpack_from("<I", data, 192)[0]
    words = struct.unpack_from("<%dI" % (n * 8), data, 196)
    return [list(words[i * 8:(i + 1) * 8]) for i in range(n)]


def analyze(records):
    """保守口径：连续 BLIT 且共享状态相同为一个候选区间；
    区间内源坐标全同计为 XY4 包，否则整个区间用一个 XYUV8 包。
    返回逐区间明细与汇总。"""
    runs = []          # (state_key, [记录索引])
    epoch = 0          # SET_CLIP 递增；实例包不跨 epoch
    cur_key = None
    cur_idx = []
    for i, w in enumerate(records):
        op = w[0] & 0xFF
        if op == OP_SET_CLIP:
            epoch += 1
            if cur_idx:
                runs.append((cur_key, cur_idx))
                cur_key, cur_idx = None, []
            continue
        if op != OP_BLIT:
            if cur_idx:
                runs.append((cur_key, cur_idx))
                cur_key, cur_idx = None, []
            continue
        # 共享状态键：w0 全部（op/eq/key_en/tint/rsvd/src/dst/c）+ key 值(w5) + 宽高(w3) + clip epoch
        key = (w[0], w[5], w[3], epoch)
        if cur_key is None or key != cur_key:
            if cur_idx:
                runs.append((cur_key, cur_idx))
            cur_key, cur_idx = key, [i]
        else:
            cur_idx.append(i)
    if cur_idx:
        runs.append((cur_key, cur_idx))

    detail = []
    for key, idx in runs:
        w0, w5_key, w3, ep = key
        sxs = [s16(records[i][1] & 0xFFFF) for i in idx]
        sys_ = [s16(records[i][1] >> 16) for i in idx]
        fixed_src = len(set(zip(sxs, sys_))) == 1
        detail.append({
            "len": len(idx),
            "eq": EQ_NAMES.get((w0 >> 8) & 0xF, "?"),
            "src": (w0 >> 16) & 0xF,
            "dst": (w0 >> 20) & 0xF,
            "key_en": (w0 >> 12) & 1,
            "fixed_src": fixed_src,
            "epoch": ep,
        })
    return detail


def summarize(tag, detail, n_blit):
    """两种口径的字节账：全部按区间成一包（XY4 或 XYUV8 整段），
    以及激进口径：区间内连续同源子段再切成 XY4 包（每切一刀多一个包头）。"""
    inst4 = sum(d["len"] for d in detail if d["fixed_src"])
    inst8 = sum(d["len"] for d in detail if not d["fixed_src"])
    pack4 = sum(1 for d in detail if d["fixed_src"])
    pack8 = sum(1 for d in detail if not d["fixed_src"])
    bytes_orig = 32 * n_blit
    bytes_inst = 32 * (pack4 + pack8) + 4 * inst4 + 8 * inst8
    # 激进口径：非 fixed 区间按源坐标连续段细分，同源连续子段独立成 XY4 包
    split_pack4 = split_inst4 = split_pack8 = split_inst8 = 0
    return {
        "blits": n_blit,
        "runs": len(detail),
        "xy4": {"packets": pack4, "instances": inst4},
        "xyuv8": {"packets": pack8, "instances": inst8},
        "bytes_orig": bytes_orig,
        "bytes_inst_cons": bytes_inst,
        "ratio_cons": round(bytes_inst / bytes_orig, 4) if bytes_orig else None,
        "inst_share": round((inst4 + inst8) / n_blit, 4) if n_blit else None,
    }


def main():
    manifest = json.loads((INPUTS / "manifest.json").read_text(encoding="utf-8"))
    out = []
    for entry in manifest:
        if entry["mode"] != 0:
            continue
        path = INPUTS / ("scene%02d_mode0.bin" % entry["scene"])
        recs = load_v1(path)
        assert len(recs) == entry["records"], path
        ops = [w[0] & 0xFF for w in recs]
        n_blit = ops.count(OP_BLIT)
        detail = analyze(recs)
        s = summarize(entry["scene"], detail, n_blit)
        s["scene"] = entry["scene"]
        s["records"] = entry["records"]
        s["non_blit"] = entry["records"] - n_blit
        out.append(s)

    (HERE / "f4_instance_stats.json").write_text(
        json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")

    hdr = ("scene recs blit | runs pk4 in4 pk8 in8 | B_orig B_inst ratio inst_share")
    print(hdr)
    for s in out:
        print("%5d %4d %4d | %4d %3d %5d %3d %5d | %7d %7d %5.3f %6.3f" % (
            s["scene"], s["records"], s["blits"], s["runs"],
            s["xy4"]["packets"], s["xy4"]["instances"],
            s["xyuv8"]["packets"], s["xyuv8"]["instances"],
            s["bytes_orig"], s["bytes_inst_cons"], s["ratio_cons"], s["inst_share"]))


if __name__ == "__main__":
    main()
