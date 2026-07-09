# Module 21 - Static Route

**Files:** `src/routing/static_route.c`, `src/routing/static_route.h`
**Depends on:** `router`, `route_table`, `interface`

## Concepts First

Static routing means the route is configured by the simulator user or scenario,
not learned from packets sent by another router.

RIP, OSPF, BGP, and IS-IS are dynamic routing protocols. They own
protocol state, parse protocol messages, run timers, and learn routes from
neighbors.

Static routing is different:

```text
The user says:
  for 172.16.0.0/16, send to next hop 10.0.0.2 through iface eth0

The simulator stores:
  one configured static-route record

The router installs:
  one ROUTE_PROTO_STATIC candidate in the RouteTable RIB
```

Static routing is not a packet protocol in this simulator. It has no wire
format, no receive function, no UDP/TCP/IP protocol number, and no periodic
timer. It is still a first-class route source because configured static routes
must compete with RIP/OSPF/BGP routes in the route table.

### Why A Separate Module?

The route table already knows how to store a `ROUTE_PROTO_STATIC` route
candidate, but that is not enough architecture.

The route table owns RIB/FIB selection. It should not own user configuration.

The static-route module owns the configured static routes:

```text
StaticRouteTable
  |
  +-- configured static route records
        |
        +-- installed into Router/RouteTable as ROUTE_PROTO_STATIC
```

This separation matters later when config files, CLI commands, route
persistence, route validation policy, and many static-route objects exist.

### Configured Route Versus Installed Route

There are two objects involved:

| Object | Owner | Meaning |
| --- | --- | --- |
| `StaticRouteEntry` | Static route module | The configured static route record. |
| `RouteRibEntry` | RouteTable | The route candidate used for best-route selection. |

Adding a static route creates or updates the configured record, then installs
the matching `ROUTE_PROTO_STATIC` route into the Router's route table.

Deleting a static route removes the configured record and deletes the matching
`ROUTE_PROTO_STATIC` route from the Router's route table.

### Static Route Key

In the first implementation, one static route is allowed per destination
prefix:

```text
key = normalized prefix + prefix_len
```

The key does not include next hop, interface, or metric.

If the user adds the same prefix again, the static-route module updates the
existing configured route and updates the installed `ROUTE_PROTO_STATIC` RIB
candidate.

This matches the current RouteTable behavior: `route_table_add` treats
`prefix + prefix_len + proto` as the duplicate key. Because all static routes
use `ROUTE_PROTO_STATIC`, the route table cannot represent multiple static
next hops for the same prefix yet.

ECMP and multiple static next hops are future behavior, not part of this
module's first implementation.

### Prefix Normalization

Static route input may contain host bits:

```text
192.168.1.77/24
```

The configured route must store the normalized prefix:

```text
192.168.1.0/24
```

The static-route module should normalize before searching its own configured
route table. RouteTable also normalizes when routes are installed, but the
static-route module must not keep duplicate config entries only because input
host bits were different.

### Direct Versus Next-Hop Static Route

Direct static route:

```text
prefix 192.168.1.0/24
next_hop 0
iface eth0
```

The router should ARP for the final packet destination.

Next-hop static route:

```text
prefix 172.16.0.0/16
next_hop 10.0.0.2
iface eth0
```

The router should ARP for `10.0.0.2`.

The static-route module only stores and installs the route. Router forwarding
owns ARP target selection.

### Administrative Distance

Static routes must use `ROUTE_PROTO_STATIC` when installed into RouteTable.

RouteTable gives static routes administrative distance `1`. That means:

- direct routes beat static routes
- static routes beat RIP, OSPF, BGP, and IS-IS for the same prefix
- metric is compared only between candidates with equal administrative distance

The static-route module must not reimplement route selection.

## Purpose

The static-route module stores configured static routes and installs/removes
them from a Router's route table as `ROUTE_PROTO_STATIC`.

It provides:

- fixed-capacity static route configuration storage
- static route add/update
- static route delete
- reapply all configured static routes to a Router route table
- flush all configured static routes

It does not:

- forward packets
- send ARP
- parse IP packets
- run routing-protocol timers
- parse CLI text
- parse topology config files
- choose active routes against RIP/OSPF/BGP
- write directly into RouteTable arrays

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Store configured static routes | StaticRouteTable |
| Normalize static-route config keys | Static route module |
| Store route candidates | RouteTable RIB |
| Select active route | RouteTable FIB rebuild |
| Perform forwarding lookup | RouteTable lookup |
| Forward packets and resolve ARP target | Router |
| Parse user CLI/config | Future CLI/config layer |

Static route code must call Router public APIs:

```c
router_add_route(router,
                 prefix,
                 prefix_len,
                 next_hop,
                 iface,
                 metric,
                 ROUTE_PROTO_STATIC);
```

and:

```c
router_del_route(router, prefix, prefix_len, ROUTE_PROTO_STATIC);
```

It must not write `router->route_tbl.rib[]` or `router->route_tbl.fib[]`
directly.

## Data Model

### Constants

```c
#define STATIC_ROUTE_MAX_ROUTES 128
```

