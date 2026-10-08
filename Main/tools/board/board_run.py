#!/usr/bin/env python3
"""上板一条命令：烧比特流 -> 开串口 -> 复位并加载程序 -> 回收串口输出。

## 为什么需要它

`doc/开发环境.md` 0.6 节的上板顺序里有三个坑反复踩：

1. **必须先开串口再加载程序。** 串口没打开期间板子发出的数据会被直接丢弃，
   驱动不缓存。程序跑完再打开串口，一个字节都读不到。
   代价是这期间板子会从 Flash 自启动跑上一次固化的程序，它打的 `done`、`fail 0`
   混在同一条串口上。所以 `--stop-on` 只从 `load_image` + `resume` 之后开始匹配，
   加载前的那一段照样进日志、但不作数（2026-09-21 踩到：Flash 里是 `bootSizeTest`，
   `--stop-on done` 当场命中，退出码 0，而要跑的程序一行都还没输出）。
2. **加载前必须 `reset halt`。** 不复位时前一次实验可能把总线留在坏状态，
   `load_image` 照样报成功，程序却一行都不跑。
3. **OpenOCD 的两个官方 cfg 在这块板上用不了**，本目录的 `gfx2d_debug.cfg` 改好了，原因写在它的头部。

## 四种用法

    <EFINITY>\\python311\\bin\\python.exe tools\\board\\board_run.py --bin <程序.bin> --stop-on done
    ...  --check-only                          调试通路检查：连开两次会话各读一次 PC
    ...  --cmd "mdw 0xf8100000 3" [--cmd ...]  不加载程序，复位后执行 OpenOCD 命令（读写寄存器）
    ...  --list-ports                          列串口

除 `--list-ports` 外都会先烧比特流，`--no-program` 跳过。
解释器要设 `PYTHONHOME=<EFINITY>\\python311`，原因见 `tools/README.md`。

## 工具位置

仓库不写绝对路径。Efinity 目录依次取 `--efinity`、`EFINITY_HOME`、PATH 里的 `efx_run.bat`，
最后从当前解释器推（用 Efinity 自带的 python 运行时，它就在 `<EFINITY>\\python311\\bin\\` 下）。
`openocd.exe` 依次取 `--openocd`、`EFINITY_IDE_HOME`、`--ide`、与 Efinity 同级的 `efinity-riscv-ide-*`。

## 退出码

0：每一步都有正面证据（烧写输出里有成功标记；OpenOCD 考察到目标、`load_image` 报了写入字节数；
串口收到了数据，给了 `--stop-on` 时还收到了该文本）。1：任何一步缺证据，或采集中途串口失效
（USB 掉线，日志末尾记 `# serial lost`）。2：参数或环境有问题。
退出码不判断程序自己的 `fail N`，那一行要看输出。

## 已验证范围

2026-09-19 在板上跑过：完整流程（`sw/apps/probe/ddrAliasTest`，`fail 0`、`done`，退出码 0）；
串口号填错（0 字节，退出码 1）；`--stop-on` 的文本没出现（退出码 1）；`--check-only`；
`--cmd` 读 APB 探针与 GPIO1。
2026-09-21 又跑过：`sw/apps/test/platformTest`（`--no-program`，`--stop-on "fail ="`，
`fail =0`，退出码 0），并在同一块板上复现了上面第 1 条那个假绿。
未验证：`--jtag-url` 换别的下载器。
"""
import argparse
import codecs
import glob
import os
import re
import subprocess
import sys
import threading
import time

from input_forward import InputForwarder, packet

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.dirname(os.path.dirname(HERE))          # <仓库>/Main

try:
    import serial
    import serial.tools.list_ports
except ImportError:                                      # 只有 Efinity 自带的 python 有 pyserial
    serial = None


