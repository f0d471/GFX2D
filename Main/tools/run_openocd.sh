#!/bin/bash
# run_openocd.sh —— 用 OpenOCD 把一个程序加载到板上并运行。
#
# 用法：bash tools/run_openocd.sh <程序.bin> [加载地址]
#
# 用法约束与两个坑：
#
#   1  串口要在调用本脚本之前就打开（见 tools/serial_capture.ps1）。
#      程序跑完再打开串口就一个字节都读不到。
#   2  WSL 不把自定义环境变量传给 Windows 进程，gfx2d_debug.cfg 第 59 行要的
#      workspace_loc 因此只能在 Tcl 里填，用 -c 前置命令而不是 export。
#   3  openocd.exe 是 Windows 程序，读不了 /mnt/... 形式的路径，用 wslpath 转。
#   4  加载前必须 `reset halt`。少了这一步，前一次实验把板子留在坏状态时
#      （例如探针主机挂住总线），load_image 仍会报成功、resume 也会返回，
#      但程序一行都不会跑，串口全空——看起来像程序有问题，其实是没复位。
#      这一条 2026-09-17 实测踩过：同一份二进制不加复位零输出，加上复位正常。
#
# 工具位置按环境变量解析，仓库不写绝对路径：
#   EFINITY_IDE_HOME  Efinity RISC-V IDE 的安装目录（默认与 Efinity 同级）
#   EFINITY_HOME      Efinity 主目录，用来推出 IDE 的位置
set -eu

BIN="${1:?用法: run_openocd.sh <程序.bin> [加载地址]}"
ADDR="${2:-0x1000}"

if [ ! -f "$BIN" ]; then
    echo "找不到程序 $BIN" >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/.." && pwd)"          # <仓库>/Main

# IDE 目录：环境变量优先，其次与 Efinity 同级
IDE="${EFINITY_IDE_HOME:-}"
if [ -z "$IDE" ] && [ -n "${EFINITY_HOME:-}" ]; then
    IDE="$(ls -d "$(dirname "$EFINITY_HOME")"/efinity-riscv-ide-* 2>/dev/null | tail -1 || true)"
fi
OCD="$IDE/openocd/bin/openocd.exe"
if [ ! -f "$OCD" ]; then
    echo "找不到 openocd.exe。设 EFINITY_IDE_HOME 指向 Efinity RISC-V IDE 的安装目录。" >&2
    exit 2
fi

# 传给 Windows 程序的路径转成 Windows 形式
BIN_WIN="$(wslpath -w "$BIN")"
WORKSPACE_WIN="$(wslpath -w "$MAIN/sdk")"
BIN_WIN="${BIN_WIN//\\//}"
WORKSPACE_WIN="${WORKSPACE_WIN//\\//}"

cd "$MAIN"
"$OCD" \
    -c "set ::env(workspace_loc) {$WORKSPACE_WIN}" \
    -c "set ::env(CPUTAPID) 0x10660A79" \
    -f tools/openocd/gfx2d_debug.cfg \
    -c "reset halt" \
    -c "load_image {$BIN_WIN} $ADDR bin" \
    -c "resume $ADDR" \
    -c "shutdown"
