#!/usr/bin/env bash
# check_sw8_stats.sh —— 演示模式与统计埋点（SW8）A、B 两批的提交前判据
#
# 判据见 Engineering/Todo/sw/SW8-演示模式与统计埋点.md 第 3 节的「A 批」「B 批」几行，
# 口径见 2.2、2.3、2.4 节与 4.4、4.9 节。
# 本脚本串起来跑：
#
#   M1 主机测试         mk/host.mk 的 test 目标：64 位（ASan/UBSan）与 -m32 输出逐字节相同、fail 0，
#                       stderr 里没有 sanitizer 报告；普查（C 组）19 行、模拟时钟（S 组）4 行、
#                       帧时间直方图（H 组）8 行、日志队列（Q 组）7 行、N_max 状态机（N 组）10 行全 ok
#   M2 普查的注错       CENSUS_NO_SETCLIP / CENSUS_NO_SURF 各编一次，指定的那一行必须 FAIL
#   M3 直方图与模拟时钟的注错
#                       FHIST_P99_LOWER / FHIST_DROP_OVER / COST_NO_B 各编一次，指定的那一行必须 FAIL
#   M4 日志队列的注错   LOGQ_SPLIT / LOGQ_WRAP 各编一次，指定的那一行必须 FAIL
#   M5 board_tx_room 的 PC 实现
#                       主机探针 check_sw8_tx.c 链真实的 bsp/host/board.c：不设 GFX2D_TX_BUDGET 时不限；
#                       设 192 时按场补满、不累加、超额计数，退出时 stderr 的汇总对得上；
#                       注错 HOST_TX_NO_REFILL 必须让 X3 见红
#   M6 板上构建         先打包素材（assets.h 是生成物），再做 gfx2dGame 的真实板上构建（mk/target.mk），
#                       零告警，bsp/ti60 里有 board_tx_room；再做 -DSTG_FIELD=3（N_max 搜索入口）的板上构建，零告警；
#                       要 RISCV_BIN=<RISCV_BIN>，找不到交叉工具链时跳过，**跳过不算通过**
#   M7 N_max 状态机的注错
#                       NMAX_BISECT_SIDE / NMAX_NO_STEPBACK / NMAX_NO_SETTLE 各编一次，指定的那一行必须 FAIL
#   M8 搜索跑在弹幕场上 gfx2dGame 的 app 测试（apps/game/gfx2dGame/test，选项同 test 目标，不链 bsp）
#                       64 位挂 ASan/UBSan、-m32 各一份，输出逐字节相同、fail 0；Y 组 9 行全 ok
#                       （负载稳态、两种负载各搜一遍 OK、结果那一档零掉帧、加 STEP_MIN 有掉帧）
#   M9 STG_FIELD=3 的 PC 驱动
#                       先打包素材，编 gfx2dGame 的 PC 版，无窗口跑：两行 nmax cfg、两行 nmax result OK、
#                       fail 0、done；两行 nmax result 与 M8 的 app 测试打出来的逐字相同
#
# 用法（WSL 或 Linux，仓库根或任意位置）：
#     RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw8_stats.sh
#
# 需要 gcc、gcc-multilib（-m32）、libsdl2-dev（mk/host.mk 解析时就要 sdl2-config）与 python3；
# M6 要交叉工具链（doc/开发环境.md 第 11 节）。全部通过时末尾是 fail 0 与 done，退出码 0。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"
HOST_CC="${HOST_CC:-gcc}"
RISCV_BIN="${RISCV_BIN:-riscv-none-elf-}"
TX_C="$MAIN/tools/check/check_sw8_tx.c"
APP="$SW/apps/game/gfx2dGame"

TMP="$(mktemp -d)"
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

# 跑一次 mk/host.mk 的 test 目标。$1 = 产物目录名，$2 = INJECT（可空）。
# stdout 与 stderr 一起进日志：sanitizer 的报告打在 stderr，test 目标本身不看它。
run_libtest() {
    local dir="$TMP/$1"
    if [ -n "$2" ]; then
        make -C "$SW" -f mk/host.mk test OBJDIR="$dir" INJECT="$2" >"$dir.log" 2>&1
    else
        make -C "$SW" -f mk/host.mk test OBJDIR="$dir" >"$dir.log" 2>&1
    fi
}

