# Module Specifications

One markdown file per module of the networking simulator. Each spec
explains the module's **purpose**, its **mental model** (with ASCII
illustration), the **public API**, and the **sequential call flow** of
its main entry points. Use these as both reading material for the
already-implemented modules and as design blueprints for the upcoming
ones.

---

## Phase 1 — Core Infrastructure ✅

| #  | Module     | Spec                                          | Status |
|----|------------|-----------------------------------------------|--------|
| 01 | Packet     | [01_packet.md](01_packet.md)                  | ✅ Done |
| 02 | Event      | [02_event.md](02_event.md)                    | ✅ Done |
| 03 | Scheduler  | [03_scheduler.md](03_scheduler.md)            | ✅ Done |
| 04 | Interface  | [04_interface.md](04_interface.md)            | ✅ Done |
| 05 | Link       | [05_link.md](05_link.md)                      | ✅ Done |
| 06 | Device     | [06_device.md](06_device.md)                  | ✅ Done |
| 07 | Topology   | [07_topology.md](07_topology.md)              | ✅ Done |
| 08 | Simulator  | [08_simulator.md](08_simulator.md)            | ✅ Done |

## Phase 2 — Protocols

| #  | Module     | Spec                                          | Status |
|----|------------|-----------------------------------------------|--------|
| 09 | Ethernet   | [09_ethernet.md](09_ethernet.md)              | ✅ Done |
| 10 | ARP Cache  | [10_arp_cache.md](10_arp_cache.md)            | ✅ Done |
| 11 | ARP        | [11_arp.md](11_arp.md)                        | ✅ Done |
| 12 | MAC Table  | [12_mac_table.md](12_mac_table.md)            | ⬜ Next |
| 13 | Switch     | [13_switch.md](13_switch.md)                  | ⬜      |
| 14 | IPv4       | [14_ip.md](14_ip.md)                          | ⬜      |
| 15 | ICMP       | [15_icmp.md](15_icmp.md)                      | ⬜      |
| 16 | UDP        | [16_udp.md](16_udp.md)                        | ⬜      |
| 17 | TCP        | [17_tcp.md](17_tcp.md)                        | ⬜      |
| 18 | Host       | [18_host.md](18_host.md)                      | ⬜      |

## Phase 3 — Routing

| #  | Module        | Spec                                          | Status |
|----|---------------|-----------------------------------------------|--------|
| 19 | Route Table   | [19_route_table.md](19_route_table.md)        | ⬜      |
| 20 | Router        | [20_router.md](20_router.md)                  | ⬜      |
| 21 | RIP           | [21_rip.md](21_rip.md)                        | ⬜      |
| 22 | OSPF          | [22_ospf.md](22_ospf.md)                      | ⬜      |
| 23 | BGP           | [23_bgp.md](23_bgp.md)                        | ⬜      |
| 24 | EIGRP         | [24_eigrp.md](24_eigrp.md)                    | ⬜      |
| 25 | IS-IS         | [25_isis.md](25_isis.md)                      | ⬜      |
| 26 | NAT / PAT     | [26_nat.md](26_nat.md)                        | ⬜      |

## Phase 4 — Display + CLI

| #  | Module             | Spec                                                 | Status |
|----|--------------------|------------------------------------------------------|--------|
| 27 | Topology Renderer  | [27_display_topology.md](27_display_topology.md)     | ⬜      |
| 28 | Packet Renderer    | [28_display_packet.md](28_display_packet.md)         | ⬜      |
| 29 | CLI / REPL         | [29_cli.md](29_cli.md)                               | ⬜      |

---

## How to Use These Files

- **Already-implemented modules** (01–11) describe the *actual code* —
  data structures, function call order, handler wiring. They are kept
  in sync with `src/`.
- **Future modules** (12–29) are *design blueprints* — use them as the
  starting point when implementing each one. Update the spec to match
  the real code once the module ships.
- Every spec includes a **Sequential Call Flow** section — read this
  first to understand what the module *does*, then look at the source.
