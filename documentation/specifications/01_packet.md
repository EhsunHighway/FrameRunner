# Module 01 - Packet Buffer

**Files:** `src/network/packet.c`, `src/network/packet.h`
**Status:** Implemented
**Depends on:** C standard library: `stdint.h`, `stddef.h`, `stdlib.h`,
`string.h`, `stdio.h`

## Concepts First

A packet buffer is the object that carries bytes through the simulator. Ethernet,
IP, ICMP, UDP, TCP, switches, links, and hosts all pass around `Packet *`.

The important idea is that the packet owns one allocated byte buffer, but the
start of the currently visible data can move inside that allocation.

```text
allocated bytes:

head
 |
 v
+----------------------+-----------------------------+
| reserved headroom    | payload/data capacity        |
| 64 bytes             | capacity bytes               |
+----------------------+-----------------------------+
                       ^
                       |
                      data at creation
```

`head` is the original allocation pointer. It never moves.

`data` is the pointer to the first byte visible to the current layer. It moves
backward when a header is prepended and forward when a header is stripped.

`len` is the number of visible bytes starting at `data`.

`capacity` is the usable payload capacity after the fixed headroom. It does not
include the 64 bytes of headroom.

### Internal Buffer Layout

This is an internal memory layout, not a network header format.

At creation:

```text
head
 |
 v
+----------------------+--------------------------------+
| headroom             | usable capacity                |
| PKT_HEADROOM bytes   | capacity bytes                 |
+----------------------+--------------------------------+
                       ^
                       |
                      data

len == 0
data == head + PKT_HEADROOM
```

After the caller writes or constructs visible payload bytes:

```text
head
 |
 v
+----------------------+----------------------+----------+
| unused headroom      | visible bytes        | unused   |
+----------------------+----------------------+----------+
                       ^                      ^
                       |                      |
                      data                 data + len

len == number of visible bytes
```

After `packet_prepend(p, header, header_len)` succeeds:

```text
before:

+----------------------+----------------------+
| unused headroom      | visible bytes        |
+----------------------+----------------------+
                       ^
                       |
                      old data

after:

+-------------------+------------------------+----------------------+
| unused headroom   | prepended header bytes | old visible bytes    |
+-------------------+------------------------+----------------------+
                    ^
                    |
                   new data

new data == old data - header_len
new len  == old len + header_len
```

After `packet_strip(p, header_len)` succeeds:

```text
before:

+-------------------+------------------------+----------------------+
| old headroom      | header being stripped  | remaining bytes      |
+-------------------+------------------------+----------------------+
                    ^
                    |
                   old data

after:

+-------------------+------------------------+----------------------+
| old headroom      | stripped bytes remain  | visible bytes        |
+-------------------+------------------------+----------------------+
                                             ^
                                             |
                                            new data

new data == old data + header_len
new len  == old len - header_len
```

### Why Headroom Exists

When an application or transport layer creates a payload, lower layers still
need to add headers in front of it:

```text
TCP header -> IP header -> Ethernet header -> payload
```

Without headroom, every layer would need to allocate a bigger buffer and copy
the old bytes. With headroom, `packet_prepend` only moves `data` backward and
copies the new header into the newly exposed space.

The current implementation reserves:

```c
#define PKT_HEADROOM 64
```

That is enough for the common Ethernet + IPv4 + TCP path:

```text
Ethernet 14 bytes + IPv4 20 bytes + TCP 20 bytes = 54 bytes
```

The extra 10 bytes give a small safety margin. This module does not grow the
allocation if headroom runs out.

### Strip Does Not Erase Bytes

`packet_strip(p, n)` advances `data` by `n` and decreases `len` by `n`.

It does not zero, free, or overwrite the stripped header bytes. The old bytes
remain in memory before the new `data` pointer until a later prepend overwrites
them.

That matters for receive paths. A protocol may read a header, validate it, then
strip it so the next protocol sees only its own payload.

### Clone Means Independent Ownership

`packet_clone` creates a second `Packet` with a separate allocation. The clone
copies only the currently visible bytes, not old stripped bytes and not unused
headroom.

This matters for switch flooding and retransmission-style paths. If the same
`Packet *` were sent to multiple owners, one receiver might free or mutate bytes
that another receiver still expects to use.

