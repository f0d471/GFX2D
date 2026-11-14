#!/usr/bin/env bash
# 计算阵列与片上缓存的仿真总入口：先跑前期研究的探针（9 项），
# 再跑入库 RTL 的正式判据 run_formal.sh，两部分都符合预期才输出 SIM OK。
#
# probe_pix.v 是研究探针（不是入库实现），回答研究清单的第 1、3 问：
#   旋转器与首末拍（两拍窗口、4 个位置、首末拍掩码）、流水线的拍对齐
#   （L=3 与色键、掩码的延迟像；「每拍一拍」判据；写流反压下整条流水停顿）。
# 第 5 问的单独综合在 syn/ 下的 Efinity 小工程（README 登记命令与判据）。
#
# 需要 WSL 里的 iverilog 12。不需要 EFINITY_HOME（不经过 Axi_Mux）。
# 用例：sweep 400 行（npx 1..25 × ls × ld，方程/格式/色键/半号滚动）+
#       FILL 六方程 + 拆段四用例 + 长行八行（253、640 像素）。
# 两种下游：默认 ws_tready 恒 1；-DREALWR 接真写核 axi_dma_wr 与板上实测校准的定时从机。
# 注错：位 0 旋转错位、位 1 跳过色键、位 2 丢首拍掩码（恒 ready 下见红）；
#       位 3 不看 ws_tready、位 4 混合核不随流水停（真写核下见红，恒 ready 下必须不红——
#       这两条说明恒 ready 的测试台看不见反压，这是复核前的判据漏掉的地方）。
set -u
cd "$(dirname "$0")"

# 单例模式：--case <用例名> 跳过研究探针，只让 run_formal.sh 跑那一个用例，
# 只打印结果；判定交给 sim/mutation 的跑器。不给参数时行为与原来一致。
CASE_ONLY=""
if [ "${1:-}" = "--case" ]; then CASE_ONLY="${2:-}"; shift 2; fi

RTL=../../../rtl
SRC="probe_pix.v tb_probe_pix.v $RTL/engine/core/engine_blend.v"
SRC_WR="$SRC $RTL/vendor/verilog-axi/axi_dma_wr.v ../../axi_probe/axi_ram_model.v"
BUILD=build
mkdir -p "$BUILD"

bad=0
total=0

# build_one <名字> <注错开关，空为干净构建> <期望 PASS|FAIL|NOT_RED> <REALWR 0|1>
build_one() {
    local name="$1" inj="$2" want="$3" real="$4" defs src
    if [ "$real" = 1 ]; then defs="-DREALWR"; src="$SRC_WR"; else defs=""; src="$SRC"; fi
    if ! iverilog -g2012 -I "$RTL/engine/include" $inj $defs -o "$BUILD/$name.vvp" $src \
            > "$BUILD/$name.compile.log" 2>&1; then
        echo "  $name: RESULT: COMPILE-ERROR (see $BUILD/$name.compile.log)"
        bad=$((bad + 1)); total=$((total + 1)); return
    fi
    vvp -n "$BUILD/$name.vvp" > "$BUILD/$name.log" 2>&1
    local r s
    r=$(grep -E '^RESULT:' "$BUILD/$name.log" | tail -1)
    s=$(grep -E '^(realwr|const-ready):' "$BUILD/$name.log" | tail -1)
    echo "  $name: ${r:-RESULT: MISSING}"
    [ -n "$s" ] && [ "$want" = PASS ] && echo "      $s"
    total=$((total + 1))
    case "$want" in
        PASS)    case "$r" in "RESULT: PASS"*)    ;; *) bad=$((bad + 1)) ;; esac ;;
        FAIL)    case "$r" in "RESULT: FAIL"*)    ;; *) bad=$((bad + 1)) ;; esac ;;
        NOT_RED) case "$r" in "RESULT: NOT_RED"*) ;; *) bad=$((bad + 1)) ;; esac ;;
    esac
}

if [ -z "$CASE_ONLY" ]; then
echo "B4 研究探针 probe_pix："
build_one probe_normal          ""  PASS    0
build_one probe_realwr          ""  PASS    1
# 研究探针是仿真模型（非生产 RTL），仍用自己的 parameter 注错口，值经 -DINJECT_VAL 传入
build_one probe_inj_rot         "-DINJECT_VAL=1"  FAIL    0     # 旋转错一个位置
build_one probe_inj_key         "-DINJECT_VAL=2"  FAIL    0     # 跳过色键（命中像素照写）
build_one probe_inj_msk         "-DINJECT_VAL=4"  FAIL    0     # 首拍掩码丢掉（行首行外像素照样写）
build_one probe_inj_rdy         "-DINJECT_VAL=8"  FAIL    1     # 不看 ws_tready（en 恒 1）
build_one probe_inj_core        "-DINJECT_VAL=16" FAIL    1     # 混合核不随流水停
build_one probe_inj_rdy_const   "-DINJECT_VAL=8"  NOT_RED 0     # 同一个错，恒 ready 的测试台看不见
build_one probe_inj_core_const  "-DINJECT_VAL=16" NOT_RED 0

if [ "$bad" -ne 0 ]; then
    echo "SIM BAD: $bad of $total probe checks unexpected"
    exit 1
fi
echo "PROBE OK: $total/$total checks as expected"
fi

# 入库 RTL 的正式判据，项数取它自己报的
if [ -n "$CASE_ONLY" ]; then
    formal=$(bash run_formal.sh --case "$CASE_ONLY")
    rc=$?
    echo "$formal"
    exit $rc
fi
formal=$(bash run_formal.sh)
rc=$?
echo "$formal"
[ "$rc" -eq 0 ] || exit 1
n=$(echo "$formal" | sed -n 's|^SIM OK: \([0-9]*\)/.*|\1|p')
echo "SIM OK: $((total + n))/$((total + n)) checks as expected"
