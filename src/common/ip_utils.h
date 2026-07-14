#ifndef IP_UTILS_H
#define IP_UTILS_H

#include <stdint.h>

/*
 * Convert an IPv4 CIDR prefix length to a host-order 32-bit mask.
 *
 * Callers that expose prefix lengths through public APIs should reject values
 * greater than 32. Handling prefix_len >= 32 here also prevents an undefined
 * shift if a defensive internal caller supplies an out-of-range value.
 */
static inline uint32_t ipv4_prefix_mask(uint8_t prefix_len)
{
    if (prefix_len == 0) {
        return 0;
    }

    if (prefix_len >= 32) {
        return 0xFFFFFFFFu;
    }

    return 0xFFFFFFFFu << (32 - prefix_len);
}

#endif /* IP_UTILS_H */
