# Networking Simulator — Formal Specification

## 1. Purpose

An educational, event-driven CLI networking simulator written in C. It models
the bottom four OSI layers (Physical, Data Link, Network, Transport) using real
C data structures and an event queue, so every step shown in the terminal
reflects actual in-memory state.

---

## 2. Scope

### In Scope
| Area | Details |
|---|---|
| OSI Layers | 1 (Physical), 2 (Data Link), 3 (Network), 4 (Transport) |
| Protocols | Ethernet, ARP, IPv4, ICMP, TCP, UDP |
| Routing | Static, RIP (Phase 1), OSPF (Phase 2 module) |
| Devices | Host, Switch, Router |
| Simulation mode | Deterministic, event-driven, single-threaded loop |
| Concurrency | Logically concurrent device activity ordered by simulated time and event sequence |
| Interface | CLI with automatic ASCII topology layout, packet animation, trace log, and formatted header boxes |
| Extensibility | Protocol plugin interface for BGP, IS-IS, VLANs, etc. |

### Out of Scope (Phase 1)
- OSI Layers 5–7 (application logic)
- Real raw sockets or live network traffic
- Wireless / WiFi simulation
- GUI (ncurses is a stretch goal)
- BGP, IS-IS (Phase 2 modules)

---

## 3. Functional Requirements

### 3.1 Topology Management
- FR-01: User can define a topology (devices + links) via a config file or CLI commands.
- FR-02: Devices have: name, type (host/switch/router), interfaces with MAC and IP addresses.
- FR-03: Links have: bandwidth (Mbps), delay (ms), packet loss rate (%).
- FR-04: Topology is displayed as ASCII art using positions calculated by the display layer. The topology configuration does not contain visual coordinates.

### 3.2 Packet Simulation
- FR-05: User can trigger a packet send: `send <src> <dst> <protocol> [options]`.
- FR-06: At each OSI layer, the appropriate header is constructed as a C struct and prepended to the packet buffer.
- FR-07: Each encapsulation step creates a persistent semantic trace record and packet snapshot that can later be displayed as a formatted ASCII header box.
- FR-08: Physical layer transmits a byte buffer over a simulated link.
- FR-09: Receiving devices decapsulate headers layer by layer and record each validation, forwarding, delivery, or drop decision in the packet trace.

### 3.3 Event System
- FR-10: Scheduled work is represented by events. Observable protocol and device decisions are represented by persistent trace records associated with packet, trace, and parent-packet IDs where applicable.
- FR-11: Events are stored in one scheduler queue ordered by `(timestamp, sequence)` so equal-time behavior is deterministic.
- FR-12: The simulation loop processes one event at a time or runs for a bounded simulated duration or bounded event count. It must not assume the queue becomes empty because periodic protocol timers continually reschedule work.
- FR-13: User can advance one event (`step`), advance animation ticks (`tick`), run for a duration (`run time`), or run for an event count (`run events`).
- FR-14: User can stop the CLI loop or disable live animation without discarding the current simulator state.

### 3.4 Protocol Modules
- FR-15: ARP: resolve IP→MAC, maintain ARP table per device, send ARP request/reply events.
- FR-16: ICMP: ping (echo request/reply), TTL exceeded, destination unreachable.
- FR-17: IP: fragmentation awareness (display only in Phase 1), TTL decrement, checksum.
- FR-18: TCP: 3-way handshake simulation (SYN, SYN-ACK, ACK), sequence numbers.
- FR-19: UDP: stateless send, no handshake.
- FR-20: RIP: periodic routing update events, distance-vector table convergence display.

### 3.5 Routing
- FR-21: Each router maintains a routing table (prefix, next-hop, interface, metric).
- FR-22: Static routes can be configured in the topology config.
- FR-23: RIP updates routing tables via events; convergence is visualized step by step.
- FR-24: Router displays its routing decision (longest prefix match) for each packet forwarded.

### 3.6 Error Simulation
- FR-25: User can inject: packet drop, bit corruption (bad checksum), link down event.
- FR-26: Corrupted packets display a checksum error and are dropped at the receiver.
- FR-27: Link down triggers re-routing events (for dynamic routing protocols).

