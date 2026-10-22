#!/usr/bin/env python3
"""唯一检查清单与按范围执行入口；默认只显示计划。"""
import argparse
import json
import os
from pathlib import Path, PureWindowsPath
from types import SimpleNamespace
import shutil
import re
import subprocess
import sys
from check_run import IncompleteCheck, ROOT, RUNS, Session, command, git, identity, write_json

CHECKS = {}


def add(name, args, purpose, dependencies=(), timeout=1800):
    CHECKS[name] = {'command': list(map(str, args)), 'purpose': purpose,
                    'dependencies': list(dependencies), 'timeout': timeout}


P = 'Main/tools/check/'
add('paths', [sys.executable, P+'localize_paths.py', '--check', '--tracked'], '入库文件的路径检查')
add('regs', [sys.executable, P+'regs_gen.py', '--check'], '寄存器生成物与真源一致')
add('semantics', [sys.executable, P+'check_cmd_semantics.py'], '命令语义向量与错误模型')
add('workflow', [sys.executable, P+'test_validation.py'], '编排、缺项、证据过期与复用反例')
add('pack', [sys.executable, 'Main/tools/assets/pack_assets.py'], '准备实际素材输入')
add('lib', ['bash', P+'check_host.sh', 'lib'], '库的正常 64/32 位与 sanitizer')
add('app', ['bash', P+'check_host.sh', 'app'], '游戏应用的正常 64/32 位与 sanitizer')
add('rast', [sys.executable, P+'check_rast.py'],
    'R1 定点/128B setup/两参考器/独立有理数模型/10万光栅与1万几何/变异/双目标', ['lib'])
for name, script, purpose in [('layers','check_sw_layers.sh','软件分层与构建'),
    ('pc','check_sw4_pc.sh','PC 显示、输入与录像'),('assets','check_sw5_assets.sh','素材与镜像'),
    ('demo','check_sw6_demo.sh','基础 Demo'),('game','check_sw7_game.sh','游戏规则、回放与相关变异'),
    ('stats','check_sw8_stats.sh','统计、日志队列、模式与相关变异')]:
    add(name, ['bash', P+script], purpose)
for name in ['hdmi','apb_probe','axi_probe','axi_mux_guard','display','engine/blend',
             'engine/mover','engine/geom','engine/pix','engine/ctrl','engine/frame','engine/tex']:
    add(name.replace('engine/',''), ['bash','Main/sim/'+name+'/run.sh'], name+' 模块与变异')
add('band', [sys.executable,P+'check_g2_band.py'], '条带数据面')
add('stripctl', [sys.executable,P+'check_g3_stripctl.py'], '条带控制与生产构造')
add('g4-host',[sys.executable,P+'check_g4_integration.py','--host'],'整机条带宿主回放', ['pack'])
add('g4-rtl',[sys.executable,P+'check_g4_integration.py','--rtl'],'整机条带 RTL 帧', ['pack'])
add('frontend',[sys.executable,P+'check_frontend.py','--models','--pipeline'],'前端模型与资源生命周期')
add('instances',[sys.executable,P+'check_f4_instances.py','--models','--rtl','--mutations','--protocols','--lease'],
    '实例构造、完整引擎、变异、错误与 lease')
add('bin',[sys.executable,P+'check_f5_bin.py','--models','--bin','--frame','--mutations','--protocols','--lease'],
    '硬件分桶、完整引擎、变异、错误与 lease')
add('bin-game',[sys.executable,P+'check_f5_bin.py','--game','--replay'],'分桶生产帧与完整宿主回放',['pack'])
add('research',[sys.executable,P+'check_f5_bin.py','--research'],'冻结研究输入复现')
add('feature-research',[sys.executable,P+'research_engine_features.py','--models','--prepare-synthesis','--blend-models'],
    '可选机制配置、编译守卫与混合核研究；不代表裁剪整机验收')
add('feature-netlist',[sys.executable,P+'feature_netlist.py','--tree-root','Main/sim/build/feature-trees',
                       '--out','Main/sim/build/feature-trees/netlist.json'],
    '按构型的 netlist 结构检查：关闭的机制不得在综合网表里留下信号（Windows，先跑综合）')
add('feature-caps',[sys.executable,P+'check_feature_caps.py'],'特性清单与两份生成物的 CAPS 同源一致')
add('feature-elab',[sys.executable,P+'check_feature_elab.py'],
    '七代表档与合法/非法结构组合的配置穷举 elaboration（自建源树）')
add('feature-probe',[sys.executable,P+'check_feature_probe.py'],
    '关闭的机制离开编译输入：实例化点与源码内容两种判据')
add('feature-contract',[sys.executable,P+'check_feature_contract.py'],
    '按构型的禁用契约与六条依赖守卫反例')
