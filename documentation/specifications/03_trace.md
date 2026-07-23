# Module 03 - Simulation Trace

**Files:** `src/engine/trace.c`, `src/engine/trace.h`
**Status:** Ready for implementation; files do not exist yet
**Depends on:** `packet`, `event`

## Concepts First

An event queue stores work that must execute in the future; a trace stores
durable history. A protocol may make several observable decisions while
processing one event, and a live packet may
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

## Architecture Boundary

`trace.h` includes the standard integer/size definitions plus `event.h` and
`packet.h`, because its public types and capture API use `EventType` and
`Packet`. `trace.c` additionally uses the allocation and byte-copy facilities
from the C standard library.

The trace module does not include `simulator.h`, scheduler headers, protocol
headers, device headers, or display headers. Simulator owns a `TraceLog` and
calls this lower-level API; the trace module never calls back into Simulator.

Trace records contain generic protocol numbers, copied names, result values,
and summaries. They do not embed protocol-specific structs or display-ready
formatted boxes.

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
    TRACE_ACTION_UNSPECIFIED       = 0,
    TRACE_EVENT_SCHEDULED          = 1,
    TRACE_EVENT_STARTED            = 2,
    TRACE_EVENT_FINISHED           = 3,
    TRACE_PACKET_CREATED           = 4,
    TRACE_PACKET_CLONED            = 5,
    TRACE_PACKET_TX                = 6,
    TRACE_PACKET_RX                = 7,
    TRACE_PACKET_DROPPED           = 8,
    TRACE_HEADER_ADDED             = 9,
    TRACE_HEADER_REMOVED           = 10,
    TRACE_ARP_LOOKUP               = 11,
    TRACE_ARP_REQUEST              = 12,
    TRACE_ARP_REPLY                = 13,
    TRACE_ARP_CACHE_UPDATE         = 14,
    TRACE_MAC_LEARN                = 15,
    TRACE_MAC_LOOKUP               = 16,
    TRACE_SWITCH_FORWARD           = 17,
    TRACE_SWITCH_FLOOD             = 18,
    TRACE_IP_LOCAL_DELIVERY        = 19,
    TRACE_ROUTE_LOOKUP             = 20,
    TRACE_ROUTE_SELECTED           = 21,
    TRACE_TTL_CHANGED              = 22,
    TRACE_TRANSPORT_DELIVERY       = 23,
    TRACE_PROTOCOL_STATE_CHANGED   = 24,
    TRACE_TIMER_FIRED              = 25,
    TRACE_ROUTE_CHANGED            = 26,
    TRACE_ACTION_COUNT             = 27
} TraceAction;
```

`TRACE_ACTION_UNSPECIFIED` is invalid for a stored record. New actions are
inserted immediately before `TRACE_ACTION_COUNT` and assigned new explicit
values. Existing numeric meanings must never change.

## Data Model

```c
#define TRACE_NAME_LEN            32
#define TRACE_SUMMARY_LEN         128
#define TRACE_PACKET_SNAPSHOT_MAX 256

typedef enum TraceResult {
    TRACE_RESULT_FAILURE = -1,
    TRACE_RESULT_NONE    = 0,
    TRACE_RESULT_SUCCESS = 1
} TraceResult;

