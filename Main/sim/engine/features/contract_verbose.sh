#!/usr/bin/env bash
# 直接打印禁用契约测试台的编译输出，用于诊断文件列表与语法问题。
# 用法（仓根）：bash Main/sim/engine/features/contract_verbose.sh <构型源树>
set -u
TREE=$(cd "${1:?需要构型源树}" && pwd)
TB="$(cd "$(dirname "$0")" && pwd)/tb_feature_contract.v"
cd "$TREE"
echo "--- 只含 engine_top 的依赖闭包 ---"
iverilog -g2012 -I rtl/engine -o /dev/null -s tb_feature_contract "$TB" \
  rtl/engine/engine_top.v rtl/engine/core/engine_regs.v rtl/engine/core/engine_cmd.v \
  rtl/engine/core/engine_geom.v rtl/engine/core/engine_pix.v rtl/engine/core/engine_blend.v \
  rtl/engine/core/engine_mover.v rtl/vendor/verilog-axi/axi_dma_rd.v \
  rtl/vendor/verilog-axi/axi_dma_wr.v rtl/engine/buffer/engine_band.v \
  rtl/engine/frontend/engine_inst.v 2>&1 | head -12
echo "--- 去掉 tb 里的 include，看是否宏缺失 ---"
echo "--- engine_pb 尾部（前 3 行尾部检查）---"
tail -3 rtl/engine/frontend/engine_pb.v | cat -A | head -5
