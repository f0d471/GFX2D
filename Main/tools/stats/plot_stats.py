#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""plot_stats.py —— 把 gfx2dGame 的统计日志画成报告与幻灯片用的三张图（sw 第 07 章 3.11 节）

输入是模式框架打出来的日志（PC 的标准输出或板上 board_run.py 录下的串口日志都行），
只认下面几种行，其余行忽略（行尾的 \\r 与 board_run.py 的 # 标记行不影响）：

    csvh,t,mode,exec,...          表头：列名以它为准（sw 第 07 章 3.9 节）
    csv,<每 60 场一行>
    csvsum,frames,fields,...
    nmax load 16x16|8x8           一遍搜索开始
    nmax cfg ... F=<一场的周期数>
    nmax probe <序号> <阶段> knob .. settle .. fields .. frames .. drops .. act_min .. ft_max .. ft_p99 .. pass|fail
    nmax result <状态> n_max <N 或 -> knob <旋钮或 -> ...

输出三张图的源码（HTML 内嵌 SVG，尺寸 1760x880，公共样式 _fig.css），
再用 Deliverables/Assert/figures/src/render.sh 导出 PNG：

    <前缀>pixel_share.html   按混合方程的像素占比：每个场景一行 100% 堆叠条，取抽样帧的平均
    <前缀>frame_time.html    帧时间分布：每个场景一格，统计每秒一行里的 ft_max，横轴以「场」为单位
    <前缀>nmax_search.html   N_max 搜索轨迹：每遍搜索一格，逐档的旋钮、通过或掉帧、最后的结果

用法：

    python3 plot_stats.py --out <figures/src 目录> [--prefix sw8_] [--ref-s1] [--nmax 日志 ...] 日志[=标签[@跳过行数]] ...

    日志=标签        位置参数的日志出前两张图：这份日志在图里叫什么（缺省用文件名）；一份日志里有几个模式就拆成几行，
                     模式 1 按抽样帧的 n_active 再拆成场景 A（方块数大于 0）与场景 B；帧时间图最多画前四个场景
    @跳过行数        每个场景丢掉开头这么多行 csv（负载还在爬升的那几秒）
    --nmax 日志      只取它的 nmax 行出搜索轨迹图（压力测试在扫负载，它的 csv 行不进前两张图的平均）；
                     位置参数的日志里有 nmax 行时也画进去
    --ref-s1         像素占比图最后加一行设计文档 4.5 节的 S1 估算，标「估算」
    --hz             板子的时钟频率，缺省 100000000；一场 F = hz / 60（日志里有 nmax cfg 时以 F= 为准）

