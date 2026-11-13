#!/usr/bin/env python3
"""变异测试跑器：在隔离影子树里改一处生产 RTL，用原有测试台跑，按见红位置分类。

生产 RTL 不含任何注错分支。变异体来自 mutants.yaml 的一条字面替换规则，
只存在于本轮运行目录的影子树里，跑完即弃。

影子树保持原来的相对布局（``Main/rtl``、``Main/sim/<套件>``、``Main/tools``），
所以各套件 `run.sh` 里的 ``../../../rtl`` 与 ``cd $(dirname $0)`` 照旧成立，
套件只需支持 ``--case <名字>`` 跑单例。

六个状态（见 Main/doc/开发环境.md 0.20 节）：

  KILLED       变异体 FAIL，且见红位置匹配登记的 evidence 正则
  KILLED-WEAK  FAIL 了，但红在别处，或只是超时——不算判据有效
  SURVIVED     变异体 PASS，判据没抓到
  PARTIAL      部分用例抓到、部分没抓到（登记了多个用例时）
  INVALID      规则不唯一 / 编译失败 / 同树基线不 PASS
  EQUIVALENT   有 proof 字段指向已跑过的等价论证；不计入分母

用法：

  python3 Main/sim/mutation/run_mutation.py --list
  python3 Main/sim/mutation/run_mutation.py --audit
  python3 Main/sim/mutation/run_mutation.py --run --ids blend.wrong_eq,pix.rot_off
  python3 Main/sim/mutation/run_mutation.py --run          # 全量，写进新的运行目录
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
import time
from dataclasses import dataclass, field
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "Main/sim/mutation/mutants.yaml"
RUNS = ROOT / "Main/sim/build/check-runs"
# 影子树要带的目录：相对 Main/。套件里的 ../../.. 相对路径靠这套布局成立。
COPY_DIRS = ("rtl", "tools/check", "sw/lib", "sim/axi_probe")
IGNORE = shutil.ignore_patterns("build", "logs", ".git", "__pycache__", "*.vvp", "*.log")

RESULT_RE = re.compile(r"RESULT:\s*([A-Za-z_-]+)")


@dataclass
class RunResult:
    case: str
    status: str = ""
    result: str = ""
    evidence: str = ""
    seconds: float = 0.0
    log: str = ""
    log_path: str = ""
    detail: str = ""


@dataclass
class MutantResult:
    mid: str
    target: str
    runs: list[RunResult] = field(default_factory=list)
    status: str = ""
    sha_before: str = ""
    sha_after: str = ""
    note: str = ""


def load_manifest(path: Path) -> list[dict]:
    rows = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(rows, list):
        sys.exit(f"{path}: 顶层必须是列表")
    seen = set()
    for row in rows:
        for key in ("id", "target", "defect", "spec", "rule", "runs", "expect"):
            if key not in row:
                sys.exit(f"{row.get('id', '?')}: 缺字段 {key}")
        if row["id"] in seen:
            sys.exit(f"重复 id: {row['id']}")
        seen.add(row["id"])
        if not row["rule"].get("find") or not row["rule"].get("replace"):
            sys.exit(f"{row['id']}: rule.find / rule.replace 不能为空")
        if not row["runs"]:
            sys.exit(f"{row['id']}: runs 为空，等于没有判据")
        for run in row["runs"]:
            extra = set(run) - {"suite", "case", "evidence"}
            if not {"suite", "case"} <= set(run) or extra:
                sys.exit(f"{row['id']}: runs 的字段应为 suite/case[/evidence]，实际 {sorted(run)}")
    return rows


def apply_rule(path: Path, find: str, replace: str) -> None:
    text = path.read_text(encoding="utf-8")
    n = text.count(find)
    if n != 1:
        raise SystemExit(f"{path}: 规则匹配 {n} 次（要求恰好 1 次），先跑 --audit 修规则")
    path.write_text(text.replace(find, replace), encoding="utf-8", newline="")


def build_shadow(run_dir: Path, suites: set[str], targets: list[str]) -> Path:
    shadow = run_dir / "shadow"
    main = shadow / "Main"
    if shadow.exists():
        shutil.rmtree(shadow)
    main.mkdir(parents=True)
    for rel in COPY_DIRS:
        shutil.copytree(ROOT / "Main" / rel, main / rel, ignore=IGNORE)
    for suite in sorted(suites):
        src = ROOT / "Main/sim" / suite
        if not src.is_dir():
            sys.exit(f"套件目录不存在: Main/sim/{suite}")
        shutil.copytree(src, main / "sim" / suite, ignore=IGNORE)
    pristine = shadow / ".pristine"
    for rel in targets:
        dst = pristine / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, dst)
    return shadow


def run_one(suite_dir: Path, case: str, timeout: float, log_rel: str, dest: Path | None = None) -> RunResult:
    res = RunResult(case=case)
    t0 = time.time()
    proc = subprocess.Popen(["bash", "run.sh", "--case", case], cwd=suite_dir,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        res.status = "超时"
        res.detail = f"超过 {timeout:.0f}s"
        res.seconds = time.time() - t0
        res.log = out[-4000:]
        if dest:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(out, encoding="utf-8")
        return res
    res.seconds = time.time() - t0
    m = RESULT_RE.findall(out)
    res.result = m[-1].upper() if m else ""
    log_path = suite_dir / log_rel.format(case=case)
    if log_path.is_file():
        res.log = log_path.read_text(encoding="utf-8", errors="replace")
        res.log_path = str(log_path)
        if dest:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(log_path, dest)
    else:
        res.log = out
        if dest:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(out, encoding="utf-8")
    # 判红的那几行摘要，便于填 evidence 正则
    hits = [ln.strip() for ln in res.log.splitlines()
            if re.search(r"FAIL|MISMATCH|mismatch|BAD|ERROR|不一致", ln)]
    res.detail = " | ".join(hits[:3])[:300]
    if not res.result:
        res.status = "无结果行"
        res.detail = out.strip().splitlines()[-1][:160] if out.strip() else "无输出"
        return res
    if res.result == "PASS":
        res.status = "通过"
    elif res.result == "FAIL":
        res.status = "见红"
    elif res.result == "NOT_RED":
        res.status = "未红"
    elif res.result.startswith("COMPILE"):
        res.status = "编译失败"
    else:
        res.status = res.result
    return res


def classify(runs: list[RunResult]) -> str:
    statuses = [r.status for r in runs]
    if any(s in ("无结果行", "基线未过", "编译失败") for s in statuses):
        return "INVALID"
    if all(s == "见红" for s in statuses):
        return "KILLED" if all(r.evidence for r in runs) else "KILLED-WEAK"
    if all(s in ("见红", "超时") for s in statuses):
        return "KILLED-WEAK"  # 靠超时见红不算判据有效
    if all(s in ("未红", "通过") for s in statuses):
        return "SURVIVED"
    return "PARTIAL"


def sha256(path: Path) -> str:
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
        n = text.count(row["rule"]["find"])
        if n != 1:
            print(f"FAIL {row['id']}: 规则匹配 {n} 次")
            bad += 1
            continue
        doc = ROOT / row["spec"]["doc"]
        if not doc.is_file():
            print(f"FAIL {row['id']}: 规格文件不存在 {row['spec']['doc']}")
            bad += 1
            continue
        if row["spec"]["section"] not in doc.read_text(encoding="utf-8"):
            print(f"FAIL {row['id']}: 规格出处找不到「{row['spec']['section']}」")
            bad += 1
            continue
        if row["expect"].get("evidence"):
            print(f"WARN {row['id']}: 见红位置写在了行级 expect 上，建议改到该用例的 runs[].evidence")
        if not any(r.get("evidence") for r in row["runs"]):
            print(f"WARN {row['id']}: 没有登记见红位置，只能算 KILLED-WEAK")
        else:
            for run in row["runs"]:
                if not run.get("evidence"):
                    print(f"WARN {row['id']}: 用例 {run['case']} 没有见红位置，该用例只能算弱证据")
    print(f"audit: {len(rows) - bad}/{len(rows)} 条可用")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="列出清单")
    ap.add_argument("--audit", action="store_true", help="只校验规则与规格出处，不跑仿真")
    ap.add_argument("--run", action="store_true", help="跑本轮变异")
    ap.add_argument("--ids", help="逗号分隔的 id；默认全量")
    ap.add_argument("--run-dir", help="运行目录，默认新建 sim/build/check-runs/mutation-<时间>")
    ap.add_argument("--timeout", type=float, default=600.0, help="单个用例超时秒数")
    args = ap.parse_args()

    rows = load_manifest(MANIFEST)
    if args.ids:
        want = [x.strip() for x in args.ids.split(",") if x.strip()]
        known = {r["id"] for r in rows}
        missing = [x for x in want if x not in known]
        if missing:
            sys.exit(f"未知 id: {', '.join(missing)}")
        rows = [r for r in rows if r["id"] in want]

    if args.list:
        for row in rows:
            cases = ",".join(f"{r['suite']}:{r['case']}" for r in row["runs"])
            print(f"{row['id']:<22} {row['defect']:<12} {cases}")
        print(f"共 {len(rows)} 条")
        return 0
    if args.audit:
        return audit(rows)

    run_dir = Path(args.run_dir) if args.run_dir else RUNS / time.strftime("mutation-%Y%m%d-%H%M%S")
    run_dir = run_dir if run_dir.is_absolute() else ROOT / run_dir
    run_dir.mkdir(parents=True, exist_ok=True)

    suites = {r["suite"] for row in rows for r in row["runs"]}
    targets = sorted({row["target"] for row in rows})
    shadow = build_shadow(run_dir, suites, targets)
    main_shadow = shadow / "Main"

    # 规则登记时用的日志相对路径：套件的 run.sh 都是 build/<用例>.log
    log_rel = "build/{case}.log"

    print(f"运行目录 {run_dir.relative_to(ROOT)}")
    print(f"影子树 {len(targets)} 个目标、{len(suites)} 个套件")

    # ---- 基线：同一棵影子树，不应用任何规则 ----
    cases = [(r["suite"], r["case"]) for row in rows for r in row["runs"]]
    baseline: dict[tuple[str, str], RunResult] = {}
    for suite, case in dict.fromkeys(cases):
        baseline[(suite, case)] = run_one(main_shadow / "sim" / suite, case, args.timeout, log_rel,
                                          dest=run_dir / "baseline" / f"{case}.log")
        b = baseline[(suite, case)]
        flag = "PASS" if b.status == "通过" else "FAIL"
        print(f"  基线 {suite}:{case:<18} {flag}  {b.seconds:.1f}s")

    results: list[MutantResult] = []
    for row in rows:
        mid = row["id"]
        mr = MutantResult(mid=mid, target=row["target"])
        target_path = shadow / row["target"]
        pristine = shadow / ".pristine" / row["target"]
        shutil.copy2(pristine, target_path)
        mr.sha_before = sha256(target_path)
        try:
            apply_rule(target_path, row["rule"]["find"], row["rule"]["replace"])
        except SystemExit as exc:
            mr.status = "INVALID"
            mr.note = str(exc)
            results.append(mr)
            print(f"  {mid:<22} INVALID  {exc}")
            continue
        mr.sha_after = sha256(target_path)

        for run in row["runs"]:
            b = baseline[(run["suite"], run["case"])]
            rr = run_one(main_shadow / "sim" / run["suite"], run["case"], args.timeout, log_rel,
                         dest=run_dir / "runs" / mid / f"{run['case']}.log")
            if b.status != "通过":
                rr.status = "基线未过"
                rr.detail = f"同树基线 {b.result or b.status}"
            elif rr.status == "见红":
                pat = run.get("evidence") or row["expect"].get("evidence") or ""
                hit = re.search(pat, rr.log) if pat else None
                rr.evidence = hit.group(0)[:80] if hit else ""
                if pat and not hit:
                    rr.detail = f"见红位置不匹配「{pat}」"
            mr.runs.append(rr)
            print(f"  {mid:<22} {run['case']:<20} {rr.status:<6} {rr.result:<8} {rr.seconds:6.1f}s"
                  + (f"  「{rr.evidence}」" if rr.evidence else ""))
            if rr.status == "见红" and rr.detail:
                print(f"       {rr.detail}")

        if any(r.status == "基线未过" for r in mr.runs):
            mr.status = "INVALID"
        else:
            mr.status = classify(mr.runs)
        results.append(mr)

    # 恢复影子树，避免留一个改过的 RTL 被后续 checkout 误用
    for row in rows:
        shutil.copy2(shadow / ".pristine" / row["target"], shadow / row["target"])

    report = {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git": subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip(),
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "baseline": {f"{s}:{c}": {"status": b.status, "result": b.result, "seconds": round(b.seconds, 1)}
                     for (s, c), b in baseline.items()},
        "mutants": [
            {
                "id": m.mid, "target": m.target, "status": m.status, "note": m.note,
                "sha_before": m.sha_before, "sha_after": m.sha_after,
                "runs": [
                    {"case": r.case, "status": r.status, "result": r.result,
                     "evidence": r.evidence, "detail": r.detail,
                     "seconds": round(r.seconds, 1)}
                    for r in m.runs
                ],
            }
            for m in results
        ],
    }
    (run_dir / "mutation-report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    counts: dict[str, int] = {}
    for m in results:
        counts[m.status] = counts.get(m.status, 0) + 1
    lines = ["| 变异 | 缺陷模型 | 用例 | 结果 | 见红位置 | 状态 |",
             "| --- | --- | --- | --- | --- | --- |"]
    defects = {row["id"]: row["defect"] for row in rows}
    for m in results:
        for i, r in enumerate(m.runs):
            lines.append(f"| {m.mid if i == 0 else ''} | {defects[m.mid] if i == 0 else ''} "
                         f"| {r.case} | {r.result or r.status} | {r.evidence or '—'} | "
                         f"{m.status if i == 0 else ''} |")
    summary = "、".join(f"{k} {v}" for k, v in sorted(counts.items()))
    (run_dir / "mutation-report.md").write_text(
        f"# 变异测试报告\n\n运行目录：`{run_dir.relative_to(ROOT)}`\n\n"
        f"基线：{sum(1 for b in baseline.values() if b.status == '通过')}/{len(baseline)} 通过\n\n"
        + "\n".join(lines) + f"\n\n合计 {len(results)} 条：{summary}\n", encoding="utf-8")

    print(f"\n合计 {len(results)} 条：{summary}")
    print(f"报告 {run_dir.relative_to(ROOT)}/mutation-report.md")
    return 1 if counts.get("INVALID") else 0


if __name__ == "__main__":
    raise SystemExit(main())
