# Module 14 - ICMP

**Files:** `src/protocols/icmp.c`, `src/protocols/icmp.h`  
**Depends on:** ip, packet, interface, simulator, byte_order

---

## Purpose

ICMP is IPv4's control-message protocol. In this simulator it provides:

1. Echo Request and Echo Reply for reachability tests.
2. Destination Unreachable messages for network, host, protocol, port, and
   fragmentation errors.
3. Time Exceeded messages for expired TTL.

ICMP is an IPv4 payload protocol. It does not register scheduler handlers and
it does not choose Ethernet destination MAC addresses. Ethernet receives
frames, IP validates and strips IPv4 headers, and the bound `IpStack`
dispatches protocol `IPPROTO_ICMP` to `icmp_receive`.

ICMP must be registered as an IP upper-layer protocol by the stack owner:

```c
ip_stack_register_protocol(&ip_stack, IPPROTO_ICMP, icmp_receive, sim);
```

`ip.c` must not include `icmp.h` or call `icmp_receive` directly. IP owns
IPv4 validation and protocol-number demux; ICMP owns ICMP message parsing and
control-message generation.

---

## Design Contract

This file is the implementation contract for the ICMP module. It describes
what the implementation must do, without embedding a full C implementation.

An implementer should be able to answer these questions from this document:

- What bytes are present in an ICMP message?
- What is the packet shape at function entry?
- Which fields must be saved before a packet pointer moves?
- Who owns each `Packet *`?
- Which counters change on each failure path?
- Which lower-layer function is called to transmit a reply or error?
- Which cases must be covered by tests?

---

## Wire Format

All multi-byte ICMP fields are in network byte order.

Every ICMP message starts with an 8-byte common header:

```text
offset  size  field
0       1     type
1       1     code
2       2     checksum
4       2     id_or_unused
6       2     seq_or_mtu
8       N     body
```

Use this packed representation:

```c
typedef struct __attribute__((packed)) IcmpHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} IcmpHeader;
```

For Echo Request and Echo Reply:

- `id` is the echo identifier.
- `seq` is the echo sequence number.
- The body is arbitrary echo payload.

For Destination Unreachable and Time Exceeded:

- `id` is the high 16 bits of the ICMP "rest of header" field.
- `seq` is the low 16 bits of the ICMP "rest of header" field.
- For most errors both are zero.
- For Fragmentation Needed, `id` is zero and `seq` is the next-hop MTU.
- The body quotes the original IPv4 header plus the first 8 bytes of original
  IPv4 payload.

---

## Constants

| Macro | Value | Meaning |
|---|---:|---|
| `ICMP_ECHO_REPLY` | `0` | Echo Reply type. |
| `ICMP_DEST_UNREACH` | `3` | Destination Unreachable type. |
| `ICMP_ECHO_REQUEST` | `8` | Echo Request type. |
| `ICMP_TIME_EXCEEDED` | `11` | Time Exceeded type. |
| `ICMP_CODE_NET_UNREACH` | `0` | Network unreachable. |
| `ICMP_CODE_HOST_UNREACH` | `1` | Host unreachable. |
| `ICMP_CODE_PROTO_UNREACH` | `2` | Protocol unreachable. |
| `ICMP_CODE_PORT_UNREACH` | `3` | Port unreachable. |
| `ICMP_CODE_FRAG_NEEDED` | `4` | Fragmentation needed and DF set. |
| `ICMP_CODE_TTL_EXCEEDED` | `0` | TTL exceeded in transit. |
| `ICMP_HDR_LEN` | `8` | Common ICMP header length. |
| `ICMP_ORIG_QUOTE_LEN` | `28` | IPv4 header plus first 8 payload bytes. |

`ICMP_ORIG_QUOTE_LEN` is `IP_HDR_LEN + 8`, not just `8`.

---

## Public API

