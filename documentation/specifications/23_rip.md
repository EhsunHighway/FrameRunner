# Module 23 - RIP

**Files:** `src/protocols/rip.c`, `src/protocols/rip.h`
**Status:** In implementation
**Depends on:** `router`, `route_table`, `udp`, `ip`, `packet`, `interface`,
`scheduler`, `event`, `simulator`, `byte_order`, `ip_utils`

## Concepts First

RIP means Routing Information Protocol.

RIP is a distance-vector routing protocol. Each router tells its neighbors what
networks it can reach and the metric, or cost, for each network.

RIP is intentionally simple as a real routing protocol. It does not build a
full link-state database, run SPF, or use bandwidth/delay composite metrics.
Its core model is small:

- metric is hop count
- directly connected networks have low cost
- unreachable routes use metric `16`
- routers periodically advertise routes
- routers update their own routes from neighbor advertisements

This simulator should preserve that simplicity while still modeling the
important RIP ideas: route advertisements, hop-count metric, split horizon,
route timeout, and garbage collection.

RIP is not the forwarding table itself. RIP is a control-plane protocol that
feeds routes into the Router's route table.

### Distance Vector

A distance-vector protocol says:

```text
I can reach prefix P with cost M.
```

When a router receives that statement from a neighbor, it adds the cost of
reaching that neighbor.

```text
neighbor advertises: 172.16.0.0/16 metric 2
local link to neighbor costs 1
local candidate metric = 3
```

This is the Bellman-Ford idea in routing-protocol form.

RIP caps metrics at `16`. A metric of `16` means unreachable.

### RIB, FIB, And RIP Database

There are three related but different data sets:

| Data | Owner | Meaning |
| --- | --- | --- |
| RIP database | RIP | RIP-learned routes plus timers and learned interface. |
| RIB | RouteTable | Route candidates from direct, static, RIP, and later protocols. |
| FIB | RouteTable | Selected active routes used for forwarding lookup. |

RIP stores its own small database because RIP needs protocol-specific state:

- learned interface, for split horizon
- last update time, for timeout
- garbage collection state
- current advertised metric

Forwarding does not use the RIP database directly. Forwarding uses the
route-table FIB.

### Packet Entry Versus RIP DB Entry

Two structures in this module both describe routes, but they are not the same
thing:

| Structure | Where It Lives | What To Do With It |
| --- | --- | --- |
| `RipEntry` | inside the received or transmitted RIP packet | read from it when parsing, write to it when encoding |
| `RipRouteInfo` | inside `state->db` | store simulator RIP state here |

During receive, a `RipEntry *` points into packet bytes owned by the incoming
UDP payload. Do not write simulator state into that object. Fields such as
`valid`, `state`, `learned_on`, and `last_update` belong to `RipRouteInfo`,
not to `RipEntry`.

During receive, the implementation reads one packet `RipEntry`, converts its
wire fields into host-order route values, then finds or creates the matching
`RipRouteInfo` in `state->db`.

### RIP Wire Format

RIP runs inside UDP. UDP ports are not part of the RIP payload. For this
module, the UDP source and destination ports are both `RIP_PORT`.

The RIP payload starts with one `RipHeader`:

```text
0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| command           | version           | zero                                  |
+-------------------+-------------------+-------------------+-------------------+
  1 byte              1 byte              2 bytes
```

Field mapping:

- `command`: `RIP_CMD_REQUEST` or `RIP_CMD_RESPONSE`
- `version`: `RIP_VERSION`
- `zero`: must be written as `0` when sending; ignored in this receive
  milestone

After the header, the payload contains zero or more fixed-size `RipEntry`
records. Each record is 20 bytes:

```text
0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| afi                                   | route_tag                             |
+-------------------+-------------------+-------------------+-------------------+
| ip_addr                                                                       |
+-------------------------------------------------------------------------------+
| subnet_mask                                                                   |
+-------------------------------------------------------------------------------+
| next_hop                                                                      |
+-------------------------------------------------------------------------------+
| metric                                                                        |
+-------------------------------------------------------------------------------+
  2 bytes                              2 bytes
  4 bytes
  4 bytes
  4 bytes
  4 bytes
```

Field mapping:

- `afi`: address family; IPv4 routes use `RIP_AFI_IPV4`
- `route_tag`: written as `0` in this milestone
- `ip_addr`: route prefix in network byte order
- `subnet_mask`: route mask in network byte order
- `next_hop`: next hop in network byte order; `0` means use the sender IP
- `metric`: RIP metric in network byte order, capped at `RIP_INFINITY`

The packet structures describe bytes on the wire. Multi-byte fields must be
converted between network order and host order when reading or writing.

### Split Horizon

Split horizon prevents a common distance-vector loop.

