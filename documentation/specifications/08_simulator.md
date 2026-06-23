# Module 08 — Simulator

**Files:** `src/engine/simulator.c`, `src/engine/simulator.h`
**Status:** ✅ Implemented (96% / 81%)
**Depends on:** topology, scheduler

---

## The Problem

We have `Topology` (what exists) and `Scheduler` (when things happen).
The user wants **one** object that:

1. Binds the two together so a handler can reach both from a single
   pointer.
2. Owns both lifetimes — `simulator_free` releases everything.
3. Provides convenient entry points: `inject_packet`, `set_end_time`,
   `register_handler` — without forcing the caller to dig into the
   scheduler's internals.

That object is `Simulator`. It is the **top-level façade** every
protocol module receives via `void *ctx`.

## Mental Model

```
   ┌────────────────────────────────────────────────────────┐
   │                       Simulator                        │
   │   topo  ───► Topology       (devices + links)          │
   │   sched ───► Scheduler      (event queue + handlers)   │
   │   end_time = 60_000_000 µs  (0 means "no limit")       │
   └────────────────────────────────────────────────────────┘
```

---

## Header File — `simulator.h`

### Struct

```c
typedef struct Simulator {
    Topology  *topo;
    Scheduler *sched;
    uint64_t   end_time;   // stop when sched->now >= end_time; 0 = no limit
} Simulator;
```

### Public API

| Function                                | Purpose                          |
|-----------------------------------------|----------------------------------|
| `simulator_create(topo, sched)`         | Bind both, set `end_time = 0`.   |
| `simulator_free`                        | Frees topology, scheduler, self. |
| `simulator_run`                         | Drive sched until empty/stop/end_time. |
| `simulator_step`                        | Single event step.               |
| `simulator_stop`                        | Set running = 0.                 |
| `simulator_set_end_time(end_us)`        | Bound the simulation.            |
| `simulator_now`                         | `sched->now` accessor.           |
| `simulator_inject_packet(src, dst, pkt, delay)` | Convenience: schedule a packet delivery. |
| `simulator_register_handler(type, fn, ctx)`  | Thin pass-through to `scheduler_register`. |

### ACSL highlight (`simulator_run`)

```
on exit: sched->eq->count == 0
       OR sched->running == 0
       OR (end_time > 0 AND sched->now >= end_time)
```

---

## Call Sequence — Bringing the simulator up

```
topo  = topology_create()           ← module #7
sched = scheduler_create(1024)      ← module #3
sim   = simulator_create(topo, sched)

Ethernet has no init hook; link receive events carry ethernet_receive_event.
arp_init(sim)         ─► scheduler_register(EVT_ARP_REQUEST, …)
                      ─► scheduler_register(EVT_ARP_REPLY,   …)
ip_init(sim, stack)   ─► bind interfaces to one IP stack
...

simulator_set_end_time(sim, 10 * 1000 * 1000)   ← 10 seconds
simulator_inject_packet(sim, h1, h2, pkt, 0)
simulator_run(sim)
```

## Call Sequence — `simulator_inject_packet`

```
simulator_inject_packet(sim, src_dev, dst_dev, pkt, delay):
   │
   │   current implementation stores Device* src/dst directly
   │
   ├─► event_create(EVT_PACKET_RECEIVE,
   │                sim->sched->now + delay,
   │                src_dev, dst_dev, pkt, NULL)
   │
   └─► scheduler_schedule(sim->sched, e)
```

## Call Sequence — `simulator_run`

```
while sched->running && eq->count > 0:
    if sim->end_time > 0 && sched->now >= sim->end_time:
        break
    scheduler_step(sched)          ← see module #3
```

---

## Design Notes

- **`Simulator *sim` is the de-facto `self`** in every protocol module.
  For fallback protocol handlers, pass it as `ctx` to
  `scheduler_register`. For owner-specific timers or deliveries, store the
  correct owner context directly in the event with `event_create_callback`.
- **Ownership chain:** `simulator_free` → `topology_free` → every
  `device_free` and `link_free` → every `interface_free` and
  `packet_free` on owned packets. One root call cleans up everything.
- **`end_time` is in µs** (matches `scheduler->now`); `0` means run
  forever (until the queue empties).
- **`simulator_inject_packet` is the test harness's friend** — it makes
  most unit and integration tests fit in one line.

## Implementation Guide

1. `simulator_create`: validate non-NULL topology and scheduler; allocate
   and store both borrowed-at-create pointers as owned fields; set
   `end_time = 0`.
2. `simulator_free`: free topology, scheduler, and simulator. Pending
   events are handled by scheduler teardown.
3. `simulator_step`: reject NULL simulator; otherwise delegate to
   `scheduler_step`.
4. `simulator_run`: keep stepping while work remains, unless
   `end_time > 0` and current scheduler time has reached it. The
   scheduler owns handler dispatch.
5. `simulator_inject_packet`: create an event at
   `simulator_now(sim) + delay_us`, with `src` and `dst` stored exactly
   as provided. This helper does not resolve interfaces or install a
   packet receive callback by itself.
6. `simulator_register_handler`: thin wrapper around
   `scheduler_register`.

## ACSL Contract Plan

- `simulator_create`: success stores exact `topo` and `sched` pointers
  and initializes `end_time`.
- `simulator_run`: exit condition is queue empty, scheduler stopped, or
  end time reached.
- `simulator_inject_packet`: success increments scheduler queue count by
  one; failure leaves queue count unchanged.
- `simulator_now`: NULL returns `0`; valid returns `sim->sched->now`.