### Checksum

`packet_checksum` computes a 16-bit one's-complement checksum over a caller
provided byte range. It is shared by protocols that need IP-style checksums.

The function treats the input as `uint16_t` words in host memory order. For an
odd trailing byte, the current implementation adds that final byte value
directly. The spec must preserve that behavior unless the checksum code and all
dependent tests are intentionally changed together.

## Purpose

The packet module provides owned byte storage plus simple operations for moving
the visible data window.

It provides:

- packet allocation with fixed headroom
- prepend of bytes before current `data`
- strip of bytes from current `data`
- validation of visible bytes and required stripped headroom against the packet
  allocation
- deep clone of visible bytes
- packet destruction
- one's-complement checksum helper
- debug dump

It does not:

- parse Ethernet, IP, UDP, TCP, or ICMP
- know MTU rules
- grow buffers dynamically
- own interfaces, links, hosts, or simulator state
- decide when a packet should be freed after transmission

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Own packet byte allocation | `Packet` |
| Move visible data pointer | Packet module |
| Validate packet allocation geometry | Packet module, using `packet_validate_view` |
| Interpret header bytes | Protocol modules |
| Decide packet route or egress interface | Host/router/switch/IP modules |
| Clone before multi-destination sends | Caller, using `packet_clone` |
| Free packet storage | Final owner, using `packet_free` |

The packet module is intentionally low-level. It should not include protocol
headers or call protocol functions.

## Data Model

### Constant

```c
#define PKT_HEADROOM 64
```

### `Packet`

```c
typedef struct Packet {
    uint8_t *head;
    uint8_t *data;
    size_t   len;
    size_t   capacity;
    uint32_t id;
    uint32_t trace_id;
    uint32_t parent_id;
    int      layer;
} Packet;
```

Field meanings:

| Field | Meaning |
| --- | --- |
| `head` | Start of the allocated byte buffer. This is the pointer that is freed. |
| `data` | Start of the currently visible packet bytes. |
| `len` | Number of visible bytes beginning at `data`. |
| `capacity` | Payload/data capacity, excluding `PKT_HEADROOM`. |
| `id` | Nonzero sequential identifier for this allocated packet object. |
| `trace_id` | Stable causal-journey identifier shared by related packets and clones. |
| `parent_id` | Immediate source packet object's `id`, or `0` when no parent is recorded. |
| `layer` | Current OSI-ish display layer used by render/debug code. |

### Packet ID State

The implementation keeps a packet-ID counter private to `packet.c`. Its initial
value is `1`. A successfully created packet receives the current nonzero counter
value, after which the counter advances. If advancing the counter produces zero,
skip zero before assigning another packet ID.

Packet IDs are expected to be unique during one practical simulator run. The
module does not scan live packets or retain previously issued IDs when the
32-bit counter wraps.

### Required Layout Invariant

For a valid packet:

```text
head <= data
data + len <= head + PKT_HEADROOM + capacity
```

The allocated byte range is:

```text
head[0 .. PKT_HEADROOM + capacity - 1]
```

The visible byte range is:

```text
data[0 .. len - 1]
```

## Ownership And Lifetime

`packet_create` allocates two objects:

- the `Packet` struct
- the byte buffer stored in `packet->head`

`packet_free` frees both. It must free `head`, not `data`, because `data` may
have moved.

`packet_free(NULL)` is valid and does nothing.

The current implementation does not defend against `NULL` in
`packet_prepend`, `packet_strip`, `packet_clone`, `packet_checksum`, or
`packet_dump`. Callers must pass valid arguments to those functions.

## Public API

```c
Packet  *packet_create(size_t capacity);

int      packet_prepend(Packet     *p,
                        const void *header,
                        size_t      header_len);

int      packet_strip(Packet *p, size_t header_len);

int      packet_validate_view(const Packet *pkt,
                              size_t        required_headroom,
                              size_t        minimum_visible_len);

Packet  *packet_clone(const Packet *p);

int      packet_inherit_trace(Packet       *child,
                              const Packet *parent);

void     packet_free(Packet *p);

uint16_t packet_checksum(const void *data, size_t len);

void     packet_dump(const Packet *p);
```

## Function Behavior

### `packet_create`

