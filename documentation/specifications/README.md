# Module Specifications

This directory contains one implementation specification per simulator module.

Each numbered module spec is written to teach the concept first, then define
the implementation contract clearly enough that the module can be implemented
and tested without guessing.

## Standard Format

Every numbered spec uses this structure:

1. Module header: files, status, dependencies
2. Concepts First
3. Purpose
4. Architecture Boundary
5. Data Model
6. Ownership And Lifetime
7. Public API
8. Function Behavior
9. Flow Charts
10. ACSL Contracts
11. KLEVA Verification Plan
12. Common Mistakes

### Concepts First

This section must teach the module and the networking or simulator concept
clearly enough that implementation words have meaning. It should define the
important vocabulary, explain why the module exists, and name the real-world
idea being simplified when there is one.

Good examples:

- ARP cache explains learned mappings and pending packets.
- Route table explains RIB, FIB, LPM, administrative distance, and metric.
- Host explains per-host protocol state and context pointers.

Avoid vague concept sections that only restate function names.

### Data Model Descriptions

Every public struct, enum, and important internal record in the Data Model
section needs a short description before the C definition.

That description must answer:

- what the object represents
- who owns it or whether it is wire data
- how it relates to nearby objects

Expand acronyms before using them as section labels or struct names. For
example, write "LSU means Link-State Update" before explaining an LSU body, and
write "TCB means Transmission Control Block" before describing `Tcb`.

### Wire And Header Formats

When a module defines bytes on the wire, the spec must show the packet or
header format before the function behavior that parses or builds it.

Use plain-text diagrams that match the C structs and constants. The diagram
should identify:

- field order
- field width
- where payload bytes begin
- which multi-byte fields are network byte order
- which fields are fixed, ignored, or simplified in this simulator

This is required for protocol modules such as Ethernet, ARP, IPv4, ICMP, UDP,
TCP, RIP, OSPF, BGP, and IS-IS. It is optional for internal-only data
modules such as ARP cache, MAC table, route table, Host, Router, and CLI unless
a visual layout would remove implementation ambiguity.

### Function Behavior Format

Function behavior is the implementation contract. It is not only a behavior
summary.

For simple functions, an `Implementation order` list is acceptable when the order is obvious and the function has no tricky ownership, state-machine, lookup, or selection logic.

For non-trivial functions, use this structure:

```text
Behavior summary:

Implementation order:

Postconditions:
```

Definitions:

- **Behavior summary** says what the function accomplishes.
- **Implementation order** tells the coder what to do in execution order.
- **Postconditions** say what must be true after the function returns.

When order matters, preserve order. Do not put a step early in the prose if it
must happen later in code. If order does not matter, say that explicitly.
Do not mix final-state facts into `Implementation order`; put them under
`Postconditions` unless the implementation must check that fact at that exact
point in control flow.

Use exact names for important values and fields. Prefer:

```text
Compare the candidate RIB entry (`rib_entry`, table->rib[i]) against the
current selected RIB entry (`table->rib[matching_fib->rib_index]`).
```

Avoid vague wording like:

```text
compare this entry against that entry
use this to find that
prepare the context
deliver the packet
```

unless the sentence immediately names the exact object, field, function, or
postcondition.

### ACSL And KLEVA

ACSL contracts belong in headers. They should describe checkable shape,
ownership, counter, and state properties. Do not write impressive-looking
predicates that do not correspond to a real invariant.

KLEVA plans should list concrete test/proof properties, especially for:

- NULL and malformed inputs
- ownership transfer
- counter updates
- byte order
- queue full/empty cases
- packet free/send/queued paths

For implemented modules, the spec should match the current `src/` behavior.

For future modules, the spec is a design contract. When implementation changes
the real behavior, update the spec in the same change.

Module numbers are stable catalog identifiers. Phase placement describes the
architectural layer and implementation prerequisite; it does not require
numbers inside one phase to be contiguous.

## Phase 1 - Core Infrastructure

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 01 | Packet | [01_packet.md](01_packet.md) | Updated |
| 02 | Event | [02_event.md](02_event.md) | Updated |
| 30 | Simulation Trace | [30_trace.md](30_trace.md) | Ready for implementation |
| 03 | Scheduler | [03_scheduler.md](03_scheduler.md) | Trace retrofit pending |
| 04 | Interface | [04_interface.md](04_interface.md) | Updated |
| 05 | Link | [05_link.md](05_link.md) | Updated |
| 06 | Device | [06_device.md](06_device.md) | Updated |
| 07 | Topology | [07_topology.md](07_topology.md) | Updated |
| 08 | Simulator | [08_simulator.md](08_simulator.md) | Trace retrofit pending |

## Phase 2 - Protocols

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 09 | Ethernet | [09_ethernet.md](09_ethernet.md) | Updated |
| 10 | ARP Cache | [10_arp_cache.md](10_arp_cache.md) | Updated |
| 11 | ARP | [11_arp.md](11_arp.md) | Updated |
| 12 | MAC Table | [12_mac_table.md](12_mac_table.md) | Updated |
| 13 | Switch | [13_switch.md](13_switch.md) | Updated |
| 14 | IPv4 | [14_ip.md](14_ip.md) | Updated |
| 15 | ICMP | [15_icmp.md](15_icmp.md) | Updated |
| 16 | UDP | [16_udp.md](16_udp.md) | Updated |
| 17 | TCP | [17_tcp.md](17_tcp.md) | Updated |
| 18 | Host | [18_host.md](18_host.md) | Updated |

## Phase 3 - Routing

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 19 | Route Table | [19_route_table.md](19_route_table.md) | Updated |
| 20 | Router | [20_router.md](20_router.md) | Updated |
| 21 | Static Route | [21_static_route.md](21_static_route.md) | Updated |
| 22 | RIP | [22_rip.md](22_rip.md) | Updated |
| 23 | OSPF | [23_ospf.md](23_ospf.md) | Updated |
| 24 | BGP | [24_bgp.md](24_bgp.md) | Updated |
| 25 | IS-IS | [25_isis.md](25_isis.md) | Updated |
| 26 | NAT / PAT | [26_nat.md](26_nat.md) | Updated |

## Phase 4 - Display Foundations

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 27 | Topology Display | [27_display_topology.md](27_display_topology.md) | Updated |
| 28 | Packet Header Display | [28_display_packet.md](28_display_packet.md) | Updated |
| 31 | Automatic Topology Layout | [31_topology_layout.md](31_topology_layout.md) | Ready for implementation |
| 32 | Trace Event Log | [32_event_log.md](32_event_log.md) | Ready for implementation |

## Phase 5 - Runnable Simulator And Animation

| # | Module | Spec | Spec Status |
| --- | --- | --- | --- |
| 29 | Topology Configuration | [29_topology_config.md](29_topology_config.md) | Ready for implementation |
| 33 | Terminal Animation | [33_animation.md](33_animation.md) | Ready for implementation |
| 34 | CLI Core | [34_cli.md](34_cli.md) | Ready for implementation |
| 35 | CLI Built-In Commands | [35_cli_commands.md](35_cli_commands.md) | Ready for implementation |

## How To Use These Files

Read `Concepts First` before coding. That section defines the vocabulary,
algorithm, and module boundaries.

Use `Function Behavior` as the implementation checklist.

Use `ACSL Contracts` as the header-contract target.

Use `KLEVA Verification Plan` as the generated-test checklist.

Use `Common Mistakes` as a review checklist before committing.

When code and spec disagree, do not guess. Either update the code to satisfy
the spec or update the spec to document the real behavior.