add('feature-function',[sys.executable,P+'check_feature_function.py'],
    '160 个合法构型的真实绘制、关闭态错误三元组、原子拒绝与六个定向变异')
add('feature-synthesis',[sys.executable,P+'check_feature_synthesis.py'],
    '本轮七代表档整机四步、全时钟时序与网表结构（Windows）',timeout=18000)
add('feature-board',[sys.executable,P+'check_feature_board.py'],
    '同一 BIN 的七代表档部署与独立 CRC 功能板测（Windows）',['feature-synthesis'],timeout=3600)
add('ddr-log',[sys.executable,P+'check_f5_bin.py','--ddr-log'],'DDR 日志 sink 的字节／CRC／溢出边界')
add('synthesis',[sys.executable,P+'run_f5_synthesis.py','--build','main','--collect'],'整机编译与资源时序（Windows）')
add('synthesis-report',[sys.executable,P+'run_f5_synthesis.py','--collect','--projects','main'],'已有整机报告的全时钟关系复核（Windows）')
add('board',[sys.executable,P+'check_board_smoke.py'],'部署、调试与引擎功能采集（Windows）')
add('board-game',[sys.executable,P+'check_board_game.py'],'当前位流的生产 HW/DDR 游戏 240 帧（Windows）',timeout=1200)

# 输入模式是保守依赖范围；按工作点复核，可在此收窄或补齐消费者。
COMMON=['Main/tools/check/check_run.py','Main/tools/check/validate.py']
SOFTWARE=['Main/sw/**','Main/sdk/**','Main/assets/**','Main/tools/assets/**','Main/tools/check/**']
RTL_INPUTS=['Main/rtl/**','Main/sim/**','Main/sw/**','Main/assets/**','Main/tools/check/**']
HARDWARE=['Main/rtl/**','Main/fpga/**','Main/sdk/**','Main/sw/**','Main/tools/board/**','Main/tools/check/**']
for name,item in CHECKS.items():
    if name=='paths':inputs=['@tracked-text']
    elif name=='workflow':inputs=['Main/tools/check/**','Main/sw/lib/test/golden.txt']
    elif name=='regs':inputs=[P+'regs_gen.py','Main/rtl/engine/engine_regs_offsets.vh','Main/sw/bsp/include/gfx_regs.h']
    elif name=='semantics':inputs=[P+'check_cmd_semantics.py','Main/doc/赛题二总体设计.md']
    elif name=='rast':inputs=SOFTWARE + ['Main/doc/三维数值契约.md']
    elif name=='ddr-log':inputs=[P+'check_f5_bin.py','Main/sw/bsp/include/board_ddr_log.h','Main/sim/engine/frontend/test_ddr_log.c']
    elif name.startswith('feature-'):
        # 特性构型的判据同时依赖 RTL、配置真源、构建入口与仿真侧脚本。
        inputs=['Main/rtl/**','Main/sim/engine/features/**',
                P+'feature_config.py',P+'feature_build.py',P+'feature_netlist.py',
                P+'check_feature_caps.py',P+'check_feature_elab.py',
                P+'check_feature_probe.py',P+'check_feature_contract.py',P+'check_feature_function.py',
                P+'check_feature_synthesis.py',P+'feature_synthesis.py',P+'check_feature_board.py',
                P+'regs_gen.py','Main/sw/bsp/include/gfx_feature.h',
                'Main/sw/apps/test/featureTest/**','Main/sw/bsp/**','Main/sw/mk/**','Main/sdk/**',
                'Main/tools/board/**','Main/fpga/**','Main/sw/lib/**',P+'check_board_smoke.py']
    elif name in ('synthesis','synthesis-report','board','board-game'):inputs=HARDWARE
    elif name in ('lib','app','layers','pc','assets','demo','game','stats','pack'):inputs=SOFTWARE
    else:inputs=RTL_INPUTS
    item['inputs']=list(dict.fromkeys(COMMON+inputs))


def expand(selected):
    result = []
    def visit(name):
        if name not in CHECKS:
            raise ValueError('未知检查项: '+name)
        if name in result:
            return
        for dependency in CHECKS[name]['dependencies']:
            visit(dependency)
        result.append(name)
    for name in selected:
        visit(name)
    return result


