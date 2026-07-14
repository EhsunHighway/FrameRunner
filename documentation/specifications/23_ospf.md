# Module 23 - OSPF

**Files:** `src/protocols/ospf.c`, `src/protocols/ospf.h`
**Depends on:** `router`, `route_table`, `ip`, `packet`, `interface`,
`scheduler`, `event`, `simulator`, `byte_order`, `ip_utils`

## Concepts First

OSPF means Open Shortest Path First.

RIP is distance-vector: routers tell neighbors "I can reach this prefix with
metric M." OSPF is link-state: each router describes its own links, floods that
description to the area, and every router builds the same topology map.

After each router has the topology map, each router runs Dijkstra's shortest
path algorithm from itself and installs the resulting best paths into the route
table.

This simulator implements a simplified OSPFv2 for IPv4:

- single area, area `0`
- router LSAs only in the first milestone
- no authentication
- no DR/BDR election in the first milestone
- fixed-size LSDB
- fixed maximum links per LSA
- SPF output installed into `RouteTable` as `ROUTE_PROTO_OSPF`

### Link-State Database

LSDB means Link-State Database.

LSA means Link-State Advertisement.

An LSA is one router's advertised description of part of the topology. In this
simulator's first OSPF milestone, the important LSA is the router-LSA: it says
which routers or networks one advertising router is connected to, and what each
link costs.

The LSDB stores the most recent LSA learned from each advertising router.

An LSA says:

```text
Router R has links:
  to router A, cost 10
  to router B, cost 20
  to network N, cost 1
```

Every router in the same area should eventually have the same LSDB.

Forwarding does not use the LSDB directly. Forwarding uses the route-table FIB.

### LSA Flooding

Flooding means that when a router receives a newer LSA, it:

- stores it in the LSDB
- forwards it to other OSPF neighbors
- schedules SPF

The router should not flood the LSA back out the interface it came from.

Duplicate or older LSAs are ignored.

### Neighbor Discovery

OSPF neighbors are discovered with Hello packets.

Each OSPF-enabled interface periodically sends Hello packets. A neighbor moves
through a small state machine as bidirectional communication is confirmed.

For the first simulator milestone, the meaningful states are:

- `DOWN`: a known neighbor record exists, but no current adjacency is alive
- `INIT`: heard a Hello from neighbor
- `TWOWAY`: neighbor's Hello lists our router ID
- `FULL`: LSDB exchange is considered complete

The full RFC state machine includes ExStart, Exchange, and Loading. This spec
defines them so the state model is recognizable, but the first implementation
may collapse database exchange from `TWOWAY` to `FULL` after accepting LSAs.

### SPF And Dijkstra

SPF means Shortest Path First.

OSPF runs Dijkstra's algorithm over the LSDB:

```text
dist[self] = 0
all other dist = infinity

repeat:
  pick unvisited router with smallest dist
  relax each link from that router
```

The result is the lowest-cost next hop and egress interface for each reachable
destination. OSPF then installs those routes into the Router's `RouteTable`.

Before installing new OSPF routes, OSPF should flush old OSPF routes:

```c
route_table_flush_proto(&router->route_tbl, ROUTE_PROTO_OSPF);
```

Then it adds the current SPF result with `ROUTE_PROTO_OSPF`.

### OSPF Transport

OSPF does not use UDP or TCP.

OSPFv2 uses IPv4 protocol number `89` directly.

That means the router needs a local IP protocol dispatch path for packets
addressed to OSPF multicast/local control-plane destinations.

The Router owns that dispatch path in `router->ip_stack`. OSPF registration
stores `ospf_receive` as the handler for protocol `89` in that Router-owned
IP stack, with the `OspfState *` as the handler context.

Router receive decides local delivery before transit forwarding. If the packet
is addressed to one of the Router's interface IPs or to OSPF link-local
multicast such as `224.0.0.5`, Router calls `ip_receive` with
`&router->ip_stack`. OSPF must not rely on forwarding lookup for protocol `89`
packets.

### Multicast Addresses

OSPF uses link-local multicast:

```text
224.0.0.5  AllSPFRouters
224.0.0.6  AllDRouters
```

The first milestone uses `224.0.0.5`.

The IP module owns IPv4 multicast-to-Ethernet multicast mapping. OSPF send code
should send through IP output with protocol `IPPROTO_OSPF`/`89` and destination
`OSPF_ALLSPFROUTERS`; it should not build Ethernet multicast frames itself.

## Purpose

The OSPF module implements simplified OSPFv2 control-plane behavior for Router
objects.

It provides:

- OSPF wire header structures
- Hello packet send and receive
- neighbor table and state updates
- LSDB storage
- LSA freshness checks
- LSA flooding
- debounced SPF scheduling
- Dijkstra shortest-path computation
- route installation into Router route table
- dead-neighbor timeout handling

It does not:

- forward data packets
- own Router interfaces
- replace the route table
- implement areas beyond area `0`
- implement DR/BDR election in the first milestone
- implement authentication
- implement all RFC database-exchange details in the first milestone

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store neighbors | OSPF |
| Store LSDB | OSPF |
| Flood LSAs | OSPF send path |
| Run SPF | OSPF |
| Store route candidates | RouteTable |
| Select active forwarding route | RouteTable FIB |
| Forward data packets | Router |
| Schedule Hello/dead/SPF events | Scheduler |
| Send IPv4 protocol 89 packets | IP/link output path |

OSPF should call Router/RouteTable public APIs. It must not write directly into
route-table arrays.

Router forwarding should not read the LSDB.

## Data Model

### Constants

```c
#define OSPF_VERSION             2
#define OSPF_PROTO_NUM           89
#define OSPF_AREA_BACKBONE       0
#define OSPF_ALLSPFROUTERS       0xE0000005u
#define OSPF_ALLDROUTERS         0xE0000006u

#define OSPF_HELLO_INTERVAL_US   10000000ULL
#define OSPF_DEAD_INTERVAL_US    40000000ULL
#define OSPF_SPF_DELAY_US        500000ULL

#define OSPF_LSDB_SIZE           256
#define OSPF_MAX_NEIGHBORS       32
#define OSPF_MAX_IFACES          16
#define OSPF_MAX_LSA_LINKS       4
#define OSPF_INFINITY            0xFFFFFFFFu

#define OSPF_LSA_TYPE_ROUTER     1
#define OSPF_SPF_MAX_NODES       \
    (1 + OSPF_LSDB_SIZE * (1 + OSPF_MAX_LSA_LINKS))
```

Use microseconds for timers because `Scheduler.now` and `Event.timestamp` use
microseconds.

### Packet Types

```c
#define OSPF_TYPE_HELLO 1
#define OSPF_TYPE_DBD   2
#define OSPF_TYPE_LSR   3
#define OSPF_TYPE_LSU   4
#define OSPF_TYPE_LSACK 5
```

The first milestone must support Hello and LSU enough to build neighbors and
flood router LSAs. DBD/LSR/LSAck may be parsed as recognized but unsupported
until full database exchange is implemented.

### `OspfHeader`

`OspfHeader` is the common OSPF packet header. Every OSPF packet starts with
this structure, no matter whether the packet is Hello, LSU, DBD, LSR, or
LSAck. The `type` field decides which body format follows the header.

```c
typedef struct __attribute__((packed)) OspfHeader {
    uint8_t  version;
    uint8_t  type;
    uint16_t pkt_len;
    uint32_t router_id;
    uint32_t area_id;
    uint16_t checksum;
    uint16_t au_type;
    uint64_t auth_data;
} OspfHeader;
```

Header length is `24` bytes.

Multi-byte fields are network byte order on the wire.

`au_type` and `auth_data` are zero in this simulator.

Wire layout:

```text
0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| version           | type              | pkt_len                               |
+-------------------+-------------------+-------------------+-------------------+
| router_id                                                                     |
+-------------------------------------------------------------------------------+
| area_id                                                                       |
+-------------------------------------------------------------------------------+
| checksum                              | au_type                               |
+-------------------+-------------------+-------------------+-------------------+
| auth_data, bytes 0..3                                                        |
+-------------------------------------------------------------------------------+
| auth_data, bytes 4..7                                                        |
+-------------------------------------------------------------------------------+
  24-byte OSPF common header
```

### `OspfHello`

`OspfHello` is the fixed body of an OSPF Hello packet. Hello packets discover
neighbors and prove bidirectional communication. The fixed body is followed by
zero or more neighbor router IDs.

```c
typedef struct __attribute__((packed)) OspfHello {
    uint32_t network_mask;
    uint16_t hello_interval;
    uint8_t  options;
    uint8_t  router_priority;
    uint32_t dead_interval;
    uint32_t dr;
    uint32_t bdr;
} OspfHello;
```

The neighbor list follows the fixed Hello body. Each neighbor ID is a 32-bit
router ID.

Wire layout for the fixed Hello body:

```text
0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| network_mask                                                                  |
+-------------------------------------------------------------------------------+
| hello_interval                       | options           | router_priority     |
+-------------------+-------------------+-------------------+-------------------+
| dead_interval                                                                 |
+-------------------------------------------------------------------------------+
| designated router                                                             |
+-------------------------------------------------------------------------------+
| backup designated router                                                      |
+-------------------------------------------------------------------------------+
| neighbor router IDs, zero or more 32-bit values ...                           |
+-------------------------------------------------------------------------------+
```

### `OspfLsaLink`