```c
int icmp_receive(Interface *iface, Packet *pkt, void *ctx);

int icmp_send_echo_request(Simulator *sim,
                           uint32_t src_ip,
                           uint32_t dst_ip,
                           uint16_t id,
                           uint16_t seq,
                           const uint8_t *payload,
                           size_t payload_len);

int icmp_send_echo_reply(Simulator *sim,
                         Interface *iface,
                         Packet *req_pkt);

int icmp_send_time_exceeded(Simulator *sim,
                            Interface *iface,
                            Packet *orig_pkt);

int icmp_send_unreach_net(Simulator *sim,
                          Interface *iface,
                          Packet *orig_pkt);

int icmp_send_unreach_host(Simulator *sim,
                           Interface *iface,
                           Packet *orig_pkt);

int icmp_send_unreach_proto(Simulator *sim,
                            Interface *iface,
                            Packet *orig_pkt);

int icmp_send_unreach_port(Simulator *sim,
                           Interface *iface,
                           Packet *orig_pkt);

int icmp_send_frag_needed(Simulator *sim,
                          Interface *iface,
                          Packet *orig_pkt,
                          uint16_t next_hop_mtu);

uint16_t icmp_checksum(const void *data, size_t len);
```

`IPPROTO_ICMP` is defined by the IP module as protocol number `1`.

When the stack is initialized, register:

```c
ip_stack_register_protocol(&ip_stack, IPPROTO_ICMP, icmp_receive, sim);
```

The registered context for ICMP is `Simulator *sim`, because ICMP send helpers
need simulator access for `ip_output`.

---

## Packet Layout At ICMP Receive

`icmp_receive` is called after Ethernet and IP have stripped their headers.
At function entry:

```text
pkt->head
  ...
  Ethernet header          at pkt->data - IP_HDR_LEN - ETH_HDR_LEN
  IPv4 header              at pkt->data - IP_HDR_LEN
pkt->data
  ICMP header              8 bytes
  ICMP body                pkt->len - 8 bytes
```

The ICMP message itself is:

```text
pkt->data[0]               type
pkt->data[1]               code
pkt->data[2..3]            checksum
pkt->data[4..5]            id_or_unused
pkt->data[6..7]            seq_or_mtu
pkt->data[8..pkt->len-1]   body
```

Before reading the stripped IPv4 header, validate:

```text
pkt->data >= pkt->head + IP_HDR_LEN
pkt->len  >= ICMP_HDR_LEN
```

Then:

```text
IpHeader *ip_hdr = (IpHeader *)(pkt->data - IP_HDR_LEN)
IcmpHeader *icmp = (IcmpHeader *)pkt->data
```

The stripped IPv4 header provides:

- `ip_hdr->src_ip`: original sender, network byte order.
- `ip_hdr->dst_ip`: local destination, network byte order.
- `ip_hdr->protocol`: must be `IPPROTO_ICMP` when IP dispatch is correct.

For calls into `ip_output`, convert IPv4 addresses from network order to host
order because `ip_output` accepts host-order `src_ip` and `dst_ip`.

---

## Ownership Rules

- `icmp_receive` consumes `pkt` for every non-null packet passed to it.
- Every `icmp_receive` return path after `pkt != NULL` must do exactly one of
  these:
  - transfer `pkt` to `icmp_send_echo_reply`;
  - free `pkt` with `packet_free(pkt)`.
- Do not call `free(pkt->head)` directly. `Packet` owns both the packet struct
  and the backing buffer; `packet_free(pkt)` frees both.
- If `icmp_receive` returns after handling the packet itself, it frees `pkt`
  with `packet_free(pkt)`.
- If `icmp_receive` calls `icmp_send_echo_reply`, ownership of `pkt` transfers
  to `icmp_send_echo_reply`.
- `icmp_send_echo_reply` consumes `req_pkt`.
- `icmp_send_echo_request` owns its newly allocated packet until it passes the
  packet to `ip_output`.
- ICMP error helpers read from `orig_pkt`; they do not free `orig_pkt` unless
  their caller explicitly transfers ownership.
- On failure before a new packet is passed to `ip_output`, the helper that
  allocated the packet frees it.
- After a new packet is passed to `ip_output`, IP/lower layers own it.

---

## `icmp_receive`

### Inputs

```c
int icmp_receive(Interface *iface, Packet *pkt, void *ctx);
```

- `iface` is the receiving interface.
- `pkt` points at the ICMP message, not the IPv4 header.
- `ctx` is the simulator context passed through IP; cast it to
  `Simulator *sim`.

### Required Local Variables

An implementation should derive these values after validation:

```text
Simulator  *sim
IcmpHeader *icmp
uint8_t     type
uint8_t     code
```

`icmp_receive` does not need to derive `IpHeader *ip_hdr` when it delegates
Echo Request handling to `icmp_send_echo_reply`. The preferred design is for
`icmp_receive` to transfer Echo Request packets to `icmp_send_echo_reply`; that
helper reads the stripped IPv4 header and computes:

