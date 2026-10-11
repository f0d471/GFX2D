# -*- coding: utf-8 -*-
"""把 parts/*.md 构建成 gfx2d技术报告.docx。

md 语法（本目录约定）：
  #TITLE|文本        封面标题（20pt 黑体居中）
  #ABSTRACT ... #END 摘要块（居中黑体"摘要"标题 + 正文）
  # 一级标题          部分标题（16pt 黑体，页首自动分页）
  ## / ###           二三级标题（14pt / 12pt 加粗黑体）
  #附|标题           不编号的附属性标题（14pt 黑体，不分页）
  TC|标题            表题（置于紧随其后的表格上方，自动编号 表N）
  FIG|相对路径|标题   插图 + 图题（置于图下方，自动编号 图N）
  PH|标题|说明        占位框（待补实拍/观察项，不占图号）
  | a | b |          表格（连续行）
  - 文本             项目符号
  [[PAGEBREAK]]      手动分页
正文：宋体/Times New Roman 12pt，1.5 倍行距，首行缩进 2 字符。
"""
import re
import sys
from pathlib import Path

from docx import Document
from docx.enum.table import WD_TABLE_ALIGNMENT, WD_ALIGN_VERTICAL
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_LINE_SPACING
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor
from PIL import Image

HERE = Path(__file__).parent
PARTS = HERE / "parts"
OUT = HERE / "gfx2d技术报告.docx"
HEADER_IMG = HERE / ".work" / "tpl" / "word" / "media" / "image1.png"

INK = RGBColor(0x1D, 0x27, 0x33)
MUTE = RGBColor(0x87, 0x92, 0x9E)


def set_font(run, size, bold=False, hei=False, color=None):
    f = run.font
    f.size = Pt(size)
    f.bold = bold
    f.name = "Times New Roman"
    f._element.rPr.rFonts.set(qn("w:eastAsia"), "黑体" if hei else "宋体")
    if color is not None:
        f.color.rgb = color


def para(doc, text="", size=12, bold=False, hei=False, align=None, indent=True,
         spacing=1.5, before=0, after=0, color=None, keep_with_next=False):
    p = doc.add_paragraph()
    pf = p.paragraph_format
    if align is not None:
        pf.alignment = align
    pf.line_spacing = spacing
    pf.space_before = Pt(before)
    pf.space_after = Pt(after)
    if keep_with_next:
        pf.keep_with_next = True
    if indent:
        pf.first_line_indent = Pt(size * 2)
    if text:
        for seg in re.split(r"(\*\*.+?\*\*)", text):
            if not seg:
                continue
            if seg.startswith("**") and seg.endswith("**"):
                r = p.add_run(seg[2:-2])
                set_font(r, size, bold=True, hei=hei, color=color)
            else:
                r = p.add_run(seg)
                set_font(r, size, bold, hei, color)
    return p


def shade(cell, hexcolor):
    tcPr = cell._tc.get_or_add_tcPr()
    el = tcPr.makeelement(qn("w:shd"), {qn("w:val"): "clear",
                                        qn("w:fill"): hexcolor})
    tcPr.append(el)


def add_table(doc, rows, widths_cm):
    tb = doc.add_table(rows=len(rows), cols=len(rows[0]))
    tb.style = "Table Grid"
    tb.alignment = WD_TABLE_ALIGNMENT.CENTER
    tb.autofit = False
    for row in tb.rows:
        trPr = row._tr.get_or_add_trPr()
        trPr.append(trPr.makeelement(qn("w:cantSplit"), {}))
    trPr = tb.rows[0]._tr.get_or_add_trPr()
    trPr.append(trPr.makeelement(qn("w:tblHeader"), {}))
    for i, row in enumerate(rows):
        for j, text in enumerate(row):
            cell = tb.cell(i, j)
            cell.width = Cm(widths_cm[j])
            cell.text = ""
            p = cell.paragraphs[0]
            p.paragraph_format.line_spacing = 1.15
            r = p.add_run(text)
            set_font(r, 10.5, bold=(i == 0))
            if i == 0:
                p.alignment = WD_ALIGN_PARAGRAPH.CENTER
                shade(cell, "EDF0F3")
    for j in range(len(rows[0])):
        r0 = 1
        while r0 < len(rows):
            r1 = r0
            while r1 + 1 < len(rows) and rows[r1 + 1][j] == rows[r0][j]:
                r1 += 1
            if r1 > r0:
                merged = tb.cell(r0, j).merge(tb.cell(r1, j))
                for extra in merged.paragraphs[1:]:
                    extra._p.getparent().remove(extra._p)
                merged.vertical_alignment = WD_ALIGN_VERTICAL.CENTER
                merged.paragraphs[0].alignment = WD_ALIGN_PARAGRAPH.CENTER
            r0 = r1 + 1
    return tb


def add_figure(doc, path, caption, fig_no, width_cm=14.4):
    img = HERE / path
    with Image.open(img) as im:
        w_px, h_px = im.size
    width_cm = width_cm
    height_cm = width_cm * h_px / w_px
    p = doc.add_paragraph()
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    p.paragraph_format.space_before = Pt(6)
    p.paragraph_format.keep_with_next = True
    p.add_run().add_picture(str(img), width=Cm(width_cm))
    cap = para(doc, f"图{fig_no}　{caption}", size=10.5, hei=True, bold=True,
               align=WD_ALIGN_PARAGRAPH.CENTER, indent=False, spacing=1.15,
               before=3, after=10)


