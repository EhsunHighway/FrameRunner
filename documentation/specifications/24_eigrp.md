# Module 24 — EIGRP (Enhanced Interior Gateway Routing Protocol)

**Files:** `src/protocols/eigrp.c`, `src/protocols/eigrp.h`
**Status:** ⬜ Not started
**Depends on:** ip, scheduler, route_table, simulator

---

## The Problem

EIGRP is Cisco's proprietary hybrid routing protocol — combining the
simple neighbor relationship of distance-vector with the loop-free
guarantee of link-state via the **DUAL (Diffusing Update Algorithm)**
algorithm. Key properties:

- **Composite metric:** bandwidth + delay (+ load, reliability, MTU
  optionally). This gives finer cost discrimination than RIP.
- **Partial updates:** only topology changes are sent (unlike RIP's
  full-table floods).
- **Reliable delivery** via RTP (Reliable Transport Protocol) — ACK
  mechanism over IP (protocol 88), multicast 224.0.0.10.
- **DUAL** guarantees loop-free convergence: each router tracks a
  *feasibility condition* before installing a backup route.

This module implements simplified EIGRP: composite metric reduced to
**inverse bandwidth plus delay** (`10^7/BW + delay`), and reliable
delivery via per-packet ACK without full RTP sequencing.

---

## Header File — `eigrp.h`

### Constants

| Macro                      | Value        | Use                              |
|----------------------------|--------------|----------------------------------|
| `EIGRP_PROTO_NUM`          | `88`         | IP protocol number               |
| `EIGRP_MULTICAST`          | `0xE000000A` | 224.0.0.10                       |
| `EIGRP_HELLO_INTERVAL`     | `5000`       | ms                               |
| `EIGRP_HOLD_TIME`          | `15000`      | ms — neighbor declared dead      |
| `EIGRP_MAX_NEIGHBORS`      | `32`         |                                  |
| `EIGRP_MAX_ROUTES`         | `256`        | Topology table entries           |
| `EIGRP_INFINITY`           | `0xFFFFFFFF` | Metric for unreachable           |
| `EIGRP_K1`                 | `1`          | Bandwidth weight                 |
| `EIGRP_K3`                 | `1`          | Delay weight (K2=K4=K5=0)        |

### EIGRP Packet Types

| Value | Name    |
|-------|---------|
| `1`   | UPDATE  |
| `3`   | QUERY   |
| `4`   | REPLY   |
| `5`   | HELLO   |

### EIGRP Header (20 bytes, packed)

```c
typedef struct __attribute__((packed)) EigrpHeader {
    uint8_t  version;      // 2
    uint8_t  opcode;       // 1/3/4/5
    uint16_t checksum;
    uint32_t flags;        // 0x01=INIT, 0x02=CR (conditional receive)
    uint32_t seq;          // RTP sequence number
    uint32_t ack;          // RTP ack
    uint32_t as_number;    // autonomous system number
} EigrpHeader;             // 20 bytes
```

### Internal Route entry in UPDATE/QUERY/REPLY (28 bytes)

```c
typedef struct __attribute__((packed)) EigrpRoute {
    uint8_t  prefix_len;
    uint8_t  _pad[3];
    uint32_t prefix;
    uint32_t bandwidth;    // Kbps × 256 (scaled)
    uint32_t delay;        // µs × 256 (scaled)
    uint32_t metric;       // composite = (K1×10^7/BW + K3×DLY)
    uint32_t next_hop;     // 0 = use sender's IP
} EigrpRoute;              // 28 bytes
```

### `EigrpNeighbor` Struct (40 bytes)

```c
typedef struct EigrpNeighbor {
    uint32_t   ip_addr;
    uint32_t   hold_deadline_ms;
    Interface *iface;
    uint32_t   rtp_seq;      // last RTP seq received from this neighbor
    int        valid;
    int        _pad;
} EigrpNeighbor;             // 32 bytes
```

### `EigrpTopoEntry` Struct (DUAL topology table, 48 bytes)

```c
typedef struct EigrpTopoEntry {
    uint32_t prefix;
    uint8_t  prefix_len;
    uint8_t  valid;
    uint8_t  _pad[2];
    uint32_t fd;           // feasible distance (best known metric)
    uint32_t rd;           // reported distance from successor
    uint32_t successor_metric;
    uint32_t next_hop;
    Interface *iface;       // successor's egress iface
} EigrpTopoEntry;           // 32 bytes
```

