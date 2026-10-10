#!/usr/bin/env python3
"""复用现有标准库 SVG 绘图函数，绘制 F1 实测与 F2 动态表记录曲线。"""
import json
from pathlib import Path
from plot_stats import text,line,rect
ROOT=Path(__file__).resolve().parents[3]
LOGS=ROOT/'Main/sim/engine/frontend/logs'
BLUE='#2563eb';ORANGE='#c2410c';GRAY='#64748b'
def page(title,body):
    return '<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="700" viewBox="0 0 1280 700"><rect width="1280" height="700" fill="white"/><g font-family="Microsoft YaHei,Noto Sans CJK SC,sans-serif">'+text(40,45,title,24)+body+'</g></svg>\n'
def panel(x,title,xmax,ymax,xticks,yticks,ylabel,xlabel):
    y=120;w=490;h=390;out=text(x+w/2,92,title,21,anchor='middle')
    for value in yticks:
        yy=y+h-value/ymax*h;out+=line(x,yy,x+w,yy,'#e2e8f0')+text(x-12,yy+6,f'{value:g}',15,anchor='end')
    for value in xticks:
        xx=x+value/xmax*w;out+=text(xx,y+h+26,str(value),15,anchor='middle')
    out+=line(x,y,x,y+h,'#334155',1.5)+line(x,y+h,x+w,y+h,'#334155',1.5)
    out+=text(x,115,ylabel,15)+text(x+w/2,y+h+57,xlabel,16,anchor='middle')
    return out,(x,y,w,h)
def curve(points,box,xmax,ymax,color):
    x,y,w,h=box;coords=[(x+a/xmax*w,y+h-b/ymax*h) for a,b,_ in points]
    out='<polyline points="'+' '.join(f'{a:.1f},{b:.1f}' for a,b in coords)+f'" fill="none" stroke="{color}" stroke-width="2.5"/>'
    for (a,b),(_,_,hint) in zip(coords,points):out+=f'<circle cx="{a:.1f}" cy="{b:.1f}" r="4" fill="{color}"><title>{hint}</title></circle>'
    return out
def main():
    baseline=json.loads((LOGS/'f1_baseline_4k.json').read_text());counts=json.loads((LOGS/'physical_counts.json').read_text())
    if len(baseline)!=34 or len(counts)!=170:raise RuntimeError('incomplete input matrix')
    by={(r['scene'],r['mode']):r for r in baseline};physical={(r['scene'],r['mode']):r for r in counts}
    body='';record='';knobs=[0,384,1024,1648,1984,2400]
    for index,size in enumerate((16,8)):
        offset=index*6;x=90+index*620
        out,box=panel(x,f'{size}×{size}',3000,3,[0,1000,2000,3000],[0,.5,1,1.5,2,2.5,3],'百万周期 / 帧','实际可见敌弹')
        budget=120+390-(100/60)/3*390
        out+=line(x,budget,x+490,budget,GRAY,1.5,'6 5')
        for mode,color in ((0,BLUE),(1,ORANGE)):
            pts=[]
            for i,k in enumerate(knobs):
                r=by[(offset+i,mode)];pts.append((r['enemy'],r['avg']/1e6,f'K={k}, enemy={r["enemy"]}, cycles={r["avg"]:.1f}'))
            out+=curve(pts,box,3000,3,color)
        body+=out
        out,box=panel(x,f'{size}×{size}',2400,100,[0,384,1024,1648,1984,2400],[0,20,40,60,80,100],'动态表片段减少 / %','K（固定输入控制参数）')
        for mode,color in ((5,BLUE),(6,ORANGE)):
            pts=[]
            for i,k in enumerate(knobs):
                original=physical[(offset+i,1)]['records'];n=physical[(offset+i,mode)]['records'];v=100*(original-n)/original
                pts.append((k,v,f'K={k}, baseline={original}, dynamic={n}, reduction={v:.3f}%'))
            out+=curve(pts,box,2400,100,color)
        record+=out
    body+=line(90,610,130,610,BLUE,3)+text(140,616,'v1 / TEX 直通构造',17)
    body+=line(480,610,520,610,ORANGE,3)+text(530,616,'原即时 v2 构造',17)
    body+=line(890,610,930,610,GRAY,1.5,'6 5')+text(940,616,'60 Hz 总周期预算',17)
    body+=text(40,667,'首轮只测构造，三次各 1000 帧均值；低于参考线不代表整机达到 60 FPS。',16)
    record+=line(90,610,130,610,BLUE,3)+text(140,616,'B1：缓存底色前缀',17)
    record+=line(480,610,520,610,ORANGE,3)+text(530,616,'B2：完整动态预合成',17)
    record+=text(40,667,'只比较 BAND 动态表；B2 仍新增 prepass 绘制、DDR 流量和完成依赖，记录减少不能替代周期收益。',16)
    a=ROOT/'Engineering/TechReports/engine/fig/12-构造周期曲线.svg';a.write_text(page('F1：真实固定状态的原生产构造成本',body),encoding='utf-8')
    b=ROOT/'Engineering/TechReports/sw/fig/09-动态表片段曲线.svg';b.write_text(page('F2：固定输入下的动态表记录变化',record),encoding='utf-8')
    print('FRONTEND FIGURES: 24 cycle points, 24 physical reduction points')
if __name__=='__main__':main()
