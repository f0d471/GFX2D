#!/usr/bin/env python3
"""展开、还原与检查仓库里的绝对路径。

厂商工具生成的少数几个文件必须写绝对路径，仓库里存的是占位符形式，
克隆之后展开一次即可使用，提交之前还原成占位符。

    python3 tools/check/localize_paths.py --apply      占位符 -> 本机路径
    python3 tools/check/localize_paths.py --restore    本机路径 -> 占位符
    python3 tools/check/localize_paths.py --check      扫描整个仓库，发现机器专属路径就报错

--check 可加 --tracked 限定 Git 已登记文件（含暂存新增文件）；目录由本脚本的位置推出。
占位符代表的是 `Main/` 的路径（历史原因叫 __REPO_ROOT__）；`--check` 扫的是整个 git 仓库。
"""

import argparse
import os
import re
import sys
import subprocess

MAIN = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROOT = os.path.dirname(MAIN)          # git 仓库根，--check 从这里扫

# 需要处理的文件，以及每个占位符在该文件里用的分隔符风格。
# bs   反斜杠，Windows 原样写法
# fs   正斜杠
# bs2  两个反斜杠，JSON 字符串里的转义写法
TARGETS = {
    "fpga/gfx2d_ti60/ip/soc/settings.json": ["bs2", "fs"],
    "fpga/gfx2d_ti60/ip/soc/source/soc_config": ["bs"],
    "sdk/software/standalone/gpio/gpioDemo/.cproject": ["bs"],
    "sdk/software/standalone/gpio/gpioDemo/gpioDemo_ti.launch": ["bs"],
    "sdk/software/standalone/gpio/gpioDemo/gpioDemo_trion.launch": ["bs"],
    "sdk/software/standalone/gpio/gpioDemo/gpioDemo_tz.launch": ["bs"],
}

TOKEN = {
    "bs": "__REPO_ROOT__",
    "fs": "__REPO_ROOT_FS__",
    "bs2": "__REPO_ROOT_BS2__",
}


def win_repo():
    """Main 目录的 Windows 写法（占位符代表的就是它）。

    这几个文件由 Windows 上的厂商工具读取，所以即使在 WSL 里运行本脚本，
    写进去的也必须是盘符路径。`/mnt/e/x` 这样的挂载点在这里换算回 `E:\\x`。
    """
    m = re.match(r"^/mnt/([a-z])(/.*)?$", MAIN)
    if m:
        return m.group(1).upper() + ":" + (m.group(2) or "/").replace("/", "\\")
    return MAIN.replace("/", "\\")


def rendered(style):
    """Main 目录在该分隔符风格下的写法。"""
    win = win_repo()
    if style == "bs":
        return win
    if style == "fs":
        return win.replace("\\", "/")
    if style == "bs2":
        return win.replace("\\", "\\\\")
    raise ValueError(style)


# --check 扫描的模式：盘符路径、WSL 的家目录、WSL 挂载点。
# 盘符前加一个后顾断言，否则 `http://` 里的 `p:/` 也会命中。
SUSPECT = re.compile(r"(?:(?<![A-Za-z0-9])[A-Za-z]:[\\/]"
                     r"|/home/[A-Za-z0-9._-]+/"
                     r"|/mnt/[a-z]/)")

# 以下路径都相对 git 仓库根。
# 允许保留绝对路径的文件。
#   Main/doc/开发环境.md   按约定集中记录本机取值，见该文件第 11 节
#   本脚本                 说明里要举路径的例子
#   原样引用厂商报错输出的文档   输出里带 Efinity 的安装路径
ALLOW = {
    "Main/doc/开发环境.md",
    "Main/tools/check/localize_paths.py",
    "Engineering/TechReports/platform/07-SoC-IP定型与工具链升级.md",
}

