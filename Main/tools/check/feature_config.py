#!/usr/bin/env python3
"""引擎特性位的单一真源：位定义、依赖、合法性与 CAPS 计算。

本模块只描述“有哪些位、谁依赖谁、什么组合合法”，不生成任何产物。
生成产物的是 feature_build.py（engine_feature.vh / 源列表 / CAPS 常量 / 软件能力头），
寄存器偏移的真源仍是 regs_gen.py；特性位只影响 CAPS 的复位值。

位序在结构位与像素位内部固定，配置名由结构位与像素位共同决定。
"""
import itertools

# 结构位（五级机制），顺序即结构位序，用于配置名的拼写与报告。
STRUCTURE = (
    ('TEX', 'GFX_HAS_TEX', '16 KiB 片上纹理与第 4~7 页装载窗口'),
    ('BAND_HW', 'GFX_HAS_BAND_HW', '条带 RAM、三相数据面与 mover 片上目的路由'),
    ('STRIP', 'GFX_HAS_STRIP', '命令层帧级条带控制与 ABI v2 条带表'),
    ('INST', 'GFX_HAS_INST', '直通 op 0x05 与实例坐标取数'),
    ('PB', 'GFX_HAS_PB', 'PB 构建、引用执行与小事务写桥'),
)
PIXEL = (
    ('EQ_EXT', 'GFX_EQ_EXT', 'CONST（eq3）与 MUL（eq4）'),
    ('ALPHA', 'GFX_ALPHA', '预乘与直通 ALPHA（eq1/eq5）及 alpha 链'),
    ('KEY', 'GFX_KEY', '色键比较、延迟链与跳过计数'),
)
DELIVERY = ('DELIVERY', 'GFX_DELIVERY', '交付构型标志，另要求 ALPHA 与 KEY 都为 1')

# 依赖：机制 -> 它需要的机制。方向只有“机制指向硬件/前置”，没有反向边。
DEPENDS = {'PB': ('BAND_HW', 'INST'), 'STRIP': ('BAND_HW',)}

GUARDS = (
    # (条件位, 缺失位, 缺失模块名)
    ('PB', 'BAND_HW', 'GFX_CONFIG_ERROR_PB_REQUIRES_BAND_HW'),
    ('PB', 'INST', 'GFX_CONFIG_ERROR_PB_REQUIRES_INST'),
    ('STRIP', 'BAND_HW', 'GFX_CONFIG_ERROR_STRIP_REQUIRES_BAND_HW'),
    ('DELIVERY', 'ALPHA', 'GFX_CONFIG_ERROR_DELIVERY_REQUIRES_ALPHA'),
    ('DELIVERY', 'KEY', 'GFX_CONFIG_ERROR_DELIVERY_REQUIRES_KEY'),
)

STRUCT_NAMES = tuple(n for n, _, _ in STRUCTURE)
PIXEL_NAMES = tuple(n for n, _, _ in PIXEL)
ALL_NAMES = STRUCT_NAMES + PIXEL_NAMES + ('DELIVERY',)

# CAPS 位号：只登记硬件能力。STRIP 没有专属位（它只改帧级控制由谁产生），
# 因此 CAPS 不能用来判断 STRIP 是否在构建里。
CAPS_BIT = {'TEX': 6, 'BAND_HW': 5, 'INST': 12, 'PB': 13,
            'EQ_EXT': 3, 'ALPHA_PM': 1, 'ALPHA_ST': 2, 'KEY': 0}
CAPS_LANES_LO, CAPS_LANES_VALUE = 8, 4


def normalize(config):
    """把配置（名字或字典）规范化成只含“开/关”的完整字典。"""
    if isinstance(config, str):
        config = parse_name(config)
    unknown = set(config) - set(ALL_NAMES)
    if unknown:
        raise ValueError('未知特性名: ' + ', '.join(sorted(unknown)))
    return {name: bool(config.get(name, False)) for name in ALL_NAMES}