def find_efinity(explicit=None):
    cands = [explicit, os.environ.get("EFINITY_HOME")]
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if d and os.path.isfile(os.path.join(d, "efx_run.bat")):
            cands.append(os.path.dirname(d))
    # <EFINITY>/python311/bin/python.exe 往上三级
    cands.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(sys.executable)))))
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "pgm", "bin", "ftdi_pgm.bat")):
            return c
    return None


def find_openocd(efinity=None, ide_root=None, explicit=None):
    """OpenOCD 随 Efinity RISC-V IDE 安装，与 Efinity 主目录是两个目录，版本号也未必一致。"""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    roots = [os.environ.get("EFINITY_IDE_HOME"), ide_root]
    if efinity:
        roots += sorted(glob.glob(os.path.join(os.path.dirname(efinity), "efinity-riscv-ide-*")),
                        reverse=True)                     # 版本号大的优先
    for r in roots:
        if r:
            cand = os.path.join(r, "openocd", "bin", "openocd.exe")
            if os.path.isfile(cand):
                return cand
    return None


def list_ports():
    print("当前串口：")
    for p in serial.tools.list_ports.comports():
        print("  %-8s %s" % (p.device, p.description))
    print("\n本板 FT4232H 的通道 A、C、D 以虚拟串口出现（通道 B 换了 libusbK 驱动做 JTAG）。"
          "\nSoC 的 UART 在通道 C（Interface 2），doc/开发环境.md 第 11 节记的参考值是 COM11。")


