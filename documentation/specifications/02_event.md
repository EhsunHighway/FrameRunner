# Module 02 — Event System

**Files:** `src/engine/event.c`, `src/engine/event.h`
**Status:** ✅ Implemented (92% / 81%)
**Depends on:** packet

---

## The Problem

A discrete-event simulator advances **only when something happens**.
"Something" is an `Event`: a timestamped record of *what should be
processed, at what virtual time, by whom*. The simulator needs:

1. A typed event record any module can produce.
2. A priority queue that always pops the **earliest** event next.
3. Enough dispatch information to let the scheduler run the right code
   when the event becomes due.

This module supplies the event record and the sorted queue. The scheduler
uses the dispatch fields when it executes events.

## Mental Model — Sorted Array

```
   EventQueue.events[]             (timestamp ascending)

   [0]  10ms  PACKET_RECEIVE  dst = R1.eth0
   [1]  15ms  ARP_REPLY
   [2]  20ms  TIMER_EXPIRED
   [3]  40ms  MAC_AGE
```

`push()` inserts at the sorted position by shifting later elements.
`pop()` returns `events[0]` and shifts the rest left. This is simple and
deterministic for the small queues used in the project.

---

## Header File — `event.h`

### Event Types — the classification key

```c
typedef enum {
    EVT_PACKET_SEND,
    EVT_PACKET_RECEIVE,   // link delivery; event usually carries ethernet callback
    EVT_ARP_REQUEST,      // arp listens on this
    EVT_ARP_REPLY,        // arp listens on this
    EVT_ROUTING_UPDATE,
    EVT_ROUTE_ADDED,
    EVT_LINK_UP,
    EVT_LINK_DOWN,
    EVT_TIMER_EXPIRED,    // periodic protocols (RIP, OSPF hello)
    EVT_RENDER,
    EVT_RESET,
    // L2 / MAC
    EVT_MAC_AGE,          // switch timer; event carries Switch context
    // Transport
    EVT_TCP_RETRANSMIT,
    // RIP
    EVT_RIP_UPDATE,
    EVT_RIP_TIMEOUT,
    EVT_RIP_GC,
    // OSPF
    EVT_OSPF_HELLO,
    EVT_OSPF_DEAD,
    EVT_OSPF_SPF,
    // BGP
    EVT_BGP_KEEPALIVE,
    EVT_BGP_HOLD,
    EVT_BGP_CONNECT_RETRY,
    // EIGRP
    EVT_EIGRP_HELLO,
    EVT_EIGRP_HOLD,
    // IS-IS
    EVT_ISIS_HELLO,
    EVT_ISIS_HOLD,
    EVT_ISIS_SPF,
    EVT_ISIS_LSP_REGEN,
    // NAT
    EVT_NAT_GC,
    EVT_TYPE_COUNT        // sentinel — must be last
} EventType;
```

`EventType` classifies the event. It is still useful for logging,
filtering, fallback dispatch, and tests. It is not the only dispatch
mechanism: an individual event may also carry its own callback and
callback context.

This distinction matters for timers and protocol work. There may be many
`EVT_TCP_RETRANSMIT` events alive at once, each belonging to a different
connection. A single global context for the event type is too coarse.
The event itself should be able to say: "when I fire, call this function
with this owner/context."

### Event struct

```c
typedef struct Event Event;
typedef void (*EventCallback)(const Event *e, void *ctx);

struct Event {
    EventType type;
    uint64_t  timestamp;     // simulated microseconds
    void     *src_device;    // typed by event: Interface*, Device*, ...
    void     *dst_device;    // same idea
    void     *packet;        // Packet* — void* to avoid circular include
    void     *data;          // protocol-specific extra payload
    EventCallback handler;   // optional per-event callback
    void     *handler_ctx;   // context passed to handler
};
```

`src_device`, `dst_device`, `packet`, `data` are `void *` on purpose —
event.h must not include every protocol header. Handlers cast back to
the concrete type they expect (e.g. `(Interface *) e->dst_device`).

`handler` and `handler_ctx` are also optional. If `handler != NULL`, the
scheduler should run this exact callback for this event. If `handler ==
NULL`, the scheduler may fall back to a registered handler for
`e->type`.

### EventQueue struct

```c
typedef struct EventQueue {
    Event   **events;     // sorted pointer array, not copies
    size_t    count;
    size_t    capacity;
} EventQueue;
```

### Public API