If Router A learned a route from Router B on interface `eth1`, Router A should
not advertise that same route back out `eth1`.

```text
entry.learned_on == out_iface
  -> skip this route in the update sent on out_iface
```

This simulator should implement split horizon in the first RIP version.

Poison reverse is a related technique where the route is advertised back with
metric `16`. This spec uses simple split horizon, not poison reverse.

### Periodic Updates

RIP sends periodic updates every 30 seconds.

The first RIP implementation uses periodic updates only:

- `rip_update_handler` sends route advertisements on the periodic update timer.
- `rip_timeout_handler` marks stale routes unreachable and removes their
  forwarding route.
- The next periodic update advertises the RIP DB state.

Protocol note: real RIP may also send triggered updates when a route changes.
A triggered update is an immediate advertisement caused by the change, rather
than waiting for the next 30-second periodic timer. This module does not
implement triggered updates in this milestone.

### UDP Port 520

RIP runs over UDP port `520`.

RIPv2 normally sends updates to multicast address `224.0.0.9`.

This simulator supports the RIPv2 multicast destination through the IPv4
output path:

```text
224.0.0.9 -> 01:00:5e:00:00:09
```

RIP send code should therefore use `RIP_MULTICAST` as the destination for
periodic updates on enabled interfaces. The IP module must skip ARP for this
destination and send the packet to the derived Ethernet multicast MAC.

### RIP Receive Context

`udp_receive` calls a bound UDP callback:

```c
void rip_receive(Interface *iface,
                 uint32_t   src_ip,
                 uint16_t   src_port,
                 Packet    *payload,
                 void      *ctx);
```

UDP passes `rip_receive` the interface that received the UDP packet. When
`rip_receive` stores or updates a RIP DB entry for a route learned from this
packet, it stores that interface in the RIP DB entry's `learned_on` field. RIP
uses that field later for split horizon, so it does not advertise the route
back out the same interface.

The `ctx` pointer must identify the RIP instance that owns:

- the simulator
- the router whose route table will be updated
- the UDP state used for binding/sending
- the enabled interfaces
- the RIP database

UDP should not know about route tables. RIP should update routes by calling
Router or RouteTable APIs.

## Purpose

The RIP module implements RIPv2-style route exchange for routers.

It provides:

- RIP wire header and entry layout
- per-router RIP state
- enabled-interface tracking
- UDP port 520 receive callback
- periodic update scheduling
- RIP update encoding
- RIP update parsing
- metric increment and capping at 16
- split horizon
- route install/delete through Router/RouteTable APIs
- route timeout and garbage-collection handlers

It does not:

- forward data packets
- own Router interfaces
- own the global simulator
- replace the route table
- implement OSPF/BGP/IS-IS behavior

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store RIP learned routes and timers | RIP |
| Store route candidates | RouteTable RIB |
| Select active forwarding route | RouteTable FIB rebuild |
| Forward packets | Router |
| Send/receive UDP datagrams | UDP/IP |
| Schedule periodic/timeout events | Scheduler |
| Decide split horizon omission | RIP |
| Resolve next-hop MAC | Router forwarding ARP path |

RIP should call `router_add_route` or `route_table_add` for learned routes. It
must not write directly into route-table arrays.

RIP should bind UDP port `520` through `udp_bind`. It must not inspect UDP
socket slots directly.

## Data Model

### Constants

```c
#define RIP_PORT               520
#define RIP_VERSION            2
#define RIP_CMD_REQUEST        1
#define RIP_CMD_RESPONSE       2
#define RIP_AFI_IPV4           2
#define RIP_MULTICAST          0xE0000009u
#define RIP_INFINITY           16
#define RIP_UPDATE_INTERVAL_US 30000000ULL
#define RIP_TIMEOUT_US         180000000ULL
#define RIP_GC_US              120000000ULL
#define RIP_MAX_IFACES         16
#define RIP_DB_SIZE            128
#define RIP_MAX_ROUTES         25
#define RIP_HDR_LEN            4
#define RIP_ENTRY_LEN          20
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` use
microseconds.

### `RipHeader`

`RipHeader` is the fixed 4-byte header at the start of every RIP payload. It
tells the receiver whether the message is a request or a response and which RIP
version the payload uses.

```c
typedef struct __attribute__((packed)) RipHeader {
    uint8_t  command;
    uint8_t  version;
    uint16_t zero;
} RipHeader;
```

Wire layout:

```text
offset  size  field
0       1     command: 1 request, 2 response
1       1     version: 2
2       2     zero: must be zero
```

### `RipEntry`

`RipEntry` is one route record inside a RIP packet. It is wire data only. The
receive path reads it, converts fields to host order, and then updates a
separate `RipRouteInfo` object in the RIP database.