typedef struct TraceRecord {
    uint64_t    timestamp;
    uint64_t    end_timestamp;
    uint64_t    event_timestamp;
    uint64_t    sequence;
    uint64_t    event_sequence;
    TraceAction action;
    EventType   event_type;

    uint32_t packet_id;
    uint32_t trace_id;
    uint32_t parent_packet_id;

    uint8_t  protocol;
    uint8_t  layer;
    TraceResult result;

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

### Field Meanings And Sentinel Values

| Field | Required meaning |
|---|---|
| `timestamp` | Simulated time at which the observation starts. |
| `end_timestamp` | Simulated completion/arrival time; equal to `timestamp` for an instantaneous action. |
| `event_timestamp` | Scheduled execution time of the associated event; zero when `event_type == EVT_TYPE_COUNT`. |
| `sequence` | Assigned only by `trace_log_append`; caller-supplied value is ignored. |
| `event_sequence` | Scheduler-assigned sequence of the associated event; zero when no event is associated. |
| `action` | One value strictly between `TRACE_ACTION_UNSPECIFIED` and `TRACE_ACTION_COUNT`. |
| `event_type` | Associated event type, or `EVT_TYPE_COUNT` when this record has no associated event. |
| `packet_id` | One packet object's ID, or zero when no packet is associated. |
| `trace_id` | Causal-journey ID, or zero when the action has no packet/causal journey. |
| `parent_packet_id` | Immediate causal packet ID, or zero when absent. |
| `protocol` | IPv4 protocol number; zero means unspecified in this simulator trace. |
| `layer` | `1 .. 4` for a known layer; zero means unspecified. |
| `result` | `FAILURE`, `NONE`, or `SUCCESS`; summaries carry action-specific detail. |
| name fields | Copied object names; an empty string means that endpoint component is unknown or inapplicable. |
| `summary` | Copied human-readable detail; it is never parsed to recover structured state. |
| `packet_length` | Complete visible packet length at capture time. |
| `snapshot_length` | Number of valid bytes in `snapshot`. |
| `snapshot_truncated` | One exactly when `snapshot_length < packet_length`; otherwise zero. |

`EVT_TYPE_COUNT` is deliberately reused as the no-event sentinel because it is
not an executable event type. A zero-initialized `event_type` is
`EVT_PACKET_SEND`, so callers must not use plain zero initialization as a
complete record initializer.

`TraceResult` is interpreted consistently:

- `TRACE_EVENT_STARTED`, `TRACE_EVENT_FINISHED`, and `TRACE_TIMER_FIRED` use
  `TRACE_RESULT_NONE` because they report occurrence rather than a boolean
  decision.
- `TRACE_ARP_LOOKUP`, `TRACE_MAC_LOOKUP`, and `TRACE_ROUTE_LOOKUP` use
  `TRACE_RESULT_SUCCESS` for a hit and `TRACE_RESULT_FAILURE` for a miss.
- `TRACE_PACKET_DROPPED` always uses `TRACE_RESULT_FAILURE`.
- Every other stored action describes a completed successful observation and
  uses `TRACE_RESULT_SUCCESS`.

`trace_record_init` establishes safe defaults, not necessarily an append-ready
record. For an action requiring success or failure, the emitter sets `result`
before append.

### Required Invariants

A valid stored record satisfies all of these rules:

- `timestamp <= end_timestamp`
- actions other than `TRACE_PACKET_TX` require `end_timestamp == timestamp`
- `TRACE_ACTION_UNSPECIFIED < action < TRACE_ACTION_COUNT`
- `EVT_PACKET_SEND <= event_type <= EVT_TYPE_COUNT`
- `event_type == EVT_TYPE_COUNT` requires `event_timestamp == 0` and
  `event_sequence == 0`
- `event_type < EVT_TYPE_COUNT` requires `event_sequence != 0`
- `layer <= 4`
- `result` is exactly one declared `TraceResult` value
- `result` follows the action-specific mapping above
- `snapshot_length <= TRACE_PACKET_SNAPSHOT_MAX`
- `snapshot_length <= packet_length`
- `snapshot_truncated == (snapshot_length < packet_length)`
- `packet_id != 0` implies `trace_id != 0`
- `parent_packet_id != 0` implies both `packet_id != 0` and `trace_id != 0`
- event actions (`SCHEDULED`, `STARTED`, and `FINISHED`) require
  `event_type < EVT_TYPE_COUNT`
- packet creation, clone, transmission, reception, drop, header, and delivery
  actions require nonzero packet and trace IDs
- every stored text array contains a terminating NUL in its final slot

A valid log satisfies:

- `records != NULL`
- `capacity > 0`
- `count <= capacity`
- `enabled` is exactly zero or one
- `next_sequence` is nonzero; `UINT64_MAX` means sequence space is exhausted
  and no later enabled append can succeed
- every record in `records[0 .. count - 1]` satisfies the stored-record
  invariants
- stored sequences are nonzero and strictly increase with record index; every
  stored sequence is lower than `next_sequence`

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

The simulator always owns one non-null `TraceLog`; runtime tracing is disabled
by setting `log->enabled` to zero, not by removing the log. The log owns its
record array, and each record contains only values and copied bytes. A pointer returned by
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

int trace_record_init(TraceRecord *record,
                      TraceAction action,
                      uint64_t    timestamp);
```

## Function Behavior

### `trace_log_create`

Purpose: allocate an enabled, empty trace log with caller-selected initial
capacity.

Implementation order:

1. Reject `initial_capacity == 0` with `NULL`.
2. If `initial_capacity > SIZE_MAX / sizeof(TraceRecord)`, return `NULL`
   without allocating.
3. Allocate and zero one `TraceLog`. If allocation fails, return `NULL`.
4. Allocate a zeroed array of exactly `initial_capacity` record slots.
5. On record-array failure, free the log and return `NULL`.
6. Store the array, set count to zero, capacity to `initial_capacity`, next
   sequence to `1`, and enabled to `1`.
7. Return the initialized log.

### `trace_record_init`

Purpose: initialize every field of one caller-owned record and establish safe
sentinel values before action-specific fields are added.

Implementation order:

1. If `record == NULL`, return `-1`.
2. If `action <= TRACE_ACTION_UNSPECIFIED || action >= TRACE_ACTION_COUNT`,
   return `-1` without modifying the record.
3. Clear the complete `TraceRecord` to zero.
4. Store `timestamp` in both `timestamp` and `end_timestamp`.
5. Store `action`, set `event_type = EVT_TYPE_COUNT`, and set
   `result = TRACE_RESULT_NONE`.
6. Leave event timestamp, trace sequence, event sequence, packet identity,
   protocol, layer, names, summary, lengths, truncation, and snapshot bytes
   zero.
7. Return `0`.

### `trace_log_append`

Purpose: validate and copy one complete record into the persistent log while
assigning its log-local sequence number.

Implementation order:

1. If `log == NULL || record == NULL`, return `-1`.
2. Validate the log shape: records must be non-null, capacity nonzero,
   `count <= capacity`, enabled must be zero or one, and next sequence must be
   nonzero. Return
   `-1` without modification when the shape is invalid.
3. If tracing is disabled, return `0` without validating or changing the input
   record and without consuming a sequence.
4. Validate the input record using every Required Invariant except stored-text
   NUL termination and sequence. The caller's sequence is ignored. Return
   `-1` without changing the log for an invalid record.
5. If `next_sequence == UINT64_MAX`, return `-1` without changing the log.
6. If `count == capacity`:
   - if `capacity > SIZE_MAX / 2`, return `-1`
   - set `new_capacity = capacity * 2`
   - if `new_capacity > SIZE_MAX / sizeof(TraceRecord)`, return `-1`
   - call `realloc` into a temporary pointer for exactly
     `new_capacity * sizeof(TraceRecord)` bytes
   - on allocation failure, return `-1` while preserving records, count,
     capacity, and next sequence
   - on success, publish the new pointer and capacity
7. Copy the complete input record into `records[count]`.
8. Set the final byte of every copied name and summary array to NUL.
9. Set `records[count].sequence = next_sequence`.
10. Increment next sequence, then increment count; neither operation wraps.
11. Return `0`.

### `trace_log_set_enabled`

Purpose: enable or disable future record storage without changing existing
history.

Implementation order:

1. If `log == NULL`, return without action.
2. If records is null, capacity is zero, or count exceeds capacity, return
   without modifying the malformed log.
3. Store `1` when `enabled != 0`; otherwise store `0`.
4. Do not clear records, reset sequence, or change capacity.

### `trace_record_capture_packet`

Purpose: replace only the packet-derived portion of a caller-owned record with
identity, layer, length, and bounded visible bytes from one borrowed packet.

Implementation order:

1. If `record == NULL`, return `-1`.
2. Set packet ID, trace ID, parent packet ID, layer, packet length, snapshot
   length, and truncation to zero; clear the complete snapshot array.
3. If `pkt == NULL`, return `0`; a non-packet event may still be traced.
4. Call `packet_validate_view(pkt, 0, 0)`. On failure, return `-1` without
   reading packet bytes; the cleared packet fields from step 2 remain cleared.
5. If `pkt->id == 0 || pkt->trace_id == 0`, return `-1` with the same cleared
   packet fields.
6. If `pkt->layer < 0 || pkt->layer > 4`, return `-1` with the packet fields
   still cleared.
7. Copy `id`, `trace_id`, `parent_id`, layer, and visible length.
8. Set snapshot length to the smaller of visible length and
   `TRACE_PACKET_SNAPSHOT_MAX`.
9. If snapshot length is nonzero, copy exactly that many bytes beginning at
   `pkt->data`.
10. Set truncation to one when visible length exceeds snapshot length; otherwise
   leave it zero.
11. Return `0`.

### `trace_log_count`

Purpose: report the number of records currently stored in a valid log.

Implementation order:

1. Return `0` when `log == NULL`.
2. Return `0` when records is null, capacity is zero, or count exceeds
   capacity.
3. Otherwise return `log->count` without modifying the log.

### `trace_log_get`

Purpose: borrow one stored record by zero-based logical index.

Implementation order:

1. If `log == NULL`, records is null, capacity is zero, count exceeds capacity,
   or `index >= count`, return `NULL`.
2. Otherwise return the borrowed address of `log->records[index]`.

### `trace_log_clear`

Purpose: discard the logical history while retaining its allocation and enabled
state for reuse.

Implementation order:

1. If `log == NULL`, return without action.
2. If records is null, capacity is zero, or count exceeds capacity, return
   without modifying the malformed log.
3. Set count to zero and next sequence to `1`.
4. Preserve the record allocation, capacity, enabled state, and existing bytes
   outside the now-empty logical range.

### `trace_log_free`

Purpose: release the record array and trace-log object owned by the caller.

Implementation order:

1. If `log == NULL`, return without action.
2. Free the record array, then free the log.

## Emission Boundary

Scheduler and protocol modules construct and emit one record in this order:

1. Declare a function-local `TraceRecord`.
2. Call `trace_record_init` with the exact action and current simulated
   timestamp. Stop trace construction if it fails.
3. When an event caused the observation, store its executable `event_type`,
   timestamp in `event_timestamp`, and sequence in `event_sequence`; otherwise
   preserve the initializer's event sentinel and zero event fields.
4. Store end timestamp, protocol, result, and copied names/summary required by
   that action. Any copied text uses its destination array size and remains
   NUL-terminated.
5. If a packet is associated, call `trace_record_capture_packet`. Do not append
   the record when capture reports invalid packet state.
6. Pass the complete record through `simulator_trace_emit`. The caller reports
   observation failure through simulator trace state but continues the existing
   networking operation exactly as its own specification requires.

Display modules call only `trace_log_count` and `trace_log_get`. They do not
modify records or retain returned pointers across append, clear, or free.

Do not store formatted pointer values, rely on object addresses as identity, or
retain borrowed object pointers in the log.

## ACSL Contracts

The header defines predicates equivalent to these literal bounds. Literal
values are used because the verification toolchain does not reliably expand C
macros inside ACSL comments.

```c
/*@
  predicate trace_action_valid(integer action) =
      1 <= action && action <= 26;

  predicate trace_event_type_valid(integer event_type) =
      0 <= event_type && event_type <= 27;

  predicate trace_result_valid(integer result) =
      result == -1 || result == 0 || result == 1;

  predicate trace_action_result_valid(integer action, integer result) =
      ((action == 2 || action == 3 || action == 25) ==> result == 0) &&
      ((action == 11 || action == 16 || action == 20) ==>
          (result == -1 || result == 1)) &&
      (action == 8 ==> result == -1) &&
      (!(action == 2 || action == 3 || action == 8 || action == 11 ||
          action == 16 || action == 20 || action == 25) ==> result == 1);

  predicate trace_record_input_valid(TraceRecord *record) =
      \valid_read(record) &&
      trace_action_valid(record->action) &&
      trace_event_type_valid(record->event_type) &&
      0 <= record->layer && record->layer <= 4 &&
      trace_result_valid(record->result) &&
      trace_action_result_valid(record->action, record->result) &&
      record->timestamp <= record->end_timestamp &&
      (record->action == 6 ||
          record->end_timestamp == record->timestamp) &&
      record->snapshot_length <= 256 &&
      record->snapshot_length <= record->packet_length &&
      record->snapshot_truncated ==
          (record->snapshot_length < record->packet_length ? 1 : 0) &&
      (record->packet_id == 0 || record->trace_id != 0) &&
      (record->parent_packet_id == 0 ||
          (record->packet_id != 0 && record->trace_id != 0)) &&
      (record->event_type != 27 ||
          (record->event_timestamp == 0 && record->event_sequence == 0)) &&
      (record->event_type == 27 || record->event_sequence != 0) &&
      ((record->action == 1 || record->action == 2 ||
          record->action == 3) ==> record->event_type < 27) &&
      (((4 <= record->action && record->action <= 10) ||
          record->action == 19 || record->action == 23) ==>
          (record->packet_id != 0 && record->trace_id != 0));

  predicate trace_log_well_formed(TraceLog *log) =
      \valid(log) && log->records != \null &&
      log->capacity > 0 && log->count <= log->capacity &&
      (log->enabled == 0 || log->enabled == 1) &&
      log->next_sequence > 0 &&
      \valid(log->records + (0 .. log->capacity - 1)) &&
      (\forall integer i; 0 <= i && i < log->count ==>
          trace_record_input_valid(&log->records[i]) &&
          log->records[i].sequence > 0 &&
          log->records[i].source_device[31] == '\0' &&
          log->records[i].source_iface[31] == '\0' &&
          log->records[i].destination_device[31] == '\0' &&
          log->records[i].destination_iface[31] == '\0' &&
          log->records[i].summary[127] == '\0') &&
      (\forall integer i; 0 <= i && i < log->count ==>
          log->records[i].sequence < log->next_sequence) &&
      (\forall integer i, j;
          0 <= i && i < j && j < log->count ==>
          log->records[i].sequence < log->records[j].sequence);
*/
```

The real header contracts additionally state:

- `trace_log_create`: zero/overflow capacity returns null; success returns a
  well-formed enabled log with zero count, exact capacity, and sequence one.
- `trace_record_init`: invalid pointer/action returns `-1`; success assigns the
  complete record and establishes every initializer postcondition.
- `trace_log_append`: null/malformed input returns `-1`; disabled success
  assigns nothing; enabled success increments count and next sequence once,
  stores a valid record at the old count, and preserves earlier records;
  failure preserves logical contents, count, capacity, and sequence.
- `trace_record_capture_packet`: assigns only packet-derived fields and the
  snapshot array; null packet produces the documented empty capture; invalid
  packet produces `-1` with empty packet-derived fields.
- count/get operations assign nothing. A successful get result points to one
  record inside the current logical range.
- clear preserves records pointer, capacity, and enabled state while setting
  count to zero and next sequence to one.
- free permits null and does not claim ownership of anything referenced only by
  copied record values.

Allocator success, `realloc` movement, stream consumers, and byte-for-byte
snapshot copying remain dynamic proof/test obligations.

## KLEVA Verification Plan

Generate cases for:

1. create with zero, multiplication-overflow, allocation-failure, and valid
   capacities
2. initializer null, unspecified action, count sentinel, and every valid action
3. append null arguments, malformed log shape, disabled log, and every invalid
   record invariant
4. append without growth, successful growth, capacity-doubling overflow,
   allocation-byte overflow, and failed `realloc`
5. trace-sequence assignment, caller trace-sequence overwrite, preservation of
   event sequence/timestamp, trace-sequence exhaustion, and clear/reuse from
   sequence one
6. forced NUL termination of all five copied text arrays
7. null packet capture, invalid packet geometry, zero-length valid packet,
   complete snapshot, exactly-256-byte snapshot, and truncated snapshot
8. invalid packet/trace IDs and invalid layer
9. packet clone and reply inheritance preserving trace and parent identity
10. count/get null, malformed, in-range, and out-of-range behavior
11. returned-record validity after the live packet and event have been freed
12. disabled tracing and append failure leaving packet delivery, event order,
    protocol state, and routing decisions unchanged

## Common Mistakes

- Do not use `memset(record, 0, ...)` as a substitute for
  `trace_record_init`; zero is a real event type and an invalid trace action.
- Logging a `Packet *` and attempting to render it after ownership transfer.
- Reusing event sequence as trace-record sequence; they order different
  collections.
- Retaining a pointer returned by `trace_log_get` across an append that may
  reallocate the record array.
- Formatting a pointer as identity instead of copying stable IDs and names.
- Treating trace append failure as permission to change network behavior.
- Appending a lookup, drop, or completed action without setting its required
  `TraceResult` value.
- Copying `pkt->capacity` bytes instead of the validated visible packet view.
