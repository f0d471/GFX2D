#!/usr/bin/env python3
"""F1～F3 报告的原生 SVG 架构图，参数与连接来自正式接口。"""
from html import escape
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def make(path,title,boxes,edges,notes):
    out=['<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="700" viewBox="0 0 1280 700">',
         '<defs><marker id="arrow" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto"><path d="M0,0 L8,3 L0,6" fill="none" stroke="#334155"/></marker></defs>',
         '<rect width="1280" height="700" fill="white"/>',
         '<g font-family="Microsoft YaHei, Noto Sans CJK SC, sans-serif" fill="#172554">',
         f'<text x="40" y="48" font-size="23">{escape(title)}</text>']
    for x,y,w,h,label,detail,kind in boxes:
        color='#e8f1fb' if kind=='cpu' else '#f1f5f9'
        out += [f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{color}" stroke="#334155" stroke-width="1.5"/>',
                f'<text x="{x+16}" y="{y+31}" font-size="18">{escape(label)}</text>']
        for i,line in enumerate(detail):out.append(f'<text x="{x+16}" y="{y+60+24*i}" font-size="14">{escape(line)}</text>')
    for coords,label,tx,ty in edges:
        out.append(f'<polyline points="{coords}" fill="none" stroke="#334155" stroke-width="2" marker-end="url(#arrow)"/>')
        out.append(f'<text x="{tx}" y="{ty}" font-size="14">{escape(label)}</text>')
    for i,line in enumerate(notes):out.append(f'<text x="40" y="{570+i*27}" font-size="15">{escape(line)}</text>')
    out += ['<rect x="40" y="660" width="18" height="15" fill="#e8f1fb" stroke="#334155"/>',
            '<text x="68" y="673" font-size="13">蓝色：CPU 构造或检查</text>',
            '<rect x="320" y="660" width="18" height="15" fill="#f1f5f9" stroke="#334155"/>',
            '<text x="348" y="673" font-size="13">灰色：存储或引擎执行</text>',
            '<text x="700" y="673" font-size="13">实线箭头：调用或数据依赖</text></g></svg>']
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text('\n'.join(out)+'\n',encoding='utf-8')
def main():
    make(ROOT/'Engineering/TechReports/engine/fig/12-构造成本测量.svg','frontendBench：固定输入、生产构造与计时外消费',[
      (40,110,350,130,'scene / stg_game',['两型 × 六个 K 档','通关录像的五个固定状态','同状态、同表面、同 clip'],'cpu'),
      (470,110,350,130,'stg_game_compose',['真实 gfx_blit_run / gfx_fill','board_cycles：100 MHz','粗粒度阶段与细分诊断独立构建'],'cpu'),
      (900,110,340,130,'gfx_list',['v1 连续表 / v2 32 KiB 桶','planned / geometry 可单独启用','END / seal 后才可消费'],'memory'),
      (40,360,350,130,'check_frontend.py',['三次各 1000 帧','总周期、p50 / p99 / max','缺组、错列、溢出、未 done 拒绝'],'cpu'),
      (470,360,350,130,'frontend_consume',['独立编译单元，无 LTO','读取全部有效记录的八个字段','固定输入 CRC 与宿主 / RV32 对账'],'cpu'),
      (900,360,340,130,'renderer_run / engine_top',['关闭渲染：隔离 CPU','开启扫描和真实执行：计依赖','PERF_BUSY 与墙钟分开登记'],'memory')],
      [('390,175 470,175','调用',410,157),('820,175 900,175','32 B 记录',830,157),
       ('1070,240 1070,360','提交',1090,310),('900,425 820,425','完成后消费',825,405),
       ('470,425 390,425','原始日志',395,405)],
      ['计时区仅含被测构造；CRC、排序和日志输出均在区间之外。',
       '细分探针会扰动被测代码；阶段和总计的差额单列，不把细项相加当成生产帧时间。'])
    make(ROOT/'Engineering/TechReports/sw/fig/09-帧计划与背景租约.svg','gfx_frame_plan：预合成完成先于 BAND LOAD',[
      (40,110,350,150,'frontend_game_compose',['draw_background：保持内部画序','动态对象：保持原相对次序','HUD：活动数确定后构造','游戏区与 HUD 的 x 区间不相交'],'cpu'),
      (470,110,350,150,'gfx_frame_plan',['最多三个 pass，依赖指向前序','SURF 描述符快照与 generation','A / B 各自背景页与命令 lease','seal 失败不发布'],'cpu'),
      (900,110,340,150,'renderer_submit_plan',['拓扑序串行提交同一引擎','空闲时绑定本 pass 的 SURF','完成 ticket 匹配后进入下一 pass','错误先排空，再释放资源'],'cpu'),
      (40,360,350,130,'prepass / engine_top',['v1 / TEX 背景和 HUD','写入本帧独占离屏面','全可见区初始化'],'memory'),
      (470,360,350,130,'BAND / engine_top',['从背景面 LOAD','执行动态 v2 稳定序列','FLUSH 到本帧 framebuffer'],'memory'),
      (900,360,340,130,'board_present',['整帧成功才请求交换','场边界完成后复用目标面','背景 lease 在完成后释放'],'memory')],
      [('390,185 470,185','构造',410,167),('820,185 900,185','seal',840,167),
       ('1070,260 1070,305 215,305 215,360','提交 prepass',600,292),
       ('390,425 470,425','完成依赖',395,405),('820,425 900,425','成功完成',830,405)],
      ['B1 只缓存每个不相交区域的底色前缀；B2 每帧重画完整背景和 HUD。',
       '同一引擎的两次执行不能并行；预合成、SURF 绑定和依赖空泡全部进入性能账。'])
    make(ROOT/'Engineering/TechReports/sw/fig/10-保序计划与封表.svg','gfx_list_begin_strips_planned：先组织引用，再物化 ABI v2',[
      (40,110,350,150,'gfx_strip_append_checked',['参数与反馈检查仍在接受时执行','全局 clip / 源有效区求交','预检全部覆盖桶与 workspace','整次追加成功后才更新计数'],'cpu'),
      (470,110,350,150,'gfx_strip_workspace',['Plan：48 B / 项，16 B 对齐','refs：u32 plan_id','counts / offsets / cursors：紧凑','零面积和裁出不占 Plan'],'memory'),
      (900,110,340,150,'gfx_strip_plan_seal',['30 桶线性 exclusive scan','按 Plan 原序散写小引用','每桶引用是原输入稳定子序列','首错阻止发布'],'cpu'),
      (40,360,350,130,'gfx_geometry_cache',['8 项，显式上下文所有权','源位置、clip 与 generation 失效','首/中/尾模式；边界走完整求交'],'cpu'),
      (470,360,350,130,'连续物化',['按桶读取 refs / Plan','重建局部 y、源偏移和 w7','仍完整写入 32 B / 片段'],'cpu'),
      (900,360,340,130,'ABI v2 / engine_top',['首段表头，1024 条 / 桶','ERROR_INDEX = s × 1024 + j','sealed 表才能提交'],'memory')],
      [('390,185 470,185','规范化',400,167),('820,185 900,185','END',840,167),
       ('1070,260 1070,310 645,310 645,360','引用区间',800,297),
       ('390,425 470,425','可选几何',395,405),('820,425 900,425','表头最后发布',825,405)],
      ['原即时 gfx_list_begin_strips 保持可选；P1 / P2 / P3 分别消融。',
       'Plan / refs 的写读成本和模板查找均计入构造时间；连续输出不预设性能收益。'])
if __name__=='__main__':main()