```c
typedef struct __attribute__((packed)) RipEntry {
    uint16_t afi;
    uint16_t route_tag;
    uint32_t ip_addr;
    uint32_t subnet_mask;
    uint32_t next_hop;
    uint32_t metric;
} RipEntry;
```

Wire layout:

```text
offset  size  field
0       2     AFI, 2 for IPv4
2       2     route tag, zero in this simulator
4       4     prefix/network address
8       4     subnet mask
12      4     next hop, zero means use sender
16      4     metric, 1..16
```

All multi-byte RIP fields are network byte order on the wire.

### `RipRouteInfo`

`RipRouteInfo` is one route stored in RIP's own database. It is not packet
memory and it is not a RouteTable RIB entry. It stores RIP-specific state such
as learned interface, timeout timestamp, garbage-collection state, and current
metric.

```c
typedef struct RipRouteInfo {
    uint32_t   prefix;
    uint8_t    prefix_len;
    uint8_t    state;
    uint8_t    valid;
    uint8_t    _pad;
    uint32_t   metric;
    uint32_t   next_hop;
    Interface *learned_on;
    uint64_t   last_update;
} RipRouteInfo;
```

Required RIP route states:

```c
#define RIP_ROUTE_ACTIVE  1
#define RIP_ROUTE_GC      2
```

`prefix` and `next_hop` are host-order IPv4 addresses.

`learned_on` is borrowed. RIP does not free interfaces.

### `RipState`

`RipState` is the complete per-router RIP control-plane state. It owns the RIP
database and the list of interfaces enabled for RIP, and it borrows the Router,
Simulator, and UDP state needed to install routes and send/receive RIP packets.

```c
typedef struct RipState {
    RipRouteInfo db[RIP_DB_SIZE];
    int          db_count;

    Interface   *ifaces[RIP_MAX_IFACES];
    int          iface_count;

    Simulator   *sim;
    Router      *router;
    UdpState    *udp_state;
} RipState;
```

`RipState` is per router.

The owner allocates `RipState`; RIP initializes it. The owner must also provide
the Router and UDP state that RIP uses for route installation and UDP binding.

## Ownership And Lifetime

RIP owns no interfaces, routers, simulators, UDP states, or packets before UDP
delivery.

`rip_init` initializes caller-owned `RipState`.

`rip_enable_iface` borrows the interface pointer.

`rip_receive` receives ownership of the UDP payload packet from UDP. It must
free that packet after parsing.

`rip_send_update` creates a UDP payload through `udp_send`; ownership follows
the UDP send contract.

Scheduled RIP events borrow `RipState *` as context. The owner must keep
`RipState` alive while scheduled RIP events can fire.

## Public API

```c
void rip_init(RipState *state,
              Simulator *sim,
              Router *router,
              UdpState *udp_state);

int rip_enable_iface(RipState *state, Interface *iface);

void rip_receive(Interface *iface,
                 uint32_t src_ip,
                 uint16_t src_port,
                 Packet *payload,
                 void *ctx);

int rip_send_update(RipState *state, Interface *out_iface);

void rip_update_handler(const Event *e, void *ctx);

void rip_timeout_handler(const Event *e, void *ctx);

void rip_gc_handler(const Event *e, void *ctx);
```

`rip_init` should bind UDP port `520`:

```c
udp_bind(udp_state, RIP_PORT, rip_receive, state);
```

If the owner uses a different control-plane UDP state arrangement, the binding
must still produce the same result: UDP port 520 delivers payloads to
`rip_receive` with this `RipState *` as context.

## Function Behavior

### `rip_init`

Behavior summary:

`rip_init` prepares caller-owned RIP state for one router. After a successful
non-NULL initialization, the RIP database is empty, no interfaces are enabled
yet, the owner pointers are stored, UDP port `520` is connected to
`rip_receive` when UDP state exists, and the first periodic update is scheduled
when a scheduler exists. The first timeout scan and first garbage-collection
scan are also scheduled when a scheduler exists, so learned routes can expire
without another module having to create those events.

Purpose:

Initialize the supplied rip state to its documented empty or default state.

Implementation task:

Implement `rip_init` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `state == NULL`, return immediately.
- Clear the entire `RipState` object.
- Store `sim`, `router`, and `udp_state`.
- If `udp_state != NULL`, call the UDP bind function for:
  - UDP state: `udp_state`
  - local port: `RIP_PORT`
  - receive callback: `rip_receive`
  - callback context: `state`
- If UDP bind fails, keep the already-initialized state and continue. This API
  returns `void`, so there is no error code to return.
- If `sim != NULL && sim->sched != NULL`, schedule first RIP update at
  `scheduler_now(sim->sched) + RIP_UPDATE_INTERVAL_US`.
