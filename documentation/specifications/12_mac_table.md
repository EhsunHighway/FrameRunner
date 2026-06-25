# Module 12 — MAC Table

**Files:** `src/network/mac_table.c`, `src/network/mac_table.h`
**Status:** ✅ Implemented
**Depends on:** interface

---

## The Problem

A switch learns *which port a MAC address lives on* by observing the
source MAC of every arriving frame. That knowledge lives in the MAC
Table (a.k.a. CAM table / forwarding database). The switch consults it
on every received frame to choose between:

- **Unicast forward** — exactly one known outgoing port.
- **Flood** — unknown destination; copy to every port except the one
  the frame arrived on.

Entries must expire to handle host moves. This module is **pure data
structure** — no events, no I/O.

## Mental Model

```
   MacTable (1 per Switch)
   ┌───────────────────────────────────────────────────────┐
   │  capacity = MAC_TABLE_SIZE (1024)                     │
   │  count    = 3  (entries in use)                       │
   │  age_ms   = MAC_AGE_MS     (300 000 ms = 5 min)       │
   │                                                       │
   │  entries[0]: AA:BB:CC:11:22:33 → port=eth0 ts=400 000 │
   │  entries[1]: DD:EE:FF:44:55:66 → port=eth1 ts=410 000 │
   │  entries[2]: 11:22:33:AA:BB:CC → port=eth2 ts=380 000 │
   │  entries[3..1023]: valid=0                            │
   └───────────────────────────────────────────────────────┘
```

---

## Header File — `mac_table.h`

### Constants

| Macro              | Value       | Use                              |
|--------------------|-------------|----------------------------------|
| `MAC_TABLE_SIZE`   | `1024`      | Fixed capacity (ACSL: literal)   |
| `MAC_AGE_MS`       | `300000`    | Entry lifetime in ms             |
| `MAC_ADDR_LEN`     | `6`         | Bytes per MAC                    |

### `MacEntry` Struct (32 bytes)

```c
typedef struct MacEntry {
    uint8_t    mac[6];      // 6 B — learned source MAC
    uint8_t    _pad[2];     // 2 B — align next field to 8
    Interface *port;        // 8 B — egress NIC (borrowed, not owned)
    uint64_t   timestamp;   // 8 B — time of last learn (ms)
    int        valid;       // 4 B
    int        _pad2;       // 4 B — align struct to 8
} MacEntry;                 // total: 32 bytes
```

### `MacTable` Struct (32 776 bytes — embedded in Switch)

```c
typedef struct MacTable {
    MacEntry entries[1024]; // 32 768 B (1024 × 32)
    int      count;         //      4 B — valid entries in use
    int      _pad;          //      4 B
} MacTable;
```

`MacTable` is embedded directly in `Switch` — no malloc, no NULL guard.

### Public API

| Function                              | Purpose                                          |
|---------------------------------------|--------------------------------------------------|
| `mac_table_init(tbl)`                 | Zero-fill; count = 0.                            |
| `mac_table_learn(tbl, mac, port, now)`| Insert new entry or refresh ts if already known. |
| `mac_table_lookup(tbl, mac)`          | Return `Interface *` or NULL.                    |
| `mac_table_age(tbl, now)`             | Invalidate entries where `now - ts > MAC_AGE_MS`.|
| `mac_table_flush_port(tbl, port)`     | Remove all entries pointing at `port`.           |


mac_table_lookup (hit):
  result != NULL ⇒ ∃ i: entries[i].valid && memcmp(entries[i].mac, mac, 6) == 0

mac_table_lookup (miss):
  result == NULL ⇒ ∀ i: !entries[i].valid || memcmp(entries[i].mac, mac, 6) != 0

mac_table_age postcondition:
  ∀ i: entries[i].valid ⇒ (now - entries[i].timestamp) < 300000
```

---

## Timer Event — Aging event

```
event_create_callback(EVT_MAC_AGE,
                      now + SW_AGE_INTERVAL,
                      NULL, NULL, NULL, NULL,
                      mac_age_handler,
                      sw)
