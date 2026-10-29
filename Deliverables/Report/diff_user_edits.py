# -*- coding: utf-8 -*-
"""比对成品 docx 与 parts/*.md 的段落文本，列出双向差异。

协作约定（用户直接改成品 docx 时）：每次交互先跑本脚本，把 docx 独有段落
（用户直改/新增）回写 parts，再动手；重建会重置 parts 独有段落，重建前必须确认。

用法：
  python diff_user_edits.py            # 默认比对 gfx2d技术报告.docx
  python diff_user_edits.py 其他.docx

归一化只去空白与行内记号；图表题注由构建自动编号，源侧对应 FIG|/TC| 行本就
不参与比对，正文引用编号差异需人工判读。
"""
import re
import sys
from pathlib import Path

from docx import Document

HERE = Path(__file__).parent
PARTS = HERE / "parts"

NUM_PREFIX = re.compile(r"^(图|表)\s*\d+\s*")


CAPTION = re.compile(r"^(图|表)\s*\d+\s*")
FIXED = {"——技术报告——", "摘 要", "AI 工具使用说明"}


def norm(s):
    s = s.replace("&&√&&", "").replace("\u3000", " ").replace("**", "").replace("%%", "").replace("@@", "").replace("&&", "").replace("`", "").replace("N_max", "Nmax")
    s = re.sub(r"\s+", " ", s).strip()
    return CAPTION.sub("", s)


def docx_texts(path):
    doc = Document(path)
    out = []
    for p in doc.paragraphs:
        t = norm(p.text)
        if t and t not in FIXED and not p.text.startswith("GFX2D："):
            out.append(t)
    for tbl in doc.tables:
        for row in tbl.rows:
            for cell in row.cells:
                for p in cell.paragraphs:
                    t = norm(p.text)
                    if t:
                        out.append(t)
    return out


def md_texts():
    out = []
    in_abstract = False
    for f in sorted(PARTS.glob("*.md")):
        for line in f.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or set(line) == {"-"}:
                continue
            if line == "#ABSTRACT":
                in_abstract = True
                continue
            if line == "#END":
                in_abstract = False
                continue
            if line == "[[PAGEBREAK]]" or line.startswith(("#TITLE|", "#附|", "PH|")):
                continue
            if line.startswith(("TC|", "FIG|")):
                seg = line.split("|")
                idx = 2 if line.startswith("FIG|") else 1
                t = norm(seg[idx]) if len(seg) > idx else ""
                if t:
                    out.append((f.name, t))
                continue
            if line.startswith("#"):
                t = norm(line.lstrip("#"))
                if t:
                    out.append((f.name, t))
                continue
            if line.startswith("|"):
                for cell in line.strip("|").split("|"):
                    cell = cell.strip()
                    if not cell or set(cell) <= {"-", ":"}:
                        continue
                    t = norm(cell)
                    if t:
                        out.append((f.name, t))
                continue
            if line.startswith("- "):
                line = line[2:]
            t = norm(line)
            if t:
                out.append((f.name, t))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    target = sys.argv[1] if len(sys.argv) > 1 else "gfx2d技术报告.docx"
    doc_set = set(docx_texts(HERE / target))
    md_items = md_texts()
    md_set = {t for _, t in md_items}

    only_docx = sorted(doc_set - md_set)
    only_md = sorted((n, t) for n, t in md_items if t not in doc_set)

    print(f"docx 段落 {len(doc_set)}，md 段落 {len(md_set)}")
    print(f"\n== 只在成品 docx（疑似用户直改，需回写 parts）：{len(only_docx)} ==")
    for t in only_docx:
        print("  +", t[:80])
    print(f"\n== 只在 parts 源（重建会重置，动手前确认）：{len(only_md)} ==")
    for n, t in only_md:
        print(f"  - [{n}] {t[:80]}")


if __name__ == "__main__":
    main()