- `orig_src_ip_host = ns_ntohl(ip_hdr->src_ip)`
- `orig_dst_ip_host = ns_ntohl(ip_hdr->dst_ip)`

For an Echo Reply, the reply source is `orig_dst_ip_host` and the reply
destination is `orig_src_ip_host`.

### Validation Sequence

Run checks in this order:

1. If `iface == NULL`, return `-1`.
2. If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
3. If `pkt->head == NULL` or `pkt->data == NULL`, increment
   `iface->rx_errors`, free `pkt`, and return `-1`.
4. If `pkt->len < ICMP_HDR_LEN`, increment `iface->rx_errors`, free `pkt`, and
   return `-1`.
5. If `pkt->data < pkt->head + IP_HDR_LEN`, increment `iface->rx_errors`, free
   `pkt`, and return `-1`.
6. If the current ICMP byte range is outside the packet allocation, increment
   `iface->rx_errors`, free `pkt`, and return `-1`.
7. Compute `icmp_checksum(pkt->data, pkt->len)`. If the result is not `0`,
   increment `iface->rx_errors`, free `pkt`, and return `-1`.
8. Read `type` and `code`.
9. Dispatch by `type` and `code`.

In normal C, the readable-range check in step 6 is the packet-buffer bounds
check:

```text
end = pkt->head + PKT_HEADROOM + pkt->capacity

pkt->data >= pkt->head + IP_HDR_LEN
pkt->data < end
pkt->len <= end - pkt->data
```

This is the implementation version of "the ICMP bytes are readable." It means
`pkt->data[0]` through `pkt->data[pkt->len - 1]` stay inside the buffer
allocated by `packet_create`.

Concrete example:

```text
PKT_HEADROOM = 64
pkt->capacity = 100
allocated range = pkt->head + 0 through pkt->head + 163
end = pkt->head + 164

Valid:
  pkt->data = pkt->head + 84
  pkt->len  = 20
  ICMP bytes read = 84 through 103

Invalid:
  pkt->data = pkt->head + 150
  pkt->len  = 30
  ICMP bytes would read = 150 through 179, past the allocation
```

The checksum check covers the ICMP header and body only. It does not include an
IPv4 pseudo-header.

### Implementation Skeleton

This is not complete C, but it shows the intended order:

```text
sim = ctx

if iface is null:
    return -1

if pkt is null:
    iface->rx_errors++
    return -1

if pkt->len < ICMP_HDR_LEN:
    iface->rx_errors++
    packet_free(pkt)
    return -1

if pkt->head or pkt->data is null:
    iface->rx_errors++
    packet_free(pkt)
    return -1

end = pkt->head + PKT_HEADROOM + pkt->capacity

if pkt->data < pkt->head + IP_HDR_LEN:
    iface->rx_errors++
    packet_free(pkt)
    return -1

if pkt->data >= end:
    iface->rx_errors++
    packet_free(pkt)
    return -1

remaining = end - pkt->data

if pkt->len > remaining:
    iface->rx_errors++
    packet_free(pkt)
    return -1

if icmp_checksum(pkt->data, pkt->len) != 0:
    iface->rx_errors++
    packet_free(pkt)
    return -1

icmp = pkt->data

if type/code is supported:
    handle or consume
else:
    iface->rx_dropped++
    packet_free(pkt)
    return -1
```

### Dispatch Table

| Type | Code | Action | Return |
|---:|---:|---|---:|
| `ICMP_ECHO_REQUEST` | `0` | Call `icmp_send_echo_reply(sim, iface, pkt)`. | Helper result |
| `ICMP_ECHO_REPLY` | `0` | Free `pkt`; no waiter table exists. | `0` |
| `ICMP_DEST_UNREACH` | `0..4` | Free `pkt`; message is consumed. | `0` |
| `ICMP_TIME_EXCEEDED` | `0` | Free `pkt`; message is consumed. | `0` |
| Any other type/code | Increment `iface->rx_dropped`, free `pkt`. | `-1` |

"Consumed" does not mean "dropped." Echo Reply, Destination Unreachable, and
Time Exceeded packets that match the table are valid ICMP messages. They are
accepted, freed, and return `0`; they do not increment `rx_dropped`.

### Error vs Drop Decision Table