### `EigrpState` Struct (per Device, ≈ 17 KB)

```c
typedef struct EigrpState {
    EigrpNeighbor  neighbors[32];   // 32 × 32  = 1 024 B
    EigrpTopoEntry topo[256];       // 256 × 32 =  8 192 B
    int            topo_count;
    Interface     *ifaces[16];
    int            iface_count;
    uint32_t       as_number;
    Simulator     *sim;
    Device        *dev;
} EigrpState;                       // ≈ 9 KB
```

### Public API

| Function                                   | Purpose                                         |
|--------------------------------------------|-------------------------------------------------|
| `eigrp_init(sim, dev, as_num)`             | Zero state; bind IP proto 88; schedule Hello.   |
| `eigrp_enable_iface(state, iface, bw, delay)` | Add NIC; send initial Hello.                |
| `eigrp_receive(iface, pkt, sim)`           | Dispatch on opcode.                             |
| `eigrp_send_hello(state, iface)`           | Periodic Hello; neighbor keepalive.             |
| `eigrp_send_update(state, iface, routes, n)` | Announce routes to neighbor.                 |
| `eigrp_dual_process(state, prefix, new_metric, nbr)` | Run DUAL; install or query.        |
| `eigrp_compute_metric(bw_kbps, delay_us)`  | Return composite metric (K1×10^7/BW + K3×DLY). |

---

## Dispatch Table

```
Scheduler.handlers[]
┌────────────────────────┬──────────────────────┬─────────────┐
│ EVT_EIGRP_HELLO        │ eigrp_hello_timer    │ eigrp_state │
│ EVT_EIGRP_HOLD         │ eigrp_hold_timer     │ eigrp_state │
└────────────────────────┴──────────────────────┴─────────────┘

ip_receive: proto == 88 → eigrp_receive(iface, pkt, sim)
```

---

## Composite Metric Formula

$$
\text{metric} = \left(\frac{K_1 \times 10^7}{\text{BW}_{\text{kbps}}}\right) + (K_3 \times \text{delay}_{\mu s})
$$

With K1=K3=1 and all others 0:

```
metric = (10^7 / bandwidth_kbps) + delay_us
```

---

## Function Call Sequence — DUAL Route Selection

```
eigrp_receive → opcode=1 (UPDATE) → for each route in pkt:
   │
   └─► eigrp_dual_process(state, prefix, reported_metric, nbr)
           │
           │   rd = reported_metric
           │   candidate = rd + link_cost_to_nbr     ← feasible distance candidate
           │
           ├─ FEASIBILITY CHECK:
           │     rd < current_FD?                    ← loop-free guarantee
           │
           ├─── YES (feasible): install as successor
           │       topo_entry->fd          = candidate
           │       topo_entry->rd          = rd
           │       topo_entry->next_hop    = nbr->ip_addr
           │       topo_entry->iface       = nbr->iface
           │       route_table_add(dev->route_tbl, prefix, plen,
           │                       nh, iface, candidate, ROUTE_PROTO_EIGRP)
           │
           └─── NO (possible loop): send QUERY to all neighbors
                   eigrp_send_update(state, each_iface, {prefix, metric=INFINITY})
                   wait for REPLY from all queried neighbors
```

---

## Design Notes

- **DUAL's feasibility condition** (`rd < fd`) is the loop-freedom
  proof: a neighbor's reported distance less than our known best means
  they cannot be routing through us.
- **No full RTP sequencing** in this milestone — UPDATE/QUERY/REPLY use
  best-effort multicast. Add per-neighbor seq/ack tracking for the full
  reliable transport layer.
- **K values K1=K3=1, K2=K4=K5=0** is the Cisco default. Enabling K2
  (load) or K4 (reliability) would require polling interface stats.
- **Composite metric avoids the 0 divisor** — check `bw_kbps > 0`
  before computing `10^7 / bw`.

## Test Plan (kleva)

- `hello_establishes_neighbor`, `hold_expired_removes_neighbor`
- `update_installs_route_feasibility_check`
- `query_sent_on_infeasible`, `reply_resolves_query`
- `metric_formula_correct`, `infinity_not_installed`
- NULL guards: `receive_null_pkt`, `dual_process_null_state`