def parse_name(name):
    """解析配置名。结构位与像素位用 0/1 串，如 10110-111 或 10110-111-d。"""
    parts = name.split('-')
    if len(parts) == 1 and parts[0] in PRESETS:
        return dict(PRESETS[parts[0]])
    if len(parts) not in (2, 3):
        raise ValueError('配置名形如 结构位-像素位[-d]，例如 10110-111-d；或代表档名如 B3')
    if len(parts) == 3 and parts[2] != 'd':
        raise ValueError('交付标志只能是 -d')
    struct, pixel = parts[0], parts[1]
    if len(struct) != len(STRUCT_NAMES) or set(struct) - set('01'):
        raise ValueError(f'结构位必须是 {len(STRUCT_NAMES)} 位 0/1')
    if len(pixel) != len(PIXEL_NAMES) or set(pixel) - set('01'):
        raise ValueError(f'像素位必须是 {len(PIXEL_NAMES)} 位 0/1')
    config = {n: b == '1' for n, b in zip(STRUCT_NAMES, struct)}
    config.update({n: b == '1' for n, b in zip(PIXEL_NAMES, pixel)})
    config['DELIVERY'] = len(parts) == 3 and parts[2] == 'd'
    return config


def check(config):
    """返回非法原因列表；空列表表示合法。"""
    config = normalize(config)
    reasons = []
    for name, needs in DEPENDS.items():
        if config[name]:
            for need in needs:
                if not config[need]:
                    reasons.append(f'{name} 需要 {need}')
    if config['DELIVERY']:
        for need in ('ALPHA', 'KEY'):
            if not config[need]:
                reasons.append(f'DELIVERY 需要 {need}')
    return reasons


def is_legal(config):
    return not check(config)


def caps(config):
    """按特性位算出 CAPS 复位值。"""
    config = normalize(config)
    value = (CAPS_LANES_VALUE << CAPS_LANES_LO)
    for name, bit in CAPS_BIT.items():
        if name == 'ALPHA_PM':
            on = config['ALPHA']
        elif name == 'ALPHA_ST':
            on = config['ALPHA']
        elif name == 'BAND_HW':
            on = config['BAND_HW']
        else:
            on = config[name]
        if on:
            value |= 1 << bit
    return value


def name_of(config):
    """配置的规范名：结构位-像素位[-d]。"""
    config = normalize(config)
    struct = ''.join('1' if config[n] else '0' for n in STRUCT_NAMES)
    pixel = ''.join('1' if config[n] else '0' for n in PIXEL_NAMES)
    return struct + '-' + pixel + ('-d' if config['DELIVERY'] else '')


def enabled_macros(config):
    config = normalize(config)
    macros = []
    for name, macro, _ in STRUCTURE + PIXEL + (DELIVERY,):
        if config[name]:
            macros.append(macro)
    return macros


def synthesis_mode(config):
    """全结构构型需要更深的逻辑优化；其它构型保留 speed。"""
    config = normalize(config)
    return 'speed2' if all(config[n] for n in STRUCT_NAMES) else 'speed'


def legal_configurations(with_pixel=True):
    """穷举合法配置。with_pixel=False 时只穷举结构位（像素位与交付位按全开取）。"""
    result = []
    for bits in itertools.product((0, 1), repeat=len(STRUCT_NAMES)):
        struct = dict(zip(STRUCT_NAMES, (b == 1 for b in bits)))
        if not is_legal({**struct, **{n: True for n in PIXEL_NAMES}, 'DELIVERY': True}):
            continue
        if not with_pixel:
            result.append({**struct, **{n: True for n in PIXEL_NAMES}, 'DELIVERY': True})
            continue
        for pbits in itertools.product((0, 1), repeat=len(PIXEL_NAMES)):
            pixel = dict(zip(PIXEL_NAMES, (b == 1 for b in pbits)))
            for delivery in (False, True):
                candidate = {**struct, **pixel, 'DELIVERY': delivery}
                if is_legal(candidate):
                    result.append(candidate)
    return result


