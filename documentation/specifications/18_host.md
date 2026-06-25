# Module 18 - Host

**Files:** `src/network/host.c`, `src/network/host.h`
**Status:** Ready for implementation
**Depends on:** `device`, `interface`, `arp_cache`, `arp`, `ip`, `icmp`, `udp`, `tcp`, `simulator`

## Concepts First

This file is an implementation specification. It must explain the concepts
before it lists functions. Host is mostly glue code, so vague words are
dangerous: a wrong interpretation of ownership or context wiring changes the
whole stack.

For this module:

- **Host** means one endpoint machine in the simulator.
- **Per-host state** means state that belongs to one Host object, not to the
  whole simulator.
- **IP dispatch table** means the table inside one `IpStack` that maps an IPv4
  protocol number to a receive handler and a context pointer.
- **Handler** means a function pointer that IP remembers and calls later when a
  matching packet arrives.
- **Context** means the `void *` value IP stores beside a handler and passes
  back to that handler later.
- **Registering a handler** means writing one entry in the IP dispatch table. It
  does not mean calling the handler during registration.

The key words `MUST`, `MUST NOT`, `SHOULD`, and `MAY` are used with their normal
specification meaning: `MUST` is required, `MUST NOT` is forbidden, `SHOULD` is
the default unless there is a documented reason, and `MAY` is optional.

## Purpose

A Host turns the lower network pieces into one usable endpoint:

- it owns a `Device` base so it can have interfaces and links
- it owns one ARP cache for its interfaces
- it owns one IP stack for receiving IPv4 packets
- it owns one UDP state table
- it owns one TCP connection table
- it stores an optional default gateway address for future routing work

A Host is not a switch and not a router. It MUST NOT forward packets between
interfaces. It sends packets created by local protocols and receives packets
addressed to one of its local interfaces.

## Architecture Boundary

Host owns and connects state. It does not implement protocol internals.

| Responsibility | Owner |
| --- | --- |
| Interface list and interface ownership | `Device` embedded in `Host` |
| Neighbor cache and pending ARP packets | Host-owned `ArpCache` |
| IPv4 receive dispatch table | Host-owned `IpStack` |
| ICMP packet parsing and ICMP replies | ICMP module |
| UDP socket table | Host-owned `UdpState`, used by UDP module |
| TCP connection table | Host-owned `TcpTable`, used by TCP module |
| Choosing outgoing interface and resolving ARP | IP/ARP output path |
| Ethernet destination MAC choice | IP/ARP/Ethernet path, not Host |
| Packet forwarding between interfaces | Router, not Host |

Host MAY call module initialization and registration functions. Host MUST NOT
inspect UDP sockets, TCP TCBs, IPv4 wire headers, or ARP packet contents except
through the public APIs of those modules.

## State Model

`Host` MUST embed `Device` as its first field. This allows code that already
works with `Device *` to use a `Host *` through its first field when connecting
interfaces and links.

This specification follows the current `host.h` storage model: protocol state
members are pointers. A successful `host_create` MUST leave these pointers
non-null and pointing to Host-owned storage:

| Host state | Required successful state |
| --- | --- |
| `sim` | Borrowed pointer equal to the `sim` argument. Host does not free it. |
| `arp_cache` | Host-owned cache object. It is not `NULL`. |
| `ip_stack` | Host-owned IP stack object. It is not `NULL`. |
| `udp_state` | Host-owned UDP state table. It is not `NULL`. |
| `udp_context` | Host-owned UDP context. It is not `NULL`. |
| `tcp_table` | Host-owned TCP connection table. It is not `NULL`. |
| `tcp_context` | Host-owned TCP context. It is not `NULL`. |
| `gateway_ip` | Host-order IPv4 address. `0` means no gateway configured. |

If the implementation later changes these fields from pointers to embedded
objects, the specification and ACSL contracts MUST be updated at the same time.
Do not mix pointer-style code with embedded-object contracts.

### ARP Cache Initial State

The phrase "initialize the ARP cache as empty" MUST NOT appear in code review as
an unresolved interpretation. For this module it means the following exact
state.

| ARP cache part | Required value after successful `host_create` |
| --- | --- |
| `host->arp_cache` | non-null Host-owned `ArpCache *` |
| `host->arp_cache->count` | `0` |
| `host->arp_cache->entries[i].valid` for `0 <= i < 256` | `0` |
| `host->arp_cache->pending_count` | `0` |
| `host->arp_cache->pending[i].valid` for `0 <= i < 32` | `0` |

