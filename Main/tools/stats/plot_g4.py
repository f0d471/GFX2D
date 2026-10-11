#!/usr/bin/env python3
"""从已核验清单、历史原始日志与分桶最终板测结果生成报告用 N_max 对照图。"""
import argparse
import json
from pathlib import Path
import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT=Path(__file__).resolve().parents[3]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--results',type=Path,default=ROOT/'Main/sim/engine/g4/logs/performance_results.json')
    ap.add_argument('--f5',type=Path,default=ROOT/'Main/sim/engine/frontend/logs/f5_nmax.json')
    ap.add_argument('--out',type=Path,default=ROOT/'Deliverables/Assert/figures/report/rep_g4_nmax.png')
    a=ap.parse_args()
    data=json.loads(a.results.read_text())
    f5=json.loads(a.f5.read_text())
    old=(ROOT/'Main/sw/apps/game/gfx2dGame/logs/sw9_perf_hw_nmax.log').read_text()
    baseline=[int(x) for x in re.findall(r'nmax result OK n_max (\d+)',old)]
    if len(baseline)!=2:
        raise RuntimeError('historical N_max result count')
    groups=[baseline]+[[data[k]['16x16']['n_max'],data[k]['8x8']['n_max']]
                       for k in ('tex_4k','strip_4k','strip_16k')]
    groups.append([int(f5['PB'][0][0]),int(f5['PB'][1][0])])
    plt.rcParams.update({'font.family':'sans-serif','font.sans-serif':['Microsoft YaHei','DejaVu Sans'],
                         'axes.unicode_minus':False,'font.size':12,'svg.fonttype':'none'})
    fig,ax=plt.subplots(figsize=(10,4.7))
    for j,(label,color) in enumerate((('16×16','#174A7E'),('8×8','#B66A24'))):
        x=[i+(j-.5)*.34 for i in range(5)]
        bars=ax.bar(x,[g[j] for g in groups],width=.32,color=color,label=label)
        ax.bar_label(bars,padding=4,fontsize=12)
    ax.set_xticks(range(5),['DDR 直通\n历史基线','4KB 直通＋\n片上纹理','4KB 条带\n后端','16KB 条带\n候选构型','4KB 分桶＋\n参数缓冲（本轮）'])
    ax.set_ylabel('N_max（活动元素数）')
    ax.set_ylim(0,max(max(g) for g in groups)*1.23)
    ax.legend(frameon=False,ncol=2,loc='upper left')
    ax.spines[['top','right']].set_visible(False)
    ax.grid(axis='y',alpha=.18);ax.set_axisbelow(True)
    fig.text(.5,.02,'各档为该构型交付轮的同板实测，每种弹型各 3600 场零掉帧确认，扫描欠载 0；'
                    '分桶收益以同一比特流、同轮直通为对照（+16.9%/+12.3%）；预测值不作实测。',ha='center',fontsize=10,color='#526174')
    fig.tight_layout(rect=(0,.07,1,1))
    a.out.parent.mkdir(parents=True,exist_ok=True)
    fig.savefig(a.out,dpi=200,bbox_inches='tight')
    fig.savefig(a.out.with_suffix('.svg'),bbox_inches='tight')
    svg=a.out.with_suffix('.svg')
    svg.write_text('\n'.join(line.rstrip() for line in svg.read_text(encoding='utf-8').splitlines())+'\n',encoding='utf-8')
    print('N_max plot:',a.out)


if __name__=='__main__':
    main()
