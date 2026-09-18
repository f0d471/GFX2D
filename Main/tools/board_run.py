#!/usr/bin/env python3
"""在板上跑一个程序并回收串口输出：烧比特流 -> 开串口 -> 加载程序 -> 打印输出。

## 为什么需要它

`doc/开发环境.md` 0.6 节的上板顺序有四步，其中两个坑反复踩：

1. **必须先开串口再加载程序。** 串口没打开期间板子发出的数据会被直接丢弃，
   驱动不缓存。程序跑完再打开串口，一个字节都读不到。
2. **OpenOCD 的两个官方 cfg 在这块板上用不了。** `debug.cfg` 与 `debug_ti.cfg` 的
   `cpu0.yaml` 查找是写死往上 3 级目录，而仓库里 `sdk/cpu0.yaml` 离实际运行目录有 5 级；
   且 `debug.cfg` 的 `jtag newtap ... -irlen 4` 与本板的 IR 宽度 5 不符，
   会报 `IR capture error at bit 4, saw 0x25 not 0x..3`。
   本脚本用 `openocd/gfx2d_debug.cfg`（本目录内），把 workspace 路径与 IR 宽度都写对。

把这三步合成一条命令，省掉每次手工串起来、以及忘记先开串口的返工。

## 用法

    <EFINITY_PYTHON> tools/board_run.py --repo <仓库根> --bin <程序.bin> [--log <日志>]

常用参数：

    --bit <比特流>     指定要烧的比特流（默认烧 <工程>/outflow/gfx2d_ti60.bit）
    --no-program      跳过烧写，直接跑
    --port COM11      串口号，默认 COM11（`doc/开发环境.md` 第 11 节的参考值）
    --seconds 60      串口采集时长上限
    --stop-on <文本>  收到该文本就提前结束采集
    --check-only      只做「连接 + 读寄存器」的连通性检查，不加载程序
    --workspace <目录> OpenOCD 找 cpu0.yaml 的目录，默认 <repo>/Main/sdk
    --openocd <路径>  直接指定 openocd.exe；不给则按 EFINITY_IDE_HOME 与常见布局找

仓库不写绝对路径，所以 Efinity 与 IDE 的位置按环境变量与 PATH 解析：
`EFINITY_HOME`、`EFINITY_IDE_HOME`，或 PATH 里的 `efx_run.bat`；
也可以用 `--efinity` / `--ide` / `--openocd` 显式给出。

## 退出码

0 表示 OpenOCD 成功考察到目标；1 表示连接失败或超时，2 表示参数或环境有问题。

## 已验证范围

2026-09-17 在板上验证过三条路径：`--list-ports` 列出 FT4232H 的三路 VCP；
`--check-only` 连续两次会话各读一次 PC，都 `Target successfully examined`；
完整的「先开串口 + `load_image` + `resume` + 回收输出」用 `sw/ddrAliasTest` 跑通，
串口收到与 `sw/README.md` 登记一致的输出（`LOW k=0..27`、`HIGH k=28..31` 全 `ok`、
`base 0x0c800000 fail 0`、末尾 `done`）。

未验证：`--jtag-url` 换别的下载器、`--efinity` / `--ide` 显式指定路径
（本机的 Efinity 在 PATH 里，走的是自动查找那条分支）。
"""
import argparse
import glob
import os
import serial
import serial.tools.list_ports
import subprocess
import sys
import threading
import time


def find_efinity():
    """定位 Efinity 安装目录。

    仓库不写绝对路径（见仓库根 README），所以按与环境有关的两条路子找：
    `EFINITY_HOME` 环境变量，或 PATH 里能搜到 `efx_run.bat`
    （安装时会把 `<EFINITY>\\bin` 加进 PATH，`doc/开发环境.md` 第 5 节用它编译）。
    """
    for key in ("EFINITY_HOME", "EFINITY_ROOT"):
        v = os.environ.get(key)
        if v and os.path.isdir(v):
            return v
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if d and os.path.isfile(os.path.join(d, "efx_run.bat")):
            return os.path.dirname(d)
    return None