`OspfLsaLink` is one link record inside a router LSA. It describes one
connection advertised by a router: the thing connected to, the link type, and
the OSPF metric for reaching it.

When an `OspfLsaLink` is stored inside `OspfLsaEntry.links[]`, it is internal
LSDB state. Its multi-byte fields are host order there. When an
`OspfLsaLink` is written into packet bytes for an outgoing LSU, its multi-byte
fields are converted to network byte order.

```c
typedef struct __attribute__((packed)) OspfLsaLink {
    uint32_t link_id;
    uint32_t link_data;
    uint8_t  type;
    uint8_t  num_tos;
    uint16_t metric;
} OspfLsaLink;
```

Required link-type constants:

```c
#define OSPF_LINK_P2P      1
#define OSPF_LINK_TRANSIT  2
#define OSPF_LINK_STUB     3
```

Meaning:

- `OSPF_LINK_P2P`: point-to-point router-to-router link. The first SPF
  implementation must process this type.
- `OSPF_LINK_TRANSIT`: link to a broadcast/transit network. Store it if
  received, but SPF may ignore it until network-LSA behavior is specified.
- `OSPF_LINK_STUB`: link to a leaf network attached to the advertising router.
  Store it if received, but route installation for stub networks is a later
  milestone unless that behavior is specified in `ospf_run_spf`.

`OSPF_LSA_TYPE_ROUTER` identifies the router-LSA records used as the graph
input for this milestone. Other LSA types may be stored and flooded, but SPF
does not interpret them yet.

`OSPF_SPF_MAX_NODES` is the maximum temporary vertex-array capacity needed by
`ospf_run_spf`: one slot for the local router plus, for every LSDB slot, one
advertising-router ID and up to `OSPF_MAX_LSA_LINKS` linked-router IDs. It is a
SPF, not a second persistent topology database.

### Neighbor State

```c
typedef enum OspfNbrState {
    OSPF_NBR_DOWN,
    OSPF_NBR_ATTEMPT,
    OSPF_NBR_INIT,
    OSPF_NBR_TWOWAY,
    OSPF_NBR_EXSTART,
    OSPF_NBR_EXCHANGE,
    OSPF_NBR_LOADING,
    OSPF_NBR_FULL
} OspfNbrState;
```

### `OspfNeighbor`

`OspfNeighbor` is the per-neighbor state learned from Hello packets. It is not
an LSA and it is not a route. It remembers which router was heard, on which
interface, and when the last valid Hello arrived.

```c
typedef struct OspfNeighbor {
    uint32_t      router_id;
    uint32_t      ip_addr;
    OspfNbrState  state;
    uint64_t      last_hello_ts;
    Interface    *iface;
    int           valid;
} OspfNeighbor;
```

Router ID and IP address are host order.

`iface` is borrowed.

### `OspfLsaEntry`

`OspfLsaEntry` is one LSDB record stored by the simulator. It is the in-memory
form of a received or locally generated LSA. `adv_router` says which router
advertised it, `seq_num` decides freshness, and `links[]` stores the topology
links carried by this LSA.

```c
typedef struct OspfLsaEntry {
    uint32_t    lsa_id;
    uint32_t    adv_router;
    uint32_t    seq_num;
    uint16_t    checksum;
    uint8_t     lsa_type;
    uint8_t     valid;
    OspfLsaLink links[OSPF_MAX_LSA_LINKS];
    int         link_count;
} OspfLsaEntry;
```

The first milestone stores router LSAs with up to four links.

All multi-byte fields in `OspfLsaEntry`, including the multi-byte fields inside
`links[]`, are host order. Incoming LSU parsing converts from network order to
host order before storing into the LSDB. Outgoing LSU generation converts from
host order to network order before writing packet bytes.

### `OspfSpfVertex`

`OspfSpfVertex` is the temporary per-router state used while calculating the
shortest-path tree. It is separate from `OspfLsaEntry`: an LSA describes
persistent topology input, while an SPF vertex records the result and progress
of one calculation.

```c
typedef struct OspfSpfVertex {
    uint32_t router_id;
    uint32_t distance;
    int      predecessor;
    int      first_hop;
    uint8_t  visited;
} OspfSpfVertex;
```

Field meanings:

| Field | Meaning |
|---|---|
| `router_id` | Router represented by this vertex, in host order. |
| `distance` | Current best cost from the local router, or `OSPF_INFINITY` when unreachable. |
| `predecessor` | Index of the preceding vertex in the current shortest-path tree, or `-1` when none exists. |
| `first_hop` | Index in the current SPF vertex array of the first router after the local router, or `-1` when no first hop has been selected. |
| `visited` | Nonzero after the vertex has been permanently selected by Dijkstra. |

An `OspfSpfVertex` borrows no pointers and owns no memory. `ospf_run_spf`
creates an array of these vertices for one calculation and discards it before
returning. SPF vertices are not stored in `OspfState` and are not part of the
LSDB.

### Simplified LSU Body

LSU means Link-State Update. An LSU packet carries one or more LSAs so routers
can synchronize their LSDBs. In this simulator, an LSU body is a compact custom
wire format for carrying router-LSA data; it is not the full RFC LSA encoding.

The first milestone uses one simplified LSU body shape. It is not the full RFC
LSA encoding. It is the simulator's compact wire representation for carrying
one or more `OspfLsaEntry` records.

`OspfLsuHeader` is the small LSU body header. It tells the receiver how many
LSA records follow.

```c
typedef struct __attribute__((packed)) OspfLsuHeader {
    uint16_t lsa_count;
    uint16_t reserved;
} OspfLsuHeader;
```

`OspfLsaWire` is the fixed wire header for one LSA inside the simplified LSU
body. It carries the LSA identity and freshness fields. The variable part of
that LSA is the `OspfLsaLink` array that follows it.

```c
typedef struct __attribute__((packed)) OspfLsaWire {
    uint32_t lsa_id;
    uint32_t adv_router;
    uint32_t seq_num;
    uint16_t checksum;
    uint8_t  lsa_type;
    uint8_t  link_count;
} OspfLsaWire;
```

The LSU body is:

```text
OspfLsuHeader
OspfLsaWire for LSA 0
OspfLsaLink[link_count] for LSA 0
OspfLsaWire for LSA 1
OspfLsaLink[link_count] for LSA 1
...
```

Wire layout for `OspfLsuHeader`:

```text
0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| lsa_count                             | reserved                              |
+-------------------+-------------------+-------------------+-------------------+
```

Wire layout for `OspfLsaWire`:

```text
0                   1                   2                   3
+-------------------------------------------------------------------------------+
| lsa_id                                                                        |
+-------------------------------------------------------------------------------+
| adv_router                                                                    |
+-------------------------------------------------------------------------------+
| seq_num                                                                       |
+-------------------+-------------------+-------------------+-------------------+
| checksum                              | lsa_type          | link_count         |
+-------------------+-------------------+-------------------+-------------------+
```

Parsing rule:

- `lsa_count` tells how many `OspfLsaWire` records are present.
- Each `OspfLsaWire.link_count` tells how many `OspfLsaLink` records follow
  that LSA header.
- If any `link_count > OSPF_MAX_LSA_LINKS`, reject the packet.
- If the packet ends before all declared LSA/link records are present, reject
  the packet.
- Multi-byte fields are network byte order.

### `OspfIface`

`OspfIface` is OSPF's per-enabled-interface record. It points to the borrowed
simulator `Interface` and stores the OSPF cost used when this interface appears
in LSAs or SPF calculations.

```c
typedef struct OspfIface {
    Interface *iface;
    uint16_t   cost;
    int        valid;
} OspfIface;
```

OSPF needs per-interface cost. Do not store only `Interface *` if the algorithm
needs a link metric.

### `OspfState`

`OspfState` is the complete per-router OSPF control-plane state. One Router
that runs OSPF owns one `OspfState` object through its caller/owner. This object
groups neighbor state, LSDB state, enabled OSPF interfaces, and the borrowed
Router/Simulator pointers needed for routing updates and timers.

```c
typedef struct OspfState {
    uint32_t      router_id;
    uint32_t      area_id;

    OspfNeighbor  neighbors[OSPF_MAX_NEIGHBORS];
    int           neighbor_count;

    OspfLsaEntry  lsdb[OSPF_LSDB_SIZE];
    int           lsdb_count;

    OspfIface     ifaces[OSPF_MAX_IFACES];
    int           iface_count;

    Simulator    *sim;
    Router       *router;
} OspfState;
```

`OspfState` is per router.

## Ownership And Lifetime

The owner allocates `OspfState`; OSPF initializes it.

OSPF borrows `Simulator *`, `Router *`, and `Interface *` pointers.

OSPF does not free Router, Simulator, or interfaces.

`ospf_receive` receives ownership of a packet from the IP protocol-dispatch
path. It must free the packet on parse errors and after consuming control-plane
messages, unless it transfers the packet to another helper that documents
ownership transfer.

Scheduled OSPF events borrow `OspfState *` as context. The owner must keep the
state alive while OSPF events can fire.

## Public API

```c
void ospf_init(OspfState *state,
               Simulator *sim,
               Router *router,
               uint32_t router_id);

int ospf_enable_iface(OspfState *state,
                      Interface *iface,
                      uint16_t cost);

int ospf_receive(Interface *iface,
                 Packet *pkt,
                 void *ctx);

int ospf_send_hello(OspfState *state, Interface *iface);

int ospf_flood_lsa(OspfState *state,
                   const OspfLsaEntry *lsa,
                   Interface *except_iface);

int ospf_run_spf(OspfState *state);

void ospf_hello_timer(const Event *e, void *ctx);

void ospf_dead_timer(const Event *e, void *ctx);

void ospf_spf_timer(const Event *e, void *ctx);
```

