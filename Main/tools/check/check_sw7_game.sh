#!/usr/bin/env bash
# check_sw7_game.sh —— 游戏本体（SW7）A、B 两批的提交前判据
#
# 判据见 Engineering/Todo/sw/SW7-游戏本体.md 第 3 节，口径见 2.1 节与 4.5 节。本脚本串起来跑：
#
#   G1 正弦表可重生成   gen_sin_tab.py 重打一份，与入库的 lib/core/sin_tab.c 逐字节相同；
#                       注错 --inject 改一项，比对必须见红
#   G2 主机测试         mk/host.mk 的 test 目标：64 位（ASan/UBSan）与 -m32 输出逐字节相同、fail 0，
#                       stderr 里没有 sanitizer 报告；数值基础（F 组）21 行、批量提交（K 组）8 行全 ok
#   G3 数值基础的注错   FX_IDX_TRUNC / FX_VEL_Q4 / FX_ATAN_NOSCALE / FX_ATAN_UNIT 各编一次，
#                       指定的那一行必须 FAIL
#   G4 实体池           apps/game/gfx2dGame/test/ 与 src/game/stg_*.c 按 test 目标的选项直接编：
#                       64 位挂 ASan/UBSan、-m32 各一份，两份输出逐字节相同、fail 0、无 sanitizer 报告；
#                       只链 lib/ 与 stg_*.c，不链 bsp
#   G5 实体池的注错     STG_POOL_STALL / STG_POOL_SWAP 各编一次，指定的那一行必须 FAIL
#   G6 批量提交的注错   BLIT_RUN_W7 / BLIT_RUN_SXY / BLIT_RUN_NO_FALLBACK 各编一次，指定的那一行必须 FAIL；
#                       最后一个要让「源等于目的」那一类见红，证明随机输入真的走到了逐条回退
#   G7 弹型             gen_stg_patterns.py（Python 独立实现）重打期望文件，与入库的
#                       apps/game/gfx2dGame/test/stg_patterns.txt 逐字节相同；G4 的两份产物各打一遍轨迹
#                       （stgTest pat），64/32 逐字节相同，再与期望文件先比参数行、再逐帧比、末行比溢出计数
#   G7i 弹型的注错      EMIT_SEQ_NOACC / EMIT_AIM_SWAP / EMIT_RAIN_ORDER 各编一次，
#                       比对必须在指定的那一段（spiral / aim / rain）报出第一个不同帧与第一颗不同的弹
#   G8 碰撞             G4 的输出里碰撞 X 组 13 行全 ok（边界、勾股数相切、大坐标差、擦弹一次、受击关掉、
#                       已标记跳过、随机对 64 位朴素参考、场里的自机狙段）
#   G8i 碰撞的注错      COLLIDE_LE 让相切那几行见红；COLLIDE_NO_CULL 让 X3 见红，且 UBSan 在 stderr 报平方溢出
#   G9 组帧的主机判据   G4 的输出里按池发出与组帧 W 组 11 行全 ok（剔除边界、分段扫 = 一次扫 = 独立重数、
#                       源坐标、六段各 300 帧的组帧：拒绝为 0、普查的 ALPHA 条数 = 发出的敌弹数）
#   G10 判据模式        gfx2dGame 以 -DSTG_FIELD=2 编 PC 版，无窗口（SDL_VIDEODRIVER=dummy）跑两遍：
#                       日志逐字节相同、fail 0、done、六段 840 帧
#   G11 板上构建与反汇编 先打包素材，再以 STG_FIELD=1、2 做板上构建，零告警；objdump 查 stg_pool_step、
#                       stg_pool_collide、stg_pool_draw 三个函数：没有 div/divu/rem/remu，没有 __div*、__mod* 调用，
#                       没有 ret 以外的 jalr/jr；注错 STG_LOOP_DIV 在更新循环里加一次除法，这条检查必须见红。
#                       要 RISCV_BIN=<RISCV_BIN>，找不到交叉工具链时跳过，**跳过不算通过**
#
# 用法（WSL 或 Linux，仓库根或任意位置）：
#     RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw7_game.sh
#
# 需要 gcc、gcc-multilib（-m32）、libsdl2-dev（mk/host.mk 解析时就要 sdl2-config）、python3 与 Pillow（打包素材）；
# G11 要交叉工具链。不需要板子。
# 全部通过时末尾是 fail 0 与 done，退出码 0。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"
GEN="$MAIN/tools/check/gen_sin_tab.py"
SIN_TAB="$SW/lib/core/sin_tab.c"
APP="$SW/apps/game/gfx2dGame"
HOST_CC="${HOST_CC:-gcc}"
RISCV_BIN="${RISCV_BIN:-riscv-none-elf-}"

