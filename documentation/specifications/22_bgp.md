# Module 20 — BGP (Border Gateway Protocol v4)

**Files:** `src/protocols/bgp.c`, `src/protocols/bgp.h`
**Status:** ⬜ Not started
**Depends on:** tcp, ip, scheduler, route_table, simulator

---

## The Problem

Within an autonomous system (AS), IGPs like OSPF/RIP suffice. BGP
carries reachability between **different ASes** — it is the routing
protocol of the Internet. Key properties:

- **Path vector**: each route announcement carries the full AS-PATH.
- **TCP-based**: sessions run over TCP port 179 — reliable, not UDP.
- **Incremental updates**: only changes are sent (UPDATE), not periodic
  full-table floods.
- **Policy-driven**: local_preference and MED allow per-AS selection.

This simulator implements **eBGP** (external BGP — between ASes) with
simplified attribute set (AS-PATH, NEXT-HOP, LOCAL-PREF, MED).

## BGP Message Types

| Type | Name       | Purpose                                    |
|------|------------|--------------------------------------------|
| `1`  | OPEN       | Session establish; exchange capabilities   |
| `2`  | UPDATE     | Announce / withdraw routes                 |
| `3`  | NOTIFICATION | Error; session teardown                 |
| `4`  | KEEPALIVE  | Heartbeat                                  |

### BGP Common Header (19 bytes, packed)

```c
typedef struct __attribute__((packed)) BgpHeader {
    uint8_t  marker[16];  // all 0xFF (sync marker)
    uint16_t length;      // total message bytes including header
    uint8_t  type;        // 1..4
} BgpHeader;              // 19 bytes
```

### OPEN body (10 bytes + optional parameters)

```c
typedef struct __attribute__((packed)) BgpOpen {
    uint8_t  version;       // 4
    uint16_t my_as;         // 2-byte ASN (simplified)
    uint16_t hold_time;     // seconds (90 typical)
    uint32_t bgp_id;        // router's BGP ID (IPv4 address)
    uint8_t  opt_param_len; // 0 (no capabilities in simplified)
} BgpOpen;                  // 10 bytes
```

---

## Header File — `bgp.h`

### Constants

| Macro                  | Value   | Use                              |
|------------------------|---------|----------------------------------|
| `BGP_PORT`             | `179`   | TCP port                         |
| `BGP_VERSION`          | `4`     | Protocol version                 |
| `BGP_HDR_LEN`          | `19`    | Common header size               |
| `BGP_HOLD_TIME`        | `90`    | Default hold time (s → ms×1000)  |
| `BGP_KEEPALIVE_INTERVAL`| `30000`| ms (1/3 of hold time)            |
| `BGP_MAX_PEERS`        | `16`    | Max BGP sessions per router      |
| `BGP_MAX_PREFIXES`     | `1024`  | Max prefixes in Adj-RIB-In       |
| `BGP_LOCAL_PREF_DEFAULT`| `100`  | Default LOCAL_PREF               |

### BGP Session State Machine

```c
typedef enum {
    BGP_IDLE,
    BGP_CONNECT,
    BGP_ACTIVE,
    BGP_OPEN_SENT,
    BGP_OPEN_CONFIRM,
    BGP_ESTABLISHED
} BgpState;
```

### `BgpPeer` Struct (64 bytes)

```c
typedef struct BgpPeer {
    uint32_t  remote_ip;
    uint16_t  remote_as;
    uint16_t  local_as;
    BgpState  state;
    Tcb      *tcb;            // underlying TCP connection
    uint32_t  bgp_id;         // peer's BGP identifier
    uint16_t  hold_time;      // negotiated (s)
    uint64_t  hold_deadline;  // ms — if exceeded: send NOTIFICATION
    uint64_t  keepalive_ts;   // ms — next KEEPALIVE due
    int       valid;
    int       _pad;
} BgpPeer;                    // 56 bytes
```

### `BgpPrefix` Struct (Adj-RIB-In entry, 40 bytes)

```c
typedef struct BgpPrefix {
    uint32_t  prefix;
    uint8_t   prefix_len;
    uint8_t   valid;
    uint8_t   _pad[2];
    uint32_t  next_hop;
    uint32_t  local_pref;
    uint32_t  med;
    uint8_t   as_path[12];   // up to 3 ASNs (4 B each) — simplified
    int       as_path_len;
} BgpPrefix;                  // 36 bytes
```

### `BgpState_t` Struct (per Device, ≈ 45 KB)

```c
typedef struct BgpStateBlock {
    BgpPeer   peers[16];           // 16 × 56   =   896 B
    BgpPrefix rib_in[1024];        // 1024 × 36 = 36 864 B
    int       rib_count;
    uint32_t  local_as;
    uint32_t  router_id;
    Simulator *sim;
    Device    *dev;
} BgpStateBlock;                   // ≈ 38 KB
```

