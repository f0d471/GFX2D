#!/usr/bin/env bash
# check_sw5_assets.sh —— 素材流水线（SW5）的提交前判据
#
# 判据见 Engineering/TechReports/sw/04-素材流水线.md 3.12 节。本脚本把这些串起来跑：
#
#   S1 打包确定性      同一个 manifest 打两次，assets.pak 与 assets.h 逐字节相同
#   S2 独立实现对账     check_sw5_pak.py 自己解包、自己从 PNG/字库重算像素字节，
#                      逐项 CRC 行还要与 C 侧 assetTest 的输出 diff
#   S3 主机侧装载解析   assetTest 跑通：装载 → 解析 → 与 assets.h 对账 → 登记表面 → BLIT
#   S4 画面            整帧 CRC、已知窗口 CRC 与 64×24 字符画对入库参考日志
#   S5 注错输入        七种坏包各自报出指定的错误码；超 4 MiB 总长必须在读正文前被拒
#   S6 注错构建        关掉两轮 CRC 之后同一个坏包不再被拒，且画面 CRC 与参考不同
#   S7 镜像            make_flash_image 合成后解回，三个分区逐字节一致、区外全 0xFF
#   S8 镜像边界        输入末字节是 0xFF 时必须保留；超限输入必须报错退出
#   S9 许可            Main/assets 下每个非生成文件都在 LICENSES.md 里登记
#
# 用法（WSL，仓库根或任意位置）：
#     bash Main/tools/check/check_sw5_assets.sh
#
# 不需要板子与交叉工具链。板上那一条（Flash 启动 → 读出版本/长度/整包 CRC → 画出已知图块）
# 需在板上单独执行，见 sw 第 04 章 3.13 节。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
APP_DIR="$MAIN/sw/apps/test/assetTest"
ASSETS="$MAIN/assets"
REF_LOG="$APP_DIR/logs/pc_normal.log"
HOST_CC="${HOST_CC:-gcc}"

TMP="$(mktemp -d)"
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

norm() { tr -d '\r' < "$1"; }

# ---------------------------------------------------------------- S1 打包确定性
echo "== S1 打包：正式产物 + 确定性 =="
if ! python3 "$MAIN/tools/assets/pack_assets.py" >"$TMP/pack.log" 2>&1; then
    bad "S1 pack_assets 打包失败"
    sed -n '1,5p' "$TMP/pack.log" | while read -r l; do note "$l"; done
    printf 'fail %d\ndone\n' "$FAIL"; exit 1
fi
note "$(grep -o 'pack_assets:.*' "$TMP/pack.log" | head -1)"

for n in 1 2; do
    python3 "$MAIN/tools/assets/pack_assets.py" --out "$TMP/p$n" >>"$TMP/pack.log" 2>&1 || \
        bad "S1 第 $n 次重打包失败"
done
if cmp -s "$TMP/p1/assets.pak" "$TMP/p2/assets.pak" &&
   cmp -s "$TMP/p1/assets.h"   "$TMP/p2/assets.h" &&
   cmp -s "$TMP/p1/assets.pak" "$ASSETS/build/assets.pak"; then
    ok "S1a 两次打包与正式产物逐字节相同（$(stat -c %s "$ASSETS/build/assets.pak") 字节）"
else
    bad "S1a 打包不确定：相同输入产出了不同的包或头文件"
fi

# 四个入口反例：它们不改变正式素材，只检查打包器的边界。
if MAIN="$MAIN" TMP="$TMP" python3 - <<'PY' >"$TMP/input_edges.log" 2>&1
import importlib.util
import io
import os
import unittest.mock
from PIL import Image

main = os.environ["MAIN"]
tmp = os.environ["TMP"]
path = os.path.join(main, "tools", "assets", "pack_assets.py")
spec = importlib.util.spec_from_file_location("pack_assets_edges", path)
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)

# 字形像素行可以从 # 开头；只有 "# " 注释行才跳过。
font = "#.......\n" + "........\n" * (95 * 16 - 1)
with unittest.mock.patch("builtins.open", side_effect=lambda *a, **k: io.StringIO(font)):
    pack.load_font_rgba("memory", "font", (128, 96), 8, 16)

