# Module 10 — ARP Cache

**Files:** `src/protocols/arp_cache.c`, `src/protocols/arp_cache.h`
**Status:** Implemented core cache and pending queue
**Depends on:** interface, packet, ip, simulator

---

## The Problem

Ethernet needs a destination MAC address. IPv4 code usually knows only a
destination IP address. The ARP cache is the per-node memory that answers:

```text
IPv4 next-hop address -> Ethernet MAC address
```

The cache is not the ARP wire protocol. It does not parse ARP packets and it
does not decide whether an ARP request or reply is valid. It only stores learned
IP-to-MAC mappings and packets waiting for a mapping.

Ownership rule:

- Host owns a Host ARP cache.
- Router owns a Router ARP cache.
- Interface borrows a pointer to its owner cache through `iface->arp_cache`.
- ARP updates the borrowed cache when ARP frames arrive.
- IP reads and queues through the borrowed cache when sending packets.

So ARP does **not** own the cache object. ARP is one producer of cache entries.
IP is one consumer of cache entries. Host and Router own the storage.

---

## Why This File Comes Before ARP

`arp.c` calls `arp_cache_add` and `arp_pending_flush` after receiving ARP
frames. `ip.c` calls `arp_cache_lookup` and `arp_pending_enqueue` before
sending IPv4 packets. Both modules need the same shared cache contract.

That makes ARP cache a lower support module than ARP wire handling in this
project, so its specification is placed after Ethernet and before ARP.

Build rule: any compile command, KLEVA YAML, or test binary that calls an
`arp_cache_*` or `arp_pending_*` function must include
`src/protocols/arp_cache.c`. Including `arp_cache.h` only gives declarations.

---

## Header File — `arp_cache.h`

### Constants

| Macro | Value | Meaning |
|---|---:|---|
| `ARP_MAX_CACHE_SIZE` | `256` | Maximum learned IP-to-MAC entries. |
| `ARP_MAX_PENDING_PACKETS` | `32` | Maximum packets waiting for ARP resolution. |

### Cache Entry

```c
typedef struct ArpCacheEntry {
    uint32_t ip_addr;
    uint8_t  mac_addr[6];
    uint64_t timestamp;
    int      valid;
} ArpCacheEntry;
```

Rules:

- `ip_addr` is stored in host byte order in the current implementation.
- `mac_addr` is the resolved six-byte Ethernet address.
- `timestamp` is simulator time from the caller.
- `valid == 1` means the slot is active.
- `valid == 0` means the slot is empty or expired.

### Pending Packet

```c
typedef struct ArpPendingPacket {
    uint32_t   target_ip;
    uint32_t   src_ip;
    uint32_t   dst_ip;
    uint8_t    protocol;
    Interface *iface;
    Packet    *payload;
    int        valid;
} ArpPendingPacket;
```

Rules:

- `target_ip` is the next-hop IP whose MAC address is needed.
- `src_ip`, `dst_ip`, and `protocol` are the values needed later by `ip_send`.
- `iface` is borrowed. The ARP cache does not own or free it.
- `payload` is owned by the pending queue after a successful enqueue.
- `valid == 1` means the pending slot owns a payload.

### Cache Object

```c
typedef struct ArpCache {
    ArpCacheEntry    entries[256];
    int              count;
    ArpPendingPacket pending[32];
    int              pending_count;
} ArpCache;
```

This object may be heap-owned by Host or embedded by Router. The cache module
does not allocate or free the object itself.

---

## Initial State

`arp_cache_init` defines what "empty ARP cache" means. It does **not** mean a
NULL pointer. NULL means missing storage.

After `arp_cache_init(cache)` on a valid cache:

- `cache->count == 0`
- every `cache->entries[i].valid == 0` for `0 <= i < 256`
- `cache->pending_count == 0`
- every `cache->pending[i].valid == 0` for `0 <= i < 32`
- every `cache->pending[i].iface == NULL` for `0 <= i < 32`
- every `cache->pending[i].payload == NULL` for `0 <= i < 32`

Current implementation reaches this state by clearing the whole object with
`memset(cache, 0, sizeof(*cache))`.

`arp_cache_init(NULL)` is a no-op for caller convenience.

---

## Public API

| Function | Purpose |
|---|---|
| `arp_cache_init(cache)` | Put an existing cache object into the empty state. |
| `arp_cache_add(cache, ip, mac, ts)` | Insert or refresh one IP-to-MAC mapping. |
| `arp_cache_lookup(cache, ip, out_mac)` | Copy a MAC address for a valid mapping. |
| `arp_cache_cleanup(cache, now)` | Expire old valid entries. |
| `arp_pending_enqueue(cache, iface, target_ip, src_ip, dst_ip, proto, payload)` | Queue one payload while MAC resolution is pending. |
| `arp_pending_flush(sim, cache, target_ip, mac)` | Send or free queued payloads after a MAC is learned. |

---

## Function Behavior

### `arp_cache_init`

```text
if cache == NULL:
    return

clear the whole ArpCache object
```

This function is the only public initializer for ARP cache state. Host and
Router creation code should call it instead of hand-setting fields.

### `arp_cache_add`

