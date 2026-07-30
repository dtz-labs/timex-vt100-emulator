/*
 * test_hires.c -- host unit tests for the Timex hi-res address math.
 *
 * The formula is the load-bearing piece M1 relies on (we own it because the
 * z88dk tshr_* helpers don't link under +zx). Verified against z88dk's
 * asm_tshr_cxy2saddr behaviour and the WoS Timex reference.
 */
#include <assert.h>
#include <stdio.h>
#include "hires.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_hires_addresses(void)
{
    /* Column parity selects the file; within a file the byte column is col>>1. */
    CHECK(hires_addr(0, 0)  == 0x4000u);   /* even col 0  -> file0, byte 0     */
    CHECK(hires_addr(1, 0)  == 0x6000u);   /* odd  col 1  -> file1, byte 0     */
    CHECK(hires_addr(2, 0)  == 0x4001u);   /* even col 2  -> file0, byte 1     */
    CHECK(hires_addr(3, 0)  == 0x6001u);   /* odd  col 3  -> file1, byte 1     */
    CHECK(hires_addr(63, 0) == 0x601Fu);   /* col 63 odd  -> file1, byte 31    */

    /* Vertical: standard ZX thirds interleave within a file. */
    CHECK(hires_addr(0, 1)   == 0x4100u);  /* next scanline in same char row   */
    CHECK(hires_addr(0, 8)   == 0x4020u);  /* second char row, first scanline  */
    CHECK(hires_addr(0, 64)  == 0x4800u);  /* start of the middle third        */
    CHECK(hires_addr(0, 191) == 0x57E0u);  /* very last scanline of file0      */
}

/*
 * hires_row_scanline_offset(row, scanline) must agree with hires_addr()'s
 * own offset math: hires_addr(0, row*8+scanline) - HIRES_FILE0 is exactly
 * what row_scanline_offset is meant to hand blit_hires.c so it can add either
 * HIRES_FILE0 or HIRES_FILE1 itself (Task 9 moved this helper out of
 * blit_hires.c and into this pure, host-tested module).
 */
static void test_row_scanline_offset_matches_hires_addr(void)
{
    u8 row, scanline;

    for (row = 0; row < 24u; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 prow = (u8)(row * 8u + scanline);
            u16 want = (u16)(hires_addr(0, prow) - HIRES_FILE0);

            CHECK(hires_row_scanline_offset(row, scanline) == want);
        }
    }
}

int main(void)
{
    test_hires_addresses();
    test_row_scanline_offset_matches_hires_addr();
    printf("hires: %d checks passed\n", checks);
    return 0;
}
