#!/usr/bin/env python3
"""构造前端研究：既有板测 CSV 回归与真实游戏分阶段记录计数。仅运行于宿主。"""
import csv
import io
import json
import re
import shutil
import statistics
import subprocess
from pathlib import Path

MAIN = Path(__file__).resolve().parents[2]
ROOT = MAIN.parent
SIM = MAIN / "sim/engine/frontend"
BUILD = SIM / "build/research"
GAME = MAIN / "sw/apps/game/gfx2dGame/src"
LIB = MAIN / "sw/lib"


def run(args):
    return subprocess.run([str(x) for x in args], cwd=ROOT, check=True,
                          capture_output=True, text=True).stdout


def regression(rows, xkey, ykey):
    xs = [float(r[xkey]) for r in rows]
    ys = [float(r[ykey]) for r in rows]
    xm, ym = statistics.mean(xs), statistics.mean(ys)
    xx = sum((x - xm)**2 for x in xs)
    if not xx:
        raise ValueError("regression requires multiple activity levels")
    slope = sum((x - xm)*(y - ym) for x, y in zip(xs, ys)) / xx
    intercept = ym - slope*xm
    ss = sum((y - ym)**2 for y in ys)
    residual = sum((y - intercept - slope*x)**2 for x, y in zip(xs, ys))
    return {"windows": len(rows), "x_min": min(xs), "x_max": max(xs),
            "intercept": round(intercept, 3), "slope": round(slope, 3),
            "r_squared": round(1 - residual/ss, 6) if ss else None}


def board_csv():
    result = {}
    for name in ("strip_4k", "strip_16k", "tex_4k"):
        source = MAIN / f"sim/engine/g4/logs/nmax_{name}.log"
        header = None
        kind = None
        groups = {16: [], 8: []}
        for line in source.read_text(encoding="utf-8", errors="strict").splitlines():
            if line.startswith("nmax load "):
                kind = int(line.split()[-1].split("x")[0])
            elif line.startswith("csvh,"):
                header = line.split(",")[1:]
            elif line.startswith("csv,"):
                values = line.split(",")[1:]
                if not header or len(values) != len(header):
                    raise ValueError(f"CSV column count mismatch: {source.name}")
                row = dict(zip(header, values))
                if kind in groups and row["bottleneck"] != "warming" and int(row["overflow"]) == 0:
                    groups[kind].append(row)
        result[name] = {"source": source.relative_to(ROOT).as_posix(), "by_kind": {}}
        for kind, rows in groups.items():
            if not rows:
                raise ValueError(f"no usable windows: {source.name} / {kind}")
            result[name]["by_kind"][kind] = {
                key: regression(rows, "n_bullet", key)
                for key in ("cyc_logic", "cyc_build", "cpu_avg_cyc", "engine_busy_avg_cyc", "cmd")
            }
    return result


def game_counts():
    BUILD.mkdir(parents=True, exist_ok=True)
    text = (GAME / "game/stg_draw.c").read_text(encoding="utf-8")
    text = text.replace("int stg_game_compose(", "int stg_game_compose_observed(", 1)
    # 生产版的主体在 stg_game_compose_layers；观察副本的主体与包装调用同时改名，避免重定义。
    text = text.replace("stg_game_compose_layers(", "stg_game_compose_layers_observed(")
    text = '#include "gfx/gfx.h"\nextern void frontend_mark(gfx_list *, unsigned);\n' + text
    replacements = [
        ("    draw_background(&d, &g->bg);", "    frontend_mark(l, 0);\n    draw_background(&d, &g->bg);\n    frontend_mark(l, 1);"),
        ("    draw_player(&d, g);", "    draw_player(&d, g);\n    frontend_mark(l, 2);"),
        ("    if (gfx_set_clip(l, screen) != 0)", "    frontend_mark(l, 6);\n    if (gfx_set_clip(l, screen) != 0)"),
        ("    draw_hud(&d, g, live);", "    draw_hud(&d, g, live);\n    frontend_mark(l, 7);"),
        ("    return 0;\n}", "    frontend_mark(l, 8);\n    return 0;\n}"),
    ]
    for before, after in replacements:
        if text.count(before) != 1:
            raise ValueError(f"production observation anchor changed: {before}")
        text = text.replace(before, after, 1)
    for stage, name in ((3, "pbullet"), (4, "ebullet"), (5, "particle")):
        pattern = rf"(?m)^(    a->{name}\s*=.*;)\s*$"
        text, n = re.subn(pattern, lambda m: m[1] + f"\n    frontend_mark(l, {stage});", text)
        if n != 1:
            raise ValueError(f"pool observation anchor changed: {name}")
    observed = BUILD / "stg_draw_observed.c"
    observed.write_text(text, encoding="utf-8")
    flags = ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
             "-fdata-sections", "-Wl,--gc-sections"]
    includes = [LIB, MAIN/"sw/bsp/include", GAME, GAME/"game", MAIN/"assets/build"]
    exe = BUILD / "probe_frontend"
    run(["gcc", *flags, *["-I" + str(p) for p in includes], SIM/"probe_frontend.c", observed,
         GAME/"stg_modes.c", *sorted((GAME/"game").glob("stg_*.c")),
         *sorted((LIB/"gfx").glob("*.c")), *sorted((LIB/"core").glob("*.c")), "-o", exe])
    output = run([exe, MAIN/"assets/build/assets.pak"])
    (BUILD/"samples.csv").write_text(output, encoding="utf-8")
    groups = {}
    for row in csv.DictReader(io.StringIO(output)):
        key = (int(row["kind"]), int(row["knob"]))
        groups.setdefault(key, []).append({k: int(v) for k, v in row.items()})
    result = []
    for (kind, knob), rows in groups.items():
        averages = {k: round(statistics.mean(r[k] for r in rows), 3)
                    for k in rows[0] if k not in ("kind", "knob", "frame")}
        averages.update(kind=kind, knob=knob, samples=len(rows))
        averages["all_bg_hud_removed_fraction"] = round((averages["bg_v2"] + averages["hud_v2"])/averages["total_v2"], 6)
        result.append(averages)
    if len(result) != 14 or sum(r["samples"] for r in result) != 1680:
        raise ValueError("incomplete game census")
    return result


def main():
    result = {
        "scope": "Host command counts and observational regressions of existing G4 board CSV; no new board performance claim",
        "source_commit": run(["git", "rev-parse", "HEAD"]).strip(),
        "host_compiler": run(["gcc", "--version"]).splitlines()[0],
        "board_regression_caveat": "All non-warming, non-overflow windows, including failed search levels; not controlled same-state microbenchmarks",
        "board_regressions": board_csv(), "game_census": game_counts(),
        "observer_equivalence": "1680 states x flat/strip; full command buffers, counts, errors and frame stats equal to unmodified production compose",
    }
    (SIM/"research_results.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("PASS: 1680 real game states, flat/strip observers identical; existing CSV regressions recorded")
    for row in result["game_census"]:
        if row["kind"] == 16 and row["knob"] in (0, 1744, 2400):
            print(json.dumps(row, ensure_ascii=False))
    shutil.rmtree(BUILD)


if __name__ == "__main__":
    main()