`ospf_receive` has the IP protocol handler shape:

```c
int handler(Interface *iface, Packet *pkt, void *ctx);
```

The intended registration is:

```c
ip_stack_register_protocol(&router->ip_stack,
                           OSPF_PROTO_NUM,
                           ospf_receive,
                           ospf_state);
```

The `router` used here is `state->router`. The registration writes into the
Router-owned local IP stack, not into a Host IP stack and not into a global
dispatch table.

## Function Behavior

### Internal periodic-timer helper

Purpose:

Schedule one occurrence of an OSPF callback that participates in a periodic
timer sequence.

Implementation task:

Given a simulator, event type, relative delay, callback, and OSPF state, create
one callback event at the current simulated time plus the delay and insert it
into the simulator's scheduler. This helper does not make the scheduler event
intrinsically periodic; the callback schedules the following occurrence.

Inputs and existing state:

- `sim` supplies the scheduler and current simulated time.
- `type`, `delay`, and `handler` describe the event to create.
- `state` is borrowed as the callback context and must remain alive until the
  event fires.

Result:

- This `void` helper silently does nothing when no scheduler exists or when
  event allocation/insertion fails.
- On successful insertion, the scheduler owns the `Event`; `state` remains
  borrowed.

Use one file-local helper with the same pattern as RIP:

```c
static void ospf_schedule_periodic(Simulator     *sim,
                                   EventType      type,
                                   uint64_t       delay,
                                   EventCallback  handler,
                                   OspfState     *state);
```

The helper schedules one occurrence of a periodic OSPF timer. It is not a
special repeating scheduler event; repetition still happens because each timer
handler calls the helper again after doing its work.

Required behavior:

- If `sim == NULL` or `sim->sched == NULL`, return without doing anything.
- Schedule at most one event per call.
- Use simulated rather than wall-clock time.
- Transfer event ownership only after successful scheduler insertion.

Implementation order:

1. If `sim == NULL` or `sim->sched == NULL`, return.
2. Read `now = scheduler_now(sim->sched)`.
3. Create a callback event with the supplied `type`, callback `handler`, context
   `state`, no packet/data payload, and timestamp `now + delay`.
4. If allocation fails, return.
5. Pass the event to `scheduler_schedule(sim->sched, event)`.
6. If insertion fails, free the event; otherwise leave it owned by the
   scheduler.

### `ospf_init`

Purpose:

`ospf_init` prepares caller-owned OSPF state for one router and schedules the
first Hello and dead-neighbor timers when a scheduler exists.

Implementation task:

Given storage supplied by the caller, initialize a fresh OSPF control-plane
state for one router. Record its simulator/router associations and router ID,
start with empty interface, neighbor, and LSDB tables, and seed both periodic
timer chains when scheduling is available. This function does not allocate or
own `OspfState` and does not register protocol `89` in the Router's IP stack.

Inputs and existing state:

- `state` is caller-owned writable storage to initialize.
- `sim` and `router` are borrowed associations and may be `NULL`.
- `router_id` is stored in host order.

Result:

- The function returns no status. A null `state` is left untouched.
- After valid initialization, the state represents backbone area `0` with
  empty fixed tables.
- Timer allocation or scheduling failure does not undo initialization.

Required behavior:

- Initialize every OSPF field deterministically when `state != NULL`.
- Borrow, rather than take ownership of, `sim` and `router`.
- Seed the Hello and dead-neighbor periodic sequences when a scheduler exists.

Implementation order:

- If `state == NULL`, return immediately.
- Zero all OSPF state.
- Store `sim`, `router`, `router_id`, and area `0`.
- Call `ospf_schedule_periodic` to schedule the first Hello timer:
  - event type: `EVT_OSPF_HELLO`
  - delay: `OSPF_HELLO_INTERVAL_US`
  - callback/handler: `ospf_hello_timer`
  - callback context: `state`
  - event data/payload: none
- Call `ospf_schedule_periodic` again to schedule the first dead-neighbor scan:
  - event type: `EVT_OSPF_DEAD`
  - delay: `OSPF_DEAD_INTERVAL_US`
  - callback/handler: `ospf_dead_timer`
  - callback context: `state`
  - event data/payload: none

Postconditions after `state != NULL`:

- Neighbor count is `0`.
- LSDB count is `0`.
- Interface count is `0`.
- `state->sim == sim`.
- `state->router == router`.
- `state->router_id == router_id`.
- Area is backbone area `0`.

Protocol registration for IP protocol `89` may be done by the Router/control
plane owner rather than by `ospf_init`, because `ospf_init` may not own the IP
stack.

### `ospf_enable_iface`

Purpose:

Enable one interface for OSPF operation or update its configured OSPF cost.

Implementation task:

Given initialized OSPF state, a borrowed interface, and a nonzero link cost,
make that interface appear exactly once in the fixed OSPF interface table. If
it is already present, update only its cost; otherwise occupy the first free
slot.

Inputs and existing state:

- `state` owns the fixed `ifaces[]` table and `iface_count`.
- `iface` is borrowed and remains owned by its device/router.
- `cost` is the positive metric advertised for this interface.

Result:

- Return `0` after adding the interface or updating its cost.
- Return `-1` for invalid input, zero cost, or lack of a free slot.
- Never create duplicate valid entries for the same interface.

Required behavior:

- Preserve all unrelated interface slots.
- Increment `iface_count` only when an invalid slot becomes valid.
- Do not take ownership of or modify the supplied `Interface`.

Implementation order:

- If `state == NULL || iface == NULL`, return `-1`.
- If `cost == 0`, return `-1`.
- If the interface is already enabled, update its cost and return `0`.
- If `iface_count >= OSPF_MAX_IFACES`, return `-1`.
- Scan `state->ifaces[0 .. OSPF_MAX_IFACES - 1]` for the first unused slot,
  where unused means `state->ifaces[i].valid == 0`.
- Store `iface` and `cost` in that slot.
- Set that slot's `valid = 1`.
- Increment `iface_count`.
- Return `0`.

OSPF does not take ownership of the interface.

### `ospf_receive`

Purpose:

`ospf_receive` consumes an IPv4 protocol-89 payload after the IPv4 header has
already been stripped by the IP receive path. The packet's visible bytes start
at `OspfHeader`, while the validated IPv4 header remains immediately before
`pkt->data` in packet headroom. The function validates the common OSPF header,
then processes a Hello or simplified LSU message.

Implementation task:

Given the ingress interface, one received OSPF payload, and the local OSPF
state supplied as callback context, validate the common wire header and process
either neighbor-discovery information or simplified link-state updates. Hello
processing refreshes one neighbor record, including the sender's source IPv4
address recovered from the stripped IPv4 header. LSU processing validates the
complete message before changing the LSDB, applies newer records, floods the
changed LSDB entries, and schedules one delayed SPF calculation.
Recognized but unsupported OSPF packet types are consumed without changing
protocol state.

Inputs and existing state:

- `iface` is the borrowed ingress interface and receives error/drop statistics.
- If `iface == NULL`, ownership of `pkt` remains with the caller because the
  receive boundary was not established. After a non-NULL `iface` is accepted,
  this function owns every non-NULL `pkt` and must free it exactly once.
- The visible packet bytes begin at `OspfHeader`. The stripped `IpHeader`
  remains in the same allocation at `pkt->data - IP_HDR_LEN`.
- `ctx` must identify the local `OspfState` used for area, neighbor, LSDB,
  flooding, and scheduler decisions.

Result:

- Return `0` for an accepted or recognized unsupported OSPF packet.
- Return `-1` for invalid callback input, malformed wire data, rejected
  authentication/checksum fields, exhausted neighbor storage, or failed LSU
  flooding/SPF scheduling.
- Once a non-NULL `iface` has been accepted, free every non-NULL packet exactly
  once before returning. A null-interface return does not consume `pkt`.

Required behavior:

- Validate lengths before reading each wire structure or variable-length list.
- Convert wire fields before comparing or storing them.
- Update only the state implied by the received packet type.
- Increment `rx_errors` for malformed input or an internal processing failure.
- Increment `rx_dropped` for a well-formed packet rejected by OSPF policy or
  unavailable protocol capacity.
- Never both free `pkt` inside a nested step and then continue to a shared
  cleanup step. Every branch below states its own final cleanup and return.

Implementation order:

- If `iface == NULL`, return `-1` without reading, modifying, or freeing `pkt`.
- If `pkt == NULL`, increment `iface->rx_errors` and return `-1`.
- If `ctx == NULL`, free `pkt`, increment `iface->rx_errors`, return `-1`.
- Let `state = (OspfState *)ctx`.
- If `iface->prefix_len > 32`, free `pkt`, increment `iface->rx_errors`, and
  return `-1`.
- If `state->sim == NULL`, `state->sim->sched == NULL`, or
  `state->router == NULL`, free `pkt`, increment `iface->rx_errors`, and return
  `-1`.
- Call `packet_validate_view(pkt, IP_HDR_LEN, sizeof(OspfHeader))` before
  reading either the stripped IPv4 header or visible OSPF bytes. If it returns
  `-1`, free `pkt`, increment `iface->rx_errors`, and return `-1`.