| Function                  | Purpose                                            |
|---------------------------|----------------------------------------------------|
| `event_queue_create(cap)` | Allocate empty sorted event array.                 |
| `event_queue_push`        | Insert sorted by `timestamp`. O(n).                |
| `event_queue_pop`         | Remove + return earliest event. O(n).              |
| `event_queue_peek`        | Look without removing.                             |
| `event_queue_is_empty`    | Trivial.                                           |
| `event_create`            | Allocate + populate an Event.                      |
| `event_create_callback`   | Allocate an Event with per-event callback/context. |
| `event_free`              | Free Event only — **not** its `packet` or `data`.  |

### ACSL highlights

```
event_queue_push:
    \result == 0  ⇒ count == \old(count) + 1
event_queue_pop:
    \result != \null ⇒ count == \old(count) - 1
event_queue_is_empty:
    \result == 1 <==> count == 0
```

---

## Call Sequence — Scheduling a packet delivery

```
link_transmit(link, pkt, src_iface, sched, now):
   │
   │   delay = bytes * 8 / bw_mbps + delay_ms
   │   arrival = now + delay
   │
   ├─► event_create_callback(EVT_PACKET_RECEIVE, arrival,
   │                         src_iface, dst_iface, pkt, NULL,
   │                         ethernet_receive_event, net_ctx)
   │       │  malloc + populate Event struct
   │       ▼
   │     Event *e
   │
   └─► scheduler_schedule(sched, e)
           │
           └─► event_queue_push(sched->eq, e)
                   │  shift later events right until timestamp order holds
                   │  insert e               O(n)
                   │  count++
                   ▼
                  ok
```

## Call Sequence — Main loop pulls events

```
scheduler_step(sched):
   │
   ├─► Event *e = event_queue_pop(sched->eq)
   │       │  events[0] is earliest
   │       │  memmove events[1..] left
   │       │  count--
   │       ▼
   │     return e
   │
   │   sched->now = e->timestamp              ← virtual clock advances
   │   if e->handler != NULL
   │       e->handler(e, e->handler_ctx)       ← per-event dispatch
   │   else
   │       h = sched->handlers[e->type]        ← fallback dispatch
   │       h.fn(e, h.ctx)
   │
   └─► event_free(e)                          ← packet keeps its own life
```

---

## Design Notes

- **Queue stores pointers, not values.** Inserts and pops move pointers,
  not whole events; event addresses stay stable.
- **`event_free` is shallow.** Handlers own packet lifetimes. Freeing the
  packet here would double-free in most flows.
- **Per-event callbacks are the normal owner-specific path.** A timer,
  retransmission, or delayed packet delivery can carry the exact callback
  and context that should handle it. This avoids one global context per
  event type.
- **Event type dispatch is a fallback and classification mechanism.**
  It keeps simple events easy and keeps event names useful for tracing,
  but it should not be the only way to route owner-specific work.
- **Ties are FIFO by insertion.** `event_queue_push` only shifts entries
  with a strictly greater timestamp, so equal timestamps remain in
  original order.
- **Capacity grows with `realloc`.** A full queue doubles its capacity on
  push. If `realloc` fails, the event is not inserted and `count` is
  unchanged.

## Implementation Guide

1. `event_create_callback`: allocate one `Event` and copy all fields,
   including `handler` and `handler_ctx`.
2. `event_create`: call `event_create_callback` with `handler = NULL`
   and `handler_ctx = NULL`. This keeps existing callers compatible.
3. `event_queue_create`: allocate the queue and the pointer array; set
   `count = 0` and `capacity` to the requested value.
4. `event_queue_push`: grow if full; find insertion index from the end;
   shift greater timestamps right; insert; increment count.
5. `event_queue_pop`: return `NULL` on empty; otherwise save `events[0]`,
   shift the remaining pointers left, decrement count, and return saved
   event.
6. `event_free`: shallow free only.

## ACSL Contract Plan

Useful predicates:

```c
/*@ predicate queue_sorted{L}(EventQueue *eq) =
      \valid(eq) &&
      \forall integer i, j;
        0 <= i <= j < eq->count ==>
          eq->events[i]->timestamp <= eq->events[j]->timestamp;
*/
```

Contract targets:

- `event_queue_push`: success increments count, preserves
  `queue_sorted`, and keeps the new event reachable in `events`.
- `event_queue_pop`: non-NULL result is the old earliest event and count
  decreases by one.
- `event_queue_peek`: returns the old earliest event without changing
  count.
- `event_create_callback`: non-NULL result preserves `handler` and
  `handler_ctx`.
- `event_free`: frees only the `Event`, never `packet` or `data`.
