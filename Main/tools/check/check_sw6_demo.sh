#!/usr/bin/env bash
# check_sw6_demo.sh —— 基础 Demo 的提交前判据
#
# 十项判据列在下面。日志侧的外部核对（两遍比对、hud 与上一帧统计、hcrc 按字库文本独立重画、s 与 scene
# 参考值、off 与 eq）由 tools/check/check_sw6_log.py 做，本脚本负责串起来：
#
#   1   录像可重生成    gen_sw6_replay.py 重打两份 .g2dr，与入库的逐字节相同；
#                       replay2c.py 独立解析一遍
#   2   编过并跑通      make PLATFORM=host，判据模式一遍，^fail 0 与 ^done、退出码 0
#       2b  演示模式    默认构建加帧数上限编过并冒烟，A、B 两段都出现
#       2c  双缓冲反例  INJECT=SW6_FB_PER_PASS、每遍 121 帧，必须出现 fb err
#   3   对期望日志      去掉 cyc 三列后与入库的期望日志逐行相同；快速档另核前缀关系
#   4   有窗与无窗      同一个二进制在 SDL_VIDEODRIVER=dummy 下再跑一遍，逐字节相同
#   5   两遍一致        p 1 与 p 2 逐帧相同；反例 INJECT=SW6_LIVE_HUD 必须见红
#   6   HUD             hud 列对上一帧统计、hcrc 对字库文本重画；反例 INJECT=EQ_COPY 见红
#   7   场景 B          scene 参考值两两不同、每个 B 帧对上本帧 s；反例 EQ_COPY 见红
#   8   计数            A 帧 off >= 4；eq A 的 copy/alpha/add、eq P 的 copy/alpha/straight 非 0
#   9   逐方程注错      COPY／ALPHA／ADD／ALPHA_STRAIGHT 各一次，每次至少改变一行
#   10  Sanitizer       加 SAN=1 再跑一遍，ASan／UBSan 无报告、fail 0
#       板上目标        有交叉工具链时另编判据与演示两种构建，零告警且 bin <= 524288 字节
#
# 用法（WSL，仓库根或任意位置）：
#     bash Main/tools/check/check_sw6_demo.sh
#     SW6_QUICK=1 bash Main/tools/check/check_sw6_demo.sh     每段每遍只跑 120 帧
#
# 需要 SDL2 与 python3；板上目标要 RISCV_BIN=<RISCV_BIN>（doc/开发环境.md 第 11 节）。
# 不上板：板上对账（判据模式快速档）用 tools/board/board_run.py 单独跑，
# 串口日志用 `check_sw6_log.py --board <串口日志> logs/sw6_judge_quick.log` 对账，
# 原始串口日志入库 logs/。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
source "$MAIN/tools/check/check_common.sh"
ROOT="$(cd "$MAIN/.." && pwd)"
APP="$MAIN/sw/apps/game/gfx2dGame"
LOGS="$APP/logs"
FONT="$MAIN/assets/font/ascii8x16.txt"
CHK="$MAIN/tools/check/check_sw6_log.py"
GEN="$MAIN/tools/check/gen_sw6_replay.py"
REPLAY2C="$MAIN/tools/check/replay2c.py"
RISCV_BIN="${RISCV_BIN:-riscv-none-elf-}"
HOST_BIN="$APP/build_host/gfx2dGame"
INJ_BIN="$APP/build_host/gfx2dGame-inject"   # 带 INJECT 时 mk 把产物名加 -inject

TMP="$(mktemp -d)"
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

norm() { tr -d '\r' < "$1"; }

# 每行去掉 cyc 三列之外的差别由 check_sw6_log.py 判；这里只做行数与关键字的粗筛。
tail_lines() { tail -n "${1:-6}"; }

# 判据模式（默认构建是演示模式），快速档时另加帧数上限并换期望日志
JUDGE_CFLAGS="-DSW6_JUDGE=1"
EXPECT="$LOGS/sw6_judge.log"
if [ "$GFX_CHECK_PROFILE" = quick ]; then
    SW6_QUICK=1
