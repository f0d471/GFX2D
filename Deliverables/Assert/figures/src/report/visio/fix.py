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
    normalize(tree, name)
    tree.write(f'{OUT}/{name}.fixed.drawio', encoding='utf-8', xml_declaration=True)
    print(f'saved {name}.fixed.drawio')


# ---------- 全局规整:色板 / 箭头 / 线宽 / 术语 / 字号 ----------
# 只归并"同义近重复"的颜色,保留语义色相;描边取深一档保证白底对比 >= 4.5:1,
# 填充统一为同族浅色,避免同一语义在不同图里出现 2~3 个近似值。
PALETTE = {
    # fills: 计算 / 数据通路 / 存储 / 外部 / 告警 / 高亮
    '#ecffed': '#e8f5e9', '#d9f2d0': '#e8f5e9', '#eef7e9': '#e8f5e9',
    '#dff4fc': '#e3f2fd', '#eaf8fd': '#e3f2fd', '#edf3fb': '#e3f2fd',
    '#eff1d8': '#fff8e1', '#e8ebc7': '#fff8e1', '#fbf2d9': '#fff8e1', '#f5f8e0': '#fff8e1',
    '#e9e8ec': '#eceff1', '#e4e4e4': '#eceff1',
    '#fee2fe': '#fce4ec', '#fb95d2': '#f8bbd0', '#ccd280': '#fff59d',
    # strokes
    '#128a0f': '#2e7d32', '#0c88ba': '#1565c0', '#156082': '#1565c0',
    '#1499e9': '#1565c0', '#0b76a0': '#1565c0',
    '#fe2e38': '#c62828', '#ee0000': '#c62828', '#ff0000': '#c62828',
    '#c5cb70': '#8d6e00', '#a9ac3e': '#8d6e00', '#ceb024': '#8d6e00',
    '#8d8f34': '#8d6e00', '#757b2d': '#8d6e00', '#7a7c2c': '#8d6e00',
    '#fd809f': '#c2185b', '#fb95d2': '#c2185b',
    '#7f7f7f': '#616161', '#a5a5a5': '#9e9e9e', '#b8b8b8': '#9e9e9e',
    '#d8d8d8': '#bdbdbd', '#0c0c0c': '#212121',
}

# 术语/单位:与正文口径统一(MHz、SoC、DSP 前留空格、去掉零宽字符与悬空星号)
WORDS = [
    ('25.2Mhz', '25.2 MHz'), ('25.2MHZ', '25.2 MHz'),
    ('Sapphire Soc', 'Sapphire SoC'),
    ('12DSP', '12 DSP'), ('LIST_*', 'LIST'), (' LIST *', ' LIST'),
    ('\u200b', ''), ('\u200c', ''), ('\u200d', ''),
    ('APB 0xF810 _', 'APB 0xF810_'),
    ('TMDS编码640x480@60HZ', 'TMDS 640×480@60'),
    ('检测 / 屏蔽', '检测/屏蔽'),
    ('CPU 生成命令', 'CPU 命令'), ('引擎渲染后台', '引擎渲染'),
    ('屏幕呈现前台', '屏幕呈现'), ('保持前帧（掉帧）', '保持前帧'),
    ('定点·CRC·正弦表·统计', '定点·CRC·统计'),
]

# 字号(单位)调整:只放大到几何上放得下的档位,不动已经是主标题的大字
FONT_BUMP = {
    'rep_system':  {8.47: 10.2},
    'rep_ddrmap':  {11.29: 14.5, 14.11: 14.5},
    'rep_dbuf':    {25.4: 35.6, 33.87: 47.4},
    'rep_engine':  {8.47: 9.6},
    'rep_assets':  {8.47: 9.2},
}

# 重写整条标签容易与图形自带文字叠字,这里留空;过长标签用手工短语替换处理
REBUILD = {}

# 图4 的整句注释移到图注,图内只留结构
DROP_TEXT = {
    'rep_dbuf': ['CPU填第N帧命令表与引擎渲染第N-1帧并行',
                 '菱形为消隐期切换点',
                 'list_done 晚于消隐'],
}


ZERO = re.compile('[\u200b\u200c\u200d\ufeff]')