def add_placeholder(doc, title, note):
    tb = doc.add_table(rows=1, cols=1)
    tb.style = "Table Grid"
    tb.alignment = WD_TABLE_ALIGNMENT.CENTER
    trPr = tb.rows[0]._tr.get_or_add_trPr()
    trPr.append(trPr.makeelement(qn("w:cantSplit"), {}))
    cell = tb.cell(0, 0)
    cell.width = Cm(14.4)
    cell.text = ""
    p1 = cell.paragraphs[0]
    p1.alignment = WD_ALIGN_PARAGRAPH.CENTER
    p1.paragraph_format.space_before = Pt(28)
    r = p1.add_run(f"【占位】{title}")
    set_font(r, 12, bold=True, color=MUTE)
    p2 = cell.add_paragraph()
    p2.alignment = WD_ALIGN_PARAGRAPH.CENTER
    p2.paragraph_format.space_after = Pt(28)
    r2 = p2.add_run(note)
    set_font(r2, 10.5, color=MUTE)
    shade(cell, "FAFAFA")


def build():
    doc = Document()
    sec = doc.sections[0]
    sec.page_width, sec.page_height = Cm(21.0), Cm(29.7)
    sec.top_margin = sec.bottom_margin = Cm(2.54)
    sec.left_margin = sec.right_margin = Cm(3.175)
    sec.header_distance, sec.footer_distance = Cm(1.5), Cm(1.4)

    # 页眉：竞赛横幅；页脚：页码
    hp = sec.header.paragraphs[0]
    hp.alignment = WD_ALIGN_PARAGRAPH.CENTER
    hp.add_run().add_picture(str(HEADER_IMG), width=Cm(8.8))
    fp = sec.footer.paragraphs[0]
    fp.alignment = WD_ALIGN_PARAGRAPH.CENTER
    fld = fp._p.makeelement(qn("w:fldSimple"), {qn("w:instr"): "PAGE"})
    r = fp._p.makeelement(qn("w:r"), {})
    t = fp._p.makeelement(qn("w:t"), {})
    t.text = "1"
    r.append(t)
    fld.append(r)
    fp._p.append(fld)

    fig_no = 0
    tab_no = 0
    lines = []
    for f in sorted(PARTS.glob("*.md")):
        lines.extend(f.read_text(encoding="utf-8").splitlines())
        lines.append("")

    i = 0
    while i < len(lines):
        line = lines[i].rstrip()
        i += 1
        if not line.strip():
            continue

        if line.startswith("#TITLE|"):
            para(doc, line.split("|", 1)[1], size=20, bold=True, hei=True,
                 align=WD_ALIGN_PARAGRAPH.CENTER, indent=False,
                 before=24, after=6)
            para(doc, "——技术报告——", size=12, hei=True,
                 align=WD_ALIGN_PARAGRAPH.CENTER, indent=False, after=18)
        elif line == "#ABSTRACT":
            para(doc, "摘　要", size=14, bold=True, hei=True,
                 align=WD_ALIGN_PARAGRAPH.CENTER, indent=False,
                 before=12, after=8)
            body = []
            while i < len(lines) and lines[i].strip() != "#END":
                body.append(lines[i].strip())
                i += 1
            i += 1  # 消费 #END 行
            para(doc, "".join(body), indent=True)
        elif line == "[[PAGEBREAK]]":
            doc.add_page_break()
        elif line.startswith("#附|"):
            para(doc, line.split("|", 1)[1], size=14, bold=True, hei=True,
                 indent=False, before=14, after=6, keep_with_next=True)
        elif line.startswith("# "):
            p = para(doc, line[2:], size=16, bold=True, hei=True,
                     align=WD_ALIGN_PARAGRAPH.CENTER, indent=False,
                     before=6, after=12)
            p.paragraph_format.page_break_before = True
        elif line.startswith("### "):
            para(doc, line[4:], size=12, bold=True, hei=True, indent=False,
                 before=8, after=4, keep_with_next=True)
        elif line.startswith("## "):
            para(doc, line[3:], size=14, bold=True, hei=True, indent=False,
                 before=10, after=5, keep_with_next=True)
        elif line.startswith("TC|"):
            tab_no += 1
            para(doc, f"表{tab_no}　{line[3:]}", size=10.5, hei=True, bold=True,
                 align=WD_ALIGN_PARAGRAPH.CENTER, indent=False, before=8, after=4,
                 keep_with_next=True)
        elif line.startswith("FIG|"):
            seg = line.split("|")
            path, caption = seg[1], seg[2]
            width = float(seg[3]) if len(seg) > 3 and seg[3] else 14.4
            fig_no += 1
            add_figure(doc, path, caption, fig_no, width)
        elif line.startswith("PH|"):
            _, title, note = line.split("|", 2)
            add_placeholder(doc, title, note)
            para(doc, "", size=6, indent=False, after=4)
        elif line.startswith("|"):
            rows = [[c.strip() for c in line.strip().strip("|").split("|")]]
            while i < len(lines) and lines[i].strip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-{3,}:?", c) for c in cells):
                    rows.append(cells)
                i += 1
            ncols = len(rows[0])
            if ncols == 4 and rows[0][0] == "资源":
                widths = [4.6, 3.5, 3.4, 2.9]
            elif ncols == 3 and rows[0][0] == "类别":
                widths = [2.2, 5.2, 7.0]
            else:
                widths = [14.4 / ncols] * ncols
            add_table(doc, rows, widths)
            para(doc, "", size=6, indent=False, after=2)
        elif line.startswith("- "):
            p = para(doc, line[2:], size=12, indent=False, spacing=1.4)
            p.paragraph_format.left_indent = Cm(0.74)
            p.style = doc.styles["List Bullet"]
            for r in p.runs:
                set_font(r, 12)
        else:
            para(doc, line, size=12)

    doc.save(OUT)
    print(f"saved {OUT}  figures={fig_no} tables={tab_no}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    build()
