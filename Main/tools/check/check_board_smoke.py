#!/usr/bin/env python3
"""验证本轮部署、两次调试连接与引擎功能；不写 Flash、不测新性能。"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from check_run import IncompleteCheck, ROOT, command, session, write_json


def uart_port():
    explicit=os.environ.get('GFX_CHECK_PORT')
    if explicit:return explicit
    import winreg
    from serial.tools import list_ports
    present={port.device for port in list_ports.comports()}
    found=[]
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'SYSTEM\CurrentControlSet\Enum\FTDIBUS') as parent:
        for i in range(winreg.QueryInfoKey(parent)[0]):
            name=winreg.EnumKey(parent,i)
            # FT4232H C 是第三个子接口；COM 号随 USB 重新枚举变化。
            if not name.upper().startswith('VID_0403+PID_6011+') or not name.upper().endswith('&3'):
                continue
            try:
                with winreg.OpenKey(parent,name+r'\0000\Device Parameters') as parameters:
                    port=winreg.QueryValueEx(parameters,'PortName')[0]
                    if port in present:found.append(port)
            except FileNotFoundError:continue
    if len(found)!=1:raise IncompleteCheck('不能唯一定位 FT4232H C 通道，请设置 GFX_CHECK_PORT')
    return found[0]


def validate_engine(body):
    boundary='# --- 程序已加载，以下才是它的输出 ---'
    if boundary not in body:raise RuntimeError('缺少本次程序加载边界')
    body=body.split(boundary,1)[1]
    # 横幅可能早于 OpenOCD 返回；本次身份由加载证据及边界后的完整功能集合共同确定。
    # CAPS-BACKEND-PAIR 是本工作点新增的判据：能力位与默认后端必须分开断言，
    # 缺了它「引擎能分桶但默认不走分桶」这条就没有板上证据。
    for marker in ['COUNT32 ok', 'TEX-CAPS ok', 'CAPS-BACKEND-PAIR ok', 'TEX-LOAD ok',
                   'PERF-BUSY ok', 'STRESS ok']:
        if marker not in body:raise RuntimeError('缺少引擎功能判据: '+marker)
    lines=[line.strip() for line in body.splitlines()]
    if [line for line in lines if re.fullmatch(r'fail \d+',line)]!=['fail 0'] or 'done' not in lines:
        raise RuntimeError('程序未正常结束')
    if '# serial lost' in body or re.search(r'\bFAIL\b',body):raise RuntimeError('串口或功能判据失败')
    golden=dict(line.split() for line in (ROOT/'Main/sw/lib/test/golden.txt').read_text().splitlines() if line.strip())
    expected={**golden,**{'TEX-'+name:crc for name,crc in golden.items()}}
    rows=re.findall(r'^(\S+) ([0-9a-f]{8}) ([0-9a-f]{8}) ok$',body,re.M)
    if len(rows)!=len(expected) or {name for name,_,_ in rows}!=set(expected):
        raise RuntimeError('DDR/TEX 金标准行数或唯一性不符')
    if any(a!=b or a!=expected[name] for name,a,b in rows):raise RuntimeError('DDR/TEX CRC 不符合独立 golden.txt')
    return {'status':'PASS','crc_cases':len(rows),'scope':'engine functional smoke; no new performance claim'}


def main():
    current=session()
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    args=parser.parse_args()
    if os.name!='nt':raise IncompleteCheck('板测入口使用 Windows 的 Efinity Python')
    logs=current.logs('board')
    installation=Path(os.environ.get('EFINITY_HOME') or Path(sys.executable).parents[2])
    pgm=installation/'pgm/bin/ftdi_pgm.bat'
    if not pgm.exists():raise IncompleteCheck('未找到 Efinity Programmer')
    scan_env=dict(os.environ)
    for key in ['EFINITY_HOME','PYTHONHOME','EFXPT_HOME','EFXPGM_HOME','EFXDBG_HOME','EFXIPM_HOME',
                'EFXSVF_HOME','EFXSERDESDBG_HOME','EFINITY_USER_DIR_INI']:
        scan_env.pop(key,None)
    scan=subprocess.run([str(pgm),'--scan_usb'],env=scan_env,capture_output=True,text=True,errors='replace')
    scan_text=scan.stdout+scan.stderr
    (logs/'usb.log').write_text(scan_text,encoding='utf-8')
    if 'Traceback' in scan_text:raise RuntimeError('Programmer 初始化失败，见 usb.log')
    if '10660a79' not in scan_text.lower():
        raise IncompleteCheck('JTAG 未识别 Ti60；检查 FT4232H B / Interface 1 的 libusbK 驱动；见 usb.log')
    port=args.port or uart_port()
    bit=ROOT/'Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    if not bit.exists():raise IncompleteCheck('缺少已编译位流')
    inputs=[* (ROOT/'Main/rtl').rglob('*.v'),* (ROOT/'Main/rtl').rglob('*.vh'),
            * (ROOT/'Main/fpga/gfx2d_ti60/sdc').glob('*.sdc')]
    if max(p.stat().st_mtime_ns for p in inputs)>bit.stat().st_mtime_ns:
        raise IncompleteCheck('RTL／头文件／时序约束晚于位流，先综合')
    timing=(bit.parent/'gfx2d_ti60.timing.rpt').read_text(encoding='utf-8')
    section=timing[timing.index('Setup (Max) Clock Relationship'):timing.index('NOTE: Values',timing.index('Setup (Max) Clock Relationship'))]
    slacks=[float(x) for x in re.findall(r'^\S+\s+\S+\s+[-\d.]+\s+([-\d.]+)\s+\(',section,re.M)]
    if not slacks or min(slacks)<0:raise RuntimeError('位流缺少有效非负时序')
    board=ROOT/'Main/tools/board/board_run.py'
    current.artifact(bit)
    current.perform('board.debug',lambda:command([sys.executable,board,'--check-only','--bit',bit],logs/'debug.log'),reusable=False)
    # 构建在 WSL 内解析交叉工具链；不依赖旧 build/ 中的二进制。
    build_script='prefix="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; test -n "$prefix" || exit 3; make -C Main/sw/apps/test/engineTest -j8 OBJDIR=build_check_smoke RISCV_BIN="${prefix%gcc}"'
    current.perform('board.build',lambda:command(['wsl.exe','-e','bash','-c',build_script],logs/'build.log'))
    binary=ROOT/'Main/sw/apps/test/engineTest/build_check_smoke/engineTest-O2.bin'
    current.artifact(binary)
    def capture():
        command([sys.executable,board,'--no-program','--bin',binary,'--port',port,
            '--stop-line','done','--seconds','180','--log',logs/'engine.log'],logs/'control.log',timeout=240)
        result=validate_engine((logs/'engine.log').read_text(encoding='utf-8-sig',errors='replace'))
        control=(logs/'control.log').read_text(encoding='utf-8',errors='replace')
        if 'Target successfully examined' not in control or str(binary.stat().st_size)+' bytes written at address' not in control:
            raise RuntimeError('缺少本次目标识别／写入字节数')
        result.update({'port':port,'bitstream':bit.relative_to(ROOT).as_posix(),'binary':binary.relative_to(ROOT).as_posix()})
        write_json(logs/'result.json',result)
    current.perform('board.engine',capture,reusable=False)
    print('BOARD PASS port='+port+' run='+current.path.relative_to(ROOT).as_posix(),flush=True)


if __name__=='__main__':
    try:main()
    except IncompleteCheck as error:print('INCOMPLETE '+str(error),file=sys.stderr);sys.exit(3)
