#!/usr/bin/env python3
"""把生产源码里的注错块抽成变异规则，并删除注错分支。

这是迁移期的一次性工具：全部模块迁完就删除。支持 Verilog（`` `ifdef ``）与
C（`` #ifdef ``），宏名以 ``INJ_`` 开头。

做的事：

1. 找到所有 ``INJ_*`` 块。**由内向外迭代剥离**：注错块可以互相嵌套
   （例如 `INJ_BAND_RAW_MASK` 嵌在 `INJ_MASK_BYPASS` 的 else 分支里），
   每轮只处理最内层，删完再重新扫描，直到文件里没有 INJ 块。
2. ``ifdef`` 分支是变异体，``else`` 分支是生产逻辑；删除整块只留生产分支。
3. 为每个宏生成一条或多条「原文 → 变异后」的字面替换规则。规则必须在**最终文件**里
   唯一匹配，否则自动向上下扩行。同一个宏在多个地方生效时，规则是一组、同时施加。
4. ``--write`` 写回新文件，``--yaml`` 输出 mutants.yaml 的骨架（元数据留占位符）。

行尾保持原样（LF 或 CRLF），混用直接报错。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("需要 PyYAML（python3 -m pip install pyyaml）")

ROOT = Path(__file__).resolve().parents[3]
DIRECTIVE = re.compile(r"^\s*[`#]\s*(ifdef|ifndef|elsif|else|endif)\b\s*(\S*)")


class Block:
    def __init__(self, name: str, start: int, else_at: int | None, end: int,
                 mut: list[str], prod: list[str], end_rest: str = ""):
        self.name = name
        self.start = start
        self.else_at = else_at
        self.end = end
        self.mut = mut
        self.prod = prod
        self.end_rest = end_rest  # endif 行尾还挂着的代码（如 " || (fd_valid && fd_ready) ||"）
        self.new_at = 0

    @property
    def lineno(self) -> str:
        return f"{self.start + 1}-{self.end + 1}"


def parse_blocks(lines: list[str]) -> list[Block]:
    stack: list[dict] = []
    found: list[tuple[dict, int]] = []
    for i, line in enumerate(lines):
        m = DIRECTIVE.match(line)
        if not m:
            continue
        kind = m.group(1)
        if kind in ("ifdef", "ifndef"):
            stack.append({"inj": m.group(2).startswith("INJ_"), "name": m.group(2),
                          "start": i, "else": None, "neg": kind == "ifndef"})
        elif kind in ("else", "elsif"):
            if stack and stack[-1]["else"] is None:
                stack[-1]["else"] = i
        elif kind == "endif":
            if not stack:
                sys.exit(f"第 {i + 1} 行有多余的 endif")
            top = stack.pop()
            if top["inj"]:
                found.append((top, i))
    if stack:
        sys.exit(f"第 {stack[-1]['start'] + 1} 行的 ifdef 没有配对的 endif")

    blocks: list[Block] = []
    for top, end in found:
        s, e = top["start"], top["else"]
        kw = re.search(r"[`#]\s*endif\b", lines[end])
        rest = lines[end][kw.end():] if kw else ""
        if not rest.strip():
            rest = ""
        if top["neg"]:
            # `ifndef INJ_X`：生产逻辑是块体，变异体是「整块不执行」
            prod, mut = lines[s + 1:end], ([] if e is None else lines[s + 1:e])
        elif e is None:
            mut, prod = lines[s + 1:end], []
        else:
            mut, prod = lines[s + 1:e], lines[e + 1:end]
        blocks.append(Block(top["name"][len("INJ_"):], s, e, end, mut, prod, rest))
    blocks.sort(key=lambda b: b.start)
    return blocks


def leaves(blocks: list[Block]) -> list[Block]:
    """没有别的 INJ 块嵌在自己里面的那些块（最内层）。"""
    return [b for b in blocks if not any(b.start < o.start and o.end < b.end for o in blocks)]


def strip(lines: list[str], blocks: list[Block]) -> list[str]:
    out: list[str] = []
    cursor = 0
    for blk in blocks:
        out.extend(lines[cursor:blk.start])
        blk.new_at = len(out)
        out.extend(blk.prod)
        if blk.end_rest:  # else/endif 行尾的代码属于生产逻辑，不能跟着指令一起删
            out.append(blk.end_rest.rstrip() + "\n")
        cursor = blk.end + 1
    out.extend(lines[cursor:])
    return out


def build_rule(new_text: str, new_lines: list[str], blk: Block) -> tuple[str, str] | None:
    start, end = blk.new_at, blk.new_at + len(blk.prod)

    def attempt(s: int, e: int) -> tuple[str, str] | None:
        find = "".join(new_lines[s:e]).rstrip("\n")
        if not find or new_text.count(find) != 1:
            return None
        repl = "".join(new_lines[s:start] + blk.mut + new_lines[end:e]).rstrip("\n")
        return None if repl == find else (find, repl)

    if not blk.prod:  # 纯插入
        for pad in range(0, 12):
            hit = attempt(start, min(len(new_lines), start + 1 + pad))
            if hit:
                return hit
        return None
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
    raw = path.read_bytes()
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n")
    if crlf and crlf != lf:
        sys.exit(f"{path}: 混用 CRLF 与 LF，先统一行尾再迁移")
    return raw.decode("utf-8").replace("\r\n", "\n"), ("\r\n" if crlf else "\n")


def migrate_text(text: str) -> tuple[str, dict[str, list[dict]], list[Block]]:
    """返回（最终文本、每个宏的规则列表、原有块列表）。"""
    original = parse_blocks(text.splitlines(keepends=True))
    cur = text.splitlines(keepends=True)
    rules: dict[str, list[dict]] = {}
    while True:
        blocks = parse_blocks(cur)
        if not blocks:
            break
        layer = leaves(blocks)
        if not layer:
            sys.exit("找不到最内层注错块，文件结构异常")
        new_lines = strip(cur, layer)
        new_text = "".join(new_lines)
        for blk in sorted(layer, key=lambda b: b.start):
            rule = build_rule(new_text, new_lines, blk)
            if rule is None:
                sys.exit(f"INJ_{blk.name}: 无法生成唯一规则（第 {blk.lineno} 行）")
            rules.setdefault(blk.name, []).append({"find": rule[0], "replace": rule[1]})
        cur = new_lines
    return "".join(cur), rules, original


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", action="append", required=True, help="要迁移的 RTL/C 文件（可重复）")
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
        if not text.endswith("\n"):
            sys.exit(f"{path}: 最后一行没有换行，先修好再迁移")

        new_text, rules, original = migrate_text(text)
        if not original:
            print(f"{path.relative_to(ROOT)}: 没有 INJ_ 块，跳过")
            continue
        print(f"{path.relative_to(ROOT)}: {len(original)} 个块 / {len(rules)} 个宏"
              f"（{len(text.splitlines())} → {len(new_text.splitlines())} 行）")

        # 说明文字可能在 ifdef 行的行尾，也可能在变异分支的行尾
        notes: dict[str, str] = {}
        for blk in original:
            cand = [text.splitlines()[blk.start]] + [ln for ln in blk.mut if "//" in ln]
            for line in cand:
                m = re.search(r"//\s*(.+?)\s*$", line) or re.search(r"/\*\s*(.+?)\s*\*/", line)
                if m and m.group(1) and blk.name not in notes:
                    notes[blk.name] = m.group(1)
                    break

        for name, ruleset in rules.items():
            rid = module_id(path, name)
            rows.append({
                "id": rid,
                "target": str(path.relative_to(ROOT)).replace("\\", "/"),
                "macro": f"INJ_{name}",
                "defect": "TODO",
                "spec": {"doc": "TODO", "section": "TODO"},
                "rules": ruleset,
                "runs": [],
                "note": notes.get(name, "TODO"),
            })
            print(f"  {rid:<30} {len(ruleset)} 处  {notes.get(name, '')}")

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
            "# 变异清单（唯一真相）。每条记录对应一处生产源码的故意改错。\n"
            "# 字段：defect 缺陷模型；spec 规格出处；rules 字面替换（每条必须唯一匹配，同一宏可多处）；\n"
            "# runs 应发现它的命令与见红位置正则；macro 只用于追溯原来的 ifdef 开关。\n"
            "# 校验与执行：run_mutation.py；迁移等价性：verify_migration.py。\n"
        )
        body = yaml.safe_dump(rows, allow_unicode=True, sort_keys=False,
                              default_flow_style=False, width=10000)
        text_out = header + body
        if yaml.safe_load(text_out) != rows:
            sys.exit("mutants.yaml 往返不等价：多行规则被折成空格，规则不可信")
        out.write_text(text_out, encoding="utf-8", newline="")
        print(f"已写 {out}（{len(rows)} 条）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
