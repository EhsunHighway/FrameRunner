# Module 31 - Trace Event Log

**Files:** `src/display/event_log.c`, `src/display/event_log.h`
**Status:** Ready for implementation; current source files are empty
**Depends on:** `trace`

## Concepts First

`TraceLog` owns durable simulation history. The event-log module is a read-only
text renderer over that history. It does not append records, inspect live
packets, execute events, or decide what the simulator should do next.

Packet ID and trace ID select different histories:

- packet ID selects records for one allocated packet object
- trace ID selects the complete causal journey, including clones, requests,
  replies, errors, and retransmissions

## Purpose

The module prints recent trace records or records selected by packet or trace
identity. Its output is used by direct CLI inspection and by the animation's
recent-process panel.

## Architecture Boundary

The renderer receives a borrowed `const TraceLog *` and `FILE *`. It prints
only copied fields stored in `TraceRecord`. Protocol-specific decoding of a
packet snapshot remains the responsibility of `header_view`.

## Ownership And Lifetime

- The caller owns the trace log and output stream.
- Returned output contains values only; the module stores no pointers between
  calls.
- The module does not clear, reorder, grow, or free the trace log.

## Public API

```c
int event_log_print_recent(const TraceLog *log,
                           size_t          limit,
                           FILE           *out);

int event_log_print_packet(const TraceLog *log,
                           uint32_t        packet_id,
                           FILE           *out);

int event_log_print_trace(const TraceLog *log,
                          uint32_t        trace_id,
                          FILE           *out);
```

## Record Format

Every printed record contains, in this order:

1. simulated `timestamp`; include `end_timestamp` when it differs
2. trace-record `sequence`
3. action name
4. associated event type, `event_timestamp`, and `event_sequence` when
   `event_type != EVT_TYPE_COUNT`
5. packet ID and trace ID when nonzero
6. source and destination device/interface names when present
7. result and summary

Use one physical output line per record. Empty copied names are omitted rather
than printed as pointer values or guessed from live topology state.

## Function Behavior

### Shared validation and output rules

1. If `log == NULL || out == NULL`, return `-1` without output.
2. Scan only `records[0 .. count - 1]` and preserve stored sequence order.
3. If any output operation fails, stop and return `-1`.
4. Otherwise return `0`, including when the selected range has no records.

### `event_log_print_recent`

1. Apply the shared validation rules.
2. Calculate `start_index` without unsigned underflow: use zero when
   `limit >= log->count`; otherwise use `log->count - limit`.
3. When `limit == 0` or the log is empty, print an explicit `no trace records`
   result and return `0` unless output fails.
4. Print `records[start_index .. log->count - 1]` using the common record
   format.

### `event_log_print_packet`

1. Apply the shared validation rules.
2. Reject `packet_id == 0` with `-1` without scanning.
3. Scan the complete logical record range in increasing index order.
4. Print a record only when `record->packet_id == packet_id`.
5. If no record matched, print an explicit result naming the requested packet
   ID.

### `event_log_print_trace`

1. Apply the shared validation rules.
2. Reject `trace_id == 0` with `-1` without scanning.
3. Scan the complete logical record range in increasing index order.
4. Print a record only when `record->trace_id == trace_id`.
5. If no record matched, print an explicit result naming the requested trace
   ID.

## ACSL Contract Targets

Contracts must express null rejection, read-only access to the logical record
range, no mutation of `TraceLog`, and zero-ID rejection for identity queries.
Stream-write success remains a dynamic-test obligation.

## KLEVA Verification Plan

Tests cover null arguments, empty logs, zero and oversized recent limits,
single and multiple matches, no-match output, packet-versus-trace selection,
stored trace-sequence order, associated event timestamp/sequence, no-event
sentinel handling, optional names, interval timestamps, and output failure.

## Common Mistakes

- Treating an event log as another owner of trace records.
- Following device or packet pointers instead of using copied record fields.
- Sorting selected records again and thereby changing stored order.
- Treating packet ID and trace ID as interchangeable.
- Returning an error merely because a valid selector found no records.
