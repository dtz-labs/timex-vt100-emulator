# Timex 2048/2068 hi-res terminal build, and (Task 9) a plain ZX Spectrum
# 40-column build from the same sources.
#
# The z88dk target is +zx for BOTH builds: TC/TS 2048/2068 machines are
# Spectrum-compatible at the binary level, so the same target works whether
# the program ends up selecting Timex SCLD hi-res mode (src/video_hires.c) or
# plain ULA text mode (src/video_ula.c). `make tap`/`make if1` produce the
# 80x24 Timex hi-res build; `make tap-zx`/`make if1-zx` produce the 40x24 ZX
# ULA build -- see TARGET_DEFS and ZX_SOURCES below for how the two diverge
# from one shared Makefile. Running the Timex build on a machine without an
# SCLD is not supported because it relies on 512x192 hi-res video -- src/main.c
# probes for an SCLD (src/machine.c) and refuses to start without one, rather
# than painting an unreadable half-image. The ZX build has no such guard: it
# only ever writes the single ULA display file every Spectrum has.

BUILD_DIR ?= build
DIST_DIR ?= dist
TARGET ?= term
IF1_TARGET ?= term-if1

VERSION ?= 0.1.0
BUILD_DATE ?= $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
GIT_COMMIT ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
RELEASE_NAME ?= zx-vt102-terminal-$(VERSION)

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
# Z88DK_DEFS is the user's overridable knob for extra defines (e.g. -DDEBUG),
# left empty by default -- kept separate from TARGET_DEFS below so overriding
# it (as `?=` invites: `make tap Z88DK_DEFS=-DDEBUG`) cannot silently drop the
# define that selects which machine this build targets.
Z88DK_DEFS ?=
# TARGET_DEFS is this target's identity, not a user knob: it selects which of
# src/main.c's (and screen.h's) #ifdef TERM_TIMEX / TERM_ZX branches compiles
# in -- the guard, COLS, and the BANNER_HW string among them. Deliberately
# `:=`, not `?=`, for the same reason as before Task 9: if this were the
# user's overridable knob, `make tap Z88DK_DEFS=-DDEBUG` -- the ordinary way
# to use an overridable *_DEFS variable -- would work fine, but the
# equivalent slip on this one would silently build a Timex TAP that claims to
# be a ZX Spectrum (or vice versa) and pass every check that does not
# deliberately look for the guard/banner. Do not change this back to `?=`.
# Matches the TERM_DEF test/run.sh already uses for the host suite.
#
# Task 9 adds a second machine (tap-zx/if1-zx), which needs a DIFFERENT fixed
# value of this SAME variable rather than a second TIMEX_DEFS/ZX_DEFS pair --
# two names would let a copy-pasted recipe use the wrong one and no test
# would catch it, since neither TAP is host-testable. The global default
# below is the Timex value; the ZX targets override it with a target-specific
# variable value (see "$(ZX_TAP) $(ZX_IF1_TAP): TARGET_DEFS := -DTERM_ZX"
# below) -- a plain Make feature that only takes effect while building that
# target's recipe, not a new mechanism. This keeps the exact same
# non-displaceable property: `make tap-zx Z88DK_DEFS=-DDEBUG` still gets
# TARGET_DEFS=-DTERM_ZX, because Z88DK_DEFS and TARGET_DEFS remain distinct
# variables no matter which target is building.
TARGET_DEFS := -DTERM_TIMEX
Z88DK_LDFLAGS ?= -m
Z88DK_PATH = $(if $(Z88DK_BIN),$(Z88DK_BIN):$(PATH),$(PATH))
Z88DK_ENV = PATH="$(Z88DK_PATH)" $(if $(ZCCCFG),ZCCCFG="$(ZCCCFG)")

CC ?= cc

