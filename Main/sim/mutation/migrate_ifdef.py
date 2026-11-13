#!/usr/bin/env python3
"""把生产 RTL 里的 ``ifdef INJ_xxx`` 注错块抽成变异规则，并删除注错分支。

这是迁移期的一次性工具：每个模块迁移完成后它就不再需要，最后一个模块迁完删除。
两个子命令可以合在一起用：

  python3 Main/sim/mutation/migrate_ifdef.py --file Main/rtl/engine/core/engine_blend.v --yaml Main/sim/mutation/mutants.yaml --write

做的事：

1. 找到文件里所有 ``ifdef INJ_*`` / ``elsif INJ_*`` 块（支持嵌套的其它 ``ifdef``）。
2. ``ifdef`` 分支是变异体，``else`` 分支是生产逻辑；删除整块只留生产分支。
3. 为每个变异体生成一条「原文 → 变异后」的字面替换规则。规则必须在新文件里
   唯一匹配，否则自动向上下扩行，直到唯一。变异体本身不留任何痕迹。
4. ``--write`` 写回新文件，``--yaml`` 输出 mutants.yaml 的骨架（元数据留占位符）。

没有 ``else`` 分支的块（变异体只是插入代码）会以「变异体 + 其后的代码」作为替换原文，
同样要求唯一匹配。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - 环境缺 PyYAML 时给出可读的失败原因
    sys.exit("需要 PyYAML（python3 -m pip install pyyaml）")

ROOT = Path(__file__).resolve().parents[3]
DIRECTIVE = re.compile(r"^\s*`(ifdef|ifndef|elsif|else|endif)\b\s*(\S*)")


@dataclass
class Block:
    name: str            # INJ_ 之后的宏名，如 COPY_DST
    start: int           # `ifdef 所在行
    else_at: int | None  # `else 所在行（没有则为 None）
    end: int             # `endif 所在行
    mut: list[str] = field(default_factory=list)
    prod: list[str] = field(default_factory=list)
    # 迁移后生产分支在文件中的位置与长度
    new_at: int = 0

    @property
    def lineno(self) -> str:
        return f"{self.start + 1}-{self.end + 1}"


def parse_blocks(lines: list[str]) -> list[Block]:
    blocks: list[Block] = []
    i = 0
    while i < len(lines):
        m = DIRECTIVE.match(lines[i])
        if not (m and m.group(1) in ("ifdef", "ifndef") and m.group(2).startswith("INJ_")):
            i += 1
            continue
        name = m.group(2)[len("INJ_"):]
        depth = 0
        else_at = None
        end = None
        j = i + 1
        while j < len(lines):
            mm = DIRECTIVE.match(lines[j])
            if mm:
                kind = mm.group(1)
                if kind in ("ifdef", "ifndef"):
                    depth += 1
                elif kind == "endif":
                    if depth == 0:
                        end = j
                        break
                    depth -= 1
                elif kind in ("else", "elsif") and depth == 0:
                    if kind == "else":
                        else_at = j
            j += 1
        if end is None:
            sys.exit(f"{name}: 第 {i + 1} 行的 `ifdef 没有配对的 `endif")
        blk = Block(name=name, start=i, else_at=else_at, end=end)
        if else_at is None:
            blk.mut = lines[i + 1:end]
            blk.prod = []
        else:
            blk.mut = lines[i + 1:else_at]
            blk.prod = lines[else_at + 1:end]
        blocks.append(blk)
        i = end + 1
    return blocks


def strip(lines: list[str], blocks: list[Block]) -> list[str]:
    out: list[str] = []
    cursor = 0
    for blk in blocks:
        out.extend(lines[cursor:blk.start])
        blk.new_at = len(out)
        out.extend(blk.prod)
        cursor = blk.end + 1
    out.extend(lines[cursor:])
    return out


def build_rule(new_text: str, new_lines: list[str], blk: Block) -> tuple[str, str] | None:
    """返回 (find, replace)，不带结尾空行；扩行直到 find 在新文件里唯一。"""
    start, end = blk.new_at, blk.new_at + len(blk.prod)

    def attempt(s: int, e: int) -> tuple[str, str] | None:
        find = "".join(new_lines[s:e]).rstrip("\n")
        if not find or new_text.count(find) != 1:
            return None
        repl = "".join(new_lines[s:start] + blk.mut + new_lines[end:e]).rstrip("\n")
        return None if repl == find else (find, repl)

    if not blk.prod:  # 纯插入：变异体加其后的代码作为原文
        for pad in range(0, 12):
            hit = attempt(start, min(len(new_lines), start + 1 + pad))
            if hit:
                return hit
        return None

    # 优先向上扩（前一行通常是注释或标号，上下文最自然），不足再向下
    for pad in range(0, 20):
        hit = attempt(max(0, start - pad), end)
        if hit:
            return hit
    for pad in range(1, 20):
        hit = attempt(start, min(len(new_lines), end + pad))
        if hit:
            return hit
    return None


def module_id(path: Path, name: str) -> str:
    stem = path.stem
    stem = stem[len("engine_"):] if stem.startswith("engine_") else stem
    return f"{stem}.{name.lower()}"


def read_lf(path: Path) -> tuple[str, str]:
    """读到 LF 规范化的文本，同时返回原文件的行尾，写回时照原样还原。"""
    raw = path.read_bytes()
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n")
    if crlf and crlf != lf:
        sys.exit(f"{path}: 混用 CRLF 与 LF，先统一行尾再迁移")
    eol = "\r\n" if crlf else "\n"
    return raw.decode("utf-8").replace("\r\n", "\n"), eol


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", action="append", required=True, help="要迁移的 RTL 文件（可重复）")
    ap.add_argument("--yaml", help="输出 mutants.yaml 骨架")
    ap.add_argument("--meta", help="按 id 合并元数据的 JSON（迁移期临时文件，不入库）")
    ap.add_argument("--from-rev", help="从该 git 版本读取源文件（迁移后再生成规则时用，禁止 --write）")
    ap.add_argument("--write", action="store_true", help="把删除注错分支后的内容写回源文件")
    ap.add_argument("--check", action="store_true", help="只检查能否生成唯一规则，不改文件")
    args = ap.parse_args()

    if args.from_rev and args.write:
        sys.exit("--from-rev 只生成规则，不能配 --write：源文件已迁移过")
    meta = json.loads(Path(args.meta).read_text(encoding="utf-8")) if args.meta else {}

    rows = []
    for arg in args.file:
        path = Path(arg)
        if not path.is_absolute():
            path = ROOT / path
        text, eol = read_lf(path)
        if args.from_rev:
            raw = subprocess.run(["git", "-C", str(ROOT), "show", f"{args.from_rev}:{path.relative_to(ROOT)}"],
                                 capture_output=True, check=True).stdout
            text = raw.decode("utf-8").replace("\r\n", "\n")
        lines = text.splitlines(keepends=True)
        if not lines or not lines[-1].endswith("\n"):
            sys.exit(f"{path}: 最后一行没有换行，先修好再迁移")

        blocks = parse_blocks(lines)
        if not blocks:
            print(f"{path.relative_to(ROOT)}: 没有 INJ_ 块，跳过")
            continue
        new_lines = strip(lines, blocks)
        new_text = "".join(new_lines)

        print(f"{path.relative_to(ROOT)}: {len(blocks)} 个块（{len(lines)} → {len(new_lines)} 行）")
        for blk in blocks:
            rule = build_rule(new_text, new_lines, blk)
            if rule is None:
                sys.exit(f"  {blk.name}: 无法生成唯一规则（第 {blk.lineno} 行）")
            find, repl = rule
            n_find = find.count("\n")
            print(f"  {module_id(path, blk.name):<28} 第 {blk.lineno:<9} 原文 {n_find} 行 → 变异 {repl.count(chr(10))} 行")
            rows.append({
                "id": module_id(path, blk.name),
                "target": str(path.relative_to(ROOT)).replace("\\", "/"),
                "macro": f"INJ_{blk.name}",
                "defect": "TODO",
                "spec": {"doc": "TODO", "section": "TODO"},
                "rule": {"find": find, "replace": repl},
                "runs": [],
                "expect": {"status": "FAIL", "evidence": ""},
                "note": "",
            })

        if args.write and not args.check:
            path.write_bytes(new_text.replace("\n", eol).encode("utf-8"))
            print(f"  已写回 {path.relative_to(ROOT)}（行尾 {'CRLF' if eol == chr(13) + chr(10) else 'LF'}）")

    if args.yaml:
        if not rows:
            sys.exit(f"没有生成任何规则，拒绝覆盖 {args.yaml}")
        for row in rows:
            row.update(meta.get(row["id"], {}))
        out = Path(args.yaml)
        if not out.is_absolute():
            out = ROOT / out
        header = (
            "# 变异清单（唯一真相）。每条记录对应一处生产 RTL 的故意改错。\n"
            "# 字段：defect 缺陷模型；spec 规格出处；rule 字面替换（必须唯一匹配）；\n"
            "# runs 应发现它的用例与见红位置正则（旧名 macro 只用于追溯原开关）。\n"
            "# 校验与执行：run_mutation.py；迁移等价性：verify_migration.py。\n"
        )
        body = yaml.safe_dump(rows, allow_unicode=True, sort_keys=False,
                              default_flow_style=False, width=10000)
        text = header + body
        if yaml.safe_load(text) != rows:
            sys.exit("mutants.yaml 往返不等价：多行规则被折成空格，规则不可信")
        out.write_text(text, encoding="utf-8", newline="")
        print(f"已写 {out.relative_to(ROOT)}（{len(rows)} 条）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
