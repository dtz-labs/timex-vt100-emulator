/*
 * test_ula.c -- host unit tests for the plain ZX Spectrum ULA address math.
 *
 * Mirrors test_hires.c: verifies the "thirds" interleave formula against
 * known addresses, and checks that ula_row_scanline_offset() (moved out of
 * blit_hires.c's row_scanline_offset in Task 9, since the formula is
 * identical inside the ULA file) agrees with ula_addr()'s own offset math.
 */
#include <assert.h>
#include <stdio.h>
#include "ula.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_ula_addresses(void)
{
    /* One display file: col is a plain byte column 0..31, no parity split. */
    CHECK(ula_addr(0, 0)  == 0x4000u);
    CHECK(ula_addr(1, 0)  == 0x4001u);
    CHECK(ula_addr(31, 0) == 0x401Fu);

    /* Vertical: standard ZX thirds interleave. */
    CHECK(ula_addr(0, 1)   == 0x4100u);  /* next scanline in same char row  */
    CHECK(ula_addr(0, 8)   == 0x4020u);  /* second char row, first scanline */
    CHECK(ula_addr(0, 64)  == 0x4800u);  /* start of the middle third       */
    CHECK(ula_addr(0, 191) == 0x57E0u);  /* very last scanline of the file  */
}

static void test_row_scanline_offset_matches_ula_addr(void)
{
    u8 row, scanline;

    for (row = 0; row < 24u; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 prow = (u8)(row * 8u + scanline);
            u16 want = (u16)(ula_addr(0, prow) - ULA_FILE);

            CHECK(ula_row_scanline_offset(row, scanline) == want);
        }
    }
}

int main(void)
{
    test_ula_addresses();
    test_row_scanline_offset_matches_ula_addr();
    printf("ula: %d checks passed\n", checks);
    return 0;
}
