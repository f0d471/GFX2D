#!/usr/bin/env python3
"""证明「把注错分支从生产 RTL 里拿掉」没有改动生产逻辑。

两个方向都做，均比较预处理结果的代码 token（Verilog 用 `iverilog -E`，C 用 `gcc -E`）：

1. **正向**：迁移前（``--base-rev`` 那个版本）不定义任何 ``INJ_`` 的展开，必须与
   迁移后的当前文件 token 序列相同。
2. **反演**：把 mutants.yaml 的规则应用回当前文件，展开必须与「迁移前 +
   ``-D<原宏名>``」token 序列相同。这条同时证明规则忠实复现了原来的变异。

比对忽略注释和排版，保留运算符、标识符与字符串差异；规则应用与变异跑器共用实现。

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
sys.path.insert(0, str(ROOT / 'Main/tools/check'))
from feature_build import write_headers
from run_mutation import mutated_bytes, verilog_tokens
MANIFEST = ROOT / "Main/sim/mutation/mutants.yaml"
INCLUDE = "Main/rtl/engine/include"


def norm(text: str) -> list[str]:
    """逐行规范化：只去掉行尾 CR 和空行，其余（含缩进、行内注释）必须原样。"""
    return [ln.rstrip("\r") for ln in text.splitlines() if ln.strip()]


def tokens(text: str) -> list[str]:
    """去掉注释与空白，保留字符串和运算符；与规则匹配使用同一个 lexer。"""
    return [m.group() for m in verilog_tokens(text)]


def expand(path: Path, defs: list[str], work: Path, include=None) -> str:
    """预处理一个源文件：Verilog 用 iverilog -E，C 用 gcc -E（只用于比对，不编译）。"""
    out = work / "expand.out"
    if path.suffix == ".c":
        cmd = ["gcc", "-E", "-P", "-I", str(ROOT / "Main/sw/lib"), "-I", str(ROOT / "Main/sw/lib/core"),
               "-I", str(ROOT / "Main/sw/lib/gfx"), "-o", str(out), *[f"-D{d}" for d in defs], str(path)]
    else:
        cmd = ["iverilog", "-E", "-I", str(include or ROOT / INCLUDE), "-I", str(ROOT / INCLUDE), "-o", str(out), *[f"-D{d}" for d in defs], str(path)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"预处理失败：{' '.join(cmd)}\n{r.stderr[:400]}")
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

    work = Path(tempfile.mkdtemp(prefix="mutation-verify-", dir=ROOT/'Main/sim/build'))
    ok_all = True
    report = {}
    configs = {name:write_headers(name, work/name) for name in ('B0','B3','B5','B7')}
    for target, mutants in by_target.items():
        pre_raw = subprocess.check_output(['git','-C',str(ROOT),'show',f'{args.base_rev}:{target}'])
        pre_file=work/('pre_'+Path(target).name);pre_file.write_bytes(pre_raw.replace(b'\r\n',b'\n'))
        post_file=ROOT/target
        forward=True;expanded={}
        for config,include in configs.items():
            before=expand(pre_file,[],work,include);after=expand(post_file,[],work,include)
            same=tokens(before)==tokens(after)
            forward &= same;expanded[config]=tokens(after)
        ok_all &= forward
        detail=[]
        for row in mutants:
            text=mutated_bytes(post_file.read_bytes(),row['rules']).decode('utf-8')
            mutated=work/('mut_'+Path(target).name);mutated.write_text(text)
            active=[];same=True
            for config,include in configs.items():
                got=tokens(expand(mutated,[],work,include))
                if got!=expanded[config]:active.append(config)
                if row.get('macro'):
                    want=tokens(expand(pre_file,[row['macro']],work,include))
                    same &= got==want
            ok=bool(active) and same;ok_all &= ok
            detail.append({'id':row['id'],'ok':ok,'active_configs':active,'historical_macro':row.get('macro')})
            print(row['id'], 'PASS' if ok else 'FAIL', 'active='+','.join(active),flush=True)
        report[target]={'forward':forward,'configs':list(configs),'mutants':detail}
    import shutil
    shutil.rmtree(work)

    if args.json:
        Path(args.json).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    entries = [m for v in report.values() for m in v['mutants']]
    historical = [m for m in entries if m['historical_macro']]
    passed = sum(1 for m in historical if m['ok'])
    print(f"\n正向 {sum(1 for v in report.values() if v['forward'])}/{len(report)} 个文件；"
          f"历史反演 {passed}/{len(historical)} 条；非空激活 {sum(bool(m['active_configs']) for m in entries)}/{len(entries)} 条；"
          f"{'全部一致' if ok_all else '有不一致，先修'}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
