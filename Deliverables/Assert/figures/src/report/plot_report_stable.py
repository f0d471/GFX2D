from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
import csv
import matplotlib
matplotlib.use("Agg")
matplotlib.rcParams["font.family"] = ["Microsoft YaHei"]
matplotlib.rcParams["axes.unicode_minus"] = False
import matplotlib.pyplot as plt

BLUE, INK, GRAY = "#4A7FD0", "#1D2733", "#87929E"
LOG = ROOT / "Main/sw/apps/game/gfx2dGame/logs/sw9_perf_hw_k896_60s.log"

hdr, rows = None, []
with open(LOG, newline="", encoding="utf-8", errors="replace") as f:
    for row in csv.reader(f):
        if not row:
            continue
        if row[0] == "csvh":
            hdr = row
        elif row[0] == "csv":
            rows.append(row)
ft = [int(r[hdr.index("ft_max")]) / 1e6 for r in rows]
nact = [int(r[hdr.index("n_active")]) for r in rows]
BUDGET = 1_666_667 / 1e6

fig, ax = plt.subplots(figsize=(7.2, 3.2), dpi=200)
fig.subplots_adjust(left=0.09, right=0.90, top=0.86, bottom=0.17)
ax.plot(range(len(ft)), ft, "-", color=BLUE, lw=1.4, marker="o", ms=2.5,
        label="该秒最长帧时间（左轴）")
ax.axhline(BUDGET, color="#E8546B", lw=1.1, ls="--")
ax.text(59, BUDGET + 0.012, "16.67 ms 帧预算", fontsize=10, color="#B23A48", ha="right")
ax.set_xlabel("上电后经过时间（秒）", fontsize=11)
ax.set_ylabel("该秒最长帧时间（ms）", fontsize=11)
ax.set_ylim(0.6, 1.78)
ax.spines[["top"]].set_visible(False)
ax.grid(axis="y", color="#E3E6EA", lw=0.7)
ax.set_axisbelow(True)
ax.tick_params(labelsize=9)

ax2 = ax.twinx()
ax2.plot(range(len(nact)), nact, "-", color=GRAY, lw=1.2, label="活动弹数（右轴）")
ax2.set_ylabel("活动弹数（颗）", fontsize=11, color="#4A5568")
ax2.set_ylim(0, 1300)
ax2.tick_params(labelsize=9, colors="#4A5568")
ax2.spines[["top"]].set_visible(False)

h1, l1 = ax.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax.legend(h1 + h2, l1 + l2, fontsize=10, frameon=False, loc="lower right")
ax.set_title("固定负载 K896 连续 60 秒逐秒实测：全部窗口低于帧预算，仅首秒 59/60",
             fontsize=11.5, color=INK, pad=8)

fig.savefig(ROOT / "Deliverables/Assert/figures/report/rep_stable.png")
print("ok rep_stable")
