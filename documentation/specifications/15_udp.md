# Module 15 — UDP

**Files:** `src/protocols/udp.c`, `src/protocols/udp.h`
**Status:** ⬜ Not started
**Depends on:** ip, packet, interface, simulator, icmp

---

## Purpose

UDP provides a small transport header above IPv4. It has no connection state,
no retransmission, and no ordering. The simulator uses UDP for protocols such
as RIP, DNS-like examples, DHCP-like examples, and small application messages.

The UDP module must:

1. Build UDP datagrams from caller payload bytes.
2. Send datagrams through `ip_output`.
3. Receive UDP datagrams after IPv4 strips the IP header.
4. Dispatch received payloads to a callback bound to the destination port.
5. Send ICMP Port Unreachable when no callback is bound.

---

## Wire Format

UDP has a fixed 8-byte header followed by payload bytes.

```
offset  size  field       byte order      meaning
------  ----  ----------  ----------      -------
0       2     src_port    network         sender port
2       2     dst_port    network         receiver port
4       2     length      network         UDP header + payload length
6       2     checksum    network         0 means checksum disabled
8       ...   payload     unchanged       application bytes
```

```c
typedef struct __attribute__((packed)) UdpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} UdpHeader;
```

`sizeof(UdpHeader)` must be `UDP_HDR_LEN`.

---

## Header Constants

Add these constants in `udp.h`:

```c
#define UDP_HDR_LEN        8
#define UDP_MAX_SOCKETS    32
#define UDP_PORT_DNS       53
#define UDP_PORT_DHCP_SRV  67
#define UDP_PORT_DHCP_CLI  68
#define UDP_PORT_RIP       520
```

Add this protocol number in `ip.h`:

```c
#define IPPROTO_UDP        17
```

---

## State Model

UDP sockets belong to a protocol owner, not globally to the whole simulator.
For the final stack, `Host` owns a `UdpState`. Until `Host` is implemented,
unit tests may allocate `UdpState` directly and pass it through `UdpContext`.

Do not store one global UDP table in `Simulator`; in a network simulator,
multiple hosts must be able to bind the same UDP port independently.

```c
typedef struct UdpSocket UdpSocket;
typedef void (*Udp_Recv_Handler)(uint32_t src_ip,
                                  uint16_t src_port,
                                  Packet *payload,
                                  void *ctx);

struct UdpSocket {
    uint16_t  port;       // host order
    int       valid;      // 1 when this slot is bound
    Udp_Recv_Handler recv_handler; // called on receive hit
    void     *ctx;                  // callback-owned context
};

typedef struct UdpState {
    UdpSocket sockets[UDP_MAX_SOCKETS];
    int       count;
} UdpState;

typedef struct UdpContext {
    Simulator *sim;
    UdpState  *state;
} UdpContext;
```

`Udp_Recv_Handler` receives ownership of `payload`. The callback must
eventually free it or pass ownership onward. UDP must not free `payload` after
calling the callback.

---

## Two Receive Handler Levels

There are two different receive handlers in this design.

### IP-level UDP handler

`udp_receive` is the handler registered with IP for protocol `IPPROTO_UDP`.
IP calls this function after it validates and strips the IPv4 header.

```c
int udp_receive(Interface *iface, Packet *pkt, void *ctx);
```

`ctx` points to `UdpContext`, so `udp_receive` can find both:

- `Simulator *sim`, needed for ICMP Port Unreachable
- `UdpState *state`, needed for the bound UDP socket table

### Application-level UDP callback

`Udp_Recv_Handler` is the callback stored inside a bound UDP socket. UDP calls
this only after it finds a matching destination port.

```c
typedef void (*Udp_Recv_Handler)(uint32_t src_ip,
                                 uint16_t src_port,
                                 Packet *payload,
                                 void *ctx);
```

Example callback:

```c
static void rip_udp_recv(uint32_t src_ip,
                         uint16_t src_port,
                         Packet *payload,
                         void *ctx) {
    RipState *rip = (RipState *)ctx;

    rip_receive_update(rip, src_ip, src_port, payload);
    packet_free(payload);
}
```

Example bind:

```c
udp_bind(&udp_state, UDP_PORT_RIP, rip_udp_recv, rip_state);
```