- Let `ip_header = (IpHeader *)(pkt->data - IP_HDR_LEN)`. If
  `ip_header->protocol != OSPF_PROTO_NUM`, free `pkt`, increment
  `iface->rx_errors`, and return `-1`.
- Set `sender_ip = ns_ntohl(ip_header->src_ip)`. Store and later use
  `sender_ip` as a host-order value.
- Let `header = (OspfHeader *)pkt->data`.
- Read host-order local values without modifying the packet bytes:
  - `pkt_len = ns_ntohs(header->pkt_len)`
  - `sender_router_id = ns_ntohl(header->router_id)`
  - `area_id = ns_ntohl(header->area_id)`
  - `checksum = ns_ntohs(header->checksum)`
  - `au_type = ns_ntohs(header->au_type)`
- If `header->version != OSPF_VERSION`, free `pkt`, increment
  `iface->rx_errors`,
  return `-1`.
- If `area_id != state->area_id`, free `pkt`, increment
  `iface->rx_dropped`, and return `-1`.
- Require `pkt_len == pkt->len` and `pkt_len >= sizeof(OspfHeader)`. Otherwise
  free `pkt`, increment `iface->rx_errors`, and return `-1`. This milestone does
  not accept undeclared trailing bytes after the OSPF packet.
- In this milestone, checksum validation is disabled. Require `checksum == 0`;
  otherwise free `pkt`, increment `iface->rx_errors`, and return `-1`.
- Require `au_type == 0` and `header->auth_data == 0`; otherwise free `pkt`,
  increment `iface->rx_errors`, and return `-1`. No byte-order conversion of
  `auth_data` is needed because the only accepted bit pattern is all zero.
- Dispatch by `header->type`:
  - `OSPF_TYPE_HELLO`: process the Hello body using the Hello receive order
    below
  - `OSPF_TYPE_LSU`: process the simplified LSU body using the LSU receive
    order below
  - `OSPF_TYPE_DBD`, `OSPF_TYPE_LSR`, or `OSPF_TYPE_LSACK`: free `pkt` and
    return `0`; these packet types are recognized but unsupported in the first
    milestone
  - any other type: free `pkt`, increment `iface->rx_errors`, and return `-1`

Hello receive order:

- If `pkt_len < sizeof(OspfHeader) + sizeof(OspfHello)`, free `pkt`, increment
  `iface->rx_errors`, and return `-1`.
- Let `hello = (OspfHello *)(pkt->data + sizeof(OspfHeader))`.
- Read these host-order values without modifying the packet bytes:
  - `network_mask = ns_ntohl(hello->network_mask)`
  - `hello_interval = ns_ntohs(hello->hello_interval)`
  - `dead_interval = ns_ntohl(hello->dead_interval)`
  `options` and `router_priority` are one-byte fields. `dr` and `bdr` are not
  used by this simplified milestone.
- Compute `neighbor_bytes = pkt_len - sizeof(OspfHeader) - sizeof(OspfHello)`.
  If `neighbor_bytes % sizeof(uint32_t) != 0`, free `pkt`, increment
  `iface->rx_errors`, and return `-1`.
- Let `neighbor_id_count = neighbor_bytes / sizeof(uint32_t)` and let
  `neighbor_ids` point to the bytes immediately after `OspfHello`.
- Treat a syntactically valid but incompatible Hello as a policy drop. If any
  of the following is true, free `pkt`, increment `iface->rx_dropped`, and
  return `-1`:
  - `sender_router_id == state->router_id`
  - `network_mask != ipv4_prefix_mask(iface->prefix_len)`
  - `hello_interval != OSPF_HELLO_INTERVAL_US / 1000000`
  - `dead_interval != OSPF_DEAD_INTERVAL_US / 1000000`
- Set `local_id_seen = 0`. Scan `neighbor_ids[0 .. neighbor_id_count - 1]` in
  increasing index order. Convert each element with `ns_ntohl`; if it equals
  `state->router_id`, set `local_id_seen = 1`. Continue the scan so every
  declared list element is read within the already validated bounds.
- Set `neighbor_index = -1`. Scan
  `state->neighbors[0 .. OSPF_MAX_NEIGHBORS - 1]` in increasing slot order. On
  the first valid slot whose `router_id == sender_router_id`, store its index in
  `neighbor_index` and stop this scan.
- If `neighbor_index == -1`, scan the same table again for the first slot whose
  `valid == 0`. Store that index and stop. If no invalid slot exists, free
  `pkt`, increment `iface->rx_dropped`, and return `-1`.
- If the selected slot was invalid, increment `state->neighbor_count` exactly
  once.
- Update the selected slot in this order:
  - `router_id = sender_router_id`
  - `ip_addr = sender_ip`
  - `iface = iface`
  - `last_hello_ts = scheduler_now(state->sim->sched)`
  - `neighbor->state = OSPF_NBR_FULL` when `local_id_seen != 0`; otherwise set
    `neighbor->state = OSPF_NBR_INIT`
  - `valid = 1`
- Free `pkt` and return `0`. Do not continue into LSU processing or a second
  cleanup step.

Simplified LSU receive order:

- If `pkt_len < sizeof(OspfHeader) + sizeof(OspfLsuHeader)`, free `pkt`,
  increment `iface->rx_errors`, and return `-1`.
- Let `lsu_header = (OspfLsuHeader *)(pkt->data + sizeof(OspfHeader))` and set
  `lsa_count = ns_ntohs(lsu_header->lsa_count)`.
- If `lsa_count > OSPF_LSDB_SIZE`, free `pkt`, increment `iface->rx_errors`, and
  return `-1`. This bound permits function-local parsed-LSA storage without
  dynamic allocation.
- Create a function-local `OspfLsaEntry parsed_lsas[OSPF_LSDB_SIZE]`. These
  temporary entries hold host-order values and are not valid LSDB slots.
- Set:

  ```c
  uint8_t *cursor = pkt->data +
                    sizeof(OspfHeader) +
                    sizeof(OspfLsuHeader);

  size_t remaining = pkt_len -
                     sizeof(OspfHeader) -
                     sizeof(OspfLsuHeader);
  ```

- Parse and validate every declared LSA before modifying `state->lsdb`. Run a
  loop with `lsa_index` from `0` through `lsa_count - 1`. Perform one iteration
  in this exact order:
  1. If `remaining < sizeof(OspfLsaWire)`, free `pkt`, increment
     `iface->rx_errors`, and return `-1`.
  2. Set `wire = (OspfLsaWire *)cursor`. This points to the fixed wire header
     of the current LSA.
  3. Set `parsed = &parsed_lsas[lsa_index]` and initialize that complete
     `OspfLsaEntry` to zero.
  4. Copy the fixed LSA header into `parsed`:
     - `parsed->lsa_id = ns_ntohl(wire->lsa_id)`
     - `parsed->adv_router = ns_ntohl(wire->adv_router)`
     - `parsed->seq_num = ns_ntohl(wire->seq_num)`
     - `parsed->checksum = ns_ntohs(wire->checksum)`
     - `parsed->lsa_type = wire->lsa_type`; this one-byte field needs no
       conversion
     - `parsed->link_count = wire->link_count`; this one-byte field needs no
       conversion
  5. Advance past the fixed LSA header:

     ```c
     cursor += sizeof(OspfLsaWire);
     remaining -= sizeof(OspfLsaWire);
     ```

  6. If `parsed->link_count > OSPF_MAX_LSA_LINKS`, free `pkt`, increment
     `iface->rx_errors`, and return `-1`.
  7. Set `link_bytes = (size_t)parsed->link_count * sizeof(OspfLsaLink)`. If
     `remaining < link_bytes`, free `pkt`, increment `iface->rx_errors`, and
     return `-1`.
  8. Set `wire_links = (OspfLsaLink *)cursor`. This points to the first link of
     the current LSA.
  9. Run a nested loop with `link_index` from `0` through
     `parsed->link_count - 1`. In each iteration, set
     `wire_link = &wire_links[link_index]` and
     `parsed_link = &parsed->links[link_index]`, then store:
     - `parsed_link->link_id = ns_ntohl(wire_link->link_id)`
     - `parsed_link->link_data = ns_ntohl(wire_link->link_data)`
     - `parsed_link->type = wire_link->type`
     - `parsed_link->num_tos = wire_link->num_tos`
     - `parsed_link->metric = ns_ntohs(wire_link->metric)`
  10. Advance past all links belonging to the current LSA:

      ```c
      cursor += link_bytes;
      remaining -= link_bytes;
      ```

- After the LSA loop finishes successfully, set `parsed_count = lsa_count`.
- After all declared LSAs are parsed, require `remaining == 0`. Otherwise free
  `pkt`, increment `iface->rx_errors`, and return `-1`. No LSDB state has been
  changed on any parsing failure above.