# 某一组的 ok 行数恰好是 $2、没有 FAIL 行。$1 = 组的前缀字母，$3 = 名字
group_all_ok() {
    local n_ok n_bad
    n_ok=$(grep -cE "^$1[0-9] .* ok\$" "$OUT" 2>/dev/null)
    n_bad=$(grep -cE "^$1[0-9] .* FAIL\$" "$OUT" 2>/dev/null)
    if [ "$n_ok" -eq "$2" ] && [ "$n_bad" -eq 0 ]; then
        ok "M1$1 $3 $2 行全 ok"
    else
        bad "M1$1 $3 应有 $2 行 ok，实际 ok $n_ok 行、FAIL $n_bad 行"
    fi
}

# 注错：编一次，指定的那一行必须 FAIL。$1 = 判据名，$2 = INJECT，$3 = 行首（grep -E）
expect_red() {
    run_libtest "i_$2" "$2"
    if grep -qE "$3.* FAIL$" "$TMP/i_$2/test/out64.txt" 2>/dev/null; then
        ok "$1 INJECT=$2 见红：$(grep -m1 -E "$3.* FAIL$" "$TMP/i_$2/test/out64.txt" | cut -c1-80)"
    else
        bad "$1 INJECT=$2 没有让「$3」见红"
    fi
}

# ---------------------------------------------------------------- M1 主机测试
echo "== M1 主机测试（64 位 ASan/UBSan 与 -m32） =="
if run_libtest t_norm ""; then rc=0; else rc=$?; fi
OUT="$TMP/t_norm/test/out64.txt"
if [ "$rc" -eq 0 ] && grep -q '^libTest 64/32 identical ok' "$TMP/t_norm.log" &&
   grep -q '^fail 0$' "$OUT" && grep -q '^done$' "$OUT"; then
    ok "M1 test 目标退出码 0、64/32 逐字节相同、fail 0、done（$(wc -l <"$OUT") 行）"
else
    bad "M1 test 目标退出码 $rc，或 64/32 不同，或没有 fail 0"
    tail -6 "$TMP/t_norm.log" | while read -r l; do note "$l"; done
fi
if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log"; then
    bad "M1s stderr 里有 sanitizer 报告"
    grep -m3 -E 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log" | while read -r l; do note "$l"; done
else
    ok "M1s stderr 里没有 sanitizer 报告"
fi
group_all_ok C 19 "普查"
grep -E '^C1 set-clip-state|^C2 ' "$OUT" | while read -r l; do note "$l"; done
group_all_ok S 4 "模拟时钟"
group_all_ok H 8 "帧时间直方图"
grep -E '^H2 990' "$OUT" | while read -r l; do note "$l"; done
group_all_ok Q 7 "日志队列"
grep -E '^Q3 ' "$OUT" | while read -r l; do note "$l"; done
group_all_ok N 10 "N_max 状态机"
grep -E '^N1 A=|^N4 A=' "$OUT" | while read -r l; do note "$l"; done

# ---------------------------------------------------------------- M2 普查的注错
echo "== M2 普查的注错（每个都要让指定的行 FAIL） =="
expect_red M2 CENSUS_NO_SETCLIP '^C1 set-clip-state'
expect_red M2 CENSUS_NO_SURF    '^C2 300 sequences'

# ---------------------------------------------------------------- M3 直方图与模拟时钟的注错
echo "== M3 直方图与模拟时钟的注错（每个都要让指定的行 FAIL） =="
expect_red M3 FHIST_P99_LOWER '^H2 990x1.0M'
expect_red M3 FHIST_DROP_OVER '^H3 p99 in last bucket'
expect_red M3 COST_NO_B       '^S1 hand example'

# ---------------------------------------------------------------- M4 日志队列的注错
echo "== M4 日志队列的注错（每个都要让指定的行 FAIL） =="
expect_red M4 LOGQ_SPLIT '^Q1 3x20 in'
expect_red M4 LOGQ_WRAP  '^Q2 across the end'