# 厂商代码与 IP 生成物不在检查范围内，它们带的是生成时那台机器的工具安装位置。
# 其中需要跟着仓库位置走的几个文件在 TARGETS 里逐个检查。
# Efinix/ 是不入库的厂商资料包。
SKIP_PREFIXES = ("Efinix/", "Main/rtl/vendor/", "Main/sdk/", "Main/fpga/gfx2d_ti60/ip/", "Main/bit/")

SKIP_DIRS = {".git", "outflow", "work_syn", "work_pnr", "work_pt", "work_dbg",
             "build", ".metadata", "flash_backup", "__pycache__"}

TEXT_SUFFIX = {".md", ".v", ".vh", ".c", ".h", ".S", ".s", ".py", ".sh", ".json",
               ".xml", ".sdc", ".ld", ".cfg", ".yaml", ".mk", ".launch", ".cproject",
               ".project", ".txt", ".bat"}


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def substitute(forward):
    """forward 为真时展开占位符，否则还原。"""
    changed = 0
    for rel, styles in TARGETS.items():
        path = os.path.join(MAIN, rel.replace("/", os.sep))
        if not os.path.exists(path):
            print("缺文件：%s" % rel)
            return 1
        text = original = read(path)
        for style in styles:
            token, value = TOKEN[style], rendered(style)
            text = text.replace(token, value) if forward else text.replace(value, token)
        if text != original:
            write(path, text)
            changed += 1
            print("已改写 %s" % rel)
    print("%s 完成，改写 %d 个文件，Main 目录 %s"
          % ("展开" if forward else "还原", changed, MAIN))
    return 0


def walk():
    for root, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            path = os.path.join(root, name)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            suffix = os.path.splitext(name)[1]
            if suffix in TEXT_SUFFIX or name in ("README", "Makefile", "makefile"):
                yield rel, path


def candidates(tracked_only=False):
    if tracked_only:
        p = subprocess.run(["git", "-c", "safe.directory=" + ROOT.replace("\\", "/"),
                            "ls-files", "-z"], cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
        if p.returncode:
            print("无法读取 Git 文件清单：" + p.stderr)
            raise RuntimeError("无法读取 Git 文件清单：" + p.stderr)
        files = [(rel, os.path.join(ROOT, rel)) for rel in p.stdout.split("\0") if rel]
        files = [(rel, path) for rel, path in files
                 if os.path.splitext(rel)[1] in TEXT_SUFFIX or os.path.basename(rel) in ("README", "Makefile", "makefile")]
    else:
        files = walk()
    for rel, path in files:
        if rel in ALLOW:
            continue
        if rel.startswith(SKIP_PREFIXES) and rel[len("Main/"):] not in TARGETS:
            continue
        yield rel, path


def check(tracked_only=False):
    hits = []
    for rel, path in candidates(tracked_only):
        try:
            text = read(path)
        except (UnicodeDecodeError, OSError):
            continue
        for num, line in enumerate(text.splitlines(), 1):
            if SUSPECT.search(line):
                hits.append((rel, num, line.strip()[:120]))
    if hits:
        print("发现 %d 处机器专属路径：" % len(hits))
        for rel, num, line in hits:
            print("  %s:%d  %s" % (rel, num, line))
        print("按 Main/doc/开发环境.md 第 1 节改成占位符，"
              "或者确认它属于该文件第 11 节那类记录再加进本脚本的 ALLOW。")
        return 1
    print("检查通过，没有机器专属路径。")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--apply", action="store_true", help="占位符展开成本机路径")
    group.add_argument("--restore", action="store_true", help="本机路径还原成占位符")
    group.add_argument("--check", action="store_true", help="扫描机器专属路径")
    parser.add_argument("--tracked", action="store_true", help="仅 --check：检查 Git 已登记文件，含暂存新增文件")
    args = parser.parse_args()
    if args.tracked and not args.check:
        parser.error("--tracked 只与 --check 一起使用")
    if args.apply:
        return substitute(True)
    if args.restore:
        return substitute(False)
    return check(args.tracked)


if __name__ == "__main__":
    sys.exit(main())