- If `sim != NULL && sim->sched != NULL`, schedule first RIP timeout scan at
  `scheduler_now(sim->sched) + RIP_TIMEOUT_US`.
- If `sim != NULL && sim->sched != NULL`, schedule first RIP garbage-collection
  scan at `scheduler_now(sim->sched) + RIP_GC_US`.

Postconditions after `state != NULL`:

- `state->db_count == 0`.
- Every RIP database slot is invalid.
- `state->iface_count == 0`.
- Every entry in `state->ifaces[0 .. RIP_MAX_IFACES - 1]` is `NULL`.
- `state->sim == sim`.
- `state->router == router`.
- `state->udp_state == udp_state`.

If UDP bind fails, RIP receive will not work, but the initialized state above is
still valid. This API returns `void`, so there is no error value to report.

### `rip_enable_iface`

Concept:

RIP sends updates only on interfaces explicitly enabled for this `RipState`.
The list stores borrowed interface pointers; it does not create, configure, or
free interfaces.

Calling `rip_enable_iface` twice with the same interface is harmless. The
second call should return success without changing `iface_count`.

Purpose:

Enable one interface in the RIP state.

Implementation task:

Implement `rip_enable_iface` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `state == NULL || iface == NULL`, return `-1`.
- If the interface is already enabled, return `0`.
- If `iface_count >= RIP_MAX_IFACES`, return `-1`.
- Scan `state->ifaces[0 .. RIP_MAX_IFACES - 1]` for the first unused slot,
  where unused means `state->ifaces[i] == NULL`.
- Store `iface` in that slot.
- Increment `iface_count`.
- Return `0`.

RIP does not take ownership of the interface.

### `rip_receive`

Concept:

A RIP payload starts with one fixed-size `RipHeader`, followed by zero or more
fixed-size `RipEntry` records. Length validation has two parts: first prove the
payload is large enough for the header, then prove the remaining bytes form
whole route entries. After that, the implementation can count the complete
entries after the header.

This module accepts one normal RIP message's worth of routes at a time. A
message with more than `RIP_MAX_ROUTES` entries is rejected before parsing
individual entries.

Each route entry in the packet is only input. For each parsed route, RIP must
find or create a `RipRouteInfo` entry in `state->db`. The RIP DB entry is the
object that stores `learned_on`, `last_update`, `state`, `valid`, metric, and
next-hop state.

Purpose:

Validate and process one RIP response delivered by UDP.

Implementation task:

Implement `rip_receive` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `payload == NULL`, return.
- If `ctx == NULL`, free payload and return.
- Cast `ctx` to `RipState *`.
- If `iface == NULL`, free payload and return.
- If `state->router == NULL`, free payload and return.
- If `state->sim == NULL` or `state->sim->sched == NULL`, free payload and
  return.
- Call `packet_validate_view(payload, 0, RIP_HDR_LEN)`. If it returns `-1`,
  free payload and return without reading RIP bytes.
- If `(payload->len - RIP_HDR_LEN) % RIP_ENTRY_LEN != 0`, free payload and
  return.
- Count the complete RIP route entries after the header.
- If that count is greater than `RIP_MAX_ROUTES`, free payload and return.
- Read the RIP header from the start of `payload->data`.
- Check the one-byte `version` field. If it is not `RIP_VERSION`, free payload
  and return.
- Check the one-byte `command` field. If it is not `RIP_CMD_RESPONSE`, free
  payload and return.
- Ignore the header `zero` field in this milestone.
- Read the current simulated time from `state->sim->sched`; this timestamp is
  used as `last_update` for RIP DB entries changed by this packet.