def all_configurations(with_pixel=True):
    """穷举全部输入，返回 (合法列表, 非法列表)。"""
    legal, illegal = [], []
    repeats = len(STRUCT_NAMES) + (len(PIXEL_NAMES) + 1 if with_pixel else 0)
    for bits in itertools.product((0, 1), repeat=repeats):
        struct = dict(zip(STRUCT_NAMES, (b == 1 for b in bits[:len(STRUCT_NAMES)])))
        if with_pixel:
            pixel = dict(zip(PIXEL_NAMES, (b == 1 for b in bits[len(STRUCT_NAMES):len(STRUCT_NAMES) + len(PIXEL_NAMES)])))
            config = {**struct, **pixel, 'DELIVERY': bool(bits[-1])}
        else:
            config = {**struct, **{n: True for n in PIXEL_NAMES}, 'DELIVERY': True}
        (legal if is_legal(config) else illegal).append(config)
    return legal, illegal


# 代表档：按“每步只加一个机制”的消融序列选，用于控制昂贵构建次数。
# B1~B4 逐个把机制加回去；B6 用于单独量条带 RAM 的成本。
PRESETS = {
    'B0': dict(TEX=0, BAND_HW=0, STRIP=0, INST=0, PB=0, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
    'B1': dict(TEX=0, BAND_HW=0, STRIP=0, INST=1, PB=0, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
    'B2': dict(TEX=1, BAND_HW=0, STRIP=0, INST=1, PB=0, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
    'B3': dict(TEX=1, BAND_HW=1, STRIP=1, INST=1, PB=0, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
    'B4': dict(TEX=1, BAND_HW=1, STRIP=1, INST=0, PB=0, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
    'B5': dict(TEX=1, BAND_HW=1, STRIP=1, INST=1, PB=1, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
    'B6': dict(TEX=1, BAND_HW=1, STRIP=0, INST=1, PB=0, EQ_EXT=1, ALPHA=1, KEY=1, DELIVERY=1),
}

# 主工程交付基本档：结构机制关闭、像素能力全开。生成头、寄存器默认值与
# 工程综合模式都跟随本配置；采用其它构型后重新同步并构建位流。
CANONICAL = 'B0'

# 每个开关对应的源文件：关闭时从编译输入里整出。
FEATURE_SOURCES = {
    'TEX': ('rtl/engine/engine_tex.v',),
    'BAND_HW': ('rtl/engine/engine_band.v',),
    'PB': ('rtl/engine/engine_pb.v', 'rtl/engine/engine_pb_dma.v',
           'rtl/engine/engine_bin.v', 'rtl/engine/engine_bounds.v',
           'rtl/engine/engine_packet_check.v'),
    'INST': ('rtl/engine/engine_inst.v',),
}


def _self_check():
    """把计数与代表档固化成断言：以后改动 DEPENDS 或 PRESETS 会在这里停住。"""
    structures = [c for c in legal_configurations(with_pixel=False)]
    assert len(structures) == 16, len(structures)
    research = [c for c in legal_configurations() if not c['DELIVERY']]
    delivery = [c for c in legal_configurations() if c['DELIVERY']]
    assert (len(research), len(delivery)) == (128, 32), (len(research), len(delivery))
    assert len(research) + len(delivery) == 160
    # 3.3.3 节的旧表数字，必须与结构位拆分前对不上，否则说明依赖被写反了。
    assert len(structures) != 20
    for name, expected in (('B0', 0x0000040F), ('B1', 0x0000140F), ('B2', 0x0000144F),
                           ('B3', 0x0000146F), ('B4', 0x0000046F), ('B5', 0x0000346F),
                           ('B6', 0x0000146F)):
        config = parse_name(name)
        assert not check(config), (name, check(config))
        assert caps(config) == expected, (name, hex(caps(config)))
    return True


assert _self_check()