| Situation | Counter | Reason |
|---|---|---|
| `pkt == NULL` | `rx_errors` | Caller gave impossible input. |
| `pkt->len < ICMP_HDR_LEN` | `rx_errors` | ICMP header is incomplete. |
| `pkt->head == NULL` or `pkt->data == NULL` | `rx_errors` | Packet object is malformed. |
| Missing stripped IPv4 header | `rx_errors` | IP/packet shape is malformed. |
| ICMP bytes run past allocation | `rx_errors` | Packet buffer is malformed. |
| ICMP checksum fails | `rx_errors` | ICMP message is corrupted. |
| Unknown ICMP type | `rx_dropped` | Message is valid enough to parse but unsupported. |
| Known ICMP type with unsupported code | `rx_dropped` | Message is valid enough to parse but unsupported. |
| Echo Reply code 0 | none | Supported message is consumed successfully. |
| Destination Unreachable code 0..4 | none | Supported message is consumed successfully. |
| Time Exceeded code 0 | none | Supported message is consumed successfully. |

### Echo Request Handling

For an Echo Request, `icmp_receive` does not build the reply inline. It calls:

```c
icmp_send_echo_reply(sim, iface, pkt)
```

`icmp_send_echo_reply` validates that `pkt` is still an Echo Request, copies
the identifier, sequence, and body, swaps source and destination IPv4
addresses, submits the new reply packet with `ip_output`, and frees the
request packet.

### Counters

Use counters consistently:

- `rx_errors`: malformed packet, unreadable stripped header, bad checksum, or
  impossible input shape.
- `rx_dropped`: syntactically valid ICMP that this module does not support.
  Do not increment this for supported Echo Reply, Destination Unreachable, or
  Time Exceeded messages that are consumed successfully.
- `tx_errors`: reply/error construction or output failure in send helpers.
- `tx_bytes` and `last_tx_time`: updated by IP/Ethernet send path, not by
  `icmp_receive` directly.

---

## `icmp_checksum`

Compute the ones' complement checksum over an ICMP message.

Inputs:

- `data` points at the first byte of the ICMP header.
- `len` is the total ICMP message length: header plus body.
- `data == NULL` is invalid.
- `len == 0` is invalid for an ICMP message.

Return rules:

- Return `0xFFFF` for `data == NULL`.
- Return `0xFFFF` for `len == 0`.
- For a valid buffer, return the 16-bit checksum value in network byte order,
  ready to store in `icmp->checksum`.
- A received ICMP message is valid when `icmp_checksum(data, len) == 0`.

Algorithm:

1. Treat the ICMP message as a sequence of 16-bit big-endian words.
2. Add each word into a 32-bit accumulator.
3. If one trailing byte remains, treat it as the high byte of the final
   16-bit word and use zero as the low byte.
4. Fold carry bits by adding the high 16 bits back into the low 16 bits until
   no carry remains.
5. Take the one's complement of the 16-bit folded sum.
6. Convert that checksum to network byte order with `ns_htons`.

Odd-length example:

```text
bytes:  0x12 0x34 0x56
words:  0x1234 + 0x5600
```

Outgoing messages:

1. Set `icmp->checksum = 0`.
2. Fill every other ICMP header/body byte.
3. Store `icmp->checksum = icmp_checksum(pkt->data, pkt->len)`.

Incoming messages:

1. Do not zero the checksum field.
2. Compute over the received message exactly as it arrived.
3. Accept only when the result is `0`.

---

## `icmp_send_echo_request`

Build and transmit an Echo Request.

Inputs:

- `sim` must be non-null.
- `src_ip` and `dst_ip` are host-order IPv4 addresses.
- `id` and `seq` are host-order inputs and are stored in network byte order.
- `payload == NULL` is valid only when `payload_len == 0`.

Message construction:

1. Allocate a packet with room for `ICMP_HDR_LEN + payload_len`.
2. Prepend or write the ICMP header.
3. Set `type = ICMP_ECHO_REQUEST`.
4. Set `code = 0`.
5. Set `checksum = 0`.
6. Store `id = ns_htons(id)`.
7. Store `seq = ns_htons(seq)`.
8. Copy payload bytes after the 8-byte header.
9. Compute checksum over the complete ICMP message.
10. Call `ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, pkt)`.

On any failure before `ip_output`, free the new packet and return `-1`.

---

## `icmp_send_echo_reply`

Build and transmit an Echo Reply from a received Echo Request.

This helper does not modify and resend the request packet in place. It creates
a second packet, copies the request ICMP bytes into that second packet, edits
the copied ICMP header, and then frees the original request packet.