- For each entry:
  - Treat the entry pointer as packet data only. Do not store RIP DB state in
    the `RipEntry` object.
  - Convert AFI from network order to host order.
  - If AFI is not `RIP_AFI_IPV4`, skip this route entry.
  - Convert prefix, subnet mask, next hop, and metric from network order to
    host order.
  - Pass the host-order subnet mask to `ip_mask_to_prefix_len`.
  - If the helper rejects the mask as non-contiguous, skip this route entry.
  - Otherwise use the returned prefix length for the RIP route.
  - Normalize the parsed prefix with the returned prefix length before DB
    lookup or router updates. For example, a received `192.168.1.7/24` route is
    stored and installed as `192.168.1.0/24`.
  - Normalize the received metric so it is not greater than `RIP_INFINITY`.
  - Add one hop for the cost of reaching the advertising neighbor. If that
    addition would exceed `RIP_INFINITY`, keep the candidate metric at
    `RIP_INFINITY`.
  - If entry next hop is zero, use `src_ip` as route next hop.
  - Search `state->db[0 .. RIP_DB_SIZE - 1]` for a valid `RipRouteInfo` whose
    `prefix` and `prefix_len` match the parsed route.
  - Keep the result of that search as the chosen RIP DB entry. If the search
    found no entry, the chosen RIP DB entry is still absent at this point.
  - If the candidate metric is `RIP_INFINITY` and no matching RIP DB entry was
    found, skip this route entry. There is no local RIP DB route to mark
    unreachable.
  - If the candidate metric is `RIP_INFINITY` and a matching RIP DB entry was
    found, update that existing RIP DB entry:
    - set `metric = RIP_INFINITY`
    - set `state = RIP_ROUTE_GC`
    - set `learned_on` to the `iface` passed to `rip_receive`
    - set `last_update` to current scheduler time
    - leave `valid = 1`
  - After marking an existing route unreachable, call
    `router_del_route(state->router, prefix, prefix_len, ROUTE_PROTO_RIP)`,
    then continue to the next packet entry.
  - At this point, the candidate metric is reachable because unreachable
    candidates have already been handled.
  - If no matching RIP DB entry was found, scan
    `state->db[0 .. RIP_DB_SIZE - 1]` for the first invalid slot.
  - If no invalid slot exists, skip this route entry.
  - If an invalid slot exists, make that slot the chosen RIP DB entry and
    increment `db_count` once.
  - Update the chosen RIP DB entry:
    - set `prefix`
    - set `prefix_len`
    - set `metric` to the candidate metric
    - set `next_hop`
    - set `learned_on` to the `iface` passed to `rip_receive`
    - set `last_update` to current scheduler time
    - set `state = RIP_ROUTE_ACTIVE`
    - set `valid = 1`
  - After updating the chosen RIP DB entry, call `router_add_route` with:
    - router `state->router`
    - the parsed prefix and prefix length
    - the resolved next hop
    - egress interface `iface`
    - the candidate metric
    - protocol `ROUTE_PROTO_RIP`
- Free payload before returning.

UDP passes the receiving interface directly to `rip_receive`. RIP must not
infer `learned_on` from source IP or topology search when the receive function
already has the exact interface.

### `rip_send_update`

Concept:

`rip_send_update` advertises the current RIP DB state on one enabled interface.
It reads `RipRouteInfo` entries from `state->db` and encodes packet
`RipEntry` records for UDP output.

Split horizon is applied here. If a RIP DB route was learned on `out_iface`,
that route is omitted from the update sent on `out_iface`.

`udp_send` takes a raw UDP payload buffer, not a `Packet *`. Therefore
`rip_send_update` builds the RIP payload bytes in a local buffer, passes that
buffer to `udp_send`, and does not call `packet_create` itself.

RIP packet entries are wire records. The send path reads each selected
`RipRouteInfo` from `state->db` and writes a corresponding `RipEntry` into the
payload buffer. It must not expose `RipRouteInfo` memory directly as packet
bytes.

Purpose:

Construct and send the requested update.

Implementation task:

Implement `rip_send_update` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `state == NULL || out_iface == NULL`, return `-1`.
- If `state->sim == NULL`, return `-1`.
- Create a local byte array named `payload` with room for one `RipHeader` and
  `RIP_MAX_ROUTES` `RipEntry` records. This array starts empty; it is not
  copied from `state->db`.
- Keep a local counter named `entry_count` for how many `RipEntry` records have
  been encoded into `payload` since the last send.
- Compute `src_ip = ns_ntohl(out_iface->ip_addr)`. Interface addresses are
  stored in network byte order, while `udp_send` and `ip_output` take
  host-order IPv4 addresses.
- Set `dst_ip = RIP_MULTICAST`. This value is the destination IP argument for
  later `udp_send` calls; it is not written into the RIP payload bytes.
- Start the first empty response payload by writing a `RipHeader` at
  `payload[0]`:
  - `command = RIP_CMD_RESPONSE`
  - `version = RIP_VERSION`
  - `zero = 0` in network byte order
- Set `entry_count = 0` after initializing the header.
- Scan `state->db[0 .. RIP_DB_SIZE - 1]` from low index to high index.
- For each RIP DB entry:
  - if `valid == 0`, skip it
  - if `learned_on == out_iface`, skip it for split horizon
  - otherwise write this DB entry into the next route-entry position in
    `payload`