ZX ?= /Applications/ZEsarUX.app/Contents/MacOS/zesarux
TIMEX_MACHINE ?= TC2048
ZRCP_PORT ?= 10001
ZRCP_HOST ?= 127.0.0.1
TERMINFO_SRC ?= terminfo/timex-vt102.terminfo
TERMINFO_ZX_SRC ?= terminfo/zx-vt102.terminfo
# Both entries, so a malformed one fails terminfo-check/install-terminfo
# instead of only ever exercising the Timex one.
TERMINFO_SRCS := $(TERMINFO_SRC) $(TERMINFO_ZX_SRC)
TERMINFO_DIR ?= $(HOME)/.terminfo

# Explicit, not a wildcard: render_hires.c/render_ula.c and blit_hires.c/
# blit_ula.c are ALTERNATIVES exporting the same symbols. A wildcard would link
# both and fail on duplicate symbols.
#
# src/machine.c is target-only (calls z80_inp/z80_outp) and lives in
# COMMON_SOURCES so both TAP builds link from one source list. The Timex
# build actually calls machine_has_scld()/machine_caps_shift_held() (guarded
# by `#ifdef TERM_TIMEX` around both the functions' only call site and
# guard_refuse() in src/main.c); the ZX build compiles and links machine.c's
# object but never calls into it, since that whole block is compiled out --
# a small, accepted amount of dead code in exchange for one shared source
# list instead of a third *_SOURCES split. It must never appear in a host
# test link line -- test/run.sh lists its sources explicitly and does not
# include it.
COMMON_SOURCES := src/conn.c src/font.c src/keybuf.c src/keymap.c src/machine.c \
	src/main.c src/render.c src/screen.c src/vtparse.c
TIMEX_SOURCES := $(COMMON_SOURCES) src/hires.c src/render_hires.c \
	src/blit_hires.c src/video_hires.c
ZX_SOURCES := $(COMMON_SOURCES) src/ula.c src/render_ula.c src/blit_ula.c \
	src/video_ula.c