TMP="$(mktemp -d)"
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

PAT_GEN="$MAIN/tools/check/gen_stg_patterns.py"
PAT_EXP="$APP/test/stg_patterns.txt"

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

# ---------------------------------------------------------------- G1 正弦表
echo "== G1 正弦表可重生成 =="
if python3 "$GEN" --out "$TMP/sin_tab.c" >"$TMP/gen.log" 2>&1; then
    if cmp -s "$TMP/sin_tab.c" "$SIN_TAB"; then
        ok "G1 重新生成的 sin_tab.c 与入库的逐字节相同（$(stat -c %s "$SIN_TAB") 字节）"
    else
        bad "G1 重新生成的 sin_tab.c 与入库的不同"
        diff "$TMP/sin_tab.c" "$SIN_TAB" | head -6 | while read -r l; do note "$l"; done
    fi
else
    bad "G1 gen_sin_tab.py 跑不动"
    head -5 "$TMP/gen.log" | while read -r l; do note "$l"; done
fi
if python3 "$GEN" --inject --out "$TMP/sin_tab_inj.c" >>"$TMP/gen.log" 2>&1 &&
   ! cmp -s "$TMP/sin_tab_inj.c" "$SIN_TAB"; then
    ok "G1i 注错 改一项之后比对见红"
else
    bad "G1i 注错 改一项之后比对见红（比对没发现改动，这条判据是空转）"
fi

# ---------------------------------------------------------------- G2 主机测试
echo "== G2 主机测试（64 位 ASan/UBSan 与 -m32） =="
if run_libtest t_norm ""; then rc=0; else rc=$?; fi
out="$TMP/t_norm/test/out64.txt"
if [ "$rc" -eq 0 ] && grep -q '^libTest 64/32 identical ok' "$TMP/t_norm.log" &&
   grep -q '^fail 0$' "$out" && grep -q '^done$' "$out"; then
    ok "G2 test 目标退出码 0、64/32 逐字节相同、fail 0、done（$(wc -l <"$out") 行）"
else
    bad "G2 test 目标退出码 $rc，或 64/32 不同，或没有 fail 0"
    tail -6 "$TMP/t_norm.log" | while read -r l; do note "$l"; done
fi
if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log"; then
    bad "G2s stderr 里有 sanitizer 报告"
    grep -m3 -E 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log" | while read -r l; do note "$l"; done
else
    ok "G2s stderr 里没有 sanitizer 报告"
fi
nf_ok=$(grep -cE '^F[0-9] .* ok$' "$out" 2>/dev/null)
nf_bad=$(grep -cE '^F[0-9] .* FAIL$' "$out" 2>/dev/null)
if [ "$nf_ok" -eq 21 ] && [ "$nf_bad" -eq 0 ]; then
    ok "G2f 数值基础 21 行全 ok"
    grep -E '^F(2 sin_tab|2 sin\^2|4 atan2 \[|5 atan2 Q|6 ring)' "$out" | while read -r l; do note "$l"; done
else
    bad "G2f 数值基础应有 21 行 ok，实际 ok $nf_ok 行、FAIL $nf_bad 行"
