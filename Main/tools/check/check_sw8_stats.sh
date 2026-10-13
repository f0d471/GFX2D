#!/usr/bin/env bash
# check_sw8_stats.sh —— 演示模式与统计埋点的提交前判据
#
# 各项判据与口径列在下面。本脚本串起来跑：
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
#   M10 模式切换        默认构建（模式框架）以 GFX2D_START_MODE=1、2、3 冷启动各跑一帧取进入行的整帧 CRC；
#                       -DGFX2D_SWITCH_JUDGE=1 的合成按键来回切 150 次：151 行进入行的 CRC 与同模式的冷启动相同，
#                       模式 1、2 进来时四个池都是 0，裁剪框复位，switch done 150、fail 0
#   M10i 切换的注错     INJECT=MODE_KEEP_POOL（重置游戏状态时漏清敌弹池），进入行必须有不符的
#   M11 系统键和弦      M8 的 app 测试输出里 H 组 5 行全 ok；注错 CHORD_RETRIGGER 必须让 H1 的「不再触发」见红
#   M12 模式 3          默认构建以 GFX2D_START_MODE=3、GFX2D_PRESS_EXIT=1 无窗口跑终符上的两遍搜索（模拟时钟）：
#                       两行 nmax cfg（SETTLE_MIN=300）、两行 nmax result OK、nmax done、fail 0。
#                       同一产物同时跑两个进程，两份日志的 nmax 行逐字节相同（模拟时钟下结果不随墙钟变；
#                       csv 行里是真实周期数，本来就不同）。本机约 7 分钟
#   M13 统计 CSV 对账   默认构建以 GFX2D_START_MODE=1、2、3 各跑 600 帧（无窗口、不等场）：脚本自己解析 csv 行再核一遍——
#                       表头逐字相同、每行 30 列、t 从 0 连续、cmd_eq 之和加 cmd_other 等于 cmd、pix_eq 之和等于 pixel，
#                       没有被别的日志夹断的行；csvsum 的 rows 等于 csv 行数、lines_dropped 0；三个模式都出过行，应用 fail 0
#   M13i CSV 的注错     INJECT=CSV_DROP_EQ（打印时漏掉方程 1 那两列），脚本的解析必须见红（应用按内部值自查，看不见）
#   M14 日志不阻塞长跑  默认构建以 GFX2D_START_MODE=2、GFX2D_FRAMES=36000 编，GFX2D_TX_BUDGET=192、无窗口、
#                       按真实 60 Hz 节拍跑（约 10 分钟，600 帧之后自动驾驶接管）：csvsum 的 lines_dropped 0、fail 0、没有 err 行，
#                       bsp 退出时的超额汇总是 over 0 times 0 bytes。SW8_SKIP_LONG=1 跳过，**跳过不算通过**
#   M15 自动驾驶的 app 测试
#                       M8 的 app 测试输出里 A 组 8 行全 ok（接管与交还、偏好与平局、躲弹、打一局两遍相同；
#                       危险度扫描对朴素参考的比对在实体池测试里）；注错 AUTO_ORDER（候选顺序倒过来）必须让 A2 的平局那一行见红
#   M16 自动驾驶打一局  gfx2dGame 以 -DSTG_FIELD=4 -DSTG_JUDGE_AUTO=1 编 PC 版（输入换成自动驾驶、受击打开），
#                       无窗口、GFX2D_UNPACED=1 跑两遍：seg 与 g 行逐字节相同（CRC 序列可复现）、fail 0、done、没有 err 行；
#                       打出活到哪一段与每帧平均开销（PC 上的周期数只作登记）
#   M16i 自动驾驶的注错 INJECT=AUTO_ORDER 编同一个入口：g 行的 CRC 序列必须与 M16 不同
#   M17 出图与第一版统计 入库的四份样例日志 apps/game/gfx2dGame/logs/sw8_stats_{demo,game,s1,press}.log
#                       逐行对账（同 M13 的解析）；Main/tools/stats/plot_stats.py 按固定参数出三张图，
#                       三份图源与 Deliverables/Assert/figures/src/sw8_*.html 逐字节相同（入库的图就是这几份日志画的）
#   M17i 出图的注错     不给搜索日志时必须报 missing 1、退出码非 0、不出搜索轨迹图
#
# 用法（WSL 或 Linux，仓库根或任意位置）：
#     RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw8_stats.sh
#
# 需要 gcc、gcc-multilib（-m32）、libsdl2-dev（mk/host.mk 解析时就要 sdl2-config）与 python3；
# M6 要交叉工具链（doc/开发环境.md 第 11 节）。全部通过时末尾是 fail 0 与 done，退出码 0。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"
source "$MAIN/tools/check/check_common.sh"
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
    check_libtest "$TMP/$1" "$2"
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
    check_missing "M6 RISCV_BIN=$RISCV_BIN 找不到交叉编译器"
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
# 选项从 mk/host.mk 取，与 check_sw7_game.sh 的实体池项同一做法
FLAGS="$(make -C "$SW" -s --no-print-directory -f mk/host.mk OBJDIR="$TMP/flags" \
         --eval 'sw8_print_flags: ; @echo -std=gnu11 -O$(OPT) -g $(WARN)' sw8_print_flags 2>/dev/null)"
