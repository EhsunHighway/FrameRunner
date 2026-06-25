# Module 25 — IS-IS (Intermediate System to Intermediate System)

**Files:** `src/protocols/isis.c`, `src/protocols/isis.h`
**Status:** ⬜ Not started
**Depends on:** packet, interface, scheduler, route_table, simulator

---

## The Problem

IS-IS is a link-state protocol that runs **directly over Layer 2**
(not over IP). It was designed for CLNS but was repurposed for IP via
RFC 1195. It is widely used in ISP core networks alongside or instead
of OSPF. Distinguishing features:

- **No IP header** — IS-IS PDUs are sent as raw L2 frames (ethertype
  `0x8870` or directly via SNAP). In this simulator we'll wrap them
  in IP (proto 124) for simplicity.
- **Two-level hierarchy**: Level-1 (intra-area) and Level-2
  (inter-area / backbone). This simulator implements **Level-1 only**.
- **Net address (NSAP)**: each router has a Network Entity Title (NET)
  — 8 bytes in our simplified form.
- **SPF** runs the same Dijkstra as OSPF on the LSDB.

---

## Header File — `isis.h`

### Constants

| Macro                    | Value     | Use                                    |
|--------------------------|-----------|----------------------------------------|
| `ISIS_PROTO_NUM`         | `124`     | IP protocol (used in simulator)        |
| `ISIS_HELLO_INTERVAL`    | `10000`   | ms                                     |
| `ISIS_HOLD_MULTIPLIER`   | `3`       | Dead = hello × 3 = 30 s               |
| `ISIS_LSDB_SIZE`         | `256`     | Max LSPs in database                   |
| `ISIS_MAX_NEIGHBORS`     | `32`      |                                        |
| `ISIS_MAX_IFACES`        | `16`      |                                        |
| `ISIS_METRIC_DEFAULT`    | `10`      | Default link metric                    |
| `ISIS_METRIC_MAX`        | `63`      | Narrow metric ceiling                  |
| `ISIS_PDU_IIH`           | `15`      | PDU type: IIH (IS-IS Hello)            |
| `ISIS_PDU_LSP`           | `18`      | PDU type: Level-1 LSP                  |
| `ISIS_PDU_CSNP`          | `24`      | PDU type: Complete SNP                 |
| `ISIS_PDU_PSNP`          | `26`      | PDU type: Partial SNP (LSP request)    |

### NET (Network Entity Title, 8 bytes simplified)

```
   [0]   AFI       = 0x49  (ISO private)
   [1..5] Area ID  = 5 bytes (e.g. 00.0001)
   [6..7] System ID = 2 bytes (derived from loopback IP low 2 octets)
   [8]   NSEL     = 0x00  (always 0 for a router)
```

In this simulator we use the loopback IP as an 8-byte NSAP (padded).

### IS-IS Common Header (8 bytes, packed)

```c
typedef struct __attribute__((packed)) IsisHeader {
    uint8_t  discr;        // 0x83 — IS-IS discriminator
    uint8_t  hdr_len;      // variable (8 for this)
    uint8_t  version;      // 1
    uint8_t  id_len;       // 0 = 6 (system ID length)
    uint8_t  pdu_type;     // 15=IIH, 18=L1 LSP, 24=CSNP, 26=PSNP
    uint8_t  version2;     // 1
    uint8_t  reserved;
    uint8_t  max_areas;    // 3
} IsisHeader;              // 8 bytes
```

### IIH (Hello) additional fields (20 bytes)

```c
typedef struct __attribute__((packed)) IsisIih {
    uint8_t  circuit_type;  // 1=L1, 2=L2, 3=both
    uint8_t  src_id[6];     // sender's system ID
    uint16_t hold_time;     // seconds
    uint16_t pdu_len;       // total PDU length
    uint8_t  priority;      // DR election priority (0..127)
    uint8_t  dis_id[7];     // designated IS (system-id + circuit-id)
    // TLV list follows
} IsisIih;                  // 20 bytes
```

### LSP Header (27 bytes)

```c
typedef struct __attribute__((packed)) IsisLspHeader {
    uint16_t pdu_len;
    uint16_t remaining_lifetime;  // seconds; 0 = purge
    uint8_t  lsp_id[8];           // system_id[6] + pseudonode[1] + fragment[1]
    uint32_t seq_num;
    uint16_t checksum;
    uint8_t  type_block;          // partition repair, attachment, overload, metric type
} IsisLspHeader;                  // 27 bytes
```

### `IsisNeighbor` Struct (32 bytes)

```c
typedef struct IsisNeighbor {
    uint8_t    sys_id[6];
    uint8_t    _pad[2];
    uint32_t   hold_deadline_ms;
    Interface *iface;
    int        valid;
    int        _pad2;
} IsisNeighbor;               // 32 bytes
```

### `IsisLspEntry` Struct (LSDB, 72 bytes)

