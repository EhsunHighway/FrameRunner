# Networking Simulator — Architecture

## 1. Runtime Model

The simulator uses one deterministic discrete-event scheduler. Devices do not
run in operating-system threads and do not own separate event queues.

Many devices can appear active at the same simulated time because their events
can share a timestamp. The scheduler resolves that logical concurrency by
processing events in `(timestamp, sequence)` order. `sequence` is assigned when
an event is scheduled and makes equal-time behavior repeatable.

Periodic RIP and OSPF timers mean the event queue may never become empty.
Interactive runs are therefore bounded by one event, an event count, or a
simulated-time limit.

```text
topology file
     |
     v
TopologyConfig ----constructs atomically----> Simulator
                                               |
user command --> CLI --> bounded run --> Scheduler
                                               |
                                               v
                         Network / Protocol / Routing modules
                                               |
                                               v
                                            TraceLog
                                               |
                         +---------------------+------------------+
                         |                     |                  |
                  TopologyLayout        HeaderView          EventLog
                         \_____________________|_________________/
                                               |
                                          Animation
```

## 2. Ownership

The CLI owns the active `Simulator` and `AnimationState`. It borrows its input
and output streams.

The simulator owns:

- the topology and all devices reachable from it
- the scheduler and queued events
- the persistent trace log
- protocol state whose module specification assigns ownership to the simulator
  or to a simulator-owned device

The animation state borrows the active simulator, trace, topology, and output
stream. It owns only display state such as focus mode, playback rate, frame
cursor, and temporary active-packet visuals.

Trace records own copies of all information needed after an event or packet is
freed. They must not retain pointers to live packets, stack buffers, device
names, interfaces, protocol headers, or LSDB entries.

`load <file>` first builds a complete replacement simulator away from the
active one. Only after parsing, reference resolution, device construction,
link construction, route installation, and animation creation all succeed may
the CLI free the old objects and install the replacement.

## 3. Module Boundaries

### Configuration (`src/config/`)

`topology_config.c/h` owns text parsing, validation, reference resolution, and
construction order. It accepts network facts: devices, interfaces, links, and
static routes.

It does not:

- store the parsed file as the runtime topology
- add visual coordinates to configuration records
- render output
- partially modify the active simulator on failure

### Simulation Engine (`src/engine/`)

`event.c/h` represents scheduled work. `scheduler.c/h` orders and dispatches
that work. `simulator.c/h` owns the runtime objects and exposes step and bounded
run operations. `trace.c/h` stores persistent observations.

An event and a trace record are different objects:

- an event says what work must execute later
- a trace record says what was scheduled, started, finished, sent, received,
  selected, changed, delivered, or dropped

A single event can produce several semantic trace records while it traverses
device and protocol code.

### Network (`src/network/`)

Packet, interface, link, device, topology, host, switch, and router modules own
runtime network state. `DeviceType` identifies the concrete device category so
the display and configuration layers do not infer it from handler pointers or
struct layout.

Topology stores connectivity only. It does not parse files and does not store
display positions.

Packet identity has three roles:

- `id` identifies one allocated packet object
- `trace_id` groups every packet in one causal journey
- `parent_id` identifies the packet that caused a clone, reply, error, or
  control packet; zero means there is no parent

### Protocol And Routing Modules

Protocol and routing modules continue to perform real packet parsing and state
changes. Their animation responsibility is limited to emitting semantic trace
records at the exact points defined by their module specifications. They do not
print frames or control animation timing.

Examples of semantic actions are ARP resolution, MAC learning, IPv4 route
selection, TTL drop, ICMP reply generation, TCP state change, RIP update, OSPF
neighbor transition, SPF run, and route installation.

### Display (`src/display/`)

The display layer has four separate jobs:

| Module | Responsibility |
|---|---|
| `topology_view.c/h` | Print a static textual topology report |
| `topology_layout.c/h` | Calculate deterministic display-only node positions |
| `header_view.c/h` | Decode and print bounded packet snapshots from trace records |
| `event_log.c/h` | Print recent records or records selected by packet/trace ID |
| `animation.c/h` | Combine topology, layout, trace time intervals, focus, and terminal frames |

Automatic layout uses stable topology order and temporary display records. The
user does not provide positions, and network structs remain independent of
terminal dimensions.

### CLI (`src/cli/`)

`cli.c/h` owns only CLI state, registration, longest-prefix dispatch, the REPL,
and atomic replacement of the active simulator/animation pair.
`commands.c/h` separately owns the built-in command names and handlers. It
translates validated arguments into calls to topology configuration,
simulation, animation, display, network, routing, and protocol public APIs.

Neither module inspects protocol-private structs or mutates scheduler arrays
directly. Built-ins are registered after CLI-core creation, so the core remains
usable with a custom command set in tests or another frontend.

The CLI supports:

- atomic topology loading
- topology, interface, ARP, and route inspection
- event stepping and bounded runs
- animation ticks and playback speed
- focus by device, packet ID, or trace ID
- persistent packet and trace inspection

## 4. Trace And Animation Flow

For a link transmission, the sender emits a record whose `timestamp` is the
simulated departure time and whose `end_timestamp` is the scheduled arrival
time. The animation can therefore interpolate a packet between the two
automatically calculated endpoint positions.

```text
packet handed to link
        |
        +--> trace departure and scheduled arrival interval
        |
        +--> scheduler queues receive event
                       |
                       v
               destination receives
                       |
                       +--> protocol/device decision records
```

Several transmission intervals may overlap. `AnimationState` scans the
relevant trace interval at the current frame time and creates one temporary
visual for every matching in-flight packet; it never assumes only one packet
is moving.

Animation time and simulator time are distinct:

- simulator time changes only when scheduler events execute
- animation frame time moves through already recorded intervals according to
  playback speed

This separation permits pausing, focusing, and replaying recorded activity
without changing protocol state.

## 5. Dependency Direction

```text
packet -> event -> scheduler -> simulator
   |                    |           |
   +--------------------+---------> trace

interface -> link -> device -> topology
                    |         |
                    +-> host / switch / router

ethernet / arp / ip / icmp / udp / tcp
routing table / static route / rip / ospf
                    |
                    +---------------------> trace API

topology_config -> simulator + topology + device constructors + routes
topology_layout -> topology
topology_view   -> topology
header_view     -> trace snapshot
event_log       -> trace
animation       -> trace + topology_layout + display helpers
cli core        -> simulator + animation
cli commands    -> cli core + topology_config + simulator + animation
                   + display helpers + owning network/protocol APIs
```

Lower-level modules do not depend on CLI or animation. Core and protocol
modules may depend only on the narrow trace-emission API needed to describe an
action; they do not depend on display formatting.

## 6. End-to-End Runnable Path

The first complete runnable path is:

1. `main` creates an initial simulator and CLI.
2. The user loads a topology file.
3. The configuration module constructs a replacement simulator atomically.
4. The layout module calculates terminal positions from the new topology.
5. A user command injects traffic such as ping.
6. Scheduler events drive host, switch, router, protocol, and link behavior.
7. Those modules append persistent trace records.
8. Step, tick, or bounded-run commands render overlapping packet movement and
   the associated semantic decisions.
9. Focus commands let the user follow all traffic, one device, one packet
   object, or the complete causal trace.

BGP, IS-IS, NAT/PAT, a GUI, and per-device threads are not required for this
path.