```text
if cache == NULL or ip == 0:
    return

scan entries[0..255] for a valid entry with the same IP
    if found:
        replace MAC
        update timestamp
        return

scan entries[0..255] for the first invalid entry
    if found:
        write IP, MAC, timestamp
        set valid = 1
        increment count
        return

if no free slot exists:
    leave cache unchanged
```

Refreshing an existing entry does not increment `count`.

### `arp_cache_lookup`

```text
if cache == NULL or ip == 0 or out_mac == NULL:
    return -1

scan entries[0..255]
    if valid entry with same IP exists:
        copy six MAC bytes into out_mac
        return 0

return -1
```

The lookup function does not send ARP requests. It only reads the cache.

### `arp_cache_cleanup`

```text
if cache == NULL:
    return

scan entries[0..255]
    if entry is valid and now - timestamp >= timeout:
        set valid = 0
        decrement count
```

Cleanup does not touch pending packets.

### `arp_pending_enqueue`

```text
if cache == NULL or iface == NULL or target_ip == 0 or payload == NULL:
    return -1

scan pending[0..31] for the first invalid slot
    if found:
        store target_ip, src_ip, dst_ip, protocol, iface, payload
        set valid = 1
        increment pending_count
        return 0

return -1
```

Ownership:

- On return `0`, the pending queue owns `payload`.
- On return `-1`, the caller still owns `payload`.
- The pending queue never owns `iface`.

### `arp_pending_flush`

```text
if sim == NULL or cache == NULL or target_ip == 0 or mac == NULL:
    return 0

sent = 0
for each pending slot:
    if slot is valid and slot.target_ip == target_ip:
        copy queued metadata into locals
        clear valid, payload, and iface
        decrement pending_count if positive

        if iface and payload and ip_send(...) succeeds:
            sent++
        else:
            packet_free(payload)

return sent
```

Flush is where the ARP cache depends on IP. The cache module does not build an
IP header itself; it calls `ip_send` using the metadata saved by enqueue.

---

## Call Flow

### IP Send Miss

```text
ip_output(sim, src_ip, dst_ip, proto, payload)
  |
  +-- arp_cache_lookup(iface->arp_cache, dst_ip, dst_mac)
        |
        +-- miss
             |
             +-- arp_send_request(sim, iface, ns_htonl(dst_ip))
             +-- arp_pending_enqueue(iface->arp_cache, iface,
                                     dst_ip, src_ip, dst_ip,
                                     proto, payload)
```

After successful enqueue, `payload` must not be freed by `ip_output`.

### ARP Reply Resolves Pending Packets

```text
arp_reply_handler(e, sim)
  |
  +-- sender_ip  = ntohl(arp.sender_protocol_addr)
  +-- sender_mac = arp.sender_hardware_addr
  |
  +-- arp_cache_add(iface->arp_cache, sender_ip, sender_mac, e->timestamp)
  +-- arp_pending_flush(sim, iface->arp_cache, sender_ip, sender_mac)
        |
        +-- ip_send(sim, queued_iface, sender_mac,
                    queued_src_ip, queued_dst_ip,
                    queued_protocol, queued_payload)
```

ARP triggers the flush, but the pending queue is part of the ARP cache module.

---

## Design Notes

- The cache is per network node in current Host/Router designs.
- Interface only stores a borrowed cache pointer.
- ARP does not allocate, own, or free the cache object.
- IP does not inspect ARP packets.
- ARP cache does not inspect ARP packets.
- Pending queue entries store enough metadata to call `ip_send` later.
- Cache operations use linear scans. The bounded sizes are small and friendly
  to KLEVA/EVA.

---

## ACSL Contract Plan

Use literal bounds in ACSL contracts because KLEVA/EVA may not preserve macro
names in generated contexts:

- cache entries: `0 .. 256-1`
- pending entries: `0 .. 32-1`
- MAC bytes: `0 .. 5`

Required contracts:

- `arp_cache_init`: NULL behavior assigns nothing. Valid behavior assigns the
  full cache object and ensures counts are zero, valid bits are zero, and
  pending `iface`/`payload` pointers are NULL.
- `arp_cache_add`: NULL/zero-IP behavior assigns nothing. Existing-entry
  behavior updates MAC/timestamp without changing count. Insert behavior fills
  one invalid slot and increments count.
- `arp_cache_lookup`: NULL/zero-IP/output-NULL returns `-1`. Hit copies all six
  MAC bytes and returns `0`. Miss returns `-1`.
- `arp_pending_enqueue`: NULL/zero-target behavior returns `-1`. Success writes
  one pending slot and increments `pending_count`.
- `arp_pending_flush`: NULL behavior returns `0`. Valid behavior clears every
  matching pending slot, decrements `pending_count` for each cleared slot, and
  returns the number of successful `ip_send` handoffs.
- `arp_cache_cleanup`: NULL behavior assigns nothing. Valid behavior can only
  clear `entries[i].valid` and decrement `count`.

KLEVA tests should include both normal cache behavior and stale-pointer
initialization behavior: dirty `pending[i].iface` and `pending[i].payload`,
call `arp_cache_init`, and assert both are NULL.