```text
req_pkt->data
  |
  v
[ ICMP Echo Request header ][ request payload ]

reply_pkt->data
  |
  v
[ ICMP Echo Reply header   ][ copied payload  ]
```

Input packet shape:

- `req_pkt->data` points at the request ICMP header.
- `req_pkt->data - IP_HDR_LEN` points at the original IPv4 header.
- `req_pkt->len >= ICMP_HDR_LEN`.

Validation:

1. Reject null inputs.
2. Reject packets shorter than `ICMP_HDR_LEN`.
3. Reject packets with null `head` or `data`.
4. Reject packets without readable stripped IPv4 header.
5. Reject packets whose ICMP type/code is not Echo Request/code 0.
6. Reject packets whose current ICMP byte range is outside the packet
   allocation.

Every validation failure after `req_pkt != NULL` increments `iface->tx_errors`
and frees `req_pkt`.

Use `req_icmp` for type/code validation after basic packet-shape checks:

```text
req_icmp = (IcmpHeader *)req_pkt->data

if req_icmp->type != ICMP_ECHO_REQUEST or req_icmp->code != 0:
    tx error
```

Use the request packet, not the reply packet, for the readable-range check
before copying:

```text
end = req_pkt->head + PKT_HEADROOM + req_pkt->capacity

req_pkt->data >= req_pkt->head + IP_HDR_LEN
req_pkt->data < end
req_pkt->len <= end - req_pkt->data
```

Reply construction:

1. Let `req_ip = (IpHeader *)(req_pkt->data - IP_HDR_LEN)`.
2. Let `req_icmp = (IcmpHeader *)req_pkt->data`.
3. Allocate a new packet with exactly `req_pkt->len` bytes of ICMP data.
4. Set `reply_pkt->len = req_pkt->len`.
5. Set `reply_pkt->layer = 4`.
6. Copy the request ICMP bytes into the reply packet.
7. Let `reply_icmp = (IcmpHeader *)reply_pkt->data`.
8. Change `reply_icmp->type` to `ICMP_ECHO_REPLY`.
9. Keep `code`, `id`, `seq`, and payload unchanged.
10. Set `reply_icmp->checksum = 0`.
11. Recompute checksum over the full reply ICMP message and store it in
    `reply_icmp->checksum`.
12. Use `src_ip = ns_ntohl(req_ip->dst_ip)`.
13. Use `dst_ip = ns_ntohl(req_ip->src_ip)`.
14. Call `ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, reply_pkt)`.
15. Free `req_pkt`.

`req_icmp` and `reply_icmp` are not separate heap allocations. They are typed
views into two different packet buffers:

```text
req_icmp   -> req_pkt->data
reply_icmp -> reply_pkt->data
```

After copying, modify `reply_icmp`, not `req_icmp`.

Return `0` if the reply was handed to IP successfully, otherwise return `-1`.
On output failure, increment `iface->tx_errors`, free `reply_pkt`, free
`req_pkt`, and return `-1`.

---

## ICMP Error Messages

All public ICMP error helpers should call one shared private builder. The
public wrappers choose only the ICMP type, code, and optional MTU value.

```text
static int icmp_send_error(Simulator *sim,
                           Interface *iface,
                           Packet *orig_pkt,
                           uint8_t type,
                           uint8_t code,
                           uint16_t next_hop_mtu)
```

This helper is private to `icmp.c`; do not add it to `icmp.h`.

### Wrapper Mapping

| Helper | Type | Code | MTU field |
|---|---:|---:|---|
| `icmp_send_time_exceeded` | `ICMP_TIME_EXCEEDED` | `ICMP_CODE_TTL_EXCEEDED` | `0` |
| `icmp_send_unreach_net` | `ICMP_DEST_UNREACH` | `ICMP_CODE_NET_UNREACH` | `0` |
| `icmp_send_unreach_host` | `ICMP_DEST_UNREACH` | `ICMP_CODE_HOST_UNREACH` | `0` |
| `icmp_send_unreach_proto` | `ICMP_DEST_UNREACH` | `ICMP_CODE_PROTO_UNREACH` | `0` |
| `icmp_send_unreach_port` | `ICMP_DEST_UNREACH` | `ICMP_CODE_PORT_UNREACH` | `0` |
| `icmp_send_frag_needed` | `ICMP_DEST_UNREACH` | `ICMP_CODE_FRAG_NEEDED` | `next_hop_mtu` |