fi
nk_ok=$(grep -cE '^K[0-9] .* ok$' "$out" 2>/dev/null)
nk_bad=$(grep -cE '^K[0-9] .* FAIL$' "$out" 2>/dev/null)
if [ "$nk_ok" -eq 8 ] && [ "$nk_bad" -eq 0 ]; then
    ok "G2k 批量提交 8 行全 ok"
    grep -E '^K1 (calls|coverage)|^K2 coverage' "$out" | while read -r l; do note "$l"; done
else
    bad "G2k 批量提交应有 8 行 ok，实际 ok $nk_ok 行、FAIL $nk_bad 行"
fi

# ---------------------------------------------------------------- G3 注错
echo "== G3 数值基础的注错（每个都要让指定的行 FAIL） =="
expect_red() {   # $1 = INJECT，$2 = 必须 FAIL 的行（grep -E，行首锚定）
    run_libtest "i_$1" "$1"
    if grep -qE "$2.* FAIL$" "$TMP/i_$1/test/out64.txt" 2>/dev/null; then
        ok "G3 INJECT=$1 见红：$(grep -m1 -E "$2.* FAIL$" "$TMP/i_$1/test/out64.txt")"
    else
        bad "G3 INJECT=$1 没有让「$2」见红"
    fi
}
expect_red FX_IDX_TRUNC    '^F3 index rounding'
expect_red FX_VEL_Q4       '^F6 ring 32 shots'
expect_red FX_ATAN_NOSCALE '^F5 atan2 Q\.8 random'
expect_red FX_ATAN_UNIT    '^F4 atan2 \[-512'

# ---------------------------------------------------------------- G4 实体池
echo "== G4 app 测试：实体池与发射器（64 位 ASan/UBSan 与 -m32，不链 bsp） =="
# 选项从 mk/host.mk 里取，不在这里另写一份：判据与被测构建必须用同一组参数
# （TechReports sw 第 01 章 4.6 节）。OBJDIR 指到临时目录，解析 host.mk 时的标记文件不落进仓库。
FLAGS="$(make -C "$SW" -s --no-print-directory -f mk/host.mk OBJDIR="$TMP/flags" \
         --eval 'sw7_print_flags: ; @echo -std=gnu11 -O$(OPT) -g $(WARN)' sw7_print_flags 2>/dev/null)"
if [ -z "$FLAGS" ]; then
    bad "G4 取不到 mk/host.mk 的编译选项"
fi
note "选项：$FLAGS"

