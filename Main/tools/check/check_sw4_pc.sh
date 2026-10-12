#!/usr/bin/env bash
# check_sw4_pc.sh —— PC 平台层的提交前判据
#
# 本脚本负责 PC 平台层判据中「要跑多次再比」
# 的那三条，其余由判据程序与主机侧测试自己判：
#
#   1 显示   十一个整帧用例的 CRC 与 lib/test/golden.txt 逐行相同
#   2 缩放   最近邻（主机侧探针 check_sw4_scale.c）+ 三种窗口尺寸
#   8 无窗口 SDL_VIDEODRIVER=dummy 与有窗两次跑同一份二进制，CRC 序列逐行相同
#   7 录像格式 录像文本经 replay2c.py 收成 .g2dr 再校验（文本往返与坏输入在 --selftest 里）
#
# 另外跑一次注错构建（INJECT=REPLAY_CLOCK）：画面依赖时刻之后，同一段录像回放两次
# 必须出现不同，用来证明「两次相同」那条判据不是空转。
#
# 用法（在 WSL 里，仓库根或任意位置）：
#     bash Main/tools/check/check_sw4_pc.sh
#     SW4_QUICK=1 bash Main/tools/check/check_sw4_pc.sh     少跑几帧，改代码时用
#
# 全量一次约四分钟：判据程序每跑一遍是 600 帧节拍（10 秒）加 1800 帧回放两遍（60 秒），
# 脚本要跑三遍（有窗、无窗、注错）。SW4_QUICK=1 把三段的帧数压到 60/30/180。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"
APP_DIR="$SW/apps/test/platformTest"
GOLDEN="$SW/lib/test/golden.txt"
REPLAY2C="$MAIN/tools/check/replay2c.py"
SCALE_C="$MAIN/tools/check/check_sw4_scale.c"

HOST_CC="${HOST_CC:-gcc}"

TMP="$(mktemp -d)"
FAIL=0