### `StaticRouteEntry`

```c
typedef struct StaticRouteEntry {
    uint32_t   prefix;
    uint8_t    prefix_len;
    uint8_t    valid;
    uint8_t    installed;
    uint8_t    _pad;
    uint32_t   next_hop;
    Interface *iface;
    uint32_t   metric;
} StaticRouteEntry;
```

Field meaning:

- `prefix`: host-order normalized IPv4 prefix.
- `prefix_len`: CIDR prefix length, `0..32`.
- `valid`: `1` when this configured static-route slot is used.
- `installed`: `1` when this configured route has been successfully installed
  into the Router route table.
- `next_hop`: host-order next-hop IPv4 address; `0` means directly connected.
- `iface`: borrowed egress interface pointer.
- `metric`: static route metric used inside RouteTable when comparing static
  routes with equal administrative distance.

### `StaticRouteTable`

```c
typedef struct StaticRouteTable {
    StaticRouteEntry routes[STATIC_ROUTE_MAX_ROUTES];
    int              count;
} StaticRouteTable;
```

`count` is the number of valid configured static-route records.

## Ownership Model

The owner allocates `StaticRouteTable`; the static-route module initializes it.

The static-route module borrows:

- `Router *`
- `Interface *`

It does not free routers or interfaces.

The `iface` pointer stored in `StaticRouteEntry` must be cleared when the entry
is deleted or flushed. Clearing the pointer means setting the stored borrowed
pointer field to `NULL`; it does not mean clearing or modifying the `Interface`
object itself.

## Function Behavior

Ordered lists in this section are implementation order. Final-state facts go
under Postconditions.

### `static_route_init`

Behavior summary:

Initialize a caller-owned `StaticRouteTable` so it contains no configured static
routes.

Implementation order:

- If `table == NULL`, return immediately.
- Clear the entire `StaticRouteTable` object.

Postconditions after valid initialization:

- `table->count == 0`.
- Every `routes[i].valid == 0`.
- Every `routes[i].installed == 0`.
- Every `routes[i].iface == NULL`.

### `static_route_add`

Behavior summary:

Add a new configured static route or update the existing configured route for
the same normalized prefix and prefix length. A successful add/update also
installs the route into the Router route table as `ROUTE_PROTO_STATIC`.

Implementation order:

- If `table == NULL || router == NULL || iface == NULL`, return `-1`.
- If `prefix_len > 32`, return `-1`.
- Compute `normalized_prefix` from `prefix` and `prefix_len`.
- Search `table->routes[0 .. STATIC_ROUTE_MAX_ROUTES - 1]` for a valid entry
  whose `prefix == normalized_prefix` and `prefix_len == prefix_len`.
- If a matching configured static-route entry exists:
  - call `router_add_route(router, normalized_prefix, prefix_len, next_hop,
    iface, metric, ROUTE_PROTO_STATIC)`
  - if that call fails, return `-1` and leave the configured static-route entry
    unchanged
  - set the configured entry's `next_hop`, `iface`, `metric`, and `installed`
    fields from the new input
  - return `0`
- If no matching configured static-route entry exists:
  - search `table->routes[0 .. STATIC_ROUTE_MAX_ROUTES - 1]` for the first
    invalid slot
  - if no invalid slot exists, return `-1`
  - call `router_add_route(router, normalized_prefix, prefix_len, next_hop,
    iface, metric, ROUTE_PROTO_STATIC)`
  - if that call fails, return `-1` and leave the invalid slot unused
  - write the new configured route into that slot
  - set `valid = 1`
  - set `installed = 1`
  - increment `table->count` once
  - return `0`

Postconditions after success:

- Exactly one configured static-route entry exists for
  `normalized_prefix/prefix_len`.
- That entry stores the input `next_hop`, `iface`, and `metric`.
- The Router route table has been asked to install the same route as
  `ROUTE_PROTO_STATIC`.

### `static_route_delete`

Behavior summary:

Remove one configured static route identified by normalized prefix and prefix
length. If the route was installed, also remove the matching
`ROUTE_PROTO_STATIC` route from the Router route table.

Implementation order:

- If `table == NULL || router == NULL`, return `-1`.
- If `prefix_len > 32`, return `-1`.
- Compute `normalized_prefix` from `prefix` and `prefix_len`.
- Search `table->routes[0 .. STATIC_ROUTE_MAX_ROUTES - 1]` for a valid entry
  whose `prefix == normalized_prefix` and `prefix_len == prefix_len`.
- If no matching configured static-route entry exists, return `-1`.
- If the matching entry has `installed == 1`, call:

```c
router_del_route(router, normalized_prefix, prefix_len, ROUTE_PROTO_STATIC);
```

- Clear the matching `StaticRouteEntry` slot.
- Decrement `table->count` once if it is greater than `0`.
- Return `0` if route-table removal succeeded or was not needed; otherwise
  return `-1`.

Postconditions after success:

- No configured static-route entry remains for `normalized_prefix/prefix_len`.
- The stored borrowed `iface` pointer in the deleted slot is `NULL`.

