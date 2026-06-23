# Repository Guidelines

## Project Structure & Module Organization

This repository is a C implementation of a Timex TC2048 VT-100 terminal core. Runtime code lives in `src/`, with matching public headers in `include/`. Keep pure, host-testable logic in modules such as `screen.c`, `vtparse.c`, `hires.c`, `keybuf.c`, and `conn.c`; isolate Timex/Z80 hardware details in `video.c`, target keyboard access, and render paths. Host tests live in `test/test_*.c`, with `test/run.sh` as the canonical fast test runner. `tools/genfont.py` maintains font data, and `docs/superpowers/specs/` contains design decisions and target-toolchain notes. Treat `build/`, `*.tap`, `*.bin`, and `*.map` as generated artifacts unless explicitly updating release outputs.

## Build, Test, and Development Commands

- `test/run.sh`: compiles and runs all native host unit tests with `cc`, `-std=c99`, `-Wall`, `-Wextra`, and `-Werror`.
- `CC=clang test/run.sh`: runs the same tests with a selected compiler.
- `export PATH="$HOME/Programowanie/z88dk/bin:$PATH"` and `export ZCCCFG="$HOME/Programowanie/z88dk/lib/config"`: prepare the z88dk target toolchain.
- `mkdir -p build && zcc +zx -SO3 -clib=sdcc_iy -iquote"$PWD/include" src/*.c -o build/term -create-app`: builds the TC2048 `.tap`.
- `/Applications/ZEsarUX.app/Contents/MacOS/zesarux --machine TC2048 --tape build/term.tap`: runs the target build in ZEsarUX.

## Coding Style & Naming Conventions

Use C99, four-space indentation, K&R-style braces as in existing sources, and concise block comments only where they clarify non-obvious behavior. Prefer fixed-width aliases from `include/types.h` (`u8`, `s8`, `u16`, `s16`). Keep APIs pointer-oriented: pass structs by pointer or out-pointer, and avoid returning structs by value for SDCC/z88dk compatibility. Do not introduce float, malloc, recursion, or headers that shadow z88dk system headers.

## Testing Guidelines

Add or update `test/test_<module>.c` for pure logic changes. Tests use plain `assert` plus local `CHECK` counters, so keep cases explicit and deterministic. Run `test/run.sh` before committing. Hardware-facing behavior should be verified with ZEsarUX, and smoke automation belongs in `test/zesarux_smoke.py`.

## Commit & Pull Request Guidelines

Recent commits use scoped, imperative subjects such as `vtparse: add CSI erase handling` or `screen: scroll region set`. Prefer `module: concise change`, and mention `host TDD` when tests drive the change. Pull requests should describe behavior changes, list test/emulator commands run, link relevant specs or issues, and include screenshots or RAM/smoke-test artifacts for visible TC2048 rendering changes.
