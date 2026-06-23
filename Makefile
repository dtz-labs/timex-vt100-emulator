# Timex 2048/2068 hi-res terminal build.
#
# The z88dk target remains +zx because TC/TS 2048/2068 machines are
# Spectrum-compatible at the binary level; the program selects Timex SCLD
# hi-res mode itself via port 0xff. Running this on non-Timex machines is not
# supported because the terminal relies on 512x192 hi-res video.

BUILD_DIR ?= build
DIST_DIR ?= dist
TARGET ?= term
IF1_TARGET ?= term-if1

VERSION ?= 0.1.0
BUILD_DATE ?= $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
GIT_COMMIT ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
RELEASE_NAME ?= timex-vt100-emulator-$(VERSION)

Z88DK_COMMON_PREFIXES ?= \
	$(HOME)/Programowanie/z88dk \
	/opt/homebrew/opt/z88dk \
	/opt/homebrew \
	/usr/local/opt/z88dk \
	/usr/local \
	/opt/local \
	/opt/z88dk \
	/usr/local/z88dk \
	/usr
ZCC_USER := $(if $(filter undefined,$(origin ZCC)),,$(ZCC))
Z88DK_USER_HOME := $(firstword $(Z88DK_HOME) $(Z88DK))
Z88DK_SEARCH_PREFIXES := $(strip $(Z88DK_USER_HOME) $(Z88DK_COMMON_PREFIXES))
Z88DK_DETECTED_HOME := $(firstword $(foreach dir,$(Z88DK_SEARCH_PREFIXES),$(if $(wildcard $(dir)/bin/zcc),$(dir))))
ZCC_PATH := $(shell command -v zcc 2>/dev/null)
ZCC_DETECTED := $(firstword \
	$(ZCC_USER) \
	$(ZCC_PATH) \
	$(foreach dir,$(Z88DK_SEARCH_PREFIXES),$(if $(wildcard $(dir)/bin/zcc),$(dir)/bin/zcc)))
Z88DK_DETECTED_FROM_ZCC := $(patsubst %/bin/zcc,%,$(filter %/bin/zcc,$(ZCC_DETECTED)))

Z88DK_HOME ?= $(firstword $(Z88DK_USER_HOME) $(Z88DK_DETECTED_FROM_ZCC) $(Z88DK_DETECTED_HOME))
Z88DK ?= $(Z88DK_HOME)
Z88DK_BIN ?= $(if $(Z88DK_HOME),$(Z88DK_HOME)/bin)
ifneq ($(wildcard $(Z88DK_BIN)),)
export PATH := $(Z88DK_BIN):$(PATH)
endif

ZCCCFG_CANDIDATES ?= \
	$(if $(Z88DK_HOME),$(Z88DK_HOME)/lib/config) \
	$(if $(Z88DK_HOME),$(Z88DK_HOME)/share/z88dk/lib/config) \
	/opt/homebrew/share/z88dk/lib/config \
	/usr/local/share/z88dk/lib/config \
	/opt/local/share/z88dk/lib/config \
	/usr/share/z88dk/lib/config \
	/opt/z88dk/lib/config \
	/usr/local/z88dk/lib/config
ZCCCFG_DETECTED := $(firstword $(foreach dir,$(ZCCCFG_CANDIDATES),$(if $(wildcard $(dir)),$(dir))))
ZCCCFG ?= $(ZCCCFG_DETECTED)
ifneq ($(ZCCCFG),)
export ZCCCFG
endif

ZCC ?= $(if $(ZCC_DETECTED),$(ZCC_DETECTED),zcc)
Z88DK_TARGET ?= +zx
Z88DK_CFLAGS ?= -SO3 -clib=sdcc_iy -iquote$(BUILD_DIR) -iquote$(CURDIR)/include
Z88DK_DEFS ?=
Z88DK_LDFLAGS ?= -m
Z88DK_PATH = $(if $(Z88DK_BIN),$(Z88DK_BIN):$(PATH),$(PATH))
Z88DK_ENV = PATH="$(Z88DK_PATH)" $(if $(ZCCCFG),ZCCCFG="$(ZCCCFG)")

CC ?= cc

ZX ?= /Applications/ZEsarUX.app/Contents/MacOS/zesarux
TIMEX_MACHINE ?= TC2048
ZRCP_PORT ?= 10001
ZRCP_HOST ?= 127.0.0.1
TERMINFO_SRC ?= terminfo/timex-vt102.terminfo
TERMINFO_DIR ?= $(HOME)/.terminfo

