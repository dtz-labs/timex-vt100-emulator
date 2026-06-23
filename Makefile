# Timex 2048/2068 hi-res terminal build.
#
# The z88dk target remains +zx because TC/TS 2048/2068 machines are
# Spectrum-compatible at the binary level; the program selects Timex SCLD
# hi-res mode itself via port 0xff. Running this on non-Timex machines is not
# supported because the terminal relies on 512x192 hi-res video.

BUILD_DIR ?= build
TARGET ?= term
IF1_TARGET ?= term-if1

Z88DK ?= $(HOME)/Programowanie/z88dk
ZCC ?= $(Z88DK)/bin/zcc
ZCCCFG ?= $(Z88DK)/lib/config
Z88DK_TARGET ?= +zx
Z88DK_CFLAGS ?= -SO3 -clib=sdcc_iy -iquote$(CURDIR)/include
Z88DK_DEFS ?=
Z88DK_LDFLAGS ?=
Z88DK_ENV = PATH="$(Z88DK)/bin:$(PATH)" ZCCCFG="$(ZCCCFG)"

CC ?= cc

ZX ?= /Applications/ZEsarUX.app/Contents/MacOS/zesarux
TIMEX_MACHINE ?= TC2048
ZRCP_PORT ?= 10001

SOURCES := $(sort $(wildcard src/*.c))
HEADERS := $(sort $(wildcard include/*.h) $(wildcard src/*.h))

APP := $(BUILD_DIR)/$(TARGET)
TAP := $(APP).tap

IF1_APP := $(BUILD_DIR)/$(IF1_TARGET)
IF1_TAP := $(IF1_APP).tap
IF1_BAUD ?= RS_BAUD_9600
IF1_DEFS ?= -DCONN_BACKEND_IF1 -DCONN_IF1_BAUD=$(IF1_BAUD)

.DELETE_ON_ERROR:

.PHONY: all tap if1 test host-test smoke run run-if1 run-tc2048 run-tc2068 run-ts2068 clean \
	check-z88dk check-zesarux check-timex-machine print-vars

all: tap

tap: $(TAP)

if1: $(IF1_TAP)

test: host-test

host-test:
	CC="$(CC)" sh test/run.sh

smoke: $(TAP) check-zesarux
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py

run: $(TAP) check-zesarux check-timex-machine
	"$(ZX)" --noconfigfile --machine "$(TIMEX_MACHINE)" --tape "$(CURDIR)/$(TAP)" --fastautoload

run-if1: $(IF1_TAP) check-zesarux check-timex-machine
	"$(ZX)" --noconfigfile --machine "$(TIMEX_MACHINE)" --tape "$(CURDIR)/$(IF1_TAP)" --fastautoload

run-tc2048: TIMEX_MACHINE = TC2048
run-tc2048: run

run-tc2068: TIMEX_MACHINE = TC2068
run-tc2068: run

run-ts2068: TIMEX_MACHINE = TS2068
run-ts2068: run

$(TAP): $(SOURCES) $(HEADERS) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(TAP)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(Z88DK_DEFS) \
		$(SOURCES) -o "$(APP)" -create-app $(Z88DK_LDFLAGS)

$(IF1_TAP): $(SOURCES) $(HEADERS) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(IF1_TAP)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(Z88DK_DEFS) \
		$(IF1_DEFS) $(SOURCES) -o "$(IF1_APP)" -create-app $(Z88DK_LDFLAGS)

$(BUILD_DIR):
	@mkdir -p "$(BUILD_DIR)"

check-z88dk:
	@PATH="$(Z88DK)/bin:$(PATH)" command -v "$(ZCC)" >/dev/null 2>&1 || { \
		echo "zcc not found: $(ZCC)"; \
		echo "Set Z88DK=/path/to/z88dk or ZCC=/path/to/zcc."; \
		exit 127; \
	}
	@PATH="$(Z88DK)/bin:$(PATH)" command -v z88dk-z80asm >/dev/null 2>&1 || { \
		echo "z88dk-z80asm not found on PATH."; \
		echo "Set Z88DK=/path/to/z88dk or add z88dk/bin to PATH."; \
		exit 127; \
	}
	@test -d "$(ZCCCFG)" || { \
		echo "ZCCCFG directory not found: $(ZCCCFG)"; \
		echo "Set Z88DK=/path/to/z88dk or ZCCCFG=/path/to/z88dk/lib/config."; \
		exit 127; \
	}

check-zesarux:
	@command -v "$(ZX)" >/dev/null 2>&1 || { \
		echo "ZEsarUX not found: $(ZX)"; \
		echo "Set ZX=/path/to/zesarux."; \
		exit 127; \
	}

check-timex-machine:
	@case "$(TIMEX_MACHINE)" in \
		TC2048|TC2068|TS2068) ;; \
		*) echo "Unsupported TIMEX_MACHINE=$(TIMEX_MACHINE). Use TC2048, TC2068, or TS2068."; exit 2 ;; \
	esac

print-vars:
	@echo "BUILD_DIR=$(BUILD_DIR)"
	@echo "TARGET=$(TARGET)"
	@echo "TAP=$(TAP)"
	@echo "IF1_TARGET=$(IF1_TARGET)"
	@echo "IF1_TAP=$(IF1_TAP)"
	@echo "IF1_BAUD=$(IF1_BAUD)"
	@echo "ZCC=$(ZCC)"
	@echo "ZCCCFG=$(ZCCCFG)"
	@echo "Z88DK_TARGET=$(Z88DK_TARGET)"
	@echo "Z88DK_CFLAGS=$(Z88DK_CFLAGS)"
	@echo "TIMEX_MACHINE=$(TIMEX_MACHINE)"
	@echo "ZX=$(ZX)"

clean:
	rm -rf "$(BUILD_DIR)/host"
	rm -f "$(APP)" "$(TAP)" "$(APP).map" "$(APP)_CODE.bin"
	rm -f "$(IF1_APP)" "$(IF1_TAP)" "$(IF1_APP).map" "$(IF1_APP)_CODE.bin"
