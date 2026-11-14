#!/usr/bin/env python3
"""变异测试跑器：在隔离影子树里改生产源码，跑该变异对应的用例，按退出码与见红位置分类。

生产源码（RTL 与软件执行器）不含任何注错分支。变异体来自 mutants.yaml 的一条或多条
字面替换规则，只存在于本轮运行目录的影子树里，跑完即弃。

影子树保持原来的相对布局（``Main/rtl``、``Main/sim/...``、``Main/tools/check``、``Main/sw``），
清单里的 ``runs[].cmd`` 是一条在原仓库根可执行的命令，在影子树里原样执行：
套件的相对路径照旧，Python 检查从 ``__file__`` 推出的仓根也落在影子树上。

判定：

  基线      同一条命令、同一棵树、不施加规则，要求退出码 0
  判红     施加规则后退出码非零，且输出/日志里匹配 ``runs[].evidence`` 正则；输出/日志里不能有编译错误
  超时      单独记，算弱证据

状态：KILLED / KILLED-WEAK / SURVIVED / PARTIAL / INVALID。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "Main/sim/mutation/mutants.yaml"
RUNS = ROOT / "Main/sim/build/check-runs"
IGNORE = shutil.ignore_patterns("build", "logs", ".git", "__pycache__", "*.vvp", "*.log", "*.bin", "*.bit")
RESULT_RE = re.compile(r"RESULT:\s*([A-Za-z_-]+)")
BAD_OUT = re.compile(r"COMPILE FAIL|COMPILE-ERROR|Syntax error|Segmentation fault")


@dataclass
class RunResult:
    cmd: str
    status: str = ""
    result: str = ""
    evidence: str = ""
    seconds: float = 0.0
    log: str = ""
    detail: str = ""


@dataclass
class MutantResult:
    mid: str
    target: str
    runs: list[RunResult] = field(default_factory=list)
    status: str = ""
    note: str = ""


def load_manifest(path: Path) -> list[dict]:
    rows = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(rows, list):
        sys.exit(f"{path}: 顶层必须是列表")
    seen = set()
    for row in rows:
        for key in ("id", "target", "rules", "runs"):
            if key not in row:
                sys.exit(f"{row.get('id', '?')}: 缺字段 {key}")
        if row["id"] in seen:
            sys.exit(f"重复 id: {row['id']}")
        seen.add(row["id"])
        if not row["rules"]:
            sys.exit(f"{row['id']}: rules 为空")
        for rule in row["rules"]:
            if not rule.get("find"):
                sys.exit(f"{row['id']}: 规则的 find 不能为空（replace 可以为空，表示整块删除）")
        if not row["runs"]:
            sys.exit(f"{row['id']}: runs 为空，等于没有判据")
        for i, run in enumerate(row["runs"]):
            extra = set(run) - {"cmd", "log", "evidence"}
            if "cmd" not in run or extra:
                sys.exit(f"{row['id']}: runs 的字段应为 cmd/log/evidence，实际 {sorted(run)}")
    return rows


def apply_rules(path: Path, rules: list[dict]) -> None:
    text = path.read_text(encoding="utf-8")
    for i, rule in enumerate(rules):
        n = text.count(rule["find"])
        if n != 1:
            raise SystemExit(f"{path}: 第 {i + 1} 条规则匹配 {n} 次（要求恰好 1 次）")
        text = text.replace(rule["find"], rule["replace"])
    path.write_text(text, encoding="utf-8", newline="")


def build_shadow(run_dir: Path, targets: list[str]) -> Path:
    # 影子树放在运行目录下，保持 Main/ 的相对布局；套件把绝对路径经 +case= 传给测试台，
    # 测试台的 plusarg 缓冲是定宽的，这里路径不能太长（曾因仓库路径变深而截断）。
    shadow = run_dir / "shadow"
    if shadow.exists():
        shutil.rmtree(shadow)
    print(f"影子树 {shadow.relative_to(ROOT) if shadow.is_relative_to(ROOT) else shadow}")
    src = ROOT / "Main"
    shutil.copytree(src, shadow / "Main", ignore=shutil.ignore_patterns(
        "build", "logs", ".git", "__pycache__", "*.vvp", "*.log", "*.bin", "*.bit"))
    for rel in targets:
        dst = shadow / ".pristine" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(shadow / rel, dst)
    return shadow


def run_cmd(shadow: Path, cmd: str, log_rel: str | None, timeout: float,
            dest: Path | None = None) -> RunResult:
    res = RunResult(cmd=cmd)
    t0 = time.time()
    env = dict(os.environ)
    # 用到 Axi_Mux / EFX 原语的套件要 EFINITY_HOME 取厂商原语模型，由调用者按
    # 开发环境第 11 节导出；跑器不改写它，免得把某台机器的安装路径写进仓库。
    proc = subprocess.Popen(["bash", "-lc", cmd], cwd=shadow, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, start_new_session=True, env=env)
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        res.status, res.detail, res.seconds, res.log = "超时", f"超过 {timeout:.0f}s", time.time() - t0, out[-4000:]
        return res
    res.seconds = time.time() - t0
    text = out
    if log_rel:
        p = shadow / log_rel
        files: list[Path] = []
        if p.is_dir():
            # 只看本轮这次命令跑出来的日志，避免拿上一条变异的旧日志当证据
            files = [f for f in sorted(p.rglob("*.log")) if f.stat().st_mtime >= t0 - 1.0]
        elif p.is_file() and p.stat().st_mtime >= t0 - 1.0:
            files = [p]
        for f in files:
            text += "\n" + f.read_text(encoding="utf-8", errors="replace")
    res.log = text
    if dest:
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(text[-200000:], encoding="utf-8")
    m = RESULT_RE.findall(out)
    res.result = m[-1].upper() if m else ("" if proc.returncode == 0 else "EXIT")
    if proc.returncode == 0:
        res.status = "通过"
    elif BAD_OUT.search(text):
        res.status = "编译失败"
    else:
        res.status = "见红"
    hits = [ln.strip() for ln in text.splitlines()
            if re.search(r"FAIL|MISMATCH|mismatch|BAD|ERROR|不等|不一致", ln)]
    res.detail = " | ".join(hits[:2])[:260]
    return res


def classify(runs: list[RunResult]) -> str:
    st = [r.status for r in runs]
    if any(s in ("无结果", "基线未过", "编译失败") for s in st):
        return "INVALID"
    if all(s == "见红" for s in st):
        return "KILLED" if all(r.evidence for r in runs) else "KILLED-WEAK"
    if all(s in ("见红", "超时") for s in st):
        return "KILLED-WEAK"
    if all(s == "通过" for s in st):
        return "SURVIVED"
    return "PARTIAL"


def sha(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


def audit(rows: list[dict]) -> int:
    bad = 0
    for row in rows:
        path = ROOT / row["target"]
        if not path.is_file():
            print(f"FAIL {row['id']}: 目标不存在 {row['target']}")
            bad += 1
            continue
        text = path.read_text(encoding="utf-8")
        miss = [i + 1 for i, r in enumerate(row["rules"]) if text.count(r["find"]) != 1]
        if miss:
            print(f"FAIL {row['id']}: 第 {miss} 条规则匹配次数不是 1")
            bad += 1
            continue
        doc = ROOT / row["spec"]["doc"]
        if not doc.is_file() or row["spec"]["section"] not in doc.read_text(encoding="utf-8"):
            print(f"FAIL {row['id']}: 规格出处找不到 {row['spec']['doc']} #{row['spec']['section']}")
            bad += 1
            continue
        if not any(r.get("evidence") for r in row["runs"]):
            print(f"WARN {row['id']}: 没有登记见红位置，只能算 KILLED-WEAK")
    print(f"audit: {len(rows) - bad}/{len(rows)} 条可用")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--audit", action="store_true")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--ids", help="逗号分隔的 id；默认全量")
    ap.add_argument("--run-dir")
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--keep-shadow", action="store_true", help="跑完保留影子树，便于排查")
    ap.add_argument("--autofill-evidence", action="store_true",
                    help="把见红行里的检查器标签自动写回清单（仍需人工过一遍）")
    args = ap.parse_args()

    rows = load_manifest(MANIFEST)
    if args.ids:
        want = [x.strip() for x in args.ids.split(",") if x.strip()]
        missing = [x for x in want if x not in {r["id"] for r in rows}]
        if missing:
            sys.exit(f"未知 id: {', '.join(missing)}")
        rows = [r for r in rows if r["id"] in want]

    if args.list:
        for row in rows:
            print(f"{row['id']:<28} {row['defect']:<10} {len(row['rules'])} 处  {row['runs'][0]['cmd']}")
        print(f"共 {len(rows)} 条")
        return 0
    if args.audit:
        return audit(rows)

    run_dir = Path(args.run_dir) if args.run_dir else RUNS / time.strftime("mutation-%Y%m%d-%H%M%S")
    run_dir = run_dir if run_dir.is_absolute() else ROOT / run_dir
    run_dir.mkdir(parents=True, exist_ok=True)

    targets = sorted({row["target"] for row in rows})
    shadow = build_shadow(run_dir, targets)
    print(f"运行目录 {run_dir.relative_to(ROOT)}；{len(targets)} 个目标文件")

    baseline: dict[str, RunResult] = {}
    for cmd in dict.fromkeys(r["cmd"] for row in rows for r in row["runs"]):
        b = run_cmd(shadow, cmd, None, args.timeout)
        baseline[cmd] = b
        print(f"  基线 {cmd[:96]:<96} {'PASS' if b.status == '通过' else 'FAIL'}  {b.seconds:.1f}s")

    results: list[MutantResult] = []
    for row in rows:
        mid = row["id"]
        mr = MutantResult(mid=mid, target=row["target"])
        target_path = shadow / row["target"]
        shutil.copy2(shadow / ".pristine" / row["target"], target_path)
        try:
            apply_rules(target_path, row["rules"])
        except SystemExit as exc:
            mr.status, mr.note = "INVALID", str(exc)
            results.append(mr)
            print(f"  {mid:<28} INVALID {exc}")
            continue
        for i, run in enumerate(row["runs"]):
            b = baseline[run["cmd"]]
            rr = run_cmd(shadow, run["cmd"], run.get("log"), args.timeout,
                         dest=run_dir / "logs" / mid / (str(i) + ".log"))
            if b.status != "通过":
                rr.status, rr.detail = "基线未过", f"基线 {b.result or b.status}"
            elif rr.status == "见红":
                pat = run.get("evidence") or ""
                hit = re.search(pat, rr.log) if pat else None
                rr.evidence = hit.group(0)[:80] if hit else ""
                if pat and not hit:
                    rr.detail = f"见红位置不匹配「{pat}」"
            mr.runs.append(rr)
            print(f"  {mid:<28} {rr.status:<5} {rr.result:<6} {rr.seconds:6.1f}s" + (f"  「{rr.evidence}」" if rr.evidence else ""))
        mr.status = "INVALID" if any(r.status == "基线未过" for r in mr.runs) else classify(mr.runs)
        results.append(mr)

    for row in rows:  # 恢复影子树
        shutil.copy2(shadow / ".pristine" / row["target"], shadow / row["target"])

    if args.autofill_evidence:
        import yaml as _yaml
        manifest = _yaml.safe_load(MANIFEST.read_text(encoding="utf-8"))
        by_id = {row["id"]: row for row in manifest}
        filled = []
        for m in results:
            for r in m.runs:
                if r.status != "见红" or r.evidence:
                    continue
                line = next((ln for ln in r.log.splitlines()
                             if re.search(r"FAIL|MISMATCH|ERROR|violat|不等|不一致", ln)), "")
                tok = re.search(r"\b([A-Z][A-Z0-9_]{3,})\b", line.replace("RESULT", "").replace("FAIL", ""))
                if not tok:
                    continue
                pat = re.escape(tok.group(1))
                for run in by_id[m.mid]["runs"]:
                    if not run.get("evidence"):
                        run["evidence"] = pat
                filled.append((m.mid, pat, line.strip()[:100]))
        MANIFEST.write_text(
            "# 变异清单（唯一真相）。每条记录对应一处生产源码的故意改错。\n"
            "# 字段：defect 缺陷模型；spec 规格出处；rules 字面替换（每条必须唯一匹配，同一宏可多处）；\n"
            "# runs 应发现它的命令与见红位置正则；macro 只用于追溯原来的 ifdef 开关。\n"
            "# 校验与执行：run_mutation.py；迁移等价性：verify_migration.py。\n"
            + _yaml.safe_dump(manifest, allow_unicode=True, sort_keys=False,
                              default_flow_style=False, width=10000),
            encoding="utf-8", newline="")
        for mid, pat, line in filled:
            print(f"  evidence {mid:<28} {pat:<24} {line}")
        print(f"自动登记见红位置 {len(filled)} 条（需人工复核）")

    counts: dict[str, int] = {}
    for m in results:
        counts[m.status] = counts.get(m.status, 0) + 1
    report = {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git": subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip(),
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "counts": counts,
        "baseline": {c: {"status": b.status, "seconds": round(b.seconds, 1)} for c, b in baseline.items()},
        "mutants": [{
            "id": m.mid, "target": m.target, "status": m.status, "note": m.note,
            "runs": [{"cmd": r.cmd, "status": r.status, "result": r.result,
                      "evidence": r.evidence, "detail": r.detail, "seconds": round(r.seconds, 1)} for r in m.runs],
        } for m in results],
    }
    (run_dir / "mutation-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                                  encoding="utf-8")
    defects = {row["id"]: row["defect"] for row in rows}
    lines = ["| 变异 | 缺陷模型 | 用例 | 结果 | 见红位置 | 状态 |", "| --- | --- | --- | --- | --- | --- |"]
    for m in results:
        for i, r in enumerate(m.runs):
            lines.append(f"| {m.mid if i == 0 else ''} | {defects[m.mid] if i == 0 else ''} | `{r.cmd.split()[-1]}` "
                         f"| {r.result or r.status} | {r.evidence or '—'} | {m.status if i == 0 else ''} |")
    (run_dir / "mutation-report.md").write_text(
        f"# 变异测试报告\n\n运行目录：`{run_dir.relative_to(ROOT)}`\n\n"
        f"基线：{sum(1 for b in baseline.values() if b.status == '通过')}/{len(baseline)} 通过\n\n"
        + "\n".join(lines) + "\n\n合计 {len(results)} 条：" + "、".join(f"{k} {v}" for k, v in sorted(counts.items())) + "\n",
        encoding="utf-8")
    print(f"\n合计 {len(results)} 条：" + "、".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"报告 {run_dir.relative_to(ROOT)}/mutation-report.md")
    if not args.keep_shadow:
        shutil.rmtree(shadow, ignore_errors=True)
    return 1 if counts.get("INVALID") else 0


if __name__ == "__main__":
    raise SystemExit(main())