# 扩展名不是判据：内容是 JPEG 即使模式为 RGB 也必须拒绝。
jpeg = io.BytesIO()
Image.new("RGB", (1, 1), (1, 2, 3)).save(jpeg, format="JPEG")
jpeg.seek(0)
try:
    pack.load_png_rgba(jpeg, "fake.png", (1, 1))
except pack.PakError:
    pass
else:
    raise AssertionError("JPEG 被当成 PNG 接受")

# C 宏名不区分原名字大小写；foo 与 FOO 必须在生成头之前拒绝。
row = dict(kind="surf", id="1", name="foo", alpha_mode="xrgb", png="x")
a = pack.Item(row, 0)
b = pack.Item(dict(row, id="2", name="FOO"), 1)
try:
    pack.check_unique([a, b])
except pack.PakError:
    pass
else:
    raise AssertionError("foo/FOO 生成重复宏却未拒绝")
print("input edges ok")
PY
then
    ok "S1b 字库行首 #、非 PNG 内容、大小写宏碰撞三个入口反例"
else
    bad "S1b 打包输入边界反例失败"
    sed -n '1,6p' "$TMP/input_edges.log" | while read -r l; do note "$l"; done
fi

if python3 "$MAIN/tools/assets/gen_placeholder.py" --check >"$TMP/placeholders.log" 2>&1 &&
   ! python3 "$MAIN/tools/assets/gen_placeholder.py" --definitely-unknown \
        >"$TMP/placeholders_badarg.log" 2>&1; then
    ok "S1c 六张占位 PNG 与生成器逐像素一致，未知参数被拒"
else
    bad "S1c 入库占位 PNG 不一致或未知参数未被拒"
    sed -n '1,7p' "$TMP/placeholders.log" | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- S2 独立实现对账
echo "== S2 独立实现校验（另一份代码解包并重算像素） =="
if python3 "$MAIN/tools/check/check_sw5_pak.py" >"$TMP/py.txt" 2>&1; then
    ok "S2a check_sw5_pak.py 全过（$(grep -c '^entry ' "$TMP/py.txt") 项）"
else
    bad "S2a 独立校验报错"
    grep '^FAIL' "$TMP/py.txt" | head -5 | while read -r l; do note "$l"; done
fi
grep '^entry ' "$TMP/py.txt" >"$TMP/py_entries.txt"

# ---------------------------------------------------------------- S3/S4 assetTest
echo "== S3/S4 主机侧装载、解析、画一整帧 =="
if ! make -C "$APP_DIR" -s >"$TMP/build.log" 2>&1; then
    bad "S3a assetTest 编不过"
    tail -5 "$TMP/build.log" | while read -r l; do note "$l"; done
    printf 'fail %d\ndone\n' "$FAIL"; exit 1
fi
BIN="$APP_DIR/build_host/assetTest"
note "二进制 sha256 前 16 位 $(sha256sum "$BIN" | cut -c1-16)"

if SDL_VIDEODRIVER=dummy "$BIN" >"$TMP/run.log" 2>&1; then
    ok "S3b assetTest 退出码 0"
else
    bad "S3b assetTest 退出码非 0"
fi
norm "$TMP/run.log" >"$TMP/run.txt"

if grep -q '^fail 0' "$TMP/run.txt" && grep -q '^done' "$TMP/run.txt"; then
    ok "S3c fail 0 且 done"
else
    bad "S3c fail 不为 0"
    grep 'FAIL' "$TMP/run.txt" | head -8 | while read -r l; do note "$l"; done
fi

nchk=$(grep -c '^chk .* ok$' "$TMP/run.txt")
nbad=$(grep -c '^chk .* FAIL$' "$TMP/run.txt")
if [ "$nbad" -eq 0 ] && [ "$nchk" -ge 18 ]; then
    ok "S3d $nchk 条自检全 ok、0 条 FAIL"
else
    bad "S3d 自检 $nchk 条 ok、$nbad 条 FAIL"
fi