- To encode one `RipEntry` from a `RipRouteInfo`:
  - the first `RIP_HDR_LEN` bytes of `payload` already contain the `RipHeader`
  - route entries begin immediately after that header
  - `entry_count` tells which route-entry position is being filled now
  - when `entry_count == 0`, write the first `RipEntry` at
    `payload + RIP_HDR_LEN`
  - when `entry_count == 1`, write the second `RipEntry` at
    `payload + RIP_HDR_LEN + RIP_ENTRY_LEN`
  - in general, write the next `RipEntry` at
    `payload + RIP_HDR_LEN + entry_count * RIP_ENTRY_LEN`
  - treat that address as a `RipEntry *` while filling the wire fields
  - set `afi = RIP_AFI_IPV4` in network byte order
  - set `route_tag = 0` in network byte order
  - set `ip_addr` to `entry.prefix` in network byte order
  - derive the subnet mask from `entry.prefix_len` and store it in network byte
    order
  - set `next_hop` to `entry.next_hop` in network byte order
  - set `metric` to `entry.metric`, capped at `RIP_INFINITY`, in network byte
    order
- After encoding an entry, increment the current payload entry count.
- If the current payload reaches `RIP_MAX_ROUTES` entries, send that payload
  immediately with `udp_send`.
- The immediate `udp_send` call uses:
  - simulator `state->sim`
  - source IP `src_ip`
  - destination IP `dst_ip`
  - UDP source port `RIP_PORT`
  - UDP destination port `RIP_PORT`
  - pointer `payload`
  - payload length `RIP_HDR_LEN + entry_count * RIP_ENTRY_LEN`
- Immediately check the return value from this `udp_send` call.
- If this `udp_send` call returns `-1`, stop and return `-1`.
- After a full payload is sent, start a new empty response payload in the same
  `payload` array:
  - write a fresh `RipHeader` at `payload[0]`
  - set `entry_count = 0`
  - continue scanning the remaining RIP DB entries
- When the scan finishes, if the current payload has one or more entries, send
  that final payload with the same `udp_send` argument pattern.
- Immediately check the final `udp_send` return value.
- If the final `udp_send` call returns `-1`, return `-1`.
- If the scan finishes without any sendable entries, return `0`.
- Return `0` after all needed update payloads are sent.

`udp_send` uses `ip_output`. The IP module owns the multicast mapping, so RIP
send code does not build Ethernet multicast MAC addresses itself.

### `rip_update_handler`

Concept:

This is the periodic advertisement timer. When it fires, RIP sends updates on
each enabled interface and then schedules the next update timer. It does not
decide whether routes have expired; timeout and garbage collection own route
aging.

Purpose:

Update handler from the supplied input and current state.

Implementation task:

Implement `rip_update_handler` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If event/context is missing, return.
- Context is `RipState *`.
- If `state->sim == NULL` or `state->sim->sched == NULL`, return.
- For each enabled interface, call `rip_send_update`.
- Schedule the next update at current scheduler time plus
  `RIP_UPDATE_INTERVAL_US`.

### `rip_timeout_handler`

Concept:

Timeout is the first phase of removing a stale RIP route. A route becomes stale
when it is active but has not been refreshed for more than `RIP_TIMEOUT_US`.
The timeout handler stops forwarding through that route immediately by deleting
the `ROUTE_PROTO_RIP` route from the Router route table.

The RIP DB entry is not deleted yet. It is moved to `RIP_ROUTE_GC` with metric
`RIP_INFINITY`, so the later garbage-collection phase can remove it after the
GC delay.

Purpose:

Process the scheduled expiration of one RIP route timeout.

Implementation task:

Implement `rip_timeout_handler` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If event/context is missing, return.
- Context is `RipState *`.
- If `state->sim == NULL` or `state->sim->sched == NULL`, return.
- Read the current simulated time from the scheduler.
- Scan `state->db[0 .. RIP_DB_SIZE - 1]`.
- For each entry where `valid == 1` and `state == RIP_ROUTE_ACTIVE`:
  - compute how long it has been since `entry.last_update`
  - if that age is less than or equal to `RIP_TIMEOUT_US`, leave the entry
    unchanged
  - otherwise set `metric = RIP_INFINITY`
  - set `state = RIP_ROUTE_GC`
  - call `router_del_route(state->router,
                           entry.prefix,
                           entry.prefix_len,
                           ROUTE_PROTO_RIP)` if `state->router != NULL`
- Schedule the next timeout scan at current scheduler time plus
  `RIP_TIMEOUT_US`.

This handler is a periodic scan. The event only means "run the timeout scan";
`e->data` does not identify a route.

### `rip_gc_handler`

Concept:

Garbage collection is the second phase of removing a stale RIP route. It only
touches entries that are already valid and in `RIP_ROUTE_GC`.

The `learned_on` pointer is borrowed. Garbage collection must not free that
interface. It only clears the pointer stored in the RIP DB entry before
invalidating the entry.

Purpose:

Process the scheduled garbage-collection deadline for one poisoned RIP route.

Implementation task:

Implement `rip_gc_handler` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If event/context is missing, return.
- Context is `RipState *`.
- If `state->sim == NULL` or `state->sim->sched == NULL`, return.
- Read the current simulated time from the scheduler.
- Scan `state->db[0 .. RIP_DB_SIZE - 1]`.
- For each entry where `valid == 1` and `state == RIP_ROUTE_GC`:
  - compute how long it has been since `entry.last_update`
  - if that age is less than or equal to `RIP_TIMEOUT_US + RIP_GC_US`, leave
    the entry unchanged
  - otherwise set `entry.valid = 0`
  - set `entry.learned_on = NULL`
  - decrement `db_count` once for this invalidated RIP DB entry
- Schedule the next garbage-collection scan at current scheduler time plus
  `RIP_GC_US`.

This handler is also a periodic scan. The event only means "run the GC scan";
`e->data` does not identify a route.

## Trace And Animation Integration

RIP control packets begin a new trace for each independently generated request
or periodic/triggered response. Clones sent on multiple interfaces preserve
their trace relationship.

Emit:

- `TRACE_TIMER_FIRED` for update, timeout, and garbage-collection callbacks
- one protocol record for request/response creation and validated receive
- `TRACE_ROUTE_CHANGED` for each route add, metric replacement, timeout, and
  removal, including destination, metric, and learned interface
- one summary after a triggered or periodic update is scheduled
- a drop record for malformed or policy-rejected RIP messages

Tracing does not change timer rescheduling, route freshness, split-horizon
behavior, packet ownership, or failure propagation.

## Flow Charts

### Initialization

```text
rip_init(state, sim, router, udp_state)
  |
  +-- null state: return
  +-- zero state
  +-- store sim/router/udp_state
  +-- udp_bind(udp_state, 520, rip_receive, state)
  +-- schedule first periodic update if scheduler exists
```

### Receive Update

```text
rip_receive(iface, src_ip, src_port, payload, state)
  |
  +-- validate payload/header/entry count
  +-- for each entry:
        |
        +-- prefix_len = mask_to_prefix_len(subnet_mask)
        +-- candidate_metric = received metric plus one hop, capped at 16
        +-- find matching RipRouteInfo by prefix/prefix_len
        |
        +-- candidate_metric < 16:
        |     use existing DB entry or allocate invalid DB slot
        |     update RipRouteInfo fields
        |     router_add_route(router, prefix, prefix_len, next_hop,
        |                      iface, candidate_metric, ROUTE_PROTO_RIP)
        |
        +-- candidate_metric == 16 and existing DB entry exists:
        |     mark RipRouteInfo metric=16 and state=RIP_ROUTE_GC
        |     router_del_route(router, prefix, prefix_len, ROUTE_PROTO_RIP)
        |
        +-- candidate_metric == 16 and no existing DB entry:
              skip entry
```

### Send Update

```text
rip_send_update(state, out_iface)
  |
  +-- build response header
  +-- scan RIP DB
        |
        +-- learned_on == out_iface: skip
        +-- otherwise append route entry
  |
  +-- send UDP src=520 dst=520
```

## ACSL Contracts

The contracts belong in `rip.h`. Use literal bounds:

- enabled interfaces: `16`
- RIP database entries: `128`
- RIP entries per packet: `25`
- RIP header bytes: `4`
- RIP entry bytes: `20`

### Shared Predicates

```c
/*@
    predicate rip_db_count_valid(RipState *state) =
        0 <= state->db_count && state->db_count <= 128;

    predicate rip_iface_count_valid(RipState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate rip_route_slot_valid(RipState *state, integer i) =
        0 <= i && i < 128 ==>
            (state->db[i].valid == 0 ||
             (state->db[i].valid == 1 &&
              state->db[i].prefix_len <= 32 &&
              1 <= state->db[i].metric &&
              state->db[i].metric <= 16));

    predicate rip_state_well_formed(RipState *state) =
        \valid(state) &&
        rip_db_count_valid(state) &&
        rip_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 128 ==>
            rip_route_slot_valid(state, i);
*/
```

