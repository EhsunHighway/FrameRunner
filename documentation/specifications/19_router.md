# Module 28 — Router

**Files:** `src/network/router.c`, `src/network/router.h`
**Status:** ⬜ Not started
**Depends on:** device, arp_cache, route_table, ip

---

## The Problem

A router is a Layer-3 forwarding node. It:

1. Receives an IP datagram on any interface.
2. Decrements TTL and drops (ICMP TTL-exceeded) if TTL hits zero.
3. Performs a **Longest Prefix Match** (LPM) lookup in its `RouteTable`.
4. Resolves the next-hop IP → MAC via its `ArpCache` (or sends an ARP request).
5. Re-encapsulates in a new Ethernet frame and forwards out the egress interface.

`Router` is a **thin specialization of `Device`** — it adds an embedded
`ArpCache` and `RouteTable`. Unlike `Switch`, it never looks at MAC
source/destination for forwarding decisions. Unlike `Host`, it has a full
LPM table rather than a single default gateway.

## Mental Model

```
   Router "R1"
   ┌──────────────────────────────────────────────────────────┐
   │  Device base (name, interfaces[], iface_count)           │
   │                                                          │
   │  arp_cache  (embedded, ~4 KB)                            │
   │    10.0.0.2   → CC:CC:CC:CC:CC:CC  (next-hop, valid)     │
   │    192.168.1.1 → AA:AA:AA:AA:AA:AA  (connected, valid)   │
   │                                                          │
   │  route_tbl  (embedded, ~8 KB)                            │
   │    0.0.0.0/0     → nh=10.0.0.1  iface=eth1  (default)   │
   │    192.168.1.0/24→ nh=0.0.0.0   iface=eth0  (connected) │
   │    10.0.0.0/8    → nh=0.0.0.0   iface=eth1  (connected) │
   │    172.16.5.0/24 → nh=10.0.0.2  iface=eth1  (RIP)       │
   │                                                          │
   │  sim  → Simulator *                                      │
   │                                                          │
   │  interfaces[0] → eth0  192.168.1.1/24 ─── SW1 g0/2      │
   │  interfaces[1] → eth1  10.0.0.1/30   ─── R2  eth0       │
   └──────────────────────────────────────────────────────────┘

   Forwarding path for dst=172.16.5.99:
     LPM: 172.16.5.0/24 → nh=10.0.0.2, iface=eth1
     ARP: 10.0.0.2 → CC:CC:CC:CC:CC:CC  (cache hit)
     ethernet_send(eth1, CC:CC:CC:CC:CC:CC, pkt)
```

---

## Header File — `router.h`

### `Router` Struct

```c
typedef struct Router {
    Device     base;        /* MUST be first — cast Router* → Device* is valid */
    ArpCache   arp_cache;   /* next-hop IP → MAC resolution                    */
    RouteTable route_tbl;   /* LPM forwarding table (~8 KB, embedded)          */
    Simulator *sim;         /* for scheduling ARP/routing events               */
} Router;
```

`Device base` must be at offset 0 so `router_add_interface` can delegate
to `device_add_interface((Device *)r, iface)` safely.

Both `ArpCache` and `RouteTable` are embedded — no malloc, no NULL guard.

### Public API

| Function                                     | Purpose                                                         |
|----------------------------------------------|-----------------------------------------------------------------|
| `router_create(name, sim)`                   | Alloc, init base Device, zero arp_cache and route_tbl.          |
| `router_free(r)`                             | Free interfaces owned by base, then `Router` itself.            |
| `router_add_interface(r, iface)`             | Delegates to `device_add_interface`, installs rx handler.       |
| `router_receive(r, in_iface, pkt)`           | TTL check → LPM → ARP resolve → forward or drop.               |
| `router_add_route(r, prefix, len, nh, iface, proto)` | Insert static/protocol route into `route_tbl`.         |
| `router_del_route(r, prefix, len)`           | Remove matching route from `route_tbl`.                         |

### ACSL Highlights

```
router_create:
  result != NULL ==>
    \valid(result) &&
    result->base.iface_count == 0 &&
    result->route_tbl.count  == 0

router_receive (TTL drop):
  ip_ttl(pkt) <= 1
  ==> icmp_send_ttl_exceeded(r, in_iface, pkt)
  &&  \result == -1

router_receive (LPM miss):
  route_table_lookup(&r->route_tbl, dst_ip) == NULL
  ==> icmp_send_unreachable(r, in_iface, pkt)
  &&  \result == -1

router_receive (forward):
  route_table_lookup(&r->route_tbl, dst_ip) == entry &&
  entry != NULL &&
  arp_cache_lookup(&r->arp_cache, entry->next_hop) == dst_mac &&
  dst_mac != NULL
  ==> ethernet_send(entry->iface, dst_mac, pkt)
  &&  \result == 0
```

---

## Function Call Sequence — Creating a router

```
sim  = simulator_create(topo, sched)
r    = router_create("R1", sim)
         │
         ├─► device_create("R1", 16)          ← base.interfaces[]
         ├─► arp_cache_init(&r->arp_cache)
         ├─► route_table_init(&r->route_tbl)
         └─► r->sim = sim

eth0 = interface_create("eth0", mac0, 192.168.1.1, 24, 1500)
router_add_interface(r, eth0)
         │
         └─► device_add_interface(&r->base, eth0)

router_add_route(r, 0, 0, 10.0.0.1, eth1, ROUTE_PROTO_STATIC)   ← default
router_add_route(r, 192.168.1.0, 24, 0, eth0, ROUTE_PROTO_DIRECT)
```

---

## Forwarding Decision Flow

```
router_receive(r, in_iface, pkt)
  │
  ├─ TTL <= 1?  ─────────────────────────────► icmp_ttl_exceeded, drop
  │
  ├─ decrement TTL
  │
  ├─ route_table_lookup(dst_ip)
  │     NULL? ──────────────────────────────► icmp_unreachable, drop
  │
  ├─ arp_cache_lookup(entry->next_hop)
  │     NULL? ──────────────────────────────► arp_send_request, queue pkt
  │
  └─ ethernet_send(entry->iface, resolved_mac, pkt)
```

---

## Design Rules

- `Router` performs **no MAC learning** — it has no `MacTable`.
- `(Device *)r` cast is always valid because `base` is at offset 0.
- Routing protocol daemons (RIP, OSPF, etc.) take a `Router *` and call
  `router_add_route` / `router_del_route` to install learned routes.
- `ArpCache` and `RouteTable` are both owned and embedded; `router_free`
  does not call `free` on them separately.
- On ARP miss, the packet is **dropped and ARP is triggered** — no
  queueing of pending packets (simplification acceptable for simulator).
