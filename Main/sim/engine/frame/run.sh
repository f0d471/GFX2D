#!/usr/bin/env bash
# 顶层集成的整帧仿真
#
# 被测是 engine_top 六层整机经 APB 提交整张命令表，从机是带随机反压的
# axi_ram_model（262144 字 = 4 MiB；用例窗口外另存命令表）。
#
# 用例三组：
#   frames/frame0..10  十一个整帧金标准（pix_cases_dump --frame 导出）；
#                      内存逐 128 位对软件执行器，帧缓冲 CRC 由本脚本按
#                      软件执行器同口径另算并与 sw/lib/test/golden.txt 比对
#   sw6/sw6a_f0、sw6a_f1  基础 Demo 场景 A 连续两帧（sw6_frames_dump 导出，游戏本体的
#                      sw6_scene_a.c 组表），内存逐 128 位对软件执行器
#   sprites            16 颗互不重叠的 16×16 ALPHA，定时从机（RD_LAT=23 RD_BUB=12
#                      WR_BUB=6 B_LAT=1），PERF_BUSY 逐颗对账（性能判据见测试台文件头）
#
# 注错：从机对帧缓冲回 SLVERR，必须观测到 BUS_WRITE(11)、总线错误与内存失配。
#
# 需要 WSL 里的 iverilog 12 与 python3；不例化 Axi_Mux，不需要 EFINITY_HOME。
set -euo pipefail
cd "$(dirname "$0")"

RTL=../../../rtl
CHK=../../../tools/check
LIB=../../../sw/lib
GAME=../../../sw/apps/game/gfx2dGame
BUILD=build
mkdir -p "$BUILD/frames" "$BUILD/sw6"

bad=0
total=0

CORE="$RTL/engine/engine_tex.v $RTL/engine/engine_top.v $RTL/engine/engine_regs.v $RTL/engine/engine_cmd.v $RTL/engine/engine_inst.v $RTL/engine/engine_bounds.v $RTL/engine/engine_packet_check.v $RTL/engine/engine_pb_dma.v $RTL/engine/engine_bin.v $RTL/engine/engine_pb.v \
$RTL/engine/engine_geom.v $RTL/engine/engine_pix.v $RTL/engine/engine_blend.v \
$RTL/engine/engine_band.v $RTL/engine/engine_mover.v $RTL/vendor/verilog-axi/axi_dma_rd.v \
$RTL/vendor/verilog-axi/axi_dma_wr.v $RTL/../sim/axi_probe/axi_ram_model.v"

# ---------------------------------------------------------------- 1 导出用例
echo "导出整帧用例（SW3 金标准与 SW6 场景 A）："
if ! gcc -O2 -std=c11 -Wall -Wextra -Werror -I"$LIB" \
        -o "$BUILD/pix_cases_dump" "$CHK/pix_cases_dump.c" \
        "$LIB/gfx/list.c" "$LIB/gfx/strip.c" "$LIB/gfx/strip_plan.c" "$LIB/gfx/check.c" "$LIB/gfx/exec_sw.c" \
        "$LIB/gfx/cases.c" "$LIB/core/format.c"; then
    echo "RESULT: SETUP-ERROR (pix_cases_dump 编不过)"; exit 1
fi
for i in {0..10}; do
    "$BUILD/pix_cases_dump" --frame "$i" "$BUILD/frames/frame$i" > /dev/null
done
if ! gcc -O2 -std=c11 -Wall -Wextra -Werror -I"$LIB" -I../../../sw/bsp/include -I"$GAME/src/game" \
        -o "$BUILD/sw6_frames_dump" "$CHK/sw6_frames_dump.c" \
        "$GAME/src/game/sw6_scene_a.c" \
        "$LIB/gfx/list.c" "$LIB/gfx/strip.c" "$LIB/gfx/strip_plan.c" "$LIB/gfx/check.c" "$LIB/gfx/exec_sw.c" \
        "$LIB/core/format.c" "$LIB/core/xorshift32.c"; then
    echo "RESULT: SETUP-ERROR (sw6_frames_dump 编不过)"; exit 1
fi
"$BUILD/sw6_frames_dump" "$BUILD/sw6/sw6a_f0" "$BUILD/sw6/sw6a_f1" > /dev/null
gcc -O2 -std=c11 -Wall -Wextra -Werror -I"$LIB" -o "$BUILD/sprites_dump" \
    "$CHK/pix_cases_dump.c" \
    "$LIB/gfx/list.c" "$LIB/gfx/strip.c" "$LIB/gfx/strip_plan.c" "$LIB/gfx/check.c" "$LIB/gfx/exec_sw.c" \
    "$LIB/gfx/cases.c" "$LIB/core/format.c"
"$BUILD/sprites_dump" --sprites "$BUILD/sprites" > /dev/null
echo "  用例导出完成。"
echo ""

