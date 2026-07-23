# Networking Simulator — Implementation Plan

Module numbers preserve the specification catalog and dependency layering; they
are not a rule that every optional protocol must be completed before the next
runnable milestone. The current priority is Modules 03 and 28–35 plus their listed
retrofits. BGP, IS-IS, and NAT/PAT can resume after that milestone. Coverage
targets remain ≥90% line and ≥80% branch (KLEVA).

Status meanings:

- ✅ Done: current implementation already satisfies its specification.
- 🟡 Base implemented; retrofit pending: existing behavior is implemented, but
  the identity, trace, device-type, or runnable-simulator changes listed below
  are not implemented yet.
- ⬜ Empty files / Not started: the required implementation does not yet exist.

---

## Phase 1 — Core Infrastructure

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
|  1 | Packet buffer       | `network/packet.c/h`        | stdlib                  | ✅ Done         | 93% / 80% ✅           |
|  2 | Event system        | `engine/event.c/h`          | packet                  | ✅ Done         | 92% / 81% ✅           |
|  3 | Simulation trace    | `engine/trace.c/h`          | packet, event           | ✅ Done         | —                      |
|  4 | Scheduler           | `engine/scheduler.c/h`      | event, trace            | 🟡 Base implemented; retrofit pending | 95% / 84% ✅ |
|  5 | Interface (NIC)     | `network/interface.c/h`     | stdlib                  | ✅ Done         | 98% / 93% ✅           |
|  6 | Link                | `network/link.c/h`          | interface, packet       | 🟡 Base implemented; retrofit pending | 89% / 80% ⚠️ |
|  7 | Device              | `network/device.c/h`        | interface, link, packet | 🟡 Base implemented; retrofit pending | 93% / 83% ✅ |
|  8 | Topology            | `network/topology.c/h`      | device, link            | ✅ Done         | 92% / 83% ✅           |
|  9 | Simulator           | `engine/simulator.c/h`      | topology, scheduler, trace | 🟡 Base implemented; retrofit pending | 96% / 81% ✅ |

---

## Phase 2 — L2 Protocols & Nodes

| #  | Module              | File(s)                     | Depends On              | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|-------------------------|-----------------|------------------------|
| 10 | Ethernet (L2)       | `protocols/ethernet.c/h`    | packet, interface       | 🟡 Base implemented; retrofit pending | 57% / 67% ❌ |
| 11 | ARP cache           | `protocols/arp_cache.c/h`   | interface, packet, ip   | ✅ Done         | —                      |
| 12 | ARP                 | `protocols/arp.c/h`         | ethernet, arp_cache     | 🟡 Base implemented; retrofit pending | 30% / 38% ❌ |
| 13 | MAC table           | `network/mac_table.c/h`     | interface               | ✅ Done         | 100% / 100% ✅         |
| 14 | Switch (L2)         | `network/switch.c/h`        | mac_table, ethernet     | 🟡 Base implemented; retrofit pending | 68% / 56% ❌ |

---

## Phase 3 — L3 Protocols & Host Node

| #  | Module              | File(s)                     | Depends On               | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|--------------------------|-----------------|------------------------|
| 15 | IPv4                | `protocols/ip.c/h`          | ethernet, arp_cache, arp | 🟡 Base implemented; retrofit pending | 62% / 48% ❌ |
| 16 | ICMP                | `protocols/icmp.c/h`        | ip                       | 🟡 Base implemented; retrofit pending | 79% / 61% ❌ |
| 17 | UDP                 | `protocols/udp.c/h`         | ip, icmp                 | 🟡 Base implemented; retrofit pending | 80% / 73% ❌ |
| 18 | TCP                 | `protocols/tcp.c/h`         | ip                       | 🟡 Base implemented; retrofit pending | — |
| 19 | Host                | `network/host.c/h`          | device, arp_cache, ip    | 🟡 Base implemented; retrofit pending | — |

---

## Phase 4 — Routing & Router Node

| #  | Module              | File(s)                     | Depends On                         | Status          | Tests (line / branch)  |
|----|---------------------|-----------------------------|------------------------------------|-----------------|------------------------|
| 20 | Routing table       | `routing/route_table.c/h`   | device, ip                         | ✅ Done         | —                      |
| 21 | Router (L3)         | `network/router.c/h`        | device, arp_cache, route_table, ip | 🟡 Base implemented; retrofit pending | — |
| 22 | Static Route        | `routing/static_route.c/h`  | route_table, router, interface     | ✅ Done         | —                      |
| 23 | RIP                 | `protocols/rip.c/h`         | route_table, router, scheduler     | 🟡 Base implemented; retrofit pending | — |
| 24 | OSPF                | `protocols/ospf.c/h`        | route_table, router, scheduler     | 🟡 Base implemented; retrofit pending | — |
| 25 | BGP                 | `protocols/bgp.c/h`         | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 26 | IS-IS               | `protocols/isis.c/h`        | route_table, router, scheduler     | ⬜ Not started  | —                      |
| 27 | NAT / PAT           | `protocols/nat.c/h`         | route_table, ip                    | ⬜ Not started  | —                      |

