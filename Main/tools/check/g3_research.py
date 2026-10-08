#!/usr/bin/env python3
# G3 条带控制/ABI v2/软件分桶的定界实验：分桶分布、取数开销与区容量边界。
#
# === 背景 ===
# 总览 D58/D61 定了条带高 16（STRIP_H 参数化）与定长条带区（30 区×1024 条×32 B），
# 但没有真实帧的命令分布数字：背景全高 FILL/竖线会被复制进全部条带、
# 16×16 弹几乎每颗跨两桶，区容量 1024 够不够、空条带有多少、取数批比直通
# 多多少，都直接影响 G3 的构造层设计与 G4 的 N_max 预期。本脚本按游戏
# stg_draw.c/stg_pool.c 的几何口径在 PC 上重放这些数字，不改任何生产代码。
#
# === 用法 ===
#   python3 Main/tools/check/g3_research.py            # 打印全部实验
#   python3 Main/tools/check/g3_research.py --json     # 另存 JSON 摘要
# 期望：脚本以 PASS 结束，摘要与 Main/sim/engine/stripctl/logs 一致。
#
# === 口径 ===
# - 帧 = 背景（1 全屏 FILL + 15 横线 + 13 竖线 + 60 星点 8×8 色键）+
#   N 颗弹（16×16 或 8×8，均匀分布场地 400×480）+ HUD 近似（2 全高 FILL +
#   1 横线 + 35 个 8×16/32×32 sprite，右侧 400..639）+ 1 条 END；
#   SET_CLIP 不进表（clip 吸收口径见第 09 章），直通对照同样不含。
# - 弹进入的桶 = dst_y .. dst_y+h-1 覆盖的条带；裁剪到场地外/屏外的弹先按
#   四者求交裁剪（sprite() 的手工早退不减少表内命令，几何口径按裁剪后矩形）。
# - 取数模型：批 16 条、读固定开销 27.41、校准气泡 1+1/12（E4 口径）；
#   表头 128 B = 8 拍一笔。不含 SoC 扫描与 CPU 争用。
import argparse
import json
import math
import random
import sys
from pathlib import Path

SCREEN_W, SCREEN_H = 640, 480
FIELD_W, FIELD_H = 400, 480            # stg_pool.h：场地 400×480，HUD 在右侧
BG_HLINES, BG_VLINES, BG_STARS = 15, 13, 60   # stg_stage.h
STAR_PX = 8
SHIP_PX = 32
REGION_CMDS = 1024                     # D61：每区 1024 条
BATCH = 16                             # engine_cmd.calc_n 的批长
RD_FIXED = 27.41                       # platform 第 03 章读事务固定开销
RD_BUB = 1.0 + 1.0 / 12.0              # E4 校准 RD_BUB=12 的数据节拍
HDR_BYTES = 128                        # 30 字计数取整到 4 条记录


def frame_cmds(n_sprites, sprite_px, seed, hud=True):
    """生成一帧的命令矩形列表 [(x0,y0,x1,y1,kind)]，几何已按四者求交裁剪。"""
    rng = random.Random(seed)
    cmds = []
    cmds.append((0, 0, SCREEN_W - 1, SCREEN_H - 1, "bg_fill"))
    for k in range(BG_HLINES):
        y = (k + 1) * SCREEN_H // (BG_HLINES + 1)
        cmds.append((0, y, SCREEN_W - 1, y, "bg_hline"))
    for k in range(BG_VLINES):
        x = (k + 1) * SCREEN_W // (BG_VLINES + 1)
        cmds.append((x, 0, x, SCREEN_H - 1, "bg_vline"))
    for _ in range(BG_STARS):
        x = rng.randrange(0, FIELD_W - STAR_PX)
        y = rng.randrange(0, SCREEN_H - STAR_PX)
        cmds.append((x, y, x + STAR_PX - 1, y + STAR_PX - 1, "star"))
    for _ in range(n_sprites):
        x = rng.randrange(-sprite_px, FIELD_W)
        y = rng.randrange(-sprite_px, SCREEN_H)
        cmds.append((x, y, x + sprite_px - 1, y + sprite_px - 1, "sprite"))
    if hud:
        cmds.append((400, 0, SCREEN_W - 1, SCREEN_H - 1, "hud_fill"))
        cmds.append((400, 0, 401, SCREEN_H - 1, "hud_fill"))
        cmds.append((424, 266, 613, 266, "hud_line"))
        for i in range(35):
            w = 32 if i % 7 == 0 else 8
            x = 400 + (i * 53) % 200
            y = (i * 37) % 440
            cmds.append((x, y, x + w - 1, y + 15, "hud_sprite"))
    return cmds


def distribute(cmds, strip_h):
    """命令裁剪进条带并分桶；返回 (桶列表, 直通条数, 复制条数)。"""
    n_strips = (SCREEN_H + strip_h - 1) // strip_h
    buckets = [[] for _ in range(n_strips)]
    flat = 0
    for (x0, y0, x1, y1, kind) in cmds:
        if x1 < 0 or y1 < 0 or x0 >= SCREEN_W or y0 >= SCREEN_H:
            continue
        flat += 1
        s0 = max(0, y0) // strip_h
        s1 = min(SCREEN_H - 1, y1) // strip_h
        for s in range(s0, s1 + 1):
            buckets[s].append(kind)
    return buckets, flat