Receive flow:

```text
ip_receive
  -> udp_receive(iface, pkt, &udp_ctx)
       -> find socket by udp_hdr->dst_port
            -> socket->recv_handler(src_ip, src_port, payload, socket->ctx)
```

So:

- `udp_receive` is the transport module's receive entry point.
- `Udp_Recv_Handler` is the upper-layer callback for a specific UDP port.
- `UdpState.sockets[]` is the dispatch table that connects ports to callbacks.

---

## Public API

```c
void udp_init(UdpState *state);

int udp_bind(UdpState *state,
             uint16_t port,
             Udp_Recv_Handler recv_handler,
             void *ctx);

int udp_unbind(UdpState *state, uint16_t port);

int udp_send(Simulator *sim,
             uint32_t src_ip,
             uint32_t dst_ip,
             uint16_t src_port,
             uint16_t dst_port,
             const uint8_t *payload,
             size_t payload_len);

int udp_receive(Interface *iface, Packet *pkt, void *ctx);
```

No public checksum function is required for this module. This simulator sends
UDP checksum `0` and accepts received UDP checksum `0` or nonzero without
validation.

---

## Ownership Rules

These rules are important for implementation and tests.

- `udp_init` does not allocate memory.
- `udp_bind` and `udp_unbind` do not allocate or free packets.
- `udp_send` creates a new packet from caller bytes.
- On successful `udp_send`, ownership of the new packet transfers to
  `ip_output`.
- If `udp_send` creates a packet and then fails before ownership transfer, it
  frees that packet before returning.
- `udp_receive` consumes every non-null `pkt` passed to it.
- On receive validation failure, `udp_receive` frees `pkt`.
- On receive hit, `udp_receive` strips the UDP header and transfers payload
  packet ownership to the callback.
- On receive miss, `udp_receive` does not strip the UDP header; it transfers
  the original packet to `icmp_send_unreach_port`.

---

## Packet Layout On Receive

`udp_receive` is called after IPv4 has stripped the IP header.

At function entry:

```c
pkt->data             points to UdpHeader
pkt->len              is UDP header + UDP payload bytes
pkt->data - IP_HDR_LEN points to the stripped IPv4 header
```

The stripped IP header is still readable because `packet_strip` only advances
`data`; it does not erase bytes.

Use the stripped IP header for:

- source IP passed to the receive callback
- validating that the packet came from IPv4 protocol `IPPROTO_UDP`
- ICMP Port Unreachable quote construction through `icmp_send_unreach_port`

Do not strip the UDP header before deciding whether a socket exists. The ICMP
miss path needs the original UDP header still present at `pkt->data`.

---

## `udp_init`

```c
void udp_init(UdpState *state);
```

Behavior:

1. If `state == NULL`, return immediately.
2. Set every socket slot to zero.
3. Set `state->count = 0`.

Counters are not involved.

