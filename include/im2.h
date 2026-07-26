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
 */
#ifndef IM2_H
#define IM2_H

#define IM2_TABLE_BASE 0xD300
#define IM2_TABLE_FILL 0xD4
#define IM2_TRAMPOLINE 0xD4D4

/* Value loaded into the I register: the table base's high byte. Kept as its
 * own macro (not an expression) because the installer's inline asm needs a
 * plain literal -- the preprocessor cannot compute ">> 8" into an asm operand.
 * The check below keeps it from drifting away from IM2_TABLE_BASE. */
#define IM2_VECTOR_PAGE 0xD3

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