- Create `int changed_slots[OSPF_LSDB_SIZE]` and set `changed_count = 0`.
- Apply the temporary entries in increasing parsed index order. Run a loop with
  `parsed_index` from `0` through `parsed_count - 1`. Perform one iteration in
  this exact order:
  1. Set `parsed = &parsed_lsas[parsed_index]`.
  2. Set `matching_index = -1` and `first_invalid_index = -1`.
  3. Scan `state->lsdb[0 .. OSPF_LSDB_SIZE - 1]` once in increasing slot
     order. For each slot, perform these checks in order:
     - Set `slot = &state->lsdb[i]`.
     - If `slot->valid == 0`, then, only when `first_invalid_index == -1`, set
       `first_invalid_index = i`. Continue with the next LSDB slot because an
       invalid slot cannot match the received LSA.
     - Otherwise the slot is valid. If both
       `slot->lsa_id == parsed->lsa_id` and
       `slot->adv_router == parsed->adv_router`, set `matching_index = i` and
       stop this LSDB scan.
     - Otherwise continue with the next LSDB slot.
     Remembering an invalid slot must not stop the scan: a matching valid slot
     later in the table takes priority over every free slot.
  4. If `matching_index != -1` and
     `state->lsdb[matching_index].seq_num >= parsed->seq_num`, this received LSA
     is not newer. Continue directly with the next `parsed_index` without
     changing the LSDB or `changed_slots`.
  5. Declare `lsdb_index`. If `matching_index != -1`, set
     `lsdb_index = matching_index`; otherwise set
     `lsdb_index = first_invalid_index`.
  6. If `lsdb_index == -1`, no matching slot or free slot is available. Skip
     this LSA and continue with the next `parsed_index`.
  7. Set `slot_was_invalid` to whether
     `state->lsdb[lsdb_index].valid == 0` before the slot is overwritten.
  8. Set `target = &state->lsdb[lsdb_index]`.
  9. Copy the complete entry with `*target = *parsed`, then set
     `target->valid = 1`.
  10. If `slot_was_invalid != 0`, increment `state->lsdb_count` exactly once.
  11. Set `changed_already_recorded = 0`. Scan
      `changed_slots[0 .. changed_count - 1]`. If an element equals
      `lsdb_index`, set `changed_already_recorded = 1` and stop this scan.
  12. If `changed_already_recorded == 0`, store
      `changed_slots[changed_count] = lsdb_index` and increment
      `changed_count`. Repeated records for the same LSA therefore leave only
      one changed-slot index, and the final stored version is the version later
      flooded.
- Set `postprocess_failed = 0`.
- For each index in `changed_slots[0 .. changed_count - 1]`, call
  `ospf_flood_lsa(state, &state->lsdb[index], iface)`. If a call returns `-1`,
  set `postprocess_failed = 1` and continue so the remaining changed LSAs still
  receive a flooding attempt.
- If `changed_count > 0`, allocate one callback event with:
  - type `EVT_OSPF_SPF`
  - timestamp `scheduler_now(state->sim->sched) + OSPF_SPF_DELAY_US`
  - no source device, destination device, packet, or event data
  - handler `ospf_spf_timer`
  - handler context `state`
- If SPF event allocation fails, set `postprocess_failed = 1`. If allocation
  succeeds but `scheduler_schedule` fails, free the unscheduled event and set
  `postprocess_failed = 1`.
- Free `pkt` exactly once after flooding and scheduling attempts finish.
- If `postprocess_failed != 0`, increment `iface->rx_errors` and return `-1`.
  Otherwise return `0`.

### `ospf_send_hello`

Purpose:

`ospf_send_hello` builds one OSPF Hello payload for the specified interface and
sends it as IPv4 multicast protocol `89` to `OSPF_ALLSPFROUTERS`.

Implementation task:

Given initialized OSPF state and one interface, construct one complete OSPF
Hello payload containing the local identity, timing parameters, interface mask,
and live neighbors heard on that interface. Encode it as wire bytes in a
`Packet`, send it through IPv4 multicast, and release the caller's packet after
the lower layer has cloned the frame for delivery. This function sends only one
Hello; the timer handler controls repetition.

Inputs and existing state:

- `state` supplies router/area identity, simulator access, and neighbor records.
- `iface` supplies the source IPv4 address and advertised prefix length.
- Both inputs are borrowed and remain owned by their existing owners.

Result:

- Return `0` after successful multicast output.
- Return `-1` before or during construction/output when the operation cannot be
  completed.
- Do not update neighbor or LSDB state; lower layers may update transmit
  statistics and schedule packet delivery.

Required behavior:

- Reject a missing state, interface, or simulator and reject an interface
  prefix length greater than `32`.
- Advertise the local router ID, area ID, interface network mask, configured
  Hello/dead intervals, and the router IDs of live neighbors learned on this
  same interface.
- Encode every multi-byte wire field in network byte order.
- Use a `Packet` as the OSPF payload passed to `ip_output`; do not pass a raw
  byte buffer.
- Use the interface's IPv4 address as the source and
  `OSPF_ALLSPFROUTERS` as the multicast destination.
- Free the caller-owned packet after `ip_output` returns. Multicast output
  prepends the lower-layer headers and `link_transmit` clones the completed
  frame for scheduled delivery; the original packet remains owned by this
  function.
- Return `0` when IP output succeeds and `-1` for invalid input, packet
  allocation failure, or IP-output failure.

Implementation order:

- If `state == NULL || iface == NULL`, return `-1`.
- If `state->sim == NULL`, return `-1`.
- If `iface->prefix_len > 32`, return `-1`.
- Count the neighbor router IDs to advertise by scanning
  `state->neighbors[0 .. OSPF_MAX_NEIGHBORS - 1]`. Count a slot only when:
  - `neighbor->valid` is nonzero
  - `neighbor->iface == iface`
  - `neighbor->state != OSPF_NBR_DOWN`
- Calculate the exact OSPF payload length as `sizeof(OspfHeader) +
  sizeof(OspfHello) + neighbor_count * sizeof(uint32_t)`. This value fits in
  the 16-bit OSPF `pkt_len` field because `neighbor_count` is bounded by
  `OSPF_MAX_NEIGHBORS`.
- Allocate `Packet *pkt` with `packet_create(payload_len)`. If allocation
  fails, return `-1`. Set `pkt->len = payload_len`.
- Interpret `pkt->data` as `OspfHeader *header` and the bytes immediately after
  that header as `OspfHello *hello`.
- Write `header` field by field:
  - `version = OSPF_VERSION`
  - `type = OSPF_TYPE_HELLO`
  - `pkt_len = payload_len` converted with `ns_htons`
  - `router_id = state->router_id` in network byte order
  - `area_id = state->area_id` in network byte order
  - `checksum = 0` in this milestone
  - `au_type = ns_htons(0)`
  - `auth_data = 0`
- Include `src/common/ip_utils.h` from `ospf.c`.
- Set `uint32_t mask = ipv4_prefix_mask(iface->prefix_len)`. The helper returns
  the required host-order mask; do not repeat the prefix-length shift logic in
  `ospf_send_hello`.
- Write `hello` field by field:
  - `network_mask = ns_htonl(mask)`
  - `hello_interval = ns_htons(OSPF_HELLO_INTERVAL_US / 1000000)`
  - set `options = 0`
  - set `router_priority = 0`
  - `dead_interval = ns_htonl(OSPF_DEAD_INTERVAL_US / 1000000)`
  - `dr = ns_htonl(0)`
  - `bdr = ns_htonl(0)`
- Interpret the bytes immediately after `OspfHello` as the start of an array of
  network-order `uint32_t` router IDs:

  ```c
  uint32_t *neighbor_ids =
      (uint32_t *)(pkt->data + sizeof(OspfHeader) + sizeof(OspfHello));
  ```

- Set `neighbor_index = 0`.
- Scan `state->neighbors[0 .. OSPF_MAX_NEIGHBORS - 1]` again in increasing slot
  order. For each slot:
  - let `neighbor = &state->neighbors[i]`
  - if `neighbor->valid == 0`, skip this slot
  - if `neighbor->iface != iface`, skip this slot
  - if `neighbor->state == OSPF_NBR_DOWN`, skip this slot
  - write `neighbor_ids[neighbor_index] = ns_htonl(neighbor->router_id)`
  - increment `neighbor_index`
- After the scan, if `neighbor_index != neighbor_count`, free `pkt` and return
  `-1`. This check detects disagreement between the counting pass and the
  writing pass.
- Read the source address as `src_ip = ns_ntohl(iface->ip_addr)` because
  `Interface.ip_addr` is stored in network order and `ip_output` accepts a
  host-order source address.
- Call `ip_output(state->sim, src_ip, OSPF_ALLSPFROUTERS, OSPF_PROTO_NUM, pkt)`
  and save its result.
- Call `packet_free(pkt)` after `ip_output` returns, regardless of whether IP
  output succeeded or failed.
- If the saved IP-output result is negative, return `-1`; otherwise return `0`.

Do not route Hello packets through ordinary unicast forwarding. They are local
control-plane packets sent to link-local multicast.

### `ospf_flood_lsa`

Purpose:

Flood one already-selected LSA to every OSPF-enabled interface except the
interface where the LSA was received.

Implementation task:

Given one host-order LSDB entry, construct a separate single-LSA LSU packet for
each eligible OSPF interface and send it to the AllSPFRouters multicast group.
Skip the optional ingress interface to prevent immediate reflection. This
function serializes and sends an existing LSA; it does not decide freshness,
store the LSA, or schedule SPF.

Inputs and existing state:

- `state` supplies local OSPF identity, simulator access, and enabled
  interfaces.
- `lsa` is borrowed host-order link-state data and must not be modified.
- `except_iface` is an optional borrowed interface to exclude.

Result:

- Return `0` after every eligible send succeeds, including when no interface is
  eligible.
- Return `-1` for invalid input, invalid link count, packet allocation failure,
  or the first failed output.

