import xml.etree.ElementTree as ET
import sys, re

# 七张报告图的 drawio 补丁:修 vsdx 导入的溢出/错位/错字。
# 用法: python3 fix.py <raw目录> <输出目录>   (文件名 rep_*.raw.drawio -> rep_*.fixed.drawio)
RAW, OUT = sys.argv[1], sys.argv[2]

def load(name):
    tree = ET.parse(f'{RAW}/{name}.raw.drawio')
    root = tree.getroot()
    cells = {}
    for el in root.iter():
        if el.tag == 'UserObject':
            cell = el.find('mxCell')
            if cell is not None:
                cells[el.get('id')] = (el, cell, cell.find('mxGeometry'), 'label')
        elif el.tag == 'mxCell':
            cid = el.get('id')
            if cid is None:
                cid = f'_n{len(cells)}'
            if cid not in cells:
                cells[cid] = (el, el, el.find('mxGeometry'), 'value')
    return tree, root, cells

def label_of(entry):
    return entry[0].get(entry[3]) or ''

def set_label(entry, s):
    entry[0].set(entry[3], s)

def geom(entry):
    return entry[2]

def set_geom(entry, **kw):
    g = entry[2]
    full = {'w': 'width', 'h': 'height'}
    for k, v in kw.items():
        g.set(full.get(k, k), str(v))

def align_left(entry):
    set_label(entry, label_of(entry).replace('text-align:center', 'text-align:left'))
    # drawio 对整块标签的落位由 style 的 align 决定,HTML 内 text-align 只管行内
    st = entry[1].get('style') or ''
    if 'align=center' in st:
        entry[1].set('style', st.replace('align=center', 'align=left'))
    elif not re.search(r'(^|;)align=', st):
        entry[1].set('style', 'align=left;' + st)

def clean_label(entry, text, px, bold=True, color='#000000'):
    b1, b2 = ('<b>', '</b>') if bold else ('', '')
    set_label(entry,
        '<div style="font-size: 1px"><p style="text-align:center;margin-left:0;margin-right:0;'
        'margin-top:0px;margin-bottom:0px;text-indent:0;vertical-align:middle;direction:ltr;">'
        f'<font style="font-size:{px}px;font-family:Themed;color:{color};direction:ltr;'
        f'letter-spacing:0px;line-height:120%;opacity:1">{b1}{text}{b2}</font></p></div>')

def label_sub(entry, old, new, must=True):
    lb = label_of(entry)
    if old not in lb:
        if must:
            raise SystemExit(f'label_sub: {old!r} not found in {label_of(entry)[:60]!r}')
        return
    set_label(entry, lb.replace(old, new))

def legend_fix(cells, text_ids=None, yband=None, gap=12):
    """图例文字贴到色块右侧并左对齐。text_ids 显式指定;否则按 yband 扫描。"""
    def visible(e):
        return re.sub(r'<[^>]+>', '', label_of(e)).strip()
    swatches = [(cid, e) for cid, e in cells.items()
                if e[2] is not None and 0 < float(e[2].get('width', 99)) <= 14
                and 0 < float(e[2].get('height', 99)) <= 14 and not visible(e)]
    pairs = []
    ids = text_ids if text_ids else [cid for cid, e in cells.items()
            if e[2] is not None and float(e[2].get('width', 0)) >= 100
            and float(e[2].get('height', 0)) <= 30 and label_of(e).strip()
            and (yband is None or yband[0] <= float(e[2].get('y', -1)) <= yband[1])]
    for tid in ids:
        e = cells[tid]; g = e[2]
        tx = float(g.get('x', 0)); ty = float(g.get('y', 0)); tw = float(g.get('width', 0))
        tcx, tcy = tx + tw / 2, ty + float(g.get('height', 0)) / 2
        best, bd = None, 1e9
        for sid, s in swatches:
            sg = s[2]
            scx = float(sg.get('x', 0)) + float(sg.get('width', 0)) / 2
            scy = float(sg.get('y', 0)) + float(sg.get('height', 0)) / 2
            d = abs(scx - tcx) + abs(scy - tcy) * 2
            if scx < tcx and abs(scy - tcy) < 25 and d < bd:
                best, bd = s, d
        if best is not None:
            sg = best[2]
            set_geom(e, x=round(float(sg.get('x', 0)) + float(sg.get('width', 0)) + gap, 1))
            align_left(e)
            pairs.append((tid, best[0]))
    print('  legend pairs:', pairs)
    return pairs