if [ -n "$FLAGS" ] && check_app_test "$TMP"; then
    r64=0
    r32=0
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

# ---------------------------------------------------------------- M10 模式切换
echo "== M10 模式切换（默认构建的模式框架，合成按键来回切 150 次） =="
# 冷启动：GFX2D_START_MODE=1、2、3 各跑一帧，取进入行的整帧 CRC
cold_ok=1
for m in 1 2 3; do
    if make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/cold$m" \
            EXTRA_CFLAGS="-DGFX2D_START_MODE=$m -DGFX2D_FRAMES=1" >"$TMP/cold$m.build.log" 2>&1; then
        SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/cold$m/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/cold$m.log"
        eval "cold_crc_$m=\"$(awk '/^mode /{print $7; exit}' "$TMP/cold$m.log")\""
    else
        cold_ok=0
    fi
done
# $1 = 日志：每一行进入行的 CRC 与同模式的冷启动相同，模式 1、2 进来时四个池都是 0，裁剪框复位；打出不符的行数
switch_bad() {
    awk -v c1="$cold_crc_1" -v c2="$cold_crc_2" -v c3="$cold_crc_3" '
        /^mode / { want = ($2 == 1) ? c1 : ($2 == 2) ? c2 : c3
                   bad = ($7 != want) || ($14 != 0 || $15 != 0 || $16 != 639 || $17 != 479)
                   if ($2 != 3) bad = bad || ($9 != 0 || $10 != 0 || $11 != 0 || $12 != 0)
                   n += bad }
        END { print n + 0 }' "$1"
}
if [ "$cold_ok" -eq 1 ] && [ -n "$cold_crc_1" ] && [ -n "$cold_crc_2" ] && [ -n "$cold_crc_3" ] &&
   make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/swj" EXTRA_CFLAGS="-DGFX2D_SWITCH_JUDGE=1" \
        >"$TMP/swj.build.log" 2>&1; then
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/swj/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/swj.log"
    nent=$(grep -c '^mode ' "$TMP/swj.log")
    nbad=$(switch_bad "$TMP/swj.log")
    if grep -q '^switch done 150$' "$TMP/swj.log" && [ "$nent" -eq 151 ] && [ "$nbad" -eq 0 ] &&
       grep -q '^fail 0$' "$TMP/swj.log" && ! grep -q '^err ' "$TMP/swj.log"; then
        ok "M10 切 150 次、151 次进入：每次的第一帧与冷启动相同，模式 1、2 进来时池为 0，裁剪框复位，fail 0"
        note "冷启动 CRC 模式 1 $cold_crc_1、模式 2 $cold_crc_2、模式 3 $cold_crc_3"
        note "进入次数 模式 1 $(grep -c '^mode 1 ' "$TMP/swj.log")、模式 2 $(grep -c '^mode 2 ' "$TMP/swj.log")、模式 3 $(grep -c '^mode 3 ' "$TMP/swj.log")"
    else
        bad "M10 进入 $nent 次、不符 $nbad 次，或没有 switch done 150 / fail 0"
        grep -E '^err|^fail|^switch' "$TMP/swj.log" | head -4 | while read -r l; do note "$l"; done
    fi
else
    bad "M10 冷启动或切换判据的 PC 版编不过"
fi

