# Module 10 — ARP (Address Resolution Protocol)

**Files:** `src/protocols/arp.c`, `src/protocols/arp.h`, `src/protocols/arp_cache.h`
**Status:** Implemented core ARP exchange; pending queue added for IP output
**Depends on:** ethernet, device, packet, simulator, scheduler

---

## The Problem

IP works at Layer 3. Ethernet works at Layer 2. When Host A wants to send an
IP packet to Host B on the **same subnet**, it knows the destination IP —
but `ethernet_send` requires a **destination MAC**. ARP bridges that gap.

```
Host A knows:  192.168.1.10              (IP of Host B)
Host A needs:  ??:??:??:??:??:??         (MAC of Host B)
```

## The Two-Phase Exchange

```
Host A (192.168.1.1)                          Host B (192.168.1.10)
AA:AA:AA:AA:AA:AA                             BB:BB:BB:BB:BB:BB
        |                                              |
        |──── WHO HAS 192.168.1.10? ─────────────────►|   (broadcast)
        |     ETH dst: FF:FF:FF:FF:FF:FF               |
        |     ARP opcode: REQUEST                      |
        |     sender_hw:  AA:AA:AA:AA:AA:AA            |
        |     sender_ip:  192.168.1.1                  |
        |     target_hw:  00:00:00:00:00:00            |
        |     target_ip:  192.168.1.10                 |
        |                                              |
        |                            [cache adds A → AA:AA:AA] ← REQ handler
        |                                              |
        |◄─── I HAVE IT. I AM BB:BB:BB:BB:BB:BB ──────|   (unicast)
        |     ETH dst: AA:AA:AA:AA:AA:AA               |
        |     ARP opcode: REPLY                        |
        |     sender_hw:  BB:BB:BB:BB:BB:BB            |
        |     sender_ip:  192.168.1.10                 |
        |     target_hw:  AA:AA:AA:AA:AA:AA            |
        |     target_ip:  192.168.1.1                  |
        |                                              |
   [cache adds B → BB:BB:BB] ← REPLY handler           |
        |                                              |
        |════ now sends real IP packet ═══════════════►|   (unicast)
```

Notice **both sides learn**: the requester learns B from the reply, and the
responder learns A "for free" from the incoming request — no extra round trip.

---

## Header File — `arp.h`

### Constants

| Macro                    | Value      | Meaning                                |
|--------------------------|------------|----------------------------------------|
| `HARDWARE_TYPE_ETHERNET` | `1`        | Hardware type field (RFC 826)          |
| `PROTOCOL_TYPE_IPV4`     | `0x0800`   | Same value as `ETHERTYPE_IPV4`         |
| `HARDWARE_ADDR_LEN`      | `6`        | MAC is 6 bytes                         |
| `PROTOCOL_ADDR_LEN`      | `4`        | IPv4 is 4 bytes                        |
| `ARP_OPCODE_REQUEST`     | `1`        | "Who has X?"                           |
| `ARP_OPCODE_REPLY`       | `2`        | "It's me"                              |
| `ARP_CACHE_TIMEOUT_MS`   | `300_000`  | 5-minute cache aging                   |

### Wire Format — `ArpPacket` (28 bytes, packed)

```
  offset   field                       size
  ──────   ─────                       ────
    0      hardware_type               2     (htons → 1)
    2      protocol_type               2     (htons → 0x0800)
    4      hardware_addr_len           1     (= 6)
    5      protocol_addr_len           1     (= 4)
    6      opcode                      2     (htons → 1 or 2)
    8      sender_hardware_addr[6]     6     (SHA)
   14      sender_protocol_addr        4     (SPA — network order)
   18      target_hardware_addr[6]     6     (THA)
   24      target_protocol_addr        4     (TPA — network order)
   28
```

```c
typedef struct __attribute__((packed)) ArpPacket {
    uint16_t  hardware_type;
    uint16_t  protocol_type;
    uint8_t   hardware_addr_len;
    uint8_t   protocol_addr_len;
    uint16_t  opcode;
    uint8_t   sender_hardware_addr[6];
    uint32_t  sender_protocol_addr;
    uint8_t   target_hardware_addr[6];
    uint32_t  target_protocol_addr;
} ArpPacket;
```

`__attribute__((packed))` is mandatory: without it the compiler would
insert padding and the struct would no longer be 28 bytes on the wire.

