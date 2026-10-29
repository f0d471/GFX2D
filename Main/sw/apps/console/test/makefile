# Tests compile the real manager/registry with faulting platform callbacks,
# then execute the real host frontend. Artifacts use check-runs, never src/.
REPO := $(abspath ../../../../..)
.PHONY: all
all:
	cd "$(REPO)" && python3 Main/tools/check/check_console.py --profile quick
