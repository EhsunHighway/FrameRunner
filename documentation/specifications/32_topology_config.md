# Module 32 - Topology Configuration

**Files:** `src/config/topology_config.c`, `src/config/topology_config.h`
**Status:** Ready for implementation; files do not exist yet
**Depends on:** `simulator`, `scheduler`, `topology`, `device`, `host`,
`router`, `switch`, `interface`, `link`, `static_route`, `byte_order`

## Concepts First

A topology file is a declarative description, not a sequence of constructor
calls. Records may refer to names declared later, so parsing and construction
are separate phases. Parsing produces temporary host-order records; semantic
resolution connects their names; construction then calls the public APIs of
the modules that own the real runtime objects.

Atomic loading means failure never leaves the caller with half a network. A
new simulator becomes visible only after every declaration has been validated
and every runtime object has been constructed successfully.

## Purpose

The topology-configuration module converts a text scenario into a completely
owned `Simulator`. It creates the scheduler and topology, creates every
declared network object through the owning module's public API, connects
interfaces, and installs declared static routes.

This module parses network meaning only. Configuration files do not contain
terminal coordinates, animation frames, colors, or other display placement.
The automatic topology-layout module derives presentation from the completed
topology graph.

## Architecture Boundary

`Topology` remains storage and lookup. It does not parse files or create
devices. The loader is an orchestration layer above the existing modules:

```text
text file
   |
   v
topology_config
   |-- creates Scheduler, Topology, Simulator
   |-- creates Host, Router, Switch and Interface objects
   |-- creates and attaches Link objects
   `-- installs static routes
```

On success, the returned `Simulator` owns its scheduler and topology; the
topology owns its devices and links; devices own their interfaces. On failure,
the loader releases every object created during that attempt.

## Ownership And Lifetime

- The loader borrows `FILE *in`, `FILE *err`, and `const char *path`.
- Temporary token strings and parsed records are owned by the loader call.
- On success, ownership of the completed simulator transfers through
  `*out_sim`.
- On failure, `*out_sim` remains `NULL` and the loader owns and releases every
  temporary or runtime object created during that attempt.
- The path wrapper owns the file stream it opens and closes it on every path.

## First-Milestone Grammar

- Input is UTF-8-compatible ASCII text read one line at a time.
- `#` begins a comment extending to the end of the line.
- Empty and comment-only lines are ignored.
- Tokens are separated by spaces or tabs.
- Quoted tokens and escaped whitespace are not supported in the first
  milestone. Names therefore cannot contain whitespace.
- Keywords are lowercase and case-sensitive.
- Device and interface names are case-sensitive.
- Visual coordinates are not part of the grammar.

Accepted records:

```text
host <name> <default-gateway-ip>
router <name>
switch <name>
interface <device> <iface> <mac> <ipv4>/<prefix> [mtu]
link <device>:<iface> <device>:<iface> <bandwidth-mbps> <delay-ms> <loss-rate>
route <router> <prefix>/<length> <next-hop> <iface> <metric>
```

Example:

```text
# two-host, two-router scenario
host H1 10.0.0.1
router R1
router R2
host H2 10.0.2.1

interface H1 eth0 02:00:00:00:00:01 10.0.0.2/24
interface R1 eth0 02:00:00:00:00:02 10.0.0.1/24
interface R1 eth1 02:00:00:00:00:03 10.0.1.1/24
interface R2 eth0 02:00:00:00:00:04 10.0.1.2/24
interface R2 eth1 02:00:00:00:00:05 10.0.2.1/24
interface H2 eth0 02:00:00:00:00:06 10.0.2.2/24

link H1:eth0 R1:eth0 1000 1 0.0
link R1:eth1 R2:eth0 1000 5 0.0
link R2:eth1 H2:eth0 1000 1 0.0

route R1 10.0.2.0/24 10.0.1.2 eth1 1
route R2 10.0.0.0/24 10.0.1.1 eth0 1
```

Protocol-specific configuration records are a later extension. The initial
runnable scenario uses connected and static routes; RIP and OSPF can be enabled
through later grammar additions after their initialization ownership is wired
through the loader.

## Validation Rules

- Device names must be nonempty, fit the current device-name capacity, and be
  unique.
- An interface record must reference an existing device record after semantic
  resolution, and interface names must be unique within that device.
