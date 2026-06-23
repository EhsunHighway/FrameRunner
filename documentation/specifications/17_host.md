# Module 27 — Host

**Files:** `src/network/host.c`, `src/network/host.h`
**Status:** ⬜ Not started
**Depends on:** device, arp_cache, ip

---

## The Problem

A host is the simplest IP-capable node in the topology. It:

1. Has one (usually) or more `Interface *` ports via the `Device` base.
2. Owns an `ArpCache` to resolve next-hop IP → MAC before sending frames.
3. Sends and receives IP datagrams — no routing, no MAC table.
4. Uses a default gateway IP for all off-subnet traffic.

`Host` is a **thin specialization of `Device`** — it adds only the ARP
cache and a default-gateway field. Unlike `Router`, it does **not** run
LPM lookup; it either sends directly (on-subnet) or forwards everything
to the gateway (off-subnet).

## Mental Model

```
   Host "H1"
   ┌─────────────────────────────────────────────────────────┐
   │  Device base (name, interfaces[], iface_count)          │
   │                                                         │
   │  arp_cache  (embedded, ~4 KB)                           │
   │    192.168.1.1 → AA:AA:AA:AA:AA:AA  (gateway, valid)    │
   │    192.168.1.5 → BB:BB:BB:BB:BB:BB  (peer, valid)       │
   │                                                         │
   │  gateway = 192.168.1.1  (default route next-hop)        │
   │                                                         │
   │  interfaces[0] → eth0  192.168.1.10/24  ─── SW1 g0/0   │
   └─────────────────────────────────────────────────────────┘
```

---

## Header File — `host.h`

### `Host` Struct

```c
typedef struct Host {
    Device   base;         /* MUST be first — cast Host* → Device* is valid */
    ArpCache arp_cache;    /* next-hop IP → MAC resolution                  */
    uint32_t gateway;      /* default gateway IP (0 = none)                 */
    Simulator *sim;        /* for ARP event scheduling                      */
} Host;
```

`Device base` must be at offset 0 so `host_add_interface` can call
`device_add_interface((Device *)h, iface)` safely.

`ArpCache` is intentionally embedded — no malloc, no NULL guard needed.

### Public API

| Function                                        | Purpose                                                |
|-------------------------------------------------|--------------------------------------------------------|
| `host_create(name, gateway_ip, sim)`            | Alloc, init `base` Device, zero `arp_cache`, store gateway. |
| `host_free(h)`                                  | Free interfaces owned by base, then `Host` itself.     |
| `host_add_interface(h, iface)`                  | Delegates to `device_add_interface`.                   |
| `host_send(h, dst_ip, payload, len)`            | ARP-resolve dst (or gateway), then `ip_send`.          |
| `host_receive(h, in_iface, pkt)`                | Hand packet up to IP layer.                            |

### ACSL Highlights

```
host_create:
  result != NULL ==>
    \valid(result) &&
    result->gateway == gateway_ip &&
    result->base.iface_count == 0

host_send (on-subnet):
  arp_cache_lookup(&h->arp_cache, dst_ip) != NULL
  ==> ethernet_send called with resolved MAC

host_send (off-subnet or ARP miss):
  arp_cache_lookup(&h->arp_cache, dst_ip) == NULL
  ==> arp_send_request called, packet queued pending ARP reply
```

---

## Function Call Sequence — Creating a host

```
sim  = simulator_create(topo, sched)
h    = host_create("H1", 192.168.1.1, sim)
         │
         ├─► device_create("H1", 4)        ← base.interfaces[]
         ├─► arp_cache_init(&h->arp_cache)
         └─► h->gateway = 192.168.1.1

eth0 = interface_create("eth0", mac, ip, 24, 1500)
host_add_interface(h, eth0)
         │
         └─► device_add_interface(&h->base, eth0)
```

---

## Design Rules

- `Host` has **no routing table** and performs **no LPM lookup**.
- Off-subnet traffic is always forwarded to `h->gateway` — one static default route.
- `ArpCache` is owned and embedded; `host_free` does not call `free` on it separately.
- No MAC table — hosts are not switches.
- `(Device *)h` cast is always valid because `base` is at offset 0.
