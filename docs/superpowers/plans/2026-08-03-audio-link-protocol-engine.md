# Audio Link Protocol Engine (PR A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Half-Duplex Terminal Protocol v1 as a pure, host-testable engine — the C slave that will run on the Z80 and the Python master that will run on the PC — with no audio, no Z80 toolchain and no emulator involved.

**Architecture:** Two independent implementations of one wire protocol. The C side (`src/alink_frame.c`, `src/alink_slave.c`) is written to the project's existing pure-logic conventions so it compiles both natively for tests and later with z88dk for the target. The Python side (`tools/alink/`) mirrors the codec and adds the master's ARQ state machine. A cross-implementation harness pipes the Python master against the natively-compiled C slave, which is the only test that catches the two implementations reading the spec differently.

**Tech Stack:** C99 (`cc`, `-std=c99 -Wall -Wextra -Werror`), Python 3.9+ standard library only. No pytest — Python tests are standalone scripts that print `name: N checks passed`, matching `test/test_check_image_limit.py`.

## Global Constraints

Copied from `docs/superpowers/specs/2026-08-02-audio-link-design.md` and the protocol v1 spec it implements. Every task's requirements implicitly include this section.

- **Frame layout:** `CTRL(1) | LEN(1) | PAYLOAD(0..64) | CRC-16(2)`. CRC transmitted **high byte first**.
- **CTRL bits:** `[TYPE:7-5][reserved:4-2][SEQ:1][ACK:0]`. Reserved bits sent as 0, ignored on receive.
- **TYPE values:** `0=HELLO, 1=WELCOME, 2=LINK, 3=BYE`. `4-7` reserved — receiver ignores the frame.
- **CRC-16/CCITT-FALSE:** poly `0x1021`, init `0xFFFF`, no reflection, xorout `0x0000`, computed over CTRL, LEN and PAYLOAD.
- **Validation** — a block is a valid frame only if ALL hold; anything else is treated as if the frame never arrived:
  1. physical block length equals `LEN + 4`
  2. CRC matches
  3. `LEN <= 64` absolute cap; for LINK frames also `LEN <= receiver's advertised max payload`
  4. for HELLO and WELCOME, `LEN >= 3`; bytes beyond offset 2 are reserved and ignored
- **HELLO/WELCOME payload (3 bytes):** offset 0 = version (v1 value `1`), offset 1 = capability flags (`0x00`), offset 2 = max payload (`64`).
- **Sequence accounting applies to LINK frames only.** HELLO, WELCOME and BYE never touch sequence state and their payloads must never enter the byte streams.
- **Session reset values:** first payload in each direction uses `SEQ=0`, and "last accepted" starts at `1` so that `SEQ=0` counts as new.
- **Receivers ignore the SEQ bit of `LEN=0` frames.**
- **BYE is master-only in v1.** No reply required.
- **C code must stay SDCC/z88dk-compatible:** C99, four-space indent, K&R braces, fixed-width aliases from `include/types.h` (`u8`, `u16`), pointer-oriented APIs, **never return a struct by value**. No float, no `malloc`, no recursion.
- **`src/alink_frame.c` and `src/alink_slave.c` are pure logic** — no hardware access, no absolute addresses. They must compile and run natively. (`src/alink_phy.c` is PR B and is target-only.)
- Neither file may include `screen.h`, so its `TERM_TIMEX`/`TERM_ZX` `#error` guard never triggers and both compile once with no machine define.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/alink.h` | Frame layout, protocol constants, both public APIs |
| `src/alink_frame.c` | CRC-16 and frame encode/decode with all four validation rules |
| `src/alink_slave.c` | Slave session state machine: LISTEN/LINKED, SEQ/ACK, duplicate suppression |
| `test/test_alink_frame.c` | Codec and CRC tests |
| `test/test_alink_slave.c` | State-machine tests |
| `tools/alink/__init__.py` | Package marker |
| `tools/alink/frame.py` | Python mirror of the codec — must agree with C bit for bit |
| `tools/alink/master.py` | Master ARQ state machine, transport-agnostic |
| `tools/alink/xcheck_slave.c` | Native harness: length-prefixed blocks on stdin/stdout, driving `alink_slave` |
| `test/test_alink_frame_py.py` | Python codec tests, including shared vectors with C |
| `test/test_alink_master.py` | Master over a simulated lossy channel |
| `test/test_alink_xcheck.py` | Python master against the compiled C slave, over a pipe |

The split follows the spec: the codec is separable from the state machine, and each has its own test cycle. `alink_slave.c` consumes `alink_frame.c` and nothing else.

---

### Task 1: Frame codec and CRC-16 (C)

**Files:**
- Create: `include/alink.h`
- Create: `src/alink_frame.c`
- Create: `test/test_alink_frame.c`
- Modify: `test/run.sh` (add the new suite to the width-independent section)

**Interfaces:**
- Consumes: `include/types.h` (`u8`, `u16`)
- Produces:
  - `u16 alink_crc16(const u8 *data, u8 n)`
  - `u8 alink_frame_encode(const alink_frame_t *f, u8 *out)` — returns bytes written (4..68), or 0 if `f->len > 64`
  - `u8 alink_frame_decode(const u8 *block, u8 block_len, u8 max_payload, alink_frame_t *out)` — returns 1 on accept, 0 on reject
  - `typedef struct { u8 type; u8 seq; u8 ack; u8 len; u8 payload[64]; } alink_frame_t;`
  - Constants `ALINK_MAX_PAYLOAD`, `ALINK_FRAME_MAX`, `ALINK_HELLO_PAYLOAD`, `ALINK_TYPE_*`, `ALINK_CTRL_SEQ`, `ALINK_CTRL_ACK`, `ALINK_CTRL_TYPE_SHIFT`, `ALINK_VERSION`, `ALINK_CAPS`

- [ ] **Step 1: Create the header**

Create `include/alink.h`:

```c
/*
 * alink.h -- Half-Duplex Terminal Protocol v1.
 *
 * Wire format:
 *
 *     +------+------+-----------+--------+
 *     | CTRL | LEN  | PAYLOAD   | CRC-16 |
 *     | 1 B  | 1 B  | 0..64 B   | 2 B    |
 *     +------+------+-----------+--------+
 *
 * CTRL bits:  [TYPE:7-5] [reserved:4-2] [SEQ:1] [ACK:0]
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0x0000)
 * over CTRL, LEN and PAYLOAD, transmitted high byte first.
 *
 * This header and its two implementation files are pure logic: they are
 * compiled natively by test/run.sh and with z88dk for the target from the
 * same source. The physical layer (src/alink_phy.c) is target-only.
 */
#ifndef ALINK_H
#define ALINK_H

#include "types.h"

#define ALINK_MAX_PAYLOAD    64u
#define ALINK_FRAME_MAX      (4u + ALINK_MAX_PAYLOAD)   /* 68 */
#define ALINK_HELLO_PAYLOAD  3u

#define ALINK_TYPE_HELLO     0u
#define ALINK_TYPE_WELCOME   1u
#define ALINK_TYPE_LINK      2u
#define ALINK_TYPE_BYE       3u

#define ALINK_CTRL_ACK        0x01u
#define ALINK_CTRL_SEQ        0x02u
#define ALINK_CTRL_TYPE_SHIFT 5u

#define ALINK_VERSION 1u
#define ALINK_CAPS    0x00u

typedef struct {
    u8 type;
    u8 seq;                          /* 0 or 1 */
    u8 ack;                          /* 0 or 1 */
    u8 len;
    u8 payload[ALINK_MAX_PAYLOAD];
} alink_frame_t;

u16 alink_crc16(const u8 *data, u8 n);

/* Serialise `f` into `out` (must hold ALINK_FRAME_MAX bytes).
 * Returns the number of bytes written, or 0 if f->len exceeds the cap. */
u8 alink_frame_encode(const alink_frame_t *f, u8 *out);

/* Validate and parse one received block. `max_payload` is what THIS receiver
 * advertised, used for validation rule 3's LINK-specific cap.
 * Returns 1 and fills `out` on accept; returns 0 on any rejection, in which
 * case `out` is untouched and the caller must behave as if nothing arrived. */
u8 alink_frame_decode(const u8 *block, u8 block_len, u8 max_payload,
                      alink_frame_t *out);

#endif /* ALINK_H */
```

- [ ] **Step 2: Write the failing test**

Create `test/test_alink_frame.c`:

```c
/*
 * test_alink_frame.c -- host tests for the protocol v1 frame codec.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "alink.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* The published CRC-16/CCITT-FALSE check value: "123456789" -> 0x29B1. */
static void test_crc_published_vector(void)
{
    const u8 v[] = { '1','2','3','4','5','6','7','8','9' };
    CHECK(alink_crc16(v, 9) == 0x29B1u);
    CHECK(alink_crc16(v, 0) == 0xFFFFu);   /* init value, nothing consumed */
}

static void test_encode_layout(void)
{
    alink_frame_t f;
    u8 out[ALINK_FRAME_MAX];
    u16 crc;
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.seq = 1;
    f.ack = 1;
    f.len = 2;
    f.payload[0] = 'h';
    f.payload[1] = 'i';

    n = alink_frame_encode(&f, out);
    CHECK(n == 6);
    CHECK(out[0] == (u8)((ALINK_TYPE_LINK << ALINK_CTRL_TYPE_SHIFT)
                         | ALINK_CTRL_SEQ | ALINK_CTRL_ACK));
    CHECK(out[1] == 2);
    CHECK(out[2] == 'h');
    CHECK(out[3] == 'i');
    crc = alink_crc16(out, 4);
    CHECK(out[4] == (u8)(crc >> 8));      /* high byte first */
    CHECK(out[5] == (u8)(crc & 0xFFu));
}

static void test_encode_rejects_oversize(void)
{
    alink_frame_t f;
    u8 out[ALINK_FRAME_MAX];

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = ALINK_MAX_PAYLOAD + 1u;
    CHECK(alink_frame_encode(&f, out) == 0);
}

