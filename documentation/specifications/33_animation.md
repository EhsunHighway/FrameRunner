# Module 33 - Terminal Animation

**Files:** `src/display/animation.c`, `src/display/animation.h`
**Status:** Ready for implementation; files do not exist yet
**Depends on:** `simulator`, `scheduler`, `trace`, `topology_layout`,
`topology_view`, `header_view`, `event_log`

## Concepts First

The scheduler describes when state changes occur; animation describes how
recorded state changes are presented between those times. A packet can depart
at one simulated timestamp and arrive at another, so its trace record defines
an interval that can be interpolated across terminal frames.

The scheduler is still single-threaded even when many transmission intervals
overlap. The animation represents that logical concurrency with an active set,
not with one global "current packet" variable.

## Purpose

The animation module presents packet movement across the complete topology and
semantic processing inside devices. It allows many devices, protocol timers,
and packets to remain active in one event-driven run.

Animation never drives protocol decisions. The scheduler remains the source of
simulated time and ordering; the trace log supplies copied observations; the
animation interpolates display frames between recorded departure and arrival
times.

## Time Model

- Simulated time is measured by `Scheduler.now` and event timestamps.
- Wall-clock playback time controls how quickly simulated intervals are shown.
- Changing playback speed never changes event timestamps.
- Rendering intermediate frames never schedules network events.
- `step` processes one scheduler event.
- `tick` processes all events at the next simulated timestamp, including
  same-time events scheduled while that timestamp is being processed.
- A run must be bounded by simulated end time, event count, or explicit user
  stop because periodic protocols may keep the queue nonempty forever.

## Active Packet Model

```c
typedef struct ActivePacketVisual {
    uint32_t packet_id;
    uint32_t trace_id;
    uint32_t parent_packet_id;

    const Device    *source_device;
    const Device    *destination_device;
    const Interface *source_iface;
    const Interface *destination_iface;

    uint64_t departure_time;
    uint64_t arrival_time;
    uint8_t  protocol;
} ActivePacketVisual;
```

An active visual is created from a packet-transmission trace record and removed
after its matching receive or drop record has been presented. Several active
visuals may occupy the same link. The renderer must not replace the global
active set when focus changes.

## Focus

```c
typedef enum AnimationFocusType {
    ANIMATION_FOCUS_ALL,
    ANIMATION_FOCUS_AUTO,
    ANIMATION_FOCUS_DEVICE,
    ANIMATION_FOCUS_PACKET,
    ANIMATION_FOCUS_TRACE
} AnimationFocusType;
```

- `ALL` gives every active record equal emphasis.
- `AUTO` keeps the topology global and uses the process panel for the most
  recently processed record.
- `DEVICE` emphasizes records involving one device.
- `PACKET` follows one physical packet object.
- `TRACE` follows the complete causal operation, including clones, ARP, replies,
  errors, and retransmissions.

Unfocused activity remains visible when space permits; it is dimmed or reduced
to a compact marker rather than removed from simulation state.

## Animation State

```c
typedef struct AnimationState {
    Simulator      *sim;
    TraceLog       *trace;
    TopologyLayout *layout;
    FILE           *out;

    ActivePacketVisual *active_packets;
    size_t              active_count;
    size_t              active_capacity;
    size_t              next_trace_index;

    double             speed;
    AnimationFocusType focus_type;
    uint32_t           focus_id;
    const Device      *focus_device;

    int viewport_x;
    int viewport_y;
    int terminal_width;
    int terminal_height;
} AnimationState;
```

The animation owns its active array and layout. It borrows simulator, trace,
output stream, and focus-device pointers.

The simulator and its topology must outlive the animation. When loading a new
scenario, free the old animation before the old simulator.

## Public API