def find_openocd(efinity=None, ide_root=None, extra=None):
    """定位 openocd.exe。

    OpenOCD 随 **Efinity RISC-V IDE** 安装，与 Efinity 主目录是两个目录
    （`doc/开发环境.md` 第 1 节把它们分成 `<EFINITY>` 与 `<IDE>`），
    版本号也未必一致，所以不能从 Efinity 版本推。查找顺序：
    显式参数 -> 环境变量 `<IDE>_HOME` 风格的两三个名字 -> IDE 默认装在
    Efinity 的上一级时的通配匹配。
    """
    if extra and os.path.isfile(extra):
        return extra
    for key in ("EFINITY_IDE_HOME", "EFINITY_RISCV_IDE_HOME", "IDE_HOME"):
        v = os.environ.get(key)
        if v:
            cand = os.path.join(v, "openocd", "bin", "openocd.exe")
            if os.path.isfile(cand):
                return cand
    bases = [b for b in (ide_root, os.path.dirname(os.path.abspath(efinity)) if efinity else None)
             if b and os.path.isdir(b)]
    for base in bases:
        for pat in ("efinity-riscv-ide-*/openocd/bin/openocd.exe",
                    "*/openocd/bin/openocd.exe",
                    "openocd/bin/openocd.exe"):
            hits = sorted(glob.glob(os.path.join(base, pat)))
            if hits:
                return hits[-1]          # 版本号大的排在后面
    return None


def list_ports():
    print("当前串口：")
    for p in serial.tools.list_ports.comports():
        print("  %-8s %s" % (p.device, p.description))
    print("\n本板的 FT4232H 三路 VCP 都会出现；SoC 的 UART 走通道 C（Interface 3），"
          "\ndoc/开发环境.md 第 11 节记的参考值是 COM11。")


def openocd_cmd(ocd, cfg, workspace):
    env = dict(os.environ)
    env["workspace_loc"] = workspace
    env["CPUTAPID"] = "0x10660A79"
    return ocd, cfg, env


def program_bitstream(pgm, bit, jtag_url):
    """烧写比特流，返回 (是否成功, 输出文本)。

    ftdi_pgm.bat 只在 `EFINITY_HOME` 未定义时才自己去调 `bin\\setup.bat`。
    父进程里设过 `EFINITY_HOME`、却没设 `PYTHONHOME` 之类时，它跳过 setup.bat，
    内部的 python 以 `Fatal Python error: init_fs_encoding`（或 `KeyError: EFXPGM_HOME`）
    退出，**退出码仍然是 0**。所以这里做两件事：

    1. 把 Efinity 的环境变量从子进程环境里去掉，让厂商脚本自己配好；
    2. 不只看退出码，还要求输出里出现成功标记 `finished with JTAG programming`。

    2026-09-19 复核时发现：脚本据退出码把失败的烧写当成功，
    接着用 FPGA 里原有的比特流跑程序，串口输出看不出任何异常。
    """
    env = dict(os.environ)
    for k in ("EFINITY_HOME", "PYTHONHOME", "EFXPGM_HOME", "EFXPT_HOME",
              "EFXDBG_HOME", "EFXIPM_HOME", "EFXSVF_HOME", "EFXSERDESDBG_HOME",
              "EFINITY_USER_DIR_INI"):
        env.pop(k, None)
    try:
        p = subprocess.run([pgm, "-m", "jtag", "-u", jtag_url, bit],
                           env=env, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return False, "[TIMEOUT] 烧写超过 180 秒"
    out = (p.stdout or "") + (p.stderr or "")
    ok = (p.returncode == 0) and ("finished with JTAG programming" in out)
    return ok, out


def run_openocd(ocd, cfg, workspace, cmds, timeout=90):
    """跑一次 OpenOCD，返回 (是否考察到目标, 输出文本)。超时则杀进程。"""
    env = dict(os.environ)
    env["workspace_loc"] = workspace
    env["CPUTAPID"] = "0x10660A79"
    full = [ocd, "-f", cfg]
    for c in cmds:
        full += ["-c", c]
    try:
        p = subprocess.run(full, env=env, capture_output=True, text=True, timeout=timeout)
        out = (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes)
               else (e.stdout or "")) + "\n[TIMEOUT %ds]" % timeout
    ok = "Target successfully examined" in out
    return ok, out


