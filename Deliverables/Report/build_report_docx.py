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


RED = RGBColor(0xFF, 0x00, 0x00)
GREEN = RGBColor(0x00, 0xB0, 0x50)
CHECK_PNG = HERE / ".work" / "check_green.png"


def ensure_check_png():
    """首次使用时生成「绿方块+镂空白勾」勾选图标(4x 超采样抗锯齿)。"""
    if CHECK_PNG.exists():
        return
    CHECK_PNG.parent.mkdir(parents=True, exist_ok=True)
    from PIL import Image, ImageDraw
    s = 4
    w = 96 * s
    img = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, w - 1, w - 1], radius=12 * s,
                        fill=(0, 0xB0, 0x50, 255))
    d.line([(26 * s, 50 * s), (42 * s, 66 * s), (70 * s, 32 * s)],
           fill=(255, 255, 255, 255), width=9 * s, joint="curve")
    img.resize((96, 96), Image.LANCZOS).save(CHECK_PNG)


def add_runs(p, text, size, bold=False, hei=False, color=None):
    """正文/表格统一出口:支持 **加粗**、%%红粗%%、@@绿字@@、&&勾选图标&&,并把 N_max 排成 N + 下标 max。"""
    for seg in re.split(r"(\*\*.+?\*\*|%%.+?%%|@@.+?@@|&&.+?&&)", text):
        if not seg:
            continue
        if seg.startswith("&&") and seg.endswith("&&"):
            ensure_check_png()
            r = p.add_run()
            r.add_picture(str(CHECK_PNG), width=Pt(9), height=Pt(9))
            continue
        seg_color, seg_bold = color, bold
        if seg.startswith("**") and seg.endswith("**"):
            core, seg_bold = seg[2:-2], True
        elif seg.startswith("%%") and seg.endswith("%%"):
            core, seg_bold, seg_color = seg[2:-2], True, RED
        elif seg.startswith("@@") and seg.endswith("@@"):
            core, seg_color = seg[2:-2], GREEN
        else:
            core = seg
        for part in re.split(r"(N_max)", core):
            if not part:
                continue
            if part == "N_max":
                r = p.add_run("N")
                set_font(r, size, seg_bold, hei, seg_color)
                r2 = p.add_run("max")
                set_font(r2, size, seg_bold, hei, seg_color)
                r2.font.subscript = True
            else:
                r = p.add_run(part)
                set_font(r, size, seg_bold, hei, seg_color)
    return p


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
        add_runs(p, text, size, bold, hei, color)
    return p


def shade(cell, hexcolor):
    tcPr = cell._tc.get_or_add_tcPr()
    el = tcPr.makeelement(qn("w:shd"), {qn("w:val"): "clear",
                                        qn("w:fill"): hexcolor})
    tcPr.append(el)


