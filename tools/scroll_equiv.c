/*
 * Frozen equivalence proof for the scroll run-coalescing change (2026-08-02).
 *
 * NOT a live regression test and deliberately not wired into test/run.sh:
 * it carries its own copies of BOTH the previous per-row scroll and the
 * coalesced one, so it can only ever prove what it proved on the day it was
 * written. Its value is reproducibility -- src/blit_hires.c and
 * src/blit_ula.c write to absolute video RAM and cannot be linked into a
 * host test, so this is the only way to re-run the argument off-target.
 *
 * The durable regression coverage for this path is `make smoke` /
 * `make smoke-zx`, scenario `scroll`, which checks glyph content of every
 * row after a real scroll on real Z80 memory.
 *
 *     cc -std=c99 -Wall -Wextra -Werror -O2 -o /tmp/scroll_equiv \
 *        tools/scroll_equiv.c && /tmp/scroll_equiv
 *
 * The THIRDS_OFFSET macro below is copied from include/ula.h; it is
 * byte-identical to include/hires.h's, so one run covers both geometries
 * (the hi-res build calls the same routine twice with a different base).
 * See docs/perf/benchmarks.md, "Scroll run coalescing".
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;

#define THIRDS_OFFSET(prow) \
    ( (u16)( ((u16)((u8)(prow) & 0xC0u) << 5) \
           | ((u16)((u8)(prow) & 0x07u) << 8) \
           | ((u16)((u8)(prow) & 0x38u) << 2) ) )

#define FILE_BYTES 6144u
#define ROWS 24u

/* ---------- the algorithm as it stood on master ---------- */

static void naive_up(u8 *f, u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = top; row < bot; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = f + THIRDS_OFFSET((u8)((row << 3) + scanline));
            const u8 *src = f + THIRDS_OFFSET((u8)(((row + 1u) << 3) + scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        memset(f + THIRDS_OFFSET((u8)((bot << 3) + scanline)), 0, 32u);
    }
}

static void naive_down(u8 *f, u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = bot; row > top; --row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = f + THIRDS_OFFSET((u8)((row << 3) + scanline));
            const u8 *src = f + THIRDS_OFFSET((u8)(((row - 1u) << 3) + scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        memset(f + THIRDS_OFFSET((u8)((top << 3) + scanline)), 0, 32u);
    }
}

/* ---------- the algorithm as rewritten ---------- */

static void fast_up(u8 *f, u8 top, u8 bot)
{
    u8 scanline;

    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 row = top;

        while (row < bot) {
            u8 run = (u8)(7u - (row & 7u));
            u8 remaining = (u8)(bot - row);
            u8 *dst = f + THIRDS_OFFSET((u8)((row << 3) + scanline));
            const u8 *src = f + THIRDS_OFFSET((u8)(((row + 1u) << 3) + scanline));

            if (run == 0u) {
                memcpy(dst, src, 32u);
                ++row;
                continue;
            }
            if (run > remaining) {
                run = remaining;
            }
            memmove(dst, src, (u16)run * 32u);
            row = (u8)(row + run);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        memset(f + THIRDS_OFFSET((u8)((bot << 3) + scanline)), 0, 32u);
    }
}

static void fast_down(u8 *f, u8 top, u8 bot)
{
    u8 scanline;

    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 row = bot;

        while (row > top) {
            u8 run = (u8)(row & 7u);
            u8 remaining = (u8)(row - top);
            u8 *dst;
            const u8 *src;

            if (run == 0u) {
                dst = f + THIRDS_OFFSET((u8)((row << 3) + scanline));
                src = f + THIRDS_OFFSET((u8)(((row - 1u) << 3) + scanline));
                memcpy(dst, src, 32u);
                --row;
                continue;
            }
            if (run > remaining) {
                run = remaining;
            }
            dst = f + THIRDS_OFFSET((u8)(((row - run + 1u) << 3) + scanline));
            src = f + THIRDS_OFFSET((u8)(((row - run) << 3) + scanline));
            memmove(dst, src, (u16)run * 32u);
            row = (u8)(row - run);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        memset(f + THIRDS_OFFSET((u8)((top << 3) + scanline)), 0, 32u);
    }
}

/* ---------- harness ---------- */

/* Fill so that every one of the 6144 bytes is distinguishable, and so that a
 * row/scanline mix-up cannot alias: value depends on row, scanline and column. */
static void fill(u8 *f)
{
    u8 row, scanline;
    unsigned col;

    memset(f, 0xEE, FILE_BYTES);   /* poison: must never survive as data */
    for (row = 0; row < ROWS; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *p = f + THIRDS_OFFSET((u8)((row << 3) + scanline));
            for (col = 0; col < 32u; ++col) {
                p[col] = (u8)(row * 11u + scanline * 3u + col + 1u);
            }
        }
    }
}

int main(void)
{
    static u8 a[FILE_BYTES], b[FILE_BYTES];
    unsigned top, bot, checked = 0;

    for (top = 0; top < ROWS; ++top) {
        for (bot = top + 1u; bot < ROWS; ++bot) {
            fill(a); fill(b);
            naive_up(a, (u8)top, (u8)bot);
            fast_up(b, (u8)top, (u8)bot);
            if (memcmp(a, b, FILE_BYTES) != 0) {
                unsigned i;
                for (i = 0; i < FILE_BYTES; ++i) {
                    if (a[i] != b[i]) {
                        printf("UP  MISMATCH top=%u bot=%u at offset %u: naive=%u fast=%u\n",
                               top, bot, i, a[i], b[i]);
                        return 1;
                    }
                }
            }

            fill(a); fill(b);
            naive_down(a, (u8)top, (u8)bot);
            fast_down(b, (u8)top, (u8)bot);
            if (memcmp(a, b, FILE_BYTES) != 0) {
                unsigned i;
                for (i = 0; i < FILE_BYTES; ++i) {
                    if (a[i] != b[i]) {
                        printf("DOWN MISMATCH top=%u bot=%u at offset %u: naive=%u fast=%u\n",
                               top, bot, i, a[i], b[i]);
                        return 1;
                    }
                }
            }
            checked += 2u;
        }
    }

    /* Independent check that the naive version is itself doing what we think:
     * after one scroll up over the full region, text row r must hold what row
     * r+1 held, and the bottom row must be blank. */
    fill(a);
    naive_up(a, 0, (u8)(ROWS - 1u));
    {
        u8 row, scanline;
        for (row = 0; row < ROWS - 1u; ++row) {
            for (scanline = 0; scanline < 8u; ++scanline) {
                const u8 *p = a + THIRDS_OFFSET((u8)((row << 3) + scanline));
                unsigned col;
                for (col = 0; col < 32u; ++col) {
                    u8 want = (u8)((row + 1u) * 11u + scanline * 3u + col + 1u);
                    assert(p[col] == want);
                }
            }
        }
        for (scanline = 0; scanline < 8u; ++scanline) {
            const u8 *p = a + THIRDS_OFFSET((u8)(((ROWS - 1u) << 3) + scanline));
            unsigned col;
            for (col = 0; col < 32u; ++col) {
                assert(p[col] == 0);
            }
        }
    }

    printf("OK: %u region/direction combinations byte-identical; "
           "semantic check on the naive baseline passed\n", checked);
    return 0;
}