SOURCES := $(TIMEX_SOURCES)
HEADERS := $(sort $(wildcard include/*.h) $(wildcard src/*.h))
BUILD_META := $(BUILD_DIR)/build_meta.h

ZX_TARGET ?= term-zx
ZX_IF1_TARGET ?= term-zx-if1

APP := $(BUILD_DIR)/$(TARGET)
TAP := $(APP).tap
MAP := $(APP).map

IF1_APP := $(BUILD_DIR)/$(IF1_TARGET)
IF1_TAP := $(IF1_APP).tap
IF1_MAP := $(IF1_APP).map

ZX_APP := $(BUILD_DIR)/$(ZX_TARGET)
ZX_TAP := $(ZX_APP).tap
ZX_MAP := $(ZX_APP).map

ZX_IF1_APP := $(BUILD_DIR)/$(ZX_IF1_TARGET)
ZX_IF1_TAP := $(ZX_IF1_APP).tap
ZX_IF1_MAP := $(ZX_IF1_APP).map

# The ZX targets are the same variable, TARGET_DEFS, with a different fixed
# value that applies only while building these two targets' recipes -- see
# the comment on the Timex default above. Also deliberately `:=`, for the
# same reason.
$(ZX_TAP) $(ZX_IF1_TAP): TARGET_DEFS := -DTERM_ZX

# IM2 vector table placement. main.c writes these addresses absolutely, so the
# linker cannot know about them -- check_image_limit.py is what enforces them.
# Parsed out of include/im2.h so the gate and the program cannot disagree.
IM2_TABLE_BASE ?= $(shell sed -n 's/^\#define IM2_TABLE_BASE[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' include/im2.h)
IM2_TABLE_FILL ?= $(shell sed -n 's/^\#define IM2_TABLE_FILL[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' include/im2.h)
CHECK_IMAGE_LIMIT = python3 tools/check_image_limit.py

# The z88dk/z88dk:latest image CI builds inside (an Alpine base) has no python3,
# so the gate cannot run as part of the `zcc` recipe there. CI instead builds
# with SKIP_IMAGE_LIMIT_CHECK=1 and runs `make check-image-limit` as its own
# step on the runner afterwards, against the .map files the container produced
# (see .github/workflows/ci.yml and release.yml). Local builds leave this unset
# and get the check inline, as before.
SKIP_IMAGE_LIMIT_CHECK ?= 0
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

.PHONY: all tap if1 tap-zx if1-zx release-build test host-test ci python-check terminfo-check smoke smoke-zx bench install-terminfo terminfo run run-zrcp run-if1 bridge-if1 inject-zrcp bridge-zrcp shell-zrcp \
	run-tc2048 run-tc2068 run-ts2068 clean \
	check-z88dk check-zesarux check-timex-machine check-image-limit print-vars FORCE

all: tap

tap: $(TAP)

if1: $(IF1_TAP)

tap-zx: $(ZX_TAP)

if1-zx: $(ZX_IF1_TAP)

release-build: tap if1 tap-zx if1-zx
	@mkdir -p "$(DIST_DIR)"
	cp "$(TAP)" "$(DIST_DIR)/$(RELEASE_NAME).tap"
	cp "$(MAP)" "$(DIST_DIR)/$(RELEASE_NAME).map"
	cp "$(IF1_TAP)" "$(DIST_DIR)/$(RELEASE_NAME)-if1.tap"
	cp "$(IF1_MAP)" "$(DIST_DIR)/$(RELEASE_NAME)-if1.map"
	cp "$(ZX_TAP)" "$(DIST_DIR)/$(RELEASE_NAME)-zx.tap"
	cp "$(ZX_MAP)" "$(DIST_DIR)/$(RELEASE_NAME)-zx.map"
	cp "$(ZX_IF1_TAP)" "$(DIST_DIR)/$(RELEASE_NAME)-zx-if1.tap"
	cp "$(ZX_IF1_MAP)" "$(DIST_DIR)/$(RELEASE_NAME)-zx-if1.map"
	@ls -l "$(DIST_DIR)"

test: host-test

host-test:
	CC="$(CC)" sh test/run.sh
	python3 test/test_check_image_limit.py
	python3 test/test_alink_frame_py.py

ci: host-test python-check terminfo-check

# NOT a prerequisite of `ci`: `ci` is designed to run with only a host C
# compiler and Python (see host-tests in .github/workflows/ci.yml, which is
# plain ubuntu-latest -- no z88dk). z88dk is only present in the separate
# build-tap job's z88dk/z88dk:latest container, which is where
# .github/workflows/ci.yml runs this target, as its own non-blocking,
# printed-not-gated step (D25) -- see tools/bench.sh's own header for why a
# regression threshold isn't safe against an unpinned `:latest` image.
bench:
	sh tools/bench.sh

python-check:
	python3 -m py_compile tools/*.py test/zesarux_smoke.py test/test_check_image_limit.py

terminfo-check: $(TERMINFO_SRCS)
	@for f in $(TERMINFO_SRCS); do tic -c -x "$$f" || exit 1; done

# Covers three of the five TAP/machine combinations from
# docs/superpowers/specs/2026-07-26-zx-spectrum-target-design.md (10) --
# term.tap on its native machine, the guard firing on a plain Spectrum, and
# CAPS SHIFT bypassing it -- plus a real-scroll content check (see
# test/zesarux_smoke.py's module docstring). `smoke-zx` below covers the
# other two, both of which use term-zx.tap.
smoke: $(TAP) check-zesarux
	@echo "=== smoke: term.tap on $(TIMEX_MACHINE) ==="
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py --tap "$(TAP)" --machine $(TIMEX_MACHINE) --geom hires --scenario normal
	@echo "=== smoke: term.tap on 48k (guard must fire) ==="
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py --tap "$(TAP)" --machine 48k --geom hires --scenario guard
	@echo "=== smoke: term.tap on 48k, CAPS SHIFT held (guard bypassed) ==="
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py --tap "$(TAP)" --machine 48k --geom hires --scenario bypass
	@echo "=== smoke: term.tap real-scroll content check on $(TIMEX_MACHINE) ==="
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py --tap "$(TAP)" --machine $(TIMEX_MACHINE) --geom hires --scenario scroll

# The other two combinations: term-zx.tap on its native machine, and --
# the row the whole "ZX TAP is the safe default" argument rests on --
# term-zx.tap on a Timex.
smoke-zx: $(ZX_TAP) check-zesarux
	@echo "=== smoke-zx: term-zx.tap on 48k ==="
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py --tap "$(ZX_TAP)" --machine 48k --geom ula --scenario normal
	@echo "=== smoke-zx: term-zx.tap on $(TIMEX_MACHINE) (the safe-default claim) ==="
	ZRCP_PORT="$(ZRCP_PORT)" python3 test/zesarux_smoke.py --tap "$(ZX_TAP)" --machine $(TIMEX_MACHINE) --geom ula --scenario normal

# Runs the IM2 image-limit gate standalone against already-built .map files.
# Used by CI as its own step outside the z88dk container (see
# SKIP_IMAGE_LIMIT_CHECK above), but works locally too: `make tap if1 tap-zx
# if1-zx SKIP_IMAGE_LIMIT_CHECK=1 && make check-image-limit`. Covers all four
# TAPs -- the ZX build gets the same gate as the Timex one.
check-image-limit:
	@test -f "$(MAP)" || { echo "$(MAP) not found; build $(TAP) first"; exit 1; }
	$(CHECK_IMAGE_LIMIT) "$(MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL)
	@test -f "$(IF1_MAP)" || { echo "$(IF1_MAP) not found; build $(IF1_TAP) first"; exit 1; }
	$(CHECK_IMAGE_LIMIT) "$(IF1_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL)
	@test -f "$(ZX_MAP)" || { echo "$(ZX_MAP) not found; build $(ZX_TAP) first"; exit 1; }
	$(CHECK_IMAGE_LIMIT) "$(ZX_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL)
	@test -f "$(ZX_IF1_MAP)" || { echo "$(ZX_IF1_MAP) not found; build $(ZX_IF1_TAP) first"; exit 1; }
	$(CHECK_IMAGE_LIMIT) "$(ZX_IF1_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL)

install-terminfo: $(TERMINFO_SRCS)
	@command -v tic >/dev/null 2>&1 || { echo "tic not found"; exit 127; }
	@for f in $(TERMINFO_SRCS); do tic -x -o "$(TERMINFO_DIR)" "$$f" || exit 1; done

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

BUILD_DATE_H := $(BUILD_DIR)/build_date.h

$(TAP): $(SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(TAP)"
	@printf '#define APP_BUILD_DATE "%s"\n' "$(BUILD_DATE)" > "$(BUILD_DATE_H)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(TARGET_DEFS) $(Z88DK_DEFS) \
		$(SOURCES) -o "$(APP)" -create-app $(Z88DK_LDFLAGS)
	@if [ "$(SKIP_IMAGE_LIMIT_CHECK)" != "1" ]; then \
		$(CHECK_IMAGE_LIMIT) "$(MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL); \
	fi

$(IF1_TAP): $(SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(IF1_TAP)"
	@printf '#define APP_BUILD_DATE "%s"\n' "$(BUILD_DATE)" > "$(BUILD_DATE_H)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(TARGET_DEFS) $(Z88DK_DEFS) \
		$(IF1_DEFS) $(SOURCES) -o "$(IF1_APP)" -create-app $(Z88DK_LDFLAGS)
	@if [ "$(SKIP_IMAGE_LIMIT_CHECK)" != "1" ]; then \
		$(CHECK_IMAGE_LIMIT) "$(IF1_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL); \
	fi

# Mirrors $(TAP)/$(IF1_TAP) above, one-for-one, but with $(ZX_SOURCES) instead
# of $(SOURCES) -- TARGET_DEFS is -DTERM_ZX for these two targets via the
# target-specific variable value set earlier in this file.
$(ZX_TAP): $(ZX_SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(ZX_TAP)"
	@printf '#define APP_BUILD_DATE "%s"\n' "$(BUILD_DATE)" > "$(BUILD_DATE_H)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(TARGET_DEFS) $(Z88DK_DEFS) \
		$(ZX_SOURCES) -o "$(ZX_APP)" -create-app $(Z88DK_LDFLAGS)
	@if [ "$(SKIP_IMAGE_LIMIT_CHECK)" != "1" ]; then \
		$(CHECK_IMAGE_LIMIT) "$(ZX_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL); \
	fi

$(ZX_IF1_TAP): $(ZX_SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(ZX_IF1_TAP)"
	@printf '#define APP_BUILD_DATE "%s"\n' "$(BUILD_DATE)" > "$(BUILD_DATE_H)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(TARGET_DEFS) $(Z88DK_DEFS) \
		$(IF1_DEFS) $(ZX_SOURCES) -o "$(ZX_IF1_APP)" -create-app $(Z88DK_LDFLAGS)
	@if [ "$(SKIP_IMAGE_LIMIT_CHECK)" != "1" ]; then \
		$(CHECK_IMAGE_LIMIT) "$(ZX_IF1_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL); \
	fi

$(BUILD_DIR):
	@mkdir -p "$(BUILD_DIR)"

FORCE:

# APP_BUILD_DATE is deliberately absent here: a timestamp in this cmp-guarded
# header would change on every make run and force a full zcc rebuild. The tap
# recipes write it to build_date.h right before compiling, so only real
# rebuilds get a fresh stamp. (zcc cannot pass string macros via -D; it strips
# the inner quotes.)
$(BUILD_META): FORCE | $(BUILD_DIR)
	@tmp="$@.tmp"; \
	{ \
		echo "#ifndef BUILD_META_H"; \
		echo "#define BUILD_META_H"; \
		echo "#define APP_VERSION_STR \"$(VERSION)\""; \
		echo "#define APP_GIT_COMMIT \"$(GIT_COMMIT)\""; \
		echo "#include \"build_date.h\""; \
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
	@echo "ZX_TARGET=$(ZX_TARGET)"
	@echo "ZX_TAP=$(ZX_TAP)"
	@echo "ZX_MAP=$(ZX_MAP)"
	@echo "ZX_IF1_TARGET=$(ZX_IF1_TARGET)"
	@echo "ZX_IF1_TAP=$(ZX_IF1_TAP)"
	@echo "ZX_IF1_MAP=$(ZX_IF1_MAP)"
	@echo "IF1_BAUD=$(IF1_BAUD)"
	@echo "IM2_TABLE_BASE=$(IM2_TABLE_BASE)"
	@echo "IM2_TABLE_FILL=$(IM2_TABLE_FILL)"
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
	@echo "TERMINFO_ZX_SRC=$(TERMINFO_ZX_SRC)"
	@echo "TERMINFO_DIR=$(TERMINFO_DIR)"
	@echo "Z88DK=$(Z88DK)"
	@echo "Z88DK_HOME=$(Z88DK_HOME)"
	@echo "Z88DK_BIN=$(Z88DK_BIN)"
	@echo "ZCC=$(ZCC)"
	@echo "ZCCCFG=$(ZCCCFG)"
	@echo "Z88DK_TARGET=$(Z88DK_TARGET)"
	@echo "Z88DK_CFLAGS=$(Z88DK_CFLAGS)"
	@echo "Z88DK_DEFS=$(Z88DK_DEFS)"
	@echo "TARGET_DEFS=$(TARGET_DEFS)"
	@echo "TIMEX_MACHINE=$(TIMEX_MACHINE)"
	@echo "ZX=$(ZX)"

clean:
	rm -rf "$(BUILD_DIR)/host"
	rm -rf "$(DIST_DIR)"
	rm -f "$(BUILD_META)" "$(BUILD_META).tmp" "$(BUILD_DATE_H)"
	rm -f "$(APP)" "$(TAP)" "$(APP).map" "$(APP)_CODE.bin"
	rm -f "$(IF1_APP)" "$(IF1_TAP)" "$(IF1_APP).map" "$(IF1_APP)_CODE.bin"
	rm -f "$(ZX_APP)" "$(ZX_TAP)" "$(ZX_APP).map" "$(ZX_APP)_CODE.bin"
	rm -f "$(ZX_IF1_APP)" "$(ZX_IF1_TAP)" "$(ZX_IF1_APP).map" "$(ZX_IF1_APP)_CODE.bin"
