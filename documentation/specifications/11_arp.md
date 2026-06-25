# Module 11 — ARP (Address Resolution Protocol)

**Files:** `src/protocols/arp.c`, `src/protocols/arp.h`
**Status:** Implemented core ARP exchange
**Depends on:** ethernet, interface, device, packet, arp_cache, simulator, scheduler

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

## Module Boundary

`arp.c` / `arp.h` own ARP wire behavior: simulator event registration, ARP
request handling, ARP reply handling, and request/reply packet construction.

ARP does **not** own the ARP cache object. Host and Router own cache storage,
Interface borrows a pointer to that cache, and ARP updates the borrowed cache
when ARP frames arrive. The cache API and pending-packet rules are specified in
[10_arp_cache.md](10_arp_cache.md).

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
flush pending packets through the cache API without globals. It is **not** the
ARP cache owner.

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
              │  arp_pending_flush(sim, iface->arp_cache,
              │                    SPA, SHA)
              │  iface->last_rx_time = e->timestamp
              └─►  (caller of arp_send_request can now lookup MAC)
```

## Design Notes

- **Scheduler handler is protocol dispatch, not ARP ownership.** There is one
  fallback handler for ARP request and one for ARP reply. Multiple devices can
  still resolve addresses independently because the event carries the receiving
  interface, and that interface carries the cache pointer.
- **Cache ownership is external to ARP.** ARP never frees the cache; it
  only mutates entries through the borrowed pointer and the ARP cache API.
- **Param rename:** `arp_send_reply` uses `req_pkt` (not `pkt`) so the
  ACSL annotation can refer to it without `unbound variable` errors.
- **Return-code trap:** current `link_transmit` returns `1` for scheduled
  success, so `ethernet_send` can return `1` on a good send. ARP send
  helpers should treat non-negative Ethernet return values as send
  success, or the lower-layer convention should be normalized.

## Implementation Guide

1. `arp_init`: register `EVT_ARP_REQUEST` and `EVT_ARP_REPLY` with the
   scheduler, using `sim` as handler context.
2. `arp_send_request`: allocate packet and ARP payload; fill request in
   network byte order; prepend ARP bytes; send through Ethernet broadcast.
3. `arp_send_reply`: validate request length before reading; copy sender
   MAC/IP from request; fill reply; unicast to request sender.
4. Request/reply handlers should free or transfer packet ownership
   deliberately. If the current code keeps packets alive, tests should
   expose and settle that ownership rule.

## ACSL Contract Plan

- `arp_send_request` / `arp_send_reply`: NULL behaviors return `-1`;
  valid send behaviors should specify packet construction fields and
  queue/counter effects through Ethernet.
- Request/reply handlers: valid ARP request/reply reception updates
  `iface->arp_cache` through `arp_cache_add`; reply handling also calls
  `arp_pending_flush`.
