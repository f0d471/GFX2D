#!/usr/bin/env bash
# 裁剪位流上板判据：逐档部署，读引擎 CAPS 与 ID，断言与特性清单一致。
#
# 不需要写任何测试程序：引擎控制寄存器挂在 APB 上，
# 位流加载后 OpenOCD 直接 mdw 就能读（见 Main/doc/开发环境.md 的上板回归表）。
# 这条判据证明的是「关掉的机制在真实硬件上确实不在」，与 netlist 结构检查互补：
# 后者读综合网表，这条读运行中的硬件。
#
# 用法（仓根，WSL）：bash Main/sim/engine/features/board_caps.sh B0 B2 B5
#                   bash Main/sim/engine/features/board_caps.sh --all
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
TREES=${GFX_FEATURE_TREES:-$ROOT/Main/sim/build/feature-accept}
cd "$ROOT"

if [ "$#" -eq 0 ] || [ "$1" = "--all" ]; then
  set -- B0 B1 B2 B3 B4 B5 B6
fi

# 用 Windows 的 Efinity 内置 Python 起 board_run.py；包装脚本与理由见
# Main/tools/board/python-efinity.cmd。用 cmd.exe /c 且转成 Windows 路径。
WRAP=$(wslpath -w Main/tools/board/python-efinity.cmd)
BOARD=$(wslpath -w Main/tools/board/board_run.py)
winpath() { wslpath -w "$1"; }

# Efinity 变量不能在父进程里残留，否则厂商脚本跳过自己的 setup.bat。
unset EFINITY_HOME PYTHONHOME EFXPT_HOME EFXPGM_HOME EFXDBG_HOME \
      EFXIPM_HOME EFXSVF_HOME EFXSERDESDBG_HOME EFINITY_USER_DIR_INI 2>/dev/null || true

fail=0
printf '%-4s %-12s %-12s %s\n' 档 期望CAPS 读回CAPS 结果
for preset in "$@"; do
  tree="$TREES/$preset"
  bit="$tree/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit"
  if [ ! -f "$bit" ]; then
    echo "$preset  缺位流（先综合）"; fail=1; continue
  fi
  want=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['caps'])" \
         "$tree/config.json" | sed 's/^0x//' | tr 'A-F' 'a-f')
  if ! cmd.exe /c "$WRAP" "$BOARD" --bit "$(winpath "$bit")" --check-only \
        >/dev/null 2>&1; then
    echo "$preset  部署失败"; fail=1; continue
  fi
  readreg() {
    cmd.exe /c "$WRAP" "$BOARD" --no-program --cmd "mdw $1" --seconds 5 2>/dev/null \
      | grep -oE "^$1: [0-9a-f]{8}" | awk '{print $2}' | head -1
  }
  got=$(readreg 0xf8100004)
  ident=$(readreg 0xf8100000)
  if [ "$got" = "$want" ] && [ "$ident" = "47465801" ]; then
    printf '%-4s %-12s %-12s %s\n' "$preset" "$want" "$got" PASS
  else
    printf '%-4s %-12s %-12s %s\n' "$preset" "$want" "${got:-读不到}" \
      "FAIL（ID=${ident:-无}）"
    fail=1
  fi
done
echo
echo 'ID 必须是 47465801（GFX 引擎）；CAPS 必须与本构型 config.json 的 caps 逐位相同。'
exit $fail