cleanup() {
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

if [ "${SW4_QUICK:-0}" = "1" ]; then
    EXTRA="-DPACE_FRAMES=60 -DLIVE_FRAMES=30 -DREPLAY_FRAMES=180"
    note "SW4_QUICK=1：帧数压到 60/30/180"
else
    EXTRA=""
fi

# CRLF 归一：应用的日志走 board_puts，行尾是 \r\n（与板上 UART 逐字节相同的约定）
norm() { tr -d '\r' < "$1"; }

# 判据序列：用例、两次回放的 CRC、录制文本。这三类行在两个运行之间必须逐行相同。
seq_of() {
    norm "$1" | grep -E '^(case |play1 |play2 |key |g2dr )'
}

echo "== 编译判据程序（有窗与无窗用同一个二进制）=="
if ! make -C "$APP_DIR" -s PLATFORM=host EXTRA_CFLAGS="$EXTRA" >"$TMP/build.log" 2>&1; then
    bad "编译 platformTest"
    note "$(tail -3 "$TMP/build.log")"
    printf 'fail %d\ndone\n' "$FAIL"
    exit 1
fi
BIN="$APP_DIR/build_host/platformTest"
SHA_BEFORE="$(sha256sum "$BIN" | cut -c1-16)"
note "平台构建完成，二进制 sha256 前 16 位 $SHA_BEFORE"

echo "== 跑一遍：有窗 =="
if ! "$BIN" >"$TMP/win.log" 2>&1; then
    bad "有窗运行退出码不是 0"
fi
norm "$TMP/win.log" >"$TMP/win.txt"

echo "== 跑一遍：无窗（SDL_VIDEODRIVER=dummy，去掉显示相关变量）=="
if ! env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
        SDL_VIDEODRIVER=dummy "$BIN" >"$TMP/head.log" 2>&1; then
    bad "无窗运行退出码不是 0"
fi
norm "$TMP/head.log" >"$TMP/head.txt"

SHA_AFTER="$(sha256sum "$BIN" | cut -c1-16)"
if [ "$SHA_BEFORE" = "$SHA_AFTER" ]; then
    ok "S0 两次跑的是同一个二进制（$SHA_BEFORE）"
else
    bad "S0 两次之间的二进制变了"
fi

echo "== 判据 1：十一个整帧用例对 golden.txt =="
norm "$TMP/win.txt" | grep '^case ' | awk '{print $3, $5}' >"$TMP/case_got.txt"
awk '{print $1, $2}' "$GOLDEN" >"$TMP/case_want.txt"
if diff -u "$TMP/case_want.txt" "$TMP/case_got.txt" >"$TMP/case.diff" 2>&1; then
    ok "S1 十一个用例的 CRC 与 golden.txt 逐行相同"
else
    bad "S1 用例 CRC 与 golden.txt 不一致"
    head -8 "$TMP/case.diff" | while read -r l; do note "$l"; done
fi

echo "== 判据 8：有窗与无窗的序列逐行相同 =="
seq_of "$TMP/win.log" >"$TMP/seq_win.txt"
seq_of "$TMP/head.log" >"$TMP/seq_head.txt"
if [ ! -s "$TMP/seq_win.txt" ]; then
    bad "S2 有窗那一次没有产生任何序列行"
elif diff -q "$TMP/seq_win.txt" "$TMP/seq_head.txt" >/dev/null 2>&1; then
    ok "S2 有窗与无窗逐行相同（$(wc -l <"$TMP/seq_win.txt" | tr -d ' ') 行）"
else
    bad "S2 有窗与无窗的序列不同"
    diff "$TMP/seq_win.txt" "$TMP/seq_head.txt" | head -8 | while read -r l; do note "$l"; done
fi

echo "== 两次回放自身必须逐行相同（判决由判据程序自己下）=="
if grep -q '^chk R3 两次回放逐帧相同 ok' "$TMP/win.txt"; then
    ok "S3 判据程序报告两次回放逐帧相同"
else
    bad "S3 判据程序没有报告两次回放相同"
fi
if grep -q '^fail =0' "$TMP/win.txt" && grep -q '^done' "$TMP/win.txt"; then
    ok "S4 有窗运行的 fail =0 且 done"
else
    bad "S4 有窗运行的 fail 不为 0"
    grep -n 'FAIL' "$TMP/win.txt" | head -8 | while read -r l; do note "$l"; done
fi

echo "== 判据 7：录制文本 -> .g2dr（录制结果就是日志）=="
grep -E '^(g2dr |key )' "$TMP/win.txt" >"$TMP/rec.txt"
if python3 "$REPLAY2C" --from-text "$TMP/rec.txt" --out "$TMP/rec.g2dr" >"$TMP/rec.log" 2>&1 &&
   python3 "$REPLAY2C" --check "$TMP/rec.g2dr" >>"$TMP/rec.log" 2>&1; then
    ok "S5 录制文本收成 .g2dr 并通过结构校验（$(grep -c '^key ' "$TMP/rec.txt") 帧）"
else
    bad "S5 录制文本收不回 .g2dr"
    tail -3 "$TMP/rec.log" | while read -r l; do note "$l"; done
fi
if python3 "$REPLAY2C" --selftest >"$TMP/self.log" 2>&1; then
    ok "S6 转换脚本自检通过（文本往返与坏输入）"
else
    bad "S6 转换脚本自检失败"
    tail -6 "$TMP/self.log" | while read -r l; do note "$l"; done
fi

echo "== 判据 2：最近邻与窗口尺寸（主机侧探针）=="
if "$HOST_CC" -std=gnu11 -O2 -Wall -Wextra -Werror -o "$TMP/scale" "$SCALE_C" \
        $(sdl2-config --cflags) $(sdl2-config --libs) >"$TMP/scale_build.log" 2>&1; then
    if SDL_VIDEODRIVER=dummy "$TMP/scale" >"$TMP/scale.log" 2>&1; then
        ok "S7 最近邻、两个 hint 有别、三种窗口尺寸都成立"
    else
        bad "S7 缩放探针报错"
        grep FAIL "$TMP/scale.log" | head -4 | while read -r l; do note "$l"; done
    fi
else
    bad "S7 缩放探针编不过"
    tail -3 "$TMP/scale_build.log" | while read -r l; do note "$l"; done
fi

echo "== 注错构建：画面依赖时刻之后，两次回放必须不同 =="
if ! make -C "$APP_DIR" -s PLATFORM=host INJECT=REPLAY_CLOCK EXTRA_CFLAGS="$EXTRA" \
        >"$TMP/build_inj.log" 2>&1; then
    bad "S8 注错构建编不过"
    tail -3 "$TMP/build_inj.log" | while read -r l; do note "$l"; done
else
    INJ_BIN="$APP_DIR/build_host/platformTest-inject"
    "$INJ_BIN" >"$TMP/inj.log" 2>&1
    norm "$TMP/inj.log" >"$TMP/inj.txt"
    grep '^play1 ' "$TMP/inj.txt" | awk '{print $2, $4}' >"$TMP/inj1.txt"
    grep '^play2 ' "$TMP/inj.txt" | awk '{print $2, $4}' >"$TMP/inj2.txt"
    if [ ! -s "$TMP/inj1.txt" ]; then
        bad "S8 注错运行没有回放行"
    elif diff -q "$TMP/inj1.txt" "$TMP/inj2.txt" >/dev/null 2>&1; then
        bad "S8 注错之后两次回放仍然相同（判据是空转）"
    elif grep -q '^fail =0' "$TMP/inj.txt"; then
        ok "S8 注错版两次回放出现不同，且 fail =0"
    else
        bad "S8 注错运行的 fail 不为 0"
    fi
fi

echo "== 复位成正常构建，别把注错产物留给下一次 =="
make -C "$APP_DIR" -s PLATFORM=host EXTRA_CFLAGS="$EXTRA" >/dev/null 2>&1 || true

printf 'fail %d\n' "$FAIL"
printf 'done\n'
[ "$FAIL" -eq 0 ] || exit 1
exit 0
