#!/usr/bin/env python3
"""把 RTL 临时还原成「注错机制替换之前」的形态，用于干净构建对账。

用途：注错机制从 `parameter integer INJECT` 换成 `ifdef INJ_xxx` 之后，要用一次
综合证明「替换前后综合出的东西一样」——否则只能靠代码审查说它一样。
做法是编译两次：第一次用工作区现在的 RTL，第二次用本脚本还原出来的原版 RTL，
比对两次的 XLR / Memory Blocks / DSP Blocks 与 setup、hold slack。

原版从 git 历史里取（`--rev`，默认取替换之前的那次提交），不保留第二份源码副本。

用法（在 Main/ 下）：

    python3 tools/check/inject_baseline.py --restore   还原成原版
    efx_run.bat gfx2d_ti60.xml                         编译第二次
    python3 tools/check/inject_baseline.py --apply     恢复工作区版本

`--restore` 会先把工作区版本备份到 `--backup` 指定的目录（默认系统临时目录），
`--apply` 从那里恢复。两个动作都会打印受影响文件清单与每个文件的 SHA256，
对账记录里要连这份清单一起留。
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

REPO_REL = os.path.join("Main", "rtl")

# 注错机制替换涉及的自写 RTL 文件（相对 Main/rtl/）
FILES = [
    "engine/core/engine_blend.v",
    "engine/core/engine_cmd.v",
    "engine/core/engine_geom.v",
    "engine/core/engine_mover.v",
    "engine/core/engine_pix.v",
    "engine/buffer/engine_tex.v",
    "engine/engine_top.v",
    "display/display_fetch.v",
    "display/display_out.v",
    "display/display_top.v",
    # 仿真侧同步改的测试台与脚本
]

SIM_FILES = [
    "sim/engine/blend/run.sh",
    "sim/engine/blend/tb_engine_blend.v",
    "sim/engine/ctrl/run.sh",
    "sim/engine/ctrl/tb_engine_top.v",
    "sim/engine/ctrl/syn/syn_engine_top.v",
    "sim/engine/geom/run.sh",
    "sim/engine/geom/tb_engine_geom.v",
    "sim/engine/mover/run.sh",
    "sim/engine/mover/tb_engine_mover.v",
    "sim/engine/pix/run.sh",
    "sim/engine/pix/run_formal.sh",
    "sim/engine/pix/tb_engine_path.v",
    "sim/engine/pix/tb_engine_pix.v",
    "sim/engine/pix/tb_engine_pix_pipe.v",
    "sim/engine/tex/run.sh",
    "sim/engine/tex/tb_engine_tex.v",
    "sim/display/run.sh",
    "sim/display/tb_display.v",
]


def repo_main() -> str:
    """定位 Main/：从当前目录往上找含 rtl/engine/core/engine_blend.v 的那一层。"""
    here = os.path.abspath(os.getcwd())
    for cand in [here] + [os.path.join(here, *[".."] * i) for i in range(1, 5)]:
        cand = os.path.normpath(cand)
        if os.path.isfile(os.path.join(cand, "rtl", "engine", "core", "engine_blend.v")):
            return cand
    sys.exit("找不到 Main/：请在 Main/ 目录下运行")


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def targets(main: str):
    for rel in FILES:
        yield rel, os.path.join(main, "rtl", rel)
    for rel in SIM_FILES:
        yield rel, os.path.join(main, rel)


def git_show(main: str, rev: str, repo_rel: str) -> bytes:
    r = subprocess.run(
        ["git", "show", f"{rev}:{repo_rel}"], cwd=main,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if r.returncode != 0:
        raise RuntimeError(
            f"git show {rev}:{repo_rel} 失败：{r.stderr.decode('utf-8', 'replace').strip()}")
    return r.stdout


def do_restore(main: str, rev: str, backup: str) -> None:
    os.makedirs(backup, exist_ok=True)
    changed = 0
    for rel, path in targets(main):
        if not os.path.isfile(path):
            print(f"  跳过（不存在）：{rel}")
            continue
        shutil.copy2(path, os.path.join(backup, rel.replace("/", "__")))
        repo_rel = os.path.relpath(path, os.path.dirname(main)).replace(os.sep, "/")
        try:
            original = git_show(main, rev, repo_rel)
        except RuntimeError as e:
            print(f"  跳过（历史里没有）：{rel}  {e}")
            continue
        with open(path, "wb") as f:
            f.write(original)
        changed += 1
        print(f"  还原 {rel}  sha256={sha256(path)}")
    print(f"共还原 {changed} 个文件；工作区版本已备份到 {backup}")
    print("编译之后务必执行 --apply 恢复，否则会把原版提交进去。")


def do_apply(main: str, backup: str) -> None:
    if not os.path.isdir(backup):
        sys.exit(f"备份目录不存在：{backup}")
    restored = 0
    for rel, path in targets(main):
        src = os.path.join(backup, rel.replace("/", "__"))
        if not os.path.isfile(src):
            continue
        shutil.copy2(src, path)
        restored += 1
        print(f"  恢复 {rel}  sha256={sha256(path)}")
    print(f"共恢复 {restored} 个文件")


def main() -> None:
    ap = argparse.ArgumentParser(description="RTL 还原成注错机制替换之前的版本（干净构建对账用）")
    ap.add_argument("--rev", default="c156fe5",
                    help="取原版的提交（默认 c156fe5，G1 验收归档那次）")
    ap.add_argument("--backup", default=os.path.join(tempfile.gettempdir(), "efinix-inject-baseline"),
                    help="工作区版本的备份目录")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--restore", action="store_true", help="还原成原版")
    g.add_argument("--apply", action="store_true", help="恢复工作区版本")
    a = ap.parse_args()

    main_dir = repo_main()
    if a.restore:
        do_restore(main_dir, a.rev, a.backup)
    else:
        do_apply(main_dir, a.backup)


if __name__ == "__main__":
    main()
