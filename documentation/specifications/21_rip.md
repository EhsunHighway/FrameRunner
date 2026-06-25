# Module 21 — RIP (Routing Information Protocol v2)

**Files:** `src/protocols/rip.c`, `src/protocols/rip.h`
**Status:** ⬜ Not started
**Depends on:** udp, route_table, ip, scheduler, simulator

---

## The Problem

RIP is the simplest distance-vector routing protocol. Every 30 s each
router broadcasts its entire routing table to its directly connected
neighbors (UDP port 520, multicast 224.0.0.9). Neighbors update their
own tables using the Bellman-Ford rule: *if my metric to you plus your
cost to X is less than my current cost to X, replace it*. Infinity
(metric = 16) means "unreachable." This module:

1. Manages per-router RIP state (timers, enabled interfaces).
2. Sends periodic UPDATE messages.
3. Receives UPDATE messages and calls `route_table_add/delete`.
4. Implements **split horizon** (don't advertise a route back out the
   interface you learned it from).
5. Triggers TRIGGERED UPDATEs on route change.

## RIP Message Wire Format

### RIP Header (4 bytes)

```
   offset  field    size  notes
   ──────  ─────    ────  ─────
     0     command   1    1=request, 2=response (update)
     1     version   1    2 for RIPv2
     2     zero      2    must be 0
```

### Route Entry (20 bytes each, up to 25 per message)

```
   offset  field         size  notes
   ──────  ─────         ────  ─────
     0     afi           2    0x0002 = IPv4
     2     route_tag     2    for external routes; 0 for internal
     4     ip_addr       4    network address
     8     subnet_mask   4    e.g. 255.255.255.0
    12     next_hop      4    0.0.0.0 = use sender's IP
    16     metric        4    1..16
```

```c
typedef struct __attribute__((packed)) RipEntry {
    uint16_t afi;
    uint16_t route_tag;
    uint32_t ip_addr;
    uint32_t subnet_mask;
    uint32_t next_hop;
    uint32_t metric;
} RipEntry;               // 20 bytes

typedef struct __attribute__((packed)) RipHeader {
    uint8_t  command;
    uint8_t  version;
    uint16_t zero;
} RipHeader;              // 4 bytes
```

Full UPDATE message: 4 + N×20 bytes (N ≤ 25 per packet).

---

## Header File — `rip.h`

### Constants

| Macro                | Value        | Use                              |
|----------------------|--------------|----------------------------------|
| `RIP_PORT`           | `520`        | UDP port                         |
| `RIP_MULTICAST`      | `0xE0000009` | 224.0.0.9 in host order          |
| `RIP_INFINITY`       | `16`         | Unreachable metric               |
| `RIP_UPDATE_INTERVAL`| `30000`      | Periodic update timer (ms)       |
| `RIP_TIMEOUT_MS`     | `180000`     | Route expiry (6 × update)        |
| `RIP_GC_MS`          | `120000`     | Garbage-collect delay after expiry|
| `RIP_MAX_IFACES`     | `16`         | RIP-enabled interfaces per router|
| `RIP_MAX_ROUTES`     | `25`         | Max route entries per UDP message|

### `RipRouteInfo` Struct (32 bytes — extends RouteEntry knowledge)

```c
typedef struct RipRouteInfo {
    uint32_t   prefix;
    uint8_t    prefix_len;
    uint8_t    _pad[3];
    uint32_t   metric;
    uint32_t   next_hop;
    Interface *learned_on;   // for split horizon
    uint64_t   last_update;  // ms — for timeout
    int        valid;
} RipRouteInfo;              // 40 bytes
```

### `RipState` Struct (per Device, ≈ 4 KB)

```c
#define RIP_DB_SIZE 128
typedef struct RipState {
    RipRouteInfo db[128];     // 128 × 40 = 5 120 B — RIP-learned routes
    int          db_count;
    Interface   *ifaces[16];  // RIP-enabled NICs
    int          iface_count;
    Simulator   *sim;
} RipState;
```

### Public API