def add_table(doc, rows, widths_cm, spread=None, font=10.5):
    """spread=(行距倍数, 单元格上下余量pt)：把表在纵向撑开，用于独占整页。"""
    tb = doc.add_table(rows=len(rows), cols=len(rows[0]))
    tb.style = "Table Grid"
    tb.alignment = WD_TABLE_ALIGNMENT.CENTER
    tb.autofit = False
    for j, width in enumerate(widths_cm):
        tb.columns[j].width = Cm(width)
    for row in tb.rows:
        trPr = row._tr.get_or_add_trPr()
        trPr.append(trPr.makeelement(qn("w:cantSplit"), {}))
    trPr = tb.rows[0]._tr.get_or_add_trPr()
    trPr.append(trPr.makeelement(qn("w:tblHeader"), {}))
    for i, row in enumerate(rows):
        for j, text in enumerate(row):
            cell = tb.cell(i, j)
            cell.width = Cm(widths_cm[j])
            cell.vertical_alignment = WD_ALIGN_VERTICAL.CENTER
            cell.text = ""
            p = cell.paragraphs[0]
            p.paragraph_format.line_spacing = 1.15
            if spread is not None and i > 0:
                ls, pad = spread
                p.paragraph_format.line_spacing = ls
                p.paragraph_format.space_before = Pt(pad)
                p.paragraph_format.space_after = Pt(pad)
            add_runs(p, text, font, bold=(i == 0))
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
    p.paragraph_format.space_before = Pt(4)
    p.paragraph_format.keep_with_next = True
    p.add_run().add_picture(str(img), width=Cm(width_cm))
    cap = para(doc, f"图{fig_no}　{caption}", size=10.5, hei=True, bold=True,
               align=WD_ALIGN_PARAGRAPH.CENTER, indent=False, spacing=1.15,
               before=3, after=8)


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
    for name in ("Normal", "Heading 1", "Heading 2", "Heading 3", "Heading 4"):
        style = doc.styles[name]
        style.font.name = "Times New Roman"
        style.font.color.rgb = RGBColor(0, 0, 0)
        # 四级标题与正文同族(宋体)，只靠加粗区分，避免比正文还抢眼
        east = "宋体" if name in ("Normal", "Heading 4") else "黑体"
        style._element.get_or_add_rPr().get_or_add_rFonts().set(qn("w:eastAsia"), east)
        style.paragraph_format.widow_control = True
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
    li_no = 0
    spread_next = None
    tab_font = 10.5
    lines = []
    for f in sorted(PARTS.glob("*.md")):
        lines.extend(f.read_text(encoding="utf-8").splitlines())
        lines.append("")

    i = 0
    pending_pb = False   # [[PAGEBREAK]] 待生效:下一段落页首起,不另占一行
    while i < len(lines):
        line = lines[i].rstrip()
        i += 1
        if not line.strip():
            continue
        if not line.startswith("- "):
            li_no = 0        # 项目列表逐段重新编号

        if line.startswith("#TITLE|"):
            para(doc, line.split("|", 1)[1], size=20, bold=True, hei=True,
                 align=WD_ALIGN_PARAGRAPH.CENTER, indent=False,
                 before=24, after=18)
        elif line == "#ABSTRACT":
            para(doc, "摘　要", size=14, bold=True, hei=True,
                 align=WD_ALIGN_PARAGRAPH.CENTER, indent=False,
                 before=8, after=6)
            body = []
            while i < len(lines) and lines[i].strip() != "#END":
                body.append(lines[i].strip())
                i += 1
            i += 1  # 消费 #END 行
            # 摘要按源文件分行排版:每行一段,空行跳过
            for seg in body:
                if seg:
                    para(doc, seg, indent=True, after=2)
        elif line == "[[PAGEBREAK]]":
            pending_pb = True
        elif line.startswith("#附|"):
            p = para(doc, line.split("|", 1)[1], size=14, bold=True, hei=True,
                     indent=False, before=14, after=6, keep_with_next=True)
            if pending_pb:
                p.paragraph_format.page_break_before = True
                pending_pb = False
        elif line.startswith("# "):
            p = para(doc, line[2:], size=16, bold=True, hei=True,
                     align=WD_ALIGN_PARAGRAPH.CENTER, indent=False,
                     before=6, after=12)
            p.style = doc.styles["Heading 1"]
            p.paragraph_format.page_break_before = True
        elif line.startswith("#### "):
            p = para(doc, line[5:], size=12, bold=True, hei=False, indent=False,
                     before=6, after=2, keep_with_next=True)
            p.style = doc.styles["Heading 4"]
            if pending_pb:
                p.paragraph_format.page_break_before = True
                pending_pb = False
        elif line.startswith("### "):
            p = para(doc, line[4:], size=12, bold=True, hei=True, indent=False,
                     before=8, after=4, keep_with_next=True)
            p.style = doc.styles["Heading 3"]
            if pending_pb:
                p.paragraph_format.page_break_before = True
                pending_pb = False
        elif line.startswith("## "):
            p = para(doc, line[3:], size=14, bold=True, hei=True, indent=False,
                     before=10, after=5, keep_with_next=True)
            p.style = doc.styles["Heading 2"]
            if pending_pb:
                p.paragraph_format.page_break_before = True
                pending_pb = False
        elif line.startswith("TC|"):
            tab_no += 1
            title = line[3:]
            # TC|标题|FILL|行距|余量pt|字号 —— 让这张表纵向撑满一页
            seg_tc = title.split("|")
            spread_next = None
            tab_font = 10.5
            if len(seg_tc) >= 2 and seg_tc[1].strip().upper() == "FILL":
                ls = float(seg_tc[2]) if len(seg_tc) > 2 and seg_tc[2] else 1.45
                pad = float(seg_tc[3]) if len(seg_tc) > 3 and seg_tc[3] else 2.5
                spread_next = (ls, pad)
                if len(seg_tc) > 4 and seg_tc[4].strip():
                    tab_font = float(seg_tc[4])
                title = seg_tc[0]
            p = para(doc, f"表{tab_no}　{title}", size=10.5, hei=True, bold=True,
                     align=WD_ALIGN_PARAGRAPH.CENTER, indent=False, before=8, after=4,
                     keep_with_next=True)
            if pending_pb:
                p.paragraph_format.page_break_before = True
                pending_pb = False
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
            if ncols == 3 and rows[0][0] == "资源":
                widths = [4.8, 4.8, 4.8]
            elif ncols == 3 and rows[0][0] == "类别":
                widths = [2.1, 7.1, 5.2]
            else:
                widths = [14.4 / ncols] * ncols
            add_table(doc, rows, widths, spread=spread_next, font=tab_font)
            spread_next = None
            spacer = para(doc, "", size=6, indent=False, after=2)
            spacer.paragraph_format.line_spacing = Pt(4)
        elif line.startswith("- "):
            li_no += 1
            p = para(doc, f"（{li_no}）{line[2:]}", size=12, indent=False, spacing=1.4)
            p.paragraph_format.left_indent = Cm(1.3)
            p.paragraph_format.first_line_indent = Cm(-1.3)
            for r in p.runs:
                set_font(r, 12)
        else:
            p = para(doc, line, size=12)
            # 紧跟表题的正文与表题同页，避免表题与表格被拆到下一页
            nxt = next((l.strip() for l in lines[i:] if l.strip()), "")
            if nxt.startswith("TC|"):
                p.paragraph_format.keep_with_next = True

    doc.save(OUT)
    print(f"saved {OUT}  figures={fig_no} tables={tab_no}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    build()