```c
AnimationState *animation_create(Simulator *sim,
                                 TraceLog  *trace,
                                 FILE      *out,
                                 int        width,
                                 int        height);

void animation_free(AnimationState *state);

int animation_relayout(AnimationState *state,
                       int             width,
                       int             height);

int animation_consume_new_records(AnimationState *state);
int animation_render(const AnimationState *state,
                     uint64_t              visual_time);

int animation_step(AnimationState *state);
int animation_tick(AnimationState *state);

int animation_run_until(AnimationState *state,
                        uint64_t        end_time,
                        size_t          max_events);

int animation_set_speed(AnimationState *state, double speed);
int animation_focus_all(AnimationState *state);
int animation_focus_auto(AnimationState *state);
int animation_focus_device(AnimationState *state, const Device *device);
int animation_focus_packet(AnimationState *state, uint32_t packet_id);
int animation_focus_trace(AnimationState *state, uint32_t trace_id);
```

## Rendering Rules

Each frame contains, when terminal size permits:

1. simulated time, playback speed, run state, and queued-event count
2. topology nodes and links from `TopologyLayout`
3. every active packet whose path intersects the viewport
4. a process panel describing the current or focused semantic record
5. a recent-event panel rendered from persistent trace records

Protocol markers have a plain-ASCII fallback:

```text
[A] ARP   [I] ICMP   [U] UDP   [T] TCP   [R] RIP   [O] OSPF
```

Color may be enabled as a renderer option, but meaning must never depend on
color alone.

Packet position for a visual time between departure and arrival is calculated
from the fraction:

```text
(visual_time - departure_time) / (arrival_time - departure_time)
```

Clamp before departure to the source endpoint and after arrival to the
destination endpoint. A zero-duration transfer is shown at the destination.

## Function Behavior

### `animation_create`

1. Reject a null simulator, trace, output stream, simulator topology, or
   nonpositive width/height with `NULL`.
2. Allocate and zero one animation state.
3. Create a topology layout borrowing the simulator topology.
4. If layout creation fails, free the animation state and return `NULL`.
5. Store borrowed pointers and dimensions; set speed to `1.0`, focus to
   `ANIMATION_FOCUS_AUTO`, focus ID to zero, focus device to `NULL`, viewport
   origin to `(0, 0)`, and next trace index to zero.
6. Call `animation_consume_new_records` so an existing trace can be displayed.
7. On consume failure, free layout and state and return `NULL`.
8. Return the completed animation.

### `animation_free`

1. If `state == NULL`, return without action.
2. Free the active-packet array.
3. Free the owned topology layout.
4. Free animation state; do not free simulator, trace, stream, devices, or
   interfaces.

### `animation_relayout`

1. Reject null state/layout or nonpositive dimensions with `-1`.
2. Rebuild the existing layout against `state->sim->topology`.
3. On failure, preserve the prior layout and dimensions and return `-1`.
4. Store the new terminal dimensions, clamp viewport offsets into the new
   content range, and return `0`.

### `animation_consume_new_records`

1. Validate state, simulator topology, trace, active-array counters, and
   `next_trace_index <= trace->count`; otherwise return `-1`.
2. Work on a temporary copy of the active array and selection state so failure
   does not partially consume the trace.
3. Scan trace indexes from `next_trace_index` through `trace->count - 1` in
   increasing order.
4. For a `TRACE_PACKET_TX` record, resolve its copied source and destination
   device/interface names against the still-live simulator topology. If either
   endpoint cannot be resolved, treat the record as non-animatable but keep it
   available to the event log. Otherwise append one active interval containing
   its IDs, resolved borrowed pointers, protocol, timestamp, and end timestamp.
5. Before appending, remove an older active interval with the same packet ID;
   one packet object cannot occupy two link intervals at once. Other packet IDs
   remain untouched.
6. For `TRACE_PACKET_RX` or `TRACE_PACKET_DROPPED`, remove the active interval
   whose packet ID matches the record. No match is an allowed historical or
   non-link record.
7. Let the newest record matching current focus become the process-panel
   selection. Focus affects selection only, not active-set membership.
8. If allocation fails, free temporary work and return `-1` without advancing
   `next_trace_index` or changing the published active set.