- MAC text must contain exactly six hexadecimal octets.
- IPv4 and prefix text must be complete; prefix length must be `0 .. 32`.
- Optional MTU defaults to the simulator's normal interface MTU and must be
  nonzero when present.
- A link endpoint must identify an existing interface.
- One interface may participate in at most one link.
- Link endpoints must differ, bandwidth must be nonzero, delay must fit
  `uint32_t`, and loss rate must be within `0.0 .. 1.0`.
- A route must reference an existing router and one of that router's
  interfaces. Prefix length must be `0 .. 32`; metric must fit the route API.
- Unknown keywords, missing tokens, extra tokens, numeric overflow, duplicate
  declarations, and unresolved references are errors.
- The parser reports the one-based source line number for every rejected
  record.

Records may appear in any order. The loader first parses declarations into
temporary configuration records, then resolves and builds them in dependency
order. A user does not have to place every device before its interfaces or
every interface before its links.

## Public API

```c
int topology_config_load(FILE       *in,
                         FILE       *err,
                         Simulator **out_sim);

int topology_config_load_path(const char *path,
                              FILE       *err,
                              Simulator **out_sim);
```

`in`, `err`, and `path` are borrowed. `err` may be `NULL` to suppress diagnostic
text. On success, the caller owns `*out_sim` and releases it with
`simulator_free`.

## Function Behavior

### `topology_config_load`

Required behavior and implementation order:

1. If `in == NULL || out_sim == NULL`, return `-1` without allocating.
2. Set `*out_sim = NULL` before parsing.
3. Initialize an empty function-owned collection of parsed configuration
   records and set `line_number = 0`.
4. Read one bounded line at a time. Reject a physical line that does not fit in
   the configured line buffer; do not silently parse a truncated prefix.
5. Increment `line_number`, remove the comment suffix, trim surrounding
   whitespace, and skip an empty result.
6. Tokenize the writable line buffer, select the exact record grammar from its
   first token, validate token count and scalar syntax, and append a temporary
   host-order record. Do not create simulator objects during this pass.
7. After the complete file is syntactically parsed, validate unique names,
   endpoint references, link participation, route owners, and every other
   cross-record rule. On failure, print one line-numbered diagnostic when
   `err != NULL`, release temporary records, and return `-1`.
8. Create a `Topology`, then a `Scheduler`, then a `Simulator`. If any creation
   fails, release the objects already created, release temporary records, and
   return `-1`.
9. Create devices in source-record order. Add each to the new topology only
   after its constructor succeeds. Respect each constructor's ownership
   transfer and failure rules.
10. Create interfaces in source-record order, convert textual addresses into
    the storage order required by `Interface`, and attach each interface
    through its device-type-specific public API. If attachment fails, free the
    still caller-owned interface.
11. Create links in source-record order. Attach the new link to both endpoints,
    then transfer it to the topology. If a later ownership step fails, undo
    only still-caller-owned work before destroying the entire temporary
    simulator.
12. Install static routes in source-record order using host-order prefix and
    next-hop values at the routing API boundary.
13. Release all temporary parsing records.
14. Store the completed simulator in `*out_sim` and return `0`.

No failure after step 8 may publish a partially built simulator through
`out_sim`.

### `topology_config_load_path`

Implementation order:

1. If `path == NULL || out_sim == NULL`, return `-1`.
2. Set `*out_sim = NULL`.
3. Open `path` for text reading. On failure, report through `err` when present
   and return `-1`.
4. Call `topology_config_load` with the opened stream.
5. Close the stream regardless of loader success.
6. Return the loader result without changing its ownership outcome.

## Verification Expectations

Tests must cover empty input, comments, forward references, every record type,
duplicate names, malformed addresses, unresolved endpoints, duplicate links,
invalid loss rate, route validation, allocation/build failure cleanup, and a
complete scenario that can be passed to `simulator_free` without leaks.

## ACSL Contract Targets

Header contracts must express null-input rejection, `*out_sim == NULL` on
failure, publication of one valid simulator only on success, and the fact that
input/error streams are borrowed rather than closed by `topology_config_load`.
File contents and allocator cleanup remain dynamic-test obligations.

## Common Mistakes

- Creating devices during the syntax pass, which makes forward references and
  atomic failure cleanup difficult.
- Publishing `*out_sim` before routes and links have finished construction.
- Treating configuration order as constructor order.
- Storing address text or network-order wire values in host-order runtime
  fields without using the owning API's boundary.
- Adding display positions to the grammar or runtime network structs.