Purpose:

Allocate and initialize a new packet object.

Implementation task:

Implement `packet_create` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `capacity > SIZE_MAX - PKT_HEADROOM`, return `NULL`.
- Allocate a `Packet`.
- If `Packet` allocation fails, return `NULL`.
- Allocate `PKT_HEADROOM + capacity` bytes for `head`.
- If `head` allocation fails:
  - free the `Packet`
  - return `NULL`
- Set `data == head + PKT_HEADROOM`.
- Set `len == 0`.
- Set `capacity == capacity`.
- Set `layer == 0`.
- Obtain the next nonzero value from the packet module's private ID counter and
  assign it to `id`.
- Set `trace_id == id`, establishing this packet as the beginning of a new
  trace journey.
- Set `parent_id == 0` because no parent packet has been recorded.
- Return the initialized packet.

`capacity == 0` is supported. The allocation still contains `PKT_HEADROOM`
bytes, `data` points immediately after those bytes, and `len` is zero. This is
required so that `packet_clone` can clone a packet whose visible length is zero.

### `packet_prepend`

Purpose:

Prepend the supplied header bytes to the packet’s visible data.

Implementation task:

Implement `packet_prepend` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- Caller must pass a valid packet.
- Caller must pass a readable `header` buffer of `header_len` bytes.
- If available headroom before `data` is smaller than `header_len`, return `-1`.
- Move `data` backward by `header_len`.
- Increase `len` by `header_len`.
- Copy `header_len` bytes from `header` into the new bytes at `data`.
- Return `0`.

Postconditions:

- On headroom failure, `data` and `len` are unchanged.
- On success, `data` points to the prepended header bytes and `len` increased
  by `header_len`.

The function does not check whether `header` is `NULL`. A nonzero
`header_len` with a bad header pointer is caller error.

### `packet_strip`

Purpose:

Remove the requested number of bytes from the front of the packet’s visible data.

Implementation task:

Implement `packet_strip` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- Caller must pass a valid packet.
- If `header_len > len`, return `-1`.
- Move `data` forward by `header_len`.
- Decrease `len` by `header_len`.
- Return `0`.

Postconditions:

- On too-large `header_len`, `data` and `len` are unchanged.
- On success, `data` advanced by `header_len` and `len` decreased by
  `header_len`.

`header_len == 0` is accepted by the implementation and is a no-op success.

### `packet_validate_view`

Purpose:

Validate that a packet's current visible view and a required amount of
previously stripped headroom are inside the packet's allocation.

Implementation task:

Given a borrowed packet, the number of bytes that a caller must be able to read
immediately before `pkt->data`, and the minimum visible protocol length, decide
whether both ranges are safe to access. This helper centralizes the allocation
geometry previously repeated by ICMP, UDP, TCP, and OSPF receive paths.

Inputs and existing state:

- `pkt` is borrowed and must not be modified or freed.
- `required_headroom` is the number of readable bytes required immediately
  before `pkt->data`; IP protocol handlers pass `IP_HDR_LEN`.
- `minimum_visible_len` is the minimum number of bytes required beginning at
  `pkt->data`.

Result:

- Return `0` only when the required headroom, visible start, and complete
  visible byte range are inside the allocation and `pkt->len` meets the minimum.
  A zero-length view may begin exactly at allocation end when
  `minimum_visible_len == 0`.
- Return `-1` for any invalid packet pointer or range.

Required behavior:

- Do not modify or free `pkt`.
- Do not update protocol counters; the caller owns failure handling.
- Check that `pkt->data` is inside the allocation before subtracting it from
  the allocation end.

Implementation order:

- If `pkt == NULL`, `pkt->head == NULL`, or `pkt->data == NULL`, return `-1`.
- If `pkt->capacity > SIZE_MAX - PKT_HEADROOM`, return `-1`.
- Set `allocation_size = PKT_HEADROOM + pkt->capacity`.
- If `required_headroom > allocation_size`, return `-1`.
- Set `end = pkt->head + allocation_size`.
- If `pkt->data < pkt->head + required_headroom` or `pkt->data > end`, return
  `-1`.
- Set `remaining = (size_t)(end - pkt->data)`.
- If `pkt->len > remaining`, return `-1`.
- If `pkt->len < minimum_visible_len`, return `-1`.
- Return `0`.