def save(tree, name):
    # Themed 不是真实字体,Chromium 会回退到日文字体导致简体字形变日式(虚线->虛線);
    # 统一替换为 SimSun,与 Visio 原稿一致。
    for el in tree.getroot().iter():
        if el.tag in ('UserObject',):
            v = el.get('label')
            if v and 'font-family:Themed' in v:
                el.set('label', v.replace('font-family:Themed', 'font-family:SimSun'))
        elif el.tag == 'mxCell':
            v = el.get('value')
            if v and 'font-family:Themed' in v:
                el.set('value', v.replace('font-family:Themed', 'font-family:SimSun'))
        st = el.get('style')
        if st and 'overflow=width' in st:
            el.set('style', st.replace('overflow=width;', '').replace('overflow=width', ''))
        if el.tag == 'UserObject':
            c = el.find('mxCell')
            if c is not None:
                st = c.get('style')
                if st and 'overflow=width' in st:
                    c.set('style', st.replace('overflow=width;', '').replace('overflow=width', ''))
    tree.write(f'{OUT}/{name}.fixed.drawio', encoding='utf-8', xml_declaration=True)
    print(f'saved {name}.fixed.drawio')

# ---------- rep_engine: AX14 -> AXI4 ----------
tree, root, cells = load('rep_engine')
hits = [cid for cid, e in cells.items() if 'AX14' in label_of(e)]
assert len(hits) == 1, hits
label_sub(cells[hits[0]], 'AX14', 'AXI4')
save(tree, 'rep_engine')

# ---------- rep_dbuf: 底注加宽左对齐 ----------
tree, root, cells = load('rep_dbuf')
note = cells['29']
set_geom(note, w=2400)
st = note[1].get('style') or ''
note[1].set('style', re.sub(r'shape=stencil\([^)]*\)', '', st))  # stencil 按原 vsdx 宽度裁字,去掉
# 该形状带 Visio 收缩适配残留(25.4px 运行 + 隐性裁剪),整体重建为干净单行
clean_label(note, 'CPU填第N帧命令表与引擎渲染第N-1帧并行;  list_done后置SWAP_REQ,  '
                  '下一场消隐期锁存切换;  引擎超期则保持前帧并计入掉帧(帧率=场数-切换数)。',
           12.7, bold=False, color='#595959')
align_left(note)
save(tree, 'rep_dbuf')

# ---------- rep_system ----------
tree, root, cells = load('rep_system')
clean_label(cells['14'], 'axi_mux_guard', 12)                 # 扁框三行 -> 单行
clean_label(cells['21'], 'SPI Flash 8 MB', 11)                # 框窄字大 -> 单行缩小
label_sub(cells['6'], 'line-height:120%', 'line-height:100%') # display 框 4 行压行高
label_sub(cells['28'], '@100 MHz', '@100 MHz)')               # 补右括号
for tid, swx in [('28', 575), ('26', 575), ('30', 575), ('32', 640), ('33', 682)]:
    set_geom(cells[tid], x=swx + 16, w=320)   # 色块右 6px 起左对齐,加宽防换行
    align_left(cells[tid])
# SPI0 取直:出口钉到锚点 40 (x=291) 正上方,浮动出口会朝目标偏成斜线
for cid, e in list(cells.items()):
    c = e[1]
    if c.get('edge') == '1' and c.get('target') == '40':
        st = re.sub(r'(exit|entry)[XY]=[-0-9.]+;?', '', c.get('style') or '')
        c.set('style', st + ';exitX=0.21;exitY=0;exitDx=0;exitDy=0;')
        print('  SPI0 edge pinned:', cid)
# APB 0xF810_0000 被 E000 的竖线(x=366,Soc->display)从中截断:标签左移到竖线左侧,
# 边 46 的左端竖线一并左移到 x=285,横线因此从 331 延长到 285 仍压住标签。
set_geom(cells['8'], x=274)
st = re.sub(r'exit[XY]=[-0-9.]+;?', '', cells['46'][1].get('style') or '')
cells['46'][1].set('style', st.rstrip(';') + ';exitX=%.6f;exitY=0;exitDx=0;exitDy=0;'
                   % ((285 - 277) / 107.0))
for arr in cells['46'][2].findall('Array'):
    if arr.get('as') == 'points':
        for p in arr.findall('mxPoint'):
            if p.get('x') == '331':
                p.set('x', '285')
print('  APB 0xF810_0000 moved left to x=274, bus left end -> 285')
save(tree, 'rep_system')

# ---------- rep_swlayers: 右侧注释 + 图例 ----------
tree, root, cells = load('rep_swlayers')
set_geom(cells['25'], w=230)
align_left(cells['25'])
for tid, swx in [('20', 152), ('21', 228), ('22', 292), ('23', 348)]:
    set_geom(cells[tid], x=swx + 17)
    align_left(cells[tid])
save(tree, 'rep_swlayers')

# ---------- rep_ddrmap: 图例 ----------
tree, root, cells = load('rep_ddrmap')
for tid, swx in [('23', 189), ('25', 314), ('27', 409)]:
    set_geom(cells[tid], x=swx + 25)
    align_left(cells[tid])
save(tree, 'rep_ddrmap')

# ---------- rep_scanout / rep_assets: 无补丁 ----------
for n in ('rep_scanout', 'rep_assets'):
    tree, root, cells = load(n)
    save(tree, n)