### 3.7 Visualization
- FR-28: Topology view: deterministic ASCII art of all devices and links using automatic display-only layout, updated when relevant state changes.
- FR-29: Packet journey view: animates every currently visible in-flight packet and shows each hop and protocol/device step for a selected packet or trace.
- FR-30: Header box view: formatted ASCII box for each protocol header with field names and values.
- FR-31: Trace log: persistent ordered records of scheduled events and semantic protocol/device actions, including simulated start and end timestamps where an action spans time.
- FR-32: Routing table view: display the routing table of any device on demand.
- FR-33: ARP table view: display the ARP cache of any device on demand.

### 3.8 CLI Commands
| Command | Description |
|---|---|
| `show topology` | Print ASCII topology |
| `send <src> <dst> <proto> [opts]` | Send a packet |
| `step` | Process next event |
| `run time <duration_us>` | Process events within a bounded simulated duration |
| `run events <count>` | Process at most the requested event count |
| `tick [n]` | Advance animation by a bounded number of wall-clock frames |
| `watch on\|off` | Enable or disable live animation output |
| `speed <factor>` | Change simulated-time-to-wall-time playback scale |
| `inject drop <device> <interface>` | Drop next packet on interface |
| `inject corrupt <device> <interface>` | Corrupt next packet |
| `inject linkdown <link>` | Bring a link down |
| `show route <device>` | Print routing table |
| `show arp <device>` | Print ARP table |
| `show packet <id>` | Show full packet journey |
| `focus all\|auto` | Select automatic or unfiltered animation focus |
| `focus device <name>` | Limit the view to activity involving one device |
| `focus packet <packet_id>` | Follow one packet object |
| `focus trace <trace_id>` | Follow an entire causal packet journey |
| `show trace <trace_id>` | Print the persistent trace for one journey |
| `load <file>` | Load topology from file |
| `help` | Print command help |
| `quit` | Exit simulator |

---

## 4. Non-Functional Requirements

- NFR-01: Written in C (C99 or C11), buildable with `gcc` and a `Makefile`.
- NFR-02: No external libraries required for Phase 1 (stdlib + POSIX only).
- NFR-03: Modular source layout — each protocol in its own `.c`/`.h` file.
- NFR-04: Adding a new protocol module must not require changes to core engine files.
- NFR-05: All header fields must use correct sizes (`uint8_t`, `uint16_t`, `uint32_t`).
- NFR-06: Packet buffers must use network byte order (big-endian) via `htons`/`htonl`.
- NFR-07: Memory: no leaks; all allocated packets/events must be freed after processing.
- NFR-08: Repeating the same topology and inputs produces the same equal-time
  event order and automatic layout.
- NFR-09: Trace records own bounded copies of display data and must not retain
  pointers to packets, stack objects, device names, or protocol structures that
  may be freed.
- NFR-10: The first runnable animation requires only the C standard library and
  POSIX terminal facilities; no GUI or external rendering library is required.

---

## 5. Protocol Header Reference

### Ethernet Frame (Layer 2)
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─────────────────────────────────────────────────────────────────┤
│                    Destination MAC (6 bytes)                     │
├─────────────────────────────────────────────────────────────────┤
│                      Source MAC (6 bytes)                        │
├─────────────────────────────────────────────────────────────────┤
│        EtherType (2 bytes)       │         Payload...           │
├─────────────────────────────────────────────────────────────────┤
│                      FCS/CRC (4 bytes)                          │
└─────────────────────────────────────────────────────────────────┘
```

### IPv4 Header (Layer 3)
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├───────┬───────┬───────────────────┬─────────────────────────────┤
│  Ver  │  IHL  │       ToS         │         Total Length        │
├───────┴───────┴───────────────────┼──┬──────────────────────────┤
│         Identification            │Fl│      Fragment Offset     │
├───────────────────────────────────┼──┴──────────────────────────┤
│       TTL       │    Protocol     │       Header Checksum       │
├─────────────────┴─────────────────┴─────────────────────────────┤
│                       Source IP Address                         │
├─────────────────────────────────────────────────────────────────┤
│                    Destination IP Address                        │
└─────────────────────────────────────────────────────────────────┘
```

### TCP Header (Layer 4)
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─────────────────────────────────┬───────────────────────────────┤
│          Source Port            │       Destination Port        │
├─────────────────────────────────┴───────────────────────────────┤
│                        Sequence Number                          │
├─────────────────────────────────────────────────────────────────┤
│                     Acknowledgment Number                       │
├────────┬────────────────────────┬───────────────────────────────┤
│ Offset │       Reserved/Flags   │            Window             │
├────────┴────────────────────────┼───────────────────────────────┤
│            Checksum             │         Urgent Pointer        │
└─────────────────────────────────┴───────────────────────────────┘
```

### UDP Header (Layer 4)
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─────────────────────────────────┬───────────────────────────────┤
│          Source Port            │       Destination Port        │
├─────────────────────────────────┼───────────────────────────────┤
│             Length              │           Checksum            │
└─────────────────────────────────┴───────────────────────────────┘
```