elif [ "${SW6_QUICK:-0}" = 1 ]; then
    check_missing "SW6_QUICK=1 不能完成 acceptance/release 的全量 Demo"
fi
if [ "${SW6_QUICK:-0}" = "1" ]; then
    JUDGE_CFLAGS="$JUDGE_CFLAGS -DSW6_FRAMES=120"
    EXPECT="$LOGS/sw6_judge_quick.log"
    note "快速档：每段每遍 120 帧，期望日志 $EXPECT"
fi

echo "== 前置：打包素材（assets.h 与 assets.pak 是生成物、不入库） =="
if python3 "$MAIN/tools/assets/pack_assets.py" >"$TMP/pack.log" 2>&1; then
    ok "素材打包成功（$(grep -o 'pack_assets:.*' "$TMP/pack.log" | head -1)）"
else
    bad "素材打包失败"
    tail_lines 5 <"$TMP/pack.log" | while read -r l; do note "$l"; done
    printf 'fail %d\ndone\n' "$FAIL"; exit 1
fi

# ---------------------------------------------------------------- 录像可重生成
echo "== D1 录像可重生成 =="
if python3 "$GEN" --out "$TMP/replay" >"$TMP/gen.log" 2>&1; then
    d1=0
    for n in sw6_a sw6_b; do
        if cmp -s "$TMP/replay/$n.g2dr" "$APP/replay/$n.g2dr"; then
            note "$n.g2dr $(stat -c %s "$APP/replay/$n.g2dr") 字节与入库的一致"
        else
            bad "D1 $n.g2dr 重新生成后与入库的不一致"
            d1=1
        fi
        if python3 "$REPLAY2C" --check "$APP/replay/$n.g2dr" >"$TMP/chk_$n.log" 2>&1; then
            note "$(head -1 "$TMP/chk_$n.log")"
        else
            bad "D1 $n.g2dr 没通过 replay2c.py 的独立解析"
            d1=1
        fi
    done
    [ "$d1" -eq 0 ] && ok "D1 两份录像都能重生成、格式独立可解"
else
    bad "D1 gen_sw6_replay.py 跑不动"
    tail_lines 5 <"$TMP/gen.log" | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- 编过并跑通
echo "== D2 判据模式（有窗） =="
if ! make -C "$APP" -s PLATFORM=host EXTRA_CFLAGS="$JUDGE_CFLAGS" >"$TMP/build.log" 2>&1; then
    bad "D2 编不过"
    tail_lines 8 <"$TMP/build.log" | while read -r l; do note "$l"; done
    printf 'fail %d\ndone\n' "$FAIL"; exit 1
fi
note "二进制 sha256 前 16 位 $(sha256sum "$HOST_BIN" | cut -c1-16)"

"$HOST_BIN" >"$TMP/run.log" 2>"$TMP/run.err"
rc=$?
norm "$TMP/run.log" >"$TMP/run.txt"
if [ "$rc" -eq 0 ] && grep -q '^fail 0' "$TMP/run.txt" && grep -q '^done' "$TMP/run.txt"; then
    ok "D2 退出码 0、fail 0、done（$(wc -l <"$TMP/run.txt") 行）"
else
    bad "D2 退出码 $rc 或没有 fail 0"
    tail_lines 8 <"$TMP/run.txt" | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- 期望日志
echo "== D3 对期望日志 =="
if [ ! -f "$EXPECT" ]; then
    bad "D3 缺少入库的期望日志 $EXPECT"
else
    if python3 "$CHK" --compare "$EXPECT" "$TMP/run.txt" >"$TMP/d3.log" 2>&1; then
        ok "D3 $(head -1 "$TMP/d3.log")"
    else
        bad "D3 与期望日志不一致"
        head -2 "$TMP/d3.log" | while read -r l; do note "$l"; done
    fi
    if [ "${SW6_QUICK:-0}" != "1" ] && [ -f "$LOGS/sw6_judge_quick.log" ]; then
        if python3 "$CHK" --quick-prefix "$EXPECT" "$LOGS/sw6_judge_quick.log" >"$TMP/d3q.log" 2>&1; then
            ok "D3 $(head -1 "$TMP/d3q.log")"
        else
            bad "D3 快速档不是全量档的前缀"
            head -2 "$TMP/d3q.log" | while read -r l; do note "$l"; done
        fi
    fi