grep '^entry ' "$TMP/run.txt" >"$TMP/c_entries.txt"
if diff -u "$TMP/py_entries.txt" "$TMP/c_entries.txt" >"$TMP/entries.diff" 2>&1; then
    ok "S3e C 侧逐项 CRC 行与独立实现逐行相同"
else
    bad "S3e C 侧逐项 CRC 与独立实现不一致"
    head -8 "$TMP/entries.diff" | while read -r l; do note "$l"; done
fi

if [ ! -f "$REF_LOG" ]; then
    bad "S4a 缺少入库参考日志 $REF_LOG"
else
    norm "$REF_LOG" >"$TMP/ref.txt"
    seq_of() { grep -E '^(frame crc=|win crc=|thumb |[ .:=+*#%@-]{64}$)' "$1"; }
    seq_of "$TMP/run.txt" >"$TMP/seq_got.txt"
    seq_of "$TMP/ref.txt" >"$TMP/seq_ref.txt"
    if diff -u "$TMP/seq_ref.txt" "$TMP/seq_got.txt" >"$TMP/seq.diff" 2>&1; then
        ok "S4a 整帧 CRC、窗口 CRC 与字符画对参考日志逐行相同"
    else
        bad "S4a 画面与参考日志不一致"
        head -8 "$TMP/seq.diff" | while read -r l; do note "$l"; done
    fi
fi

# ---------------------------------------------------------------- S5 注错输入
echo "== S5 七种坏包各自报指定的错 =="

# 造坏包：$1 = 类型，$2 = 输出文件。全部基于正式产物。
corrupt() {
    python3 - "$1" "$ASSETS/build/assets.pak" "$2" <<'PY'
import struct, sys
kind, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
b = bytearray(open(src, "rb").read())
count = struct.unpack_from("<I", b, 12)[0]
if kind == "data1":                      # 第 0 项数据区翻一位 -> entry_crc
    off = struct.unpack_from("<I", b, 32 + 4)[0]
    b[off + 5] ^= 0xFF
elif kind == "magic":                    # -> bad_magic
    b[0] ^= 0xFF
elif kind == "dir":                      # 第 0 项 stride_log2 改成 1 -> bad_geom
    b[32 + 16] = 1
elif kind == "overlap":                  # 第 1 项起点改成第 0 项的 -> overlap
    struct.pack_into("<I", b, 32 + 32 + 4, struct.unpack_from("<I", b, 32 + 4)[0])
elif kind == "bounds":                   # 第 0 项起点顶到包外 -> out_of_bounds
    # 起点取头里的总长向上对齐到 64 字节：对齐、不落进头与目录两项仍然成立，
    # 只剩「越过包末端」这一项，与包有多大无关
    total = struct.unpack_from("<I", b, 16)[0]
    struct.pack_into("<I", b, 32 + 4, (total + 63) & ~63)
elif kind == "trunc":                    # 文件截短到 100 字节 -> bad_total
    b = b[:100]
elif kind == "bigtotal":                 # 头里写 6 MiB 总长，文件只有 32 字节 -> bad_total
    struct.pack_into("<I", b, 16, 6 * 1024 * 1024)
    b = b[:32]
else:
    raise SystemExit("unknown kind %s" % kind)
open(dst, "wb").write(bytes(b))
PY
}

check_err() {
    # $1 标签 $2 坏包类型 $3 期望的「错误码 + 名字」 $4 期望 id（- 表示不断言）
    corrupt "$2" "$TMP/bad_$2.pak" || { bad "$1 造不出坏包"; return; }
    GFX2D_ASSETS_PAK="$TMP/bad_$2.pak" SDL_VIDEODRIVER=dummy "$BIN" >"$TMP/bad_$2.log" 2>&1
    rc=$?
    line="$(grep -m1 '^pak err ' "$TMP/bad_$2.log" || true)"
    if [ $rc -eq 0 ] || [ -z "$line" ]; then
        bad "$1 坏包没有被拒绝（退出码 $rc）"
        return
    fi
    got="$(printf '%s' "$line" | awk '{print $3, $4}')"
    if [ "$got" != "$3" ]; then
        bad "$1 期望「$3」，实际：$line"
        return
    fi
    if [ "$4" != "-" ] && ! printf '%s' "$line" | grep -q "id=$4"; then
        bad "$1 错误码对但没报出条目 ID $4：$line"
        return
    fi
    ok "$1 $line"
}

