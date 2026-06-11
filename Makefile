# BarTender — firmware convenience targets (ESP-IDF 5.3+)
#
# Usage:
#   make flash              # build + flash (auto-detect USB port)
#   make flash PORT=/dev/tty.usbmodem101
#   make monitor            # serial monitor only
#   make build              # compile firmware
#
# If idf.py is not on PATH, sources IDF_EXPORT (default: ~/esp/esp-idf/export.sh).

.PHONY: help build flash monitor flash-monitor check-port

FW_DIR := firmware
PORT ?= $(firstword $(wildcard /dev/tty.usbmodem*))
IDF_EXPORT ?= $(firstword $(wildcard $(HOME)/esp/esp-idf/export.sh))

# Source ESP-IDF in the same shell as idf.py when not already exported.
define ensure_idf
if command -v idf.py >/dev/null 2>&1; then :; \
elif [ -n "$(IDF_EXPORT)" ] && [ -f "$(IDF_EXPORT)" ]; then . "$(IDF_EXPORT)"; \
else \
	echo "error: idf.py not found" >&2; \
	echo "  source ~/esp/esp-idf/export.sh" >&2; \
	echo "  or: make flash IDF_EXPORT=/path/to/esp-idf/export.sh" >&2; \
	exit 1; \
fi
endef

help:
	@echo "BarTender firmware targets:"
	@echo "  make build          Compile firmware (idf.py build)"
	@echo "  make flash          Build and flash to ESP32-S3"
	@echo "  make monitor        Open serial monitor"
	@echo "  make flash-monitor  Build, flash, then monitor"
	@echo ""
	@echo "Set PORT if auto-detect fails, e.g.: make flash PORT=/dev/tty.usbmodem101"

check-port:
	@test -n "$(PORT)" || { \
		echo "error: no ESP32 USB port found (expected /dev/tty.usbmodem*)" >&2; \
		echo "       connect the board or run: make flash PORT=/dev/tty.usbmodemNNN" >&2; \
		exit 1; \
	}

build:
	@$(ensure_idf); cd $(FW_DIR) && idf.py build

flash: check-port build
	@$(ensure_idf); cd $(FW_DIR) && idf.py -p "$(PORT)" flash

monitor: check-port
	@$(ensure_idf); cd $(FW_DIR) && idf.py -p "$(PORT)" monitor

flash-monitor: flash monitor
