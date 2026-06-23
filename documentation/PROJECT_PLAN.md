# Networking Simulator — Implementation Plan

Implementation order follows strict dependency: each module only calls modules
above it in the table. Numbers are sequential build order. Coverage targets:
≥90% line, ≥80% branch (kleva).

---

## Phase 1 — Core Infrastructure

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
|  1 | Packet buffer       | `network/packet.c/h`        | stdlib                  | ✅ Done         | 93% / 80% ✅           |
|  2 | Event system        | `engine/event.c/h`          | packet                  | ✅ Done         | 92% / 81% ✅           |
|  3 | Scheduler           | `engine/scheduler.c/h`      | event                   | ✅ Done         | 95% / 84% ✅           |
|  4 | Interface (NIC)     | `network/interface.c/h`     | stdlib                  | ✅ Done         | 98% / 93% ✅           |
|  5 | Link                | `network/link.c/h`          | interface, packet       | ✅ Done         | 89% / 80% ⚠️           |
|  6 | Device              | `network/device.c/h`        | interface, link, packet | ✅ Done         | 93% / 83% ✅           |
|  7 | Topology            | `network/topology.c/h`      | device, link            | ✅ Done         | 92% / 83% ✅           |
|  8 | Simulator           | `engine/simulator.c/h`      | topology, scheduler     | ✅ Done         | 96% / 81% ✅           |

---

## Phase 2 — L2 Protocols & Nodes

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
|  9 | Ethernet (L2)       | `protocols/ethernet.c/h`    | packet, interface       | ✅ Done         | 57% / 67% ❌           |
| 10 | ARP                 | `protocols/arp.c/h`         | ethernet, device        | ✅ Done         | 30% / 38% ❌           |
| 11 | MAC table           | `network/mac_table.c/h`     | interface               | ✅ Done         | 100% / 100% ✅         |
| 12 | Switch (L2)         | `network/switch.c/h`        | mac_table, ethernet     | ✅ Done         | 68% / 56% ❌           |

---

## Phase 3 — L3 Protocols & Host Node

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
| 13 | IPv4                | `protocols/ip.c/h`          | ethernet, arp           | ✅ Done         | 62% / 48% ❌           |
| 14 | ICMP                | `protocols/icmp.c/h`        | ip                      | ✅ Done         | 79% / 61% ❌           |
| 15 | UDP                 | `protocols/udp.c/h`         | ip, icmp                | ✅ Done         | 80% / 73% ❌           |
| 16 | TCP                 | `protocols/tcp.c/h`         | ip                      | ⬜ Not started  | —                      |
| 17 | Host                | `network/host.c/h`          | device, arp_cache, ip   | ⬜ Not started  | —                      |

---

## Phase 4 — Routing & Router Node

| #  | Module              | File(s)                     | Depends On                         | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|------------------------------------|-----------------|------------------------|
| 18 | Routing table       | `routing/route_table.c/h`   | device, ip                         | ⬜ Not started  | —                      |
| 19 | Router (L3)         | `network/router.c/h`        | device, arp_cache, route_table, ip | ⬜ Not started  | —                      |
| 20 | RIP                 | `routing/rip.c/h`           | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 21 | OSPF                | `routing/ospf.c/h`          | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 22 | BGP                 | `routing/bgp.c/h`           | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 23 | EIGRP               | `routing/eigrp.c/h`         | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 24 | IS-IS               | `routing/isis.c/h`          | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 25 | NAT / PAT           | `routing/nat.c/h`           | route_table, ip                    | ⬜ Not started  | —                      |

---

## Phase 5 — Display + CLI

| #  | Module              | File(s)                     | Depends On           | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|----------------------|-----------------|------------------------|
| 26 | Topology renderer   | `display/topology.c/h`      | topology             | ⬜ Not started  | —                      |
| 27 | Packet renderer     | `display/packet.c/h`        | packet, protocols    | ⬜ Not started  | —                      |
| 28 | CLI / REPL          | `cli/cli.c/h`               | simulator, all       | ⬜ Not started  | —                      |

---

## Dependency Graph

```
packet ─────────────────────────────────────────────┐
event ──────────────────────────────────────────┐   │
scheduler ──────────────────────────────────┐   │   │
                                            │   │   │
interface ──────────────┐                   │   │   │
link (interface)        │                   │   │   │
device (interface,link) │                   │   │   │
topology (device,link)  │                   │   │   │
simulator ──────────────┴───────────────────┘   │   │
                                                │   │
ethernet (packet, interface) ───────────────────┘   │
arp (ethernet, device)                              │
mac_table (interface)                               │
switch (mac_table, ethernet)                        │
ip (ethernet, arp) ─────────────────────────────────┘
icmp / udp / tcp (ip)
host (device, arp_cache, ip)
route_table (device, ip)
router (device, arp_cache, route_table, ip)
rip / ospf / bgp / eigrp / isis (route_table, router, scheduler)
nat (route_table, ip)
display / cli (everything)
```

---

## Key Design Rules

- Every `.c` file has a matching `.h` with ACSL contracts on all public functions
- Every module gets a `kleva/` YAML and reaches ≥90% line / ≥80% branch before moving on
- No module reaches upward (e.g. `interface.c` must not call `device.c`)
- `malloc` failure branches are excluded from coverage targets (guarded by `-eva-no-alloc-returns-null`)

## Latest Coverage Notes

Fresh coverage for rows 9-14 was generated on 2026-06-11 under
`tests/coverage/project_plan_20260611/`.

UDP coverage was generated on 2026-06-16 under
`tests/coverage/udp_kleva_shapes/` after KLEVA generated 26 UDP test vectors
with 38 EVA-proven assertions and 0 unproven assertions.

- MAC table is above the coverage baseline.
- Ethernet, ARP, Switch, IPv4, and ICMP are implemented but below the
  required coverage baseline.
- UDP is implemented and has generated KLEVA tests, but remains below the
  required line and branch coverage baseline.
- Switch coverage compilation currently needs the generated test to see the
  system declaration for `htons`; the coverage run used `-include arpa/inet.h`.