```c
typedef struct IsisLspEntry {
    uint8_t  lsp_id[8];          //  8 B
    uint32_t seq_num;            //  4 B
    uint16_t checksum;           //  2 B
    uint16_t lifetime;           //  2 B — seconds remaining
    uint32_t neighbor_ids[8];    // 32 B — up to 8 neighbors (IP)
    uint16_t metrics[8];         // 16 B — metric per neighbor
    int      neighbor_count;     //  4 B
    int      valid;              //  4 B
} IsisLspEntry;                  // 72 bytes
```

### `IsisState` Struct (per Device, ≈ 20 KB)

```c
typedef struct IsisState {
    uint8_t       net[8];              // this router's NET
    IsisNeighbor  neighbors[32];       // 32 × 32  =  1 024 B
    IsisLspEntry  lsdb[256];           // 256 × 72 = 18 432 B
    int           lsdb_count;
    Interface    *ifaces[16];
    int           iface_count;
    Simulator    *sim;
    Device       *dev;
} IsisState;                           // ≈ 20 KB
```

### Public API

| Function                             | Purpose                                         |
|--------------------------------------|-------------------------------------------------|
| `isis_init(sim, dev, net)`           | Zero state; bind IP 124; schedule Hello.        |
| `isis_enable_iface(state, iface, metric)` | Add NIC to IS-IS.                         |
| `isis_receive(iface, pkt, sim)`      | Dispatch on PDU type.                           |
| `isis_send_iih(state, iface)`        | Periodic Hello.                                 |
| `isis_flood_lsp(state, lsp, except)` | Forward LSP to all ifaces except source.        |
| `isis_run_spf(state)`                | Dijkstra on LSDB; install routes.               |
| `isis_generate_lsp(state)`           | Build own LSP and flood it.                     |

---

## Dispatch Table

```
Scheduler.handlers[]
┌─────────────────────┬────────────────────┬─────────────┐
│ EVT_ISIS_HELLO      │ isis_hello_timer   │ isis_state  │
│ EVT_ISIS_HOLD       │ isis_hold_timer    │ isis_state  │
│ EVT_ISIS_SPF        │ isis_spf_timer     │ isis_state  │
│ EVT_ISIS_LSP_REGEN  │ isis_lsp_regen     │ isis_state  │
└─────────────────────┴────────────────────┴─────────────┘

ip_receive: proto == 124 → isis_receive(iface, pkt, sim)
```

---

## Function Call Sequence — Adjacency Formation

```
isis_hello_timer → isis_send_iih(state, each_iface):
   │  build IsisHeader{pdu_type=15} + IsisIih{src_id, hold=30, ...}
   │  append TLV 6 (IS neighbors — MACs already seen)
   └─► ip_send(sim, iface->ip_addr, 224.0.0.5, 124, pkt)

[peer receives IIH]
isis_receive → pdu_type==15 → isis_process_iih:
   │  find/create IsisNeighbor for src_id
   │  reset hold_deadline = now + 30 000
   └─  if neighbor state was DOWN → UP: generate_lsp → flood
```

## Function Call Sequence — SPF after LSP flood

```
isis_receive → pdu_type==18 (LSP) → isis_process_lsp:
   ├─  install into lsdb[] (update if seq_num > existing)
   ├─► isis_flood_lsp(state, lsp, except=in_iface)
   └─► schedule EVT_ISIS_SPF in 500 ms (debounce)

isis_spf_timer → isis_run_spf(state):
   │  Dijkstra on lsdb[] (same algorithm as OSPF module #19)
   ├─► route_table_flush_proto(dev->route_tbl, ROUTE_PROTO_ISIS)
   └─► for each reachable node: route_table_add(...)
```

---

## Design Notes

- **L2-native vs IP-encapsulated**: real IS-IS uses raw L2 frames with
  multicast MAC `01:80:C2:00:00:14`. In this simulator, encapsulating
  in IP (proto 124) keeps the same `ip_receive` dispatch path as all
  other protocols and avoids a special Ethernet demux path.
- **LSP lifetime countdown**: in a real router the `remaining_lifetime`
  field decreases every second. A periodic `EVT_ISIS_LSP_REGEN` event
  regenerates the local LSP every 900 s before expiry.
- **CSNP/PSNP** (database synchronization) are simplified out — initial
  implementation floods everything; database exchange optimization is
  a future milestone.
- **Metrics are narrow (0–63)** — wide metrics (RFC 3784, up to 16M)
  require a `uint32_t` metric field in TLVs; add that when needed.

## Test Plan (kleva)

- `iih_advances_neighbor_state`, `hold_expired_removes_neighbor`
- `lsp_flood_reaches_all_routers`, `duplicate_seq_dropped`
- `spf_installs_routes`, `old_routes_flushed_before_spf`
- `lsp_regen_updates_seq_num`
- NULL guards: `receive_null_pkt`, `run_spf_null_state`