fi

# ---------------------------------------------------------------- 有窗与无窗
echo "== D4 无窗同一个二进制 =="
SDL_VIDEODRIVER=dummy "$HOST_BIN" >"$TMP/dummy.log" 2>"$TMP/dummy.err"
rc=$?
norm "$TMP/dummy.log" >"$TMP/dummy.txt"
if [ "$rc" -ne 0 ]; then
    bad "D4 无窗跑的退出码是 $rc"
elif python3 "$CHK" --compare "$TMP/run.txt" "$TMP/dummy.txt" >"$TMP/d4.log" 2>&1; then
    ok "D4 有窗与无窗去掉 cyc 后逐字节相同"
else
    bad "D4 有窗与无窗不一致"
    head -2 "$TMP/d4.log" | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- 演示模式
echo "== D2b 演示模式（默认构建）编过并冒烟 =="
# 判据模式换掉了默认构建，所以这一条放在无窗复跑之后：日志侧判据只用已经采集好的 run.txt。
# 演示模式是 `#else` 那一半，`-DSW6_FRAMES` 的编译期分支只在它里面求值——
# 第一版 `SW6_FRAMES_MAX` 带 `(uint32_t)` 转型，`#if` 不认，演示模式加这个开关编不过。
if ! make -C "$APP" -s PLATFORM=host EXTRA_CFLAGS="-DGFX2D_START_MODE=1 -DSW6_STAGE_FRAMES=60 -DSW6_FRAMES=200" \
        >"$TMP/build_demo.log" 2>&1; then
    bad "D2b 演示模式编不过"
    tail_lines 6 <"$TMP/build_demo.log" | while read -r l; do note "$l"; done
else
    SDL_VIDEODRIVER=dummy "$HOST_BIN" >"$TMP/demo.log" 2>&1
    rc=$?
    norm "$TMP/demo.log" >"$TMP/demo.txt"
    n_a=$(grep -c '^f [0-9]* A ' "$TMP/demo.txt" || true)
    n_b=$(grep -c '^f [0-9]* B ' "$TMP/demo.txt" || true)
    if [ "$rc" -eq 0 ] && grep -q '^fail 0' "$TMP/demo.txt" && [ "$n_a" -gt 0 ] && [ "$n_b" -gt 0 ]; then
        ok "D2b 演示模式编过并跑通（$n_a 帧 A、$n_b 帧 B，阶段轮换到了）"
    else
        bad "D2b 演示模式冒烟失败（退出码 $rc，A $n_a 帧、B $n_b 帧）"
        tail_lines 5 <"$TMP/demo.txt" | while read -r l; do note "$l"; done
    fi
fi

# ---------------------------------------------------------------- 双缓冲反例
echo "== D2c 双缓冲不变量的反例：按每遍帧号选面、每遍 121 帧（奇数） =="
# 应用在每帧开头核对「目的面不是上一次 present 的那一面」，违反就打 fb err 并计 fail。
# 正常构建按全局帧序号轮换，判据模式的全量跑（B 段一遍 367 帧，奇数）已经覆盖；
# 反例退回按每遍帧号选面，一遍的帧数是奇数时下一遍第 0 帧必然撞上，这一条必须见红。
if ! make -C "$APP" -s PLATFORM=host INJECT=SW6_FB_PER_PASS \
        EXTRA_CFLAGS="-DSW6_JUDGE=1 -DSW6_FRAMES=121" >"$TMP/build_fb.log" 2>&1; then
    bad "D2c 反例注错版编不过"
    tail_lines 5 <"$TMP/build_fb.log" | while read -r l; do note "$l"; done
