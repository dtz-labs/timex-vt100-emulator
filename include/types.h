/*
 * types.h -- shared fixed-width integer types.
 *
 * Pure definitions, no hardware dependency: included both by the Z80 target
 * build and by the host (macOS) unit-test build. Mirrors the convention from
 * the sibling twin-stick game project.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;

#endif /* TYPES_H */