static void test_round_trip_all_lengths(void)
{
    alink_frame_t in, out;
    u8 block[ALINK_FRAME_MAX];
    u8 len, i, n;

    for (len = 0; len <= ALINK_MAX_PAYLOAD; ++len) {
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.type = ALINK_TYPE_LINK;
        in.seq = (u8)(len & 1u);
        in.ack = (u8)((len >> 1) & 1u);
        in.len = len;
        for (i = 0; i < len; ++i) {
            in.payload[i] = (u8)(i * 7u + 1u);
        }
        n = alink_frame_encode(&in, block);
        CHECK(n == (u8)(len + 4u));
        CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
        CHECK(out.type == in.type);
        CHECK(out.seq == in.seq);
        CHECK(out.ack == in.ack);
        CHECK(out.len == in.len);
        CHECK(memcmp(out.payload, in.payload, len) == 0);
    }
}

/* Validation rule 1: physical block length must equal LEN + 4. */
static void test_reject_length_mismatch(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = 4;
    n = alink_frame_encode(&f, block);
    CHECK(n == 8);
    CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(alink_frame_decode(block, (u8)(n - 1u), ALINK_MAX_PAYLOAD, &out) == 0);
    CHECK(alink_frame_decode(block, (u8)(n + 1u), ALINK_MAX_PAYLOAD, &out) == 0);
    CHECK(alink_frame_decode(block, 3, ALINK_MAX_PAYLOAD, &out) == 0);
}

/* Validation rule 2: any single flipped bit must be rejected. */
static void test_reject_bit_flips(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 good[ALINK_FRAME_MAX];
    u8 n, i, b;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = 3;
    f.payload[0] = 'a';
    f.payload[1] = 'b';
    f.payload[2] = 'c';
    n = alink_frame_encode(&f, good);

    for (i = 0; i < n; ++i) {
        for (b = 0; b < 8u; ++b) {
            memcpy(block, good, n);
            block[i] ^= (u8)(1u << b);
            /* Flipping a TYPE bit can produce a reserved type, which is also
             * a rejection -- either way decode must return 0. */
            CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 0);
        }
    }
}

/* Validation rule 3: absolute cap, then the LINK-only advertised cap. */
static void test_reject_oversize_len(void)
{
    alink_frame_t out;
    u8 block[80];

    memset(block, 0, sizeof block);
    block[0] = (u8)(ALINK_TYPE_LINK << ALINK_CTRL_TYPE_SHIFT);
    block[1] = 65;                       /* over the absolute cap */
    CHECK(alink_frame_decode(block, 69, ALINK_MAX_PAYLOAD, &out) == 0);
}

static void test_reject_over_advertised_payload(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = 40;
    n = alink_frame_encode(&f, block);
    CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(alink_frame_decode(block, n, 32, &out) == 0);   /* over advertised */
    CHECK(alink_frame_decode(block, n, 40, &out) == 1);   /* exactly at it */
}

/* Rule 3's LINK-only wording: a HELLO larger than the advertised payload is
 * still accepted, because the cap applies to LINK frames. */
static void test_advertised_cap_is_link_only(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_HELLO;
    f.len = 40;
    f.payload[0] = ALINK_VERSION;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    n = alink_frame_encode(&f, block);
    CHECK(alink_frame_decode(block, n, 8, &out) == 1);
}

/* Validation rule 4: HELLO/WELCOME need LEN >= 3; extra bytes are ignored. */
static void test_hello_minimum_length(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n, len;

    for (len = 0; len < ALINK_HELLO_PAYLOAD; ++len) {
        memset(&f, 0, sizeof f);
        f.type = ALINK_TYPE_HELLO;
        f.len = len;
        n = alink_frame_encode(&f, block);
        CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 0);
        f.type = ALINK_TYPE_WELCOME;
        n = alink_frame_encode(&f, block);
        CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 0);
    }

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_HELLO;
    f.len = 5;                            /* longer than 3: extras ignored */
    f.payload[0] = ALINK_VERSION;
    f.payload[1] = ALINK_CAPS;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    f.payload[3] = 0xDE;
    f.payload[4] = 0xAD;
    n = alink_frame_encode(&f, block);
    CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(out.len == 5);
    CHECK(out.payload[0] == ALINK_VERSION);
    CHECK(out.payload[2] == ALINK_MAX_PAYLOAD);
}

/* TYPE 4-7 are reserved: the receiver ignores the frame. */
static void test_reject_reserved_types(void)
{
    alink_frame_t out;
    u8 block[4];
    u16 crc;
    u8 t;

    for (t = 4; t < 8u; ++t) {
        block[0] = (u8)(t << ALINK_CTRL_TYPE_SHIFT);
        block[1] = 0;
        crc = alink_crc16(block, 2);
        block[2] = (u8)(crc >> 8);
        block[3] = (u8)(crc & 0xFFu);
        CHECK(alink_frame_decode(block, 4, ALINK_MAX_PAYLOAD, &out) == 0);
    }
}

/* Reserved CTRL bits 4-2 are sent as 0 and ignored on receive: a frame with
 * them set must still decode, and to the same fields. */
