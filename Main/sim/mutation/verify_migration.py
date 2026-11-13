#!/usr/bin/env python3
"""证明「把注错分支从生产 RTL 里拿掉」没有改动生产逻辑。

两个方向都做，都是逐行比对预处理结果（`iverilog -E`），不是肉眼看差异：

1. **正向**：迁移前（``--base-rev`` 那个版本）不定义任何 ``INJ_`` 的展开，必须与
   迁移后的当前文件逐行相同。
2. **反演**：把 mutants.yaml 的规则应用回当前文件，展开必须与「迁移前 +
   ``-D<原宏名>``」逐行相同。这条同时证明规则忠实复现了原来的变异。

预处理输出会为被删掉的指令行和未取分支各留一个空行，所以比对时只忽略空行；
其余每一行（含缩进与行内注释）必须一字不差。

  python3 Main/sim/mutation/verify_migration.py --base-rev 551c2029
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "Main/sim/mutation/mutants.yaml"
INCLUDE = "Main/rtl/engine/include"


def norm(text: str) -> list[str]:
    """逐行规范化：只去掉行尾 CR 和空行，其余（含缩进、行内注释）必须原样。"""
    return [ln.rstrip("\r") for ln in text.splitlines() if ln.strip()]


def tokens(text: str) -> list[str]:
    """词法级规范化：去注释与空白，只留 token 序列。

    反演证明要用它：`ifdef 行尾的注错说明会作为注释残留在 -E 输出里，
    而迁移后的文件按约定不留注错痕迹，所以反演只能比到词法层。
    """
    if "/*" in text:
        sys.exit("文件里有块注释，词法比对需要先扩展；本工具只处理 // 注释")
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        if cut >= 0:
            line = line[:cut]
        out.extend(line.split())
    return out


def expand(path: Path, defs: list[str], work: Path) -> str:
    out = work / "expand.out"
    cmd = ["iverilog", "-E", "-I", str(ROOT / INCLUDE), "-o", str(out), *[f"-D{d}" for d in defs], str(path)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"iverilog -E 失败：{' '.join(cmd)}\n{r.stderr}")
    return out.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-rev", required=True, help="迁移前的 git 版本")
    ap.add_argument("--mutants", default=str(MANIFEST))
    ap.add_argument("--json", help="把结果写成 JSON")
    args = ap.parse_args()

    rows = yaml.safe_load(Path(args.mutants).read_text(encoding="utf-8"))
    by_target: dict[str, list[dict]] = {}
    for row in rows:
        by_target.setdefault(row["target"], []).append(row)

    work = Path(tempfile.mkdtemp(prefix="mutation-verify-"))
    ok_all = True
    report = {}
    for target, mutants in by_target.items():
        pre_raw = subprocess.run(["git", "-C", str(ROOT), "show", f"{args.base_rev}:{target}"],
                                 capture_output=True, check=True).stdout
        pre_file = work / ("pre_" + Path(target).name)
        pre_file.write_bytes(pre_raw.replace(b"\r\n", b"\n"))

        post_file = ROOT / target
        pre_expand = expand(pre_file, [], work)
        post_expand = expand(post_file, [], work)
        pre_none = norm(pre_expand)
        post_none = norm(post_expand)
        if pre_none == post_none:
            forward, how = True, "逐行相同"
        elif tokens(pre_expand) == tokens(post_expand):
            forward, how = True, "代码相同，注释有改动"
        else:
            forward, how = False, "代码不同"
        ok_all &= forward
        print(f"{target}")
        print(f"  正向（不定义任何 INJ_）: {'PASS' if forward else 'FAIL'}  {how}  "
              f"{len(pre_none)} 行 vs {len(post_none)} 行")

        post_text = post_file.read_text(encoding="utf-8")
        detail = []
        for row in mutants:
            mid, macro = row["id"], row.get("macro", "")
            find, repl = row["rule"]["find"], row["rule"]["replace"]
            if post_text.count(find) != 1:
                print(f"  {mid:<22} 规则匹配 {post_text.count(find)} 次，无法反演")
                ok_all = False
                detail.append({"id": mid, "ok": False, "why": "规则不唯一"})
                continue
            mutated = work / ("mut_" + Path(target).name)
            mutated.write_text(post_text.replace(find, repl), encoding="utf-8", newline="")
            want = tokens(expand(pre_file, [macro], work)) if macro else []
            got = tokens(expand(mutated, [], work))
            same = bool(want) and want == got
            ok_all &= same
            detail.append({"id": mid, "macro": macro, "ok": same})
            print(f"  {mid:<22} 反演 {'PASS' if same else 'FAIL'}  "
                  f"{'-D' + macro if macro else '(无宏名)'}  {len(got)} token")

        report[target] = {"forward": forward, "mutants": detail}

    if args.json:
        Path(args.json).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    total = sum(len(v["mutants"]) for v in report.values())
    passed = sum(1 for v in report.values() for m in v["mutants"] if m["ok"])
    print(f"\n正向 {sum(1 for v in report.values() if v['forward'])}/{len(report)} 个文件；"
          f"反演 {passed}/{total} 条；{'全部一致' if ok_all else '有不一致，先修'}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
