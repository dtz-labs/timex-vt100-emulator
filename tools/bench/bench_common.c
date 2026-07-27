/*
 * bench_common.c -- see bench_common.h.
 *
 * The volatile write forces both functions to keep a real body (rather than
 * being folded to nothing), and living in their own translation unit keeps
 * them from being inlined into a bench_*.c caller.
 */
#include "bench_common.h"
#include "types.h"

volatile u8 bench_marker_sink;

void bench_mark_a(void)
{
    bench_marker_sink = 1u;
}

void bench_mark_b(void)
{
    bench_marker_sink = 2u;
}
