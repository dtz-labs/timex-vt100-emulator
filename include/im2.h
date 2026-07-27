/*
 * im2.h -- placement of the Z80 interrupt-mode-2 vector table.
 *
 * In IM2 the CPU builds the vector address from the I register (high byte) and
 * a byte read off the data bus (low byte). On a Spectrum that bus byte is not
 * reliably any particular value, so the table is filled with ONE repeated byte:
 * whatever the bus supplies, the vector read yields the same address.
 *
 * That gives three linked constants:
 *   IM2_TABLE_BASE = I << 8          (so the base is 256-byte aligned)
 *   IM2_TABLE_FILL = the repeated byte
 *   IM2_TRAMPOLINE = 0x0101 * fill   (where the vector read lands)
 *
 * These addresses are written ABSOLUTELY at runtime. The linker does not model
 * them, so tools/check_image_limit.py parses this header and the link map and
 * fails the build if the image, the table, or the stack overlap. Changing a
 * value here changes the gate too -- that is the point of the single source.
 *
 * The build reads IM2_TABLE_BASE and IM2_TABLE_FILL out of this file with sed,
 * so keep each on its own line in the form `#define NAME 0xNNNN`.
 *
 * MEASURED on target (ZEsarUX ZRCP, term.tap, 2026-07-27): __crt_stack_size
 * (0x0200 / 512 bytes) is z88dk's *configured* allowance, not real usage, and
 * real usage is far deeper. A canary scan of 0xD000-0xF900 after a realistic
 * session (startup banner render, ~4KB of injected text forcing many scroll
 * cycles, a few keypresses) found the lowest touched byte at 0xEF3D -- 3427
 * bytes below the 0xFA02 top of the (then-current) 0xF900 table, and a direct
 * hexdump caught live plaintext from the injected session overwriting the
 * table itself at 0xF900-0xFA20 while the program kept running normally. Both
 * 0xF900 and the 0xF000 fallback once considered here sit inside that
 * observed range (0xEF3D..0xFF58), so this branch moves the table below it,
 * to 0xE000, for real measured clearance rather than an assumed one. See the
 * fix-wave report for the full measurement writeup; the underlying deep call
 * chain (likely in the render/scroll path) is not fixed by relocation alone
 * and needs its own investigation. */
#ifndef IM2_H
#define IM2_H

#define IM2_TABLE_BASE 0xE000
#define IM2_TABLE_FILL 0xE1
#define IM2_TRAMPOLINE 0xE1E1

/* Value loaded into the I register: the table base's high byte. Kept as its
 * own macro (not an expression) because the installer's inline asm needs a
 * plain literal -- the preprocessor cannot compute ">> 8" into an asm operand.
 * The check below keeps it from drifting away from IM2_TABLE_BASE. */
#define IM2_VECTOR_PAGE 0xE0

/* The table must be 256-byte aligned and the trampoline must be where the
 * vector read lands, or interrupts jump into nothing. Checked at compile time
 * so a typo cannot reach the target. */
#if (IM2_TABLE_BASE & 0xFF) != 0
#error "IM2_TABLE_BASE must be 256-byte aligned (it is I << 8)"
#endif
#if IM2_TRAMPOLINE != (0x0101 * IM2_TABLE_FILL)
#error "IM2_TRAMPOLINE must equal 0x0101 * IM2_TABLE_FILL"
#endif
#if IM2_TABLE_BASE != (IM2_VECTOR_PAGE << 8)
#error "IM2_TABLE_BASE must equal IM2_VECTOR_PAGE << 8"
#endif

#endif /* IM2_H */