Required behavior:

- Send at most one packet per eligible interface.
- Produce network-order LSU bytes without modifying the source LSA.
- Never send through `except_iface`.
- Release each caller-owned packet after multicast output returns.

Implementation order:

- If state == NULL || lsa == NULL, return -1.
- If state->sim == NULL || state->router == NULL, return -1.
- If lsa->link_count is less than 0 or greater than OSPF_MAX_LSA_LINKS, return -1.
- Scan state->ifaces[0 .. OSPF_MAX_IFACES - 1].
- For each slot:
  - if slot.valid == 0, skip it.
  - let out_iface = slot.iface.
  - if out_iface == NULL, skip it.
  - if except_iface != NULL && out_iface == except_iface, skip it.
  - create one `Packet *pkt` whose data bytes contain exactly one OSPF LSU
    carrying this one LSA:
    - allocate/create `pkt` with data capacity:
      `sizeof(OspfHeader) + sizeof(OspfLsuHeader) + sizeof(OspfLsaWire) +
      lsa->link_count * sizeof(OspfLsaLink)`
    - use `pkt->data` as the start of the OSPF bytes; do not cast `pkt`
      itself to an OSPF header
    - compute layout addresses as byte offsets from `pkt->data`:
      - `OspfHeader *ospf_hdr = (OspfHeader *)pkt->data`
      - `OspfLsuHeader *lsu_hdr = (OspfLsuHeader *)(pkt->data + sizeof(OspfHeader))`
      - `OspfLsaWire *lsa_wire = (OspfLsaWire *)(pkt->data + sizeof(OspfHeader) + sizeof(OspfLsuHeader))`
      - `OspfLsaLink *links = (OspfLsaLink *)(pkt->data + sizeof(OspfHeader) + sizeof(OspfLsuHeader) + sizeof(OspfLsaWire))`
    - fill `ospf_hdr` field by field:
      - version = OSPF_VERSION
      - type = OSPF_TYPE_LSU
      - pkt_len = total LSU byte length in network byte order
      - router_id = state->router_id in network byte order
      - area_id = state->area_id in network byte order
      - checksum = 0 in this milestone
      - au_type = 0 in network byte order
      - auth_data = 0
    - fill `lsu_hdr` field by field:
      - lsa_count = 1 in network byte order
      - reserved = 0 in network byte order
    - fill `lsa_wire` field by field from `lsa`:
      - lsa_id = lsa->lsa_id in network byte order
      - adv_router = lsa->adv_router in network byte order
      - seq_num = lsa->seq_num in network byte order
      - checksum = lsa->checksum in network byte order
      - lsa_type = lsa->lsa_type
      - link_count = lsa->link_count
    - write every `lsa->links[k]` into `links[k]` for
      `0 <= k < lsa->link_count`; do not repeatedly write `links[0]`
    - when writing each link, convert multi-byte link fields to network byte
      order:
      - link_id
      - link_data
      - metric
    - copy one-byte link fields directly:
      - type
      - num_tos
    - after writing the bytes, set the Packet's valid data length to the full
      LSU byte length according to the Packet API used by the implementation
  - if packet creation fails, return -1.
  - compute source IP as `ns_ntohl(out_iface->ip_addr)`.
  - send that packet through IP output and save the result, using:
    - sim = state->sim
    - src_ip = source IP from out_iface
    - dst_ip = OSPF_ALLSPFROUTERS
    - protocol = OSPF_PROTO_NUM
    - payload = pkt
  - call `packet_free(pkt)` after multicast IP output returns; the scheduled
    link path has cloned the completed frame on success
  - if the saved output result is negative, return `-1`
- Return 0.

Postconditions:

- `except_iface` is never used as an output interface.
- If no eligible output interface exists, return 0.
- The input `lsa` is not modified.

### Internal SPF-vertex helper

Purpose:

Maintain and query the temporary router-ID-to-SPF-vertex mapping used by one
`ospf_run_spf` invocation.

Implementation task:

The ensure helper supports the vertex-construction phase by returning the
existing index for a router ID or initializing the next unused vertex. The
find helper supports the Dijkstra phase by locating an already constructed
vertex without changing the vertex set.

Inputs and existing state:

- `vertices` is caller-owned temporary SPF workspace.
- `vertex_count` identifies the populated prefix of that workspace.
- `router_id` is the host-order lookup key.

Result:

- Both helpers return a populated vertex index on success and `-1` when their
  requested operation cannot be completed.
- Only the ensure helper may initialize a vertex and increase the count.

Use one file-local helper to find an existing SPF vertex or add a new one:

```c
static int ospf_ensure_spf_vertex(OspfSpfVertex *vertices,
                                  int           *vertex_count,
                                  uint32_t       router_id);
```

Required behavior:

- Search only the populated range `vertices[0 .. *vertex_count - 1]`.
- If that range already contains a vertex whose `router_id` equals the supplied
  `router_id`, leave the array and count unchanged and return its index.
- Otherwise, if the array has capacity, initialize the next unused vertex for
  that router ID, increase `*vertex_count`, and return the new index.
- If no new vertex can be added because `*vertex_count` has reached
  `OSPF_SPF_MAX_NODES`, return `-1` without changing the array or count.
- The helper does not assign source-specific values. Every vertex created by
  this helper starts as an ordinary unreachable, unvisited vertex.

Implementation order:

1. Scan indexes `0` through `*vertex_count - 1` for a matching `router_id`.
2. If a match is found, return that index immediately.
3. If `*vertex_count >= OSPF_SPF_MAX_NODES`, return `-1`.
4. Use `*vertex_count` as the new vertex index.
5. Initialize that vertex with the supplied `router_id`, distance
   `OSPF_INFINITY`, predecessor `-1`, first hop `-1`, and visited false.
6. Increase `*vertex_count` by one and return the new index.

Use a second file-local helper when SPF needs to find a vertex without adding
one:

```c
static int ospf_find_spf_vertex(const OspfSpfVertex *vertices,
                                int                  vertex_count,
                                uint32_t             router_id);
```

Required behavior:

- Search only `vertices[0 .. vertex_count - 1]`.
- If a vertex whose `router_id` equals the supplied `router_id` exists, return
  its index.
- If no matching vertex exists, return `-1`.
- Do not modify the vertex array or vertex count.

Implementation order:

1. Scan indexes `0` through `vertex_count - 1` in increasing order.
2. If `vertices[i].router_id == router_id`, return `i` immediately.
3. If the complete populated range is scanned without a match, return `-1`.

Use `ospf_ensure_spf_vertex` only while constructing the vertex set, when a
missing router must be added. Use `ospf_find_spf_vertex` during Dijkstra, when
the vertex set is already complete and a missing router is an internal
inconsistency.

### `ospf_run_spf`

Purpose:

This milestone runs SPF over router-to-router links only. Stub/network routes
can be added later when their LSA representation is fully specified.

Implementation task:

Given the local router's current LSDB, create a temporary vertex set and run
Dijkstra from the local router ID over directed point-to-point links. Record a
distance, predecessor, and first hop for each reachable router. After the
calculation completes, replace old OSPF routes with `/32` routes whose next-hop
IP and egress interface are resolved through the live neighbor table. The
temporary SPF tree exists only for this invocation.

Inputs and existing state:

- `state->lsdb[]` is the persistent topology input and is not modified.
- `state->neighbors[]` maps calculated first-hop router IDs to usable adjacent
  IP/interface pairs.
- `state->router->route_tbl` receives the resulting OSPF routes.
- Router IDs, link metrics, neighbor IPs, and route-table arguments are host
  order.

Result:

- Return `0` after calculation and the complete eligible installation pass.
- Return `-1` for invalid state, inconsistent SPF workspace, or route insertion
  failure.
- The temporary vertices disappear on return; successfully installed routes
  are not rolled back if a later insertion fails.

Required behavior:

- If `state == NULL || state->router == NULL`, return `-1`.
- Use the valid router LSAs in `state->lsdb[]` as the topology input and compute
  the shortest paths from `state->router_id` over point-to-point links.
- Do not modify the LSDB, neighbor table, interface table, or persistent OSPF
  state while calculating SPF.
- Treat a point-to-point link advertised by router A to router B as the
  directed edge A to B. Do not create an unadvertised reverse edge.
- Ignore transit and stub links in this milestone.
- For every reachable remote router, determine its total cost and first-hop
  router. Equal-cost results must be deterministic: prefer the lower router ID
  when selecting the next unvisited vertex, and keep the already selected path
  when a new path has exactly the same cost.
- Replace the Router's old OSPF routes only after the complete SPF calculation
  succeeds.
- Install eligible reachable router IDs as `/32` OSPF routes using the live
  first-hop neighbor's IP address and interface.
- Return `0` after a successful calculation and installation pass, including
  when no remote route is eligible. Return `-1` on invalid input, calculation
  failure, or route insertion failure.

Implementation order:

1. If `state == NULL || state->router == NULL`, return `-1`.
2. Create a function-local `OspfSpfVertex vertices[OSPF_SPF_MAX_NODES]` array
   and set a local populated-vertex count to zero.
3. Add the vertex for `state->router_id` first. Initialize it with distance
   zero, predecessor `-1`, first hop `-1`, and visited false. This is the source
   vertex even if the LSDB has no local router LSA.
4. Scan all `OSPF_LSDB_SIZE` LSDB slots. Skip a slot unless it is valid and its
   `lsa_type` is `OSPF_LSA_TYPE_ROUTER`.
