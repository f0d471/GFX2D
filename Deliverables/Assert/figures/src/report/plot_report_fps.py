from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.family"] = ["SimSun"]
plt.rcParams["axes.unicode_minus"] = False

BLUE, GRAY = "#4A7FD0", "#87929E"
INK = "#1D2733"

fig, ax = plt.subplots(figsize=(7.0, 3.1), dpi=400)
fig.subplots_adjust(left=0.09, right=0.97, top=0.86, bottom=0.16)

groups = ["普通游戏（模式 2）", "压力关卡（负载旋钮 1744）"]
sw = [2.5, 2.1]
hw = [60.0, 30.6]
x = [0, 1]
w = 0.32

b1 = ax.bar([i - w/2 for i in x], sw, w, color=GRAY, label="纯软件渲染（CPU 逐像素）")
b2 = ax.bar([i + w/2 for i in x], hw, w, color=BLUE, label="硬件加速渲染（本引擎）")
ax.bar_label(b1, fmt="%.1f", fontsize=10, color=INK, padding=2)
ax.bar_label(b2, fmt="%.1f", fontsize=10, color=INK, padding=2)

ax.annotate("约 24 倍", xy=(0.16, 55), fontsize=11, color=INK, ha="center",
            bbox=dict(boxstyle="round,pad=0.25", fc="#EDF3FB", ec="#4A7FD0", lw=0.8))
ax.annotate("软件存量未稳态", xy=(0.84, 36), fontsize=11, color=INK, ha="center",
            bbox=dict(boxstyle="round,pad=0.25", fc="#EDF3FB", ec="#4A7FD0", lw=0.8))
ax.axhline(60, color="#E8546B", lw=1.0, ls="--")
ax.text(1.46, 61.5, "60 FPS", fontsize=10, color="#B23A48", ha="right")

ax.set_xticks(x)
ax.set_xticklabels(groups, fontsize=11)
ax.set_ylabel("每秒呈现帧数（fps）", fontsize=11)
ax.set_ylim(0, 72)
ax.spines[["top", "right"]].set_visible(False)
ax.grid(axis="y", color="#E3E6EA", lw=0.7)
ax.set_axisbelow(True)
ax.tick_params(labelsize=9)
ax.legend(fontsize=10, frameon=False, loc="upper right", bbox_to_anchor=(1.0, 1.14), ncol=2)

fig.savefig(ROOT / "Deliverables/Assert/figures/report/rep_fps.png")
print("ok rep_fps")
