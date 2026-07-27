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
 * HISTORY (2026-07-27 fix wave, superseded below): __crt_stack_size (0x0200 /
 * 512 bytes) is z88dk's *configured* allowance, not measured usage. A canary
 * scan of 0xD000-0xF900 after a realistic session (startup banner render,
 * ~4KB of injected text forcing many scroll cycles, a few keypresses) found
 * the lowest touched byte at 0xEF3D -- 2757 bytes below the 0xFA02 top of the
 * then-current 0xF900 table (0xFA02 - 0xEF3D = 0xAC5 = 2757; an earlier note
 * here miscalculated this as 3427), and a direct hexdump caught live
 * plaintext from the injected session overwriting the table itself at
 * 0xF900-0xFA20 while the program kept running normally. Both 0xF900 and the
 * 0xF000 fallback once considered sat inside that observed range
 * (0xEF3D..0xFF58), so that fix wave moved the table to 0xE000 for real
 * measured clearance rather than an assumed one. At the time, the suspected
 * root cause was a deep call chain in the render/scroll path, not fixed by
 * relocation alone.
 *
 * ROOT CAUSE FOUND AND FIXED (im2-guard follow-up, same date): the "deep call
 * chain" was not recursion or an unusually long call sequence -- it was one
 * oversized stack frame. main() (src/main.c) declared `screen_t scr` and
 * `vtparse_t vt` as locals: screen_t alone is 24 * 80 = 1,920 cell_t, 2 bytes
 * each = 3,840 bytes, plus its scalars, with vtparse_t beside it, together
 * accounting for almost all of the measured 4,123-byte frame (0xFF58 - 0xEF3D
 * = 4,123 bytes: the SP seed down to the 0xEF3D low-water mark above -- this
 * is a call-stack low-water mark including return addresses and nested
 * frames, not literally sizeof(scr)+sizeof(vt), which measures (host cc,
 * -std=c99) at 3,878 + 51 = 3,929 bytes; the remaining ~194 bytes is real
 * call-stack overhead). Both are now file-scope statics in main.c, for the
 * same reason blit_hires.c hoisted its per-row scratch state (see
 * src/main.c's scr/vt comment for the current cross-reference -- that
 * specific blit_hires.c example was replaced by Task 5's per-group dirty
 * rendering, which no longer needs a whole-row scratch buffer, but the
 * "stack depth the linker cannot see" principle is unchanged): it turns
 * stack depth the linker cannot see into BSS that
 * tools/check_image_limit.py's
 * __BSS_END_tail reading can. This moved __BSS_END_tail from 0xCF1C to
 * 0xDE62 (0xDE62 - 0xCF1C = 3,910 bytes) and, per the remeasurement below,
 * cut real stack depth from 4,123 bytes to 0xFF58 - 0xFE8F = 0xC9 = 201 bytes.
 *
 * REMEASURED after the hoist (ZEsarUX ZRCP, term.tap, 2026-07-27): two full
 * runs, each five passes (boot + banner render, ~2.6KB injected text forcing
 * scroll, a keypress burst, a heavier ~7KB session with scroll-region changes
 * and insert/delete-line churn, a second keypress burst), read back with
 * ZRCP's get-visualmem-written-dump instead of a canary fill. The boot pass
 * in both runs showed *every* byte from 0xDE62 (the new image end) to 0xFFFF
 * written 3-5 times each -- the Spectrum's own power-on RAM test/clear, not
 * program activity, confirmed by the fact that it covers 100% of the range
 * uniformly regardless of session content. Excluding that boot artifact, the
 * real low-water mark measured identically in both runs at 0xFE8F (heaviest
 * pass), i.e. 0xFF58 - 0xFE8F = 0xC9 = 201 bytes below the 0xFF58 seed -- far
 * inside the configured 512-byte __crt_stack_size. A planned third,
 * still-heavier pass (a full 24-row screen clear+redraw plus scroll-region
 * extremes) was started but did not finish in the time available, so this
 * measurement, though reproduced twice, is not a proof that 0xFE8F is the
 * absolute deepest the program can ever reach -- only the deepest observed.
 * The table below is placed with margin against both this measured figure
 * and the more conservative __crt_stack_size-implied floor (0xFD58), not
 * against 0xFE8F alone. See the hoist report for the full writeup and raw
 * data. */
#ifndef IM2_H
#define IM2_H

#define IM2_TABLE_BASE 0xEE00
#define IM2_TABLE_FILL 0xEF
#define IM2_TRAMPOLINE 0xEFEF

/* Value loaded into the I register: the table base's high byte. Kept as its
 * own macro (not an expression) because the installer's inline asm needs a
 * plain literal -- the preprocessor cannot compute ">> 8" into an asm operand.
 * The check below keeps it from drifting away from IM2_TABLE_BASE. */
#define IM2_VECTOR_PAGE 0xEE

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