### `packet_clone`

Purpose:

Create an independently owned copy of the packet’s visible bytes and metadata.

Implementation task:

Implement `packet_clone` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- Caller must pass a valid source packet.
- Allocate a new packet with capacity equal to `p->len`.
- If allocation fails, return `NULL`.
- Copy exactly the visible bytes from `p->data[0 .. p->len - 1]`.
- Set clone `len == p->len`.
- Set clone `layer == p->layer`.
- Call `packet_inherit_trace(clone, p)`.
- If trace inheritance fails, free the clone and return `NULL`.
- Return the clone.

The clone starts with fresh headroom, so lower layers can prepend headers to the
clone independently. The clone keeps the new `id` assigned by `packet_create`,
but it shares the source packet's `trace_id` and records the source packet's
`id` as its `parent_id`.

### `packet_inherit_trace`

Purpose:

Connect an already-created child packet to the trace journey of the packet that
caused it to be created.

Implementation task:

Update only the child's trace relationship fields. This helper is used for
clones and for protocol-generated packets such as replies and errors.

Inputs and existing state:

- `child` is the independently allocated packet whose trace relationship will
  be updated.
- `parent` is the borrowed packet from which the child was derived.
- The helper does not take ownership of either packet.

Result:

- Return `0` after recording the relationship.
- Return `-1` when either argument is `NULL`.

Required behavior:

- Preserve the child's `id`; the child remains a distinct packet object.
- Do not modify the parent.
- Do not copy packet bytes, length, capacity, layer, or buffer pointers.

Implementation order:

- If `child == NULL` or `parent == NULL`, return `-1` without modifying either
  packet.
- Copy `parent->trace_id` into `child->trace_id`.
- Store `parent->id` in `child->parent_id`.
- Return `0`.

### `packet_free`

Purpose:

Release the byte buffer and descriptor owned by the packet.

Implementation task:

Implement `packet_free` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `p == NULL`, return immediately.
- Free `p->head`.
- Free `p`.

The function must not free `p->data`.

### `packet_checksum`

Purpose:

Compute the packet module’s 16-bit one’s-complement checksum for a supplied byte range.

Implementation task:

Implement `packet_checksum` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- Caller must pass a readable byte range of `len` bytes.
- Sum 16-bit words until fewer than two bytes remain.
- If one byte remains, add that byte value to the sum.
- Fold carries until the sum fits in 16 bits.
- Return the one's complement of the folded sum.

The implementation currently requires `len > 0` in its ACSL contract.

### `packet_dump`

Purpose:

Print the currently visible packet metadata and bytes for debugging.

Implementation task:

Implement `packet_dump` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- Caller must pass a valid packet.
- Print packet id, length, layer, and visible data bytes.
- Assign no packet state.

This is a debugging helper, not a verification target.

## Trace Identity Integration

Packet trace fields are network-observation metadata. They are not serialized
into Ethernet, IP, or protocol headers and do not affect checksums or packet
length.

- Protocol-generated replies and errors call `packet_inherit_trace` before the
  child packet becomes visible to tracing or lower-layer output.
- Packet prepend, strip, validation, checksum, and free operations do not
  modify any identity field.
- `packet_create`, `packet_clone`, and `packet_inherit_trace` define their trace
  field changes in their own function sections above. This section defines only
  the cross-module usage rule.

## Flow Charts

### Outbound Header Construction

```text
packet_create(capacity)
  |
  +-- data starts at head + PKT_HEADROOM
  |
  +-- write payload bytes, if caller has payload
  |
  +-- packet_prepend(TCP header)
  |
  +-- packet_prepend(IP header)
  |
  +-- packet_prepend(Ethernet header)
  |
  +-- transmit packet
```

### Inbound Header Consumption

```text
received Packet
  |
  +-- Ethernet reads bytes at data
  +-- packet_strip(ethernet header length)
  |
  +-- IP reads bytes at new data
  +-- packet_strip(ip header length)
  |
  +-- UDP/TCP/ICMP reads bytes at new data
```

### Clone For Multiple Consumers

```text
switch needs to flood one ingress packet
  |
  +-- for each egress interface:
        |
        +-- packet_clone(original)
        +-- send clone to that interface
  |
  +-- original owner frees or consumes original once
```

