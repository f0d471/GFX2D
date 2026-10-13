#!/usr/bin/env bash
# 供聚合入口和软件专项共用的正常宿主测试。
set -u
MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"
APP="$SW/apps/game/gfx2dGame"
HOST_CC="${HOST_CC:-gcc}"
source "$MAIN/tools/check/check_common.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAIL=0
case "${1:-lib}" in
    lib)
        if check_libtest "$TMP/lib" ""; then
            cat "$TMP/lib.log"
            if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/lib.log"; then FAIL=1; fi
        else FAIL=1; fi ;;
    app)
        if check_app_test "$TMP"; then cat "$TMP/app64.txt"; else FAIL=1; fi ;;
    *) printf 'usage: check_host.sh lib|app\n'; exit 2 ;;
esac
check_finish
