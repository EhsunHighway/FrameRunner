# Module 01 — Packet Buffer

**Files:** `src/network/packet.c`, `src/network/packet.h`
**Status:** ✅ Implemented (93% line / 80% branch)
**Depends on:** stdlib only

---

## The Problem

A network packet travels through many layers (Ethernet → IP → TCP → app
data). Each layer wants to add a header on send and strip a header on
receive. The naive approach — allocate a fresh buffer per layer and copy
— burns memory and CPU on every hop. We need **one buffer** that grows
backward (prepend) and shrinks forward (strip) without re-allocating.

## The Buffer Trick

```
   buffer:  [ unused headroom .................. payload ........ trailing slack ]
                                        ▲                                  ▲
                                       data                          data + len
                                                                              ▲
                                                                          capacity
```

- `data` floats inside the allocation.
- `packet_prepend(p, hdr, n)` moves `data` **back by n** and copies `hdr` in.
- `packet_strip(p, n)`        moves `data` **forward by n**.
- `len` is the *current* payload length; `capacity` is the *fixed* buffer size.

Build the packet bottom-up, dismantle top-down — zero copies in between.

---

## Header File — `packet.h`

### Struct

```c
typedef struct Packet {
    uint8_t *head;        // original malloc pointer; never moves
    uint8_t *data;        // floating pointer inside buffer
    size_t   len;         // current used length from data
    size_t   capacity;    // payload capacity, excludes headroom
    uint32_t id;          // unique packet ID (for tracing)
    int      layer;       // current OSI layer (1-4) for the renderer
} Packet;
```

### Public API

| Function            | Returns | Purpose                                          |
|---------------------|---------|--------------------------------------------------|
| `packet_create`     | `Packet*` | Allocate packet with `capacity` bytes.        |
| `packet_prepend`    | `int`     | Add `header_len` bytes in front of `data`.    |
| `packet_strip`      | `int`     | Remove `header_len` bytes from the front.     |
| `packet_clone`      | `Packet*` | Deep-copy (used for switch flooding).         |
| `packet_free`       | `void`    | Release packet + buffer.                      |
| `packet_checksum`   | `uint16_t`| One's-complement sum (RFC 1071).              |
| `packet_dump`       | `void`    | Hex-dump to stderr for debugging.             |

### ACSL highlights

```
packet_prepend:
    \result == 0  ⇒  p->len == \old(p->len) + header_len
    \result == -1 ⇒  p->len == \old(p->len)

packet_strip (behavior valid_strip):
    header_len <= p->len  ⇒  result == 0, p->len decreases
packet_strip (behavior overflow):
    header_len > p->len   ⇒  result == -1, p->len unchanged
```

---

## Call Sequence — Outbound (Send)

```
app
 │
 │ packet_create(1500)        ┐
 │ write payload bytes         │ packet starts empty
 │                             │ data points near end of buffer
 │ packet_prepend(p, &tcp, 20) │ data ← data - 20 ; copy header
 │ packet_prepend(p, &ip, 20)  │ data ← data - 20 ; copy header
 │ packet_prepend(p, &eth, 14) │ data ← data - 14 ; copy header
 ▼
link_transmit(p)
```

After three prepends, the same buffer holds `[ETH | IP | TCP | payload]`,
no realloc, no copy.

## Call Sequence — Inbound (Receive)

```
link delivers p
 │
 ▼
ethernet_receive(p) → packet_strip(p, 14)     │ data ← data + 14
 │
 ▼
ip_receive(p)       → packet_strip(p, 20)
 │
 ▼
tcp_receive(p)      → packet_strip(p, 20)
 │
 ▼
deliver payload to socket buffer
```

---

## Design Notes

- **Capacity = MTU + worst-case header stack.** The buffer is sized so
  the deepest prepend never runs out of headroom.
- **`head` is the allocation owner.** Always free `head`, never `data`.
  `data` moves during prepend/strip and may point anywhere between
  `head` and `head + PKT_HEADROOM + capacity`.
- **Stripped headers remain readable.** `packet_strip` advances `data`
  but does not erase bytes. A module can read the just-stripped header at
  `p->data - header_len` after checking that pointer is still at or after
  `p->head`.
- **`packet_clone` is mandatory for L2 flooding** — the switch sends a
  separate copy out each egress port (otherwise the second link would
  free a packet still referenced by the first).
- **Checksum** is the classic one's-complement sum used by IP, ICMP, UDP,
  TCP. Centralised here so every L3/L4 module shares the same
  implementation.
- `id` is a monotonically increasing counter used by the packet
  renderer to follow a single packet through the simulation.

## Implementation Guide

Implement packet functions in this order:

1. `packet_create`: allocate `Packet`, allocate `PKT_HEADROOM + capacity`
   bytes, set `head`, set `data = head + PKT_HEADROOM`, set `len = 0`,
   and assign a fresh `id`.
2. `packet_prepend`: reject when `(size_t)(p->data - p->head) <
   header_len`; otherwise move `data` backward, increase `len`, and copy
   the header into the new front.
3. `packet_strip`: reject when `header_len > p->len`; otherwise move
   `data` forward and decrease `len`. Do not scrub the stripped bytes.
4. `packet_clone`: create a fresh packet with capacity `p->len`, copy
   only the currently valid bytes from `p->data`, copy `layer`, and leave
   the clone with a different `id`.
5. `packet_checksum`: operate over the supplied byte range only. For odd
   lengths, include the final byte as the low-address byte according to
   the current implementation and lock that behavior with tests.

## ACSL Contract Plan

The useful proof surface here is pointer movement and ownership:

```c
/*@ predicate packet_layout{L}(Packet *p) =
      \valid(p) &&
      \valid(p->head + (0 .. PKT_HEADROOM + p->capacity - 1)) &&
      p->head <= p->data &&
      p->data + p->len <= p->head + PKT_HEADROOM + p->capacity;
*/
```

Contract targets:

- `packet_create`: result has `packet_layout(result)`, `len == 0`,
  `data == head + PKT_HEADROOM`, `layer == 0`.
- `packet_prepend`: success moves `data` backward exactly `header_len`
  and increases `len`; failure leaves both unchanged.
- `packet_strip`: success moves `data` forward exactly `header_len` and
  decreases `len`; failure leaves both unchanged.
- `packet_clone`: clone has equal visible bytes, equal `len`, equal
  `layer`, different `id`, and independent storage.
- `packet_free`: accepts `NULL`; frees only storage owned by the packet.
