# Module 13 — Switch

**Files:** `src/network/switch.c`, `src/network/switch.h`
**Status:** ✅ Implemented
**Depends on:** mac_table, interface, device, ethernet, scheduler, simulator

---

## The Problem

A Layer-2 switch is a multi-port device that **forwards Ethernet frames
based on learned MAC addresses**. Unlike a hub (blind flood), a switch
inspects the source MAC on arrival and the destination MAC for
forwarding. It:

1. Owns a `MacTable` for learned mappings.
2. Has multiple `Interface *` ports (up to `SW_MAX_PORTS`).
3. Registers a receive handler and hooks into the scheduler.
4. Runs a periodic aging event to evict stale MAC entries.

`Switch` is a **thin specialization of `Device`** — it reuses device's
interface array and back-pointers, adding only the MAC table and
switch-specific logic.

## Mental Model

```
   Switch "SW1"
   ┌────────────────────────────────────────────────────────┐
   │  Device base (name, interfaces[8])   │
   │                                                        │
   │  mac_table  (embedded, 32 KB)                          │
   │    AA:BB:CC:11:22:33 → g0/0   (ts=410 000)             │
   │    DD:EE:FF:44:55:66 → g0/1   (ts=415 000)             │
   │                                                        │
   │  port_count = 4                                        │
   │  interfaces[0] → g0/0 ─────────────── H1               │
   │  interfaces[1] → g0/1 ─────────────── H2               │
   │  interfaces[2] → g0/2 ─────────────── R1               │
   │  interfaces[3] → g0/3 (up=0, down)                     │
   └────────────────────────────────────────────────────────┘
```
Frame arrives on in_port
        │
        ▼
1. Sanity checks (port up? frame has Ethernet header?)
        │
        ▼
2. Parse Ethernet header → src_mac, dst_mac
        │
        ▼
3. LEARN: record src_mac → in_port in MAC table
        │
        ▼
4. FORWARD DECISION on dst_mac:
        │
        ├── dst_mac is broadcast (FF:FF:FF:FF:FF:FF)?  ──► FLOOD
        │
        ├── mac_table_lookup(dst_mac) == NULL?          ──► FLOOD (unknown unicast)
        │
        └── mac_table_lookup(dst_mac) == egress?
                ├── egress == in_port?  ──► DROP (loop — src and dst on same port)
                ├── egress is down?     ──► DROP
                └── otherwise           ──► FORWARD to egress

FLOOD = send a clone to every port except in_port that is up and has a link
FORWARD = send original frame to egress port

---

## Header File — `switch.h`

### Constants

| Macro              | Value | Use                           |
|--------------------|-------|-------------------------------|
| `SW_MAX_PORTS`     | `48`  | Interface array fixed capacity |
| `SW_AGE_INTERVAL`  | `30000` | Aging timer interval (ms)   |

### `Switch` Struct (≈ 33 KB)

```c
typedef struct Switch {
    Device   base;                    // MUST be first (name, interfaces)
    MacTable mac_tbl;                 // 32 776 B embedded
    int      port_count;              // live ports
    Simulator *sim;                   // for scheduling aging events
} Switch;
```

**Why `Device base` must be first:** `switch_add_port` calls
`device_add_interface((Device *)sw, iface)` — the cast is valid only
if `base` is at offset 0.

### Public API

| Function                            | Purpose                                        |
|-------------------------------------|------------------------------------------------|
| `switch_create(name, sim)`          | Alloc, init `base` Device, init `mac_tbl`, schedule first age event. |
| `switch_free`                       | Free `base` device (cascades to interfaces), then `Switch` itself. |
| `switch_add_port(sw, iface)`        | `device_add_interface` + register rx_handler on iface. |
| `switch_receive(sw, in_port, frame, ethertype)` | Learn src, lookup dst, forward or flood. |
| `switch_port_down(sw, port)`        | `interface_set_up(port, 0)`, flush MAC table entries for that port. |
| `switch_get_port_by_name`           | Thin wrapper over `device_get_interface_by_name`. |

---

## Function Implementation Guide

This section is the practical implementation reference for
`src/network/switch.c`.

### Static helper: `mac_age_handler(e, ctx)`

Purpose: periodically remove stale entries from the switch MAC table.

Implementation:

1. Ignore `e`; the handler only needs the switch context.
2. Cast `ctx` to `Switch *`.
3. If the switch, simulator, or scheduler is NULL, return immediately.
4. Call `mac_table_age(&sw->mac_tbl, simulator_now(sw->sim))`.
5. Create a new `EVT_MAC_AGE` event scheduled at
   `simulator_now(sw->sim) + SW_AGE_INTERVAL`.
6. Store `sw` in the event's `data` field, then call
   `scheduler_schedule(sw->sim->sched, event)`.
