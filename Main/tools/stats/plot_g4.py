#!/usr/bin/env python3
"""从 G4 已核验清单与 SW9 历史原始日志生成报告用 N_max 对照图。"""
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
    ap.add_argument('--out',type=Path,default=ROOT/'Deliverables/Assert/figures/report/rep_g4_nmax.png')
    a=ap.parse_args()
    data=json.loads(a.results.read_text())
    old=(ROOT/'Main/sw/apps/game/gfx2dGame/logs/sw9_perf_hw_nmax.log').read_text()
    baseline=[int(x) for x in re.findall(r'nmax result OK n_max (\d+)',old)]
    if len(baseline)!=2:
        raise RuntimeError('historical N_max result count')
    groups=[baseline]+[[data[k]['16x16']['n_max'],data[k]['8x8']['n_max']]
                       for k in ('tex_4k','strip_4k','strip_16k')]
    plt.rcParams.update({'font.family':'sans-serif','font.sans-serif':['Microsoft YaHei','DejaVu Sans'],
                         'axes.unicode_minus':False,'font.size':12,'svg.fonttype':'none'})
    fig,ax=plt.subplots(figsize=(10,4.7))
    for j,(label,color) in enumerate((('16×16','#174A7E'),('8×8','#B66A24'))):
        x=[i+(j-.5)*.34 for i in range(4)]
        bars=ax.bar(x,[g[j] for g in groups],width=.32,color=color,label=label)
        ax.bar_label(bars,padding=4,fontsize=12)
    ax.set_xticks(range(4),['P1 / DDR\n历史基线','4KB / TEX\n同位流直通','4KB / STRIP\n本轮主线','16KB / STRIP\n候选构型'])
    ax.set_ylabel('N_max（活动元素数）')
    ax.set_ylim(0,max(max(g) for g in groups)*1.23)
    ax.legend(frameon=False,ncol=2,loc='upper left')
    ax.spines[['top','right']].set_visible(False)
    ax.grid(axis='y',alpha=.18);ax.set_axisbelow(True)
    fig.text(.5,.02,'本轮三档：每种弹型各 3600 场零掉帧确认，扫描欠载 0；预测值不作实测。',ha='center',fontsize=10,color='#526174')
    fig.tight_layout(rect=(0,.07,1,1))
    a.out.parent.mkdir(parents=True,exist_ok=True)
    fig.savefig(a.out,dpi=200,bbox_inches='tight')
    fig.savefig(a.out.with_suffix('.svg'),bbox_inches='tight')
    svg=a.out.with_suffix('.svg')
    svg.write_text('\n'.join(line.rstrip() for line in svg.read_text(encoding='utf-8').splitlines())+'\n',encoding='utf-8')
    print('G4 plot:',a.out)


if __name__=='__main__':
    main()