### `rip_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->db[0 .. 127],
                state->db_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->sim,
                state->router,
                state->udp_state;
        ensures state->db_count == 0;
        ensures state->iface_count == 0;
        ensures state->sim == sim;
        ensures state->router == router;
        ensures state->udp_state == udp_state;
        ensures \forall integer i; 0 <= i && i < 128 ==>
                state->db[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void rip_init(RipState *state,
              Simulator *sim,
              Router *router,
              UdpState *udp_state);
```

### `rip_enable_iface`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes rip_state_well_formed(state);
        assumes \valid(iface);
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                state->iface_count == \old(state->iface_count) ||
                state->iface_count == \old(state->iface_count) + 1;

    complete behaviors;
    disjoint behaviors;
*/
int rip_enable_iface(RipState *state, Interface *iface);
```

### `rip_receive`

```c
/*@
    behavior null_payload:
        assumes payload == \null;
        assigns \nothing;

    behavior bad_input:
        assumes payload != \null && (ctx == \null || iface == \null);
        assigns \everything;

    behavior valid:
        assumes payload != \null;
        assumes ctx != \null;
        assumes iface != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns \everything;
*/
void rip_receive(Interface *iface,
                 uint32_t src_ip,
                 uint16_t src_port,
                 Packet *payload,
                 void *ctx);
```

Additional required proof/test property:

- `rip_receive(NULL payload)` returns without freeing.
- `rip_receive` frees non-NULL payload on every path where it does not
  transfer ownership elsewhere.
- Valid metric is incremented by one and capped at `16`.
- Metric `16` does not install a forwarding route.

### `rip_send_update`

```c
/*@
    behavior bad_input:
        assumes state == \null || out_iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes rip_state_well_formed(state);
        assumes \valid(out_iface);
        assigns \everything;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int rip_send_update(RipState *state, Interface *out_iface);
```

This contract intentionally uses `assigns \everything` for the valid behavior
because `rip_send_update` calls `udp_send`, which can allocate packets, prepend
UDP/IP/Ethernet headers through lower layers, update interface transmit state,
queue packets, free packets on failure, and schedule/send through the simulator.
A narrower contract requires modeled contracts for that whole send path first.

### Event Handlers

```c
/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns \everything;
*/
void rip_update_handler(const Event *e, void *ctx);
void rip_timeout_handler(const Event *e, void *ctx);
void rip_gc_handler(const Event *e, void *ctx);
```

The event-handler contract intentionally over-approximates effects with
`\everything`, because these handlers may call UDP send, Router route deletion,
event allocation, scheduler insertion, and event cleanup helpers. A narrower
contract must model those callees first instead of pretending the handlers only
touch RIP DB fields.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `rip_init(NULL, ...)` does not crash.
2. Valid init clears DB count and interface count.
3. Valid init stores simulator, router, and UDP state pointers.
4. Valid init clears every DB valid bit.
5. Valid init binds UDP port 520 when UDP state exists.
6. Valid init schedules first update when scheduler exists.
7. `rip_enable_iface` rejects NULL state and NULL interface.
8. `rip_enable_iface` accepts first interface.
9. `rip_enable_iface` does not duplicate an already enabled interface.
10. `rip_enable_iface` rejects a full interface list.
11. `rip_receive(NULL payload)` does not crash.
12. `rip_receive` with NULL context frees payload.
13. `rip_receive` rejects too-short payload.
14. `rip_receive` rejects invalid entry alignment.
15. `rip_receive` rejects more than 25 entries.
16. `rip_receive` rejects wrong RIP version.
17. `rip_receive` skips non-IPv4 AFI entries.
18. `rip_receive` skips entries with non-contiguous subnet masks.
19. Received metric `1` stores RIP DB metric `2` and installs route with
    `ROUTE_PROTO_RIP`.
20. Received metric `15` becomes candidate metric `16`.
21. Received metric `16` removes the matching `ROUTE_PROTO_RIP` route from the
    Router route table and keeps the RIP DB entry in garbage collection.
22. Unreachable received route with no existing RIP DB entry is skipped.
23. Reachable received route allocates an invalid RIP DB slot when no matching
    entry exists.
24. Reachable received route is skipped when RIP DB is full.
25. Entry next hop `0` uses sender IP as next hop.
26. Nonzero entry next hop is preserved.
27. Split horizon omits routes learned on outgoing interface.
28. Update packet contains at most 25 entries.
29. Periodic update sends one update per enabled interface.
30. Timeout marks stale route unreachable.
31. Garbage collection invalidates stale unreachable route.
32. Valid receive frees payload after parsing.
33. RIP multicast send path is covered by an IP multicast output integration
    test for `224.0.0.9`.

## Common Mistakes

- Do not make RIP forwarding data packets; Router forwards packets.
- Do not write directly into `RouteTable` arrays.
- Do not let forwarding read the RIP database.
- Do not forget that RIP metric `16` means unreachable.
- Do not advertise a route back out the interface it was learned on.
- Do not use milliseconds for scheduler timestamps; scheduler uses
  microseconds.
- Do not build Ethernet multicast MAC addresses in RIP; IP output owns that
  mapping.
- Do not ignore the ingress-interface problem; split horizon needs it.
- Do not let UDP inspect RIP routes.
- Do not leak the UDP payload passed to `rip_receive`.
- Do not write `learned_on`, `valid`, `state`, or `last_update` into a packet
  `RipEntry`; those fields belong to `RipRouteInfo` in `state->db`.
