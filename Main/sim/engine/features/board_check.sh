#!/usr/bin/env bash
# 板测入口：本机没有独立 Windows Python，用 Efinity 内置 Python 的包装脚本启动。
# 用法（仓根，必须在 Windows 侧运行）：
#   bash Main/sim/engine/features/board_check.sh --check-only
#   GFX_CHECK_PORT=COM11 bash Main/sim/engine/features/board_check.sh
#
# 包装脚本与理由见 Main/tools/board/python-efinity.cmd：
# Efinity 的 python311 要 PYTHONHOME 才能起，而厂商烧写工具要 PYTHONHOME 未定义，
# 两者由包装脚本与 board_run.py 的 program_bitstream 分别处理。
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$ROOT"
export GFX_CHECK_PORT=${GFX_CHECK_PORT:-COM11}
exec cmd.exe /c "Main\\tools\\board\\python-efinity.cmd" \
    "Main/tools/check/check_board_smoke.py" "$@"