# ---------------------------------------------------------------- M5 board_tx_room（PC）
echo "== M5 board_tx_room 的 PC 实现（主机探针，链真实的 bsp/host/board.c） =="
# 选项与 mk/host.mk 编 bsp 的那一条相同：WARN 从 host.mk 里取，SDL 头按系统头引入
FLAGS="$(make -C "$SW" -s --no-print-directory -f mk/host.mk OBJDIR="$TMP/flags" \
         --eval 'sw8_print_flags: ; @echo -std=gnu11 -O$(OPT) -g $(WARN)' sw8_print_flags 2>/dev/null)"
SDL_CF="$(sdl2-config --cflags | sed 's/-I/-isystem /g')"
build_tx() {   # $1 = 产物名，$2 = 追加的 -D（可空）
    # shellcheck disable=SC2086
    "$HOST_CC" $FLAGS $2 -I"$SW/bsp/include" $SDL_CF -DGFX2D_ASSETS_PAK_DEFAULT='""' \
        -o "$TMP/$1" "$TX_C" "$SW/bsp/host/board.c" "$SW/bsp/memmap.c" $(sdl2-config --libs) \
        >"$TMP/$1.build.log" 2>&1
}
if build_tx tx "" && build_tx tx_inj "-DINJECT_HOST_TX_NO_REFILL"; then
    env -u DISPLAY -u WAYLAND_DISPLAY -u GFX2D_TX_BUDGET SDL_VIDEODRIVER=dummy \
        "$TMP/tx" >"$TMP/tx0.out" 2>"$TMP/tx0.err"
    if grep -q '^X1 .* ok$' "$TMP/tx0.out" && grep -q '^fail 0$' "$TMP/tx0.out" &&
       ! grep -q 'tx budget' "$TMP/tx0.err"; then
        ok "M5a 不设 GFX2D_TX_BUDGET：不限，退出时不打汇总"
    else
        bad "M5a 不设 GFX2D_TX_BUDGET 时应不限"
        cat "$TMP/tx0.out" "$TMP/tx0.err" | head -5 | while read -r l; do note "$l"; done
    fi

    env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEODRIVER=dummy GFX2D_TX_BUDGET=192 \
        "$TMP/tx" >"$TMP/tx1.out" 2>"$TMP/tx1.err"
    tries="$(sed -n 's/^X2 tries \([0-9]*\)$/\1/p' "$TMP/tx1.out")"
    if grep -q '^X2 .* ok$' "$TMP/tx1.out" && grep -q '^X3 .* ok$' "$TMP/tx1.out" &&
       grep -q '^fail 0$' "$TMP/tx1.out"; then
        ok "M5b GFX2D_TX_BUDGET=192：按场补满、不累加、超额归零"
    else
        bad "M5b GFX2D_TX_BUDGET=192 的额度模拟不对"
        grep -E '^X|^fail' "$TMP/tx1.out" | while read -r l; do note "$l"; done
    fi
    if [ "$tries" = "1" ] && grep -q 'tx budget 192 B/field, over 1 times 18 bytes' "$TMP/tx1.err"; then
        ok "M5c 退出时的超额汇总：$(head -1 "$TMP/tx1.err")"
    elif [ -n "$tries" ] && [ "$tries" != "1" ] && grep -q 'tx budget 192 B/field' "$TMP/tx1.err"; then
        ok "M5c 退出时有超额汇总（X2 重试了 $tries 次，次数不核）"
    else
        bad "M5c 退出时的超额汇总对不上"
        head -3 "$TMP/tx1.err" | while read -r l; do note "$l"; done
    fi

    env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEODRIVER=dummy GFX2D_TX_BUDGET=192 \
        "$TMP/tx_inj" >"$TMP/tx2.out" 2>"$TMP/tx2.err"
    if grep -q '^X3 .* FAIL$' "$TMP/tx2.out"; then
        ok "M5i INJECT=HOST_TX_NO_REFILL 见红：$(grep -m1 '^X3' "$TMP/tx2.out")"
    else
        bad "M5i INJECT=HOST_TX_NO_REFILL 没有让 X3 见红"
    fi
else
    bad "M5 探针编不过"
    tail -5 "$TMP"/tx*.build.log | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- M6 板上构建