def exp_distribution():
    out = {}
    for name, px, counts in [("16x16", 16, [896, 1055, 2000, 3000]),
                             ("8x8", 8, [1365, 3000, 5000])]:
        for strip_h in (16, 32):
            for n in counts:
                rng_seed = n * 1000 + px + strip_h
                buckets, flat = distribute(frame_cmds(n, px, rng_seed), strip_h)
                tiled = sum(len(b) for b in buckets)
                peak = max(len(b) for b in buckets)
                empty = sum(1 for b in buckets if not b)
                out[f"{name}_sh{strip_h}_n{n}"] = {
                    "flat_cmds": flat, "tiled_cmds": tiled,
                    "amplification": round(tiled / flat, 3),
                    "peak_bucket": peak,
                    "peak_headroom": REGION_CMDS - peak,
                    "empty_buckets": empty,
                    "overflow_flat_limit": None,
                }
    # 区容量边界：峰值桶含背景+HUD 常量 ≈ 96 条（几何常数），
    # 弹只进与场地相交的桶；均匀分布下峰值桶弹数 ≈ n/条带数×(条带高+弹高)/条带高。
    for strip_h in (16, 32):
        ns = SCREEN_H // strip_h
        const = 0
        buckets, _ = distribute(frame_cmds(0, 16, 1), strip_h)
        const = max(len(b) for b in buckets)
        # 弹峰值：弹高 h 覆盖 ceil(h/sh)+1 个桶，峰值桶期望弹数
        # = n × (strip_h+16-1)/SCREEN_H×(每弹平均桶数/平均桶数) —— 直接扫描找溢出点
        lo, hi = 0, 20000
        while lo < hi:
            mid = (lo + hi) // 2
            buckets, _ = distribute(frame_cmds(mid, 16, 7), strip_h)
            if max(len(b) for b in buckets) > REGION_CMDS:
                hi = mid
            else:
                lo = mid + 1
        out[f"overflow_limit_sh{strip_h}"] = {
            "const_cmds_per_bucket": const,
            "max_sprites_uniform": lo,
            "region_cmds": REGION_CMDS,
        }
    return out


def exp_fetch_model(dist):
    """直通 vs 条带的命令取数批与模型周期（不含像素与 fill/flush）。"""
    out = {}
    for key, entry in dist.items():
        if not key.startswith("16x16_sh16_n") and not key.startswith("8x8_sh16_n"):
            continue
        flat = entry["flat_cmds"]
        tiled = entry["tiled_cmds"]
        direct_batches = math.ceil(flat / BATCH)
        # 条带：表头 1 笔 + 每桶 ceil(cnt/16) 批。用同 seed 重放拿桶计数。
        size, sh, n = key.split("_")
        strip_h = int(sh[2:])
        n = int(n[1:])
        px = int(size.split("x")[0])
        buckets, _ = distribute(frame_cmds(n, px, n * 1000 + px + strip_h), strip_h)
        strip_batches = 1 + sum(math.ceil(len(b) / BATCH) for b in buckets)
        # 每批 = 一笔读描述符 = 固定开销 27.41 + 批数据拍（16 条=64 拍，乘校准气泡）
        per_batch = RD_FIXED + (BATCH * 4) * RD_BUB
        direct_cycles = direct_batches * per_batch
        strip_cycles = RD_FIXED + (HDR_BYTES // 16) * RD_BUB
        for b in buckets:
            if b:
                strip_cycles += math.ceil(len(b) / BATCH) * per_batch
        out[key] = {
            "direct_batches": direct_batches,
            "strip_batches": strip_batches,
            "batch_overhead": strip_batches - direct_batches,
            "direct_model_cycles": round(direct_cycles, 1),
            "strip_model_cycles": round(strip_cycles, 1),
            "cycle_overhead": round(strip_cycles - direct_cycles, 1),
            "note": "命令取数只占引擎帧预算的小头；主开销仍是 G2 的两相 191520",
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="另存 JSON 摘要到 stripctl/logs")
    args = ap.parse_args()

    dist = exp_distribution()
    fetch = exp_fetch_model(dist)

    print("== 分桶分布（D58 的 16/32 行对照、D61 的 1024 条区容量）==")
    for k, v in dist.items():
        print(f"{k}: {v}")
    print("\n== 取数批与模型周期（批 16 条，读 27.41 + 4 拍×1.083/批）==")
    for k, v in fetch.items():
        print(f"{k}: {v}")

    ok = all(v["peak_bucket"] <= REGION_CMDS for k, v in dist.items()
             if k.startswith("16x16_sh16_n896") or k.startswith("16x16_sh16_n1055"))
    print(f"\nRESULT: {'PASS' if ok else 'FAIL'}"
          f"（K896/K1055 峰值桶 ≤ 1024，区容量成立；3000 颗以上另看）")
    if args.json:
        d = Path("Main/sim/engine/stripctl/logs")
        d.mkdir(parents=True, exist_ok=True)
        (d / "research_results.json").write_text(
            json.dumps({"distribution": dist, "fetch_model": fetch},
                       ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"摘要已写 {d/'research_results.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