5. For each accepted router LSA:
   - call `ospf_ensure_spf_vertex` with the LSA's `adv_router`; if it returns
     `-1`, return `-1` from `ospf_run_spf`
   - scan its valid link range `links[0 .. link_count - 1]`
   - ignore links whose type is not `OSPF_LINK_P2P`
   - for each point-to-point link, call `ospf_ensure_spf_vertex` with the link's
     `link_id`; if it returns `-1`, return `-1` from `ospf_run_spf`
   - do not use the LSDB index or link index as a vertex index; the helper's
     return value is the vertex index for the requested router ID
6. Start one outer Dijkstra loop that repeats until explicitly stopped. Each
   pass through this outer loop performs one Dijkstra iteration:
   1. Declare a local `int selected_index` and initialize it to `-1`. In this
      iteration, `-1` means that the vertex scan has not found an eligible
      vertex yet.
   2. Run an inner vertex-selection loop over
      `vertices[0 .. vertex_count - 1]` in increasing index order. For each
      index `i`:
      - if `vertices[i].visited` is nonzero, skip index `i`
      - if `vertices[i].distance == OSPF_INFINITY`, skip index `i`
      - if `selected_index == -1`, assign `selected_index = i` because this is
        the first eligible vertex found in this iteration
      - otherwise, if `vertices[i].distance` is less than
        `vertices[selected_index].distance`, assign `selected_index = i`
      - otherwise, if the two distances are equal and `vertices[i].router_id`
        is less than `vertices[selected_index].router_id`, assign
        `selected_index = i`
      - otherwise leave `selected_index` unchanged
   3. After the inner vertex-selection loop finishes, test `selected_index`.
      If it is `-1`, the scan found no unvisited vertex with a finite distance:
      break out of the outer Dijkstra loop and proceed to implementation step
      7, which updates the route table. Otherwise remain in the current outer
      iteration and use `vertices[selected_index]` as the selected vertex.
   4. Let `selected` refer to the selected vertex and set `selected->visited`
      to nonzero.
   5. Scan all `state->lsdb[0 .. OSPF_LSDB_SIZE - 1]` slots. For this iteration,
      accept a slot only when it is valid, has type `OSPF_LSA_TYPE_ROUTER`, and
      its `adv_router` equals `selected->router_id`.
   6. For each accepted LSA, scan `links[0 .. link_count - 1]`. Skip every link
      whose type is not `OSPF_LINK_P2P`.
   7. For each accepted point-to-point link, call
      `ospf_find_spf_vertex(vertices, vertex_count, link->link_id)` and store
      its return value in `link_vertex_index`. This index identifies the vertex
      at the far end of the current link. If the helper returns `-1`, return
      `-1` because vertex construction and LSDB traversal disagree. Otherwise
      let `link_vertex` refer to `vertices[link_vertex_index]`.
   8. If `link_vertex` is already visited, leave it unchanged and
      continue with the next link.
   9. If `selected->distance >= OSPF_INFINITY - link->metric`, leave the
      link vertex unchanged because the addition would overflow or produce the
      reserved infinity value. Otherwise calculate candidate distance as
      `selected->distance + link->metric`.
   10. If the candidate distance is greater than or equal to `link_vertex`'s
       current distance, leave `link_vertex` unchanged and continue with the
       next link. Processing every duplicate edge this way naturally allows
       only its lowest-cost result to remain.
   11. Store the candidate distance in `link_vertex->distance` and store the
       selected-vertex index in `link_vertex->predecessor`.
   12. If the selected-vertex index is `0`, store `link_vertex_index` in
       `link_vertex->first_hop`. Otherwise, first verify that
       `selected->first_hop` is in the populated vertex-index range
       `0 .. vertex_count - 1`. If it is outside that range, return `-1`
       because a reachable non-source selected vertex must already identify a
       valid first-hop vertex. If it is valid, copy `selected->first_hop` into
       `link_vertex->first_hop`.
   13. Complete the link loop for each matching LSA, then complete the LSDB
       loop for the selected vertex. After both loops finish, start the next
       outer Dijkstra iteration by resetting `selected_index` to `-1` and
       repeating the vertex-selection scan in substeps 1 through 3.
7. After Dijkstra finishes successfully, call `route_table_flush_proto` to
   remove the old OSPF routes. Its nonnegative return value is the number of
   routes removed, not a success/failure status; do not treat a nonzero value
   as an error.

```c
route_table_flush_proto(&state->router->route_tbl, ROUTE_PROTO_OSPF);
```

8. Scan route-candidate indexes `0 .. vertex_count - 1` in increasing index
   order. For each `route_vertex_index`:
   1. Let `route_vertex` refer to `vertices[route_vertex_index]`.
   2. If `route_vertex_index == 0`, skip it because index zero is the local
      source router and must not receive a route through itself.
   3. If `route_vertex->distance == OSPF_INFINITY`, skip it because the router
      is unreachable.
   4. Read `first_hop_index = route_vertex->first_hop`. If
      `first_hop_index < 0 || first_hop_index >= vertex_count`, return `-1`
      because every reachable non-source destination must identify a populated
      first-hop vertex.
   5. Read `first_hop_router_id = vertices[first_hop_index].router_id`.
   6. Set a local neighbor index to `-1`, then scan
      `state->neighbors[0 .. OSPF_MAX_NEIGHBORS - 1]` in increasing index
      order. A neighbor matches only when all of the following are true:
      - `neighbor->valid` is nonzero
      - `neighbor->router_id == first_hop_router_id`
      - `neighbor->state != OSPF_NBR_DOWN`
      - `neighbor->iface != NULL`
   7. On the first matching neighbor, store its index and stop the neighbor
      scan. If the neighbor index remains `-1` after the scan, skip this
      route candidate and continue with the next `route_vertex_index`.
   8. Let `neighbor` refer to
      `state->neighbors[neighbor_index]`. This is the directly adjacent OSPF
      router selected as the first hop toward `route_vertex`; it may differ
      from the final destination router.
   9. Call `router_add_route` to add the `/32` route with:
      - destination prefix: `route_vertex->router_id`
      - prefix length: `32`
      - next-hop address: `neighbor->ip_addr`
      - egress interface: `neighbor->iface`
      - metric: `route_vertex->distance`
      - protocol: `ROUTE_PROTO_OSPF`
      The two address arguments, `route_vertex->router_id` and
      `neighbor->ip_addr`, are already host-order values; pass them directly
      without byte-order conversion.
   10. If `router_add_route` returns nonzero, return `-1`. Routes already
       installed by this run remain installed. Otherwise continue with the next
       `route_vertex_index`.
9. Return `0`.

Network/stub route installation can be added after router-LSA link semantics
are fully represented.

### `ospf_hello_timer`

Purpose:

Send the current periodic Hello on every enabled OSPF interface and continue
the Hello timer sequence.

Implementation task:

Given borrowed OSPF state as scheduler callback context, visit every valid
OSPF interface, send one Hello through each non-NULL interface, and then
schedule the next Hello occurrence. One failed interface send does not prevent
the other enabled interfaces from being attempted or the next timer from being
scheduled.

Inputs and existing state:

- `e` is the borrowed event that caused the callback; its fields are not needed.
- `ctx` must point to the borrowed `OspfState` used for interface and scheduler
  access.

Result:

- This callback returns no status and does not free the event or state.
- It may send zero or more packets and schedule one later Hello event.

Required behavior:

- Do nothing when `ctx == NULL`.
- Attempt one Hello only for slots that are valid and have a non-NULL interface.
- Continue the periodic sequence when a scheduler exists.

Implementation order:

1. Ignore `e` and return if `ctx == NULL`.
2. Cast `ctx` to `OspfState *state`.
3. Scan all OSPF interface slots; call `ospf_send_hello` for each valid slot
   whose interface pointer is non-NULL.
4. Call `ospf_schedule_periodic` for `EVT_OSPF_HELLO`, delay
   `OSPF_HELLO_INTERVAL_US`, handler `ospf_hello_timer`, and context `state`.

### `ospf_dead_timer`

Purpose:

Expire neighbors that have stopped sending Hellos, request SPF when reachability
changes, and continue the dead-neighbor scan sequence.

Implementation task:

At the current simulated time, examine every known live neighbor and move each
one whose last valid Hello is at least one dead interval old to
`OSPF_NBR_DOWN`. Preserve the neighbor record so a later Hello can revive it.
If at least one state changes, schedule one delayed SPF calculation, then
schedule the next dead-neighbor scan.

Inputs and existing state:

- `e` is the borrowed triggering event and is not inspected.
- `ctx` identifies the borrowed OSPF state, neighbor table, and scheduler.

Result:

- This callback returns no status and does not free the event or state.
- It may change neighbor states, schedule at most one SPF event, and schedule
  one later dead-neighbor event.

Required behavior:

- Do nothing without valid callback context and scheduler access.
- Use one `now` value for the entire expiration scan.
- Preserve slot validity, interface association, and neighbor count when a
  neighbor becomes down.
- Schedule no more than one SPF event for one scan, regardless of how many
  neighbors expire.

Implementation order:

1. Ignore `e`; return if `ctx == NULL`.
2. Cast `ctx` to `OspfState *state`; return if its simulator or scheduler is
   missing.
3. Read `now = scheduler_now(state->sim->sched)` and initialize a local
   neighbor-changed flag to false.
