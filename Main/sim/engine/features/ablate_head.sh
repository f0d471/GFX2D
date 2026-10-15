#!/usr/bin/env bash
# 消融：把指定文件还原到 HEAD，用于按文件量 XLR 变化的来源。
# 只在一次性的构型源树里操作，不动工作区。
# 用法（仓根）：ablate_head.sh <构型源树> <相对 Main 的路径...>
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
TREE=${1:?需要构型源树目录}
shift
cd "$ROOT"
for rel in "$@"; do
  case "$rel" in
    rtl/*) target="$TREE/$rel" ;;
    *)     target="$TREE/rtl/$rel" ;;
  esac
  git show "HEAD:Main/$rel" > "$target" || { echo "FAIL 取 HEAD:$rel"; exit 1; }
  echo "还原 $rel -> $target"
done
