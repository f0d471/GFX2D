#!/usr/bin/env bash
# 控制层的仿真
#
# 三步：
#   1 研究探针 probe_check.v（十步检查三种结构，保留为回归）
#   2 engine_top 单元仿真：159 条一致性向量（APB 提交、端到端）+ 取数边界/总线错误/
#     超时/软复位/写后读/首错顺序/PERF 抽样的定向用例（判据见 tb_engine_top.v 文件头）
#   3 两个注错构建：-DINJ_CHK_SWAP 对调 space/format、-DINJ_NO_FB 关掉逐条回退，
#     各自必须让至少一条向量见红（+vectors_only 反转判据）
#
# 需要 WSL 里的 iverilog 12 与 python3；engine_top 套件不需要 EFINITY_HOME
#（不例化 Axi_Mux，直接接 axi_ram_model）。
set -u
cd "$(dirname "$0")"

RTL=../../../rtl
BUILD=build
mkdir -p "$BUILD"

bad=0
total=0

CORE="$RTL/engine/engine_tex.v $RTL/engine/engine_top.v $RTL/engine/engine_regs.v $RTL/engine/engine_cmd.v $RTL/engine/engine_inst.v $RTL/engine/engine_bounds.v $RTL/engine/engine_packet_check.v $RTL/engine/engine_pb_dma.v $RTL/engine/engine_bin.v $RTL/engine/engine_pb.v \
$RTL/engine/engine_geom.v $RTL/engine/engine_pix.v $RTL/engine/engine_blend.v \
$RTL/engine/engine_band.v $RTL/engine/engine_mover.v $RTL/vendor/verilog-axi/axi_dma_rd.v \
$RTL/vendor/verilog-axi/axi_dma_wr.v $RTL/../sim/axi_probe/axi_ram_model.v"

# ---------------------------------------------------------------- 1 研究探针
echo "研究探针 probe_check（B5 前期研究）："
if ! iverilog -g2012 -o "$BUILD/probe.vvp" probe_check.v tb_probe_check.v \
        > "$BUILD/probe.compile.log" 2>&1; then
    echo "  RESULT: COMPILE-ERROR (see $BUILD/probe.compile.log)"
    bad=$((bad + 1))
else
    vvp -n "$BUILD/probe.vvp" > "$BUILD/probe.log" 2>&1
    r=$(grep -E '^RESULT:' "$BUILD/probe.log")
    echo "  probe: ${r:-RESULT: MISSING}"
    total=$((total + 1))
    case "$r" in "RESULT: PASS"*) ;; *) bad=$((bad + 1)) ;; esac
fi
echo ""

# ---------------------------------------------------------------- 2 准备向量
if ! python3 ../../../tools/check/check_cmd_semantics.py \
        --emit "$BUILD/emit.txt" --no-selftest > "$BUILD/emit.log" 2>&1; then
    echo "RESULT: SETUP-ERROR (check_cmd_semantics.py --emit 失败，见 $BUILD/emit.log)"
    exit 1
fi
python3 ../../../tools/check/vec_to_sim.py "$BUILD/emit.txt" "$BUILD/vec_stream.txt" || exit 1

# ---------------------------------------------------------------- 3 engine_top
build_engine() {  # build_engine <名字> <注错开关，空为干净构建> <plusargs> <期望 RESULT>
    local name=$1 inj=$2 pargs=$3 want=$4
    if ! iverilog -g2012 $inj -I "$RTL/engine" -I "$RTL/../sim/axi_probe" \
            -o "$BUILD/$name.vvp" tb_engine_top.v $CORE \
            > "$BUILD/$name.compile.log" 2>&1; then
        echo "  $name: RESULT: COMPILE-ERROR (see $BUILD/$name.compile.log)"
        echo "    失败"
        return 1
    fi
    (cd "$BUILD" && vvp -n "$name.vvp" +$pargs) > "$BUILD/$name.log" 2>&1
    local r
    r=$(grep -E '^RESULT:' "$BUILD/$name.log" | tail -1)
    echo "  $name: ${r:-RESULT: MISSING}"
    case "$r" in
        "RESULT: $want"*) return 0 ;;
        *) grep -E 'VEC-FAIL|VEC-MEM|CASE-FAIL|MEM-FAIL|PERF-FAIL|IRQ-FAIL|CIDX-ERR|TIMEOUT-WAIT' \
               "$BUILD/$name.log" | head -8 | sed 's/^/    /'
           return 1 ;;
    esac
}

echo "engine_top 单元仿真（B1~B5 全部，159 向量 + 定向）："
if build_engine engine_top "" "" "PASS"; then total=$((total + 1)); else bad=$((bad + 1)); fi
echo ""

echo "注错构建（期望见红，即 RESULT: FAIL）："
if build_engine inject_swap "-DINJ_CHK_SWAP" "vectors_only" "FAIL"; then total=$((total + 1)); else bad=$((bad + 1)); fi
if build_engine inject_nofb "-DINJ_NO_FB" "vectors_only" "FAIL"; then total=$((total + 1)); else bad=$((bad + 1)); fi
echo ""

# ---------------------------------------------------------------- 结论
if [ $bad -eq 0 ]; then
    echo "SIM OK: $total/$total checks as expected"
    exit 0
else
    echo "SIM FAIL: $bad/$total 项不符合期望"
    exit 1
fi
