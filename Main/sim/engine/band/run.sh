#!/usr/bin/env bash
# G2 前期研究入口；正常结果与对应变异均符合才完成，不是正式 BAND 验收。
set -euo pipefail
cd "$(dirname "$0")/../../../.."
python3 Main/tools/check/g2_research.py
echo 'SIM OK: 11 research checks as expected'