Wrapper bodies should be very small. They should not allocate packets, inspect
packet bytes, compute checksums, or duplicate suppression rules. Their only job
is to pass the correct constants into the shared builder:

```text
icmp_send_time_exceeded(...):
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_TIME_EXCEEDED,
                           ICMP_CODE_TTL_EXCEEDED,
                           0)

icmp_send_unreach_net(...):
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_NET_UNREACH,
                           0)

icmp_send_unreach_host(...):
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_HOST_UNREACH,
                           0)

icmp_send_unreach_proto(...):
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_PROTO_UNREACH,
                           0)

icmp_send_unreach_port(...):
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_PORT_UNREACH,
                           0)

icmp_send_frag_needed(..., next_hop_mtu):
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_FRAG_NEEDED,
                           next_hop_mtu)
```

### Original Packet Shape

`orig_pkt` is the packet that caused the error. In this project, the normal
shape is:

```text
orig_pkt->head
  ...
  original IPv4 header       at orig_pkt->data - IP_HDR_LEN
orig_pkt->data
  original IPv4 payload      maybe UDP/TCP/ICMP bytes, or fewer bytes if short
```

So:

```text
orig_ip = (IpHeader *)(orig_pkt->data - IP_HDR_LEN)
```

Before reading `orig_ip`, validate:

```text
orig_pkt != NULL
orig_pkt->head != NULL
orig_pkt->data != NULL
orig_pkt->data >= orig_pkt->head + IP_HDR_LEN
```

Before quoting original payload bytes, validate the readable range from
`orig_pkt->data` through `orig_pkt->data + orig_pkt->len - 1`:

```text
end = orig_pkt->head + PKT_HEADROOM + orig_pkt->capacity

orig_pkt->data < end
orig_pkt->len <= end - orig_pkt->data
```

The stripped IPv4 header is in headroom. It is not counted in `orig_pkt->len`,
because `orig_pkt->len` starts at `orig_pkt->data`.

### Builder Validation And Ownership

The error helpers do not consume `orig_pkt`. Their caller still owns the
original packet.

Validation failure behavior:

- If `sim == NULL`, `iface == NULL`, or `orig_pkt == NULL`, return `-1`.
- If `orig_pkt` has invalid `head`/`data`, increment `iface->tx_errors` and
  return `-1`.
- If the stripped IPv4 header is not readable, increment `iface->tx_errors` and
  return `-1`.
- If the original payload readable range is invalid, increment
  `iface->tx_errors` and return `-1`.
- If error suppression says no ICMP error should be generated, return `0`.
  Suppression is not a transmit error.

New packet ownership:

- The builder owns `error_pkt` until it calls `ip_output`.
- If any failure occurs before `ip_output`, free `error_pkt`.
- If `ip_output` returns `-1`, increment `iface->tx_errors`, free `error_pkt`,
  and return `-1`.
- If `ip_output` returns `0`, lower layers own `error_pkt`; return `0`.

This is different from `icmp_send_echo_reply`: Echo Reply consumes the request
packet, but the ICMP error helpers only observe the original packet.

### Error Suppression

Suppression means: decide not to send an ICMP error packet, return success
(`0`), and do not increment `tx_errors`. The original packet is still owned by
the caller.

The reason is loop prevention. If host A sends an ICMP error to host B, and host
B has a problem delivering or processing that ICMP error, host B must not answer
with another ICMP error. Otherwise two machines can bounce error messages back
and forth.

Only ICMP error messages are suppressed by this rule. ICMP Echo Request and Echo
Reply are not ICMP error messages.

Examples:

| Original IP protocol | Original ICMP type | Builder action |
|---|---:|---|
| UDP/TCP/other non-ICMP | n/a | Build and send the ICMP error. |
| ICMP | `ICMP_ECHO_REQUEST` | Build and send the ICMP error. |
| ICMP | `ICMP_ECHO_REPLY` | Build and send the ICMP error. |
| ICMP | `ICMP_DEST_UNREACH` | Suppress: return `0`, send nothing. |
| ICMP | `ICMP_TIME_EXCEEDED` | Suppress: return `0`, send nothing. |

Suppression check:

1. Read `orig_ip->protocol`.
2. If protocol is not `IPPROTO_ICMP`, do not suppress for this rule.
3. If protocol is `IPPROTO_ICMP`, the embedded original ICMP header starts at
   `orig_pkt->data`.
4. If at least `ICMP_HDR_LEN` original payload bytes are readable, inspect
   `((IcmpHeader *)orig_pkt->data)->type`.
