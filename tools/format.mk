.PHONY: format format-check

FORMAT_SCRIPT := $(abspath tools/format.py)

format:
	cd /tmp && python3 $(FORMAT_SCRIPT) --write

format-check:
	cd /tmp && python3 $(FORMAT_SCRIPT) --check