An empty ARP cache is a valid cache with no learned entries and no queued
pending packets. A `NULL` ARP cache is not empty; it is missing. If Host cannot
create this state, `host_create` MUST fail and return `NULL`.

`arp_cache_init` is the public API that produces this state. Host owns the cache
storage, but ARP cache owns the initialization rule. Host creation MUST use that
public initializer instead of manually reaching into every ARP cache field.

### IP Stack Initial State

The Host IP stack initial state is:

- the IP stack object exists
- it is not `NULL` after successful `host_create`
- its simulator pointer is the `sim` argument
- every protocol entry starts with no handler and no context

After protocol registration, the dispatch table contains the ICMP, UDP, and TCP
entries described below.

### UDP State Initial State

The Host creates its UDP state by allocating `host->udp_state` and then calling
`udp_init(host->udp_state)`.

After that initializer call:

- `host->udp_state` is not `NULL`
- `host->udp_state->count == 0`
- every UDP socket slot has `valid == 0`

This is per Host. Two Hosts may both bind UDP port 520 because they have
different `UdpState` objects.

### TCP Table Initial State

The Host creates its TCP table by allocating `host->tcp_table` and then calling
`tcp_init(host->tcp_table)`.

After that initializer call:

- `host->tcp_table` is not `NULL`
- `host->tcp_table->count == 0`
- every TCB slot has `valid == 0`

This is per Host. A connection on one Host MUST NOT appear in another Host's
TCP table.

### UDP And TCP Contexts

`IpStack` stores one `void *ctx` beside each registered handler. IP does not
know what that pointer means. The receiving protocol knows.

For UDP:

- `udp_context` MUST identify the same simulator passed to `host_create`
- `udp_context` MUST identify this Host's `udp_state`
- IP stores `udp_context` beside `udp_receive`
- later, `udp_receive` receives that context and uses it to find this Host's UDP
  socket table

For TCP:

- `tcp_context` MUST identify the same simulator passed to `host_create`
- `tcp_context` MUST identify this Host's `tcp_table`
- IP stores `tcp_context` beside `tcp_receive`
- later, `tcp_receive` receives that context and uses it to find this Host's TCP
  connection table

The context objects are not extra UDP sockets or extra TCP connections. They are
the bridge between generic IP dispatch and Host-owned protocol state.

## Flow Charts

### Host Creation

```text
host_create(name, sim, gateway_ip)
  |
  +-- reject NULL name or NULL sim
  |
  +-- allocate Host and clear it
  |
  +-- initialize embedded Device fields
  |
  +-- create Host-owned ARP cache and initialize it with arp_cache_init:
      count 0, pending_count 0,
      all entry valid bits 0,
      all pending valid bits 0
  |
  +-- create Host-owned IpStack:
      simulator pointer is sim,
      all protocol handlers and contexts initially null
  |
  +-- allocate Host-owned UdpState and call udp_init(host->udp_state):
      after the call, count is 0 and all socket valid bits are 0
  |
  +-- allocate Host-owned TcpTable and call tcp_init(host->tcp_table):
      after the call, count is 0 and all TCB valid bits are 0
  |
  +-- create UDP context:
      simulator is sim,
      state is this Host's UdpState
  |
  +-- create TCP context:
      simulator is sim,
      table is this Host's TcpTable
  |
  +-- register ICMP, UDP, TCP handlers in this Host's IpStack
  |
  +-- store gateway_ip
  |
  +-- return Host
```

If any allocation or registration step fails, Host creation MUST release
everything already created and return `NULL`.

### Receive Path

```text
Ethernet frame arrives
  |
  v
Interface receive handler
  |
  v
ip_receive using this Host's IpStack
  |
  +-- protocol == IPPROTO_ICMP -> icmp_receive(ctx = simulator)
  |
  +-- protocol == IPPROTO_UDP  -> udp_receive(ctx = this Host's UDP context)
  |
  +-- protocol == IPPROTO_TCP  -> tcp_receive(ctx = this Host's TCP context)
```

The handler registration happens during `host_create`; the handler call happens
later during receive. These are different events.

### Send Path

```text
local protocol or application has L4 payload
  |
  v
host_send_ip
  |
  v
ip_output
  |
  +-- choose source interface
  +-- check on-link reachability
  +-- use interface ARP cache
  +-- send now or queue pending ARP
```

Host does not choose the destination MAC address. Host does not perform ARP
lookup by hand.