## ACSL Contracts

The contracts belong in `packet.h`. Keep literal numeric behavior simple for
KLEVA/EVA.

### Shared Predicates

```c
/*@
    predicate packet_layout(Packet *p) =
        \valid(p) &&
        p->head != \null &&
        \valid(p->head + (0 .. PKT_HEADROOM + p->capacity - 1)) &&
        p->head <= p->data &&
        p->data + p->len <= p->head + PKT_HEADROOM + p->capacity;

    predicate packet_visible_bytes(Packet *p) =
        packet_layout(p) &&
        (p->len == 0 || \valid(p->data + (0 .. p->len - 1)));
*/
```

### `packet_create`

```c
/*@
    requires capacity <= SIZE_MAX - PKT_HEADROOM;
    allocates \result;
    ensures \result == \null || packet_layout(\result);
    ensures \result != \null ==> \result->len == 0;
    ensures \result != \null ==> \result->capacity == capacity;
    ensures \result != \null ==> \result->layer == 0;
    ensures \result != \null ==> \result->data == \result->head + PKT_HEADROOM;
    ensures \result != \null ==> \result->id != 0;
    ensures \result != \null ==> \result->trace_id == \result->id;
    ensures \result != \null ==> \result->parent_id == 0;
*/
Packet *packet_create(size_t capacity);
```

### `packet_prepend`

```c
/*@
    requires packet_layout(p);
    requires header_len > 0;
    requires \valid_read((uint8_t *)header + (0 .. header_len - 1));
    assigns p->data, p->len, p->head[0 .. PKT_HEADROOM + p->capacity - 1];

    behavior ok:
        assumes (size_t)(p->data - p->head) >= header_len;
        ensures \result == 0;
        ensures p->data == \old(p->data) - header_len;
        ensures p->len == \old(p->len) + header_len;
        ensures packet_layout(p);

    behavior no_headroom:
        assumes (size_t)(p->data - p->head) < header_len;
        ensures \result == -1;
        ensures p->data == \old(p->data);
        ensures p->len == \old(p->len);
        ensures packet_layout(p);

    complete behaviors;
    disjoint behaviors;
*/
int packet_prepend(Packet *p, const void *header, size_t header_len);
```

### `packet_strip`

```c
/*@
    requires packet_layout(p);
    assigns p->data, p->len;

    behavior valid_strip:
        assumes header_len <= p->len;
        ensures \result == 0;
        ensures p->data == \old(p->data) + header_len;
        ensures p->len == \old(p->len) - header_len;
        ensures packet_layout(p);

    behavior overflow:
        assumes header_len > p->len;
        ensures \result == -1;
        ensures p->data == \old(p->data);
        ensures p->len == \old(p->len);
        ensures packet_layout(p);

    complete behaviors;
    disjoint behaviors;
*/
int packet_strip(Packet *p, size_t header_len);
```

### `packet_validate_view`

```c
/*@
    assigns \nothing;
    ensures \result == 0 || \result == -1;
*/
int packet_validate_view(const Packet *pkt,
                         size_t required_headroom,
                         size_t minimum_visible_len);
```

The natural-language contract above is authoritative for allocation geometry.
The lightweight ACSL contract records that the helper is pure and returns only
the documented status values.

### `packet_clone`

```c
/*@
    requires packet_visible_bytes((Packet *)p);
    allocates \result;
    ensures \result == \null || packet_layout(\result);
    ensures \result != \null ==> \result->len == p->len;
    ensures \result != \null ==> \result->capacity == p->len;
    ensures \result != \null ==> \result->layer == p->layer;
    ensures \result != \null ==> \result->id != p->id;
    ensures \result != \null ==> \result->trace_id == p->trace_id;
    ensures \result != \null ==> \result->parent_id == p->id;
*/
Packet *packet_clone(const Packet *p);
```

Additional required proof/test property:

- If `packet_clone` succeeds, the clone's visible byte sequence equals the
  source packet's visible byte sequence.
- The clone's `head` allocation is independent from the source packet's `head`
  allocation.

### `packet_inherit_trace`