echo "== M10i 模式切换的注错（重置时漏清敌弹池） =="
if make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/swji" EXTRA_CFLAGS="-DGFX2D_SWITCH_JUDGE=1" \
        INJECT=MODE_KEEP_POOL >"$TMP/swji.build.log" 2>&1; then
    bin="$(ls "$TMP"/swji/gfx2dGame* 2>/dev/null | head -1)"
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$bin" 2>/dev/null | tr -d '\r' >"$TMP/swji.log"
    nbad=$(switch_bad "$TMP/swji.log")
    if [ "$nbad" -gt 0 ]; then
        ok "M10i INJECT=MODE_KEEP_POOL 见红：$nbad 次进入与冷启动不符，第一处 $(awk -v c1="$cold_crc_1" '/^mode 1 / && ($9 != 0) {print "模式 1 进来时敌弹池 " $9; exit}' "$TMP/swji.log")"
    else
        bad "M10i INJECT=MODE_KEEP_POOL 没有见红（切换判据看不见漏清，这条判据是空转）"
    fi
else
    bad "M10i INJECT=MODE_KEEP_POOL 编不过"
fi

# ---------------------------------------------------------------- M11 和弦
echo "== M11 系统键和弦（app 测试的 H 组） =="
nh_ok=$(grep -cE '^H[0-9] .* ok$' "$TMP/app64.txt" 2>/dev/null)
nh_bad=$(grep -cE '^H[0-9] .* FAIL$' "$TMP/app64.txt" 2>/dev/null)
if [ "$nh_ok" -eq 5 ] && [ "$nh_bad" -eq 0 ]; then
    ok "M11 和弦 5 行全 ok（7 帧不触发、8 帧触发一次、不松开不再触发；64/32 同 M8）"
else
    bad "M11 和弦应有 5 行 ok，实际 ok $nh_ok 行、FAIL $nh_bad 行"