## Use Cases

### Use Case: Create A Host

Preconditions:

- caller provides non-null `name`
- caller provides non-null `sim`

Required result on success:

- Host exists
- embedded Device has zero interfaces and capacity `8`
- ARP cache exists with `count == 0`, `pending_count == 0`, no valid entries,
  and no valid pending packets
- IP stack exists and has ICMP, UDP, and TCP dispatch entries
- UDP state exists with bound-socket count `0` and no valid socket slots
- TCP table exists with connection count `0` and no valid TCB slots
- UDP context points to simulator and this Host's UDP state
- TCP context points to simulator and this Host's TCP table
- `gateway_ip` is stored unchanged

### Use Case: Add Interface To Host

Preconditions:

- Host exists
- interface exists
- embedded Device still has capacity

Required result on success:

- interface is added to the embedded Device
- interface `device` back-pointer points at the embedded Device
- interface ARP cache pointer points at the Host-owned ARP cache
- interface receive handler enters this Host's IP stack
- Host interface count increases by one

### Use Case: Receive UDP Packet

Preconditions:

- interface was added to this Host
- IP stack has registered `IPPROTO_UDP`
- incoming IPv4 packet has protocol byte `IPPROTO_UDP`

Flow:

1. Interface delivery reaches this Host's IP receive path.
2. IP validates and strips the IPv4 header.
3. IP looks up protocol `IPPROTO_UDP` in this Host's dispatch table.
4. IP calls the registered UDP handler.
5. IP passes this Host's UDP context to the handler.
6. UDP uses that context to find this Host's UDP state table.

### Use Case: Send IP Payload

Preconditions:

- Host exists
- payload exists
- source and destination IP addresses are nonzero host-order values

Required behavior:

- Host delegates to IP output.
- On success, ownership follows `ip_output`.
- On failure, caller still owns the payload unless `ip_output` documents a
  different transfer for that path.

## Public API

The Host module exposes a small API:

```c
Host *host_create(const char *name, Simulator *sim, uint32_t gateway_ip);
void  host_free(Host *host);

int   host_add_interface(Host *host, Interface *iface);

int   host_receive(Host *host,
                   Interface *iface,
                   Packet *pkt,
                   uint16_t ethertype);

int   host_send_ip(Host *host,
                   uint32_t src_ip,
                   uint32_t dst_ip,
                   uint8_t protocol,
                   Packet *payload);
```

Higher-level application helpers may be added later. Host MUST NOT duplicate
full UDP or TCP packet construction because UDP and TCP already provide their
own send APIs.

## Byte Order Rules

Use these rules consistently:

- `gateway_ip` is host order
- `host_send_ip` `src_ip` and `dst_ip` are host order
- UDP/TCP public API IP arguments are host order
- ARP cache lookup keys are host order in the current stack
- `Interface.ip_addr` is network order
- wire headers are network order

Host mostly connects modules together, so it should not rewrite addresses
unless the public API it calls requires that conversion.

## Function Behavior

### `host_create`

Required behavior:

- If `name` is `NULL`, return `NULL`.
- If `sim` is `NULL`, return `NULL`.
- Allocate and clear one Host.
- Initialize the embedded Device fields:
  - copy `name` into the device name buffer and terminate it
  - allocate the interface pointer array with capacity `8`
  - set interface count to zero
  - set interface capacity to `8`
- Create the Host-owned ARP cache and initialize it through `arp_cache_init`.
  After initialization, `count == 0`, `pending_count == 0`, every entry is
  invalid, every pending packet slot is invalid, and pending packet pointers are
  cleared. A `NULL` cache is failure, not an empty cache.
- Create the Host-owned IP stack and put it in the required initial state:
  simulator pointer equal to `sim`, and no registered protocol handlers before
  the ICMP/UDP/TCP registration step.
- Allocate the Host-owned UDP state, then call `udp_init(host->udp_state)`.
  After that call, `host->udp_state->count == 0` and every socket slot is
  invalid. Host creation should not manually set those UDP fields.
- Allocate the Host-owned TCP table, then call `tcp_init(host->tcp_table)`.
  After that call, `host->tcp_table->count == 0` and every TCB slot is invalid.
  Host creation should not manually set those TCP fields.
- Create the Host-owned UDP context with simulator equal to `sim` and state
  equal to this Host's UDP state.
- Create the Host-owned TCP context with simulator equal to `sim` and table
  equal to this Host's TCP table.