| Function                                 | Purpose                                           |
|------------------------------------------|---------------------------------------------------|
| `rip_init(sim, dev)`                     | Zero state; bind UDP 520; schedule first UPDATE.  |
| `rip_enable_iface(state, iface)`         | Add iface to `ifaces[]`.                          |
| `rip_receive(src_ip, src_port, pkt, ctx)`| UDP callback; parse entries; install routes.      |
| `rip_send_update(state, out_iface)`      | Build RIP message; call `udp_send`.               |
| `rip_timeout_handler(e, ctx)`            | Mark timed-out routes metric=16; trigger update.  |
| `rip_gc_handler(e, ctx)`                 | Remove routes that hit metric=16 and expired.     |

### ACSL Highlights

```
rip_receive (new route, metric < 16):
  route_table_add called with metric = min(entry->metric + 1, 16)

rip_receive (metric == 16):
  route_table_delete called for that prefix

split horizon (rip_send_update):
  for each db entry e: e.learned_on == out_iface ⇒ NOT included in update
```

---

## Dispatch Table

```
Scheduler.handlers[]
┌──────────────────┬────────────────────┬───────────┐
│ EVT_RIP_UPDATE   │ rip_update_handler │ rip_state │  ← fires every 30 s
│ EVT_RIP_TIMEOUT  │ rip_timeout_handler│ rip_state │  ← per-route 180 s
│ EVT_RIP_GC       │ rip_gc_handler     │ rip_state │  ← 120 s after timeout
└──────────────────┴────────────────────┴───────────┘

UDP socket table (port 520):
┌──────────┬─────────────┬───────────┐
│ port=520 │ rip_receive │ rip_state │  ← udp_bind() in rip_init()
└──────────┴─────────────┴───────────┘
```

---

## Function Call Sequence — Periodic UPDATE

```
rip_update_handler(e, rip_state):        (fires every 30 000 ms)
   │
   ├─► for each iface in rip_state->ifaces[]:
   │       rip_send_update(rip_state, iface)
   │           │
   │           │   build RipHeader{cmd=2, ver=2, zero=0}
   │           │   for each entry in db[]:
   │           │       if entry.learned_on == iface: SKIP (split horizon)
   │           │       else: append RipEntry{..., metric=entry.metric}
   │           │
   │           ├─► packet_create, packet_prepend(hdr, 4)
   │           └─► udp_send(sim, iface->ip_addr, 224.0.0.9, 520, 520, pkt)
   │
   └─► scheduler_schedule(EVT_RIP_UPDATE, now + 30000, ..., rip_state)
```

## Function Call Sequence — UPDATE received

```
udp_receive → port 520 → rip_receive(src_ip, 520, pkt, rip_state):
   │
   │   hdr = (RipHeader *) pkt->data; check version == 2
   │   entries = (RipEntry *)(pkt->data + 4)
   │   n = (pkt->len - 4) / 20
   │
   │   for i in 0..n:
   │       metric = ntohl(entries[i].metric) + 1     ← increment hop count
   │       if metric > 16: metric = 16
   │
   │       if metric < 16:
   │           route_table_add(&dev->route_tbl, prefix, plen,
   │                           nh=src_ip, iface, metric, ROUTE_PROTO_RIP)
   │           rip_db_update(rip_state, prefix, plen, metric, iface)
   │           schedule EVT_RIP_TIMEOUT at now + 180 000
   │       else:
   │           route_table_delete(&dev->route_tbl, prefix, plen, ROUTE_PROTO_RIP)
   │           schedule EVT_RIP_GC at now + 120 000   ← send metric=16 then remove
```

---

## Design Notes

- **Split horizon** prevents the most common distance-vector loop: A
  learned a route via B, so A won't advertise it back to B.
- **Triggered updates** are sent immediately on metric change. Omitted
  in the first milestone — add `rip_send_triggered` when implementing.
- **Metric is incremented on receipt** (not on send) — the router
  receiving the update adds 1 for the link it arrived on.
- **`RIP_DB_SIZE` is separate from `ROUTE_TABLE_SIZE`** — the RIP
  database tracks per-route timers; the route table is the forwarding
  table used by IP. They stay in sync via `route_table_add/delete`.

## Test Plan (kleva)

- `receive_installs_route`, `receive_metric16_removes_route`
- `split_horizon_omits_learned_route`
- `periodic_update_scheduled`, `timeout_sets_metric16`
- `gc_removes_expired_route`
- NULL guards: `receive_null_pkt`, `send_update_null_iface`
