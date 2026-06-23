#ifndef BYTE_ORDER_H
#define BYTE_ORDER_H

#include <stdint.h>

/*
 * Project-owned byte-order helpers.
 *
 * Network protocols store multi-byte integers in network byte order
 * (big-endian). The simulator currently runs on little-endian hosts, so
 * converting between host order and network order means reversing the bytes.
 *
 * Keeping this logic here avoids libc/compiler byte-swap builtins that are
 * harder for EVA/KLEE to reason about. If big-endian host support is needed
 * later, this header is the single place to make ns_hton/ns_ntoh identity
 * operations on those hosts.
 */

/* Swap the two bytes in a 16-bit value: 0x1234 -> 0x3412. */
static inline uint16_t ns_bswap16(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

/* Swap the four bytes in a 32-bit value: 0x11223344 -> 0x44332211. */
static inline uint32_t ns_bswap32(uint32_t x)
{
    /*
     * Mask one byte at a time, move it to its opposite position, then OR the
     * four moved bytes back together.
     */
    return ((x & 0x000000ffu) << 24) |
           ((x & 0x0000ff00u) << 8)  |
           ((x & 0x00ff0000u) >> 8)  |
           ((x & 0xff000000u) >> 24);
}

/* Host uint16_t to network uint16_t. */
static inline uint16_t ns_htons(uint16_t x)
{
    return ns_bswap16(x);
}

/* Network uint16_t to host uint16_t. Same operation as host -> network. */
static inline uint16_t ns_ntohs(uint16_t x)
{
    return ns_bswap16(x);
}

/* Host uint32_t to network uint32_t. */
static inline uint32_t ns_htonl(uint32_t x)
{
    return ns_bswap32(x);
}

/* Network uint32_t to host uint32_t. Same operation as host -> network. */
static inline uint32_t ns_ntohl(uint32_t x)
{
    return ns_bswap32(x);
}

#endif /* BYTE_ORDER_H */