### `static_route_apply`

Behavior summary:

Install every valid configured static route into the Router route table. This
is useful after a Router route table is reinitialized or after static route
configuration is loaded before the Router is ready.

Implementation order:

- If `table == NULL || router == NULL`, return `-1`.
- Set local `applied` count to `0`.
- Scan `table->routes[0 .. STATIC_ROUTE_MAX_ROUTES - 1]`.
- For each invalid entry, continue to the next slot.
- For each valid entry, call `router_add_route` with the entry's stored
  `prefix`, `prefix_len`, `next_hop`, `iface`, `metric`, and
  `ROUTE_PROTO_STATIC`.
- If any install call fails:
  - set that entry's `installed = 0`
  - return `-1`
- If install succeeds:
  - set that entry's `installed = 1`
  - increment `applied`
- After the scan, return `applied`.

Postconditions after success:

- Every valid configured static route has `installed == 1`.
- The return value is the number of valid static routes applied.

### `static_route_flush`

Behavior summary:

Remove every configured static route from the static-route table. For each
installed entry, ask Router to delete the matching `ROUTE_PROTO_STATIC` route.

Implementation order:

- If `table == NULL || router == NULL`, return `-1`.
- Set local `removed` count to `0`.
- Scan `table->routes[0 .. STATIC_ROUTE_MAX_ROUTES - 1]`.
- For each invalid entry, continue to the next slot.
- For each valid entry:
  - if `installed == 1`, call `router_del_route(router, entry->prefix,
    entry->prefix_len, ROUTE_PROTO_STATIC)`
  - clear the `StaticRouteEntry` slot
  - increment `removed`
- Set `table->count = 0`.
- Return `removed`.

Postconditions after success:

- `table->count == 0`.
- Every `routes[i].valid == 0`.
- Every `routes[i].installed == 0`.
- Every `routes[i].iface == NULL`.

## ACSL Contracts

The header contracts should describe:

- null inputs return safely
- init clears all configured route slots
- add may modify static-route storage and Router route-table storage
- delete may modify static-route storage and Router route-table storage
- apply may modify installed flags and Router route-table storage
- flush clears static-route storage and may modify Router route-table storage

Because `static_route_add`, `static_route_delete`, `static_route_apply`, and
`static_route_flush` call Router APIs, their assigns clauses must include the
Router route-table fields those APIs can modify.

Do not write contracts that pretend these functions only modify
`StaticRouteTable`.

## Flow

Add/update:

```text
static_route_add
  |
  +-- validate table/router/iface/prefix_len
  +-- normalize prefix
  +-- search configured static-route table for same prefix/prefix_len
  |
  +-- match found:
  |     install/update ROUTE_PROTO_STATIC in Router route table
  |     update existing StaticRouteEntry fields
  |
  +-- no match:
        find invalid StaticRouteEntry slot
        install ROUTE_PROTO_STATIC in Router route table
        write new StaticRouteEntry
```

Delete:

```text
static_route_delete
  |
  +-- validate table/router/prefix_len
  +-- normalize prefix
  +-- find matching configured StaticRouteEntry
  +-- delete ROUTE_PROTO_STATIC from Router route table
  +-- clear StaticRouteEntry slot
```

## Error Handling

- Invalid pointers return `-1`.
- Invalid prefix length returns `-1`.
- Full static-route table returns `-1`.
- Router route-table install failure returns `-1`.
- Deleting a missing static route returns `-1`.
- `static_route_apply` stops at the first failed route install and returns `-1`.

## KLEVA Verification Plan

1. `static_route_init(NULL)` is safe.
2. Valid init clears count, valid bits, installed bits, and borrowed interface
   pointers.
3. Add rejects NULL table.
4. Add rejects NULL router.
5. Add rejects NULL interface.
6. Add rejects `prefix_len > 32`.
7. Add normalizes prefix before storing.
8. Add stores a new configured route in the first invalid slot.
9. Add increments count once for a new configured route.
10. Add with same normalized prefix/prefix_len updates the existing entry.
11. Duplicate add does not increment count.
12. Add returns `-1` when the static-route table is full.
13. Delete rejects NULL table.
14. Delete rejects NULL router.
15. Delete rejects `prefix_len > 32`.
16. Delete normalizes prefix before matching.
17. Delete missing route returns `-1`.
18. Delete existing route clears the slot and decrements count once.
19. Apply installs every valid configured route.
20. Apply skips invalid slots.
21. Flush clears every valid configured route.
22. Flush sets count to `0`.

## Common Mistakes

- Do not implement static routing as RIP/OSPF-style packet receive code.
- Do not add UDP/TCP/IP protocol handlers for static routes.
- Do not let StaticRouteTable replace RouteTable.
- Do not write directly into `router->route_tbl.rib[]`.
- Do not forget to normalize prefix before searching static-route config.
- Do not treat `iface` ownership as transferred; it is borrowed.
- Do not clear the `Interface` object when deleting a static route; only clear
  the stored pointer field.
- Do not claim multiple static next hops for the same prefix work until
  RouteTable supports that duplicate key.