5. Suppress when embedded type is:
   - `ICMP_DEST_UNREACH`
   - `ICMP_TIME_EXCEEDED`

If the original protocol is ICMP but there are fewer than `ICMP_HDR_LEN`
readable payload bytes, suppress the error. The packet is too incomplete to
prove it is safe to answer.

Implementation shape:

```c
if (orig_ip->protocol == IPPROTO_ICMP) {
    if (orig_pkt->len < ICMP_HDR_LEN) {
        return 0;
    }

    IcmpHeader *orig_icmp = (IcmpHeader *)orig_pkt->data;
    if (orig_icmp->type == ICMP_DEST_UNREACH ||
        orig_icmp->type == ICMP_TIME_EXCEEDED) {
        return 0;
    }
}

/* Not suppressed: continue building the ICMP error packet. */
```

Broadcast, multicast, invalid source address, and non-initial fragment
suppression rules are outside this module until the IP layer models those
concepts.

### Quote Construction

ICMP error body quotes:

```text
original IPv4 header        20 bytes
first original payload      up to 8 bytes
```

Quote length:

```text
payload_quote_len = min(orig_pkt->len, 8)
quote_len = IP_HDR_LEN + payload_quote_len
error_len = ICMP_HDR_LEN + quote_len
```

Allocate:

```text
error_pkt = packet_create(error_len)
```

Then set:

```text
error_pkt->len = error_len
error_pkt->layer = 4
```

Write the ICMP header at `error_pkt->data`:

```text
icmp = (IcmpHeader *)error_pkt->data

icmp->type = type
icmp->code = code
icmp->checksum = 0
icmp->id = 0
icmp->seq = 0
```

For Fragmentation Needed only:

```text
icmp->seq = ns_htons(next_hop_mtu)
```

For all other errors, `id` and `seq` stay zero. These two fields represent
the ICMP error "rest of header" field.

Write the quoted bytes immediately after the ICMP header:

```text
quote = error_pkt->data + ICMP_HDR_LEN

copy IP_HDR_LEN bytes from (orig_pkt->data - IP_HDR_LEN) into quote
copy payload_quote_len bytes from orig_pkt->data into quote + IP_HDR_LEN
```

Then compute:

```text
icmp->checksum = icmp_checksum(error_pkt->data, error_pkt->len)
```

### Addressing

The error goes back to the source of the packet that caused the error:

```text
dst_ip = ns_ntohl(orig_ip->src_ip)
```

For the source IP, use the receiving/outgoing interface IP:

```text
src_ip = ns_ntohl(iface->ip_addr)
```

Then send:

```text
ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, error_pkt)
```

This keeps ICMP at the IP layer. ICMP does not choose the destination MAC.

### Implementation Skeleton

```text
if sim, iface, or orig_pkt is null:
    return -1

validate orig_pkt head/data
validate stripped IPv4 header is readable
validate original payload range is readable

orig_ip = orig_pkt->data - IP_HDR_LEN

if should suppress error:
    return 0

payload_quote_len = min(orig_pkt->len, 8)
error_len = ICMP_HDR_LEN + IP_HDR_LEN + payload_quote_len

error_pkt = packet_create(error_len)
if allocation fails:
    iface->tx_errors++
    return -1

error_pkt->len = error_len
error_pkt->layer = 4

fill ICMP error header
copy original IPv4 header into body
copy first payload_quote_len bytes into body
compute checksum

src_ip = ns_ntohl(iface->ip_addr)
dst_ip = ns_ntohl(orig_ip->src_ip)

if ip_output(...) fails:
    iface->tx_errors++
    packet_free(error_pkt)
    return -1

return 0
```

### Wrapper Implementation Skeleton

After the shared builder exists, each public wrapper should be only one return
statement:

```c
int icmp_send_unreach_port(Simulator *sim,
                           Interface *iface,
                           Packet *orig_pkt)
{
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_PORT_UNREACH,
                           0);
}

int icmp_send_frag_needed(Simulator *sim,
                          Interface *iface,
                          Packet *orig_pkt,
                          uint16_t next_hop_mtu)
{
    return icmp_send_error(sim, iface, orig_pkt,
                           ICMP_DEST_UNREACH,
                           ICMP_CODE_FRAG_NEEDED,
                           next_hop_mtu);
}
```

All other wrappers have the same shape with different constants from the mapping
table.

---

## Error Suppression