def suggest(changed):
    selected = ['paths']
    for path in changed:
        name = path.replace('\\','/')
        if name.endswith('.md') or name in ('.gitignore','.gitattributes'):
            continue
        recognized=False
        if name.startswith('Main/tools/check/'):
            recognized=True
            selected.append('workflow')
            if Path(name).name in ('rast_model.py','rast_check.c','gen_recip_tab.py'):
                selected.append('rast')
            if Path(name).name in ('check_common.sh','check_host.sh'):
                selected += ['lib','app','game','stats']
            for key, value in CHECKS.items():
                if any(arg == name for arg in value['command']):
                    selected.append(key)
        if name.startswith('Main/sw/lib/'):
            recognized=True
            selected.append('lib')
            if '/rast_' in name or 'test_rast' in name or 'rast_golden' in name or 'recip_tab' in name:
                selected.append('rast')
            elif '/gfx/' in name:selected += ['instances','bin','stripctl']
            if any(x in name for x in ('fixmath','sin_tab')):selected += ['app','game']
        if name.startswith('Main/sw/apps/test/rastTest/'):
            recognized=True
            selected.append('rast')
        if name.startswith('Main/sw/apps/game/'):
            recognized=True
            selected += ['app','game']
            if any(x in name for x in ('renderer','modes','stats','press','auto','chord','main.c')):
                selected.append('stats')
            if 'renderer' in name:selected += ['frontend','instances','bin']
        if name.startswith(('Main/sw/bsp/','Main/sw/mk/')):
            recognized=True
            selected += ['layers','pc','demo','game','stats']
        if name.startswith(('Main/assets/','Main/tools/assets/')):
            recognized=True
            selected += ['assets','demo','game']
        if name.startswith('Main/rtl/engine/'):
            recognized=True
            part = Path(name).stem.removeprefix('engine_')
            selected.append(part if part in CHECKS else 'ctrl')
            selected += ['frame','instances','bin','synthesis']
        if name.startswith(('Main/rtl/top/','Main/rtl/bus/','Main/fpga/','Main/sdk/')):
            recognized=True
            selected += ['axi_mux_guard','display','frame','instances','bin','synthesis','board']
        if name.startswith(('Main/rtl/display/','Main/rtl/hdmi/')):
            recognized=True
            selected += ['display' if '/display/' in name else 'hdmi','synthesis','board']
        if name.startswith('Main/rtl/vendor/'):
            recognized=True
            selected += ['mover','pix','frame','band','stripctl','instances','bin','synthesis','board']
        if name.startswith('Main/tools/board/'):
            recognized=True
            selected += ['workflow','board']
        if name.startswith('Main/sim/'):
            recognized=True
            selected += ['instances','bin'] if '/frontend/' in name else ['stripctl'] if '/stripctl/' in name else ['frame']
        if name.endswith('axi_ram_model.v'):
            selected += ['axi_probe','axi_mux_guard','display','mover','pix','ctrl','frame','band','stripctl','instances','bin']
        if not recognized:raise ValueError('未映射的改动需显式选择 --checks: '+name)
    return expand(selected)