```

`mac_age_handler` is stored on each aging event with that event's
`Switch *` context. It calls `mac_table_age` then reschedules itself
`SW_AGE_INTERVAL` ms into the future with another callback event.

---

## Function Call Sequence — Frame arrives at switch port P

```
ethernet_receive_event(e, sim):
   └─► ethernet_receive(iface_P, frame, &ethertype)
           │   (strip L2; MAC check is already done vs iface_P->mac)
           ▼
       switch_receive(sw, iface_P, frame, ethertype):
           │
           ├─► mac_table_learn(sw->mac_tbl, src_mac, iface_P, now)
           │       for i in 0..count: if mac[i] matches → update ts, return 0
           │       else: entries[count++] = {src_mac, iface_P, now, valid=1}
           │
           ├─► egress = mac_table_lookup(sw->mac_tbl, dst_mac)
           │       for i in 0..count: if valid && mac[i] matches → return port
           │       else: return NULL
           │
           ├─ if egress && egress != iface_P:
           │       └─► ethernet_send(sim, egress, dst_mac, ethertype, frame)
           │               └─► link_transmit(...)
           │
           └─ else (flood):
                   for each port in sw->interfaces except iface_P:
                       packet_copy(frame)
                       └─► ethernet_send(sim, port, dst_mac, ethertype, copy)
```

## Function Call Sequence — Aging

```
mac_age_handler(e, ctx):          (fires every 30 000 ms)
   ├─► mac_table_age(sw->mac_tbl, sched->now)
   │       for i in 0..capacity:
   │           if entries[i].valid && (now - entries[i].timestamp) > 300000:
   │               entries[i].valid = 0
   │               count--
   │
   └─► event_create_callback(EVT_MAC_AGE,
                             now + SW_AGE_INTERVAL,
                             NULL, NULL, NULL, NULL,
                             mac_age_handler, sw)
       scheduler_schedule(sched, e)        ← reschedule itself
```

---

## Design Notes

- **Fixed array, no realloc.** 1024 entries × 32 bytes = 32 KB — fits
  in L1/L2 on any modern CPU. Full-table linear scan is ~1 µs.
- **Entries never shrink the array** — they are just invalidated. The
  `count` field tracks valid entries.
- **`port` is a borrowed pointer.** `mac_table_flush_port` is called by
  `switch_port_down` before an interface is freed.
- **ACSL literal 1024, not `MAC_TABLE_SIZE`** — EVA cannot evaluate
  C macros inside annotations (same lesson as ARP cache).
- **Aging is eventually consistent** — in the worst case an entry lives
  up to `MAC_AGE_MS + 30_000 ms` before being swept.

## Test Plan (kleva)

- `learn_new`, `learn_update_port`, `learn_table_full`
- `lookup_hit`, `lookup_miss`, `lookup_after_age_expires`
- `age_keeps_fresh_entry`, `age_removes_stale`
- `flush_port_removes_entries`, `flush_port_leaves_others`
- NULL guards: `learn_null_tbl`, `lookup_null_mac`, `age_null_tbl`

## Implementation Guide

1. `mac_table_init`: `memset` the whole table to zero. `count` must be
   zero and every entry invalid.
2. `mac_table_learn`: reject NULLs; scan the full fixed table for an
   existing valid MAC and refresh `port`/`timestamp` without changing
   count; otherwise fill the first invalid slot, mark valid, increment
   count; return NULL only when invalid input or full table.
3. `mac_table_lookup`: reject NULLs; scan all entries; return the
   borrowed `Interface *` for the first valid MAC match.
4. `mac_table_age`: invalidate entries where `now - timestamp >
   MAC_AGE_MS`; decrement count for each invalidated entry.
5. `mac_table_flush_port`: invalidate all entries pointing at the given
   port and decrement count for each.

Implementation traps:

- Do not scan only `0 .. count - 1`; aging and flush leave holes.
- Count must never go below zero. Decrement only when changing an entry
  from valid to invalid.
- `port` is borrowed. The table never frees interfaces.

## ACSL Contract Plan

Useful predicates:

```c
/*@ predicate mac_eq{L}(uint8_t *a, uint8_t *b) =
      \forall integer i; 0 <= i < 6 ==> a[i] == b[i];

    predicate mac_table_count_ok{L}(MacTable *table) =
      table->count ==
        \numof(integer i; 0 <= i < 1024 && table->entries[i].valid == 1);
*/
```

Contract targets:

- `mac_table_learn`: update behavior preserves count; insert behavior
  increments count; full behavior returns NULL and leaves table state
  unchanged.
- `mac_table_lookup`: hit result equals the port of a valid matching
  entry; miss proves no valid matching entry exists.
- `mac_table_age`: after return, every valid entry is fresh enough.
- `mac_table_flush_port`: after return, no valid entry references the
  flushed port and unrelated entries preserve validity.
