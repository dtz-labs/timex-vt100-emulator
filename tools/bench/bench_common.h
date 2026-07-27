/*
 * bench_common.h -- T-state measurement markers for the z88dk-ticks harness.
 *
 * z88dk-ticks starts (resets) its cycle counter the instant PC reaches a
 * -start address and stops it the instant PC reaches a -end address. Both
 * addresses are ordinary global-function entry points resolved from the
 * build's .map file, so bench_mark_a/bench_mark_b -- defined in a SEPARATE
 * translation unit from every bench_*.c harness -- give a real CALL/RET
 * boundary that the compiler cannot inline away across files (z88dk does not
 * do cross-file inlining), which is what makes their addresses a reliable
 * splice point.
 *
 * Bracket the code under test:
 *
 *     bench_mark_a();
 *     thing_under_test();
 *     bench_mark_b();
 *
 * and measure with `-start _bench_mark_a -end _bench_mark_b`. This adds a
 * small, fixed, disclosed overhead (documented in tools/bench.sh and
 * docs/perf/benchmarks.md): bench_mark_a's own body plus the CALL that enters
 * bench_mark_b, on the order of 40 T-states -- negligible against the
 * hundred-thousand-T-state paths this harness measures.
 */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

void bench_mark_a(void);
void bench_mark_b(void);

#endif /* BENCH_COMMON_H */
