# Module 22 — OSPF (Open Shortest Path First v2)

**Files:** `src/protocols/ospf.c`, `src/protocols/ospf.h`
**Status:** ⬜ Not started
**Depends on:** ip, scheduler, route_table, simulator

---

## The Problem

RIP's hop-count metric and 30-second convergence are inadequate for
larger networks. OSPF is a **link-state** protocol: every router floods
a description of its own directly-connected links (an LSA) to all other
routers. Each router then independently runs **Dijkstra's shortest-path
algorithm** on its complete map (LSDB) and installs the results into its
routing table. Key advantages:

- **Metric = cost** (not hop count) — maps to bandwidth.
- **Converges in seconds**, not minutes.
- **No count-to-infinity** — each router has the full topology.
- **Areas** subdivide large networks to limit flooding scope.

This simulator implements OSPFv2 (IPv4), single-area (area 0 only).

## OSPF Protocol Overview

```
   Phase 1 — Neighbor Discovery (Hello packets every HelloInterval)
   Phase 2 — Database Exchange  (DBD, LSR, LSU, LSAck)
   Phase 3 — Flooding           (LSA flooded to all neighbors except sender)
   Phase 4 — SPF Calculation    (Dijkstra on LSDB after topology stabilizes)
   Phase 5 — Route Installation (best paths → route_table_add)
```

---

## Header File — `ospf.h`

### Constants

| Macro                     | Value    | Use                                  |
|---------------------------|----------|--------------------------------------|
| `OSPF_HELLO_INTERVAL`     | `10000`  | ms — Hello period                    |
| `OSPF_DEAD_INTERVAL`      | `40000`  | ms — neighbor declared dead          |
| `OSPF_LSDB_SIZE`          | `256`    | Max LSAs in the link-state database  |
| `OSPF_MAX_NEIGHBORS`      | `32`     | Per-router neighbor limit            |
| `OSPF_MAX_IFACES`         | `16`     | OSPF-enabled interfaces              |
| `OSPF_PROTO_NUM`          | `89`     | IP protocol number                   |
| `OSPF_ALLROUTERS`         | `0xE0000005` | 224.0.0.5 — all OSPF routers   |
| `OSPF_INFINITY`           | `0xFFFF` | Unreachable link cost                |

### OSPF Packet Types

| Value | Name          |
|-------|---------------|
| `1`   | Hello         |
| `2`   | DBD (Database Description) |
| `3`   | LSR (Link State Request)   |
| `4`   | LSU (Link State Update)    |
| `5`   | LSAck                      |

### OSPF Common Header (24 bytes, packed)

```c
typedef struct __attribute__((packed)) OspfHeader {
    uint8_t  version;      // 2
    uint8_t  type;         // 1..5
    uint16_t pkt_len;      // total bytes including header
    uint32_t router_id;    // 32-bit RID (usually highest IP)
    uint32_t area_id;      // 0 = backbone
    uint16_t checksum;
    uint16_t au_type;      // 0 = no auth
    uint64_t auth_data;    // 0
} OspfHeader;              // 24 bytes
```

### Hello Packet (additional 20 bytes after header)

```c
typedef struct __attribute__((packed)) OspfHello {
    uint32_t network_mask;
    uint16_t hello_interval;
    uint8_t  options;
    uint8_t  router_priority;
    uint32_t dead_interval;
    uint32_t dr;           // Designated Router (0 if none)
    uint32_t bdr;          // Backup DR
    // neighbor list follows (variable length, each 4 bytes = router_id)
} OspfHello;               // 20 bytes
```

### Router LSA body entry (12 bytes per link)

```c
typedef struct __attribute__((packed)) OspfLsaLink {
    uint32_t link_id;      // IP of neighbor router (p2p) or network IP
    uint32_t link_data;    // local interface IP
    uint8_t  type;         // 1=p2p, 2=transit, 3=stub
    uint8_t  num_tos;      // 0
    uint16_t metric;       // link cost
} OspfLsaLink;             // 12 bytes
```

### Neighbor State Machine

