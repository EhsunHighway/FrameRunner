# Module 17 — Route Table

**Files:** `src/routing/route_table.c`, `src/routing/route_table.h`
**Status:** ⬜ Not started
**Depends on:** interface, device

---

## The Problem

IP forwarding needs to answer one question per datagram: **"Which
interface should I send this out, and what is the next-hop IP?"**
The answer lives in the routing table — an ordered list of (prefix,
prefix-len) entries. The longest-matching prefix wins (LPM). This
module:

1. Stores static and dynamic (RIP/OSPF-installed) routes.
2. Performs LPM lookup in O(N) — sufficient for a 256-entry table.
3. Supports route add/delete (called by routing daemons and CLI).
4. Is **embedded in `Device`** — every router has exactly one.

## Mental Model

```
   RouteTable (per Device/Router)
   ┌──────────────────────────────────────────────────────────┐
   │  entries[0]: 0.0.0.0/0  → nh=192.168.1.1  iface=eth1 (default)
   │  entries[1]: 10.0.0.0/8 → nh=0.0.0.0      iface=eth0 (connected)
   │  entries[2]: 10.1.2.0/24→ nh=10.0.0.5     iface=eth0 (learned via RIP)
   │  entries[3..255]: valid=0
   │  count = 3
   └──────────────────────────────────────────────────────────┘

   LPM example: dst = 10.1.2.99
     entry[2]: 10.1.2.0/24 matches (prefix_len=24, most specific) ← winner
     entry[1]: 10.0.0.0/8  matches (prefix_len=8)
     entry[0]: 0.0.0.0/0   matches (prefix_len=0)
```

---

## Header File — `route_table.h`

### Constants

| Macro                | Value | Use                          |
|----------------------|-------|------------------------------|
| `ROUTE_TABLE_SIZE`   | `256` | Fixed capacity               |
| `ROUTE_PROTO_STATIC` | `1`   | Installed by CLI/config      |
| `ROUTE_PROTO_DIRECT` | `2`   | Connected interface subnet   |
| `ROUTE_PROTO_RIP`    | `3`   | Learned via RIP              |
| `ROUTE_PROTO_OSPF`   | `4`   | Learned via OSPF             |
| `ROUTE_PROTO_BGP`    | `5`   | Learned via BGP              |
| `ROUTE_PROTO_EIGRP`  | `6`   | Learned via EIGRP            |
| `ROUTE_PROTO_ISIS`   | `7`   | Learned via IS-IS            |

### `RouteEntry` Struct (32 bytes)

```c
typedef struct RouteEntry {
    uint32_t   prefix;         //  4 B — network address (host order OK)
    uint8_t    prefix_len;     //  1 B — CIDR 0..32
    uint8_t    proto;          //  1 B — ROUTE_PROTO_*
    uint8_t    valid;          //  1 B
    uint8_t    _pad;           //  1 B
    uint32_t   next_hop;       //  4 B — 0.0.0.0 = directly connected
    Interface *iface;          //  8 B — egress NIC
    uint32_t   metric;         //  4 B — cost (hop count, OSPF cost, etc.)
    uint32_t   _pad2;          //  4 B
} RouteEntry;                  // 32 bytes
```

### `RouteTable` Struct (8 200 bytes — embedded in Device)

```c
typedef struct RouteTable {
    RouteEntry entries[256]; // 8 192 B
    int        count;        //     4 B
    int        _pad;         //     4 B
} RouteTable;                // 8 200 bytes
```

> **Note:** `Device` will need a `RouteTable route_tbl` field added
> alongside `arp_cache`. No separate allocation needed.

### Public API

| Function                                    | Purpose                                          |
|---------------------------------------------|--------------------------------------------------|
| `route_table_init(tbl)`                     | Zero-fill; count = 0.                            |
| `route_table_add(tbl, prefix, plen, nh, iface, metric, proto)` | Insert or update. |
| `route_table_delete(tbl, prefix, plen, proto)` | Remove matching entry.                      |
| `route_table_lookup(tbl, dst_ip)`           | LPM; return best `RouteEntry *` or NULL.         |
| `route_table_flush_proto(tbl, proto)`       | Remove all entries of given protocol (e.g. RIP). |

### ACSL Highlights

```
route_table_lookup:
  result != NULL ⇒ (result->prefix & mask(result->prefix_len)) ==
                   (dst_ip       & mask(result->prefix_len))
                && ∀ i: entries[i].valid && prefix_len[i] > result->prefix_len
                        ⇒ NOT a match for dst_ip

route_table_add:
  result == 0 ⇒ tbl->count == \old(tbl->count) + 1  (if new entry)
             OR tbl->count == \old(tbl->count)       (if update)
```

---

## Function Call Sequence — LPM Lookup

```
ip_forward(sim, iface, pkt):
   │
   └─► route = route_table_lookup(&dev->route_tbl, dst_ip)
           │
           │   best_match = NULL; best_plen = -1
           │
           │   for i in 0..count:
           │       if entries[i].valid:
           │           mask = 0xFFFFFFFF << (32 - prefix_len)
           │           if (dst_ip & mask) == (entries[i].prefix & mask):
           │               if entries[i].prefix_len > best_plen:
           │                   best_plen = entries[i].prefix_len
           │                   best_match = &entries[i]
           │
           └─► return best_match     (or NULL → ICMP unreachable)
```

## Function Call Sequence — RIP installs a route

```
rip_receive_update(sim, iface, update):
   │  for each (prefix, plen, metric) in update:
   │      if metric < ROUTE_INFINITY:
   │          route_table_add(&dev->route_tbl,
   │                          prefix, plen,
   │                          next_hop=iface->ip_addr,
   │                          iface,
   │                          metric,
   │                          ROUTE_PROTO_RIP)
   │      else:
   │          route_table_delete(&dev->route_tbl, prefix, plen, ROUTE_PROTO_RIP)
```

---

## Design Notes

- **LPM is O(N) linear scan** — fine for ≤256 entries. A production
  router would use a trie (Patricia tree or LC-trie).
- **Embedded in Device** avoids an extra malloc and NULL check every time
  IP calls `route_table_lookup`.
- **`metric` is protocol-agnostic** — RIP uses hop count (1–15), OSPF
  uses cost (1–65535), static routes use 0.
- **`route_table_flush_proto`** is called on daemon restart to clear
  stale dynamic routes before reinstalling fresh ones.
- **Connected routes** are installed automatically by
  `device_add_interface` at prefix `iface->ip_addr & mask`, nh = 0.

## Test Plan (kleva)

- `lookup_exact_match`, `lookup_lpm_wins_over_shorter`
- `lookup_default_route`, `lookup_no_match_returns_null`
- `add_new_entry`, `add_update_existing`, `add_table_full`
- `delete_removes_entry`, `flush_proto_removes_rip_keeps_static`
- NULL guards: `lookup_null_tbl`, `add_null_iface`