fi
if "$HOST_CC" $FLAGS -m32 -DINJECT_CHORD_RETRIGGER -I"$SW/lib" -I"$APP/src" -o "$TMP/chord_inj" \
        "$APP"/test/*.c "$APP"/src/game/stg_*.c "$SW"/lib/core/*.c "$SW"/lib/gfx/*.c -lm \
        >"$TMP/chord_inj.build.log" 2>&1; then
    "$TMP/chord_inj" >"$TMP/chord_inj.txt" 2>&1
    if grep -qE '^H1 held 200 more frames: no retrigger FAIL$' "$TMP/chord_inj.txt"; then
        ok "M11i INJECT=CHORD_RETRIGGER 见红：H1 held 200 more frames: no retrigger FAIL"
    else
        bad "M11i INJECT=CHORD_RETRIGGER 没有让 H1 的「不再触发」见红"
    fi
else
    bad "M11i INJECT=CHORD_RETRIGGER 编不过"
fi

# ---------------------------------------------------------------- M12 模式 3 在终符上搜索
echo "== M12 模式 3 在终符上搜索（默认构建，GFX2D_START_MODE=3，模拟时钟，无窗口） =="
if [ "$GFX_CHECK_PROFILE" = quick ]; then
    check_na "M12 生产模式完整 N_max 双进程搜索属于 acceptance/release；快速档保留 M7/M8/M9 状态机与弹幕场检查"
elif make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/m3" EXTRA_CFLAGS="-DGFX2D_START_MODE=3 -DGFX2D_PRESS_EXIT=1" \
        >"$TMP/m3.build.log" 2>&1; then
    # 两个进程同时跑：墙钟下的实测帧率各不相同，模拟时钟的结果若受它影响，两份日志就会分叉
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/m3/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/m3b.log" &
    m3b_pid=$!
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/m3/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/m3.log"
    wait "$m3b_pid"
    ncfg=$(grep -c '^nmax cfg .*SETTLE_MIN=300 ' "$TMP/m3.log")
    nres=$(grep -c '^nmax result OK ' "$TMP/m3.log")
    grep '^nmax' "$TMP/m3.log" >"$TMP/m3.nmax"
    grep '^nmax' "$TMP/m3b.log" >"$TMP/m3b.nmax"
    if ! cmp -s "$TMP/m3.nmax" "$TMP/m3b.nmax"; then
        bad "M12 同时跑的两个进程 nmax 行不同（模拟时钟的结果随墙钟变了）"
        diff "$TMP/m3.nmax" "$TMP/m3b.nmax" | head -4 | while read -r l; do note "$l"; done
    elif [ "$ncfg" -eq 2 ] && [ "$nres" -eq 2 ] && grep -q '^nmax done$' "$TMP/m3.log" &&
       grep -q '^fail 0$' "$TMP/m3.log" && ! grep -q '^err ' "$TMP/m3.log"; then
        ok "M12 两遍搜索都 OK（SETTLE_MIN=300）、fail 0（$(grep -c '^nmax probe' "$TMP/m3.log") 行轨迹），两个进程的 nmax 行逐字节相同"
        grep -E '^nmax (load|result)' "$TMP/m3.log" | while read -r l; do note "$l"; done
    else
        bad "M12 cfg 行（SETTLE_MIN=300）$ncfg、OK 结果 $nres，或没有 nmax done / fail 0"
        grep -E '^nmax result|^err|^fail' "$TMP/m3.log" | head -5 | while read -r l; do note "$l"; done
    fi
else
    bad "M12 GFX2D_START_MODE=3 的 PC 版编不过"
fi

# ---------------------------------------------------------------- M13 统计 CSV 对账
# $1 = 日志。打一行：rows <行数> bad <不符行数> broken <夹断行数> head <表头行数> sum <汇总行对得上 1/0>
#                   dropped <lines_dropped> modes <出过行的模式> first <第一处不符>
csv_parse() {
    python3 - "$1" <<'PY'
import sys
HEAD = ("csvh,t,mode,exec,fps,drop,n_active,n_bullet,overflow,cmd,pixel,key_skip,ft_max,"
        "cyc_logic,cyc_build,cyc_exec,cyc_auto,cmd_eq0,cmd_eq1,cmd_eq2,cmd_eq3,cmd_eq4,cmd_eq5,"
        "cmd_other,pix_eq0,pix_eq1,pix_eq2,pix_eq3,pix_eq4,pix_eq5")
cols = HEAD.split(",")
nh = rows = bad = broken = 0
modes = set()
sums = []
first = ""
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    if "csv" in line and not line.startswith("csv"):
        broken += 1
    if line.startswith("csvh,"):
        nh += 1
        if line != HEAD:
            bad += 1
            first = first or "表头不符"
    elif line.startswith("csv,"):
        f = line.split(",")
        ok = len(f) == len(cols) and all(x.isdigit() for i, x in enumerate(f) if i not in (0, 2, 3))
        if ok:
            r = dict(zip(cols[1:], f[1:]))
            v = {k: int(x) for k, x in r.items() if k not in ("mode", "exec")}
            eq = [v["cmd_eq%d" % e] for e in range(6)]
            px = [v["pix_eq%d" % e] for e in range(6)]
            ok = (v["t"] == rows and r["mode"] in ("demo", "game", "press") and r["exec"] in ("sw", "hw")
                  and sum(eq) + v["cmd_other"] == v["cmd"] and sum(px) == v["pixel"])
            if not ok and not first:
                first = "t %d cmd %d/%d pix %d/%d" % (v["t"], sum(eq) + v["cmd_other"], v["cmd"], sum(px), v["pixel"])
            modes.add(r["mode"])
        elif not first:
            first = "列数或数字不对：" + line[:60]
        bad += not ok
        rows += 1
    elif line.startswith("csvsum,"):
        sums.append(line.split(","))
ok_sum = len(sums) == 1 and len(sums[0]) == 8 and all(x.isdigit() for x in sums[0][1:]) and int(sums[0][6]) == rows
dropped = int(sums[0][7]) if ok_sum else -1
print("rows %d bad %d broken %d head %d sum %d dropped %d modes %s first %s" % (
    rows, bad, broken, nh, 1 if ok_sum else 0, dropped, "+".join(sorted(modes)) or "-", first or "-"))
PY
}
# $1 = csv_parse 的输出里某个键
csv_get() { awk -v k="$1" '{ for (i = 1; i < NF; i++) if ($i == k) { print $(i + 1); exit } }'; }

echo "== M13 统计 CSV 对账（默认构建，三个模式各 600 帧，脚本解析 csv 行） =="
m13_modes=""
m13_bad=0
for m in 1 2 3; do
    if make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/csv$m" \
            EXTRA_CFLAGS="-DGFX2D_START_MODE=$m -DGFX2D_FRAMES=600" >"$TMP/csv$m.build.log" 2>&1; then
        SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/csv$m/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/csv$m.log"
        res=$(csv_parse "$TMP/csv$m.log")
        note "模式 $m：$res"
        rows=$(echo "$res" | csv_get rows)
        if [ "${rows:-0}" -gt 0 ] && [ "$(echo "$res" | csv_get bad)" = 0 ] &&
           [ "$(echo "$res" | csv_get broken)" = 0 ] && [ "$(echo "$res" | csv_get head)" = 1 ] &&
           [ "$(echo "$res" | csv_get sum)" = 1 ] && [ "$(echo "$res" | csv_get dropped)" = 0 ] &&
           grep -q '^fail 0$' "$TMP/csv$m.log" && ! grep -q '^err ' "$TMP/csv$m.log"; then
            m13_modes="$m13_modes$(echo "$res" | csv_get modes) "
        else
            m13_bad=$((m13_bad + 1))
        fi
    else
        note "模式 $m 的 PC 版编不过"
        m13_bad=$((m13_bad + 1))
    fi
done
if [ "$m13_bad" -eq 0 ] && [ "$m13_modes" = "demo game press " ]; then
    ok "M13 三个模式都出过行，每行按方程的两组列之和对得上 cmd 与 pixel，没有夹断的行，lines_dropped 0，fail 0"
else
    bad "M13 有 $m13_bad 个模式的 CSV 对不上，或出过行的模式不是三个（$m13_modes）"
fi

echo "== M13i CSV 的注错（打印时漏掉方程 1 那两列） =="
if make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/csvi" INJECT=CSV_DROP_EQ \
        EXTRA_CFLAGS="-DGFX2D_START_MODE=1 -DGFX2D_FRAMES=300" >"$TMP/csvi.build.log" 2>&1; then
    bin="$(ls "$TMP"/csvi/gfx2dGame* 2>/dev/null | head -1)"
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$bin" 2>/dev/null | tr -d '\r' >"$TMP/csvi.log"
    res=$(csv_parse "$TMP/csvi.log")
    nb=$(echo "$res" | csv_get bad)
    if [ "${nb:-0}" -gt 0 ]; then
        ok "M13i INJECT=CSV_DROP_EQ 见红：脚本解析出 $nb 行不符，第一处 $(echo "$res" | sed 's/.* first //')"
    else
        bad "M13i INJECT=CSV_DROP_EQ 没有见红（脚本的解析看不见漏列，这条判据是空转）"
    fi
else
    bad "M13i INJECT=CSV_DROP_EQ 编不过"
fi

# ---------------------------------------------------------------- M14 日志不阻塞长跑
echo "== M14 日志不阻塞长跑（模式 2、36000 帧、GFX2D_TX_BUDGET=192、按 60 Hz 节拍，约 10 分钟） =="
if [ "${SW8_SKIP_LONG:-0}" = 1 ]; then
    check_missing "M14 SW8_SKIP_LONG=1，日志长跑未执行"
elif [ "$GFX_CHECK_PROFILE" = quick ]; then
    check_na "M14 十分钟日志长跑属于 acceptance/release；快速档仍校验三个模式的 CSV 与日志队列"
elif make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/long" \
        EXTRA_CFLAGS="-DGFX2D_START_MODE=2 -DGFX2D_FRAMES=36000" >"$TMP/long.build.log" 2>&1; then
    SDL_VIDEODRIVER=dummy GFX2D_TX_BUDGET=192 "$TMP/long/gfx2dGame" 2>"$TMP/long.err" | tr -d '\r' >"$TMP/long.log"
    res=$(csv_parse "$TMP/long.log")
    over=$(grep -E '^board: tx budget ' "$TMP/long.err" | tail -1)
    if [ "$(echo "$res" | csv_get sum)" = 1 ] && [ "$(echo "$res" | csv_get dropped)" = 0 ] &&
       [ "$(echo "$res" | csv_get bad)" = 0 ] && [ "$(echo "$res" | csv_get broken)" = 0 ] &&
       grep -q '^fail 0$' "$TMP/long.log" && ! grep -q '^err ' "$TMP/long.log" &&
       echo "$over" | grep -q 'over 0 times 0 bytes$'; then
        ok "M14 lines_dropped 0、fail 0、bsp 超额汇总 over 0 times 0 bytes"
    else
        bad "M14 长跑不过：$res；$over"
        grep -E '^err|^fail|^csvsum' "$TMP/long.log" | head -4 | while read -r l; do note "$l"; done
    fi
    note "$res"
    note "$(grep '^csvsum' "$TMP/long.log")；$over"
else
    bad "M14 长跑的 PC 版编不过"
fi

# ---------------------------------------------------------------- M15 自动驾驶的 app 测试
echo "== M15 自动驾驶（app 测试的 A 组） =="
na_ok=$(grep -cE '^A[0-9] .* ok$' "$TMP/app64.txt" 2>/dev/null)
na_bad=$(grep -cE '^A[0-9] .* FAIL$' "$TMP/app64.txt" 2>/dev/null)
if [ "$na_ok" -eq 8 ] && [ "$na_bad" -eq 0 ]; then
    ok "M15 A 组 8 行全 ok（64/32 同 M8）"
    grep -E '^A4 ' "$TMP/app64.txt" | while read -r l; do note "$l"; done
else
    bad "M15 A 组应有 8 行 ok，实际 ok $na_ok 行、FAIL $na_bad 行"
fi
if "$HOST_CC" $FLAGS -m32 -DINJECT_AUTO_ORDER -I"$SW/lib" -I"$APP/src" -o "$TMP/auto_inj" \
        "$APP"/test/*.c "$APP"/src/game/stg_*.c "$SW"/lib/core/*.c "$SW"/lib/gfx/*.c -lm \
        >"$TMP/auto_inj.build.log" 2>&1; then
    "$TMP/auto_inj" >"$TMP/auto_inj.txt" 2>&1
    if grep -qE '^A2 tie among .* FAIL$' "$TMP/auto_inj.txt"; then
        ok "M15i INJECT=AUTO_ORDER 见红：$(grep -E '^A2 tie among ' "$TMP/auto_inj.txt")"
    else
        bad "M15i INJECT=AUTO_ORDER 没有让 A2 的平局那一行见红"
    fi
else
    bad "M15i INJECT=AUTO_ORDER 编不过"
fi

# ---------------------------------------------------------------- M16 自动驾驶打一局
echo "== M16 自动驾驶打一局（STG_FIELD=4、STG_JUDGE_AUTO=1，无窗口两遍） =="
auto_seq() { grep -E '^(seg|g) ' "$1"; }
if make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/auto" EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_AUTO=1" \
        >"$TMP/auto.build.log" 2>&1; then
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/auto/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/auto1.log"
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$TMP/auto/gfx2dGame" 2>/dev/null | tr -d '\r' >"$TMP/auto2.log"
    auto_seq "$TMP/auto1.log" >"$TMP/auto1.seq"
    auto_seq "$TMP/auto2.log" >"$TMP/auto2.seq"
    ng=$(grep -c '^g ' "$TMP/auto1.seq")
    if [ "$ng" -gt 0 ] && cmp -s "$TMP/auto1.seq" "$TMP/auto2.seq" &&
       grep -q '^fail 0$' "$TMP/auto1.log" && grep -q '^done$' "$TMP/auto1.log" &&
       grep -q '^auto frames ' "$TMP/auto1.log" && ! grep -q '^err ' "$TMP/auto1.log"; then
        ok "M16 两遍的 seg 与 g 行逐字节相同（$ng 帧）、fail 0、done"
        grep -E '^seg ' "$TMP/auto1.log" | tr '\n' ';' | sed 's/;$//' | while read -r l; do note "段：$l"; done
        note "$(grep '^auto frames ' "$TMP/auto1.log")（PC 上的周期数只作登记）"
        note "最后一帧：$(grep '^g ' "$TMP/auto1.log" | tail -1)"
    else
        bad "M16 帧数 $ng、两遍不同、没有 fail 0 / done / auto 行，或有 err 行"
        grep -E '^err|^fail' "$TMP/auto1.log" | head -4 | while read -r l; do note "$l"; done
    fi
else
    bad "M16 STG_FIELD=4 STG_JUDGE_AUTO=1 的 PC 版编不过"
fi

echo "== M16i 自动驾驶的注错（候选顺序倒过来） =="
if make -C "$APP" -s PLATFORM=host OBJDIR="$TMP/autoi" INJECT=AUTO_ORDER \
        EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_AUTO=1" >"$TMP/autoi.build.log" 2>&1; then
    bin="$(ls "$TMP"/autoi/gfx2dGame* 2>/dev/null | head -1)"
    SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 "$bin" 2>/dev/null | tr -d '\r' >"$TMP/autoi.log"
    auto_seq "$TMP/autoi.log" >"$TMP/autoi.seq"
    if [ -s "$TMP/auto1.seq" ] && ! cmp -s "$TMP/auto1.seq" "$TMP/autoi.seq"; then
        first=$(diff "$TMP/auto1.seq" "$TMP/autoi.seq" | grep -m1 '^[<>] g ' | awk '{print $3}')
        ok "M16i INJECT=AUTO_ORDER 见红：CRC 序列变了，第一处不同在第 ${first:-?} 帧"
    else
        bad "M16i INJECT=AUTO_ORDER 没有让 CRC 序列变（平局的取法看不见，这条判据是空转）"
    fi
else
    bad "M16i INJECT=AUTO_ORDER 编不过"
fi

# ---------------------------------------------------------------- M17 出图与第一版统计
echo "== M17 出图：入库的四份样例日志对账，plot_stats.py 出三张图并与入库的图源逐字节相同 =="
SLOGS="$APP/logs"
PLOT="$MAIN/tools/stats/plot_stats.py"
FIG_SRC="$MAIN/../Deliverables/Assert/figures/src"
m17_bad=0
for n in demo game s1 press; do
    res=$(csv_parse "$SLOGS/sw8_stats_$n.log")
    if [ "$(echo "$res" | csv_get rows)" -gt 0 ] 2>/dev/null && [ "$(echo "$res" | csv_get bad)" = 0 ] &&
       [ "$(echo "$res" | csv_get broken)" = 0 ] && [ "$(echo "$res" | csv_get head)" = 1 ] &&
       [ "$(echo "$res" | csv_get sum)" = 1 ] && [ "$(echo "$res" | csv_get dropped)" = 0 ] &&
       grep -q '^fail 0$' "$SLOGS/sw8_stats_$n.log" && ! grep -q '^err ' "$SLOGS/sw8_stats_$n.log"; then
        note "sw8_stats_$n.log：$res"
    else
        note "sw8_stats_$n.log 不对：$res"
        m17_bad=$((m17_bad + 1))
    fi
done
if [ "$m17_bad" -eq 0 ]; then
    ok "M17a 四份样例日志的 csv 行逐行对账（方程条数与像素之和）、汇总行齐全、fail 0"
else
    bad "M17a $m17_bad 份样例日志对账不过"
fi
python3 "$PLOT" --out "$TMP/fig" --ref-s1 --nmax "$SLOGS/sw8_stats_press.log" \
    "$SLOGS/sw8_stats_demo.log=基础 Demo" "$SLOGS/sw8_stats_game.log=游戏（自动驾驶）" \
    "$SLOGS/sw8_stats_s1.log=S1 规模（模式 3，旋钮 1744）@10" >"$TMP/plot.txt" 2>&1
rp=$?
same=0
for f in pixel_share frame_time nmax_search; do
    cmp -s "$TMP/fig/sw8_$f.html" "$FIG_SRC/sw8_$f.html" && same=$((same + 1))
done
if [ "$rp" -eq 0 ] && grep -q '^figures 3 missing 0$' "$TMP/plot.txt" && [ "$same" -eq 3 ]; then
    ok "M17 plot_stats.py 出了三张图，三份图源与 Deliverables/Assert/figures/src 里入库的逐字节相同"
    grep -E '^(pixel_share|nmax) ' "$TMP/plot.txt" | while read -r l; do note "$l"; done
else
    bad "M17 退出码 $rp、图没出齐，或与入库的图源不同（相同 $same 份）"
    tail -3 "$TMP/plot.txt" | while read -r l; do note "$l"; done
fi
echo "== M17i 出图的注错（不给搜索日志） =="
python3 "$PLOT" --out "$TMP/figi" "$SLOGS/sw8_stats_demo.log=基础 Demo" >"$TMP/ploti.txt" 2>&1
ri=$?
if [ "$ri" -ne 0 ] && grep -q '^figures 2 missing 1$' "$TMP/ploti.txt" && [ ! -f "$TMP/figi/sw8_nmax_search.html" ]; then
    ok "M17i 缺搜索日志时报 missing 1、退出码 $ri、不出搜索轨迹图"
else
    bad "M17i 缺搜索日志时没有报出来（退出码 $ri），缺数据会被当成出齐了"
fi

check_finish