7. If `event_create` returns NULL, do not schedule anything.

The handler self-reschedules, so `switch_create` only needs to schedule
the first aging event.

### Static helper: `switch_rx_shim(iface, pkt, ethertype, ctx)`

Purpose: adapt the generic `RxHandler` signature used by `Interface`
to the switch-specific receive function.

Implementation:

1. Cast `ctx` to `Switch *`.
2. If `sw`, `iface`, or `pkt` is NULL, return.
3. Call `switch_receive(sw, iface, pkt, ethertype)`.

This function is registered on each switch port by `switch_add_port`.

### `switch_create(const char *name, Simulator *sim)`

Purpose: allocate a switch, initialize its embedded `Device` base,
initialize the MAC table, remember the simulator, and start MAC aging.

Return value:

- Return `NULL` if `name` or `sim` is NULL.
- Return `NULL` if any allocation fails.
- Return the initialized `Switch *` on success.

Implementation:

1. Allocate `sizeof(Switch)` with `malloc`.
2. Zero the whole struct with `memset`.
3. Initialize `sw->base` as a real `Device` with capacity
   `SW_MAX_PORTS`.
   - Because `Device base` is embedded, do not store a separate
     `Device *` inside the switch.
   - One acceptable pattern is:
     1. call `device_create(name, SW_MAX_PORTS)`;
     2. copy the returned `Device` struct into `sw->base`;
     3. free only the temporary `Device` wrapper, not its
        `interfaces` array.
   - Another acceptable pattern is to initialize `sw->base.name`,
     allocate `sw->base.interfaces`, and set `iface_count`/`iface_max`
     manually to match `device_create`.
4. Set `sw->port_count = 0`.
5. Set `sw->sim = sim`.
6. Call `mac_table_init(&sw->mac_tbl)`.
7. Create the first MAC aging event with `event_create_callback`, using
   `mac_age_handler` and `sw` as the per-event context.
8. Schedule that event on `sim->sched`. If event allocation or scheduling
   fails, release the partially created switch and return `NULL`.

Important ownership detail: after `sw->base` is initialized,
`sw->base.interfaces` belongs to the switch and must be released by
`switch_free`.

### `switch_free(Switch *sw)`

Purpose: release a switch and all interfaces owned by its embedded
device base.

Implementation:

1. If `sw` is NULL, return.
2. Free the embedded base as a `Device`.

Because `base` is the first field in `Switch`, `(Device *)sw` points to
the same address as `&sw->base`. Calling `device_free((Device *)sw)` is
valid because `device_free` frees the interface array, each owned
interface, and finally the pointer passed to it. Do not call
`free(sw)` again afterward.

### `switch_add_port(Switch *sw, Interface *iface)`

Purpose: attach an interface to the switch and make received Ethernet
frames enter `switch_receive`.

Return value:

- Return `-1` if `sw` or `iface` is NULL.
- Return `-1` if `device_add_interface` fails, usually because the
  switch is full.
- Return `0` on success.

Implementation:

1. Validate arguments.
2. Call `device_add_interface((Device *)sw, iface)`.
3. If that succeeds, call
   `interface_set_rx_handler(iface, switch_rx_shim, sw)`.
4. Set the port administratively up with `interface_set_up(iface, 1)`,
   unless the project intentionally leaves ports down until tests or
   topology setup enable them.
5. Update `sw->port_count` from `sw->base.iface_count`.

`device_add_interface` sets `iface->device = (Device *)sw`, so later
code can walk from the interface back to the owning switch/device.

### `switch_receive(Switch *sw, Interface *in_port, Packet *frame, uint16_t ethertype)`

Purpose: learn the source MAC of an arriving Ethernet payload, then
forward or flood it based on the destination MAC.

Important input expectation: `ethernet_receive` has already stripped the
Ethernet header before calling the port RX handler. With the current
`Packet` layout, stripping advances `frame->data` but does not destroy
the bytes before it. The original Ethernet header is still readable at
`frame->data - ETH_HDR_LEN` as long as that address is not before
`frame->head`.

After `ethernet_receive` succeeds:

```
frame->head
   │
   ▼
[ headroom ... ][ EthernetHeader ][ L3 payload ... ]
                              ▲    ▲
                              │    └─ frame->data
                              └────── frame->data - ETH_HDR_LEN
```

The intended switch behavior is:

1. Learn `src_mac` on `in_port`.
2. Look up `dst_mac`.
3. If `dst_mac` is known and points to a different up port, send once.
4. If `dst_mac` is unknown, broadcast, or maps to a down port, flood to
   all up ports except `in_port`.
5. If `dst_mac` maps back to `in_port`, drop the packet.

Implementation:

1. If `sw`, `in_port`, or `frame` is NULL, return.
2. If `in_port` is down, free or drop the packet according to the
   packet ownership convention used by the caller, then return.
3. Recover `src_mac` and `dst_mac` from the stripped Ethernet header.
   Since `ethernet_receive` already called `packet_strip(frame,
   ETH_HDR_LEN)`, the header starts at `frame->data - ETH_HDR_LEN`, not
   at `frame->data`.
   - First verify that `frame->data >= frame->head + ETH_HDR_LEN`.
   - Then use
     `EthernetHeader *hdr = (EthernetHeader *)(frame->data - ETH_HDR_LEN)`.
   - Copy `hdr->src_mac` and `hdr->dst_mac` into local `uint8_t[6]`
     arrays before forwarding. Do not keep the `hdr` pointer around
     after calling `ethernet_send`, because `ethernet_send` may prepend
     a new Ethernet header and overwrite that headroom.
4. Let `now = simulator_now(sw->sim)`.
5. Call `mac_table_learn(&sw->mac_tbl, src_mac, in_port, now)`.
   The current code drops when learning fails. A more switch-like policy
   is to keep forwarding even if the table is full; choose one behavior
   and test it.
6. If `dst_mac` is all `FF:FF:FF:FF:FF:FF`, flood.
7. Otherwise call `mac_table_lookup(&sw->mac_tbl, dst_mac)`.
8. If lookup returns `in_port`, drop the packet. The destination is
   already on the same segment as the source.
9. If lookup returns a non-NULL up egress port, call
   `ethernet_send(sw->sim, egress, dst_mac, ethertype, frame)` exactly
   once. `ethernet_send`/`link_transmit` takes ownership of `frame`.
10. Otherwise flood:
    - Iterate `sw->base.interfaces[0 .. sw->base.iface_count - 1]`.
    - Skip NULL ports.
    - Skip `in_port`.
    - Skip ports where `interface_is_up(port)` is false.
    - For each selected port, send a packet clone with
      `packet_clone(frame)`.
    - If cloning fails for a port, skip that port and continue.
    - After all clones are sent, free the original `frame` because the
      flood path sends clones, not the original.

Flood uses clones so each outgoing link owns a distinct `Packet *`.

### `switch_port_down(Switch *sw, Interface *port)`

Purpose: administratively disable a port and remove learned MAC entries
that point to it.

Implementation:

1. If `sw` or `port` is NULL, return.
2. Call `interface_set_up(port, 0)`.
3. Call `mac_table_flush_port(&sw->mac_tbl, port)`.

This prevents future forwarding decisions from using a disabled egress.

### `switch_get_port_by_name(Switch *sw, const char *name)`

Purpose: find a switch port by interface name.

Implementation:

1. If `sw` or `name` is NULL, return NULL.
2. Return `device_get_interface_by_name((Device *)sw, name)`.

## ACSL Contract Plan

The contracts should focus on ownership, stripped-header safety, and
forward/flood effects:

- `switch_create`: success initializes embedded `Device`, zeroes MAC
  table, stores `sim`, and registers the age handler.
- `switch_add_port`: success increments `base.iface_count`, sets
  `iface->device`, stores `switch_rx_shim`, stores `sw` as handler
  context, sets port up, and syncs `port_count`.
- `switch_receive`: null inputs are no-ops; down input port consumes the
  packet and does not learn; missing stripped Ethernet header increments
  `rx_errors`; valid frames learn source MAC before forwarding decision.
- Flood path sends only clones and frees the original after all clone
  attempts.
- Known-unicast path sends the original exactly once when egress is up
  and not the input port; otherwise it drops/frees the original.
- `switch_port_down`: sets `port->up == 0` and leaves no valid MAC table
  entries referencing that port.
2. Return `device_get_interface_by_name((Device *)sw, name)`.

### ACSL Highlights

```
switch_receive (forward):
  ∃ egress: mac_table_lookup(sw->mac_tbl, dst_mac) == egress
            && egress != in_port && egress->up == 1
  ⇒ ethernet_send called exactly once (to egress)

switch_receive (flood):
  mac_table_lookup(sw->mac_tbl, dst_mac) == NULL
  ⇒ ethernet_send called for every port p where p != in_port && p->up == 1
```

---

## Timer Dispatch

```
switch_create("SW1", sim)
  └─► event_create_callback(EVT_MAC_AGE,
                            now + SW_AGE_INTERVAL,
                            NULL, NULL, NULL, NULL,
                            mac_age_handler,
                            sw)
        └─► scheduler_schedule(...)
```

Switch MAC aging is not a global `EVT_MAC_AGE` fallback registration.
Each aging event carries the specific `Switch *` it belongs to. This lets
multiple switches age independently without overwriting one shared scheduler
handler slot.