---

## 6. Data Structures (High Level)

```c
// Packet buffer — raw bytes + metadata
typedef struct Packet {
    uint8_t  *data;       // raw byte buffer (all headers + payload)
    size_t    len;        // current buffer length
    size_t    capacity;   // allocated capacity
    uint32_t  id;         // unique packet ID
    uint32_t  trace_id;   // causal journey shared by related packets
    uint32_t  parent_id;  // packet that caused this packet, or zero
    int       layer;      // current OSI layer (1-4)
} Packet;

// Event in the event queue
typedef struct Event {
    EventType    type;
    uint64_t     timestamp;   // simulated microseconds
    uint64_t     sequence;    // deterministic tie breaker
    void        *src_device;
    void        *dst_device;
    Packet      *packet;      // may be NULL for control events
    void        *data;        // protocol-specific payload
} Event;

// Network device
typedef struct Device {
    char         name[32];
    DeviceType   type;        // HOST, SWITCH, ROUTER
    Interface   *interfaces;  // array of interfaces
    int          iface_count;
    RouteTable  *route_table; // NULL for hosts/switches
    ARPTable    *arp_table;
} Device;
```

---

## 7. Phases

### Phase 1 — Core
- Single-threaded event loop
- Persistent semantic trace with packet lineage
- Ethernet, ARP, IPv4, ICMP, TCP, UDP
- Static routing
- Base packet, event, network, routing, and protocol modules

### Phase 2 — Protocols
- RIP dynamic routing
- OSPF module
- TCP 3-way handshake full simulation

### Phase 3 — Display Foundations
- Automatic topology layout
- Static topology and packet-header views
- Trace event-log rendering

### Phase 4 — Runnable Simulator
- Text topology configuration and atomic load
- Multi-packet terminal animation and bounded CLI controls

### Phase 5 — Extensions
- BGP module
- IS-IS and NAT/PAT modules
- VLAN support
- ncurses interactive UI
- Scenario save/export

---

## 8. File Layout (Planned)

```
networking_simulator/
├── Makefile
├── README.md
├── SPECIFICATION.md
├── ARCHITECTURE.md
├── src/
│   ├── main.c              # entry point, CLI loop
│   ├── engine/
│   │   ├── event.c/h       # event types, queue, dispatch
│   │   ├── simulator.c/h   # main simulation loop
│   │   ├── scheduler.c/h   # timestamp and sequence ordering
│   │   └── trace.c/h       # persistent semantic simulation trace
│   ├── config/
│   │   └── topology_config.c/h # text parsing and atomic construction
│   ├── network/
│   │   ├── device.c/h      # device struct, init, dispatch
│   │   ├── interface.c/h   # network interface management
│   │   ├── link.c/h        # link simulation (delay, loss)
│   │   ├── packet.c/h      # packet buffer management
│   │   └── topology.c/h    # topology storage and lookup
│   ├── protocols/
│   │   ├── ethernet.c/h    # Layer 2: Ethernet framing
│   │   ├── arp.c/h         # ARP request/reply, ARP table
│   │   ├── ip.c/h          # IPv4 header, checksum, TTL
│   │   ├── icmp.c/h        # ICMP echo, unreachable, TTL exceeded
│   │   ├── tcp.c/h         # TCP header, 3-way handshake
│   │   ├── udp.c/h         # UDP header
│   │   └── rip.c/h         # RIP routing updates
│   ├── routing/
│   │   ├── route_table.c/h # routing table CRUD, LPM
│   │   └── static_route.c/h
│   ├── cli/
│   │   ├── cli.c/h         # command parser and REPL
│   │   └── commands.c/h    # command implementations
│   └── display/
│       ├── topology_view.c/h  # ASCII topology renderer
│       ├── topology_layout.c/h # deterministic automatic layout
│       ├── header_view.c/h    # protocol header box renderer
│       ├── event_log.c/h      # trace history display
│       └── animation.c/h      # multi-packet terminal animation
└── tests/
    ├── test_packet.c
    ├── test_arp.c
    ├── test_routing.c
    └── test_event.c
```
