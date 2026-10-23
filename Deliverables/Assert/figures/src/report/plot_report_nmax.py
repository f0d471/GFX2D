from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.family"] = ["SimSun"]
plt.rcParams["axes.unicode_minus"] = False

LOG = ROOT / "Main/sw/apps/game/gfx2dGame/logs/sw9_perf_hw_nmax.log"
BLUE, RED, GOLD, GRAY, INK = "#4A7FD0", "#E8546B", "#A37A1C", "#87929E", "#1D2733"

pat = re.compile(
    r"nmax (load|probe|result) (.*)")
sections = []
cur = None
for line in open(LOG, encoding="utf-8", errors="replace"):
    m = pat.match(line.strip())
    if not m:
        continue
    kind, rest = m.group(1), m.group(2)
    if kind == "load":
        cur = {"name": rest.split()[0], "probes": [], "result": None}
        sections.append(cur)
    elif kind == "probe" and cur is not None:
        g = re.search(r"knob (\d+) .* (pass|fail)$", rest)
        a = re.search(r"act_min (\d+)", rest)
        if g:
            cur["probes"].append((int(g.group(1)), int(a.group(1)), g.group(2)))
    elif kind == "result" and cur is not None:
        g = re.search(r"n_max (\d+) knob (\d+)", rest)
        if g:
            cur["result"] = (int(g.group(1)), int(g.group(2)))

fig, axes = plt.subplots(1, 2, figsize=(7.6, 3.2), dpi=400, sharey=False)
fig.subplots_adjust(left=0.08, right=0.98, top=0.82, bottom=0.18, wspace=0.24)

for ax, sec, label in zip(axes, sections, ["16×16 标准弹", "8×8 小弹"]):
    for knob, act, verdict in sec["probes"]:
        if verdict == "pass":
            ax.plot(knob, act, "o", color=BLUE, ms=6, zorder=3)
        else:
            ax.plot(knob, act, "x", color=RED, ms=8, mew=2, zorder=3)
    if sec["result"]:
        nmax, knob = sec["result"]
        ax.axhspan(0, nmax, color="#EEF7E9", zorder=0)
        ax.axhline(nmax, color=GOLD, lw=1.0, ls="--", zorder=2)
        xytext = (420, 1150) if "16" in label else (700, 1750)
        ax.annotate("$N_{max}$ = %d" % nmax, xy=(knob, nmax), xytext=xytext,
                    fontsize=11, color=INK,
                    arrowprops=dict(arrowstyle="-", color=GOLD, lw=0.8))
    ax.set_title(label + f"（结果旋钮 {sec['result'][1]}）", fontsize=11.5)
    ax.set_xlabel("负载旋钮 K（每 125 帧发射弹数）", fontsize=10)
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="#E3E6EA", lw=0.7)
    ax.set_axisbelow(True)
    ax.tick_params(labelsize=8)

axes[0].set_ylabel("实测活动弹数（颗）", fontsize=10)
from matplotlib.lines import Line2D
handles = [Line2D([], [], marker="o", ls="", color=BLUE, label="该档通过（120 场无掉帧）"),
           Line2D([], [], marker="x", ls="", color=RED, label="该档掉帧"),
           Line2D([], [], color=GOLD, ls="--", label="确认档（3600 场无掉帧）")]
axes[0].legend(handles=handles, fontsize=9.5, frameon=False, loc="lower right")

fig.savefig(ROOT / "Deliverables/Assert/figures/report/rep_nmax.png")
print("ok rep_nmax")