# 七种坏包。d 那一条落在**读入口**上：host 的文件比头里写的总长短，
# 读不满 declared 长度，报的是板级读错误 board_read（2），
# 而不是解析器的 bad_total——解析器那一条由主机侧单测与 S5g 覆盖。
check_err "S5a 第 0 项数据翻一位" data1    "14 entry_crc"     1
check_err "S5b magic 被改"        magic    "2 bad_magic"      -
check_err "S5c 目录行跨被改"      dir      "8 bad_geom"       -
check_err "S5d 文件比总长短"      trunc    "2 board_read"     -
check_err "S5e 两项数据重叠"      overlap  "11 overlap"       -
check_err "S5f 条目越出包末端"    bounds   "12 out_of_bounds" 1
check_err "S5g 头写 6 MiB 总长（必须在读正文前拒）" bigtotal "6 bad_total" -

# ---------------------------------------------------------------- S6 注错构建
echo "== S6 注错构建：关掉两轮 CRC 之后同一个坏包不再被拒 =="
if ! make -C "$APP_DIR" -s INJECT=ASSET_SKIP_CRC >"$TMP/build_inj.log" 2>&1; then
    bad "S6a 注错构建编不过"
    tail -5 "$TMP/build_inj.log" | while read -r l; do note "$l"; done
else
    INJ="$APP_DIR/build_host/assetTest-inject"
    GFX2D_ASSETS_PAK="$TMP/bad_data1.pak" SDL_VIDEODRIVER=dummy "$INJ" \
        >"$TMP/inj.log" 2>&1
    norm "$TMP/inj.log" >"$TMP/inj.txt"
    if grep -q '^pak err ' "$TMP/inj.txt"; then
        bad "S6b 注错版仍然拒绝了坏包（说明第 5 条判据不是它拦下来的）"
    else
        inj_crc="$(grep -m1 '^frame crc=' "$TMP/inj.txt" || true)"
        ref_crc="$(grep -m1 '^frame crc=' "$TMP/ref.txt" || true)"
        if [ -z "$inj_crc" ]; then
            bad "S6b 注错版没有画出画面"
        elif [ "$inj_crc" = "$ref_crc" ]; then
            bad "S6b 注错版画面与参考相同（坏包没有影响画面？）"
        else
            ok "S6b 注错版收下坏包且画面 CRC 与参考不同（$inj_crc）"
        fi
    fi
fi
make -C "$APP_DIR" -s >/dev/null 2>&1 || true      # 复位成正常构建

# ---------------------------------------------------------------- S7/S8 镜像
echo "== S7/S8 Flash 镜像合成 =="
APP_BIN="$APP_DIR/build/assetTest-O2.bin"
if [ ! -f "$APP_BIN" ]; then
    note "没有板上产物 $APP_BIN，跳过镜像判据（板上构建：make PLATFORM=target RISCV_BIN=<RISCV_BIN>）"
else
    if python3 "$MAIN/tools/board/make_flash_image.py" --app "$APP_BIN" \
            --out "$TMP/flash.hex" >"$TMP/img.log" 2>&1; then
        ok "S7a 合成镜像成功（$(grep -o '镜像 [0-9]* 字节' "$TMP/img.log" | head -1)）"
        if python3 - "$TMP/flash.hex" "$MAIN/fpga/gfx2d_ti60/outflow/gfx2d_ti60.hex" \
                "$APP_BIN" "$ASSETS/build/assets.pak" <<'PY'
import sys
img = bytes(int(t, 16) for t in open(sys.argv[1]).read().split())
bit = bytes(int(t, 16) for t in open(sys.argv[2]).read().split())
app = open(sys.argv[3], "rb").read()
pak = open(sys.argv[4], "rb").read()
ok = True
for name, start, data in (("bit", 0, bit), ("app", 0x380000, app), ("pak", 0x400000, pak)):
    if img[start:start + len(data)] != data:
        print("FAIL %s 分区不一致" % name)
        ok = False