```c
/*@
    requires child == \null || \valid(child);
    requires parent == \null || \valid_read(parent);

    behavior invalid_argument:
        assumes child == \null || parent == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid_arguments:
        assumes child != \null && parent != \null;
        assigns child->trace_id, child->parent_id;
        ensures \result == 0;
        ensures child->id == \old(child->id);
        ensures child->trace_id == parent->trace_id;
        ensures child->parent_id == parent->id;

    complete behaviors;
    disjoint behaviors;
*/
int packet_inherit_trace(Packet *child, const Packet *parent);
```

### `packet_free`

```c
/*@
    assigns \nothing;
*/
void packet_free(Packet *p);
```

ACSL cannot model `free` ownership precisely with the lightweight style used in
this project. The important implementation rule is natural-language: free
`p->head`, then free `p`; accept `NULL`.

### `packet_checksum`

```c
/*@
    requires len > 0;
    requires \valid_read((uint8_t *)data + (0 .. len - 1));
    assigns \nothing;
*/
uint16_t packet_checksum(const void *data, size_t len);
```

### `packet_dump`

```c
/*@
    requires packet_visible_bytes((Packet *)p);
    assigns \nothing;
*/
void packet_dump(const Packet *p);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `packet_create(1)` returns either `NULL` or a packet with `len == 0`.
2. Successful create sets `data == head + 64`.
3. Successful create sets `capacity` to the requested capacity.
4. `packet_prepend` succeeds when enough headroom exists.
5. Successful prepend moves `data` backward by exactly `header_len`.
6. Successful prepend increases `len` by exactly `header_len`.
7. Successful prepend copies header bytes into the visible front.
8. `packet_prepend` fails when headroom is insufficient.
9. Failed prepend leaves `data` and `len` unchanged.
10. `packet_strip` succeeds when `header_len <= len`.
11. Successful strip moves `data` forward by exactly `header_len`.
12. Successful strip decreases `len` by exactly `header_len`.
13. `packet_strip` fails when `header_len > len`.
14. Failed strip leaves `data` and `len` unchanged.
15. `packet_strip(p, 0)` is a no-op success.
16. `packet_validate_view` rejects NULL packet, `head`, and `data` pointers.
17. It rejects insufficient required headroom.
18. It rejects a visible start beyond allocation end and permits a start equal
    to allocation end only for a zero-length view with a zero minimum.
19. It rejects a visible length that exceeds the remaining allocation.
20. It rejects a visible length below `minimum_visible_len`.
21. It accepts a valid view without changing packet fields.
22. `packet_clone` copies visible bytes and `layer`.
23. `packet_clone` gives the clone a different `id`.
24. Mutating clone visible bytes does not mutate source visible bytes.
25. `packet_free(NULL)` does not crash.
26. `packet_checksum` handles even-length input.
27. `packet_checksum` handles odd-length input according to current behavior.
28. `packet_create(0)` succeeds when allocation succeeds and creates an empty
    packet with `data == head + 64`.
29. `packet_create` rejects a capacity larger than `SIZE_MAX - 64`.
30. A successfully created packet has a nonzero `id`, `trace_id == id`, and
    `parent_id == 0`.
31. Two consecutive successful creates receive different packet IDs.
32. A successful clone keeps its newly assigned packet ID, copies the source
    `trace_id`, and records the source `id` as `parent_id`.
33. `packet_inherit_trace` returns `-1` for each null-argument case without
    modifying a non-null argument.
34. Successful trace inheritance preserves the child's `id`, copies the
    parent's `trace_id`, and records the parent's `id`.
35. Prepend, strip, and validation do not modify `id`, `trace_id`, or
    `parent_id`.

## Common Mistakes

- Do not free `data`; free `head`.
- Do not assume `data == head + PKT_HEADROOM` after prepends or strips.
- Do not assume stripped bytes were erased.
- Do not send the same `Packet *` to multiple owners without cloning.
- Do not call `packet_prepend` with a nonzero length and a bad header pointer.
- Do not add protocol parsing to this module.
- Do not make `capacity` include `PKT_HEADROOM`; the code treats them
  separately.
- Do not assign a new packet ID in `packet_inherit_trace`; preserve the ID
  assigned when the child was created.
- Do not set a clone's `trace_id` to its new `id`; a clone remains in its
  source packet's trace journey.