echo "== M6 板上构建（bsp/ti60 的 board_tx_room） =="
if command -v "${RISCV_BIN}gcc" >/dev/null 2>&1; then
    # gfx2dGame 包含生成物 assets.h，素材包与头文件都不入库：干净克隆里不先打包，
    # 这一次构建会因为找不到 assets.h 而失败，而那是假红（与 check_sw_layers.sh 同一做法）。
    if ! python3 "$MAIN/tools/assets/pack_assets.py" >"$TMP/pack.log" 2>&1; then
        bad "M6 素材打包失败，板上构建会找不到 assets.h"
        sed -n '1,5p' "$TMP/pack.log" | while read -r l; do note "$l"; done
    elif make -C "$SW/apps/game/gfx2dGame" -s RISCV_BIN="$RISCV_BIN" OBJDIR="$TMP/b_ti60" \
            >"$TMP/b_ti60.log" 2>&1 &&
       ! grep -qiE 'warning|error' "$TMP/b_ti60.log" &&
       "${RISCV_BIN}nm" "$TMP/b_ti60/bsp/ti60/board.o" 2>/dev/null | grep -q ' T board_tx_room$'; then
        ok "M6 gfx2dGame 板上构建零告警，bsp/ti60/board.o 里有 board_tx_room（bin $(stat -c %s "$TMP"/b_ti60/gfx2dGame-O2.bin) 字节）"
    else
        bad "M6 板上构建失败、有告警，或 bsp/ti60 里没有 board_tx_room"
        grep -iE 'warning|error' "$TMP/b_ti60.log" | head -5 | while read -r l; do note "$l"; done
    fi
else
    note "M6 跳过：RISCV_BIN=$RISCV_BIN 找不到交叉编译器（跳过不算通过）"
fi
if command -v "${RISCV_BIN}gcc" >/dev/null 2>&1; then
    if make -C "$APP" -s RISCV_BIN="$RISCV_BIN" OBJDIR="$TMP/b_stg3" EXTRA_CFLAGS="-DSTG_FIELD=3" \
            >"$TMP/b_stg3.log" 2>&1 && [ -f "$TMP/b_stg3/gfx2dGame-O2.bin" ] &&
       ! grep -qiE 'warning|error' "$TMP/b_stg3.log"; then
        ok "M6b STG_FIELD=3 板上构建零告警（bin $(stat -c %s "$TMP/b_stg3/gfx2dGame-O2.bin") 字节）"
    else
        bad "M6b STG_FIELD=3 板上构建失败或有告警"
        grep -iE 'warning|error' "$TMP/b_stg3.log" | head -5 | while read -r l; do note "$l"; done
    fi
fi

# ---------------------------------------------------------------- M7 N_max 状态机的注错
echo "== M7 N_max 状态机的注错（每个都要让指定的行 FAIL） =="
expect_red M7 NMAX_BISECT_SIDE '^N1 six models'
expect_red M7 NMAX_NO_STEPBACK '^N4 one spike'
expect_red M7 NMAX_NO_SETTLE   '^N1 six models'

# ---------------------------------------------------------------- M8 搜索跑在弹幕场上
echo "== M8 搜索跑在弹幕场上（gfx2dGame 的 app 测试，64 位 ASan/UBSan 与 -m32） =="
# 选项从 mk/host.mk 取，与 check_sw7_game.sh 的 G4 同一做法
FLAGS="$(make -C "$SW" -s --no-print-directory -f mk/host.mk OBJDIR="$TMP/flags" \
         --eval 'sw8_print_flags: ; @echo -std=gnu11 -O$(OPT) -g $(WARN)' sw8_print_flags 2>/dev/null)"
