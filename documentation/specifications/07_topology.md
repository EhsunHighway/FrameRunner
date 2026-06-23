# Module 07 — Topology

**Files:** `src/network/topology.c`, `src/network/topology.h`
**Status:** ✅ Implemented (92% / 83%)
**Depends on:** device, link, interface

---

## The Problem

`Device` is one node. `Link` is one cable. A simulation is **the whole
network** — every device and every link. We need a registry that:

1. Owns every `Device *` and `Link *`.
2. Lets the renderer iterate without knowing internal layout.
3. Provides lookups: device by name (CLI: `R1 ping ...`) and by IP
   (renderer needs to know "this IP belongs to that node").
4. Helps find the `Link` between two interfaces (used when wiring or
   for `show neighbors`).

## Mental Model

```
   Topology
   ├─ devices: [H1, H2, R1, R2, SW1]   (heap, grows via realloc)
   └─ links:   [L1, L2, L3, L4]        (heap, grows via realloc)

       H1 ─L1─ SW1 ─L2─ R1 ─L3─ R2 ─L4─ H2
```

Storage uses pointer arrays, each with a `cap` and a `count`. On
overflow, `realloc` doubles the capacity — amortised O(1) inserts.

---

## Header File — `topology.h`

### Struct

```c
typedef struct Topology {
    Device **devices;
    int      dev_count;
    int      dev_cap;
    Link   **links;
    int      link_count;
    int      link_cap;
} Topology;
```

### Public API

| Function                            | Purpose                              |
|-------------------------------------|--------------------------------------|
| `topology_create()`                 | Allocate with non-zero `dev_cap` and `link_cap`. |
| `topology_free`                     | Frees every device, every link, then arrays + self. |
| `topology_add_device`               | Append (realloc on full). Returns 0/−1.          |
| `topology_add_link`                 | Same for links.                                  |
| `topology_find_device_by_name`      | Linear scan.                                     |
| `topology_find_device_by_ip`        | Device → Interface scan.                         |
| `topology_get_link_between(a,b)`    | Find a link whose two ends are `a` and `b`.      |
| `topology_device_count / link_count`| Trivial accessors for iteration.                 |

### Ownership

`topology_free` frees **everything** (devices via `device_free`, links
via `link_free`, plus the two arrays and the struct). Adding a device
or link to the topology transfers ownership to it.

Implementation note: the current `topology_free` frees devices first and
links second. This is safe only because `link_free` does not dereference
link endpoints. If link teardown later touches interfaces, free links
before devices.

---

## Call Sequence — Building a tiny topology

```
topo = topology_create()

h1  = device_create("H1", 1)
sw1 = device_create("SW1", 4)
topology_add_device(topo, h1)
topology_add_device(topo, sw1)

e0  = interface_create("eth0", mac_h1, ip_h1, 24, 1500)
sw0 = interface_create("g0/0", mac_sw1, 0, 0, 1500)
device_add_interface(h1, e0)        ← sets e0->device = h1
device_add_interface(sw1, sw0)      ← sets sw0->device = sw1

l1 = link_create(e0, sw0, 1000 /*Mbps*/, 1 /*ms*/, 0.0f)
topology_add_link(topo, l1)
interface_set_link(e0,  l1)
interface_set_link(sw0, l1)
```

## Call Sequence — Lookup

```
dev = topology_find_device_by_name(topo, "H1")
   │  for i in 0..dev_count:
   │      if strcmp(devices[i]->name, "H1") == 0: return devices[i]
   ▼
returns pointer or NULL

dev = topology_find_device_by_ip(topo, 192.168.1.10)
   │  for d in devices:
   │      for j in 0..d->iface_count:
   │          if d->interfaces[j]->ip_addr == target: return d
   ▼
returns first owning device or NULL
```

---

## Design Notes

- **Owns by composition.** Devices and links live as long as the
  topology. The simulator just borrows the topology pointer.
- **Linear scans.** A workspace of < 100 nodes is the design target.
  Promotable to a hash map without API change.
- **Capacity grows via realloc.** First `topology_create` sets a small
  initial `cap` (e.g. 8) and `_add_*` doubles it when full.
- **`get_link_between` walks `links[]`** comparing `(end_a, end_b)`
  against the requested pair in either order — used by the renderer and
  by future LLDP-style modules.

## Implementation Guide

1. `topology_create`: allocate topology, initialize device/link counts
   to zero, capacities to `8`, and allocate both pointer arrays. If
   either array allocation fails, free all partial allocations.
2. `topology_add_device` / `topology_add_link`: reject NULLs; double the
   corresponding capacity with `realloc` when full; append on success.
3. `topology_find_device_by_name`: linear scan by `strcmp`.
4. `topology_find_device_by_ip`: nested scan over devices and their live
   interfaces.
5. `topology_get_link_between`: compare endpoint pairs in either order.

## ACSL Contract Plan

- Add functions: success increments the relevant count and stores the
  pointer at the old count. Realloc failure leaves count and old array
  intact.
- Lookup functions: hit postconditions should prove the returned pointer
  came from the topology; miss postconditions should quantify over live
  entries only.
- Count accessors: NULL topology returns `0`; valid topology returns the
  stored count.
