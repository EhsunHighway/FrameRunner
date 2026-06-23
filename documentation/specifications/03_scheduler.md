# Module 03 — Scheduler

**Files:** `src/engine/scheduler.c`, `src/engine/scheduler.h`
**Status:** ✅ Implemented (95% / 84%)
**Depends on:** event

---

## The Problem

The event queue (module #2) stores what to do and when. The **scheduler**
adds the missing pieces:

1. A **dispatch rule** for executing the event's callback when one is
   stored directly in the event.
2. A **fallback dispatch table** keyed by `EventType` for simple events
   that do not carry their own callback.
3. The **virtual clock** (`now`) that advances to whatever timestamp the
   next event carries.
4. A **run loop** (`scheduler_run`) that drains the queue until empty or
   stopped.

All time-bound work in the simulator funnels through here.

## Mental Model — Event Callback + Fallback Table

```
   ┌─────────────────────────────────────────────────────────────┐
   │                       Scheduler                             │
   │                                                             │
   │   eq ────► EventQueue (sorted array, owned)                  │
   │   now    = 0                       (advances per event)     │
   │   running = 1                      (set false by stop)      │
   │                                                             │
   │   handlers[EVT_TYPE_COUNT]          fallback only            │
   │     ┌──────────────────────────┬─────────────────────┐      │
   │     │ EVT_ARP_REQUEST          │ arp_request_handler │ sim  │
   │     │ EVT_ARP_REPLY            │ arp_reply_handler   │ sim  │
   │     │ EVT_TIMER_EXPIRED        │ rip_tick            │ ctx  │
   │     │ ...                                                   │
   │     └───────────────────────────────────────────────────────┘
   └─────────────────────────────────────────────────────────────┘
```

The normal owner-specific path is stored inside each `Event`:

```c
e->handler(e, e->handler_ctx)
```

The handler table remains useful for simple global/default handlers. Each
entry is `(EventHandler fn, void *ctx)` — the `ctx` lets a fallback
handler reach its owner without globals.

---

## Header File — `scheduler.h`

### Types

```c
typedef EventCallback EventHandler;

typedef struct {
    EventHandler fn;     // NULL = no fallback handler registered
    void        *ctx;    // fallback context
} HandlerEntry;

typedef struct Scheduler {
    EventQueue   *eq;
    uint64_t      now;                       // current sim time
    int           running;                   // 1 while loop active
    HandlerEntry  handlers[EVT_TYPE_COUNT];  // fallback by EventType
} Scheduler;
```

### Public API

| Function                | Purpose                                          |
|-------------------------|--------------------------------------------------|
| `scheduler_create(cap)` | Allocate scheduler + queue of size `cap`.        |
| `scheduler_register`    | Bind a fallback handler/context to `EventType`.  |
| `scheduler_schedule`    | Enqueue an event (used by every module).         |
| `scheduler_step`        | Pop one event, advance `now`, dispatch, free.    |
| `scheduler_run`         | Loop step() while `running && !empty`.           |
| `scheduler_stop`        | Set `running = 0` (safe from inside a handler).  |
| `scheduler_now`         | Read current virtual time.                       |

### ACSL highlights

```
scheduler_register (valid):
    handlers[type].fn  == fn
    handlers[type].ctx == ctx
scheduler_step (postcondition):
    result == 1 ⇒ count decreased by 1 AND now monotonically non-decreased
scheduler_run (postcondition):
    on exit either queue empty OR running == 0
```

---

## Call Sequence — Initialization

```
sim_create:
    sched = scheduler_create(1024)            allocates event array of 1024
    Ethernet has no init hook; link receive events carry
    ethernet_receive_event directly.
    arp_init(sim)
        ├─► scheduler_register(sched, EVT_ARP_REQUEST,
        │                      arp_request_handler, sim)
        └─► scheduler_register(sched, EVT_ARP_REPLY,
                               arp_reply_handler, sim)
    IpStack ip_stack;
    ip_init(sim, &ip_stack)
        └─► initializes the stack and binds IP receive on interfaces
```

The fallback handler table is still a *single* slot per type. That is
fine for default handlers, but it is too coarse for owner-specific work.
A TCP retransmission timer, MAC aging timer, or protocol timeout should
carry a per-event callback/context so each event points at its exact
owner.

## Call Sequence — Main loop step

```
scheduler_step(s):
   │
   ├─► e = event_queue_pop(s->eq)            ┐ earliest event
   │   if (!e) return 0                        ┘
   │
   │   if e->timestamp > s->now:
   │       s->now = e->timestamp               virtual clock never moves backward
   │
   │   if (e->handler)
   │       e->handler(e, e->handler_ctx)       per-event dispatch
   │   else if e->type is valid
   │       h = s->handlers[e->type]
   │       if (h.fn) h.fn(e, h.ctx)            fallback dispatch
   │
   └─► event_free(e)                           Event freed; packet keeps life
       return 1
```

`scheduler_run` simply loops `step()` until `running == 0` or the queue
is empty.

---

## Design Notes

- **Per-event callback wins.** If `e->handler != NULL`, the scheduler
  calls that function with `e->handler_ctx`. This is the owner-specific
  path for timers and delayed work.
- **The handler table is fallback dispatch.** `scheduler_register`
  remains useful for simple events that do not need a per-event owner.
- **`ctx` is the de-facto `self` parameter.** Whether it comes from
  `e->handler_ctx` or the fallback table, handlers cast it back. No
  globals are needed.
- **`scheduler_stop` is safe inside a handler** — the current step
  finishes, then `run()` exits cleanly.
- **Time only moves forward.** Popping sets `now = e->timestamp`. A bug
  that schedules events in the past still executes, but `scheduler_step`
  does not move `now` backward.

## Implementation Guide

1. `scheduler_create`: allocate, zero handler table, create the event
   queue, initialize `now = 0` and `running = 0`.
2. `scheduler_register`: ignore NULL scheduler or out-of-range type;
   otherwise replace the fallback handler slot for that type.
3. `scheduler_schedule`: reject NULL scheduler/event; delegate to
   `event_queue_push`.
4. `scheduler_step`: pop one event. If none, return `0`. Advance `now`
   only if the event timestamp is greater than current `now`. If the
   event has `handler`, call it with `handler_ctx`. Otherwise, if
   `type` is valid, dispatch through the fallback table. Always
   shallow-free the event after dispatch.
5. `scheduler_run`: set `running = 1`, step while events exist and the
   scheduler remains running, then set `running = 0` before return.
6. `scheduler_free`: pop and free pending events, free queue, free
   scheduler. It does not free packets stored inside pending events.

## ACSL Contract Plan

Contract targets:

- `scheduler_create`: result has a valid queue, zeroed handlers,
  `now == 0`, and `running == 0`.
- `scheduler_register`: valid call updates exactly
  `handlers[type].fn` and `handlers[type].ctx`; invalid type is a no-op.
- `scheduler_schedule`: success increments `eq->count`; failure leaves
  count unchanged.
- `scheduler_step`: result `0` means no event was available; result `1`
  means one event was removed, `now >= old(now)`, and the event was
  shallow-freed after per-event or fallback dispatch.
- `scheduler_run`: exits only after the queue is empty or
  `scheduler_stop` made `running == 0`.
