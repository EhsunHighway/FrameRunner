# Module 09 — Ethernet (L2)

**Files:** `src/protocols/ethernet.c`, `src/protocols/ethernet.h`
**Status:** ✅ Implemented (57% line / 65% branch — 21 EVA-proven)
**Depends on:** packet, interface, simulator, scheduler, link

---

## The Problem

Bytes travel on the wire wrapped in a fixed 14-byte **Ethernet header**
that says *who sent it*, *who it's for*, and *what's inside*. The
Ethernet module:

1. **Sends:** prepends the header, hands the frame to the link.
2. **Receives:** validates the destination MAC (must be mine or
   broadcast), strips the header, tells the caller what the upper-layer
   protocol is (via `ethertype`), then calls the per-interface
   `rx_handler` to deliver upward.

## Frame Layout — `EthernetHeader` (14 bytes, packed)

```
   offset   field           size
   ──────   ─────           ────
     0      dst_mac[6]       6        destination MAC
     6      src_mac[6]       6        source MAC
    12      ethertype        2        network byte order
    14
```

```c
typedef struct __attribute__((packed)) EthernetHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;    // htons on the wire
} EthernetHeader;
```

`packed` is mandatory — without it the compiler may align `ethertype`
and the struct would be 16 bytes, breaking interop.

### Constants

| Macro              | Value       | Meaning                              |
|--------------------|-------------|--------------------------------------|
| `ETH_HDR_LEN`      | `14`        | Header size                          |
| `ETH_ALEN`         | `6`         | MAC address length                   |
| `ETHERTYPE_IPV4`   | `0x0800`    | IPv4 payload                         |
| `ETHERTYPE_ARP`    | `0x0806`    | ARP payload                          |
| `ETHERTYPE_IPV6`   | `0x86DD`    | IPv6 payload (reserved, not used)    |
| `ETH_BROADCAST[6]` | `FF FF FF FF FF FF` (extern const) | all-stations broadcast |

---

## Header File — `ethernet.h`

### Public API

| Function              | Purpose                                          |
|-----------------------|--------------------------------------------------|
| `ethernet_send(sim, iface, dst_mac, ethertype, payload)` | Build frame, transmit. |
| `ethernet_receive(iface, frame, &out_ethertype)`        | Strip header, return ethertype. |

### Return codes for `ethernet_receive`

| return | meaning                                       |
|--------|-----------------------------------------------|
| `0`    | Frame accepted; `frame` is now payload-only   |
| `1`    | Dropped (dst MAC not mine and not broadcast)  |
| `-1`   | Bad input or strip failure                    |

### ACSL highlight

```
ethernet_send (valid):
    result == 0 ⇒ payload->len == \old(payload->len) + ETH_HDR_LEN

ethernet_receive (drop):
    when dst_mac[0] != iface->mac[0] && dst_mac[0] != ETH_BROADCAST[0]
    ⇒ result == 1

ethernet_receive (valid):
    result == 0 ⇒
        frame->len == \old(frame->len) - ETH_HDR_LEN
        *out_ethertype == (\old(frame->data[12]) << 8 | \old(frame->data[13]))
```

---

## Receive Dispatch — How Ethernet plugs into the scheduler

```
ethernet_send
  └─► link_transmit(..., ethernet_receive_event, sim)
        └─► event_create_callback(EVT_PACKET_RECEIVE,
                                  arrival_time,
                                  src_iface,
                                  dst_iface,
                                  cloned_frame,
                                  NULL,
                                  ethernet_receive_event,
                                  sim)
```

Ethernet receive is no longer installed as a single global fallback handler.
The link creates each packet-arrival event with its own callback. That keeps
packet delivery local to the event and avoids one global
`EVT_PACKET_RECEIVE` owner.

`ethernet_receive_event` still performs the same demultiplexing step after
the Ethernet header is accepted: it calls the receiving interface's
`rx_handler`.

---

## Function Call Sequence — `ethernet_send`