### Public API

| Function                                  | Purpose                                        |
|-------------------------------------------|------------------------------------------------|
| `bgp_init(sim, dev, local_as, router_id)` | Zero state; call `tcp_listen(179)`.            |
| `bgp_add_peer(state, remote_ip, remote_as)` | Configure neighbor; initiate TCP connect.    |
| `bgp_receive(tcb, pkt, ctx)`              | TCP recv_fn; dispatch on BGP message type.     |
| `bgp_send_open(state, peer)`              | Send OPEN after TCP established.               |
| `bgp_send_keepalive(state, peer)`         | Periodic heartbeat.                            |
| `bgp_send_update(state, peer, prefixes, withdrawn)` | Announce / withdraw prefixes.        |
| `bgp_select_best(state, prefix)`          | Apply BGP decision process; install to RIB-Loc.|
| `bgp_keepalive_timer(e, ctx)`             | Fires every BGP_KEEPALIVE_INTERVAL.            |
| `bgp_hold_timer(e, ctx)`                  | Fires if hold_deadline exceeded; send NOTIFICATION. |

---

## Dispatch Table

```
Scheduler.handlers[]
┌────────────────────────┬──────────────────────┬────────────┐
│ EVT_BGP_KEEPALIVE      │ bgp_keepalive_timer  │ bgp_state  │
│ EVT_BGP_HOLD           │ bgp_hold_timer       │ bgp_state  │
│ EVT_BGP_CONNECT_RETRY  │ bgp_connect_retry    │ bgp_state  │
└────────────────────────┴──────────────────────┴────────────┘

TCP socket (port 179):
   tcp_listen(sim, local_ip, 179, bgp_receive, bgp_state)
```

---

## Function Call Sequence — Session Establishment

```
bgp_add_peer(state, peer_ip, peer_as):
   ├─ peer->state = BGP_CONNECT
   └─► tcp_connect(sim, local_ip, peer_ip, ephemeral, 179,
                   bgp_receive, state)

[TCP established → bgp_receive(tcb, NULL, state)]
   └─► bgp_send_open(state, peer)
           ├─ peer->state = BGP_OPEN_SENT
           └─ build BgpHeader{type=1} + BgpOpen{my_as, hold=90, bgp_id}
              tcp_send(sim, tcb, pkt)

[peer sends OPEN back → bgp_receive → type==1]
   ├─ peer->state = BGP_OPEN_CONFIRM
   └─► bgp_send_keepalive(state, peer)   ← acknowledge OPEN

[peer sends KEEPALIVE → bgp_receive → type==4]
   └─ peer->state = BGP_ESTABLISHED
      schedule EVT_BGP_KEEPALIVE in 30 000 ms
      schedule EVT_BGP_HOLD in 90 000 ms
```

## Function Call Sequence — Route Selection (BGP Decision Process)

```
bgp_receive_update(state, peer, pkt):
   │   parse announced prefixes → append to rib_in[]
   │   parse withdrawn prefixes → mark invalid in rib_in[]
   │
   └─► for each changed prefix:
           bgp_select_best(state, prefix)
               │   1. prefer highest LOCAL_PREF
               │   2. prefer shortest AS_PATH
               │   3. prefer lowest MED (among same AS)
               │   4. prefer eBGP over iBGP
               │   5. prefer lowest router_id (tie-break)
               │
               ├─ if best changed:
               │       route_table_add(dev->route_tbl, prefix, plen,
               │                       nh, iface, AS_PATH_LEN, ROUTE_PROTO_BGP)
               └─ if withdrawn and was best:
                       route_table_delete(...)
```

---

## Design Notes

- **2-byte ASNs only** (RFC 4271). 4-byte ASNs (RFC 4893) require
  `uint32_t` and an `AS4_PATH` attribute — out of scope.
- **No route reflection / confederation** — flat iBGP mesh assumed.
- **AS_PATH stored as raw bytes** (up to 3 ASNs × 4 bytes). Loop
  detection: if local_as appears in received AS_PATH, drop the update.
- **Hold timer** is reset on every received message (KEEPALIVE, UPDATE,
  or OPEN). If it expires, send NOTIFICATION(hold timer expired) and
  reset to IDLE.
- **TCP teardown on NOTIFICATION** — `bgp_receive` calls `tcp_close`
  immediately after sending.

## Test Plan (kleva)

- `open_exchange_reaches_established`
- `keepalive_resets_hold_timer`
- `hold_expired_sends_notification`
- `update_installs_route`, `withdraw_removes_route`
- `loop_detection_drops_own_as`, `best_path_selection_local_pref`
- NULL guards: `receive_null_tcb`, `send_update_null_peer`