build_app() {   # $1 = 产物名，$2 = 64|32
    local extra="-m32"
    [ "$2" = "64" ] && extra="-fsanitize=address,undefined"
    # shellcheck disable=SC2086
    "$HOST_CC" $FLAGS $extra -I"$SW/lib" -I"$APP/src" -o "$TMP/$1" \
        "$APP"/test/*.c "$APP"/src/game/stg_*.c "$SW"/lib/core/*.c "$SW"/lib/gfx/*.c -lm \
        >"$TMP/$1.build.log" 2>&1
}
if [ -n "$FLAGS" ] && build_app app64 64 && build_app app32 32; then
    "$TMP/app64" >"$TMP/app64.txt" 2>"$TMP/app64.err"; r64=$?
    "$TMP/app32" >"$TMP/app32.txt" 2>&1; r32=$?
    ny_ok=$(grep -cE '^Y[0-9] .* ok$' "$TMP/app64.txt")
    ny_bad=$(grep -cE '^Y[0-9] .* FAIL$' "$TMP/app64.txt")
    if [ "$r64" -eq 0 ] && [ "$r32" -eq 0 ] && cmp -s "$TMP/app64.txt" "$TMP/app32.txt" &&
       grep -q '^fail 0$' "$TMP/app64.txt" && [ "$ny_ok" -eq 9 ] && [ "$ny_bad" -eq 0 ]; then
        ok "M8 app 测试 64/32 逐字节相同、fail 0；Y 组 9 行全 ok"
        grep -E '^Y (16x16|8x8) (search|verify)' "$TMP/app64.txt" | while read -r l; do note "$l"; done
    else
        bad "M8 退出码 64:$r64 32:$r32、64/32 不同、没有 fail 0，或 Y 组 ok $ny_ok 行、FAIL $ny_bad 行"
        grep -E 'FAIL' "$TMP/app64.txt" | head -5 | while read -r l; do note "$l"; done
    fi
    if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/app64.err"; then
        bad "M8s app 测试有 sanitizer 报告"
        head -3 "$TMP/app64.err" | while read -r l; do note "$l"; done
    fi
else
    bad "M8 app 测试编不过（或取不到 mk/host.mk 的选项）"
    tail -5 "$TMP"/app*.build.log 2>/dev/null | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- M9 STG_FIELD=3 的 PC 驱动
echo "== M9 STG_FIELD=3 的 PC 驱动（模拟时钟，无窗口） =="
if ! python3 "$MAIN/tools/assets/pack_assets.py" >"$TMP/pack9.log" 2>&1; then
    bad "M9 素材打包失败"
elif make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/h_stg3" EXTRA_CFLAGS="-DSTG_FIELD=3" \
        >"$TMP/h_stg3.log" 2>&1 && [ -x "$TMP/h_stg3/gfx2dGame" ]; then
    # board_puts 的行尾是 \r\n，比之前去掉 \r
    SDL_VIDEODRIVER=dummy "$TMP/h_stg3/gfx2dGame" 2>"$TMP/stg3.err" | tr -d '\r' >"$TMP/stg3.log"
    r=${PIPESTATUS[0]}
    ncfg=$(grep -c '^nmax cfg ' "$TMP/stg3.log")
    nres=$(grep -c '^nmax result OK ' "$TMP/stg3.log")
    if [ "$r" -eq 0 ] && [ "$ncfg" -eq 2 ] && [ "$nres" -eq 2 ] &&
       grep -q '^fail 0$' "$TMP/stg3.log" && grep -q '^done$' "$TMP/stg3.log" &&
       [ "$(grep '^nmax result' "$TMP/stg3.log")" = "$(grep '^nmax result' "$TMP/app64.txt" 2>/dev/null)" ]; then
        ok "M9 两遍搜索都 OK、fail 0，结果行与 app 测试逐字相同（$(grep -c '^nmax probe' "$TMP/stg3.log") 行轨迹）"
        grep -E '^nmax (load|result)' "$TMP/stg3.log" | while read -r l; do note "$l"; done
    else
        bad "M9 退出码 $r、cfg 行 $ncfg、OK 结果 $nres，或没有 fail 0，或结果行与 app 测试不同"
        grep -E '^nmax result|^err|^fail' "$TMP/stg3.log" | head -5 | while read -r l; do note "$l"; done
    fi
else
    bad "M9 STG_FIELD=3 的 PC 版编不过"
    tail -5 "$TMP/h_stg3.log" | while read -r l; do note "$l"; done
fi

printf 'fail %d\ndone\n' "$FAIL"
[ "$FAIL" -eq 0 ]