# ---------------------------------------------------------------- 2 跑仿真
run_case() {  # run_case <名字> <CASE前缀> <额外宏/参数> <期望 RESULT>
    local name=$1 casep=$2 extra=$3 want=$4
    # INJECT 一律从 $extra 传（iverilog 对同名 -P 取先出现的那个，写死在前面会
    # 让注错构建的覆盖失效——第一版注错构建因此全绿）
    if ! iverilog -g2012 -DCASE="\"$casep\"" $extra \
            -I "$RTL/engine" -I "$RTL/../sim/axi_probe" \
            -o "$BUILD/$name.vvp" tb_engine_frame.v $CORE \
            > "$BUILD/$name.compile.log" 2>&1; then
        echo "  $name: RESULT: COMPILE-ERROR (see $BUILD/$name.compile.log)"
        echo "    失败"
        return 1
    fi
    (cd "$BUILD" && vvp -n "$name.vvp") > "$BUILD/$name.log" 2>&1
    local r
    r=$(grep -E '^RESULT:' "$BUILD/$name.log" | tail -1)
    grep -E '^FRAME|^SPRITES' "$BUILD/$name.log" | sed 's/^/    /'
    echo "  $name: ${r:-RESULT: MISSING}"
    case "$r" in
        "RESULT: $want"*) return 0 ;;
        *) grep -E 'LIST1-ERR|LIST2-ERR|MEM |PERF-|BUS-ERR|WLAST-ERR|TIMEOUT-WAIT|ID-FAIL|CAPS-FAIL|APB-FAIL' \
               "$BUILD/$name.log" | head -6 | sed 's/^/    /'
           return 1 ;;
    esac
}

echo "十一个整帧金标准（内存对软件执行器 + CRC 对 golden.txt）："
for i in {0..10}; do
    if run_case "frame$i" "frames/frame$i" "" "PASS"; then total=$((total + 1)); else bad=$((bad + 1)); fi
done
echo ""

echo "SW6 场景 A 两帧："
if run_case sw6a_f0 "sw6/sw6a_f0" "" "PASS"; then total=$((total + 1)); else bad=$((bad + 1)); fi
if run_case sw6a_f1 "sw6/sw6a_f1" "" "PASS"; then total=$((total + 1)); else bad=$((bad + 1)); fi
echo ""

echo "性能（定时从机，16 颗 16×16 ALPHA）："
if run_case sprites_timed "sprites" "-DTIMED -DSPRITES" "PASS"; then total=$((total + 1)); else bad=$((bad + 1)); fi
echo ""

echo "注错构建（从机把帧缓冲设成 SLVERR 窗口，期望见红）："
# 帧级注错用总线错误：ERRWIN=1 让从机对用例的帧缓冲区间回 SLVERR，引擎的写
# 必须以 BUS_WRITE 报错、期望内存随之全面失配。ctrl 套件的 INJECT=1/2 是检查
# 优先级与回退的结构注错，只对带错的表见红，对全合法的帧本来就不触发。
# 注错用例用独立前缀：测试台会把帧缓冲写到 <CASE>.got.hex，复用 frame0 的前缀
# 会把金标准导出覆盖掉，后面的 CRC 块就对错误现场算了（第一版栽在这里）。
mkdir -p "$BUILD/inject"
for ext in init cmd surf exp cnt stats; do
    cp "$BUILD/frames/frame0.$ext.hex" "$BUILD/inject/frame0.$ext.hex"
done
if run_case inject_slverr "inject/frame0" "-Ptb_engine_frame.ERRWIN=1" "FAIL (BUS_WRITE confirmed)"; then total=$((total + 1)); else bad=$((bad + 1)); fi
echo ""

# ---------------------------------------------------------------- 3 帧缓冲 CRC
echo "帧缓冲 CRC（sw 第 03 章 3.6 节口径，对 golden.txt）："
crc_bad=0
python3 - <<'PY' || crc_bad=1
from pathlib import Path
import zlib

base = Path('build/frames')
golden = dict(line.split() for line in Path('../../../sw/lib/test/golden.txt').read_text().splitlines())
fails = 0
for i in range(11):
    prefix = base / f'frame{i}'
    meta = dict(line.split('=', 1) for line in Path(f'{prefix}.meta.txt').read_text().splitlines())
    raw = bytearray()
    for word in Path(f'{prefix}.got.hex').read_text().splitlines():
        # 从机 4 MiB，用例窗口之外是 X：只发生在窗口后面的行，帧缓冲用不到
        if 'x' in word or 'X' in word:
            continue
        raw.extend(bytes.fromhex(word)[::-1])
    crc = 0
    for y in range(480):
        offset = 0x1000 + y * 4096
        crc = zlib.crc32(raw[offset:offset+640*4], crc)
    actual = f'{crc:08x}'
    expected = golden[meta['name']]
    ok = actual == expected
    print(f'CRC {i:02d} {meta["name"]}: {actual} {"PASS" if ok else "FAIL (want " + expected + ")"}')
    if not ok:
        fails += 1
if fails:
    raise SystemExit(1)
print(f'CRC OK: 11/11')
PY
total=$((total + 1))
if [ "$crc_bad" -eq 0 ]; then echo "  CRC: PASS"; else echo "  CRC: FAIL"; bad=$((bad + 1)); fi
echo ""

# ---------------------------------------------------------------- 结论
if [ $bad -eq 0 ]; then
    echo "SIM OK: $total/$total checks as expected"
    exit 0
else
    echo "SIM FAIL: $bad/$total 项不符合期望"
    exit 1
fi