---

## Function Call Sequence — Creating a switch

```
sim  = simulator_create(topo, sched)
sw   = switch_create("SW1", sim)
         │
         ├─► device_create("SW1", SW_MAX_PORTS)   ← base.interfaces[]
         ├─► mac_table_init(&sw->mac_tbl)
         ├─► event_create_callback(EVT_MAC_AGE,
         │                         now + SW_AGE_INTERVAL,
         │                         NULL, NULL, NULL, NULL,
         │                         mac_age_handler, sw)
         └─► scheduler_schedule(sim->sched, e)    ← first age tick

g0_0 = interface_create("g0/0", mac0, 0, 0, 1500)
switch_add_port(sw, g0_0)
   └─► device_add_interface((Device*)sw, g0_0)    ← sets g0_0->device = (Device*)sw
   └─► interface_set_rx_handler(g0_0, switch_rx_shim, sw)

topology_add_device(topo, (Device*)sw)
```

## Function Call Sequence — Frame forward path

```
[scheduler fires EVT_PACKET_RECEIVE event carrying ethernet_receive_event]
   ▼
ethernet_receive_event(e, sim):
   └─► ethernet_receive(in_port, frame, &ethertype)
           └─► packet_strip(frame, ETH_HDR_LEN)
               frame->data now points at L3 payload
               old EthernetHeader remains at frame->data - ETH_HDR_LEN

switch_rx_shim(in_port, frame, ethertype, sw):
   └─► switch_receive(sw, in_port, frame, ethertype)
           │
           ├─► hdr = (EthernetHeader *)(frame->data - ETH_HDR_LEN)
           ├─► copy hdr->src_mac and hdr->dst_mac
           ├─► mac_table_learn(&sw->mac_tbl, src_mac, in_port, now)
           │
           ├─► egress = mac_table_lookup(&sw->mac_tbl, dst_mac)
           │
           ├─── UNICAST (egress != NULL && egress != in_port):
           │       └─► ethernet_send(sw->sim, egress, dst_mac, ethertype, frame)
           │
           └─── FLOOD (egress == NULL):
                   for each port i in base.interfaces[] where port_i != in_port
                                                             && port_i->up:
                       copy = packet_clone(frame)
                       └─► ethernet_send(sw->sim, port_i, dst_mac, ethertype, copy)
```
### `mac_age_handler` (internal, static)

| Parameter | Type            | Description                          |
|-----------|-----------------|--------------------------------------|
| `e`       | `const Event *` | Triggering event (unused)            |
| `ctx`     | `void *`        | Cast to `Switch *`; owns `mac_tbl`   |

Stored inside each `EVT_MAC_AGE` event by `event_create_callback`.
Self-reschedules every `SW_AGE_INTERVAL` ms by creating another callback
event with the same `Switch *` context. Must not be called directly.

ACSL:
  requires ctx != \null && \valid((Switch *)ctx);
  assigns ((Switch *)ctx)->mac_tbl.entries[0..1023],
          ((Switch *)ctx)->mac_tbl.count;

### ACSL Highlights

```
mac_table_learn:
  result == NULL ⇒ table is full or args are null
  result != NULL ⇒ result->port == port
                && ∀ i ∈ [0,6): result->mac[i] == mac[i]
                && result->timestamp == now
                && result->valid == 1
```

---

## Design Notes

- **`Device base` at offset 0** allows safe `(Device *)sw` casts for
  functions that only need device-level access (topology, display).
- **Flood gives each egress its own packet** — `ethernet_send` calls
  `link_transmit`, which takes ownership of the packet it receives. The
  simplest implementation sends `packet_clone(frame)` on each flooded
  port, then frees the original frame after the loop.
- **Stripped Ethernet headers remain readable.** `Packet` has a stable
  `head` pointer and a moving `data` pointer. `packet_strip` advances
  `data`; it does not erase the old header bytes. After
  `ethernet_receive`, the switch can read the previous Ethernet header
  at `frame->data - ETH_HDR_LEN`, then copy the MAC addresses before
  any later `ethernet_send` prepends a new header.
- **STP (spanning tree) is out of scope** for this milestone. Loops in
  the topology will cause infinite floods.
- **`SW_MAX_PORTS` 48** mirrors a real access switch; the embedded
  `Device` array costs 48 × 8 = 384 bytes (pointers) plus the devices.

## Test Plan (kleva)

- `receive_learns_src`, `receive_unicast_forward`, `receive_flood`
- `flood_skips_ingress_port`, `flood_skips_down_port`
- `port_down_flushes_mac`, `aging_event_reschedules`
- NULL guards: `receive_null_sw`, `receive_null_frame`