else
    SDL_VIDEODRIVER=dummy "$INJ_BIN" >"$TMP/fb.log" 2>&1
    norm "$TMP/fb.log" >"$TMP/fb.txt"
    if grep -q '^fb err' "$TMP/fb.txt" && ! grep -q '^fail 0' "$TMP/fb.txt"; then
        ok "D2c 反例：按每遍帧号选面时 fb err 见红（$(grep -c '^fb err' "$TMP/fb.txt") 次）"
    else
        bad "D2c 反例：按每遍帧号选面时没有 fb err（不变量空转）"
    fi
fi

# ---------------------------------------------------------------- 日志侧判据
echo "== D5 两遍一致 =="
if python3 "$CHK" --pair "$TMP/run.txt" >"$TMP/d5.log" 2>&1; then
    grep '^chk' "$TMP/d5.log" | while read -r l; do note "$l"; done
    ok "D5 两遍逐帧相同"
else
    bad "D5 两遍不一致"
    head -3 "$TMP/d5.log" | while read -r l; do note "$l"; done
fi

echo "== D6 HUD 与字形 =="
if python3 "$CHK" --hud "$TMP/run.txt" --font "$FONT" >"$TMP/d6.log" 2>&1; then
    ok "D6 $(head -1 "$TMP/d6.log")"
else
    bad "D6 HUD 对账失败"
    head -3 "$TMP/d6.log" | while read -r l; do note "$l"; done
fi

echo "== D7 场景 B =="
if python3 "$CHK" --scene "$TMP/run.txt" >"$TMP/d7.log" 2>&1; then
    grep '^chk' "$TMP/d7.log" | while read -r l; do note "$l"; done
    ok "D7 场景参考值与逐帧 s 都对上"
else
    bad "D7 场景 B 判据失败"
    head -3 "$TMP/d7.log" | while read -r l; do note "$l"; done
fi

echo "== D8 计数 =="
if python3 "$CHK" --counts "$TMP/run.txt" >"$TMP/d8.log" 2>&1; then
    grep '^chk' "$TMP/d8.log" | while read -r l; do note "$l"; done
    ok "D8 off 与 eq 计数符合"
else
    bad "D8 计数不符"
    head -3 "$TMP/d8.log" | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- 逐方程注错
echo "== D9 逐方程注错（与快速档期望日志比，至少改变一行） =="
QEXPECT="$LOGS/sw6_judge_quick.log"
QFLAGS="-DSW6_JUDGE=1 -DSW6_FRAMES=120"
if [ ! -f "$QEXPECT" ]; then
    bad "D9 缺少快速档期望日志 $QEXPECT"
else
    for inj in EQ_COPY EQ_ALPHA EQ_ADD EQ_ALPHA_STRAIGHT; do
        if ! make -C "$APP" -s PLATFORM=host INJECT="$inj" EXTRA_CFLAGS="$QFLAGS" \
                >"$TMP/build_$inj.log" 2>&1; then
            bad "D9 $inj 注错版编不过"
            continue
        fi
        SDL_VIDEODRIVER=dummy "$INJ_BIN" >"$TMP/inj_$inj.log" 2>&1
        norm "$TMP/inj_$inj.log" >"$TMP/inj_$inj.txt"
        if python3 "$CHK" --compare "$QEXPECT" "$TMP/inj_$inj.txt" >"$TMP/cmp_$inj.log" 2>&1; then
            bad "D9 INJECT=$inj 与期望日志完全相同（判据空转）"
        else
            ok "D9 INJECT=$inj 至少改变一行（$(sed -n '1p' "$TMP/cmp_$inj.log" | cut -c1-60)…）"
        fi
        # HUD／场景 B 的反例用 EQ_COPY 那一次
        if [ "$inj" = "EQ_COPY" ]; then
            if python3 "$CHK" --hud "$TMP/inj_EQ_COPY.txt" --font "$FONT" >"$TMP/c6.log" 2>&1; then
                bad "D6 反例：EQ_COPY 下 hcrc 仍然全对（判据空转）"
            else
                ok "D6 反例：EQ_COPY 下 hcrc 对不上"
            fi
            if python3 "$CHK" --scene "$TMP/inj_EQ_COPY.txt" >"$TMP/c7.log" 2>&1; then
                bad "D7 反例：EQ_COPY 下 B 帧 scrc 仍然对上（判据空转）"
            else
                ok "D7 反例：EQ_COPY 下 B 帧 scrc 对不上"
            fi
        fi
    done
