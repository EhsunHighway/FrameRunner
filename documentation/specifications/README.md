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
| 10 | ARP        | [10_arp.md](10_arp.md)                        | ✅ Done |
| 11 | MAC Table  | [11_mac_table.md](11_mac_table.md)            | ⬜ Next |
| 12 | Switch     | [12_switch.md](12_switch.md)                  | ⬜      |
| 13 | IPv4       | [13_ip.md](13_ip.md)                          | ⬜      |
| 14 | ICMP       | [14_icmp.md](14_icmp.md)                      | ⬜      |
| 15 | UDP        | [15_udp.md](15_udp.md)                        | ⬜      |
| 16 | TCP        | [16_tcp.md](16_tcp.md)                        | ⬜      |

## Phase 3 — Routing

| #  | Module        | Spec                                          | Status |
|----|---------------|-----------------------------------------------|--------|
| 17 | Route Table   | [17_route_table.md](17_route_table.md)        | ⬜      |
| 18 | RIP           | [18_rip.md](18_rip.md)                        | ⬜      |
| 19 | OSPF          | [19_ospf.md](19_ospf.md)                      | ⬜      |
| 20 | BGP           | [20_bgp.md](20_bgp.md)                        | ⬜      |
| 21 | EIGRP         | [21_eigrp.md](21_eigrp.md)                    | ⬜      |
| 22 | IS-IS         | [22_isis.md](22_isis.md)                      | ⬜      |
| 23 | NAT / PAT     | [23_nat.md](23_nat.md)                        | ⬜      |

## Phase 4 — Display + CLI

| #  | Module             | Spec                                                 | Status |
|----|--------------------|------------------------------------------------------|--------|
| 24 | Topology Renderer  | [24_display_topology.md](24_display_topology.md)     | ⬜      |
| 25 | Packet Renderer    | [25_display_packet.md](25_display_packet.md)         | ⬜      |
| 26 | CLI / REPL         | [26_cli.md](26_cli.md)                               | ⬜      |

---

## How to Use These Files

- **Already-implemented modules** (01–10) describe the *actual code* —
  data structures, function call order, handler wiring. They are kept
  in sync with `src/`.
- **Future modules** (11–26) are *design blueprints* — use them as the
  starting point when implementing each one. Update the spec to match
  the real code once the module ships.
- Every spec includes a **Sequential Call Flow** section — read this
  first to understand what the module *does*, then look at the source.
