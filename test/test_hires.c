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

int main(void)
{
    test_hires_addresses();
    printf("hires: %d checks passed\n", checks);
    return 0;
}
