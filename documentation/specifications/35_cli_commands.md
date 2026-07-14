# Module 35 - CLI Built-In Commands

**Files:** `src/cli/commands.c`, `src/cli/commands.h`
**Status:** Ready for implementation; source files are currently empty
**Depends on:** `cli`, `topology_config`, `simulator`, `scheduler`, `animation`,
`topology_view`, `event_log`, `device`, `interface`, `link`, `arp_cache`,
`route_table`, `icmp`

## Concepts First

The CLI core knows how to dispatch a command but not what a command means.
This module owns the built-in vocabulary and translates validated text
arguments into calls to the public API that owns the requested behavior.

Inspection commands borrow state and print it. Control commands advance the
scheduler through simulator/animation APIs. Mutation commands validate every
reference before changing state. No handler reaches into protocol-private
storage merely because the CLI can see its address.

## Purpose

The module registers and implements the simulator's built-in commands. It does
not own `CliState`, parse command-name prefixes, implement networking
algorithms, or render packet bytes itself.

## Public API

```c
int commands_register_builtins(CliState *state);

int cmd_show_topology(CliState *state, int argc, char **argv);
int cmd_show_interfaces(CliState *state, int argc, char **argv);
int cmd_show_arp(CliState *state, int argc, char **argv);
int cmd_show_route(CliState *state, int argc, char **argv);
int cmd_ping(CliState *state, int argc, char **argv);
int cmd_set_link(CliState *state, int argc, char **argv);
int cmd_add_route(CliState *state, int argc, char **argv);
int cmd_step(CliState *state, int argc, char **argv);
int cmd_tick(CliState *state, int argc, char **argv);
int cmd_run_time(CliState *state, int argc, char **argv);
int cmd_run_events(CliState *state, int argc, char **argv);
int cmd_stop(CliState *state, int argc, char **argv);
int cmd_load(CliState *state, int argc, char **argv);
int cmd_watch(CliState *state, int argc, char **argv);
int cmd_speed(CliState *state, int argc, char **argv);
int cmd_focus_all(CliState *state, int argc, char **argv);
int cmd_focus_auto(CliState *state, int argc, char **argv);
int cmd_focus_device(CliState *state, int argc, char **argv);
int cmd_focus_packet(CliState *state, int argc, char **argv);
int cmd_focus_trace(CliState *state, int argc, char **argv);
int cmd_show_packet(CliState *state, int argc, char **argv);
int cmd_show_trace(CliState *state, int argc, char **argv);
int cmd_help(CliState *state, int argc, char **argv);
int cmd_exit(CliState *state, int argc, char **argv);
```

Handlers receive only arguments after the registered command name. They return
`0` for success, `1` for usage error, and `-1` for runtime failure.

## Built-In Registry

Register these exact names and usages in this exact order:

| Name | Usage |
|---|---|
| `show topology` | `show topology` |
| `show interfaces` | `show interfaces [device]` |
| `show arp` | `show arp [device]` |
| `show ip route` | `show ip route [device]` |
| `show packet` | `show packet <packet_id>` |
| `show trace` | `show trace <trace_id>` |
| `ping` | `ping <src_device> <dst_ip> [count]` |
| `set link` | `set link <device>:<iface> up|down` |
| `add route` | `add route <router> <prefix/length> <next-hop> <iface> <metric>` |
| `step` | `step [count]` |
| `tick` | `tick [count]` |
| `run time` | `run time <duration_us>` |
| `run events` | `run events <count>` |
| `stop` | `stop` |
| `load` | `load <file>` |
| `watch` | `watch on|off` |
| `speed` | `speed <factor>` |
| `focus all` | `focus all` |
| `focus auto` | `focus auto` |
| `focus device` | `focus device <name>` |
| `focus packet` | `focus packet <packet_id>` |
| `focus trace` | `focus trace <trace_id>` |
| `help` | `help [command]` |
| `exit` | `exit` |

There is no unbounded `run` command because periodic RIP and OSPF timers may
keep the scheduler nonempty forever.

## Shared Handler Rules

1. Reject null state, simulator, required animation/output object, or an
   inconsistent `argc`/`argv` pair with `-1`.
2. Return `1` for the wrong number of arguments or syntactically invalid text.
3. Parse unsigned integers using a conversion that detects empty input,
   trailing bytes, sign characters, and overflow.
4. Look up names through topology/device public lookup APIs.
5. Print a specific runtime diagnostic before returning `-1` for an unknown
   object or unavailable subsystem.
6. Do not retain any handler argument pointer after returning.

## Function Behavior

### `commands_register_builtins`

1. Reject null state with `-1`.
2. Save the initial command count.
3. Register every row in Built-In Registry order with `cli_register`.
4. If one registration fails, restore command count to its initial value so no
   partial built-in set remains, then return `-1`.
5. Return `0` after every registration succeeds.

### `cmd_show_topology`

1. Require zero arguments.
2. Pass `state->sim->topo` and `state->out` to `topology_view_print`.
3. Return `0` on renderer success or `-1` on renderer failure.

### `cmd_show_interfaces`

1. Accept zero or one device-name argument.
2. With a name, find that device and print its interfaces in increasing slot
   order; unknown name is a runtime error.
3. Without a name, scan topology devices in increasing index order and print
   each device followed by its interfaces.
4. Print name, MAC, IPv4/prefix, MTU, operational state, and connected peer for
   each interface using display/address helpers.