def program_bitstream(pgm, bit, jtag_url):
    """烧写比特流，返回 (是否成功, 输出文本)。

    ftdi_pgm.bat 只在 `EFINITY_HOME` 未定义时才自己调 `bin\\setup.bat`。父进程里设过
    `EFINITY_HOME`、却没设齐其余变量时，它跳过 setup.bat，内部 python 以
    `Fatal Python error` 或 `KeyError: EFXPGM_HOME` 退出，**退出码仍然是 0**。
    所以把 Efinity 的环境变量从子进程里去掉让厂商脚本自己配，并且要求输出里有成功标记。
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
    return (p.returncode == 0 and "finished with JTAG programming" in out), out


def run_openocd(ocd, cfg, workspace, cmds, timeout=90):
    """跑一次 OpenOCD，返回 (是否考察到目标, 输出文本)。超时则杀进程。

    `workspace_loc` 用 -c 在 Tcl 里设，不走环境变量：这样从 WSL 调 openocd.exe 时也成立
    （WSL 不把自定义环境变量传给 Windows 进程）。路径用正斜杠，Tcl 会把反斜杠当转义。
    """
    full = [ocd, "-c", "set ::env(workspace_loc) {%s}" % workspace.replace("\\", "/"),
            "-f", cfg]
    for c in cmds:
        full += ["-c", c]
    try:
        p = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
        out = (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired as e:
        raw = e.stdout or b""
        out = (raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw) \
            + "\n[TIMEOUT %ds]" % timeout
    return "Target successfully examined" in out, out


def serial_worker(port, baud, out_path, seconds, stop_on, ready, armed, result,
                  forward_input=False, input_script=None, script_after=None):
    """先开串口再让主线程去加载程序；读到的内容边读边写文件。

    `armed` 是主线程在 load_image + resume 之后置位的事件。它之前收到的字节
    **不参与 `stop_on` 匹配**：串口要先于加载打开（见模块头第 1 条），而烧完比特流
    板子会从 Flash 自启动，跑的是上一次固化的程序。那个程序同样会打 `done`、
    `fail 0` 这类字样，直接匹配就会在新程序还没开始跑的时候报成功退出——
    日志里留下的是另一个程序的输出，而退出码是 0。

    加载前的内容照样写进日志（要能看出板子自启动跑了什么），只是匹配窗口从
    `# --- 程序已加载` 这一行之后才开始。代价是 resume 到 openocd 退出之间
    （不到一秒）新程序的输出也落在窗口外，所以 `stop_on` 要取程序末尾的标记，
    不要取开头的横幅。
    """
    try:
        sp = serial.Serial(port, baud, timeout=0.02 if (forward_input or input_script) else 0.2)
        sp.reset_input_buffer()                   # 不让上一次运行残留的字节混进这一次
    except Exception as e:                        # 串口打不开要立刻告诉主线程
        result["error"] = str(e)
        ready.set()
        return
    ready.set()
    dec = codecs.getincrementaldecoder("utf-8")("replace")   # 多字节字符可能跨两次 read
    text = ""
    nbytes = 0
    match_from = None                             # None 表示还没加载完，先不匹配
    marker_pending = False
    deadline = time.time() + seconds
    forwarder = InputForwarder() if forward_input else None
    script_start = None
    script_pos = 0
    try:
        with open(out_path, "w", encoding="utf-8", newline="") as f:
            f.write("# %s @ %d 8N1, opened %s\n" % (port, baud, time.strftime("%H:%M:%S")))
            while time.time() < deadline:
                if input_script and armed.is_set():
                    if script_start is None and (script_after is None or
                            (match_from is not None and script_after in text[match_from:])):
                        script_start = time.monotonic()
                    if script_start is not None:
                        ms = (time.monotonic() - script_start) * 1000
                        while script_pos < len(input_script) and ms >= input_script[script_pos][0]:
                            sp.write(packet(input_script[script_pos][1]))
                            script_pos += 1
                if forwarder is not None and armed.is_set():
                    forwarder.send(sp, time.monotonic())
                if match_from is None and armed.is_set():
                    match_from = len(text)
                    marker_pending = True
                    if not text or text.endswith("\n"):
                        f.write("# --- 程序已加载，以下才是它的输出 ---\n")
                        marker_pending = False
                chunk = sp.read(4096)
                if not chunk:
                    continue
                nbytes += len(chunk)
                s = dec.decode(chunk)
                if marker_pending and "\n" in s:
                    first, rest = s.split("\n", 1)
                    f.write(first + "\n# --- 程序已加载，以下才是它的输出 ---\n" + rest)
                    marker_pending = False
                else:
                    f.write(s)
                f.flush()
                text += s
                if stop_on and match_from is not None and stop_on in text[match_from:]:
                    result["stopped"] = True
                    break
            f.write("\n# closed %s, %d bytes\n" % (time.strftime("%H:%M:%S"), nbytes))
    except serial.SerialException as e:
        # USB 掉线时串口句柄失效，若不单独记下，主线程只会报成「N 秒内没收到」
        result["read_error"] = str(e)
        with open(out_path, "a", encoding="utf-8", newline="") as f:
            f.write("\n# serial lost %s, %d bytes: %s\n" % (time.strftime("%H:%M:%S"), nbytes, e))
    finally:
        try:
            if forwarder is not None or input_script:
                sp.write(packet(0))
            sp.close()
        except serial.SerialException:
            pass
        result["bytes"] = nbytes
        result["pre_load_bytes"] = 0 if match_from is None else match_from


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=None, help="要加载的程序 .bin")
    ap.add_argument("--addr", default="0x1000", help="程序加载地址，默认 0x1000")
    ap.add_argument("--cmd", action="append", default=[],
                    help="不加载程序，复位后执行这条 OpenOCD 命令；可给多个，按顺序执行")
    ap.add_argument("--check-only", action="store_true", help="只做调试通路检查")
    ap.add_argument("--flash-boot", action="store_true",
                    help="从 Flash 启动：先开串口，再烧比特流；SoC 复位后 bootloader 从 "
                         "Flash 0x380000 把程序拷进 DDR 并运行，本模式不 load_image。"
                         "SW5 的板上装载判据用它（程序与素材包都在 Flash 里）")
    ap.add_argument("--list-ports", action="store_true", help="列出串口后退出")
    ap.add_argument("--bit", default=None,
                    help="要烧的比特流，默认 Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit")
    ap.add_argument("--no-program", action="store_true", help="跳过烧写比特流")
    ap.add_argument("--port", default="COM11", help="串口号，默认 COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--forward-input", action="store_true",
                    help="同一串口会话转发启动时前台窗口的键盘和 XInput 手柄")
    ap.add_argument("--input-script", default=None,
                    help="诊断输入，毫秒:10位按键位图，逗号分隔；如 500:64,550:0")
    ap.add_argument("--input-script-after", default=None,
                    help="诊断输入在本次程序串口输出包含此文本后才开始计时")
    ap.add_argument("--seconds", type=int, default=60, help="串口采集时长上限")
    ap.add_argument("--stop-on", default=None, help="收到该文本就结束采集；没收到时退出码为 1")
    ap.add_argument("--log", default=None, help="串口日志路径，默认 %%TEMP%%/board_run.log")
    ap.add_argument("--jtag-url", default="ftdi://0x0403:0x6011:0:ff/1",
                    help="下载器 URL，默认板载 FT4232H 的通道 B")
    ap.add_argument("--efinity", default=None, help="Efinity 安装目录")
    ap.add_argument("--ide", default=None, help="Efinity RISC-V IDE 安装目录（找 OpenOCD 用）")
    ap.add_argument("--openocd", default=None, help="直接指定 openocd.exe")
    ap.add_argument("--workspace", default=None,
                    help="OpenOCD 找 cpu0.yaml 的目录，默认 Main/sdk")
    ap.add_argument("--dry-run", action="store_true", help="只打印将执行的步骤")
    args = ap.parse_args()

    if args.forward_input and args.input_script:
        ap.error("--forward-input 与 --input-script 只能选一个")
    if args.input_script_after and not args.input_script:
        ap.error("--input-script-after 需要 --input-script")
    script = None
    if args.input_script:
        try:
            script = []
            for item in args.input_script.split(","):
                ms, keys = item.split(":", 1)
                pair = (int(ms, 0), int(keys, 0))
                if pair[0] < 0 or pair[1] < 0 or pair[1] > 0x3ff:
                    raise ValueError(item)
                script.append(pair)
            if any(script[i][0] > script[i + 1][0] for i in range(len(script) - 1)):
                raise ValueError("时间必须递增")
        except ValueError as exc:
            ap.error("--input-script 格式错误: %s" % exc)

    if serial is None and (args.list_ports or not (args.check_only or args.cmd or args.dry_run)):
        print("ERROR: 当前解释器没有 pyserial，用 Efinity 自带的 python 运行", file=sys.stderr)
        return 2
    if args.list_ports:
        list_ports()
        return 0
    if sum(map(bool, (args.bin, args.cmd, args.check_only, args.flash_boot))) != 1:
        print("ERROR: --bin、--cmd、--check-only、--flash-boot 四选一", file=sys.stderr)
        return 2
    if args.bin and not os.path.isfile(args.bin):
        print("ERROR: 找不到程序 %s" % args.bin, file=sys.stderr)
        return 2

    cfg = os.path.join(HERE, "gfx2d_debug.cfg")
    workspace = os.path.abspath(args.workspace or os.path.join(MAIN, "sdk"))
    efinity = find_efinity(args.efinity)
    ocd = find_openocd(efinity, args.ide, args.openocd)
    if not ocd:
        print("ERROR: 找不到 openocd.exe。它随 Efinity RISC-V IDE 安装，"
              "用 --openocd 指定，或设 EFINITY_IDE_HOME", file=sys.stderr)
        return 2
    pgm = os.path.join(efinity, "pgm", "bin", "ftdi_pgm.bat") if efinity else None
    bit = args.bit or os.path.join(MAIN, "fpga", "gfx2d_ti60", "outflow", "gfx2d_ti60.bit")
    log = args.log or os.path.join(os.environ.get("TEMP", "."), "board_run.log")
    if (args.bin or args.flash_boot) and not args.dry_run:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
        except OSError as e:
            print("ERROR: 无法准备串口日志目录：%s" % e, file=sys.stderr)
            return 2
    load_cmds = ["reset halt", "load_image {%s} %s bin" % (os.path.abspath(args.bin or "").replace("\\", "/"), args.addr),
                 "resume %s" % args.addr, "shutdown"]

    # 从 Flash 启动：串口必须先开，再把比特流烧进去——FPGA 配置完成的那一刻
    # SoC 就复位了，bootloader 立刻开始拷程序并打印，晚开串口就丢开头。
    # 这里不用 OpenOCD：板子自己会从 Flash 跑，load_image 反而会把 Flash 启动路径绕过去。
    if args.flash_boot and not args.dry_run:
        print("=== 先开串口 %s，再烧比特流（从 Flash 启动）" % args.port)
        ready = threading.Event()
        armed = threading.Event()
        result = {}
        th = threading.Thread(target=serial_worker, daemon=True,
                              args=(args.port, args.baud, log, args.seconds,
                                    args.stop_on, ready, armed, result, args.forward_input, script,
                                    args.input_script_after))
        th.start()
        ready.wait(timeout=10)
        if result.get("error"):
            print("ERROR: 打不开串口 %s：%s" % (args.port, result["error"]), file=sys.stderr)
            return 2

        # 配置完成之后跑的就是 Flash 里的程序，所以现在就开匹配窗口。
        armed.set()
        if not args.no_program:
            if not os.path.isfile(bit):
                print("ERROR: 找不到比特流 %s" % bit, file=sys.stderr)
                return 2
            if not pgm:
                print("ERROR: 找不到 Efinity（pgm/bin/ftdi_pgm.bat），用 --efinity 指定",
                      file=sys.stderr)
                return 2
            print("=== 烧写比特流 %s" % bit)
            ok, out = program_bitstream(pgm, bit, args.jtag_url)
            if not ok:
                print(out)
                print("ERROR: 烧写失败（输出里没有成功标记；ftdi_pgm.bat 出错时也可能返回 0）",
                      file=sys.stderr)
                return 1

        print("=== 等串口采集结束（上限 %d 秒）" % args.seconds)
        th.join(timeout=args.seconds + 30)
        print("--- 串口输出（%s）---" % log)
        try:
            with open(log, "r", encoding="utf-8", errors="replace") as f:
                sys.stdout.write(f.read())
        except OSError as e:
            print("读日志失败：%s" % e)

        rc = 0
        if result.get("read_error"):
            print("ERROR: 采集中途串口失效（USB 掉线？）：%s" % result["read_error"], file=sys.stderr)
            rc = 1
        elif not result.get("bytes"):
            print("ERROR: 串口 0 字节。先查串口号（--list-ports）、比特流是否已加载", file=sys.stderr)
            rc = 1
        elif args.stop_on and not result.get("stopped"):
            print("ERROR: %d 秒内没有收到 %r" % (args.seconds, args.stop_on), file=sys.stderr)
            rc = 1
        return rc

    if args.dry_run:
        print("OpenOCD   : %s\n配置      : %s\nworkspace : %s" % (ocd, cfg, workspace))
        if not args.no_program:
            print("烧写      : %s -m jtag -u %s %s" % (pgm, args.jtag_url, bit))
        if args.check_only:
            print("命令      : reg pc; exit（连两次）")
        elif args.cmd:
            print("命令      : %s" % "; ".join(["reset halt"] + args.cmd + ["shutdown"]))
        elif args.flash_boot:
            print("串口      : %s @ %d -> %s" % (args.port, args.baud, log))
            print("流程      : 先开串口，再烧比特流，等板子从 Flash 自启动")
        else:
            print("串口      : %s @ %d -> %s" % (args.port, args.baud, log))
            print("命令      : %s" % "; ".join(load_cmds))
        return 0

    if not args.no_program:
        if not os.path.isfile(bit):
            print("ERROR: 找不到比特流 %s" % bit, file=sys.stderr)
            return 2
        if not pgm:
            print("ERROR: 找不到 Efinity（pgm/bin/ftdi_pgm.bat），用 --efinity 指定", file=sys.stderr)
            return 2
        print("=== 烧写比特流 %s" % bit)
        ok, out = program_bitstream(pgm, bit, args.jtag_url)
        if not ok:
            print(out)
            print("ERROR: 烧写失败（输出里没有成功标记；ftdi_pgm.bat 出错时也可能返回 0）",
                  file=sys.stderr)
            return 1

    if args.check_only:
        print("=== 调试通路检查：连续两次会话各读一次 PC")
        for i in (1, 2):
            ok, out = run_openocd(ocd, cfg, workspace, ["reg pc", "exit"], timeout=60)
            pc = re.search(r"pc \(/32\): (0x[0-9a-fA-F]+)", out)
            print("  第 %d 次：%s %s" % (i, "OK" if ok and pc else "FAIL", pc.group(1) if pc else ""))
            if not (ok and pc):
                print(out)
                return 1
        return 0

    if args.cmd:
        print("=== 复位后执行 %d 条命令" % len(args.cmd))
        ok, out = run_openocd(ocd, cfg, workspace, ["reset halt"] + args.cmd + ["shutdown"])
        print(out)
        if not ok:
            print("ERROR: 没有考察到目标", file=sys.stderr)
        return 0 if ok else 1

    print("=== 先开串口 %s，再加载程序（顺序不能反）" % args.port)
    ready = threading.Event()
    armed = threading.Event()       # 加载完成之前收到的字节不参与 stop_on 匹配
    result = {}
    th = threading.Thread(target=serial_worker, daemon=True,
                          args=(args.port, args.baud, log, args.seconds,
                          args.stop_on, ready, armed, result, args.forward_input, script,
                          args.input_script_after))
    th.start()
    ready.wait(timeout=10)
    if result.get("error"):
        print("ERROR: 打不开串口 %s：%s" % (args.port, result["error"]), file=sys.stderr)
        return 2

    print("=== reset halt + load_image + resume")
    ok, out = run_openocd(ocd, cfg, workspace, load_cmds)
    written = re.search(r"(\d+) bytes written at address", out)
    size = os.path.getsize(args.bin)
    for line in out.splitlines():
        if re.search(r"examined|bytes written|downloaded|Error|TIMEOUT", line):
            print("   " + line.strip())
    loaded = ok and written and int(written.group(1)) == size
    if not loaded:
        print("ERROR: 加载没有正面证据（要求考察到目标，且写入 %d 字节）" % size, file=sys.stderr)
    # 从这里开始收到的才算这个程序的输出：烧完比特流板子会从 Flash 自启动，
    # 那一份程序打的 done / fail 0 不能拿来当本次的证据（见 serial_worker）。
    armed.set()

    print("=== 等串口采集结束（上限 %d 秒）" % args.seconds)
    th.join(timeout=args.seconds + 30)
    print("--- 串口输出（%s）---" % log)
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            sys.stdout.write(f.read())
    except OSError as e:
        print("读日志失败：%s" % e)

    rc = 0 if loaded else 1
    if result.get("read_error"):
        print("ERROR: 采集中途串口失效（USB 掉线？）：%s" % result["read_error"], file=sys.stderr)
        rc = 1
    elif not result.get("bytes"):
        print("ERROR: 串口 0 字节。先查串口号（--list-ports）、比特流是否已加载", file=sys.stderr)
        rc = 1
    elif args.stop_on and not result.get("stopped"):
        print("ERROR: %d 秒内没有收到 %r" % (args.seconds, args.stop_on), file=sys.stderr)
        if result.get("pre_load_bytes"):
            print("       （加载前板子自启动打了 %d 字节，那一段不参与匹配；"
                  "--stop-on 要取程序末尾的标记）" % result["pre_load_bytes"],
                  file=sys.stderr)
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