def normalize(tree, name):
    bumps = FONT_BUMP.get(name, {})
    drops = DROP_TEXT.get(name, [])

    def fix_text(raw):
        if not raw:
            return raw
        raw = ZERO.sub('', raw)          # 零宽字符会让后面的短语匹配失败,先清掉
        for a, b in WORDS:
            raw = raw.replace(a, b)
        for a, b in bumps.items():
            raw = raw.replace('font-size:%gpx' % a, 'font-size:%gpx' % b)
        return raw

    for el in tree.getroot().iter():
        if el.tag == 'UserObject':
            if el.get('label') is not None:
                el.set('label', fix_text(el.get('label')))
            c = el.find('mxCell')
        else:
            c = el if el.tag == 'mxCell' else None
        if c is None:
            continue
        if c.get('value') is not None:
            c.set('value', fix_text(c.get('value')))
        st = c.get('style') or ''
        st2 = re.sub(r'strokeWidth=[0-9.]+;?', '', st)          # 线宽统一走默认
        st2 = re.sub(r'(startSize|endSize)=[0-9.]+', r'\1=4', st2)  # 箭头统一大小
        if c.get('edge') == '1':
            # 显式补齐,否则未写的边仍用 drawio 默认值,同一张图里箭头大小还会不一致
            if 'endSize=' not in st2:
                st2 += 'endSize=4;'
            if 'startSize=' not in st2:
                st2 += 'startSize=4;'
        for a, b in PALETTE.items():
            st2 = st2.replace(a, b).replace(a.upper(), b)
        c.set('style', st2)

    # 个别过长标签按行重写(保证不压框线)
    for key, lines in REBUILD.get(name, []):
        for el in tree.getroot().iter():
            c = el.find('mxCell') if el.tag == 'UserObject' else (el if el.tag == 'mxCell' else None)
            if c is None:
                continue
            raw = (el.get('label') if el.tag == 'UserObject' else c.get('value')) or ''
            if key not in re.sub(r'<[^>]+>', '', raw):
                continue
            html = '<div style="font-size:1px">' + ''.join(
                '<p style="text-align:center;margin:0;line-height:115%%;">'
                '<font style="font-size:%.1fpx;font-family:SimSun;color:#1d2733">%s</font></p>' % (s, t)
                for s, t in lines) + '</div>'
            if el.tag == 'UserObject':
                el.set('label', html)
            else:
                c.set('value', html)

    # 丢掉整句注释(标签清空):按去空白后的纯文本匹配,避免标记/空格差异漏掉
    if drops:
        for el in tree.getroot().iter():
            c = el.find('mxCell') if el.tag == 'UserObject' else (el if el.tag == 'mxCell' else None)
            if c is None:
                continue
            v = (el.get('label') if el.tag == 'UserObject' else c.get('value')) or ''
            plain = re.sub(r'\s+', '', re.sub(r'<[^>]+>', '', v))
            if any(re.sub(r'\s+', '', d) in plain for d in drops):
                if el.tag == 'UserObject':
                    el.set('label', '')
                else:
                    c.set('value', '')


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


def wp_set(cid, idx, axis, val):
    """改第 idx 个折点的坐标;vsdx 导入的折点与图形锚点常差零点几单位,渲染成斜线。"""
    for arr in cells[cid][2].findall('Array'):
        if arr.get('as') == 'points':
            arr.findall('mxPoint')[idx].set(axis, '%.2f' % val)
            return
    raise SystemExit('wp_set: no waypoints in ' + cid)


def frac_set(cid, key, val):
    c = cells[cid][1]
    c.set('style', re.sub(key + r'=[-0-9.]+', '%s=%.10f' % (key, val), c.get('style')))


# 取直:5 条线在 vsdx 里端点/折点各差 0.4~1 单位,按 5.5px/单位渲染后是 2~5px 的斜线
wp_set('39', 0, 'x', 372.0)                        # 中断_A 落 CPU 的竖段(原 372.8)
wp_set('44', 0, 'x', 544.0)                        # S0 落 Axi_Mux 的竖段(原 544.8)
wp_set('50', 0, 'x', 331.0)                        # UART0 落 CPU 的竖段(原 331.4)
wp_set('47', 1, 'x', 640.0)                        # E000 落 display 的竖段(原 640.1)
frac_set('42', 'exitY', (683.0 - 578.0) / 110.0)   # CPU->guard 横段(y 684->683)
frac_set('48', 'exitY', (617.0 - 578.0) / 110.0)   # CPU->Axi_Mux 横段(y 616->617)
print('  straightened: 39/44/50/47 verticals, 42/48 horizontals')
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