- **REQUEST:** THA = `00:00:00:00:00:00` (unknown — that's why we asked).
- **REPLY:**   THA = SHA copied from the request (we know exactly who asked).

### Cache Structures — `arp_cache.h`

Split into its own header so interfaces can carry a borrowed cache
pointer without making `interface.h` include ARP internals.

```
  ArpCache                                       (4 KB per Device)
  ┌─────────────────────────────────────────────┐
  │ entries[0 .. 255]                           │  256 × 16 bytes
  │  ┌─────────────────────────────────────┐    │
  │  │ ip_addr   : uint32_t  (4)           │    │
  │  │ mac_addr  : uint8_t[6](6)           │    │
  │  │ timestamp : uint64_t  (8)           │    │
  │  │ valid     : int       (4)           │    │
  │  └─────────────────────────────────────┘    │
  │ count : int   (active valid entries)        │
  └─────────────────────────────────────────────┘
```

`ArpCache` is reached through `Interface.arp_cache`, which is a borrowed
pointer. Setup code must create/own the cache elsewhere and call
`interface_set_arp_cache(iface, cache)` before ARP handlers can learn, lookup,
or queue packets.

This is the ARP ownership rule:

```
incoming ARP frame
  └─► Event.dst_device
        └─► Interface *iface
              └─► iface->arp_cache
```

The scheduler does not own ARP state. It only dispatches the ARP protocol
handler. The handler chooses the correct cache from the interface that received
the frame. This lets several hosts, routers, or future L3 switch interfaces
resolve addresses independently in the same simulator.

The cache also owns a small pending queue for packets waiting on ARP
resolution:

```
  ArpPendingPacket
  ┌─────────────────────────────────────┐
  │ valid      : int                     │
  │ target_ip  : uint32_t host order     │  IP whose MAC is needed
  │ src_ip     : uint32_t host order     │  original IP sender
  │ dst_ip     : uint32_t host order     │  final IP destination
  │ protocol   : uint8_t                 │  IPv4 protocol byte
  │ iface      : Interface * borrowed    │  outgoing interface
  │ payload    : Packet * owned          │  L4 payload awaiting send
  └─────────────────────────────────────┘
```

Pending queue ownership:

- `arp_pending_enqueue` takes ownership of `payload` on success.
- If enqueue fails, the caller still owns `payload`.
- `arp_pending_flush` sends or frees queued payloads for the resolved IP.
- ARP does not own `iface`; it only borrows the outgoing interface pointer.

---

## Dispatch Table — How handlers are reached

`arp_init` registers one protocol-level handler for each ARP event type:

```
  Scheduler.handlers[EVT_TYPE_COUNT]
  ┌───────────────────────┬──────────────────────┬───────┐
  │ EVT_ARP_REQUEST       │ arp_request_handler  │ sim   │ ← arp_init()
  │ EVT_ARP_REPLY         │ arp_reply_handler    │ sim   │ ← arp_init()
  │ EVT_ROUTING_UPDATE    │ ...                  │       │
  │ ...                                                  │
  └──────────────────────────────────────────────────────┘
```

`EVT_PACKET_RECEIVE` is not owned by ARP and is not globally owned by
Ethernet. Link delivery events carry `ethernet_receive_event` as their
per-event callback. After Ethernet strips the frame, the interface receive
handler or Ethernet demultiplexing path decides whether the payload becomes
ARP, IP, or another L3 protocol.

The `ctx` slot carries `Simulator *sim` so handlers can schedule replies or
flush pending packets without globals. It is **not** the ARP cache owner.

The cache owner is selected at event time:

- ARP request received on interface `B0` updates `B0->arp_cache`.
- ARP reply received on interface `A0` updates `A0->arp_cache`.
- If a host/router has multiple interfaces, each interface may point to a
  different cache or to a shared owner cache, depending on the device model.
- A pure Layer-2 switch does not need ARP for forwarding. If a future switch
  has a management IP or L3 interface, that logical interface should have an
  ARP cache just like a host/router interface.

---

## Function Call Sequence — REQUEST path

```
Caller
  │
  └─► arp_send_request(sim, iface, target_ip)
          │  malloc ArpPacket
          │  fill: opcode = REQUEST, SHA = iface->mac,
          │        SPA = iface->ip_addr,
          │        THA = 00:00:00:00:00:00, TPA = target_ip
          │  packet_create(28) + packet_prepend()
          │
          └─► ethernet_send(sim, iface, ETH_BROADCAST,
                            ETHERTYPE_ARP, pkt)
                  │  prepend EthernetHeader (dst = FF:FF:FF:FF:FF:FF)
                  │
                  └─► link_transmit(...)
                          │
                          └─► scheduler_schedule(EVT_PACKET_RECEIVE
                                                  with ethernet callback)
                                    │
                            [sim advances time]
                                    │
                            scheduler calls event's ethernet callback
                                    ▼
                        ethernet_receive_event(e, sim)
                            │  ethernet_receive → strips 14 bytes,
                            │     out_ethertype = 0x0806
                            │
                            (in this simulator, the receive path that
                             converts ETHERTYPE_ARP → EVT_ARP_REQUEST
                             is implemented by injecting an
                             EVT_ARP_REQUEST event; the demultiplex
                             could equally well call arp directly via
                             iface->rx_handler)
                                    ▼
                        arp_request_handler(e, ctx = sim)
                            │  iface = (Interface *) e->dst_device
                            │  pkt   = (Packet *)    e->packet
                            │  if (ntohs(opcode) != REQUEST)           ⇒ ignore
                            │  if (ntohl(TPA) != ntohl(iface->ip))     ⇒ ignore
                            │  arp_send_reply(sim, iface, pkt) ─────┐
                            │  arp_cache_add(iface->arp_cache,
                            │                SPA, SHA, e->timestamp) │
                            │  iface->last_tx_time = e->timestamp    │
                            └───────────────────────────────────────►┘
```

## Function Call Sequence — REPLY path

```
arp_send_reply(sim, iface, req_pkt)
  │  req = (ArpPacket *) req_pkt->data
  │  dst_mac = req->sender_hardware_addr     ← unicast back to requester
  │  dst_ip  = req->sender_protocol_addr
  │  fill new ArpPacket:
  │      opcode = REPLY
  │      SHA    = iface->mac                 (I am the answer)
  │      SPA    = iface->ip_addr
  │      THA    = dst_mac
  │      TPA    = dst_ip
  │  packet_create(28) + packet_prepend()
  │
  └─► ethernet_send(sim, iface, dst_mac, ETHERTYPE_ARP, reply_pkt)
          │
          └─► link_transmit → scheduler_schedule(EVT_PACKET_RECEIVE
                                                  with ethernet callback)
                    │
              [time advances]
                    ▼
          ethernet_receive_event → EVT_ARP_REPLY dispatched
                    ▼
          arp_reply_handler(e, ctx unused)
              │  if (ntohs(opcode) != REPLY) ⇒ ignore
              │  arp_cache_add(iface->arp_cache,
              │                SPA, SHA, e->timestamp)
              │  arp_pending_flush(iface->arp_cache,
              │                    SPA, SHA)
              │  iface->last_rx_time = e->timestamp
              └─►  (caller of arp_send_request can now lookup MAC)
```

## Function Call Sequence — IP packet waits for ARP

`ip_output` handles direct-subnet destinations before the routing table module
exists:

```
ip_output(sim, src_ip, dst_ip, proto, payload)
  │
  ├─ find source interface by src_ip
  ├─ ensure dst_ip is on that interface's subnet
  ├─ arp_cache_lookup(iface->arp_cache, dst_ip, dst_mac)
  │
  ├─ HIT:
  │    └─ ip_send(sim, iface, dst_mac, src_ip, dst_ip, proto, payload)
  │
  └─ MISS:
       ├─ arp_send_request(sim, iface, ns_htonl(dst_ip))
       ├─ arp_pending_enqueue(cache, iface, dst_ip, src_ip, dst_ip,
       │                       proto, payload)
       └─ return 0 because ARP now owns the queued payload
```

Later, when an ARP reply arrives:

```
arp_reply_handler
  ├─ learn sender_protocol_addr -> sender_hardware_addr
  └─ arp_pending_flush(cache, sender_ip, sender_mac)
       └─ for each matching queued payload:
            ip_send(sim, queued_iface, sender_mac,
                    queued_src_ip, queued_dst_ip,
                    queued_protocol, queued_payload)
```

This is still not routing. The pending queue only handles direct-neighbor ARP
misses. Non-local destinations still require the future route table/default
gateway module.

## Cache Operations — Quick reference

```
arp_cache_add(cache, ip, mac, ts):
    if cache==NULL or ip==0: return
    scan 0..255 for existing entry with same ip
        if found:  memcpy new mac, update ts, return
    scan 0..255 for first invalid slot
        if found:  fill ip/mac/ts, valid=1, count++

arp_cache_lookup(cache, ip, out_mac):
    if cache==NULL or ip==0 or out_mac==NULL: return -1
    scan 0..255 for valid entry with same ip
        if found: memcpy mac, return 0
    return -1

arp_cache_cleanup(cache, now):
    if cache==NULL: return
    scan 0..255:
        if valid and now - ts >= 300_000: valid=0, count--

arp_pending_enqueue(cache, iface, target_ip, src_ip, dst_ip, proto, payload):
    if any required pointer is NULL or target_ip is zero: return -1
    scan pending queue for first invalid slot
        if found: store fields, mark valid, increment pending_count, return 0
    return -1

arp_pending_flush(sim, cache, target_ip, mac):
    if any required pointer is NULL or target_ip is zero: return 0
    scan pending queue
        for each valid entry matching target_ip:
            call ip_send with queued metadata and resolved mac
            clear entry and decrement pending_count
    return number of queued packets successfully handed to IP
```

---

## Design Notes

- **Cache pointer use:** handlers receive a bare `Interface *` via
  `e->dst_device`. They reach the cache through `iface->arp_cache` if it
  has been configured.
- **Scheduler handler is protocol dispatch, not ARP ownership.** There is one
  fallback handler for ARP request and one for ARP reply. Multiple devices can
  still resolve addresses independently because the event carries the receiving
  interface, and that interface carries the cache pointer.
- **Cache ownership is external to ARP.** ARP never frees the cache; it
  only mutates entries through the borrowed pointer.
- **Pending payload ownership:** once a payload is queued, ARP owns it.
  When the matching MAC is learned, ARP either hands the packet to IP/Ethernet
  through `ip_send` or frees it on send failure.
- **Linear scan (256):** more than enough for any sane LAN; a hash table
  is unnecessary at this scale.
- **ACSL literals:** `ARP_MAX_CACHE_SIZE` is a `#define`, invisible to
  EVA. The contracts use the literal `255` / `256` instead.
- **Param rename:** `arp_send_reply` uses `req_pkt` (not `pkt`) so the
  ACSL annotation can refer to it without `unbound variable` errors.
- **Return-code trap:** current `link_transmit` returns `1` for scheduled
  success, so `ethernet_send` can return `1` on a good send. ARP send
  helpers should treat non-negative Ethernet return values as send
  success, or the lower-layer convention should be normalized.

## Implementation Guide

1. `arp_init`: register `EVT_ARP_REQUEST` and `EVT_ARP_REPLY` with the
   scheduler, using `sim` as handler context.
2. `arp_cache_add`: reject NULL cache or zero IP. Refresh an existing
   valid entry first; otherwise fill the first invalid slot and increment
   `count`.
3. `arp_cache_lookup`: reject NULLs/zero IP; copy the MAC on hit; return
   `-1` on miss.
4. `arp_cache_cleanup`: invalidate entries whose age is at least
   `ARP_CACHE_TIMEOUT_MS` and decrement `count`.
5. `arp_pending_enqueue`: store unresolved IP payload metadata in the first
   free pending slot. Enqueue success transfers packet ownership to ARP.
6. `arp_pending_flush`: when a MAC is learned, send every queued packet whose
   `target_ip` matches the learned sender IP.
7. `arp_send_request`: allocate packet and ARP payload; fill request in
   network byte order; prepend ARP bytes; send through Ethernet broadcast.
8. `arp_send_reply`: validate request length before reading; copy sender
   MAC/IP from request; fill reply; unicast to request sender.
9. Request/reply handlers should free or transfer packet ownership
   deliberately. If the current code keeps packets alive, tests should
   expose and settle that ownership rule.

## ACSL Contract Plan

- Cache predicates: define `arp_cache_count_matches(cache)` so `count`
  equals the number of valid entries.
- `arp_cache_add`: existing-entry behavior leaves `count` unchanged;
  insert behavior increments `count`; full-table behavior leaves count
  unchanged.
- `arp_cache_lookup`: hit copies all six MAC bytes; miss leaves output
  unconstrained unless the contract says it is unchanged.
- `arp_send_request` / `arp_send_reply`: NULL behaviors return `-1`;
  valid send behaviors should specify packet construction fields and
  queue/counter effects through Ethernet.