```
Caller (ARP / IP / test)
   │
   └─► ethernet_send(sim, iface, dst_mac, ethertype, payload)
           │
           │   if !sim || !iface || !dst_mac || !payload   ⇒ -1
           │   if iface->state == IFACE_ERR_DISABLED        ⇒ -1
           │
           │   malloc EthernetHeader (14 bytes)
           │     dst_mac     = dst_mac (copied)
           │     src_mac     = iface->mac
           │     ethertype   = htons(ethertype)
           │
           ├─► packet_prepend(payload, &eth_hdr, 14)
           │       (data ← data - 14, copy header in)
           │
           │   free(eth_hdr)              (header copied into pkt)
           │   iface->tx_bytes += payload->len
           │   iface->last_tx_time = simulator_now(sim)
           │   payload->layer = 2
           │
           └─► link_transmit(iface->link, payload,
                             iface, sim->sched,
                             simulator_now(sim),
                             ethernet_receive_event, sim)
                   │
                   └─► scheduler_schedule(event with per-event callback)
```

## Function Call Sequence — Receive path

```
[time advances; scheduler pops EVT_PACKET_RECEIVE]
   │
   │   event carries handler = ethernet_receive_event
   │
   ▼
ethernet_receive_event(e, ctx unused):
   │   iface = (Interface *) e->dst_device
   │   frame = (Packet *)    e->packet
   │
   ├─► ethernet_receive(iface, frame, &ethertype)
   │       │
   │       │   if !iface || !frame || frame->len < 14 ⇒ -1
   │       │   if iface->state == ERR_DISABLED         ⇒ -1
   │       │
   │       │   eth_hdr = (EthernetHeader *) frame->data
   │       │   if dst_mac != iface->mac && dst_mac != FF:..:FF ⇒ 1 (drop)
   │       │
   │       │   *out_ethertype = ntohs(eth_hdr->ethertype)
   │       │   packet_strip(frame, 14)                     (data += 14)
   │       │   iface->rx_bytes += frame->len
   │       │   frame->layer = 3
   │       ▼
   │      return 0
   │
   │   if result == 0:
   │       iface->last_rx_time = e->timestamp
   │       iface->rx_handler(iface, frame, ethertype, iface->handler_ctx)
   │           │
   │           └─► ip_receive / arp_receive / ...
   │   elif result == 1: iface->rx_dropped++
   │   else:             iface->rx_errors++; last_error_time = …
```

---

## Design Notes

- **No `Device *` cast in the receive path.** The handler only uses
  `Interface *` (which is what `e->dst_device` actually holds when the
  link scheduled the event). No back-pointer needed at this layer.
- **Multicast and 802.1Q VLAN are out of scope** for this milestone.
  Adding them would require checking the group bit and parsing the
  optional 4-byte tag.
- **Header is currently `malloc`-then-`free`.** A stack header would also
  be safe because `packet_prepend` copies bytes immediately, but the
  implementation currently allocates the temporary header.
- **`iface->rx_handler` is set by whoever owns the upper layer.** This
  is the seam where IP plugs in: `interface_set_rx_handler(iface,
  ip_receive, sim)`.
- **Stripped Ethernet headers remain readable.** After
  `ethernet_receive` succeeds, `frame->data` points at the L3 payload and
  the old Ethernet header is at `frame->data - ETH_HDR_LEN` if that is
  still at or after `frame->head`. Switch and ICMP use this fact.

## Implementation Guide

1. `ethernet_send`: reject NULLs and `IFACE_ERR_DISABLED`; build header
   with copied destination MAC, source MAC from `iface->mac`, and
   network-order EtherType; prepend; update tx counters/timestamp/layer;
   call `link_transmit`.
2. `ethernet_receive`: reject NULLs, too-short frames, and
   error-disabled interfaces; accept only destination MAC equal to
   `iface->mac` or broadcast; convert EtherType to host order; strip 14
   bytes; update rx bytes and layer.
3. `ethernet_receive_event`: pull `Interface *` from `e->dst_device`
   and `Packet *` from `e->packet`; on accepted frame, call
   `iface->rx_handler` if present; on drop/error, update counters.

## ACSL Contract Plan

- `ethernet_send`: success increases packet length by `ETH_HDR_LEN`
  before the link receives it, sets layer to `2`, and increments
  `iface->tx_bytes` by the framed length.
- `ethernet_receive`: accepted frame decreases length by `ETH_HDR_LEN`,
  sets `*out_ethertype` from the stripped header, and sets layer to `3`.
- Drop behavior should compare all six MAC bytes, not only byte zero.
- Error-disabled send/receive should be explicit failure behaviors.