def portable(value):
    if isinstance(value,dict):return {key:portable(item) for key,item in value.items()}
    if isinstance(value,list):return [portable(item) for item in value]
    if not isinstance(value,str):return value
    roots={str(ROOT),ROOT.as_posix()}
    match=re.match(r'/mnt/([a-z])/(.*)',ROOT.as_posix())
    if match:
        windows=match[1].upper()+':/'+match[2]
        roots.update([windows,windows.replace('/','\\')])
    else:
        match=re.match(r'([A-Za-z]):/(.*)',ROOT.as_posix())
        if match:roots.add('/mnt/'+match[1].lower()+'/'+match[2])
    for prefix in sorted(roots,key=len,reverse=True):value=value.replace(prefix,'<REPO>')
    value=re.sub(r'/home/[^/]+/','<HOME>/',value)
    value=re.sub(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/][^\s'\"\],]+",lambda match:'<HOST_PATH>/'+PureWindowsPath(match[0]).name,value)
    if PureWindowsPath(value).is_absolute():value='<HOST_PATH>/'+PureWindowsPath(value).name
    return value


def archive(current, destination, snapshot=False):
    target = (ROOT / destination).resolve()
    if not target.is_relative_to(ROOT.resolve()) or target.exists():
        raise ValueError('归档目标必须是仓库内尚不存在的目录')
    data=json.loads((current.path/'run.json').read_text(encoding='utf-8'))
    if any(item['status']=='RUNNING' for item in data['checks'].values()):raise ValueError('运行尚未结束，不能归档')
    if not snapshot:
        result=json.loads((current.path/'result.json').read_text(encoding='utf-8'))
        current.require(['check:'+name for name in result['checks']])
    # 摘要的消费者是归档后的字节比较；源对象登记一次，复制后验证目标。
    originals = {p.relative_to(current.path).as_posix(): identity(p)
                 for p in current.path.rglob('*') if p.is_file() and 'logs' in p.relative_to(current.path).parts}
    write_json(current.path/'archive-files.json', originals)
    target.mkdir(parents=True)
    for filename in ['run.json','plan.json','result.json','archive-files.json']:
        if (current.path/filename).exists():
            write_json(target/filename,portable(json.loads((current.path/filename).read_text(encoding='utf-8'))))
    shutil.copytree(current.path/'logs',target/'logs')
    for name, expected in originals.items():
        if identity(target/name)['sha256'] != expected['sha256']:
            raise RuntimeError('归档复制不一致: '+name)
    write_json(target/'archive.json',{'kind':'historical_run_snapshot' if snapshot else 'current_run',
        'current_inputs_revalidated':not snapshot,'local_paths':'placeholders in metadata; raw logs byte-identical',
        'source_run':current.path.relative_to(ROOT).as_posix()})
    print('ARCHIVE '+('SNAPSHOT ' if snapshot else 'PASS ')+target.relative_to(ROOT).as_posix())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list',action='store_true')
    parser.add_argument('--checks',nargs='+',choices=CHECKS)
    parser.add_argument('--changed',nargs='*',help='按路径建议范围；省略路径时读取工作区差异')
    parser.add_argument('--profile',choices=['quick','acceptance','release'],default='quick')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--run-dir')
    parser.add_argument('--collect',action='store_true')
    parser.add_argument('--archive')
    parser.add_argument('--snapshot',action='store_true',help='仅归档运行当时的状态，不重判当前源码，不生成新的通过结论')
    parser.add_argument('--fresh-inputs',action='store_true',help='保留时间戳的复制／外部改写后强制复核输入字节')
    args=parser.parse_args()
    if args.list:
        for name,item in CHECKS.items(): print(name+'\t'+item['purpose'])
        return
    if args.collect or args.archive:
        if not args.run_dir:parser.error('汇总／归档必须提供 --run-dir')
        if args.snapshot:
            if not args.archive or args.collect or args.run:parser.error('--snapshot 只用于 --archive')
            path=(ROOT/args.run_dir).resolve()
            if not path.is_relative_to(RUNS.resolve()) or path==RUNS.resolve():parser.error('不是运行目录')
            archive(SimpleNamespace(path=path),args.archive,True)
            return
        current=Session(args.run_dir,args.profile,fresh=args.fresh_inputs)
        plan=json.loads((current.path/'plan.json').read_text(encoding='utf-8'))
        current.require(['check:'+name for name in plan['checks']])
        write_json(current.path/'result.json',{'status':'PASS','profile':current.profile,'checks':plan['checks'],'run_id':current.path.name})
        if args.archive:archive(current,args.archive)
        print('COLLECT PASS scope='+current.profile)
        return
    changed=args.changed
    if changed==[]:
        changed=git('diff','--name-only','HEAD').decode('utf-8').splitlines()
        changed+=git('ls-files','--others','--exclude-standard').decode('utf-8').splitlines()
    selected=expand(args.checks) if args.checks else suggest(changed) if changed is not None else []
    if not selected:parser.error('提供 --checks 或 --changed；--list 查看唯一清单')
    plan={'profile':args.profile,'checks':selected,'changed':changed,
          'commands':{name:CHECKS[name]['command'] for name in selected},
          'timeouts':{name:CHECKS[name]['timeout'] for name in selected},
          'input_patterns':list(dict.fromkeys(pattern for name in selected for pattern in CHECKS[name]['inputs']))}
    for name in selected:print('PLAN '+name+' '+CHECKS[name]['purpose'])
    print('SCOPE '+args.profile+'；计划按影响范围复核后加 --run 执行')
    if not args.run:return
    current=Session(args.run_dir,args.profile,plan['input_patterns'],args.fresh_inputs)
    if (current.path/'plan.json').exists():
        old=json.loads((current.path/'plan.json').read_text(encoding='utf-8'))
        if old['checks']!=selected or old['commands']!=plan['commands']:raise RuntimeError('本轮检查范围／命令已改变，请使用新运行目录')
    write_json(current.path/'plan.json',plan)
    print('RUN '+current.path.relative_to(ROOT).as_posix(),flush=True)
    for name in selected:
        print('START '+name,flush=True)
        def execute(name=name):
            log=current.logs('runner')/(name+'.log')
            output=command(CHECKS[name]['command'],log,timeout=CHECKS[name]['timeout'])
            return {'log':log.relative_to(ROOT).as_posix(),
                    'not_applicable':re.findall(r'^NOT_APPLICABLE .*$',output,re.M)}
        current.perform('check:'+name,execute,reusable=name not in ('board','feature-board','board-game'))
        print('PASS '+name,flush=True)
    current.require(['check:'+name for name in selected])
    write_json(current.path/'result.json',{'status':'PASS','profile':current.profile,'checks':selected,'run_id':current.path.name})
    print('VALIDATION PASS scope='+current.profile+' checks='+str(len(selected)),flush=True)


if __name__=='__main__':
    try:main()
    except IncompleteCheck as error:print('INCOMPLETE '+str(error),file=sys.stderr);sys.exit(3)
    except (RuntimeError,ValueError,subprocess.CalledProcessError) as error:
        print('FAIL '+str(error),file=sys.stderr);sys.exit(1)
