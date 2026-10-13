#!/usr/bin/env bash
# 公共正常测试在同轮复用；不同注错仍各自构建。
CHECK_INCOMPLETE=0
CHECK_NA=0
GFX_CHECK_PROFILE="${GFX_CHECK_PROFILE:-acceptance}"

check_missing() {
    printf 'INCOMPLETE %s\n' "$*"
    CHECK_INCOMPLETE=$((CHECK_INCOMPLETE + 1))
}

check_na() {
    printf 'NOT_APPLICABLE scope=%s %s\n' "$GFX_CHECK_PROFILE" "$*"
    CHECK_NA=$((CHECK_NA + 1))
}

check_finish() {
    printf 'fail %d\ndone\n' "$FAIL"
    if [ "$FAIL" -ne 0 ]; then
        printf 'VALIDATION FAIL scope=%s\n' "$GFX_CHECK_PROFILE"
        return 1
    fi
    if [ "$CHECK_INCOMPLETE" -ne 0 ]; then
        printf 'VALIDATION INCOMPLETE scope=%s missing=%d\n' "$GFX_CHECK_PROFILE" "$CHECK_INCOMPLETE"
        return 3
    fi
    printf 'VALIDATION PASS scope=%s not_applicable=%d\n' "$GFX_CHECK_PROFILE" "$CHECK_NA"
}

check_libtest() {
    local destination="$1" inject="$2" cache result
    if [ -n "$inject" ] || [ -z "${GFX_CHECK_SHARED:-}" ]; then
        make -C "$SW" -f mk/host.mk test OBJDIR="$destination" INJECT="$inject" >"$destination.log" 2>&1
        return $?
    fi
    cache="$GFX_CHECK_SHARED/libtest"
    mkdir -p "$GFX_CHECK_SHARED"
    if [ ! -f "$cache.status" ] || [ "$(cat "$cache.status")" -ne 0 ]; then
        make -C "$SW" -f mk/host.mk test OBJDIR="$cache" >"$cache.log" 2>&1
        result=$?
        printf '%s\n' "$result" >"$cache.status"
        printf 'EXECUTE shared libtest\n'
    else
        printf 'REUSE shared libtest\n'
    fi
    mkdir -p "$destination/test"
    cp "$cache.log" "$destination.log"
    if [ -f "$cache/test/out64.txt" ]; then cp "$cache/test/out64.txt" "$destination/test/"; fi
    if [ -f "$cache/test/out32.txt" ]; then cp "$cache/test/out32.txt" "$destination/test/"; fi
    return "$(cat "$cache.status")"
}

check_app_test() {
    local destination="$1" cache flags bits extra result=0
    cache="${GFX_CHECK_SHARED:-$destination}/apptest"
    mkdir -p "$cache"
    if [ ! -f "$cache/status" ] || [ "$(cat "$cache/status")" -ne 0 ]; then
        flags="$(make -C "$SW" -s --no-print-directory -f mk/host.mk OBJDIR="$cache/flags" \
            --eval 'check_print_flags: ; @echo -std=gnu11 -O$(OPT) -g $(WARN)' check_print_flags 2>/dev/null)"
        if [ -z "$flags" ]; then return 1; fi
        for bits in 64 32; do
            extra="-m32"
            [ "$bits" = 64 ] && extra="-fsanitize=address,undefined"
            # flags 来自生产构建的参数展开。
            "$HOST_CC" $flags $extra -I"$SW/lib" -I"$APP/src" -o "$cache/app$bits" \
                "$APP"/test/*.c "$APP"/src/game/stg_*.c "$SW"/lib/core/*.c "$SW"/lib/gfx/*.c -lm \
                >"$cache/app$bits.build.log" 2>&1 || result=1
            if [ "$result" = 0 ]; then
                "$cache/app$bits" >"$cache/app$bits.txt" 2>"$cache/app$bits.err" || result=1
            fi
        done
        if [ "$result" = 0 ]; then
            cmp -s "$cache/app64.txt" "$cache/app32.txt" || result=1
            grep -q '^fail 0$' "$cache/app64.txt" || result=1
            grep -q '^done$' "$cache/app64.txt" || result=1
            if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$cache/app64.err"; then result=1; fi
        fi
        printf '%s\n' "$result" >"$cache/status"
        printf 'EXECUTE shared apptest\n'
    else
        printf 'REUSE shared apptest\n'
    fi
    for bits in 64 32; do
        if [ -x "$cache/app$bits" ]; then ln -sf "$cache/app$bits" "$destination/app$bits"; fi
        for extra in txt err build.log; do
            if [ -f "$cache/app$bits.$extra" ]; then cp "$cache/app$bits.$extra" "$destination/app$bits.$extra"; fi
        done
    done
    return "$(cat "$cache/status")"
}
