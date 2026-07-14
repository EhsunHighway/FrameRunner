# Module 30 - Simulation Trace

**Files:** `src/engine/trace.c`, `src/engine/trace.h`
**Status:** Ready for implementation; files do not exist yet
**Depends on:** `packet`, `event`

## Concepts First

An event queue is future work; a trace is durable history. A protocol may make
several observable decisions while processing one event, and a live packet may
be changed or freed before a user asks to inspect its journey. The trace must
therefore store semantic records and bounded byte copies rather than pointers
to runtime objects.

Packet lineage separates one allocated buffer from the wider operation that
caused it. An ARP request, an ICMP reply, and a cloned Ethernet frame can have
different packet IDs while remaining part of the same trace.

## Purpose

The trace module stores persistent, read-only facts about simulation activity
after the live `Packet` and `Event` objects have been changed or freed. It is
the data source for animation, event history, packet journeys, and CLI trace
inspection.

Trace is observation, not simulation. A trace allocation or append failure
must be reportable, but enabling or disabling tracing must not change packet
delivery, protocol state, route selection, or event ordering.

## Identity Model

Three identities are distinct:

- `packet_id`: one allocated `Packet` object.
- `trace_id`: one causal user operation or protocol transaction shared by
  related packets and clones.
- `parent_packet_id`: the immediate packet object cloned or used to generate
  this packet; `0` means no parent is recorded.

`packet_create` starts a new trace. `packet_clone` assigns a new packet ID,
copies the source trace ID, and records the source packet ID as its parent.
Protocol-generated requests, replies, and errors explicitly inherit the trace
of the packet that caused them when such a packet exists.

## Trace Actions

The first implementation defines stable semantic actions rather than logging C
function calls:

```c
typedef enum TraceAction {
    TRACE_EVENT_SCHEDULED,
    TRACE_EVENT_STARTED,
    TRACE_EVENT_FINISHED,
    TRACE_PACKET_CREATED,
    TRACE_PACKET_CLONED,
    TRACE_PACKET_TX,
    TRACE_PACKET_RX,
    TRACE_PACKET_DROPPED,
    TRACE_HEADER_ADDED,
    TRACE_HEADER_REMOVED,
    TRACE_ARP_LOOKUP,
    TRACE_ARP_REQUEST,
    TRACE_ARP_REPLY,
    TRACE_ARP_CACHE_UPDATE,
    TRACE_MAC_LEARN,
    TRACE_MAC_LOOKUP,
    TRACE_SWITCH_FORWARD,
    TRACE_SWITCH_FLOOD,
    TRACE_IP_LOCAL_DELIVERY,
    TRACE_ROUTE_LOOKUP,
    TRACE_ROUTE_SELECTED,
    TRACE_TTL_CHANGED,
    TRACE_TRANSPORT_DELIVERY,
    TRACE_PROTOCOL_STATE_CHANGED,
    TRACE_TIMER_FIRED,
    TRACE_ROUTE_CHANGED
} TraceAction;
```

New actions may be appended; existing numeric meanings must not be reordered
after trace files or tests depend on them.

## Data Model

```c
#define TRACE_NAME_LEN            32
#define TRACE_SUMMARY_LEN         128
#define TRACE_PACKET_SNAPSHOT_MAX 256

typedef struct TraceRecord {
    uint64_t    timestamp;
    uint64_t    end_timestamp;
    uint64_t    sequence;
    TraceAction action;
    EventType   event_type;

    uint32_t packet_id;
    uint32_t trace_id;
    uint32_t parent_packet_id;

    uint8_t  protocol;
    uint8_t  layer;
    int      result;

    char source_device[TRACE_NAME_LEN];
    char source_iface[TRACE_NAME_LEN];
    char destination_device[TRACE_NAME_LEN];
    char destination_iface[TRACE_NAME_LEN];
    char summary[TRACE_SUMMARY_LEN];

    size_t   packet_length;
    size_t   snapshot_length;
    uint8_t  snapshot_truncated;
    uint8_t  snapshot[TRACE_PACKET_SNAPSHOT_MAX];
} TraceRecord;

typedef struct TraceLog {
    TraceRecord *records;
    size_t       count;
    size_t       capacity;
    uint64_t     next_sequence;
    int          enabled;
} TraceLog;
```

Names, summaries, and packet bytes are copied. A record never owns or borrows a
live `Packet *`, `Event *`, `Device *`, or `Interface *`.

`sequence` orders records that have the same simulated timestamp. It is
assigned by the trace log and increases monotonically for every stored record.