fi

# ---------------------------------------------------------------- 两遍一致的反例
echo "== D5 反例：判据模式照样显示实时量 =="
if ! make -C "$APP" -s PLATFORM=host INJECT=SW6_LIVE_HUD EXTRA_CFLAGS="$QFLAGS" \
        >"$TMP/build_live.log" 2>&1; then
    bad "D5 反例注错版编不过"
    tail_lines 5 <"$TMP/build_live.log" | while read -r l; do note "$l"; done
elif [ ! -f "$QEXPECT" ]; then
    bad "D5 反例缺少快速档期望日志"
else
    SDL_VIDEODRIVER=dummy "$INJ_BIN" >"$TMP/live.log" 2>&1
    norm "$TMP/live.log" >"$TMP/live.txt"
    if python3 "$CHK" --pair "$TMP/live.txt" >"$TMP/d5c.log" 2>&1; then
        bad "D5 反例：SW6_LIVE_HUD 下两遍仍然相同（判据空转）"
    else
        ok "D5 反例：SW6_LIVE_HUD 下两遍见红"
    fi
fi

# ---------------------------------------------------------------- Sanitizer
echo "== D10 Sanitizer 与板上目标 =="
if make -C "$APP" -s PLATFORM=host SAN=1 EXTRA_CFLAGS="$QFLAGS" >"$TMP/build_san.log" 2>&1; then
    SDL_VIDEODRIVER=dummy "$HOST_BIN" >"$TMP/san.log" 2>&1
    rc=$?
    norm "$TMP/san.log" >"$TMP/san.txt"
    if grep -qE 'AddressSanitizer|runtime error:|LeakSanitizer' "$TMP/san.txt"; then
        bad "D10 ASan／UBSan 有报告"
        grep -m2 -E 'AddressSanitizer|runtime error:' "$TMP/san.txt" | while read -r l; do note "$l"; done
    elif [ "$rc" -ne 0 ] || ! grep -q '^fail 0' "$TMP/san.txt"; then
        bad "D10 SAN 版退出码 $rc 或没有 fail 0"
    else
        ok "D10 SAN=1 无报告、fail 0"
    fi
else
    bad "D10 SAN=1 编不过"
    tail_lines 5 <"$TMP/build_san.log" | while read -r l; do note "$l"; done
fi

if command -v "${RISCV_BIN}gcc" >/dev/null 2>&1; then
    bfail=0
    for mode in "judge:$QFLAGS" "demo:"; do
        name="${mode%%:*}"
        flags="${mode#*:}"
        if make -C "$APP" -s RISCV_BIN="$RISCV_BIN" OBJDIR="$TMP/b_$name" \
                EXTRA_CFLAGS="$flags" >"$TMP/b_$name.log" 2>&1; then
            bin="$TMP/b_$name/gfx2dGame-O2.bin"
            size=$(stat -c %s "$bin" 2>/dev/null || echo 0)
            if [ "$size" -le 524288 ]; then
                ok "D10 板上目标（$name）零告警，bin $size 字节"
            else
                bad "D10 板上目标（$name）bin $size 字节，超过 524288"
            fi
        else
            bad "D10 板上目标（$name）编不过"
            tail_lines 5 <"$TMP/b_$name.log" | while read -r l; do note "$l"; done
            bfail=1
        fi
    done
else
    # 工具链装在 WSL 里、不在 PATH 上是常态（doc/开发环境.md 第 11 节的 <RISCV_BIN>），
    # 所以找不到时先怀疑没传 RISCV_BIN，而不是没装。跳过不算通过：验收前必须带上它重跑。
    check_missing "D10 找不到 ${RISCV_BIN}gcc，板上目标未构建"
fi

# 收尾：把构建复位成默认的演示模式，别把注错版留在 build_host/
make -C "$APP" -s PLATFORM=host >/dev/null 2>&1 || true

check_finish