4. Scan every neighbor slot. Skip invalid slots, already-down neighbors,
   timestamps later than `now`, and neighbors younger than
   `OSPF_DEAD_INTERVAL_US`. For every remaining neighbor, set its state to
   `OSPF_NBR_DOWN` and set the changed flag.
5. Do not clear `valid` or `iface` and do not decrement `neighbor_count`.
6. If the changed flag is true, create and schedule one `EVT_OSPF_SPF` callback
   at `now + OSPF_SPF_DELAY_US` with handler `ospf_spf_timer` and context
   `state`; free the event if scheduler insertion fails.
7. Call `ospf_schedule_periodic` for `EVT_OSPF_DEAD`, delay
   `OSPF_DEAD_INTERVAL_US`, handler `ospf_dead_timer`, and context `state`.

### `ospf_spf_timer`

Purpose:

Run one delayed SPF recalculation requested by an OSPF topology change.

Implementation task:

Given OSPF state as scheduler callback context, invoke the SPF calculation once.
This is a one-shot event handler; `ospf_run_spf` and future topology changes
decide what routes or later SPF events are needed.

Inputs and existing state:

- `e` is the borrowed triggering event and is not inspected.
- `ctx` must point to the borrowed OSPF state passed to `ospf_run_spf`.

Result:

- This callback returns no status and does not free the event or state.
- The return value from `ospf_run_spf` cannot be propagated through the callback
  signature and is ignored.

Required behavior:

- Do nothing when `ctx == NULL`.
- Invoke `ospf_run_spf` exactly once for valid context.

Implementation order:

1. Ignore `e` and return if `ctx == NULL`.
2. Cast `ctx` to `OspfState *state`.
3. Call `ospf_run_spf(state)` once and ignore its return value.

## Flow Charts

### Initialization

```text
ospf_init(state, sim, router, router_id)
  |
  +-- null state: return
  +-- zero state
  +-- store sim/router/router_id/area 0
  +-- schedule EVT_OSPF_HELLO with ospf_hello_timer if scheduler exists
```

### Hello Receive

```text
ospf_receive(iface, pkt, state)
  |
  +-- validate OSPF header
  +-- type == Hello
  +-- find or create neighbor by router_id
  +-- update last_hello_ts
  +-- if our router_id is in neighbor list: TWOWAY/FULL
  +-- else INIT
  +-- free packet
```

### LSA Flood And SPF

```text
LSU received
  |
  +-- for each LSA:
        |
        +-- older or duplicate: ignore
        +-- newer:
              store in LSDB
              flood to other OSPF interfaces
              schedule SPF after OSPF_SPF_DELAY_US
```

### SPF

```text
ospf_run_spf(state)
  |
  +-- build temporary SPF vertices from LSDB
  +-- run Dijkstra from local router_id
  +-- route_table_flush_proto(... OSPF)
  +-- router_add_route(... OSPF) for reachable results
```

## ACSL Contracts

The contracts belong in `ospf.h`. Use literal bounds:

- neighbors: `32`
- LSDB entries: `256`
- OSPF interfaces: `16`
- links per LSA: `4`
- OSPF header bytes: `24`

### Shared Predicates

```c
/*@
    predicate ospf_neighbor_count_valid(OspfState *state) =
        0 <= state->neighbor_count && state->neighbor_count <= 32;

    predicate ospf_lsdb_count_valid(OspfState *state) =
        0 <= state->lsdb_count && state->lsdb_count <= 256;

    predicate ospf_iface_count_valid(OspfState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate ospf_lsa_slot_valid(OspfState *state, integer i) =
        0 <= i && i < 256 ==>
            (state->lsdb[i].valid == 0 ||
             (state->lsdb[i].valid == 1 &&
              0 <= state->lsdb[i].link_count &&
              state->lsdb[i].link_count <= 4));

    predicate ospf_state_well_formed(OspfState *state) =
        \valid(state) &&
        ospf_neighbor_count_valid(state) &&
        ospf_lsdb_count_valid(state) &&
        ospf_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 256 ==>
            ospf_lsa_slot_valid(state, i);
*/
```

### `ospf_init`

```c
/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->router_id,
                state->area_id,
                state->neighbors[0 .. 31],
                state->neighbor_count,
                state->lsdb[0 .. 255],
                state->lsdb_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->sim,
                state->router;
        ensures state->router_id == router_id;
        ensures state->area_id == 0;
        ensures state->neighbor_count == 0;
        ensures state->lsdb_count == 0;
        ensures state->iface_count == 0;
        ensures state->sim == sim;
        ensures state->router == router;

    complete behaviors;
    disjoint behaviors;
*/
void ospf_init(OspfState *state,
               Simulator *sim,
               Router *router,
               uint32_t router_id);
```

### `ospf_enable_iface`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null || cost == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid(iface);
        assumes cost != 0;
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_enable_iface(OspfState *state, Interface *iface, uint16_t cost);
```

### `ospf_receive`

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

    behavior bad_ctx:
        assumes iface != \null && pkt != \null && ctx == \null;
        assigns \everything;
        ensures \result == -1;

    behavior valid:
        assumes \valid(iface);
        assumes pkt != \null;
        assumes ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns \everything;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_receive(Interface *iface, Packet *pkt, void *ctx);
```

### `ospf_send_hello`

```c
/*@
    behavior bad_input:
        assumes state == \null || iface == \null ||
                (state != \null && state->sim == \null) ||
                (iface != \null && iface->prefix_len > 32);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid(iface);
        assumes state->sim != \null;
        assumes iface->prefix_len <= 32;
        assigns \everything;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_send_hello(OspfState *state, Interface *iface);
```

`ospf_send_hello` uses `assigns \everything` because it builds packet bytes and
calls the IP output path. A narrower contract needs modeled packet allocation,
IP output, ARP/cache, and interface transmit effects.

### `ospf_flood_lsa`

```c
/*@
    behavior bad_input:
        assumes state == \null || lsa == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid_read(lsa);
        assigns \everything;
        ensures \result >= -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_flood_lsa(OspfState          *state,
                   const OspfLsaEntry *lsa,
                   Interface          *except_iface);
```

`except_iface` may be `NULL`. When non-NULL, it is the ingress interface that
must be skipped while flooding. The contract still uses `assigns \everything`
because flooding sends packets on zero or more interfaces.

### `ospf_run_spf`

```c
/*@
    behavior bad_input:
        assumes state == \null || state->router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes state->router != \null;
        assigns state->router->route_tbl.rib[0 .. 255],
                state->router->route_tbl.rib_count,
                state->router->route_tbl.fib[0 .. 255],
                state->router->route_tbl.fib_count,
                state->router->route_tbl.next_sequence;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int ospf_run_spf(OspfState *state);
```

### Timer Handlers

```c
/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns \everything;
*/
void ospf_hello_timer(const Event *e, void *ctx);
void ospf_dead_timer(const Event *e, void *ctx);
void ospf_spf_timer(const Event *e, void *ctx);
```

`ospf_receive` and the timer handlers intentionally use `assigns \everything`
for the paths that can free packets, send packets, flood LSAs, schedule events,
or update Router/RouteTable state. Narrower contracts require precise modeled
contracts for IP output, packet lifetime, scheduler insertion, and Router route
updates first.

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `ospf_init(NULL, ...)` does not crash.
2. Valid init clears neighbor, LSDB, and interface counts.
3. Valid init stores router ID, area ID, simulator, and router.
4. Valid init schedules first Hello when scheduler exists.
5. `ospf_enable_iface` rejects NULL state, NULL interface, and zero cost.
6. `ospf_enable_iface` adds first interface and cost.
7. `ospf_enable_iface` updates cost for duplicate interface.
8. `ospf_enable_iface` rejects full interface list.
9. `ospf_receive` rejects NULL interface.
10. `ospf_receive` with NULL packet increments `rx_errors`.
11. `ospf_receive` with NULL context frees packet and increments `rx_errors`.
12. Too-short OSPF packet is rejected.
13. Wrong OSPF version is rejected.
14. Wrong area ID is rejected.
15. Bad packet length is rejected.
16. Hello creates or updates neighbor entry.
17. Hello without our router ID leaves neighbor in INIT.
18. Hello listing our router ID moves neighbor to TWOWAY or FULL.
19. Newer LSA updates LSDB.
20. Duplicate or older LSA is ignored.
21. Newer LSA is flooded except on ingress interface.
22. Newer LSA schedules SPF.
23. SPF flushes old OSPF routes.
24. SPF installs reachable routes as `ROUTE_PROTO_OSPF`.
25. Dead timer marks stale neighbor down.
26. Dead neighbor schedules SPF.
27. OSPF protocol 89 receive path is covered by Router/IP integration test.
28. OSPF multicast send path is covered by an IP multicast output integration
    test for `224.0.0.5`.

## Common Mistakes

- Do not implement OSPF over UDP.
- Do not let Router forwarding treat protocol 89 control packets as ordinary
  transit traffic when they are addressed to the router/control plane.
- Do not write directly into route-table arrays.
- Do not make forwarding read the LSDB.
- Do not use milliseconds for scheduler timestamps.
- Do not build Ethernet multicast MAC addresses in OSPF; IP output owns that
  mapping.
- Do not run SPF after every LSA immediately if a debounce timer is available.
- Do not forget to flush old OSPF routes before installing new SPF results.
- Do not free Router, Simulator, or interfaces from OSPF state.
