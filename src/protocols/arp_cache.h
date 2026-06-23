#ifndef ARP_CACHE_H
#define ARP_CACHE_H

#include <stdint.h>

#define ARP_MAX_CACHE_SIZE        256
#define ARP_MAX_PENDING_PACKETS   32

struct Interface;
struct Packet;

typedef struct ArpCacheEntry {
    uint32_t        ip_addr;              // IPv4 address (network byte order)
    uint8_t         mac_addr[6];          // MAC address
    uint64_t        timestamp;            // last updated time (sim time ms)
    int             valid;                // 1 if entry is valid, 0 if expired/invalid
} ArpCacheEntry;

typedef struct ArpPendingPacket {
    uint32_t          target_ip;   /* Host-order next-hop IP waiting for MAC. */
    uint32_t          src_ip;      /* Host-order IPv4 source for ip_send. */
    uint32_t          dst_ip;      /* Host-order IPv4 destination for ip_send. */
    uint8_t           protocol;    /* IPv4 protocol byte. */
    struct Interface *iface;       /* Borrowed outgoing interface. */
    struct Packet    *payload;     /* Owned queued L4 payload. */
    int               valid;
} ArpPendingPacket;

typedef struct ArpCache {
    ArpCacheEntry     entries[ARP_MAX_CACHE_SIZE];
    int               count;
    ArpPendingPacket  pending[ARP_MAX_PENDING_PACKETS];
    int               pending_count;
} ArpCache;

#endif /* ARP_CACHE_H */