---

## Phase 5 — Display Foundations

| #  | Module              | File(s)                              | Depends On           | Status          | Tests (line / branch)  |
|----|---------------------|--------------------------------------|----------------------|-----------------|------------------------|
| 28 | Topology report     | `display/topology_view.c/h`          | topology             | ⬜ Not started  | —                      |
| 29 | Packet header view  | `display/header_view.c/h`            | packet, protocols    | ⬜ Not started  | —                      |
| 30 | Automatic topology layout | `display/topology_layout.c/h`  | topology, device, link | ⬜ Not started | —                     |
| 31 | Trace event-log renderer | `display/event_log.c/h`         | trace                | ⬜ Empty files | —                     |

## Phase 6 — Runnable Simulator + Animation

This is the next implementation milestone. BGP, IS-IS, and NAT remain in the
plan, but they are not prerequisites for proving the existing simulator
end-to-end.

| #  | Module                    | File(s)                              | Depends On                    | Status          | Tests (line / branch) |
|----|---------------------------|--------------------------------------|-------------------------------|-----------------|-----------------------|
| 32 | Topology configuration    | `config/topology_config.c/h`         | simulator, topology, devices  | ⬜ Not started  | —                     |
| 33 | Terminal animation        | `display/animation.c/h`              | trace, topology_layout, topology_view, header_view, event_log | ⬜ Not started | — |
| 34 | CLI core / REPL           | `cli/cli.c/h`                        | simulator, animation          | ⬜ Empty files | — |
| 35 | CLI built-in commands     | `cli/commands.c/h`                   | cli, topology_config, simulator, animation, topology_view, event_log, device, link, route_table, icmp | ⬜ Empty files | — |

### Required Retrofit Before Phase 6 Is Complete

The following modules already have base implementations. They require the
trace, identity, ordering, and device-type additions defined by their updated
module specifications; this does not reopen unrelated protocol behavior.

| Area | Specifications | Required integration |
|---|---|---|
| Packet and event identity | 01, 02 | Packet lineage and deterministic event sequence |
| Scheduler and simulator | 04, 09 | Own the trace log and emit scheduled/start/finish records |
| Network movement | 06, 07 | Device type and link departure/arrival trace records |
| Layer 2 and Layer 3 | 10, 12, 14, 15, 16, 19, 21 | Record semantic receive, decision, send, drop, and delivery actions |
| Transport and routing | 17, 18, 23, 24 | Record state changes, control traffic, route changes, and timers |

### Phase 6 Acceptance Scenario

Phase 6 is complete when one executable can:

1. Load a text topology file without partially replacing the active simulator
   when parsing or construction fails.
2. Calculate deterministic display positions without topology-file coordinates
   or visual fields in network structs.
3. Inject a ping in a topology containing hosts, switches, and at least two
   routers, then show the resulting protocol and device decisions.
4. Animate multiple in-flight packets whose simulated lifetimes overlap.
5. Advance by one event, by animation ticks, by a bounded simulated duration,
   or by a bounded number of events.
6. Focus the view on all traffic, one device, one packet ID, or one trace ID.
7. Display persistent packet/header snapshots after the live packet has been
   freed.
8. Exit without packet, event, trace, topology, or display-state leaks.

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
arp_cache (interface, packet, ip)                   │
arp (ethernet, arp_cache)                           │
mac_table (interface)                               │
switch (mac_table, ethernet)                        │
ip (ethernet, arp_cache, arp) ──────────────────────┘
icmp / udp / tcp (ip)
host (device, arp_cache, ip)
route_table (device, ip)
router (device, arp_cache, route_table, ip)
rip / ospf / bgp / isis (route_table, router, scheduler)
nat (route_table, ip)
topology_config (simulator, topology, devices, links, routes)
trace (packet, event)
topology_layout (topology, device, link)
topology_view / header_view / event_log (topology, trace, protocols)
animation (trace, topology_layout, topology_view, header_view, event_log)
cli core (simulator, animation)
cli commands (cli core, topology_config, simulator, animation, display,
              owning network/protocol APIs)
```

---

## Key Design Rules

- Every `.c` file has a matching `.h` with ACSL contracts on all public functions
- Every module gets a `kleva/` YAML and reaches ≥90% line / ≥80% branch before moving on
- No module reaches upward (e.g. `interface.c` must not call `device.c`)
- `malloc` failure branches are excluded from coverage targets (guarded by `-eva-no-alloc-returns-null`)
- The simulator remains single-threaded. Simultaneous device activity is
  represented by timestamped events, not per-device operating-system threads.
- Repeated runs are deterministic: equal-time events are ordered by event
  sequence, and automatic topology layout uses stable topology order.
- Topology configuration contains network facts only. Visual coordinates are
  calculated by the display layout module and are never stored in topology,
  device, interface, or link objects.

## Latest Coverage Notes

Fresh coverage for Ethernet, ARP, Switch, IPv4, and ICMP was generated on
2026-06-11 under
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