def serial_worker(port, baud, out_path, seconds, stop_on, ready, result):
    """先开串口再让主线程去加载程序；读到的内容边读边写文件。"""
    try:
        sp = serial.Serial(port, baud, timeout=1)
    except Exception as e:                       # 串口打不开要立刻告诉主线程
        result["error"] = str(e)
        ready.set()
        return
    result["opened"] = True
    ready.set()
    buf = []
    deadline = time.time() + seconds
    try:
        with open(out_path, "w", encoding="utf-8", newline="") as f:
            f.write("# %s @ %d 8N1, opened %s\n" % (port, baud, time.strftime("%H:%M:%S")))
            f.flush()
            while time.time() < deadline:
                chunk = sp.read(4096)
                if chunk:
                    text = chunk.decode("utf-8", "replace")
                    f.write(text)
                    f.flush()
                    buf.append(text)
                    if stop_on and stop_on in "".join(buf):
                        break
            f.write("\n# closed %s\n" % time.strftime("%H:%M:%S"))
    finally:
        sp.close()
    result["text"] = "".join(buf)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True, help="仓库根目录（含 Main/）")
    ap.add_argument("--bin", default=None, help="要加载的程序 .bin")
    ap.add_argument("--bit", default=None,
                    help="要烧写的比特流，默认 <repo>/Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit")
    ap.add_argument("--no-program", action="store_true", help="跳过烧写比特流")
    ap.add_argument("--port", default="COM11", help="串口号，默认 COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--jtag-url", default="ftdi://0x0403:0x6011:0:ff/1",
                    help="下载器 URL，默认板载 FT4232H 的通道 B")
    ap.add_argument("--efinity", default=None, help="Efinity 安装目录")
    ap.add_argument("--ide", default=None, help="Efinity RISC-V IDE 安装目录（找 OpenOCD 用）")
    ap.add_argument("--openocd", default=None, help="直接指定 openocd.exe")
    ap.add_argument("--seconds", type=int, default=60, help="串口采集时长上限")
    ap.add_argument("--stop-on", default=None, help="收到该文本就提前结束采集")
    ap.add_argument("--log", default=None, help="串口日志路径，默认写到临时目录")
    ap.add_argument("--workspace", default=None, help="OpenOCD 找 cpu0.yaml 的目录")
    ap.add_argument("--addr", default="0x1000", help="程序加载地址，默认 0x1000")
    ap.add_argument("--check-only", action="store_true", help="只做连通性检查（连两次、读寄存器）")
    ap.add_argument("--list-ports", action="store_true", help="列出串口后退出")
    ap.add_argument("--dry-run", action="store_true", help="只打印将执行的命令")
    args = ap.parse_args()

    if args.list_ports:
        list_ports()
        return 0

    repo = os.path.abspath(args.repo)
    main_dir = os.path.join(repo, "Main")
    if not os.path.isdir(main_dir):
        print("ERROR: %s 下没有 Main/，--repo 要指向仓库根" % repo, file=sys.stderr)
        return 2

    workspace = args.workspace or os.path.join(main_dir, "sdk")
    cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "openocd", "gfx2d_debug.cfg")
    if not os.path.isfile(cfg):
        print("ERROR: 缺 %s" % cfg, file=sys.stderr)
        return 2

    efinity = args.efinity or find_efinity()
    ocd = find_openocd(efinity, args.ide, args.openocd)
    if not ocd:
        print("ERROR: 找不到 openocd.exe。它随 Efinity RISC-V IDE 安装，"
              "用 --openocd 指定路径，或设 EFINITY_IDE_HOME 环境变量", file=sys.stderr)
        return 2
    pgm = os.path.join(efinity, "pgm", "bin", "ftdi_pgm.bat") if efinity else None

    bit = args.bit or os.path.join(main_dir, "fpga", "gfx2d_ti60", "outflow",
                                   "gfx2d_ti60.bit")
    log = args.log or os.path.join(os.environ.get("TEMP", "."), "board_run.log")

    if args.dry_run:
        print("OpenOCD    : %s" % ocd)
        print("配置       : %s" % cfg)
        print("workspace  : %s" % workspace)
        if not args.no_program:
            print("烧写       : %s -m jtag -u <JTAG> %s" % (pgm, bit))
        print("串口       : %s @ %d -> %s" % (args.port, args.baud, log))
        if args.check_only:
            print("命令       : reg pc; exit   （连两次）")
        else:
            print("命令       : load_image {%s} %s bin; resume %s; shutdown"
                  % (args.bin, args.addr, args.addr))
        return 0

    if not args.no_program:
        if not bit or not os.path.isfile(bit):
            print("ERROR: 找不到比特流 %s" % bit, file=sys.stderr)
            return 2
        if not pgm or not os.path.isfile(pgm):
            print("ERROR: 找不到 ftdi_pgm.bat（Efinity 的 pgm/bin 下）", file=sys.stderr)
            return 2
        print("=== 1) 烧写比特流 %s" % bit)
        ok, out = program_bitstream(pgm, bit, args.jtag_url)
        if not ok:
            print(out)
            print("ERROR: 烧写失败。注意 ftdi_pgm.bat 出错时也可能返回 0，"
                  "所以这里要求输出里出现成功标记。", file=sys.stderr)
            return 1

    if args.check_only:
        print("=== 连通性检查：连续两次会话各读一次 PC")
        for i in (1, 2):
            ok, out = run_openocd(ocd, cfg, workspace, ["reg pc", "exit"], timeout=60)
            print("  第 %d 次：%s" % (i, "OK" if ok else "FAIL"))
            if not ok:
                print(out)
                return 1
        print("两次会话都成功，调试通路可用。")
        return 0

    if not args.bin:
        print("ERROR: 要加载程序就要给 --bin", file=sys.stderr)
        return 2
    if not os.path.isfile(args.bin):
        print("ERROR: 找不到程序 %s" % args.bin, file=sys.stderr)
        return 2

    print("=== 2) 先开串口 %s，再加载程序（顺序不能反）" % args.port)
    ready = threading.Event()
    result = {}
    th = threading.Thread(target=serial_worker,
                          args=(args.port, args.baud, log, args.seconds,
                                args.stop_on, ready, result))
    th.daemon = True
    th.start()
    ready.wait(timeout=10)
    if result.get("error"):
        print("ERROR: 打不开串口 %s：%s" % (args.port, result["error"]), file=sys.stderr)
        return 2

    print("=== 3) load_image + resume")
    cmds = ["load_image {%s} %s bin" % (args.bin, args.addr),
            "resume %s" % args.addr, "shutdown"]
    ok, out = run_openocd(ocd, cfg, workspace, cmds, timeout=90)
    for line in out.splitlines():
        if any(k in line for k in ("examined", "communicate", "Downloading",
                                   "shutdown", "not examined", "TIMEOUT")):
            print("   " + line.strip())
    if not ok:
        print("WARN: 没有看到 'Target successfully examined'，加载可能没生效")

    print("=== 4) 等串口采集结束（上限 %d 秒）" % args.seconds)
    th.join(timeout=args.seconds + 30)
    print("--- 串口输出（%s）---" % log)
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            sys.stdout.write(f.read())
    except OSError as e:
        print("读日志失败：%s" % e)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