9. Publish the completed active set and selection, set `next_trace_index` to
   `trace->count`, and return `0`.

### `animation_render`

1. Validate state, output stream, layout, active range, and focus state.
2. Clear or begin one complete frame according to the configured terminal
   mode.
3. Print status, topology, every active packet intersecting the viewport, the
   selected process record, and recent trace records in the Rendering Rules
   order.
4. Derive packet coordinates only from layout endpoints and the clamped time
   fraction; do not modify active intervals while rendering.
5. If any output operation fails, return `-1`; otherwise finish the frame and
   return `0`.

### `animation_set_speed`

1. Reject null state, non-finite speed, or `speed <= 0.0` with `-1`.
2. Store the speed and return `0`; do not modify scheduler timestamps.

### Focus setters

- Every focus function rejects null state with `-1`.
- `animation_focus_all` and `animation_focus_auto` set the named mode and clear
  focus ID/device.
- `animation_focus_device` additionally rejects a null device or a device not
  present in the current layout. On success, store the borrowed device and
  clear focus ID.
- Packet and trace focus reject ID zero. On success, store the ID, clear focus
  device, and set the corresponding mode. The ID may have no records yet; this
  allows focus to be selected before new activity arrives.
- A rejected call preserves the previous focus.

### `animation_step`

1. Validate state, simulator, scheduler, and trace references.
2. Call `simulator_step` exactly once.
3. If the call returns a negative value, return `-1` without pretending an
   event was processed.
4. If no event existed, consume any pending trace records, render the current
   state once, and return `0`.
5. Consume every trace record created by that event in sequence order.
6. Update active packet visuals, current process selection, and viewport focus.
7. Render at the scheduler's new simulated time and return `1`.

### `animation_tick`

1. Peek at the next event. If none exists, render current state and return `0`.
2. Save its timestamp as `tick_time`.
3. Repeatedly process one event while the queue head exists and its timestamp
   equals `tick_time`. Re-check the queue after each event so newly scheduled
   same-time events are included.
4. Consume trace records after each event so packet ownership never affects
   display data.
5. Render one combined frame for `tick_time` and return the number of processed
   events.

### `animation_run_until`

1. Reject a call when both `end_time == 0` and `max_events == 0`, or when
   `max_events > INT_MAX`; an unbounded run is unsafe with periodic timers and
   the processed-count return type is `int`.
2. Process events in scheduler order until the next event exceeds end time,
   the processed count reaches max events, the queue becomes empty, or the
   simulator is stopped.
3. Before each scheduler time jump, interpolate active packets through wall-
   clock frames according to `speed`. Rendering delays do not modify scheduler
   state.
4. Consume newly appended trace records after every processed event.
5. Render a final frame and return the processed-event count, or `-1` on an
   animation-state failure.

## Terminal and Testing Boundary

The first implementation may use ANSI cursor movement but must also support a
plain-frame mode that writes complete sequential frames to a `FILE *`. Layout,
record consumption, interpolation, focus selection, and frame text must be
testable without sleeping or requiring an interactive terminal.

## Verification Expectations

Tests cover concurrent packets, opposite-direction traffic, equal-time event
batches, zero-delay links, packet drops, clone/trace focus, viewport clipping,
relayout, bounded periodic runs, plain output, and deterministic replay from a
fixed trace log.

## ACSL Contract Targets

Contracts must cover borrowed versus owned pointers, active-count bounds,
next-trace-index bounds, positive dimensions and speed, failure preserving
focus/layout state, and free not releasing borrowed simulator objects.

## Common Mistakes

- Advancing simulated time once per rendered wall-clock frame.
- Keeping only one active packet and overwriting it when another device sends.
- Reconstructing animation by dereferencing a packet that has already been
  freed.
- Hiding unfocused packets by deleting them from the active set.
- Implementing an unbounded run that never returns while RIP or OSPF timers
  continue to schedule events.
- Freeing the simulator before the animation that borrows its devices.