marks = [(0, len(bit)), (0x380000, 0x380000 + len(app)), (0x400000, 0x400000 + len(pak))]
for i, v in enumerate(img):
    if not any(a <= i < b for a, b in marks) and v != 0xFF:
        print("FAIL 偏移 0x%06X 区外不是 0xFF" % i)
        ok = False
        break
print("fail %d" % (0 if ok else 1))
sys.exit(0 if ok else 1)
PY
        then
            ok "S7b 解回后三个分区逐字节一致、区外全 0xFF"
        else
            bad "S7b 镜像解回后与输入不一致"
        fi
    else
        bad "S7a 镜像合成失败"
        tail -3 "$TMP/img.log" | while read -r l; do note "$l"; done
    fi

    # 输入末字节是 0xFF 时必须保留：把 app 末尾补上 0xFF 再合成
    cp "$APP_BIN" "$TMP/app_ff.bin"
    printf '\377\377' >>"$TMP/app_ff.bin"
    if python3 "$MAIN/tools/board/make_flash_image.py" --app "$TMP/app_ff.bin" \
            --out "$TMP/flash_ff.hex" >"$TMP/img_ff.log" 2>&1 &&
       python3 - "$TMP/flash_ff.hex" "$TMP/app_ff.bin" <<'PY'
import sys
img = bytes(int(t, 16) for t in open(sys.argv[1]).read().split())
app = open(sys.argv[2], "rb").read()
seg = img[0x380000:0x380000 + len(app)]
print("fail %d" % (0 if seg == app else 1))
sys.exit(0 if seg == app else 1)
PY
    then
        ok "S8a 输入末字节是 0xFF 时仍然完整保留（$(stat -c %s "$TMP/app_ff.bin") 字节）"
    else
        bad "S8a 以 0xFF 结尾的输入被截断或没保留"
    fi

    # 超限：运行预算收紧到比包小 -> 必须报错退出
    if python3 "$MAIN/tools/board/make_flash_image.py" --app "$APP_BIN" \
            --max-asset 1048576 --out "$TMP/flash_big.hex" >"$TMP/img_big.log" 2>&1; then
        bad "S8b 素材包超过运行预算却合成成功"
    else
        ok "S8b 素材超预算被拒（$(head -1 "$TMP/img_big.log")）"
    fi
    if python3 "$MAIN/tools/board/make_flash_image.py" --app "$APP_BIN" \
            --max-asset 8388608 --out "$TMP/flash_over.hex" >"$TMP/img_over.log" 2>&1; then
        bad "S8c --max-asset 超过 4 MiB 分区却没有被拒"
    else
        ok "S8c --max-asset 不能越过 4 MiB 物理上限"
    fi
    if python3 "$MAIN/tools/board/make_flash_image.py" \
            --bit "$MAIN/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit" \
            --app "$APP_BIN" --out "$TMP/flash_passive.hex" >"$TMP/img_passive.log" 2>&1; then
        bad "S8d passive .bit 被当成 active .hex 接受"
    elif grep -q '不是 active 模式' "$TMP/img_passive.log"; then
        ok "S8d passive .bit 被拒"
    else
        bad "S8d passive .bit 虽失败但原因不对"
        head -2 "$TMP/img_passive.log" | while read -r l; do note "$l"; done
    fi
fi

# ---------------------------------------------------------------- S9 许可
echo "== S9 许可台账 =="
missing=""
while IFS= read -r f; do
    rel="${f#"$ASSETS"/}"
    # 生成物不登记：placeholder 下的 PNG 与 build/ 下的产物
    case "$rel" in
        placeholder/*|build/*) continue ;;
    esac
    if ! grep -qF "$rel" "$ASSETS/LICENSES.md"; then
        missing="$missing $rel"
    fi
done < <(find "$ASSETS" -type f ! -name '*.pyc' | sort)
if [ -z "$missing" ]; then
    ok "S9 LICENSES.md 覆盖了 assets 下每个非生成文件"
else
    bad "S9 这些非生成文件没有登记许可：$missing"
fi

printf 'fail %d\n' "$FAIL"
printf 'done\n'
[ "$FAIL" -eq 0 ] || exit 1
exit 0
