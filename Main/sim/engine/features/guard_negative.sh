#!/usr/bin/env bash
# 反例：非法构型必须被构建入口在任何产物生成之前拒绝。
# 判据是进程退出码与诊断信息，不是 elaboration——守卫模块有对侧声明，
# 所以非法清单不会在 elaboration 阶段失败，见 feature_build.py 的 error_modules()。
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-/tmp/gfx_feature_elab}
mkdir -p "$WORK"
cd "$ROOT"

fail=0
check() {
  local name="${1:?}" config="${2:?}" expect="${3:?}" dir="$WORK/invalid-${1:?}"
  rm -rf "$dir"
  if python3 Main/tools/check/feature_build.py --config "$config" --out-dir "$dir" \
       >"$WORK/invalid-$name.log" 2>&1; then
    echo "ENTRY-FAIL $name：非法构型 $config 被接受"
    fail=1
    return
  fi
  if [ -e "$dir" ]; then
    echo "ENTRY-FAIL $name：被拒绝但仍生成了产物目录"
    fail=1
    return
  fi
  if grep -q "$expect" "$WORK/invalid-$name.log"; then
    echo "ENTRY-PASS $name：拒绝并指明原因（$expect）"
  else
    echo "ENTRY-WRONG-REASON $name：期望 $expect，实际：$(head -1 "$WORK/invalid-$name.log")"
    fail=1
  fi
}

# 结构位串顺序：TEX BAND_HW STRIP INST PB；像素位：EQ_EXT ALPHA KEY
# 每条只构造一处违规，避免诊断被另一条依赖抢走。
check pb-needs-band    "10001-111-d" "PB 需要 BAND_HW"
check pb-needs-inst    "11101-111-d" "PB 需要 INST"
check strip-needs-band "10100-111-d" "STRIP 需要 BAND_HW"
check delivery-needs-key "11111-110-d" "DELIVERY 需要 KEY"
check delivery-needs-alpha "11111-101-d" "DELIVERY 需要 ALPHA"
check rast-needs-strip "1100010-111-d" "RAST 需要 STRIP"
check rast-tex-needs-rast "1111001-111-d" "RAST_TEX 需要 RAST"
check rast-tex-needs-tex "0111011-111-d" "RAST_TEX 需要 TEX"
check pb-excludes-rast "1111110-111-d" "PB 与 RAST 互斥"

# 正例：研究构型（不带交付标志）必须被接受，且不得被当成交付构型拒掉。
if python3 Main/tools/check/feature_build.py --config "11111-111" --out-dir "$WORK/research-config" \
     >"$WORK/research-config.log" 2>&1; then
  echo "ENTRY-PASS research-config：研究构型被接受"
else
  echo "ENTRY-FAIL research-config：研究构型被拒绝：$(head -1 "$WORK/research-config.log")"
  fail=1
fi

exit $fail
