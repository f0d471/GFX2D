#!/usr/bin/env bash
# 条带数据面正式判据；前期研究仍使用 tools/check/g2_research.py 独立复现。
set -euo pipefail
cd "$(dirname "$0")/../../../.."
python3 Main/tools/check/check_g2_band.py
echo 'SIM OK: 17 formal checks as expected'