- Register the three IP protocol handlers:

  | IPv4 protocol | Handler remembered by IP | Context remembered by IP |
  | --- | --- | --- |
  | `IPPROTO_ICMP` | `icmp_receive` | the simulator pointer |
  | `IPPROTO_UDP` | `udp_receive` | this Host's UDP context |
  | `IPPROTO_TCP` | `tcp_receive` | this Host's TCP context |

- Store `gateway_ip`.
- Return the Host.

If any allocation or registration fails, release all Host-owned storage already
created and return `NULL`.

### `host_free`

Required behavior:

- If `host` is `NULL`, return.
- Free interfaces owned through the embedded Device, following the same
  ownership rule as `device_free`.
- Free the embedded Device's interface pointer array.
- Free Host-owned ARP cache storage.
- Free Host-owned IP stack storage.
- Free Host-owned UDP state storage.
- Free Host-owned UDP context storage.
- Free Host-owned TCP table storage.
- Free Host-owned TCP context storage.
- Free the Host itself.

Cleanup MUST match the final storage model in `host.h`. If a field is embedded,
do not free that field separately. If a field is a Host-owned pointer, free it.

### `host_add_interface`

Required behavior:

- If `host` is `NULL`, return `-1`.
- If `iface` is `NULL`, return `-1`.
- Add the interface to the embedded Device.
- If the Device is full or rejects the interface, return `-1`.
- Point the interface at the Host-owned ARP cache.
- Bind the interface receive path to the Host-owned IP stack.
- Return `0` on success.

The interface does not need to be administratively up when it is added. Send and
receive paths already check interface state where it matters.

### `host_receive`

`host_receive` is a convenience wrapper for tests and code that wants to enter
at the Host boundary.

Required behavior:

- If `host`, `iface`, or `pkt` is `NULL`, return `-1`.
- If `ethertype` is not IPv4, free `pkt` and return `-1`.
- Deliver the packet to this Host's IP receive path.
- Return the result from the IP receive path.

Normal link delivery may call the interface receive handler directly. That is
still valid because `host_add_interface` binds each interface to the Host's IP
stack.

### `host_send_ip`

Required behavior:

- If `host` is `NULL`, return `-1`.
- If `payload` is `NULL`, return `-1`.
- If `src_ip` is `0`, return `-1`.
- If `dst_ip` is `0`, return `-1`.
- Delegate to IP output using the Host's simulator pointer.
- Return the result from IP output.

Ownership follows `ip_output`:

- on success, the payload is owned by IP, ARP pending state, or the lower layer
- on failure before ownership transfer, the caller still owns the payload

Host MUST NOT manually choose a destination MAC address and MUST NOT perform ARP
lookup itself.

## Gateway And Routing Boundary

The Host stores `gateway_ip`, but Phase 1 does not implement gateway routing in
Host.

Current `ip_output` sends only to destinations reachable on the same subnet as
the chosen source interface. If the destination is off-subnet, `ip_output`
returns `-1`.

Future gateway support should be added by extending IP output/routing behavior,
not by making Host manually bypass IP:

- Host owns the configured gateway value.
- IP output decides whether the next hop is the destination or the gateway.
- ARP resolves the selected next-hop address.

## ACSL Contract Requirements

ACSL is part of the specification for this module. The header contracts should
be strong enough for KLEVA/EVA to check null behavior, ownership-relevant state,
and observable effects.

Use literal numeric bounds in ACSL when the prover has trouble with macros:

- use `8` for `HOST_MAX_PORTS`
- use `256` for ARP cache entries
- use `32` for ARP pending packets and UDP sockets
- use `64` for TCP connection slots

### `host_create` ACSL Obligations

Required behaviors:

- `name == \null || sim == \null` assigns nothing and returns `\null`.
- Valid inputs either return `\null` on allocation/registration failure or
  return a valid Host.
- On success:
  - `\result->sim == sim`
  - `\result->base.iface_count == 0`
  - `\result->base.iface_max == 8`
  - `\result->base.interfaces != \null`
  - `\result->arp_cache != \null`
  - `\result->ip_stack != \null`
  - `\result->udp_state != \null`
  - `\result->udp_context != \null`
  - `\result->tcp_table != \null`
  - `\result->tcp_context != \null`
  - `\result->gateway_ip == gateway_ip`

The contract should also describe the initialized contents where KLEVA can
reason about them:

- `\result->arp_cache->count == 0`.
- `\result->arp_cache->pending_count == 0`.
- No ARP entry slot is valid.
- No ARP pending slot is valid.
- UDP bound-socket count is zero.
- TCP connection count is zero.

If proving every array slot is too heavy in the first pass, keep the count
postconditions in the public contract and move full slot validity checks into
targeted KLEVA tests.

### `host_free` ACSL Obligations

Required behaviors:

- `host == \null` assigns nothing.
- Non-null Host frees all Host-owned pointer fields and the Host itself.
- The frees clause MUST match the actual storage model in `host.h`.

If the final design uses embedded protocol objects, do not list those embedded
objects in `frees`. If the final design uses pointer fields, list the owned
pointer fields.

### `host_add_interface` ACSL Obligations

Required behaviors:

- `host == \null || iface == \null` assigns nothing and returns `-1`.
- Full Device returns `-1` and does not increment `iface_count`.
- Success:
  - returns `0`
  - increments `iface_count` by one
  - stores `iface` in the next interface slot
  - sets `iface->device` to the embedded Device
  - sets `iface->arp_cache` to the Host-owned ARP cache
  - installs the Host IP receive path on the interface

Because receive handler function pointers can be awkward for ACSL, the first
contract may specify the assigned fields and simple pointer relationships. KLEVA
tests should check the exact handler/context effects.

### `host_receive` ACSL Obligations

Required behaviors:

- null Host/interface/packet returns `-1`.
- non-IPv4 consumes the packet and returns `-1`.
- IPv4 delegates to the Host IP receive path and returns that result.

If packet freeing is hard to express in the public contract, keep the return
contract in ACSL and check packet consumption with KLEVA tests.

### `host_send_ip` ACSL Obligations

Required behaviors:

- null Host/payload returns `-1`.
- zero source or destination address returns `-1`.
- valid input delegates to IP output and returns its result.
- Host does not free `payload` on validation failure.

## KLEVA Verification Plan

KLEVA tests are required for this module. They should exercise the public Host
API and check the concrete state that the ACSL contracts either state directly
or intentionally leave to generated tests.

Minimum KLEVA behaviors:

1. `host_create(NULL, sim, 0)` returns `NULL`.
2. `host_create("h1", NULL, 0)` returns `NULL`.
3. Valid `host_create` returns a Host with non-null Host-owned state pointers.
4. Valid `host_create` creates the required ARP cache initial state:
   - `count == 0`
   - `pending_count == 0`
   - every entry slot has `valid == 0`
   - every pending slot has `valid == 0`
5. Valid `host_create` creates an initialized IP stack:
   - stack simulator is the input simulator
   - ICMP handler/context entry is registered
   - UDP handler/context entry is registered
   - TCP handler/context entry is registered
6. Valid `host_create` allocates `udp_state` and calls `udp_init`; after that:
   - `count == 0`
   - every socket slot has `valid == 0`
7. Valid `host_create` allocates `tcp_table` and calls `tcp_init`; after that:
   - `count == 0`
   - every TCB slot has `valid == 0`
8. `host_add_interface(NULL, iface)` returns `-1`.
9. `host_add_interface(host, NULL)` returns `-1`.
10. `host_add_interface` on a full Host returns `-1` and does not change count.
11. Successful `host_add_interface`:
    - increments `iface_count`
    - stores the interface pointer
    - sets `iface->device`
    - sets `iface->arp_cache`
    - installs an IP receive handler and context
12. `host_receive` rejects null inputs.
13. `host_receive` rejects non-IPv4 ethertypes and consumes the packet.
14. `host_send_ip` rejects null Host, null payload, zero source IP, and zero
    destination IP.
15. `host_send_ip` with valid inputs reaches the IP output path without Host
    doing ARP or Ethernet work itself.

Generated KLEVA wrappers should keep scenarios small. Prefer separate wrappers
for creation, interface binding, receive rejection, and send rejection instead
of one large test that tries to prove the whole Host lifecycle at once.

## Common Mistakes

- Do not treat an empty ARP cache as `NULL`.
- Do not use one global UDP or TCP state for every Host.
- Do not call ICMP, UDP, or TCP receive handlers during Host creation.
- Do not store host-order addresses directly in wire headers.
- Do not make Host perform Ethernet destination MAC selection.
- Do not make Host forward packets like a router.
- Do not free a payload inside `host_send_ip` when validation fails; the caller
  still owns it.
- Do not let `host.h`, `host.c`, ACSL contracts, and this spec use different
  names for the same Host field.