ACSL shape:

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->sockets[0 .. UDP_MAX_SOCKETS-1], state->count;
        ensures state->count == 0;
        ensures \forall integer i; 0 <= i < UDP_MAX_SOCKETS ==>
            state->sockets[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
```

---

## `udp_bind`

```c
int udp_bind(UdpState *state,
             uint16_t port,
             Udp_Recv_Handler recv_handler,
             void *ctx);
```

Ports are stored in host byte order.

Validation order:

1. If `state == NULL`, return `-1`.
2. If `recv_handler == NULL`, return `-1`.
3. If `port == 0`, return `-1`. Port zero is not bindable in this simulator.
4. Search all slots for an existing valid socket with the same port.
5. If found, return `-1`.
6. Search all slots for the first invalid slot.
7. If no free slot exists, return `-1`.
8. Fill the free slot:
   - `valid = 1`
   - `port = port`
   - `recv_handler = recv_handler`
   - `ctx = ctx`
9. Increment `state->count`.
10. Return `0`.

Do not increment `count` on any failure path.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes state == \null || recv_handler == \null || port == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior duplicate:
        assumes \valid(state) && recv_handler != \null && port != 0;
        assumes \exists integer i; 0 <= i < UDP_MAX_SOCKETS &&
            state->sockets[i].valid == 1 &&
            state->sockets[i].port == port;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes \valid(state) && recv_handler != \null && port != 0;
        assumes \forall integer i; 0 <= i < UDP_MAX_SOCKETS ==>
            state->sockets[i].valid == 1;
        assigns \nothing;
        ensures \result == -1;

    behavior bound:
        assumes \valid(state) && recv_handler != \null && port != 0;
        assumes \forall integer i; 0 <= i < UDP_MAX_SOCKETS ==>
            state->sockets[i].valid == 0 || state->sockets[i].port != port;
        assumes \exists integer i; 0 <= i < UDP_MAX_SOCKETS &&
            state->sockets[i].valid == 0;
        assigns state->sockets[0 .. UDP_MAX_SOCKETS-1], state->count;
        ensures \result == 0;
        ensures state->count == \old(state->count) + 1;

    complete behaviors;
*/
```

---

## `udp_unbind`

```c
int udp_unbind(UdpState *state, uint16_t port);
```

Validation order:

1. If `state == NULL`, return `-1`.
2. If `port == 0`, return `-1`.
3. Search all slots for a valid socket with `port`.
4. If not found, return `-1`.
5. Clear the slot to zero.
6. Decrement `state->count` if it is greater than zero.
7. Return `0`.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes state == \null || port == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior not_found:
        assumes \valid(state) && port != 0;
        assumes \forall integer i; 0 <= i < UDP_MAX_SOCKETS ==>
            state->sockets[i].valid == 0 || state->sockets[i].port != port;
        assigns \nothing;
        ensures \result == -1;

    behavior removed:
        assumes \valid(state) && port != 0;
        assumes \exists integer i; 0 <= i < UDP_MAX_SOCKETS &&
            state->sockets[i].valid == 1 &&
            state->sockets[i].port == port;
        assigns state->sockets[0 .. UDP_MAX_SOCKETS-1], state->count;
        ensures \result == 0;

    complete behaviors;
*/
```

---

## `udp_send`

```c
int udp_send(Simulator *sim,
             uint32_t src_ip,
             uint32_t dst_ip,
             uint16_t src_port,
             uint16_t dst_port,
             const uint8_t *payload,
             size_t payload_len);
```

Inputs:

- `src_ip` and `dst_ip` are host-order IPv4 addresses.
- `src_port` and `dst_port` are host-order UDP ports.
- `payload` points to caller-owned bytes.
- `payload_len` may be zero.

Validation order:

1. If `sim == NULL`, return `-1`.
2. If `dst_port == 0`, return `-1`.
3. If `payload_len > 0 && payload == NULL`, return `-1`.
4. If `payload_len > UINT16_MAX - UDP_HDR_LEN`, return `-1`.
5. Create a packet with capacity `UDP_HDR_LEN + payload_len`.
6. If allocation fails, return `-1`.
7. Treat `pkt->data` as the start of an empty writable UDP datagram buffer.
   `packet_create` does not put a UDP header there; it only gives you storage.
8. Create a typed view over those first 8 bytes:

   ```c
   UdpHeader *udp_hdr = (UdpHeader *)pkt->data;
   ```

9. Fill the UDP header fields through that typed view:

   ```c
   udp_hdr->src_port = ns_htons(src_port);
   udp_hdr->dst_port = ns_htons(dst_port);
   udp_hdr->length   = ns_htons(UDP_HDR_LEN + payload_len);
   udp_hdr->checksum = 0;
   ```

   After these writes, the first 8 bytes of `pkt->data` contain:

   ```text
   pkt->data + 0  src_port
   pkt->data + 2  dst_port
   pkt->data + 4  length
   pkt->data + 6  checksum
   ```

10. Copy payload bytes after the header if `payload_len > 0`:

    ```c
    memcpy(pkt->data + UDP_HDR_LEN, payload, payload_len);
    ```

11. Set `pkt->len = UDP_HDR_LEN + payload_len`.
12. Set `pkt->layer = 4`.
13. Call `ip_output(sim, src_ip, dst_ip, IPPROTO_UDP, pkt)`.
14. If `ip_output` returns `-1`, free `pkt` and return `-1`.
15. Otherwise return `0`.

Concrete send-buffer shape before IP adds its header:

```text
pkt->head
  [ 64 bytes headroom for future IP/Ethernet prepends ][ UDP datagram storage ]
pkt->data
  [ UDP header, 8 bytes ][ UDP payload, payload_len bytes ]
```

So in C, the field writes are not `pkt->data->src_port`. `pkt->data` is a
`uint8_t *`. Use either a typed header pointer:

```c
UdpHeader *udp_hdr = (UdpHeader *)pkt->data;
udp_hdr->src_port = ns_htons(src_port);
```

or explicit byte offsets. The typed header pointer is the preferred style here
because `UdpHeader` is packed.

`udp_send` must not call `ip_send` directly. `ip_output` owns routing,
interface selection, ARP lookup, and ARP-pending queue behavior.

ACSL shape:

```c
/*@
    behavior null_input:
        assumes sim == \null ||
                dst_port == 0 ||
                (payload_len > 0 && payload == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior too_large:
        assumes sim != \null;
        assumes dst_port != 0;
        assumes payload_len > 0 ==> payload != \null;
        assumes payload_len > 65535 - UDP_HDR_LEN;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(sim);
        assumes dst_port != 0;
        assumes payload_len <= 65535 - UDP_HDR_LEN;
        assumes payload_len == 0 || \valid_read(payload + (0 .. payload_len-1));
        assigns \nothing;
        ensures \result == 0 || \result == -1;

    complete behaviors;
*/
```

---

## `udp_receive`

```c
int udp_receive(Interface *iface, Packet *pkt, void *ctx);
```

`ctx` must point to `UdpContext`.

Validation order:

1. If `iface == NULL`, return `-1`.
2. If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
3. If `ctx == NULL`, increment `iface->rx_errors`, free `pkt`, return `-1`.
4. Cast `ctx` to `UdpContext *udp_ctx`.
5. If `udp_ctx->state == NULL`, increment `iface->rx_errors`, free `pkt`,
   return `-1`.
6. If `pkt->len < UDP_HDR_LEN`, increment `iface->rx_errors`, free `pkt`,
   return `-1`.
7. If `pkt->head == NULL || pkt->data == NULL`, increment `iface->rx_errors`,
   free `pkt`, return `-1`.
8. Verify the current UDP bytes are readable:
   - `end = pkt->head + PKT_HEADROOM + pkt->capacity`
   - `pkt->data >= pkt->head + IP_HDR_LEN`
   - `pkt->data < end`
   - `pkt->len <= (size_t)(end - pkt->data)`
9. If the readable-range check fails, increment `iface->rx_errors`, free
   `pkt`, return `-1`.
10. Recover the stripped IP header:
    - `IpHeader *ip_hdr = (IpHeader *)(pkt->data - IP_HDR_LEN)`
11. If `ip_hdr->protocol != IPPROTO_UDP`, increment `iface->rx_errors`,
    free `pkt`, return `-1`.
12. Read the UDP header:
    - `UdpHeader *udp_hdr = (UdpHeader *)pkt->data`
13. Convert:
    - `src_port = ns_ntohs(udp_hdr->src_port)`
    - `dst_port = ns_ntohs(udp_hdr->dst_port)`
    - `udp_len = ns_ntohs(udp_hdr->length)`
    - `src_ip = ns_ntohl(ip_hdr->src_ip)`
14. If `udp_len < UDP_HDR_LEN`, increment `iface->rx_errors`, free `pkt`,
    return `-1`.
15. If `udp_len > pkt->len`, increment `iface->rx_errors`, free `pkt`,
    return `-1`.
16. If `udp_len < pkt->len`, trim trailing bytes by setting
    `pkt->len = udp_len`.
17. Search `udp_ctx->state->sockets` for a valid socket with
    `port == dst_port`.
18. If no socket is found:
    - If `udp_ctx->sim != NULL`, call
      `icmp_send_unreach_port(udp_ctx->sim, iface, pkt)`.
    - If `udp_ctx->sim == NULL`, increment `iface->rx_dropped`, free `pkt`,
      return `-1`.
    - Return the ICMP function result.
19. If a socket is found:
    - Call `packet_strip(pkt, UDP_HDR_LEN)`.
    - If strip fails, increment `iface->rx_errors`, free `pkt`, return `-1`.
    - Set `pkt->layer = 5`.
    - Call `socket->recv_handler(src_ip, src_port, pkt, socket->ctx)`.
    - Return `0`.

Counters:

- Malformed UDP input increments `iface->rx_errors`.
- Missing bound port is not a malformed packet. If ICMP can be sent, leave
  `rx_errors` unchanged and return the ICMP send result.
- If no socket exists and no simulator exists for ICMP, increment
  `iface->rx_dropped`.
- Receive hit does not increment `rx_dropped`.

ACSL shape:

```c
/*@
    behavior null_iface:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior null_pkt:
        assumes iface != \null && pkt == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior null_ctx:
        assumes \valid(iface) && \valid(pkt) && ctx == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior too_short:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes pkt->len < UDP_HDR_LEN;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior readable_udp:
        assumes \valid(iface) && \valid(pkt) && ctx != \null;
        assumes pkt->len >= UDP_HDR_LEN;
        assumes pkt->data >= pkt->head + IP_HDR_LEN;
        assumes \valid_read(pkt->data + (0 .. pkt->len-1));
        assigns pkt->data, pkt->len, pkt->layer;
        ensures \result == 0 || \result == -1;

    complete behaviors;
*/
```

---

## IP Integration

UDP depends on IP for output, but IP must not depend on UDP internals.

Correct dependency direction:

```text
udp_send  -> ip_output(..., IPPROTO_UDP, ...)
ip_receive -> generic protocol demux table -> registered UDP handler
```

`ip.c` must not include `udp.h`, must not mention `UdpState`, and must not
construct `UdpContext`. The stack owner constructs the UDP context and
registers UDP as an IP upper-layer protocol:

```c
UdpState udp_state;
udp_init(&udp_state);

UdpContext udp_ctx = {
    .sim = sim,
    .state = &udp_state,
};

ip_stack_register_protocol(&ip_stack, IPPROTO_UDP, udp_receive, &udp_ctx);
```

After a valid IP packet is stripped to layer 4, IP conceptually dispatches by
the saved IPv4 protocol number through the bound `IpStack`:

```c
entry = ip_stack->protocols[protocol];

if (entry.handler != NULL) {
    return entry.handler(iface, frame, entry.ctx);
}
```

The protocol table is stack-private state. Other modules interact with it only
through `ip_stack_register_protocol` and `ip_stack_unregister_protocol`.

For UDP, `entry.handler` is `udp_receive` and `entry.ctx` is `&udp_ctx`.
For ICMP, `entry.handler` is `icmp_receive` and `entry.ctx` is `sim`.

For direct `udp_receive` unit tests, construct `UdpContext` directly and call
`udp_receive(iface, pkt, &udp_ctx)`. For full IP dispatch tests, register
`udp_receive` with `ip_stack_register_protocol(&ip_stack, IPPROTO_UDP, udp_receive, &udp_ctx)`
before calling `ip_receive`.

---

## Implementation Checklist

1. Add `IPPROTO_UDP` to `ip.h`.
2. Define UDP constants, structs, and public prototypes in `udp.h`.
3. Implement `udp_init`.
4. Implement `udp_bind`.
5. Implement `udp_unbind`.
6. Implement `udp_send`.
7. Implement `udp_receive`.
8. Register UDP with the owning `IpStack` using
   `ip_stack_register_protocol(&ip_stack, IPPROTO_UDP, udp_receive, &udp_ctx)`.
9. Add IP dispatch tests for protocol `17` using the registered handler path.
10. Add KLEVA YAML and generated tests.

---

## Test Plan

Minimum KLEVA behaviors:

- `udp_init`: null, valid clears all slots.
- `udp_bind`: null state, null callback, port zero, duplicate, table full,
  successful bind.
- `udp_unbind`: null state, port zero, not found, successful unbind.
- `udp_send`: null simulator, null payload with nonzero length, too-large
  payload, valid zero-length payload, valid nonzero payload.
- `udp_receive`: null interface, null packet, null context, null state,
  too short, missing stripped IP header, unreadable UDP bytes, wrong IP
  protocol, bad UDP length, port hit, port miss with ICMP, port miss without
  simulator.

Coverage target remains at least 90% line and 80% branch, excluding malloc
failure branches when EVA is configured with no-null allocation.