```c
typedef enum {
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

### `OspfNeighbor` Struct (48 bytes)

```c
typedef struct OspfNeighbor {
    uint32_t      router_id;
    uint32_t      ip_addr;
    OspfNbrState  state;
    uint64_t      last_hello_ts;   // ms
    Interface    *iface;
    int           valid;
    int           _pad;
} OspfNeighbor;               // 40 bytes
```

### `OspfLsaEntry` Struct (LSDB entry, 64 bytes)

```c
typedef struct OspfLsaEntry {
    uint32_t      lsa_id;          // advertising router ID
    uint32_t      adv_router;
    uint32_t      seq_num;         // monotonically increasing
    uint16_t      checksum;
    uint8_t       lsa_type;        // 1=router, 2=network (simplified)
    uint8_t       valid;
    OspfLsaLink   links[4];        // 4 × 12 = 48 B (simplified)
    int           link_count;
} OspfLsaEntry;                   // 68 bytes (simplified — 4 links max)
```

### `OspfState` Struct (per Device, ≈ 20 KB)

```c
typedef struct OspfState {
    uint32_t      router_id;
    OspfNeighbor  neighbors[32];      // 32 × 40  = 1 280 B
    OspfLsaEntry  lsdb[256];          // 256 × 68 = 17 408 B
    int           lsdb_count;
    Interface    *ifaces[16];
    int           iface_count;
    Simulator    *sim;
    Device       *dev;
} OspfState;                          // ≈ 18 KB
```

### Public API

| Function                            | Purpose                                         |
|-------------------------------------|-------------------------------------------------|
| `ospf_init(sim, dev, router_id)`    | Zero state; bind IP proto 89; schedule Hello.   |
| `ospf_enable_iface(state, iface, cost)` | Add NIC to OSPF; set link cost.            |
| `ospf_receive(iface, pkt, sim)`     | Dispatch on type (Hello/DBD/LSU/LSAck).         |
| `ospf_send_hello(state, iface)`     | Periodic neighbor keepalive.                    |
| `ospf_flood_lsa(state, lsa, except_iface)` | Send LSU to all OSPF ifaces except source. |
| `ospf_run_spf(state)`               | Dijkstra on lsdb; call route_table_add.         |
| `ospf_hello_timer(e, ctx)`          | Fires every OSPF_HELLO_INTERVAL.                |
| `ospf_dead_timer(e, ctx)`           | Fires if no Hello from neighbor in DEAD_INTERVAL.|

---

## Dispatch Table

```
Scheduler.handlers[]
┌────────────────────┬────────────────────┬────────────┐
│ EVT_OSPF_HELLO     │ ospf_hello_timer   │ ospf_state │
│ EVT_OSPF_DEAD      │ ospf_dead_timer    │ ospf_state │
│ EVT_OSPF_SPF       │ ospf_spf_timer     │ ospf_state │
└────────────────────┴────────────────────┴────────────┘

ip_receive: proto == 89 → ospf_receive(iface, pkt, sim)
```

---

## Function Call Sequence — Neighbor Adjacency

```
ospf_hello_timer(e, ospf_state):
   ├─► ospf_send_hello(state, each iface)
   │       build OspfHeader{type=1} + OspfHello{hello=10, dead=40}
   │       append neighbor list (router_ids of state->neighbors[])
   │       ip_send(sim, iface->ip_addr, 224.0.0.5, OSPF_PROTO_NUM, pkt)
   └─► reschedule EVT_OSPF_HELLO in 10 000 ms

[neighbor receives Hello]
ospf_receive → type==1 → ospf_process_hello(state, iface, pkt):
   │   nbr = find or create OspfNeighbor for sender's router_id
   │   nbr->last_hello_ts = now
   │
   ├─ if nbr->state < TWOWAY: nbr->state = INIT
   │
   ├─ if my router_id found in Hello's neighbor list:
   │       nbr->state = TWOWAY
   │       schedule EVT_OSPF_DEAD at now + 40 000 (reset if exists)
   │
   └─ if nbr->state == TWOWAY: begin database exchange (EXSTART → FULL)
```

## Function Call Sequence — SPF After LSA Flood

```
LSA received → ospf_flood_lsa → lsdb updated → EVT_OSPF_SPF scheduled

ospf_spf_timer(e, state):
   └─► ospf_run_spf(state)
           │
           │   Dijkstra on lsdb[]:
           │     dist[router_id] = 0; dist[*] = INFINITY
           │     priority queue on (cost, router_id)
           │
           │     for each router in queue:
           │         for each link in lsdb entry:
           │             if dist[link.router] > dist[u] + link.metric:
           │                 dist[link.router] = dist[u] + link.metric
           │                 next_hop[link.router] = first_hop_iface
           │
           ├─► route_table_flush_proto(dev->route_tbl, ROUTE_PROTO_OSPF)
           │
           └─► for each reachable router:
                   route_table_add(dev->route_tbl,
                                   router_prefix, 32,
                                   next_hop_ip, next_hop_iface,
                                   dist[router], ROUTE_PROTO_OSPF)
```

---

## Design Notes

- **Single area (area 0)** — no ABR/ASBR logic needed for this milestone.
- **SPF is debounced** — a `EVT_OSPF_SPF` fired 500 ms after the last LSA
  change prevents Dijkstra from running on every individual LSA flood.
- **LSA max 4 links per entry** keeps `OspfLsaEntry` at 64 bytes without
  variable-length malloc.
- **No DR/BDR election** for this milestone (point-to-point links only).
  Add DR logic when implementing multi-access (Ethernet) segments.

## Test Plan (kleva)

- `hello_advances_neighbor_state`, `dead_timer_removes_neighbor`
- `lsa_flood_reaches_all_routers`, `duplicate_lsa_dropped`
- `spf_computes_shortest_path`, `spf_installs_routes`
- `route_removed_on_lsa_withdrawal`
- NULL guards: `receive_null_pkt`, `run_spf_null_state`