`end_timestamp` equals `timestamp` for an instantaneous observation. A
transmission record stores departure in `timestamp` and scheduled arrival in
`end_timestamp`, allowing animation to interpolate without parsing summary
text.

The packet snapshot begins at the packet's current visible `data` pointer. It
contains `min(pkt->len, TRACE_PACKET_SNAPSHOT_MAX)` bytes. `packet_length`
retains the complete visible length and `snapshot_truncated` states whether the
copy is partial.

## Ownership And Lifetime

The simulator owns one `TraceLog`. The log owns its record array, and each
record contains only values and copied bytes. A pointer returned by
`trace_log_get` borrows an element of that array and is invalidated by a grow,
clear, or free operation.

## Public API

```c
TraceLog *trace_log_create(size_t initial_capacity);
void      trace_log_free(TraceLog *log);
void      trace_log_clear(TraceLog *log);
void      trace_log_set_enabled(TraceLog *log, int enabled);

int       trace_log_append(TraceLog          *log,
                           const TraceRecord *record);

size_t    trace_log_count(const TraceLog *log);

const TraceRecord *trace_log_get(const TraceLog *log,
                                 size_t          index);

int trace_record_capture_packet(TraceRecord *record,
                                const Packet *pkt);
```

## Function Behavior

### `trace_log_create`

1. Reject `initial_capacity == 0` with `NULL`.
2. Allocate and zero one `TraceLog`.
3. Allocate exactly `initial_capacity` record slots.
4. On record-array failure, free the log and return `NULL`.
5. Set count to zero, capacity to the requested value, next sequence to `1`,
   and tracing enabled.

### `trace_log_append`

1. If `log == NULL || record == NULL`, return `-1`.
2. If tracing is disabled, return `0` without changing the log.
3. If the array is full, grow it by doubling capacity. On overflow or
   allocation failure, return `-1` without changing existing records.
4. Copy the complete input record into `records[count]`.
5. Force NUL termination of every copied text field.
6. Assign `records[count].sequence = next_sequence`, then increment
   `next_sequence`.
7. Increment count and return `0`.

### `trace_log_set_enabled`

1. If `log == NULL`, return without action.
2. Store `1` when `enabled != 0`; otherwise store `0`.
3. Do not clear records, reset sequence, or change capacity.

### `trace_record_capture_packet`

1. If `record == NULL`, return `-1`.
2. Clear packet identity, length, snapshot-length, and truncation fields.
3. If `pkt == NULL`, return `0`; a non-packet event may still be traced.
4. Validate the current packet view before reading bytes. On invalid view,
   return `-1` without reading packet data.
5. Copy packet ID, trace ID, parent ID, layer, and visible length.
6. Copy at most `TRACE_PACKET_SNAPSHOT_MAX` visible bytes.
7. Set truncation when the visible length exceeds the copied length and return
   `0`.

### `trace_log_count`

1. Return `0` when `log == NULL`.
2. Otherwise return `log->count` without modifying the log.

### `trace_log_get`

1. If `log == NULL` or `index >= log->count`, return `NULL`.
2. Otherwise return the borrowed address of `log->records[index]`.

### `trace_log_clear`

1. If `log == NULL`, return without action.
2. Set count to zero and next sequence to `1`.
3. Preserve the record allocation, capacity, enabled state, and existing bytes
   outside the now-empty logical range.

### `trace_log_free`

1. If `log == NULL`, return without action.
2. Free the record array, then free the log.

## Emission Boundary

Scheduler and protocol modules build a complete local `TraceRecord`, initialize
it to zero, fill semantic fields, capture a packet when relevant, and pass it
to the simulator-owned log. Display modules only read records.

Do not store formatted pointer values, rely on object addresses as identity, or
retain borrowed object pointers in the log.

## Verification Expectations

Tests cover disabled tracing, growth, stable equal-time sequence ordering,
clear/reuse, text termination, null and non-packet records, complete and
truncated packet snapshots, clone lineage, and record validity after the live
packet is freed.

## ACSL Contract Targets

Contracts must cover null behavior, `count <= capacity`, the valid logical
record range, borrowed query results, the no-change disabled append path, and
clear preserving capacity. Allocation growth and byte-copy correctness remain
dynamic-test obligations.

## Common Mistakes

- Logging a `Packet *` and attempting to render it after ownership transfer.
- Reusing event sequence as trace-record sequence; they order different
  collections.
- Formatting a pointer as identity instead of copying stable IDs and names.
- Treating trace append failure as permission to change network behavior.
- Copying `pkt->capacity` bytes instead of the validated visible packet view.