static void test_reserved_ctrl_bits_ignored(void)
{
    alink_frame_t out;
    u8 block[4];
    u16 crc;

    block[0] = (u8)((ALINK_TYPE_LINK << ALINK_CTRL_TYPE_SHIFT)
                    | 0x1Cu | ALINK_CTRL_SEQ);
    block[1] = 0;
    crc = alink_crc16(block, 2);
    block[2] = (u8)(crc >> 8);
    block[3] = (u8)(crc & 0xFFu);
    CHECK(alink_frame_decode(block, 4, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(out.type == ALINK_TYPE_LINK);
    CHECK(out.seq == 1);
    CHECK(out.ack == 0);
    CHECK(out.len == 0);
}

int main(void)
{
    test_crc_published_vector();
    test_encode_layout();
    test_encode_rejects_oversize();
    test_round_trip_all_lengths();
    test_reject_length_mismatch();
    test_reject_bit_flips();
    test_reject_oversize_len();
    test_reject_over_advertised_payload();
    test_advertised_cap_is_link_only();
    test_hello_minimum_length();
    test_reject_reserved_types();
    test_reserved_ctrl_bits_ignored();
    printf("alink_frame: %d checks passed\n", checks);
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cc -std=c99 -Wall -Wextra -Werror -Iinclude test/test_alink_frame.c src/alink_frame.c -o /tmp/t
```
Expected: FAIL — `src/alink_frame.c` does not exist yet ("no such file or directory").

- [ ] **Step 4: Write the implementation**

Create `src/alink_frame.c`:

```c
/*
 * alink_frame.c -- protocol v1 frame codec.
 *
 * Pure logic: no hardware, no absolute addresses, no machine define. Compiled
 * natively by test/run.sh and with z88dk for the target from this same source.
 *
 * The CRC is computed bitwise rather than from a table: 256 entries would cost
 * 512 bytes of a Timex image that has under 1,800 to spare (see the memory
 * budget in docs/superpowers/specs/2026-08-02-audio-link-design.md), and at
 * ~68 bytes per frame against a link that spends 0.4 s per frame on the wire,
 * the table would buy nothing measurable.
 */
#include "alink.h"

u16 alink_crc16(const u8 *data, u8 n)
{
    u16 crc = 0xFFFFu;
    u8 i, b;

    for (i = 0; i < n; ++i) {
        crc ^= (u16)((u16)data[i] << 8);
        for (b = 0; b < 8u; ++b) {
            if (crc & 0x8000u) {
                crc = (u16)((u16)(crc << 1) ^ 0x1021u);
            } else {
                crc = (u16)(crc << 1);
            }
        }
    }
    return crc;
}

u8 alink_frame_encode(const alink_frame_t *f, u8 *out)
{
    u16 crc;
    u8 i;

    if (f->len > ALINK_MAX_PAYLOAD) {
        return 0;
    }

    out[0] = (u8)(((u8)(f->type & 0x07u) << ALINK_CTRL_TYPE_SHIFT)
                  | (u8)(f->seq ? ALINK_CTRL_SEQ : 0u)
                  | (u8)(f->ack ? ALINK_CTRL_ACK : 0u));
    out[1] = f->len;
    for (i = 0; i < f->len; ++i) {
        out[2u + i] = f->payload[i];
    }

    crc = alink_crc16(out, (u8)(2u + f->len));
    out[2u + f->len] = (u8)(crc >> 8);
    out[3u + f->len] = (u8)(crc & 0xFFu);
    return (u8)(4u + f->len);
}

u8 alink_frame_decode(const u8 *block, u8 block_len, u8 max_payload,
                      alink_frame_t *out)
{
    u16 crc, want;
    u8 len, type, i;

    if (block_len < 4u) {
        return 0;
    }

    len = block[1];
    if (len > ALINK_MAX_PAYLOAD) {          /* rule 3, absolute cap */
        return 0;
    }
    if (block_len != (u8)(len + 4u)) {      /* rule 1 */
        return 0;
    }

    crc = alink_crc16(block, (u8)(2u + len));                       /* rule 2 */
    want = (u16)(((u16)block[2u + len] << 8) | block[3u + len]);
    if (crc != want) {
        return 0;
    }

    type = (u8)((block[0] >> ALINK_CTRL_TYPE_SHIFT) & 0x07u);
    if (type > ALINK_TYPE_BYE) {            /* 4-7 reserved: ignore the frame */
        return 0;
    }
    if (type == ALINK_TYPE_LINK && len > max_payload) {   /* rule 3, LINK only */
        return 0;
    }
    if ((type == ALINK_TYPE_HELLO || type == ALINK_TYPE_WELCOME)
            && len < ALINK_HELLO_PAYLOAD) {                          /* rule 4 */
        return 0;
    }

    out->type = type;
    out->seq = (u8)((block[0] & ALINK_CTRL_SEQ) ? 1u : 0u);
    out->ack = (u8)((block[0] & ALINK_CTRL_ACK) ? 1u : 0u);
    out->len = len;
    for (i = 0; i < len; ++i) {
        out->payload[i] = block[2u + i];
    }
    return 1;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cc -std=c99 -Wall -Wextra -Werror -Iinclude test/test_alink_frame.c src/alink_frame.c -o /tmp/t && /tmp/t
```
Expected: `alink_frame: <N> checks passed`, exit 0, no warnings.

- [ ] **Step 6: Wire the suite into the host test runner**

In `test/run.sh`, in the width-independent section (the one introduced by the comment "Width-independent suites: compiled once, with no machine define at all"), add immediately after the `test_font` block:

```sh
$CC $CFLAGS "$ROOT/test/test_alink_frame.c" "$ROOT/src/alink_frame.c" -o "$OUT/test_alink_frame"
"$OUT/test_alink_frame"
```

- [ ] **Step 7: Run the full host suite**

Run: `make host-test`
Expected: `alink_frame: <N> checks passed` appears, followed by `ALL HOST TESTS PASSED`.

- [ ] **Step 8: Commit**

```bash
git add include/alink.h src/alink_frame.c test/test_alink_frame.c test/run.sh
git commit -m "alink: protocol v1 frame codec with CRC-16 (host TDD)"
```

---

### Task 2: Slave session state machine (C)

**Files:**
- Modify: `include/alink.h` (append the slave API before `#endif`)
- Create: `src/alink_slave.c`
- Create: `test/test_alink_slave.c`
- Modify: `test/run.sh`

**Interfaces:**
- Consumes: everything Task 1 produced — `alink_frame_t`, `alink_frame_decode()`, `ALINK_TYPE_*`, `ALINK_VERSION`, `ALINK_CAPS`, `ALINK_MAX_PAYLOAD`, `ALINK_HELLO_PAYLOAD`
- Produces:
  - `typedef struct { ... } alink_slave_t;`
  - `typedef struct { u8 respond; u8 deliver_len; u8 session_reset; u8 carrier_lost; } alink_rx_t;`
  - `void alink_slave_init(alink_slave_t *s)`
  - `void alink_slave_feed(alink_slave_t *s, const u8 *block, u8 block_len, alink_rx_t *out)`
  - `const u8 *alink_slave_rx_payload(const alink_slave_t *s)`
  - `u8 alink_slave_tx_ready(const alink_slave_t *s)`
  - `void alink_slave_tx_set(alink_slave_t *s, const u8 *buf, u8 n)`
  - `void alink_slave_response(const alink_slave_t *s, alink_frame_t *resp)`
  - `ALINK_LISTEN`, `ALINK_LINKED`

**Why `feed()` and `response()` are separate calls.** Protocol v1 requires the
slave to consume a payload *before* replying — that is the flow-control
mechanism. Splitting them lets the caller render the delivered payload and
then top up the upstream slot in the same transaction, so a payload the master
just acknowledged can be refilled without wasting a round trip. The integration
sequence is: `feed()` → render `alink_slave_rx_payload()` → if
`alink_slave_tx_ready()` then `alink_slave_tx_set()` → `alink_slave_response()`.

- [ ] **Step 1: Append the slave API to the header**

In `include/alink.h`, insert before the closing `#endif /* ALINK_H */`:

```c
/* ---- slave session state machine ---- */

#define ALINK_LISTEN 0u
#define ALINK_LINKED 1u

typedef struct {
    u8 state;                        /* ALINK_LISTEN or ALINK_LINKED */
    u8 last_rx_seq;                  /* SEQ of the last accepted downstream
                                      * payload; reset to 1 so that the
                                      * session's first SEQ=0 counts as new */
    u8 tx_seq;                       /* SEQ carried by our upstream payload */
    u8 tx_pending;                   /* 1 while tx_payload is unacknowledged */
    u8 tx_len;
    u8 tx_payload[ALINK_MAX_PAYLOAD];
    u8 max_payload;                  /* what we advertise; v1 = 64 */
    u8 resp_type;                    /* type of the pending response */
    alink_frame_t rx;                /* last successfully decoded frame */
} alink_slave_t;

typedef struct {
    u8 respond;                      /* 1 -> call alink_slave_response() */
    u8 deliver_len;                  /* >0 -> that many bytes at
                                      * alink_slave_rx_payload() go to the
                                      * VT parser */
    u8 session_reset;                /* 1 -> a HELLO reset the session */
    u8 carrier_lost;                 /* 1 -> BYE: show NO CARRIER, LISTEN */
} alink_rx_t;

void alink_slave_init(alink_slave_t *s);

/* Feed one received block. Never partially applies: a block that fails
 * validation leaves every field of *s untouched and clears *out. */
void alink_slave_feed(alink_slave_t *s, const u8 *block, u8 block_len,
                      alink_rx_t *out);

const u8 *alink_slave_rx_payload(const alink_slave_t *s);

/* 1 when LINKED with no unacknowledged upstream payload, i.e. when
 * alink_slave_tx_set() would be accepted rather than silently dropped. */
u8 alink_slave_tx_ready(const alink_slave_t *s);

/* Load the next upstream payload. Bytes beyond ALINK_MAX_PAYLOAD are dropped.
 * Has no effect unless alink_slave_tx_ready() is 1 -- protocol v1 slave rule 3
 * requires the outstanding payload to repeat unchanged until acknowledged. */
void alink_slave_tx_set(alink_slave_t *s, const u8 *buf, u8 n);

/* Build the response for the frame most recently fed. Call only when
 * alink_rx_t.respond was 1. */
void alink_slave_response(const alink_slave_t *s, alink_frame_t *resp);
```

- [ ] **Step 2: Write the failing test**

Create `test/test_alink_slave.c`:

```c
/*
 * test_alink_slave.c -- host tests for the protocol v1 slave state machine.
 *
 * Every test drives the slave the way the wire does: by encoding a master
 * frame, handing the bytes to alink_slave_feed(), and inspecting what comes
 * back. Nothing reaches into the struct to set up a state the wire could not
 * produce.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "alink.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* Encode a master frame and feed it to the slave. */
static void feed(alink_slave_t *s, u8 type, u8 seq, u8 ack,
                 const char *payload, alink_rx_t *out)
{
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = type;
    f.seq = seq;
    f.ack = ack;
    f.len = (u8)(payload ? (u8)strlen(payload) : 0u);
    if (payload) {
        memcpy(f.payload, payload, f.len);
    }
    n = alink_frame_encode(&f, block);
    alink_slave_feed(s, block, n, out);
}

static void feed_hello(alink_slave_t *s, alink_rx_t *out)
{
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_HELLO;
    f.len = ALINK_HELLO_PAYLOAD;
    f.payload[0] = ALINK_VERSION;
    f.payload[1] = ALINK_CAPS;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    n = alink_frame_encode(&f, block);
    alink_slave_feed(s, block, n, out);
}

static void test_starts_in_listen_and_ignores_link(void)
{
    alink_slave_t s;
    alink_rx_t r;

    alink_slave_init(&s);
    CHECK(s.state == ALINK_LISTEN);
    CHECK(alink_slave_tx_ready(&s) == 0);      /* not LINKED */

    feed(&s, ALINK_TYPE_LINK, 0, 0, "hello", &r);
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
    CHECK(s.state == ALINK_LISTEN);
}

static void test_hello_gets_welcome_and_links(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    CHECK(r.respond == 1);
    CHECK(r.session_reset == 1);
    CHECK(r.deliver_len == 0);                 /* negotiation never delivers */
    CHECK(s.state == ALINK_LINKED);

    alink_slave_response(&s, &resp);
    CHECK(resp.type == ALINK_TYPE_WELCOME);
    CHECK(resp.seq == 0);
    CHECK(resp.ack == 0);
    CHECK(resp.len == ALINK_HELLO_PAYLOAD);
    CHECK(resp.payload[0] == ALINK_VERSION);
    CHECK(resp.payload[1] == ALINK_CAPS);
    CHECK(resp.payload[2] == ALINK_MAX_PAYLOAD);
}

static void test_first_link_payload_is_delivered(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.respond == 1);
    CHECK(r.deliver_len == 3);
    CHECK(memcmp(alink_slave_rx_payload(&s), "abc", 3) == 0);

    alink_slave_response(&s, &resp);
    CHECK(resp.type == ALINK_TYPE_LINK);
    CHECK(resp.ack == 0);                      /* echoes last accepted SEQ */
    CHECK(resp.len == 0);                      /* nothing queued upstream */
}

static void test_duplicate_is_answered_but_not_redelivered(void)
{
    alink_slave_t s;
    alink_rx_t r;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.deliver_len == 3);

    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.respond == 1);                     /* still answered */
    CHECK(r.deliver_len == 0);                 /* but not delivered twice */

    feed(&s, ALINK_TYPE_LINK, 1, 0, "de", &r); /* new SEQ -> new payload */
    CHECK(r.deliver_len == 2);
}

/* Receivers ignore the SEQ bit of LEN=0 frames: an empty poll must never
 * advance the accepted-SEQ state, or the next real payload would be dropped. */
static void test_empty_poll_does_not_touch_seq_state(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    feed(&s, ALINK_TYPE_LINK, 1, 0, NULL, &r);   /* empty poll, SEQ=1 */
    CHECK(r.respond == 1);
    CHECK(r.deliver_len == 0);
    alink_slave_response(&s, &resp);
    CHECK(resp.ack == 1);        /* still the reset value, nothing accepted */

    feed(&s, ALINK_TYPE_LINK, 0, 0, "x", &r);    /* the session's first payload */
    CHECK(r.deliver_len == 1);
}

static void test_upstream_repeats_until_acked(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    CHECK(alink_slave_tx_ready(&s) == 1);
    alink_slave_tx_set(&s, (const u8 *)"KEY", 3);
    CHECK(alink_slave_tx_ready(&s) == 0);

    feed(&s, ALINK_TYPE_LINK, 0, 0, NULL, &r);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 3);
    CHECK(resp.seq == 0);
    CHECK(memcmp(resp.payload, "KEY", 3) == 0);

    /* Master did not acknowledge (ack still 1, our SEQ is 0): repeat it. */
    feed(&s, ALINK_TYPE_LINK, 0, 1, NULL, &r);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 3);
    CHECK(resp.seq == 0);
    CHECK(memcmp(resp.payload, "KEY", 3) == 0);
    CHECK(alink_slave_tx_ready(&s) == 0);

    /* Now acknowledged: slot frees and SEQ advances. */
    feed(&s, ALINK_TYPE_LINK, 0, 0, NULL, &r);
    CHECK(alink_slave_tx_ready(&s) == 1);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 0);
    CHECK(resp.seq == 1);
}

/* Rule 3: the outstanding payload must repeat unchanged. A tx_set() while a
 * payload is still in flight must not corrupt it. */
static void test_tx_set_ignored_while_pending(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    alink_slave_tx_set(&s, (const u8 *)"AAA", 3);
    alink_slave_tx_set(&s, (const u8 *)"BBBBB", 5);   /* must be ignored */

    feed(&s, ALINK_TYPE_LINK, 0, 0, NULL, &r);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 3);
    CHECK(memcmp(resp.payload, "AAA", 3) == 0);
}

static void test_tx_set_clamps_to_max_payload(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;
    u8 big[ALINK_MAX_PAYLOAD + 8u];
    u8 i;

    for (i = 0; i < sizeof big; ++i) {
        big[i] = (u8)(i + 1u);
    }
    alink_slave_init(&s);
    feed_hello(&s, &r);
    alink_slave_tx_set(&s, big, (u8)ALINK_MAX_PAYLOAD);

    feed(&s, ALINK_TYPE_LINK, 0, 0, NULL, &r);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == ALINK_MAX_PAYLOAD);
    CHECK(memcmp(resp.payload, big, ALINK_MAX_PAYLOAD) == 0);
}

/* Rule 4: a valid HELLO in any state resets the session and drops pending
 * state in both directions. */
static void test_hello_resets_mid_session(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    alink_slave_tx_set(&s, (const u8 *)"KEY", 3);
    CHECK(alink_slave_tx_ready(&s) == 0);

    feed_hello(&s, &r);
    CHECK(r.session_reset == 1);
    CHECK(r.respond == 1);
    CHECK(s.state == ALINK_LINKED);
    CHECK(alink_slave_tx_ready(&s) == 1);        /* in-flight payload dropped */
    alink_slave_response(&s, &resp);
    CHECK(resp.type == ALINK_TYPE_WELCOME);

    /* Sequence state was reset, so SEQ=0 counts as new again. */
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.deliver_len == 3);
}

static void test_bye_drops_to_listen_without_replying(void)
{
    alink_slave_t s;
    alink_rx_t r;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_BYE, 0, 0, NULL, &r);
    CHECK(r.carrier_lost == 1);
    CHECK(r.respond == 0);                       /* no reply required */
    CHECK(s.state == ALINK_LISTEN);

    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);  /* LISTEN ignores LINK */
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
}

/* A slave never acts on a WELCOME -- that is a slave-to-master frame. */
static void test_welcome_from_master_is_ignored(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_WELCOME;
    f.len = ALINK_HELLO_PAYLOAD;
    f.payload[0] = ALINK_VERSION;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    n = alink_frame_encode(&f, block);
    alink_slave_feed(&s, block, n, &r);
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
    CHECK(s.state == ALINK_LINKED);
}

/* A corrupted block must leave the slave exactly as it was. */
static void test_invalid_block_changes_nothing(void)
{
    alink_slave_t s, before;
    alink_rx_t r;
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.seq = 1;
    f.len = 2;
    f.payload[0] = 'z';
    f.payload[1] = 'z';
    n = alink_frame_encode(&f, block);
    block[3] ^= 0x40u;                           /* corrupt the payload */

    memcpy(&before, &s, sizeof s);
    alink_slave_feed(&s, block, n, &r);
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
    CHECK(memcmp(&before, &s, sizeof s) == 0);
}

int main(void)
{
    test_starts_in_listen_and_ignores_link();
    test_hello_gets_welcome_and_links();
    test_first_link_payload_is_delivered();
    test_duplicate_is_answered_but_not_redelivered();
    test_empty_poll_does_not_touch_seq_state();
    test_upstream_repeats_until_acked();
    test_tx_set_ignored_while_pending();
    test_tx_set_clamps_to_max_payload();
    test_hello_resets_mid_session();
    test_bye_drops_to_listen_without_replying();
    test_welcome_from_master_is_ignored();
    test_invalid_block_changes_nothing();
    printf("alink_slave: %d checks passed\n", checks);
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cc -std=c99 -Wall -Wextra -Werror -Iinclude test/test_alink_slave.c src/alink_slave.c src/alink_frame.c -o /tmp/t
```
Expected: FAIL — `src/alink_slave.c` does not exist yet.

- [ ] **Step 4: Write the implementation**

Create `src/alink_slave.c`:

```c
/*
 * alink_slave.c -- protocol v1 slave session state machine.
 *
 * Pure logic, compiled both natively and for the target. The rule numbers in
 * the comments refer to the "Slave rules" list in the protocol v1 spec
 * (dtz-labs/zx-audio-link).
 *
 * alink_slave_feed() never partially applies a block: validation happens
 * first, and a rejected block leaves the whole struct untouched. That is what
 * lets the caller treat "invalid" and "never arrived" as the same thing, which
 * the spec requires.
 */
#include "alink.h"

#define ALINK_RESP_NONE    0u
#define ALINK_RESP_WELCOME 1u
#define ALINK_RESP_LINK    2u

/* Clear everything a session owns. Leaves max_payload alone: it is
 * configuration, not session state. */
static void session_reset(alink_slave_t *s)
{
    s->last_rx_seq = 1u;    /* so the session's first SEQ=0 counts as new */
    s->tx_seq = 0u;
    s->tx_pending = 0u;
    s->tx_len = 0u;
    s->resp_type = ALINK_RESP_NONE;
}

void alink_slave_init(alink_slave_t *s)
{
    u8 i;

    s->state = ALINK_LISTEN;
    s->max_payload = ALINK_MAX_PAYLOAD;
    for (i = 0; i < ALINK_MAX_PAYLOAD; ++i) {
        s->tx_payload[i] = 0u;
    }
    s->rx.type = 0u;
    s->rx.seq = 0u;
    s->rx.ack = 0u;
    s->rx.len = 0u;
    for (i = 0; i < ALINK_MAX_PAYLOAD; ++i) {
        s->rx.payload[i] = 0u;
    }
    session_reset(s);
}

void alink_slave_feed(alink_slave_t *s, const u8 *block, u8 block_len,
                      alink_rx_t *out)
{
    alink_frame_t f;

    out->respond = 0u;
    out->deliver_len = 0u;
    out->session_reset = 0u;
    out->carrier_lost = 0u;

    /* Decode into a local first, so a rejected block cannot disturb s->rx. */
    if (!alink_frame_decode(block, block_len, s->max_payload, &f)) {
        return;
    }

    /* Rule 4: a valid HELLO in any state resets the session. Its payload is
     * consumed by negotiation and must never enter the byte stream. */
    if (f.type == ALINK_TYPE_HELLO) {
        session_reset(s);
        s->state = ALINK_LINKED;
        s->resp_type = ALINK_RESP_WELCOME;
        out->session_reset = 1u;
        out->respond = 1u;
        return;
    }

    /* Rule 1: while in LISTEN, silently ignore everything except HELLO. */
    if (s->state != ALINK_LINKED) {
        return;
    }

    if (f.type == ALINK_TYPE_BYE) {
        session_reset(s);
        s->state = ALINK_LISTEN;
        out->carrier_lost = 1u;
        return;                      /* no reply required */
    }

    /* WELCOME is a slave-to-master frame; a slave never acts on one. */
    if (f.type != ALINK_TYPE_LINK) {
        return;
    }

    s->rx = f;

    /* Rule 3: our outstanding payload is released when the master echoes its
     * SEQ back as ACK. Only then does our SEQ advance. */
    if (s->tx_pending && s->rx.ack == s->tx_seq) {
        s->tx_pending = 0u;
        s->tx_len = 0u;
        s->tx_seq = (u8)(s->tx_seq ^ 1u);
    }

    /* Rule 2: accept a downstream payload only from a LEN>0 frame whose SEQ
     * differs from the last accepted one. The LEN>0 test is also what
     * implements "receivers ignore the SEQ bit of LEN=0 frames" -- an empty
     * poll must never advance last_rx_seq, or the next real payload carrying
     * that same SEQ would be discarded as a duplicate. */
    if (s->rx.len > 0u && s->rx.seq != s->last_rx_seq) {
        s->last_rx_seq = s->rx.seq;
        out->deliver_len = s->rx.len;
    }

    /* Rule 1: while LINKED, respond to every valid master frame. */
    s->resp_type = ALINK_RESP_LINK;
    out->respond = 1u;
}

const u8 *alink_slave_rx_payload(const alink_slave_t *s)
{
    return s->rx.payload;
}

u8 alink_slave_tx_ready(const alink_slave_t *s)
{
    return (u8)((s->state == ALINK_LINKED && s->tx_pending == 0u) ? 1u : 0u);
}

void alink_slave_tx_set(alink_slave_t *s, const u8 *buf, u8 n)
{
    u8 i;

    if (!alink_slave_tx_ready(s)) {
        return;               /* rule 3: the outstanding payload must repeat */
    }
    if (n > ALINK_MAX_PAYLOAD) {
        n = ALINK_MAX_PAYLOAD;
    }
    for (i = 0; i < n; ++i) {
        s->tx_payload[i] = buf[i];
    }
    s->tx_len = n;
    s->tx_pending = (u8)((n > 0u) ? 1u : 0u);
}

void alink_slave_response(const alink_slave_t *s, alink_frame_t *resp)
{
    u8 i;

    if (s->resp_type == ALINK_RESP_WELCOME) {
        resp->type = ALINK_TYPE_WELCOME;
        resp->seq = 0u;                        /* negotiation is SEQ=0, ACK=0 */
        resp->ack = 0u;
        resp->len = ALINK_HELLO_PAYLOAD;
        resp->payload[0] = ALINK_VERSION;
        resp->payload[1] = ALINK_CAPS;
        resp->payload[2] = s->max_payload;
        return;
    }

    resp->type = ALINK_TYPE_LINK;
    resp->seq = s->tx_seq;
    resp->ack = s->last_rx_seq;                /* rule 1: always echo it */
    resp->len = (u8)(s->tx_pending ? s->tx_len : 0u);
    for (i = 0; i < resp->len; ++i) {
        resp->payload[i] = s->tx_payload[i];
    }
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cc -std=c99 -Wall -Wextra -Werror -Iinclude test/test_alink_slave.c src/alink_slave.c src/alink_frame.c -o /tmp/t && /tmp/t
```
Expected: `alink_slave: <N> checks passed`, exit 0, no warnings.

- [ ] **Step 6: Wire the suite into the host test runner**

In `test/run.sh`, immediately after the `test_alink_frame` block added in Task 1:

```sh
$CC $CFLAGS "$ROOT/test/test_alink_slave.c" "$ROOT/src/alink_slave.c" "$ROOT/src/alink_frame.c" -o "$OUT/test_alink_slave"
"$OUT/test_alink_slave"
```

- [ ] **Step 7: Run the full host suite**

Run: `make host-test`
Expected: both `alink_frame:` and `alink_slave:` lines, then `ALL HOST TESTS PASSED`.

- [ ] **Step 8: Commit**

```bash
git add include/alink.h src/alink_slave.c test/test_alink_slave.c test/run.sh
git commit -m "alink: protocol v1 slave session state machine (host TDD)"
```

---

### Task 3: Python frame codec

**Files:**
- Create: `tools/alink/__init__.py`
- Create: `tools/alink/frame.py`
- Create: `test/test_alink_frame_py.py`
- Modify: `Makefile` (add the Python suite to `host-test`)

**Interfaces:**
- Consumes: nothing from earlier tasks at runtime; must agree with Task 1's wire format exactly
- Produces:
  - `alink.frame.HELLO = 0`, `WELCOME = 1`, `LINK = 2`, `BYE = 3`
  - `alink.frame.MAX_PAYLOAD = 64`, `HELLO_PAYLOAD = 3`, `VERSION = 1`, `CAPS = 0`
  - `alink.frame.crc16(data: bytes) -> int`
  - `alink.frame.encode(type_: int, seq: int, ack: int, payload: bytes = b"") -> bytes`
  - `alink.frame.decode(block: bytes, max_payload: int = MAX_PAYLOAD) -> Frame | None`
  - `class Frame` with attributes `type`, `seq`, `ack`, `payload` and property `len`

- [ ] **Step 1: Write the failing test**

Create `test/test_alink_frame_py.py`:

```python
#!/usr/bin/env python3
"""Host tests for the Python side of the protocol v1 frame codec.

Standalone script, no pytest -- matches test/test_check_image_limit.py, which
is how this repo runs its Python tests.

The point of these tests is not that the codec is self-consistent. It is that
it agrees with src/alink_frame.c. Task 5 proves that end to end against the
compiled C slave; this file pins the wire bytes so a divergence shows up here
first, with a readable diff.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame  # noqa: E402

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def test_crc_published_vector():
    check(frame.crc16(b"123456789") == 0x29B1, "CCITT-FALSE check value")
    check(frame.crc16(b"") == 0xFFFF, "empty input returns the init value")


def test_encode_layout():
    block = frame.encode(frame.LINK, seq=1, ack=1, payload=b"hi")
    check(len(block) == 6, "4 + payload")
    check(block[0] == (frame.LINK << 5) | 0x02 | 0x01, "CTRL bit layout")
    check(block[1] == 2, "LEN")
    check(block[2:4] == b"hi", "payload")
    crc = frame.crc16(block[:4])
    check(block[4] == (crc >> 8) & 0xFF, "CRC high byte first")
    check(block[5] == crc & 0xFF, "CRC low byte second")


def test_round_trip_all_lengths():
    for length in range(0, frame.MAX_PAYLOAD + 1):
        payload = bytes((i * 7 + 1) & 0xFF for i in range(length))
        block = frame.encode(frame.LINK, seq=length & 1,
                             ack=(length >> 1) & 1, payload=payload)
        check(len(block) == length + 4, f"block length at len={length}")
        f = frame.decode(block)
        check(f is not None, f"decodes at len={length}")
        check(f.type == frame.LINK, f"type at len={length}")
        check(f.seq == (length & 1), f"seq at len={length}")
        check(f.ack == ((length >> 1) & 1), f"ack at len={length}")
        check(f.payload == payload, f"payload at len={length}")
        check(f.len == length, f"len property at len={length}")


def test_reject_length_mismatch():
    block = frame.encode(frame.LINK, 0, 0, b"abcd")
    check(frame.decode(block) is not None, "the intact block decodes")
    check(frame.decode(block[:-1]) is None, "one byte short")
    check(frame.decode(block + b"\x00") is None, "one byte long")
    check(frame.decode(b"\x00\x00\x00") is None, "shorter than a header")


def test_reject_bit_flips():
    good = frame.encode(frame.LINK, 0, 0, b"abc")
    for i in range(len(good)):
        for bit in range(8):
            bad = bytearray(good)
            bad[i] ^= 1 << bit
            check(frame.decode(bytes(bad)) is None,
                  f"flip byte {i} bit {bit} must be rejected")


def test_reject_oversize_and_over_advertised():
    block = frame.encode(frame.LINK, 0, 0, b"x" * 40)
    check(frame.decode(block, max_payload=frame.MAX_PAYLOAD) is not None,
          "40 bytes is under the absolute cap")
    check(frame.decode(block, max_payload=32) is None, "over advertised")
    check(frame.decode(block, max_payload=40) is not None, "exactly at it")

    hello = frame.encode(frame.HELLO, 0, 0,
                         bytes([frame.VERSION, frame.CAPS, 64]) + b"x" * 37)
    check(frame.decode(hello, max_payload=8) is not None,
          "the advertised cap applies to LINK frames only")


def test_hello_minimum_length():
    for length in range(0, frame.HELLO_PAYLOAD):
        for type_ in (frame.HELLO, frame.WELCOME):
            block = frame.encode(type_, 0, 0, b"\x00" * length)
            check(frame.decode(block) is None,
                  f"type {type_} with LEN={length} must be rejected")

    block = frame.encode(frame.HELLO, 0, 0,
                         bytes([frame.VERSION, frame.CAPS, 64, 0xDE, 0xAD]))
    f = frame.decode(block)
    check(f is not None, "LEN>3 HELLO decodes")
    check(f.payload[0] == frame.VERSION, "version at offset 0")
    check(f.payload[2] == 64, "max payload at offset 2")
    check(f.len == 5, "extra bytes are kept, just ignored by negotiation")


def test_reject_reserved_types():
    for type_ in range(4, 8):
        block = frame.encode(type_, 0, 0, b"")
        check(frame.decode(block) is None, f"type {type_} is reserved")


def test_reserved_ctrl_bits_ignored():
    body = bytes([(frame.LINK << 5) | 0x1C | 0x02, 0])
    block = body + frame.crc16(body).to_bytes(2, "big")
    f = frame.decode(block)
    check(f is not None, "reserved CTRL bits do not invalidate a frame")
    check(f.type == frame.LINK, "type survives")
    check(f.seq == 1 and f.ack == 0, "seq/ack survive")


def main():
    test_crc_published_vector()
    test_encode_layout()
    test_round_trip_all_lengths()
    test_reject_length_mismatch()
    test_reject_bit_flips()
    test_reject_oversize_and_over_advertised()
    test_hello_minimum_length()
    test_reject_reserved_types()
    test_reserved_ctrl_bits_ignored()
    print(f"alink_frame_py: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 test/test_alink_frame_py.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'alink'`.

- [ ] **Step 3: Write the implementation**

Create `tools/alink/__init__.py`:

```python
"""Half-Duplex Terminal Protocol v1 -- PC side.

The wire protocol is specified in dtz-labs/zx-audio-link; this package is the
master half of it. The slave half is src/alink_frame.c and src/alink_slave.c,
compiled for the Z80. Standard library only.
"""
```

Create `tools/alink/frame.py`:

```python
"""Protocol v1 frame codec -- the Python mirror of src/alink_frame.c.

    +------+------+-----------+--------+
    | CTRL | LEN  | PAYLOAD   | CRC-16 |
    | 1 B  | 1 B  | 0..64 B   | 2 B    |
    +------+------+-----------+--------+

CTRL bits: [TYPE:7-5] [reserved:4-2] [SEQ:1] [ACK:0]

Any change here must be mirrored in src/alink_frame.c and vice versa.
test/test_alink_xcheck.py is what actually enforces that.
"""

HELLO = 0
WELCOME = 1
LINK = 2
BYE = 3

MAX_PAYLOAD = 64
FRAME_MAX = 4 + MAX_PAYLOAD
HELLO_PAYLOAD = 3

VERSION = 1
CAPS = 0x00

CTRL_ACK = 0x01
CTRL_SEQ = 0x02
CTRL_TYPE_SHIFT = 5


class Frame:
    __slots__ = ("type", "seq", "ack", "payload")

    def __init__(self, type_, seq, ack, payload):
        self.type = type_
        self.seq = seq
        self.ack = ack
        self.payload = payload

    @property
    def len(self):
        return len(self.payload)

    def __repr__(self):
        names = {HELLO: "HELLO", WELCOME: "WELCOME", LINK: "LINK", BYE: "BYE"}
        return (f"Frame({names.get(self.type, self.type)}, seq={self.seq}, "
                f"ack={self.ack}, len={self.len})")


def crc16(data):
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode(type_, seq, ack, payload=b""):
    """Serialise one frame. Raises ValueError if the payload exceeds the cap."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} exceeds {MAX_PAYLOAD}")
    ctrl = ((type_ & 0x07) << CTRL_TYPE_SHIFT)
    if seq:
        ctrl |= CTRL_SEQ
    if ack:
        ctrl |= CTRL_ACK
    body = bytes([ctrl, len(payload)]) + payload
    return body + crc16(body).to_bytes(2, "big")


def decode(block, max_payload=MAX_PAYLOAD):
    """Validate and parse one received block.

    Returns a Frame, or None if the block fails any validation rule -- in which
    case the caller must behave exactly as if nothing had arrived.
    """
    if len(block) < 4:
        return None

    length = block[1]
    if length > MAX_PAYLOAD:                       # rule 3, absolute cap
        return None
    if len(block) != length + 4:                   # rule 1
        return None

    body = block[:2 + length]                      # rule 2
    want = int.from_bytes(block[2 + length:4 + length], "big")
    if crc16(body) != want:
        return None

    type_ = (block[0] >> CTRL_TYPE_SHIFT) & 0x07
    if type_ > BYE:                                # 4-7 reserved: ignore
        return None
    if type_ == LINK and length > max_payload:     # rule 3, LINK only
        return None
    if type_ in (HELLO, WELCOME) and length < HELLO_PAYLOAD:   # rule 4
        return None

    return Frame(type_,
                 1 if block[0] & CTRL_SEQ else 0,
                 1 if block[0] & CTRL_ACK else 0,
                 bytes(block[2:2 + length]))
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 test/test_alink_frame_py.py`
Expected: `alink_frame_py: <N> checks passed`, exit 0.

- [ ] **Step 5: Wire it into `host-test`**

In `Makefile`, change the `host-test` target to:

```make
host-test:
	CC="$(CC)" sh test/run.sh
	python3 test/test_check_image_limit.py
	python3 test/test_alink_frame_py.py
```

- [ ] **Step 6: Verify the whole suite and the compile check**

Run: `make ci`
Expected: `ALL HOST TESTS PASSED`, then `alink_frame_py: <N> checks passed`, then `py_compile` succeeds. (`python-check` compiles `tools/*.py`; `tools/alink/*.py` is a subdirectory and is not matched by that glob, which is why the test above imports the package directly — that import is the compile check that matters.)

- [ ] **Step 7: Commit**

```bash
git add tools/alink/__init__.py tools/alink/frame.py test/test_alink_frame_py.py Makefile
git commit -m "alink: Python frame codec mirroring the C implementation"
```

---

### Task 4: Python master ARQ state machine

**Files:**
- Create: `tools/alink/master.py`
- Create: `test/test_alink_master.py`
- Modify: `Makefile` (add the suite to `host-test`)

**Interfaces:**
- Consumes: `alink.frame` — `encode`, `decode`, `HELLO`, `WELCOME`, `LINK`, `BYE`, `MAX_PAYLOAD`, `HELLO_PAYLOAD`, `VERSION`, `CAPS`, `Frame`
- Produces:
  - `class Channel` — the transport interface a master needs: `send(block: bytes) -> None`, `recv(timeout: float) -> bytes | None`
  - `class Master` with `__init__(self, channel, *, t_resp=10.0, retries=3, t_hello_retry=2.0, max_payload=64)`
  - `Master.state` — `"DOWN"` or `"LINKED"`
  - `Master.send(data: bytes) -> None` — queue downstream bytes (from the pty)
  - `Master.received` — `bytearray` of upstream bytes accumulated (keystrokes)
  - `Master.transact() -> bool` — run exactly one exchange; returns True if it completed
  - `Master.close() -> None` — best-effort BYE

**Master rules being implemented** (from protocol v1, quoted so the implementer does not have to fetch the spec):

1. Send a frame; wait up to `T_RESP` for a response, measured from the end of our own transmission.
2. **Any valid response completes the transaction** and resets the retry counter. A transaction fails only on timeout or an invalid block. The ACK bit is never a failure condition.
3. Downstream delivery: if a downstream payload is outstanding and the response's ACK equals its SEQ, it is delivered — advance SEQ. Otherwise it stays outstanding and is retransmitted as the next transaction's frame.
4. Upstream acceptance: if the response is a LINK with LEN>0 and SEQ differs from the last accepted upstream SEQ, accept the payload and update the ACK we send from now on.
5. On failure: resend the identical frame. At most `RETRIES` attempts **in total**; exhausting them declares the link DOWN.
6. Unacknowledged in-flight payloads are dropped on any session reset and must never be resent into a new session.

- [ ] **Step 1: Write the failing test**

Create `test/test_alink_master.py`:

```python
#!/usr/bin/env python3
"""Host tests for the protocol v1 master, over a simulated channel.

The channel can drop, corrupt and duplicate blocks on demand, so every ARQ
rule is exercised without any audio. The slave on the far side is a minimal
in-process model -- the REAL C slave is exercised in test_alink_xcheck.py.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame  # noqa: E402
from alink.master import Master  # noqa: E402

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


class ModelSlave:
    """A minimal protocol-conformant slave, used only to drive the master."""

    def __init__(self):
        self.state = "LISTEN"
        self.last_rx_seq = 1
        self.tx_seq = 0
        self.tx_payload = b""
        self.tx_pending = False
        self.delivered = bytearray()

    def queue(self, data):
        if not self.tx_pending and self.state == "LINKED":
            self.tx_payload = data[:frame.MAX_PAYLOAD]
            self.tx_pending = bool(self.tx_payload)

    def handle(self, block):
        f = frame.decode(block)
        if f is None:
            return None
        if f.type == frame.HELLO:
            self.__init__()
            self.state = "LINKED"
            return frame.encode(frame.WELCOME, 0, 0,
                                bytes([frame.VERSION, frame.CAPS, 64]))
        if self.state != "LINKED":
            return None
        if f.type == frame.BYE:
            self.__init__()
            return None
        if f.type != frame.LINK:
            return None
        if self.tx_pending and f.ack == self.tx_seq:
            self.tx_pending = False
            self.tx_payload = b""
            self.tx_seq ^= 1
        if f.len > 0 and f.seq != self.last_rx_seq:
            self.last_rx_seq = f.seq
            self.delivered.extend(f.payload)
        return frame.encode(frame.LINK, self.tx_seq, self.last_rx_seq,
                            self.tx_payload if self.tx_pending else b"")


class FakeChannel:
    """Carries blocks to a ModelSlave, with scriptable faults."""

    def __init__(self, slave):
        self.slave = slave
        self.drop_next = 0        # drop this many upcoming responses
        self.corrupt_next = 0     # corrupt this many upcoming responses
        self.sent = []
        self._pending = None

    def send(self, block):
        self.sent.append(block)
        resp = self.slave.handle(block)
        if resp is not None and self.drop_next > 0:
            self.drop_next -= 1
            resp = None
        elif resp is not None and self.corrupt_next > 0:
            self.corrupt_next -= 1
            bad = bytearray(resp)
            bad[0] ^= 0xFF
            resp = bytes(bad)
        self._pending = resp

    def recv(self, timeout):
        resp, self._pending = self._pending, None
        return resp


def linked_pair():
    slave = ModelSlave()
    ch = FakeChannel(slave)
    m = Master(ch)
    check(m.transact() is True, "HELLO/WELCOME completes")
    check(m.state == "LINKED", "master is LINKED after WELCOME")
    return m, ch, slave


def test_handshake():
    m, ch, slave = linked_pair()
    check(slave.state == "LINKED", "slave is LINKED")
    check(len(ch.sent) == 1, "one frame sent for the handshake")
    check(frame.decode(ch.sent[0]).type == frame.HELLO, "it was a HELLO")


def test_downstream_delivery_in_order():
    m, ch, slave = linked_pair()
    m.send(b"hello world")
    m.transact()
    check(bytes(slave.delivered) == b"hello world", "payload delivered once")
    m.send(b"!")
    m.transact()
    check(bytes(slave.delivered) == b"hello world!", "second payload appended")


def test_downstream_chunked_at_max_payload():
    m, ch, slave = linked_pair()
    m.send(b"x" * 150)
    for _ in range(5):
        m.transact()
    check(bytes(slave.delivered) == b"x" * 150, "all 150 bytes arrive")
    check(all(frame.decode(b).len <= frame.MAX_PAYLOAD
              for b in ch.sent), "no frame exceeded the cap")


def test_lost_response_retransmits_without_duplicating():
    m, ch, slave = linked_pair()
    m.send(b"abc")
    ch.drop_next = 1
    m.transact()                       # response dropped -> retry inside
    check(bytes(slave.delivered) == b"abc",
          "the slave saw it once despite the retransmit")
    m.send(b"de")
    m.transact()
    check(bytes(slave.delivered) == b"abcde", "stream stays in order")


def test_corrupt_response_is_treated_as_lost():
    m, ch, slave = linked_pair()
    m.send(b"abc")
    ch.corrupt_next = 1
    m.transact()
    check(bytes(slave.delivered) == b"abc", "delivered exactly once")


def test_retry_exhaustion_declares_down():
    m, ch, slave = linked_pair()
    m.send(b"abc")
    ch.drop_next = 99
    check(m.transact() is False, "transaction fails")
    check(m.state == "DOWN", "link declared DOWN after RETRIES")


def test_upstream_keystrokes_arrive_once():
    m, ch, slave = linked_pair()
    slave.queue(b"ls\r")
    m.transact()
    check(bytes(m.received) == b"ls\r", "keystrokes arrive")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"ls\r", "and are not delivered again")


def test_upstream_survives_retransmit_storm_unduplicated():
    """A keyboard chunk pending during a retransmit storm must survive
    un-duplicated -- named explicitly as a regression case by the protocol
    spec's testing section."""
    m, ch, slave = linked_pair()
    slave.queue(b"KEY")
    ch.drop_next = 2
    m.transact()
    check(bytes(m.received) == b"KEY", "arrives despite two lost responses")
    for _ in range(3):
        m.transact()
    check(bytes(m.received) == b"KEY", "exactly once")


def test_idle_polling_never_flaps():
    """An idle link polled forever must never flap -- empty polls complete
    transactions regardless of ACK bits."""
    m, ch, slave = linked_pair()
    for _ in range(50):
        check(m.transact() is True, "idle poll completes")
    check(m.state == "LINKED", "still LINKED after 50 idle polls")
    check(len(m.received) == 0, "no phantom upstream data")


def test_welcome_payload_never_reaches_the_stream():
    """The WELCOME payload must never enter the byte stream."""
    m, ch, slave = linked_pair()
    check(len(m.received) == 0, "handshake produced no stream bytes")
    m.transact()
    check(len(m.received) == 0, "and still none after a poll")


def test_reconnect_after_down_drops_inflight():
    m, ch, slave = linked_pair()
    m.send(b"lost forever")
    ch.drop_next = 99
    m.transact()
    check(m.state == "DOWN", "went DOWN")

    ch.drop_next = 0
    check(m.transact() is True, "HELLO retry reconnects")
    check(m.state == "LINKED", "LINKED again")
    check(bytes(slave.delivered) == b"",
          "the in-flight payload was dropped, not resent into the new session")


def main():
    test_handshake()
    test_downstream_delivery_in_order()
    test_downstream_chunked_at_max_payload()
    test_lost_response_retransmits_without_duplicating()
    test_corrupt_response_is_treated_as_lost()
    test_retry_exhaustion_declares_down()
    test_upstream_keystrokes_arrive_once()
    test_upstream_survives_retransmit_storm_unduplicated()
    test_idle_polling_never_flaps()
    test_welcome_payload_never_reaches_the_stream()
    test_reconnect_after_down_drops_inflight()
    print(f"alink_master: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 test/test_alink_master.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'alink.master'`.

- [ ] **Step 3: Write the implementation**

Create `tools/alink/master.py`:

```python
"""Protocol v1 master: stop-and-wait ARQ with piggybacked upstream data.

The master owns every exchange. It is transport-agnostic: give it any object
with send(block) and recv(timeout) and it will run the protocol over it. That
is what lets the same class run against a simulated lossy channel in tests, a
pipe to the compiled C slave in the cross-check, and real audio in PR B.
"""
from . import frame


class Channel:
    """The transport interface a Master needs.

    Implementations must be half-duplex: send() returns once the block is fully
    on the wire, and recv() waits up to `timeout` seconds for one whole block.
    """

    def send(self, block):
        raise NotImplementedError

    def recv(self, timeout):
        """Return one complete block, or None on timeout."""
        raise NotImplementedError


class Master:
    def __init__(self, channel, *, t_resp=10.0, retries=3,
                 t_hello_retry=2.0, max_payload=frame.MAX_PAYLOAD):
        self.channel = channel
        self.t_resp = t_resp
        self.retries = retries
        self.t_hello_retry = t_hello_retry
        self.max_payload = max_payload

        self.state = "DOWN"
        self.received = bytearray()      # upstream bytes, for the pty
        self._outbox = bytearray()       # downstream bytes, from the pty
        self._reset_session()

    # -- session state ---------------------------------------------------

    def _reset_session(self):
        """Rule 6: in-flight payloads are dropped on any session reset and
        must never be resent into a new session."""
        self.tx_seq = 0
        self.tx_payload = None           # the outstanding downstream payload
        self.last_rx_seq = 1             # so the first SEQ=0 counts as new
        self.peer_max_payload = frame.MAX_PAYLOAD

    # -- public API ------------------------------------------------------

    def send(self, data):
        """Queue downstream bytes. Chunking happens at transmission time."""
        self._outbox.extend(data)

    def close(self):
        """Best-effort courtesy close. BYE requires no reply."""
        if self.state == "LINKED":
            try:
                self.channel.send(frame.encode(frame.BYE, 0, 0))
            except OSError:
                pass
        self.state = "DOWN"

    def transact(self):
        """Run exactly one exchange. Returns True if it completed."""
        if self.state != "LINKED":
            return self._handshake()
        return self._link_transaction()

    # -- internals -------------------------------------------------------

    def _exchange(self, block):
        """Send a block and wait for a valid response.

        Rule 2: ANY valid response completes the transaction. Rule 5: on
        failure resend the identical block, at most `retries` attempts in
        total including the first.
        """
        for _ in range(self.retries):
            self.channel.send(block)
            reply = self.channel.recv(self.t_resp)
            if reply is None:
                continue
            f = frame.decode(reply, self.max_payload)
            if f is not None:
                return f
        return None

    def _handshake(self):
        offer = bytes([frame.VERSION, frame.CAPS, frame.MAX_PAYLOAD])
        f = self._exchange(frame.encode(frame.HELLO, 0, 0, offer))
        if f is None or f.type != frame.WELCOME:
            return False

        version = f.payload[0]
        peer_max = f.payload[2]
        if version == 0 or not 1 <= peer_max <= frame.MAX_PAYLOAD:
            return False                 # an invalid WELCOME is no WELCOME

        self._reset_session()
        self.peer_max_payload = peer_max
        self.state = "LINKED"
        return True

    def _link_transaction(self):
        # Rule 3: an outstanding payload is retransmitted unchanged until the
        # slave acknowledges it, so only pull new bytes when nothing is in
        # flight.
        if self.tx_payload is None and self._outbox:
            take = min(len(self._outbox), self.peer_max_payload)
            self.tx_payload = bytes(self._outbox[:take])
            del self._outbox[:take]

        payload = self.tx_payload if self.tx_payload is not None else b""
        block = frame.encode(frame.LINK, self.tx_seq, self.last_rx_seq, payload)

        f = self._exchange(block)
        if f is None:
            self.state = "DOWN"          # rule 5: retries exhausted
            self._reset_session()
            return False

        if f.type != frame.LINK:
            return True                  # valid, so the transaction completed

        # Rule 3: delivery is confirmed by the ACK matching our SEQ.
        if self.tx_payload is not None and f.ack == self.tx_seq:
            self.tx_payload = None
            self.tx_seq ^= 1

        # Rule 4: accept upstream only from LEN>0 with a new SEQ. The LEN>0
        # test is also what implements "receivers ignore the SEQ bit of LEN=0
        # frames".
        if f.len > 0 and f.seq != self.last_rx_seq:
            self.last_rx_seq = f.seq
            self.received.extend(f.payload)

        return True
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 test/test_alink_master.py`
Expected: `alink_master: <N> checks passed`, exit 0.

- [ ] **Step 5: Wire it into `host-test`**

In `Makefile`, add to the `host-test` target after the frame test:

```make
	python3 test/test_alink_master.py
```

- [ ] **Step 6: Run the whole suite**

Run: `make ci`
Expected: all suites pass, including `alink_master: <N> checks passed`.

- [ ] **Step 7: Commit**

```bash
git add tools/alink/master.py test/test_alink_master.py Makefile
git commit -m "alink: Python master ARQ state machine over a lossy channel"
```

---

### Task 5: Cross-implementation check — Python master against the C slave

**Files:**
- Create: `tools/alink/xcheck_slave.c`
- Create: `test/test_alink_xcheck.py`
- Modify: `Makefile` (add the suite to `host-test`)

**Interfaces:**
- Consumes: `src/alink_slave.c`, `src/alink_frame.c`, `include/alink.h` from Tasks 1–2; `alink.frame` and `alink.master.Master` from Tasks 3–4
- Produces: nothing other tasks depend on. This is the gate.

**Why this task exists.** Tasks 1–4 each test one implementation against its own understanding of the spec. Two implementations can both be self-consistent and still disagree — on CRC byte order, on whether an empty poll advances SEQ, on what a duplicate does. This is the only test that would catch that, and it must pass before any audio work begins, because after PR B a failure here would be indistinguishable from a signal problem.

**Framing on the pipe.** The harness reads a one-byte length followed by that many bytes, and writes responses the same way. This is deliberately *not* the protocol's own framing: the physical layer is what delivers block boundaries (protocol v1, "Assumptions about the physical layer", point 3), so the harness must supply them too, and it must be possible to hand the slave a deliberately wrong `block_len`.

- [ ] **Step 1: Write the harness**

Create `tools/alink/xcheck_slave.c`:

```c
/*
 * xcheck_slave.c -- run the real C slave against the Python master.
 *
 * Protocol on stdin/stdout: one length byte, then that many bytes of block.
 * A response is written back the same way; a block the slave chooses not to
 * answer produces a single 0x00 length byte, so the master's channel can tell
 * "no response" from "still thinking" without a timeout.
 *
 * The length prefix is NOT part of the wire protocol. Block boundaries come
 * from the physical layer (protocol v1, "Assumptions about the physical
 * layer", point 3); this harness stands in for that layer.
 *
 * A command byte 0xFF followed by a length and payload queues upstream data,
 * standing in for the keyboard buffer.
 */
#include <stdio.h>
#include <string.h>
#include "alink.h"

#define CMD_QUEUE_TX 0xFFu

static int read_exact(u8 *buf, unsigned n)
{
    unsigned got = 0;
    int c;

    while (got < n) {
        c = getchar();
        if (c == EOF) {
            return 0;
        }
        buf[got++] = (u8)c;
    }
    return 1;
}

int main(void)
{
    alink_slave_t slave;
    alink_rx_t rx;
    alink_frame_t resp;
    u8 block[256];
    u8 out[ALINK_FRAME_MAX];
    u8 pending_tx[ALINK_MAX_PAYLOAD];
    u8 pending_tx_len = 0;
    int c;
    unsigned n, i;

    alink_slave_init(&slave);

    for (;;) {
        c = getchar();
        if (c == EOF) {
            break;
        }

        if ((u8)c == CMD_QUEUE_TX) {
            c = getchar();
            if (c == EOF) {
                break;
            }
            n = (unsigned)c;
            if (!read_exact(pending_tx, n)) {
                break;
            }
            pending_tx_len = (u8)n;
            continue;
        }

        n = (unsigned)c;
        if (n > 0 && !read_exact(block, n)) {
            break;
        }

        alink_slave_feed(&slave, block, (u8)n, &rx);

        /* Mirror the integration order the header documents: consume the
         * delivered payload, then top up the upstream slot, then answer. */
        if (rx.deliver_len > 0u) {
            /* Nothing to render here; the Python side asserts on delivery by
             * echoing it back as upstream data instead. */
            if (alink_slave_tx_ready(&slave)) {
                alink_slave_tx_set(&slave,
                                   alink_slave_rx_payload(&slave),
                                   rx.deliver_len);
            }
        } else if (pending_tx_len > 0u && alink_slave_tx_ready(&slave)) {
            alink_slave_tx_set(&slave, pending_tx, pending_tx_len);
            pending_tx_len = 0u;
        }

        if (!rx.respond) {
            putchar(0);
            fflush(stdout);
            continue;
        }

        alink_slave_response(&slave, &resp);
        i = alink_frame_encode(&resp, out);
        putchar((int)i);
        for (n = 0; n < i; ++n) {
            putchar((int)out[n]);
        }
        fflush(stdout);
    }

    return 0;
}
```

- [ ] **Step 2: Write the failing test**

Create `test/test_alink_xcheck.py`:

```python
#!/usr/bin/env python3
"""Cross-implementation check: the Python master against the REAL C slave.

Tasks 1-4 each test one implementation against its own reading of the spec.
Two implementations can both be self-consistent and still disagree. This is the
only test that catches that, and it must pass before any audio work starts --
after PR B a failure here would be indistinguishable from a signal problem.

The C slave echoes every downstream payload back as upstream data, so a single
assertion covers both directions at once.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame  # noqa: E402
from alink.master import Master  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(ROOT, "build", "host")
BINARY = os.path.join(BUILD, "alink_xcheck_slave")

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def build_slave():
    os.makedirs(BUILD, exist_ok=True)
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
         "-I", os.path.join(ROOT, "include"),
         os.path.join(ROOT, "tools", "alink", "xcheck_slave.c"),
         os.path.join(ROOT, "src", "alink_slave.c"),
         os.path.join(ROOT, "src", "alink_frame.c"),
         "-o", BINARY],
        check=True,
    )


class PipeChannel:
    """Carries blocks to the C slave over its stdin/stdout, length-prefixed."""

    def __init__(self, proc):
        self.proc = proc
        self._pending = None

    def send(self, block):
        self.proc.stdin.write(bytes([len(block)]) + block)
        self.proc.stdin.flush()
        n = self.proc.stdout.read(1)
        if not n:
            raise OSError("slave exited")
        n = n[0]
        self._pending = self.proc.stdout.read(n) if n else None

    def recv(self, timeout):
        resp, self._pending = self._pending, None
        return resp

    def queue_upstream(self, data):
        self.proc.stdin.write(bytes([0xFF, len(data)]) + data)
        self.proc.stdin.flush()


def run(fn):
    proc = subprocess.Popen([BINARY], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE)
    try:
        ch = PipeChannel(proc)
        fn(Master(ch), ch)
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)


def test_handshake(m, ch):
    check(m.transact() is True, "HELLO/WELCOME completes against the C slave")
    check(m.state == "LINKED", "master reaches LINKED")


def test_echo_round_trip(m, ch):
    m.transact()
    m.send(b"hello")
    m.transact()                     # slave receives and queues the echo
    m.transact()                     # slave sends it back
    check(bytes(m.received) == b"hello",
          f"echo returned intact, got {bytes(m.received)!r}")


def test_duplicate_suppression(m, ch):
    """Re-sending the identical block must be answered but not re-delivered.
    If the two implementations disagree on the SEQ rule, the echo doubles."""
    m.transact()
    block = frame.encode(frame.LINK, 0, 1, b"abc")
    ch.send(block)
    ch.recv(0)
    ch.send(block)                   # byte-identical retransmission
    ch.recv(0)
    m.last_rx_seq = 1
    m.transact()
    m.transact()
    check(bytes(m.received) == b"abc",
          f"delivered exactly once, got {bytes(m.received)!r}")


def test_empty_polls_do_not_disturb_sequence(m, ch):
    m.transact()
    for _ in range(10):
        check(m.transact() is True, "idle poll completes against the C slave")
    m.send(b"z")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"z",
          "a payload after ten empty polls still arrives")


def test_all_payload_lengths(m, ch):
    m.transact()
    for length in (1, 2, 63, 64):
        m.received.clear()
        payload = bytes((i * 3 + 1) & 0xFF for i in range(length))
        m.send(payload)
        for _ in range(4):
            m.transact()
        check(bytes(m.received) == payload,
              f"length {length} round-trips, got {len(m.received)} bytes")


def test_hello_resets_mid_session(m, ch):
    m.transact()
    m.send(b"abc")
    m.transact()
    m.state = "DOWN"                 # force a fresh handshake
    m._reset_session()
    check(m.transact() is True, "the C slave answers a mid-session HELLO")
    check(m.state == "LINKED", "and the session is LINKED again")
    m.received.clear()
    m.send(b"q")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"q", "the reset session carries data again")


def main():
    build_slave()
    for fn in (test_handshake, test_echo_round_trip, test_duplicate_suppression,
               test_empty_polls_do_not_disturb_sequence,
               test_all_payload_lengths, test_hello_resets_mid_session):
        run(fn)
    print(f"alink_xcheck: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `python3 test/test_alink_xcheck.py`
Expected: FAIL — the compile step cannot find `tools/alink/xcheck_slave.c` (if Step 1 was skipped) or, once it exists, the test runs and any spec divergence between the two implementations surfaces as a failed assertion.

- [ ] **Step 4: Make it pass**

If assertions fail, the two implementations disagree. Fix whichever side departs from the Global Constraints above — **do not** adjust the test to accommodate a divergence. Re-run until clean.

- [ ] **Step 5: Run the test to verify it passes**

Run: `python3 test/test_alink_xcheck.py`
Expected: `alink_xcheck: <N> checks passed`, exit 0.

- [ ] **Step 6: Wire it into `host-test`**

In `Makefile`, add to `host-test` after the master test:

```make
	python3 test/test_alink_xcheck.py
```

- [ ] **Step 7: Verify the mutation guard**

Prove the cross-check is not vacuous. Temporarily change `src/alink_frame.c`'s CRC transmission order:

```c
    out[2u + f->len] = (u8)(crc & 0xFFu);
    out[3u + f->len] = (u8)(crc >> 8);
```

Run: `python3 test/test_alink_xcheck.py`
Expected: FAIL. Revert the change and confirm it passes again.

- [ ] **Step 8: Run the whole suite**

Run: `make ci`
Expected: every suite passes, ending with `alink_xcheck: <N> checks passed`.

- [ ] **Step 9: Commit**

```bash
git add tools/alink/xcheck_slave.c test/test_alink_xcheck.py Makefile
git commit -m "alink: cross-check the Python master against the compiled C slave"
```

---

## Self-Review

**1. Spec coverage.** Every PR A item in the spec maps to a task:

| Spec item | Task |
|---|---|
| `include/alink.h`, `src/alink_frame.c`, CRC-16/CCITT-FALSE | 1 |
| Validation rules 1–4 | 1 (tests) + 1 (implementation) |
| `src/alink_slave.c`, LISTEN/LINKED, SEQ/ACK, dedupe | 2 |
| Keyboard-overflow BEL, drop-newest | **PR B** — it belongs to the `conn` backend, which owns the 128-byte buffer; the slave engine holds only the 64-byte outstanding payload |
| `tools/alink/frame.py` | 3 |
| `tools/alink/master.py`, pty integration | 4 (state machine); pty wiring is PR B |
| Cross-implementation test | 5 |
| Named regression cases (idle link never flaps, WELCOME never reaches the pty, keyboard chunk survives a retransmit storm) | 4 |
| Physical layer, `alink_phy.c`, `phy.py`, ZEsarUX tooling | **PR B** |

Two deliberate deferrals to PR B, both recorded above rather than silently dropped: keyboard-buffer overflow handling (it needs the `conn` ring that only exists on the target) and pty wiring (it needs a real pty, which is not a unit test).

**2. Placeholder scan.** No "TBD", no "add error handling", no "similar to Task N". Every code step carries the actual code. Task 5 Step 4 says "fix whichever side departs" rather than naming the fix, which is correct: the failure it responds to is by definition unknown in advance, and the Global Constraints say which side is authoritative.

**3. Type consistency.** Checked across tasks: `alink_frame_t` fields (`type`, `seq`, `ack`, `len`, `payload`) are identical in Tasks 1, 2 and 5. `alink_slave_feed()` takes `(s, block, block_len, out)` everywhere. `alink_rx_t` fields (`respond`, `deliver_len`, `session_reset`, `carrier_lost`) match between the header, the tests and `xcheck_slave.c`. Python `Frame` exposes `type`/`seq`/`ack`/`payload` plus a `len` property, and `decode()` takes `max_payload` in both Tasks 3 and 4. `Master.__init__` keyword names match their use in Task 5.