### `cmd_show_arp`

1. Accept zero or one device-name argument.
2. Select one named device or every topology device in increasing order.
3. For each selected device that owns an ARP cache, call the ARP-cache display
   API; explicitly state when a selected device has no cache.
4. Unknown named device or output failure returns `-1`.

### `cmd_show_route`

1. Accept zero or one device-name argument.
2. Select one named device or every device in topology order.
3. Print routing tables only for devices that own one; explicitly state when a
   specifically named device has no route table.
4. Preserve route-table order and use its public display/iteration API.

### `cmd_ping`

1. Require two or three arguments.
2. Resolve the source name to a host or router and parse destination IPv4 into
   the host-order representation required by ICMP.
3. Parse optional count as a positive bounded integer; default to one.
4. For each request, call the ICMP echo-send API exactly once and stop on its
   first failure.
5. Packet ownership follows the ICMP API; the command retains no packet.

### `cmd_set_link`

1. Require endpoint and state arguments.
2. Split endpoint at exactly one colon into nonempty device/interface names.
3. Resolve the device, interface, and attached link before mutation.
4. Accept only `up` or `down`; call the owning link/interface state API.
5. If requested state already holds, report success without generating a
   duplicate transition.

### `cmd_add_route`

1. Require five arguments.
2. Resolve the named router and its egress interface.
3. Parse prefix/length, next-hop IPv4, and metric completely with overflow
   checks.
4. Normalize prefix host bits according to prefix length.
5. Call the router/route-table public add API with host-order addresses,
   egress interface, metric, and static-route protocol.
6. Return `-1` when validation, duplicate policy, capacity, or installation
   fails; do not partially alter another route.

### `cmd_step`

1. Accept zero or one positive count; default to one.
2. Call `animation_step` at most count times.
3. Stop early when it returns zero because no event exists.
4. Propagate a negative result as `-1`; otherwise return `0`.

### `cmd_tick`

1. Accept zero or one positive count; default to one.
2. Call `animation_tick` once per requested tick.
3. Stop early when no next timestamp exists; return `-1` on animation failure.

### `cmd_run_time`

1. Require one positive simulated-microsecond duration.
2. Add it to current scheduler time with overflow detection.
3. Call `animation_run_until` with that absolute end time and no event-count
   limit.
4. Return `-1` on parse, overflow, or animation failure; otherwise return `0`.

### `cmd_run_events`

1. Require one positive count no greater than `INT_MAX`.
2. Call `animation_run_until` with no time limit and that event limit.
3. Return `-1` on animation failure; otherwise return `0` even when the queue
   empties before the limit.

### `cmd_stop`

1. Require zero arguments.
2. Call `simulator_stop(state->sim)` and return `0`.

### `cmd_load`

1. Require exactly one path.
2. Call `topology_config_load_path` into local `new_sim`; on failure preserve
   active state and return `-1`.
3. Call `cli_replace_simulator(state, new_sim)`.
4. If replacement fails, free `new_sim` because ownership did not transfer,
   preserve active state, and return `-1`.
5. Print the new device/link counts and return `0`.

### `cmd_watch`

1. Require exactly `on` or `off`.
2. Store one or zero in `watch_enabled`.
3. Do not disable tracing or change scheduler state.

### `cmd_speed`

1. Require one finite positive floating-point factor with no trailing bytes.
2. Call `animation_set_speed`; preserve the old speed on rejection.

### Focus handlers

- `cmd_focus_all` and `cmd_focus_auto` require zero arguments and call their
  matching animation functions.
- `cmd_focus_device` requires one name, resolves it before calling
  `animation_focus_device`, and preserves focus for an unknown name.
- Packet and trace focus require one positive `uint32_t` ID and call the
  matching animation function.
- Return `-1` if lookup or animation rejects the request; otherwise return `0`.

### `cmd_show_packet`

1. Require one positive `uint32_t` packet ID.
2. Call `event_log_print_packet(state->sim->trace, id, state->out)` and return
   its result.

### `cmd_show_trace`

1. Require one positive `uint32_t` trace ID.
2. Call `event_log_print_trace(state->sim->trace, id, state->out)` and return
   its result.

### `cmd_help`

1. With zero arguments, print every registered command name and usage in slot
   order.
2. With arguments, use `cli_find_command` on those words. Require the match to
   consume every supplied word; otherwise return `1`.
3. Print the selected usage and return `0`; output failure returns `-1`.

### `cmd_exit`

1. Require zero arguments.
2. Set `state->running = 0` and return `0`. Do not free state inside its handler.

## ACSL Contract Targets

Contracts must cover argument-count branches, complete numeric parsing,
registry rollback, no state mutation on lookup/parse failure, bounded run
arguments, and simulator ownership transfer in `cmd_load`.

Use literal numeric bounds in ACSL comments rather than project macros.

## KLEVA Verification Plan

Test every handler for wrong argument count, malformed numeric text, unknown
objects, downstream failure, output failure, and success. Add focused ownership
tests for topology load/replacement and state-preservation tests for failed
link, route, speed, and focus commands.

## Common Mistakes

- Registering built-ins inside `cli_create` and coupling CLI core to commands.
- Calling protocol internals instead of the owning public API.
- Implementing an unbounded run.
- Freeing `new_sim` after successful replacement or leaking it after failed
  replacement.
- Treating a trace ID as a packet ID.