只用标准库（WSL 的 python3 没有 matplotlib）。标准输出打出每张图用到的数（各场景的方程像素均值、
每秒 ft_max 的分布、每遍搜索的档数与结果），开发日志与报告引用这些数，不从图上读。退出码 0 表示三张图都出了；
缺某种行时对应那张图不出、打一行说明，退出码 1。
"""

import argparse
import html
import os
import sys

EQ_NAMES = ["COPY", "ALPHA", "ADD", "CONST", "MUL", "ALPHA_ST"]
# 与 Deliverables/Assert/figures/src/profile.html 的方程颜色一致：COPY 蓝、ALPHA 绿、ADD 金；CONST 红；
# MUL 与 ALPHA_ST（直通 ALPHA）本项目的场景里不出现，出现时用灰。
# 这组颜色在 #FAFAF7 上过了 dataviz 的配色验证：金与绿相邻那一对的色盲区分度在 6~8 之间，
# 所以每段都带直接标注（放得下时）与 2 像素间隙，图例常在。
EQ_COLORS = ["#4A7FD0", "#2E7D4F", "#A37A1C", "#E8546B", "#87929E", "#B7BEC7"]
MODE_NAMES = {"demo": "模式 1", "game": "模式 2 游戏", "press": "模式 3 压力"}

INK = "#1D2733"
INK2 = "#3E4A59"
MUTED = "#6B7684"
GRID = "#E3E6EA"
BLUE = "#4A7FD0"
GRAY = "#87929E"
SURFACE = "#FAFAF7"

# 设计文档 4.5 节场景 S1 的估算（按像素）。本 ABI 里 FILL 走 COPY 方程，所以 FILL 与三种 COPY 合在 COPY 里；
# 受击闪白取 0（它是偶发的）。数字照原表逐项相加，改表时这里同步改。
REF_S1 = {
    "COPY": 192000 + 115200 + 20480 + 1024 + 7680 + 2000,
    "ALPHA": 288000,
    "ADD": 5120 + 51200,
    "CONST": 0,
}


# --------------------------------------------------------------------------- 解析

class Log:
    def __init__(self, path, label, skip):
        self.path = path
        self.label = label
        self.skip = skip
        self.cols = None
        self.rows = []          # dict 列名 -> 字符串
        self.sums = []
        self.passes = []        # 每遍搜索：{"name", "probes": [...], "result": {...}}
        self.field_cyc = None


def parse_kv_tail(parts):
    """把 'knob 1744 settle 300 timeout fields 120 ...' 这样的尾巴拆成字典；单独的词记成 True。"""
    out = {}
    i = 0
    while i < len(parts):
        k = parts[i]
        if i + 1 < len(parts) and (parts[i + 1].isdigit() or parts[i + 1] == "-"):
            out[k] = parts[i + 1]
            i += 2
        else:
            out[k] = True
            i += 1
    return out


def parse_log(path, label, skip):
    lg = Log(path, label, skip)
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip().replace("\r", "")
            if not line or line.startswith("#"):
                continue
            if line.startswith("csvh,"):
                lg.cols = line.split(",")[1:]
            elif line.startswith("csv,") and lg.cols:
                vals = line.split(",")[1:]
                if len(vals) == len(lg.cols):
                    lg.rows.append(dict(zip(lg.cols, vals)))
            elif line.startswith("csvsum,"):
                lg.sums.append(line.split(",")[1:])
            elif line.startswith("nmax load "):
                cur = {"name": line.split()[2], "probes": [], "result": None}
                lg.passes.append(cur)
            elif line.startswith("nmax cfg"):
                for p in line.split():
                    if p.startswith("F="):
                        lg.field_cyc = int(p[2:])
            elif line.startswith("nmax probe ") and cur is not None:
                parts = line.split()
                rec = parse_kv_tail(parts[4:])
                rec["index"] = int(parts[2])
                rec["stage"] = parts[3]
                rec["pass"] = "pass" in rec
                cur["probes"].append(rec)
            elif line.startswith("nmax result ") and cur is not None:
                parts = line.split()
                rec = parse_kv_tail(parts[3:])
                rec["status"] = parts[2]
                cur["result"] = rec
    return lg


def groups_of(logs):
    """按（日志、模式、场景）分组 csv 行，保持出现顺序；每组丢掉开头 skip 行。"""
    out = []
    for lg in logs:
        modes = []
        for r in lg.rows:
            if r["mode"] not in modes:
                modes.append(r["mode"])
        for m in modes:
            rows = [r for r in lg.rows if r["mode"] == m]
            if m == "demo":
                subs = [("场景 A", [r for r in rows if int(r["n_active"]) > 0]),
                        ("场景 B", [r for r in rows if int(r["n_active"]) == 0])]
            else:
                subs = [("", rows)]
            for sub, rs in subs:
                rs = rs[lg.skip:]
                if not rs:
                    continue
                name = lg.label
                if len(modes) > 1 or m == "demo":
                    name = name + " · " + MODE_NAMES.get(m, m) + ((" " + sub) if sub else "")
                out.append({"name": name, "mode": m, "rows": rs})
    return out


# --------------------------------------------------------------------------- 画图的公共件

def page(title, body_svg, w=1760, h=880):
    return ("<!doctype html><html><head><meta charset=\"utf-8\">\n"
            "<meta name=\"fig-size\" content=\"%dx%d\">\n"
            "<title>%s</title>\n"
            "<link rel=\"stylesheet\" href=\"_fig.css\">\n"
            "</head>\n<body><div id=\"fig\" style=\"width:%dpx;height:%dpx\">\n"
            "<!-- 生成物：Main/tools/stats/plot_stats.py 按日志生成，改图改脚本或日志后重新生成，不手改 -->\n"
            "<svg width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" "
            "style=\"position:absolute;left:0;top:0\" font-family=\"Noto Sans SC, Microsoft YaHei, sans-serif\">\n"
            "%s</svg>\n</div></body></html>\n") % (w, h, html.escape(title), w, h, w, h, w, h, body_svg)


def text(x, y, s, size=19, color=INK2, anchor="start", weight="400"):
    return ("<text x=\"%.1f\" y=\"%.1f\" font-size=\"%d\" fill=\"%s\" text-anchor=\"%s\" font-weight=\"%s\">%s</text>\n"
            % (x, y, size, color, anchor, weight, html.escape(s)))


def rect(x, y, w, h, fill, rx=0):
    return ("<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"%d\" fill=\"%s\"/>\n"
            % (x, y, max(w, 0.0), max(h, 0.0), rx, fill))


def line(x1, y1, x2, y2, color=GRID, w=1, dash=None):
    d = (" stroke-dasharray=\"%s\"" % dash) if dash else ""
    return ("<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"%g\"%s/>\n"
            % (x1, y1, x2, y2, color, w, d))


def nice_step(vmax, ticks):
    """1、2、5 乘 10 的幂里，让 vmax 分成不超过 ticks 格的最小一档。"""
    mag = 1
    while True:
        for m in (1, 2, 5):
            if vmax <= m * mag * ticks:
                return m * mag
        mag *= 10


def fmt_px(v):
    if v >= 10000:
        return "%.1f 万" % (v / 10000.0)
    return "%d" % round(v)


# --------------------------------------------------------------------------- 图一：按方程的像素占比

def chart_pixel_share(groups, ref_s1):
    rows = []
    for g in groups:
        n = len(g["rows"])
        mean = [sum(int(r["pix_eq%d" % e]) for r in g["rows"]) / float(n) for e in range(6)]
        rows.append((g["name"], mean, "抽样 %d 秒" % n))
    if ref_s1:
        mean = [float(REF_S1.get(EQ_NAMES[e], 0)) for e in range(6)]
        rows.append(("设计文档 4.5 节 S1（估算）", mean, "估算"))
    used = [e for e in range(6) if any(r[1][e] > 0 for r in rows)]

    x0, x1 = 420, 1500           # 条的左右端
    top, pitch, bar_h = 160, 124, 44
    body = text(20, 40, "按混合方程的像素占比（每行一个场景，抽样帧的平均）", 24, INK, weight="700")
    # 图例：常在
    lx = 20
    for e in used:
        body += rect(lx, 70, 22, 22, EQ_COLORS[e], 4)
        body += text(lx + 30, 89, EQ_NAMES[e], 19, INK2)
        lx += 30 + 16 * len(EQ_NAMES[e]) + 40
    body += text(x1 + 40, 128, "每帧像素（均值）", 18, MUTED)
    report = []
    for i, (name, mean, note) in enumerate(rows):
        y = top + i * pitch
        total = sum(mean)
        body += text(20, y + bar_h / 2 + 7, name, 20, INK, weight="700")
        body += text(20, y + bar_h / 2 + 33, note, 16, MUTED)
        x = float(x0)
        segs = [(e, mean[e]) for e in used if mean[e] > 0]
        for j, (e, v) in enumerate(segs):
            w = (x1 - x0) * v / total if total > 0 else 0.0
            gap = 2.0 if j + 1 < len(segs) else 0.0      # 段与段之间 2 像素的底色间隙
            body += rect(x, y, w - gap, bar_h, EQ_COLORS[e], 4 if (j == 0 or j + 1 == len(segs)) else 0)
            pct = 100.0 * v / total
            lab = "%s %.0f%%" % (EQ_NAMES[e], pct)
            if w - gap >= 11 * len(lab) + 16:
                body += text(x + (w - gap) / 2, y + bar_h / 2 + 7, lab, 18, "#FFFFFF", "middle", "700")
            x += w
        body += text(x1 + 40, y + bar_h / 2 + 7, fmt_px(total), 20, INK, weight="700")
        report.append("pixel_share %s total %d %s" % (
            name, round(total), " ".join("%s %d(%.1f%%)" % (EQ_NAMES[e], round(mean[e]),
                                         100.0 * mean[e] / total if total else 0.0) for e in used)))
    return page("按方程的像素占比", body), report


# --------------------------------------------------------------------------- 图二：帧时间分布

def chart_frame_time(groups, field_cyc):
    panels = groups[:4]
    n = len(panels)
    pw = (1760 - 40 - (n - 1) * 30) / float(n)
    bins, lo, hi = 32, 0.0, 2.0                # 0~2 场，每格 1/16 场；超出的进最后一格
    body = text(20, 40, "帧时间分布（每秒一行的 ft_max，以场为单位，1 场 = %d 周期）" % field_cyc, 24, INK, weight="700")
    report = []
    for k, g in enumerate(panels):
        ft = [int(r["ft_max"]) / float(field_cyc) for r in g["rows"]]
        cnt = [0] * bins
        for v in ft:
            b = int((v - lo) / (hi - lo) * bins)
            cnt[min(max(b, 0), bins - 1)] += 1
        px0 = 20 + k * (pw + 30)
        py0, ph = 150, 560
        cmax = max(cnt) if cnt else 1
        ytop = max(1, cmax)
        body += text(px0, 110, g["name"], 20, INK, weight="700")
        over = sum(1 for v in ft if v > 1.0)
        body += text(px0, 136, "%d 秒 · 最大 %.2f 场 · 超过 1 场的 %d 秒" % (len(ft), max(ft) if ft else 0.0, over), 16, MUTED)
        ax_x0, ax_x1 = px0 + 50, px0 + pw - 10
        body += line(ax_x0, py0 + ph, ax_x1, py0 + ph, "#C9D0D8", 1)
        for t in (0.5, 1.0):
            yy = py0 + ph - ph * t
            body += line(ax_x0, yy, ax_x1, yy, GRID, 1)
            body += text(ax_x0 - 8, yy + 6, "%d" % round(ytop * t), 16, MUTED, "end")
        body += text(ax_x0 - 8, py0 + ph + 6, "0", 16, MUTED, "end")
        bw = (ax_x1 - ax_x0) / bins
        for b in range(bins):
            if cnt[b] == 0:
                continue
            h = ph * cnt[b] / float(ytop)
            body += rect(ax_x0 + b * bw + 1, py0 + ph - h, bw - 2, h, BLUE, 3)
        for t in (0.0, 0.5, 1.0, 1.5, 2.0):
            xx = ax_x0 + (ax_x1 - ax_x0) * t / (hi - lo)
            body += text(xx, py0 + ph + 30, ("%.1f" % t).rstrip("0").rstrip("."), 16, MUTED, "middle")
        xb = ax_x0 + (ax_x1 - ax_x0) * 1.0 / (hi - lo)
        body += line(xb, py0 - 10, xb, py0 + ph, "#B23A48", 2)
        body += text(xb + 8, py0 + 28, "一场预算", 16, "#B23A48")
        body += text((ax_x0 + ax_x1) / 2, py0 + ph + 58, "每秒最大帧时间（场）", 16, MUTED, "middle")
        report.append("frame_time %s seconds %d max %.3f over1 %d hist %s" % (
            g["name"], len(ft), max(ft) if ft else 0.0, over, ",".join(str(c) for c in cnt)))
    return page("帧时间分布", body), report


# --------------------------------------------------------------------------- 图三：N_max 搜索轨迹

STAGE_CN = {"double": "倍增", "bisect": "二分", "confirm": "确认"}


def chart_nmax(passes):
    n = len(passes)
    pw = (1760 - 40 - (n - 1) * 40) / float(n)
    body = text(20, 40, "N_max 搜索轨迹（逐档的旋钮；实心为通过，空心叉为掉帧）", 24, INK, weight="700")
    # 图例
    body += "<circle cx=\"31\" cy=\"80\" r=\"7\" fill=\"%s\" stroke=\"%s\" stroke-width=\"2\"/>\n" % (BLUE, SURFACE)
    body += text(46, 87, "这一档零掉帧（通过）", 18, INK2)
    body += ("<g stroke=\"%s\" stroke-width=\"2.5\"><circle cx=\"301\" cy=\"80\" r=\"7\" fill=\"%s\"/>"
             "<line x1=\"296\" y1=\"75\" x2=\"306\" y2=\"85\"/><line x1=\"296\" y1=\"85\" x2=\"306\" y2=\"75\"/></g>\n"
             % (GRAY, SURFACE))
    body += text(316, 87, "这一档有掉帧", 18, INK2)
    report = []
    for k, p in enumerate(passes):
        probes = p["probes"]
        res = p["result"] or {}
        px0 = 20 + k * (pw + 40)
        py0, ph = 170, 560
        ax_x0, ax_x1 = px0 + 80, px0 + pw - 20
        knobs = [int(q["knob"]) for q in probes]
        kmax = max(knobs) if knobs else 1
        step = nice_step(kmax, 5)
        ytop = step * (kmax // step + 1)
        name = {"16x16": "16×16 圆弹", "8x8": "8×8 小圆弹"}.get(p["name"], p["name"])
        body += text(px0, 128, name, 20, INK, weight="700")
        if res.get("status") in ("OK", "CAPPED"):
            summ = "%s · N_max %s · 旋钮 %s · %d 档" % (res["status"], res.get("n_max"), res.get("knob"), len(probes))
        else:
            summ = "%s · %d 档" % (res.get("status", "未结束"), len(probes))
        body += text(px0, 154, summ, 17, MUTED)
        v = 0
        while v <= ytop:
            yy = py0 + ph - ph * v / float(ytop)
            body += line(ax_x0, yy, ax_x1, yy, GRID if v else "#C9D0D8", 1)
            body += text(ax_x0 - 10, yy + 6, "{:,}".format(v), 16, MUTED, "end")
            v += step
        nq = max(len(probes), 1)
        dx = (ax_x1 - ax_x0) / float(nq + 1)
        pts = []
        for i, q in enumerate(probes):
            x = ax_x0 + dx * (i + 1)
            y = py0 + ph - ph * int(q["knob"]) / float(ytop)
            pts.append((x, y, q))
        if len(pts) > 1:
            body += ("<polyline fill=\"none\" stroke=\"#B7BEC7\" stroke-width=\"2\" stroke-linejoin=\"round\" points=\"%s\"/>\n"
                     % " ".join("%.1f,%.1f" % (x, y) for x, y, _ in pts))
        if res.get("status") in ("OK", "CAPPED") and res.get("knob", "-") != "-":
            yr = py0 + ph - ph * int(res["knob"]) / float(ytop)
            body += line(ax_x0, yr, ax_x1, yr, BLUE, 1.5, "6,5")
            body += text(ax_x0 + 8, yr - 10, "结果旋钮 %s" % res["knob"], 16, INK2)
        prev = None
        for x, y, q in pts:
            if q["pass"]:
                body += "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"7\" fill=\"%s\" stroke=\"%s\" stroke-width=\"2\"/>\n" % (x, y, BLUE, SURFACE)
            else:
                body += ("<g stroke=\"%s\" stroke-width=\"2.5\"><circle cx=\"%.1f\" cy=\"%.1f\" r=\"7\" fill=\"%s\"/>"
                         "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\"/><line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\"/></g>\n"
                         % (GRAY, x, y, SURFACE, x - 5, y - 5, x + 5, y + 5, x - 5, y + 5, x + 5, y - 5))
            if q["stage"] != prev:                         # 阶段换了：在横轴下标一次
                body += line(x, py0 + ph, x, py0 + ph + 8, "#C9D0D8", 1)
                body += text(x, py0 + ph + 30, STAGE_CN.get(q["stage"], q["stage"]), 16, MUTED, "middle")
                prev = q["stage"]
        body += text((ax_x0 + ax_x1) / 2, py0 + ph + 60, "搜索的第几档", 16, MUTED, "middle")
        report.append("nmax %s probes %d result %s n_max %s knob %s knobs %s" % (
            p["name"], len(probes), res.get("status", "-"), res.get("n_max", "-"), res.get("knob", "-"),
            ",".join(("%s%s" % (q["knob"], "" if q["pass"] else "x")) for q in probes)))
    return page("N_max 搜索轨迹", body), report


# --------------------------------------------------------------------------- 入口

def main():
    ap = argparse.ArgumentParser(description="把 gfx2dGame 的统计日志画成三张图")
    ap.add_argument("logs", nargs="+", help="日志[=标签[@跳过行数]]")
    ap.add_argument("--out", required=True, help="输出目录（Deliverables/Assert/figures/src）")
    ap.add_argument("--prefix", default="sw8_", help="输出文件名前缀，缺省 sw8_")
    ap.add_argument("--ref-s1", action="store_true", help="像素占比图加一行设计文档 4.5 节 S1 的估算")
    ap.add_argument("--hz", type=int, default=100000000, help="板子的时钟频率，缺省 100 MHz")
    ap.add_argument("--nmax", action="append", default=[], help="只取 nmax 行的日志，可给多次")
    args = ap.parse_args()

    logs = []
    for spec in args.logs:
        path, label, skip = spec, None, 0
        if "=" in spec:
            path, label = spec.split("=", 1)
            if "@" in label:
                label, s = label.rsplit("@", 1)
                skip = int(s)
        if label is None:
            label = os.path.splitext(os.path.basename(path))[0]
        logs.append(parse_log(path, label, skip))

    nlogs = [parse_log(p, p, 0) for p in args.nmax]
    field_cyc = next((lg.field_cyc for lg in logs + nlogs if lg.field_cyc), None) or (args.hz // 60)
    groups = groups_of(logs)
    passes = [p for lg in logs + nlogs for p in lg.passes if p["probes"]]
    made, missing = 0, 0
    os.makedirs(args.out, exist_ok=True)

    def emit(name, doc_report):
        doc, report = doc_report
        path = os.path.join(args.out, args.prefix + name + ".html")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(doc)
        for r in report:
            print(r)
        print("wrote %s" % path)

    if groups:
        emit("pixel_share", chart_pixel_share(groups, args.ref_s1))
        emit("frame_time", chart_frame_time(groups, field_cyc))
        made += 2
    else:
        print("没有 csv 行：像素占比与帧时间两张图不出")
        missing += 2
    if passes:
        emit("nmax_search", chart_nmax(passes))
        made += 1
    else:
        print("没有 nmax probe 行：搜索轨迹图不出")
        missing += 1
    print("figures %d missing %d" % (made, missing))
    return 0 if missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