The error suppression rules are specified in the ICMP error builder section.
Keep that logic inside the shared private builder so every public wrapper has
the same suppression behavior.

---

## Integration With IP

The IP receive path dispatches ICMP packets after validating and stripping the
IPv4 header:

```text
if ip_hdr.protocol == IPPROTO_ICMP:
    strip IPv4 header
    call icmp_receive(iface, pkt, sim)
```

IP must preserve enough headroom for ICMP to read:

```text
pkt->data - IP_HDR_LEN
```

ICMP transmit functions call:

```c
ip_output(sim, src_ip, dst_ip, IPPROTO_ICMP, pkt)
```

`ip_output` owns outgoing-interface selection, route lookup, next-hop choice,
ARP lookup or resolution, and the lower-level Ethernet send.

---

## ACSL Guidance

Header ACSL should describe public behavior that is stable and checkable.
Avoid large annotations that refer to helper predicates before those predicates
exist.

Useful first predicates:

```c
/*@ predicate icmp_message_readable{L}(Packet *pkt) =
      \valid(pkt) &&
      pkt->len >= ICMP_HDR_LEN &&
      \valid_read(pkt->data + (0 .. pkt->len - 1));

    predicate stripped_ip_readable{L}(Packet *pkt) =
      \valid(pkt) &&
      pkt->data >= pkt->head + IP_HDR_LEN &&
      \valid_read(pkt->data - IP_HDR_LEN + (0 .. IP_HDR_LEN - 1));
*/
```

Contract targets:

- `icmp_receive`: null interface returns `-1`; null packet increments
  `rx_errors`; too-short, missing stripped IP, unreadable message, and bad
  checksum increment `rx_errors`; unsupported valid messages increment
  `rx_dropped`.
- `icmp_send_echo_request`: null simulator fails; null payload is allowed only
  when length is zero; success hands an ICMP packet to IP.
- `icmp_send_echo_reply`: valid Echo Request produces an Echo Reply with copied
  identifier, sequence, and payload.
- ICMP error wrappers: null `sim`, `iface`, or `orig_pkt` returns `-1` without
  modifying counters; invalid original packet shape increments `tx_errors` and
  returns `-1`; suppressed errors return `0` without changing transmit counters;
  valid non-suppressed input either hands an ICMP error packet to IP and returns
  `0`, or increments `tx_errors` and returns `-1` on allocation/output failure.
- Shared error builder: body starts with the original IPv4 header followed by up
  to 8 bytes of original payload; Fragmentation Needed stores `next_hop_mtu` in
  the ICMP rest-of-header field; all other error helpers leave that field zero.
- `icmp_checksum`: null or empty input returns `0xFFFF`; valid input does not
  mutate memory.

Syntax rules:

- Use `behavior name:`, not `behavior name;`.
- Use `\null`, not `NULL`.
- Do not call ordinary C helper functions inside `assumes` unless they have
  logic-compatible contracts.

---

## Implementation Checklist

1. Define constants and `IcmpHeader` in `icmp.h`.
2. Define `IPPROTO_ICMP` in `ip.h`.
3. Implement `icmp_checksum`.
4. Implement `icmp_receive` validation and dispatch exactly as specified.
5. Implement `icmp_send_echo_request`.
6. Implement `icmp_send_echo_reply`.
7. Implement the shared ICMP error builder.
8. Implement public ICMP error wrappers.
9. Add IP dispatch from protocol `IPPROTO_ICMP` to `icmp_receive`.
10. Add unit tests for checksum, receive validation, echo, and error quoting.

---

## Test Plan

- `checksum_even_len`
- `checksum_odd_len`
- `checksum_validates_to_zero`
- `receive_null_iface`
- `receive_null_pkt`
- `receive_too_short`
- `receive_missing_stripped_ip`
- `receive_bad_checksum`
- `receive_unknown_type_drops`
- `receive_echo_request_calls_reply`
- `receive_echo_reply_consumes_packet`
- `receive_dest_unreach_consumes_packet`
- `receive_time_exceeded_consumes_packet`
- `echo_request_writes_id_seq_payload`
- `echo_reply_preserves_id_seq_payload`
- `echo_reply_swaps_ip_addresses`
- `time_exceeded_quotes_original_ip`
- `unreach_proto_uses_code_2`
- `unreach_port_uses_code_3`
- `frag_needed_sets_next_hop_mtu`
- `error_suppressed_for_icmp_error_input`
- `error_quote_handles_short_original`