# $1 = 产物名，$2 = 64|32，$3 = 追加的 -D（可空）
build_stg() {
    local extra=""
    [ "$2" = "64" ] && extra="-fsanitize=address,undefined"
    [ "$2" = "32" ] && extra="-m32"
    # shellcheck disable=SC2086
    "$HOST_CC" $FLAGS $extra $3 -I"$SW/lib" -I"$APP/src" -o "$TMP/$1" \
        "$APP"/test/*.c "$APP"/src/game/stg_*.c "$SW"/lib/core/*.c "$SW"/lib/gfx/*.c -lm \
        >"$TMP/$1.build.log" 2>&1
}

if build_stg stg64 64 "" && build_stg stg32 32 ""; then
    "$TMP/stg64" >"$TMP/stg64.txt" 2>"$TMP/stg64.err"; rc64=$?
    "$TMP/stg32" >"$TMP/stg32.txt" 2>"$TMP/stg32.err"; rc32=$?
    if [ "$rc64" -eq 0 ] && [ "$rc32" -eq 0 ] && grep -q '^fail 0$' "$TMP/stg64.txt" &&
       grep -q '^done$' "$TMP/stg64.txt" && cmp -s "$TMP/stg64.txt" "$TMP/stg32.txt"; then
        ok "G4 app 测试 64/32 逐字节相同、fail 0、done（$(grep -cE ' ok$' "$TMP/stg64.txt") 行 ok）"
        grep -E '^P2 totals' "$TMP/stg64.txt" | while read -r l; do note "$l"; done
    else
        bad "G4 app 测试退出码 64:$rc64 32:$rc32，或没有 fail 0，或 64/32 输出不同"
        grep -E 'FAIL|^fail' "$TMP/stg64.txt" | head -5 | while read -r l; do note "$l"; done
    fi
    if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/stg64.err"; then
        bad "G4s app 测试有 sanitizer 报告"
        head -3 "$TMP/stg64.err" | while read -r l; do note "$l"; done
    else
        ok "G4s app 测试没有 sanitizer 报告"
    fi
else
    bad "G4 app 测试编不过"
    tail -5 "$TMP"/stg*.build.log | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- G5 实体池的注错
echo "== G5 实体池的注错（每个都要让指定的行 FAIL） =="
expect_stg_red() {   # $1 = INJECT，$2 = 必须 FAIL 的行
    if build_stg "stg_$1" 64 "-DINJECT_$1"; then
        "$TMP/stg_$1" >"$TMP/stg_$1.txt" 2>&1
        if grep -qE "$2.* FAIL$" "$TMP/stg_$1.txt"; then
            ok "G5 INJECT=$1 见红：$(grep -m1 -E "$2.* FAIL$" "$TMP/stg_$1.txt")"
        else
            bad "G5 INJECT=$1 没有让「$2」见红"
        fi
    else
        bad "G5 INJECT=$1 编不过"
    fi
}
expect_stg_red STG_POOL_STALL '^P2 300 rounds: removed == oob'
expect_stg_red STG_POOL_SWAP  '^P3 300 rounds: survivors keep'

# ---------------------------------------------------------------- G6 批量提交的注错
echo "== G6 批量提交的注错（每个都要让指定的行 FAIL） =="
run_libtest_red() {   # 与 G3 同一个做法，只是换一组行
    run_libtest "i_$1" "$1"
    if grep -qE "$2.* FAIL$" "$TMP/i_$1/test/out64.txt" 2>/dev/null; then
        ok "G6 INJECT=$1 见红：$(grep -m1 -E "$2.* FAIL$" "$TMP/i_$1/test/out64.txt" | cut -c1-72)"
    else
        bad "G6 INJECT=$1 没有让「$2」见红"
    fi
}
run_libtest_red BLIT_RUN_W7          '^K1 1000 groups'
run_libtest_red BLIT_RUN_SXY         '^K1 1000 groups'
run_libtest_red BLIT_RUN_NO_FALLBACK '^K1 src==dst groups'

# ---------------------------------------------------------------- G7 弹型
echo "== G7 弹型（期望文件由 Python 独立实现生成） =="
if python3 "$PAT_GEN" --out "$TMP/stg_patterns.txt" >"$TMP/patgen.log" 2>&1; then
    if cmp -s "$TMP/stg_patterns.txt" "$PAT_EXP"; then
        ok "G7 重新生成的期望文件与入库的逐字节相同（$(wc -l <"$PAT_EXP") 行）"
    else
        bad "G7 重新生成的期望文件与入库的不同"
        diff "$TMP/stg_patterns.txt" "$PAT_EXP" | head -6 | while read -r l; do note "$l"; done
    fi
else
    bad "G7 gen_stg_patterns.py 跑不动"
    head -5 "$TMP/patgen.log" | while read -r l; do note "$l"; done
fi

if [ -x "$TMP/stg64" ] && [ -x "$TMP/stg32" ]; then
    "$TMP/stg64" pat >"$TMP/pat64.txt" 2>"$TMP/pat64.err"
    "$TMP/stg32" pat >"$TMP/pat32.txt" 2>&1
    if cmp -s "$TMP/pat64.txt" "$TMP/pat32.txt"; then
        ok "G7m 轨迹 64/32 逐字节相同（$(wc -l <"$TMP/pat64.txt") 行）"
    else
        bad "G7m 轨迹 64/32 不同"
    fi
    if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/pat64.err"; then
        bad "G7s 轨迹有 sanitizer 报告"
        head -3 "$TMP/pat64.err" | while read -r l; do note "$l"; done
    fi
    if python3 "$PAT_GEN" --compare "$TMP/pat64.txt" --expect "$PAT_EXP" --c-bin "$TMP/stg64" \
           >"$TMP/patcmp.txt" 2>&1; then
        ok "G7c C 的轨迹与期望文件逐行一致：参数行、六段各 120 帧、溢出计数"
        awk '/^seg /{s=$3} /^f 119 /{printf "    %-6s 第 119 帧存活 %s\n", s, $3}' "$PAT_EXP"
    else
        bad "G7c C 的轨迹与期望文件不一致"
        head -30 "$TMP/patcmp.txt" | while read -r l; do note "$l"; done
    fi
else
    bad "G7c 没有 G4 的产物，轨迹没跑"
fi

echo "== G7i 弹型的注错（比对必须在指定的段见红） =="
expect_pat_red() {   # $1 = INJECT，$2 = 必须第一个报出不一致的段名
    if build_stg "pat_$1" 64 "-DINJECT_$1"; then
        "$TMP/pat_$1" pat >"$TMP/pat_$1.txt" 2>&1
        if ! python3 "$PAT_GEN" --compare "$TMP/pat_$1.txt" --expect "$PAT_EXP" --c-bin "$TMP/pat_$1" \
               >"$TMP/pat_$1.cmp" 2>&1 &&
           [ "$(grep -m1 -oE '^seg [0-9] [a-z]+' "$TMP/pat_$1.cmp")" = "$(grep -m1 -oE "^seg [0-9] $2" "$PAT_EXP")" ] &&
           grep -q '第一颗不同的弹' "$TMP/pat_$1.cmp"; then
            ok "G7i INJECT=$1 见红：$(grep -m1 -E '^seg [0-9] ' "$TMP/pat_$1.cmp")"
            grep -m1 -A2 '第一颗不同的弹' "$TMP/pat_$1.cmp" | while read -r l; do note "$l"; done
        else
            bad "G7i INJECT=$1 没有在 $2 段见红"
            head -4 "$TMP/pat_$1.cmp" | while read -r l; do note "$l"; done
        fi
    else
        bad "G7i INJECT=$1 编不过"
    fi
}
expect_pat_red EMIT_SEQ_NOACC  spiral
expect_pat_red EMIT_AIM_SWAP   aim
expect_pat_red EMIT_RAIN_ORDER rain

# ---------------------------------------------------------------- G8 碰撞
echo "== G8 碰撞（自机判定点对敌弹） =="
nx_ok=$(grep -cE '^X[0-9] .* ok$' "$TMP/stg64.txt" 2>/dev/null)
nx_bad=$(grep -cE '^X[0-9] .* FAIL$' "$TMP/stg64.txt" 2>/dev/null)
if [ "$nx_ok" -eq 13 ] && [ "$nx_bad" -eq 0 ]; then
    ok "G8 碰撞 13 行全 ok（64/32 同 G4）"
    grep -E '^X(7 totals|8 aim 240)' "$TMP/stg64.txt" | while read -r l; do note "$l"; done
else
    bad "G8 碰撞应有 13 行 ok，实际 ok $nx_ok 行、FAIL $nx_bad 行"
fi

echo "== G8i 碰撞的注错 =="
expect_collide_red() {   # $1 = INJECT，$2 = 必须 FAIL 的行，$3 = 非空时 stderr 里还必须有 UBSan 的 runtime error
    if build_stg "col_$1" 64 "-DINJECT_$1"; then
        "$TMP/col_$1" >"$TMP/col_$1.txt" 2>"$TMP/col_$1.err"
        if grep -qE "$2.* FAIL$" "$TMP/col_$1.txt" &&
           { [ -z "$3" ] || grep -q 'runtime error' "$TMP/col_$1.err"; }; then
            ok "G8i INJECT=$1 见红：$(grep -m1 -E "$2.* FAIL$" "$TMP/col_$1.txt" | cut -c1-80)"
            [ -n "$3" ] && note "$(grep -m1 'runtime error' "$TMP/col_$1.err" | sed 's/.*runtime error/runtime error/')"
        else
            bad "G8i INJECT=$1 没有让「$2」见红${3:+，或 stderr 里没有 UBSan 报告}"
        fi
    else
        bad "G8i INJECT=$1 编不过"
    fi
}
expect_collide_red COLLIDE_LE      '^X1 distance == r_p \+ r_b \(1536\)' ""
expect_collide_red COLLIDE_NO_CULL '^X3 differences near field size'     ubsan

# ---------------------------------------------------------------- G9 组帧（主机）
echo "== G9 按池发出与组帧（app 测试的 W 组） =="
nw_ok=$(grep -cE '^W[0-9] .* ok$' "$TMP/stg64.txt" 2>/dev/null)
nw_bad=$(grep -cE '^W[0-9] .* FAIL$' "$TMP/stg64.txt" 2>/dev/null)
if [ "$nw_ok" -eq 11 ] && [ "$nw_bad" -eq 0 ]; then
    ok "G9 按池发出与组帧 11 行全 ok（64/32 同 G4）"
    grep -E '^W(2|4) totals' "$TMP/stg64.txt" | while read -r l; do note "$l"; done
else
    bad "G9 按池发出与组帧应有 11 行 ok，实际 ok $nw_ok 行、FAIL $nw_bad 行"
fi

# ---------------------------------------------------------------- G10 判据模式
echo "== G10 弹幕场的判据模式（PC，无窗口两遍） =="
# gfx2dGame 包含生成物 assets.h，干净克隆里要先打包（SW8 4.8 节的教训）
if ! python3 "$MAIN/tools/assets/pack_assets.py" >"$TMP/pack.log" 2>&1; then
    bad "G10 素材打包失败"
    sed -n '1,5p' "$TMP/pack.log" | while read -r l; do note "$l"; done
elif make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/h_stg2" EXTRA_CFLAGS="-DSTG_FIELD=2" \
        >"$TMP/h_stg2.log" 2>&1 && [ -x "$TMP/h_stg2/gfx2dGame" ]; then
    # board_puts 的行尾是 \r\n（与板上串口一致），比之前去掉 \r
    SDL_VIDEODRIVER=dummy "$TMP/h_stg2/gfx2dGame" 2>"$TMP/stg_j1.err" | tr -d '\r' >"$TMP/stg_j1.log"
    r1=${PIPESTATUS[0]}
    SDL_VIDEODRIVER=dummy "$TMP/h_stg2/gfx2dGame" 2>"$TMP/stg_j2.err" | tr -d '\r' >"$TMP/stg_j2.log"
    r2=${PIPESTATUS[0]}
    nf=$(grep -c '^f ' "$TMP/stg_j1.log")
    nseg=$(grep -c '^seg ' "$TMP/stg_j1.log")
    if [ "$r1" -eq 0 ] && [ "$r2" -eq 0 ] && cmp -s "$TMP/stg_j1.log" "$TMP/stg_j2.log" &&
       [ "$(head -1 "$TMP/stg_j1.log")" = "gfx2dGame STG" ] &&
       grep -q '^fail 0$' "$TMP/stg_j1.log" && grep -q '^done$' "$TMP/stg_j1.log" &&
       [ "$nf" -eq 840 ] && [ "$nseg" -eq 6 ] && ! grep -q '^err ' "$TMP/stg_j1.log"; then
        ok "G10 两遍日志逐字节相同、fail 0、done（$nseg 段、$nf 帧，$(wc -l <"$TMP/stg_j1.log") 行）"
        grep -E '^f (119|359|839) ' "$TMP/stg_j1.log" | while read -r l; do note "$l"; done
    else
        bad "G10 退出码 $r1/$r2、两遍不同、没有 fail 0，或帧数 $nf、段数 $nseg 不对"
        grep -E '^err |^fail' "$TMP/stg_j1.log" | head -5 | while read -r l; do note "$l"; done
    fi
else
    bad "G10 STG_FIELD=2 的 PC 版编不过"
    tail -5 "$TMP/h_stg2.log" | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- G11 板上构建与反汇编
echo "== G11 板上构建（STG_FIELD=1、2）与三个循环的反汇编 =="
# $1 = elf，$2 = 函数名：打出这个函数的反汇编（不带机器码），找不到时为空
fn_body() {
    "${RISCV_BIN}objdump" -d --no-show-raw-insn "$1" 2>/dev/null |
        awk -v f="<$2>:" '$2 == f { p = 1; next } p && NF == 0 { exit } p { print }'
}
# 从函数体里挑出违规的指令：除法与取模、除法库调用、ret 以外的间接跳转
loop_offenders() {
    awk -F'\t' '{ m = $2; sub(/[ \t]+$/, "", m) }
                m ~ /^(div|divu|rem|remu)$/ || m == "jalr" || m == "jr" || $0 ~ /<__(u)?(div|mod)/ { print }'
}
LOOPS="stg_pool_step stg_pool_collide stg_pool_draw"
if command -v "${RISCV_BIN}gcc" >/dev/null 2>&1; then
    for m in 1 2; do
        if make -C "$APP" -s RISCV_BIN="$RISCV_BIN" OBJDIR="$TMP/b_stg$m" EXTRA_CFLAGS="-DSTG_FIELD=$m" \
                >"$TMP/b_stg$m.log" 2>&1 && [ -f "$TMP/b_stg$m/gfx2dGame-O2.bin" ] &&
           ! grep -qiE 'warning|error' "$TMP/b_stg$m.log"; then
            ok "G11 STG_FIELD=$m 板上构建零告警（bin $(stat -c %s "$TMP/b_stg$m/gfx2dGame-O2.bin") 字节）"
        else
            bad "G11 STG_FIELD=$m 板上构建失败或有告警"
            grep -iE 'warning|error' "$TMP/b_stg$m.log" | head -5 | while read -r l; do note "$l"; done
        fi
    done

    elf="$TMP/b_stg2/gfx2dGame-O2.elf"
    for fn in $LOOPS; do
        body="$(fn_body "$elf" "$fn")"
        n=$(printf '%s\n' "$body" | grep -c .)
        offend="$(printf '%s\n' "$body" | loop_offenders)"
        if [ "$n" -gt 0 ] && [ -z "$offend" ]; then
            ok "G11 $fn：$n 条指令，没有除法、取模与间接跳转"
        else
            bad "G11 $fn：没找到函数体（$n 条），或有违规指令"
            printf '%s\n' "$offend" | head -3 | while read -r l; do note "$l"; done
        fi
    done

    # 注错：更新循环里加一次除法，上面这条检查必须看得见
    if make -C "$APP" -s RISCV_BIN="$RISCV_BIN" OBJDIR="$TMP/b_div" EXTRA_CFLAGS="-DSTG_FIELD=2" \
            INJECT=STG_LOOP_DIV >"$TMP/b_div.log" 2>&1; then
        elf_div="$(ls "$TMP"/b_div/*.elf 2>/dev/null | head -1)"
        offend="$(fn_body "$elf_div" stg_pool_step | loop_offenders)"
        if [ -n "$offend" ]; then
            ok "G11i INJECT=STG_LOOP_DIV 见红：stg_pool_step 里查到 $(printf '%s\n' "$offend" | head -1 | awk -F'\t' '{print $2}' | tr -d ' ')"
        else
            bad "G11i INJECT=STG_LOOP_DIV 没有见红（反汇编检查看不见除法，这条判据是空转）"
        fi
    else
        bad "G11i INJECT=STG_LOOP_DIV 的板上构建失败"
        tail -3 "$TMP/b_div.log" | while read -r l; do note "$l"; done
    fi
else
    note "G11 跳过：RISCV_BIN=$RISCV_BIN 找不到交叉编译器（跳过不算通过）"
fi

printf 'fail %d\ndone\n' "$FAIL"
[ "$FAIL" -eq 0 ]