SOURCES := $(sort $(wildcard src/*.c))
HEADERS := $(sort $(wildcard include/*.h) $(wildcard src/*.h))
BUILD_META := $(BUILD_DIR)/build_meta.h

APP := $(BUILD_DIR)/$(TARGET)
TAP := $(APP).tap
MAP := $(APP).map

IF1_APP := $(BUILD_DIR)/$(IF1_TARGET)
IF1_TAP := $(IF1_APP).tap
IF1_MAP := $(IF1_APP).map
IF1_BAUD ?= RS_BAUD_9600
IF1_DEFS ?= -DCONN_BACKEND_IF1 -DCONN_IF1_BAUD=$(IF1_BAUD)
SERIAL ?=
SERIAL_BAUD ?= 9600
SERIAL_TERM ?= vt100
SERIAL_CMD ?=
ZRCP_MAP ?= $(MAP)
ZRCP_STDIN_MODE ?= immediate
ZRCP_INPUT_NEWLINE ?= cr
ZRCP_OUTPUT_NEWLINE ?= lf
ZRCP_BRIDGE_OPTS ?= --local-echo
ZRCP_TERM ?= vt100
ZRCP_CMD ?=

.DELETE_ON_ERROR:

.PHONY: all tap if1 release-build test host-test ci python-check terminfo-check smoke install-terminfo terminfo run run-zrcp run-if1 bridge-if1 inject-zrcp bridge-zrcp shell-zrcp \
	run-tc2048 run-tc2068 run-ts2068 clean \
	check-z88dk check-zesarux check-timex-machine print-vars FORCE

all: tap

tap: $(TAP)

if1: $(IF1_TAP)

release-build: tap if1
	@mkdir -p "$(DIST_DIR)"
	cp "$(TAP)" "$(DIST_DIR)/$(RELEASE_NAME).tap"
	cp "$(MAP)" "$(DIST_DIR)/$(RELEASE_NAME).map"
	cp "$(IF1_TAP)" "$(DIST_DIR)/$(RELEASE_NAME)-if1.tap"
	cp "$(IF1_MAP)" "$(DIST_DIR)/$(RELEASE_NAME)-if1.map"
	@ls -l "$(DIST_DIR)"

test: host-test

host-test:
	CC="$(CC)" sh test/run.sh

ci: host-test python-check terminfo-check

python-check:
	python3 -m py_compile tools/*.py test/zesarux_smoke.py

terminfo-check: $(TERMINFO_SRC)
	tic -c -x "$(TERMINFO_SRC)"

smoke: $(TAP) check-zesarux
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py

install-terminfo: $(TERMINFO_SRC)
	@command -v tic >/dev/null 2>&1 || { echo "tic not found"; exit 127; }
	tic -x -o "$(TERMINFO_DIR)" "$(TERMINFO_SRC)"

terminfo: install-terminfo

run: $(TAP) check-zesarux check-timex-machine
	"$(ZX)" --noconfigfile --machine "$(TIMEX_MACHINE)" --tape "$(CURDIR)/$(TAP)" --fastautoload

run-zrcp: $(TAP) check-zesarux check-timex-machine
	"$(ZX)" --noconfigfile --machine "$(TIMEX_MACHINE)" --tape "$(CURDIR)/$(TAP)" --fastautoload \
		--enable-remoteprotocol --remoteprotocol-port "$(ZRCP_PORT)"

run-if1: $(IF1_TAP) check-zesarux check-timex-machine
	"$(ZX)" --noconfigfile --machine "$(TIMEX_MACHINE)" --tape "$(CURDIR)/$(IF1_TAP)" --fastautoload

bridge-if1:
	@test -n "$(SERIAL)" || { echo "Set SERIAL=/dev/cu.your-adapter"; exit 2; }
	python3 tools/if1_pty_bridge.py "$(SERIAL)" --baud "$(SERIAL_BAUD)" \
		--term "$(SERIAL_TERM)" --cmd $(SERIAL_CMD)

inject-zrcp: $(TAP)
	python3 tools/zesarux_pipe_inject.py --host "$(ZRCP_HOST)" --port "$(ZRCP_PORT)" --map "$(ZRCP_MAP)"

bridge-zrcp: $(TAP)
	python3 tools/zesarux_stdio_bridge.py --host "$(ZRCP_HOST)" --port "$(ZRCP_PORT)" \
		--map "$(ZRCP_MAP)" --stdin-mode "$(ZRCP_STDIN_MODE)" \
		--input-newline "$(ZRCP_INPUT_NEWLINE)" \
		--output-newline "$(ZRCP_OUTPUT_NEWLINE)" $(ZRCP_BRIDGE_OPTS)

shell-zrcp: $(TAP)
	python3 tools/zesarux_stdio_bridge.py --host "$(ZRCP_HOST)" --port "$(ZRCP_PORT)" \
		--map "$(ZRCP_MAP)" --term "$(ZRCP_TERM)" --cmd $(ZRCP_CMD)

run-tc2048: TIMEX_MACHINE = TC2048
run-tc2048: run

run-tc2068: TIMEX_MACHINE = TC2068
run-tc2068: run

run-ts2068: TIMEX_MACHINE = TS2068
run-ts2068: run

$(TAP): $(SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(TAP)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(Z88DK_DEFS) \
		$(SOURCES) -o "$(APP)" -create-app $(Z88DK_LDFLAGS)

$(IF1_TAP): $(SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(IF1_TAP)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(Z88DK_DEFS) \
		$(IF1_DEFS) $(SOURCES) -o "$(IF1_APP)" -create-app $(Z88DK_LDFLAGS)

$(BUILD_DIR):
	@mkdir -p "$(BUILD_DIR)"

FORCE:

$(BUILD_META): FORCE | $(BUILD_DIR)
	@tmp="$@.tmp"; \
	{ \
		echo "#ifndef BUILD_META_H"; \
		echo "#define BUILD_META_H"; \
		echo "#define APP_VERSION_STR \"$(VERSION)\""; \
		echo "#define APP_BUILD_DATE \"$(BUILD_DATE)\""; \
		echo "#define APP_GIT_COMMIT \"$(GIT_COMMIT)\""; \
		echo "#endif /* BUILD_META_H */"; \
	} > "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then rm "$$tmp"; else mv "$$tmp" "$@"; fi

check-z88dk:
	@{ test -x "$(ZCC)" || PATH="$(Z88DK_PATH)" command -v "$(ZCC)" >/dev/null 2>&1; } || { \
		echo "zcc not found: $(ZCC)"; \
		echo "Install z88dk so zcc is on PATH, or set Z88DK_HOME=/path/to/z88dk, Z88DK=/path/to/z88dk, or ZCC=/path/to/zcc."; \
		exit 127; \
	}
	@PATH="$(Z88DK_PATH)" command -v z88dk-z80asm >/dev/null 2>&1 || { \
		echo "z88dk-z80asm not found on PATH."; \
		echo "Install z88dk so its tools are on PATH, or set Z88DK_HOME=/path/to/z88dk or Z88DK=/path/to/z88dk."; \
		exit 127; \
	}
	@if [ -n "$(ZCCCFG)" ]; then test -d "$(ZCCCFG)" || { \
		echo "ZCCCFG directory not found: $(ZCCCFG)"; \
		echo "Set ZCCCFG=/path/to/z88dk/lib/config, or set Z88DK_HOME/Z88DK to the z88dk prefix."; \
		exit 127; \
	}; fi

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
	@echo "DIST_DIR=$(DIST_DIR)"
	@echo "TARGET=$(TARGET)"
	@echo "TAP=$(TAP)"
	@echo "MAP=$(MAP)"
	@echo "IF1_TARGET=$(IF1_TARGET)"
	@echo "IF1_TAP=$(IF1_TAP)"
	@echo "IF1_MAP=$(IF1_MAP)"
	@echo "IF1_BAUD=$(IF1_BAUD)"
	@echo "VERSION=$(VERSION)"
	@echo "BUILD_DATE=$(BUILD_DATE)"
	@echo "GIT_COMMIT=$(GIT_COMMIT)"
	@echo "RELEASE_NAME=$(RELEASE_NAME)"
	@echo "SERIAL=$(SERIAL)"
	@echo "SERIAL_BAUD=$(SERIAL_BAUD)"
	@echo "SERIAL_TERM=$(SERIAL_TERM)"
	@echo "ZRCP_HOST=$(ZRCP_HOST)"
	@echo "ZRCP_PORT=$(ZRCP_PORT)"
	@echo "ZRCP_MAP=$(ZRCP_MAP)"
	@echo "ZRCP_STDIN_MODE=$(ZRCP_STDIN_MODE)"
	@echo "ZRCP_INPUT_NEWLINE=$(ZRCP_INPUT_NEWLINE)"
	@echo "ZRCP_OUTPUT_NEWLINE=$(ZRCP_OUTPUT_NEWLINE)"
	@echo "ZRCP_BRIDGE_OPTS=$(ZRCP_BRIDGE_OPTS)"
	@echo "ZRCP_TERM=$(ZRCP_TERM)"
	@echo "ZRCP_CMD=$(ZRCP_CMD)"
	@echo "TERMINFO_SRC=$(TERMINFO_SRC)"
	@echo "TERMINFO_DIR=$(TERMINFO_DIR)"
	@echo "Z88DK=$(Z88DK)"
	@echo "Z88DK_HOME=$(Z88DK_HOME)"
	@echo "Z88DK_BIN=$(Z88DK_BIN)"
	@echo "ZCC=$(ZCC)"
	@echo "ZCCCFG=$(ZCCCFG)"
	@echo "Z88DK_TARGET=$(Z88DK_TARGET)"
	@echo "Z88DK_CFLAGS=$(Z88DK_CFLAGS)"
	@echo "TIMEX_MACHINE=$(TIMEX_MACHINE)"
	@echo "ZX=$(ZX)"

clean:
	rm -rf "$(BUILD_DIR)/host"
	rm -rf "$(DIST_DIR)"
	rm -f "$(BUILD_META)" "$(BUILD_META).tmp"
	rm -f "$(APP)" "$(TAP)" "$(APP).map" "$(APP)_CODE.bin"
	rm -f "$(IF1_APP)" "$(IF1_TAP)" "$(IF1_APP).map" "$(IF1_APP)_CODE.bin"
