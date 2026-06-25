# Module 04 — Interface (NIC)

**Files:** `src/network/interface.c`, `src/network/interface.h`
**Status:** ✅ Implemented (98% / 93%)
**Depends on:** stdlib (forward decls: `Link`, `Packet`, `Device`)

---

## The Problem

A real router has multiple physical ports — each with its own MAC, IP,
MTU, link state, and statistics. We need a single object that:

1. Carries the identity of *one* NIC.
2. Knows which `Link` it is attached to (or `NULL`).
3. Carries a back-pointer to its owning `Device` so receive handlers can
   reach device-level state (ARP cache, routing table) without a global.
4. Exposes a per-interface **receive callback** (`RxHandler`) so the
   layer above (typically Ethernet) can hand frames upward without
   re-importing every protocol header.

## Mental Model

```
   Interface "eth0"
   ┌────────────────────────────────────────────────────────────────┐
   │ name           = "eth0"                                        │
   │ mac            = AA:BB:CC:11:22:33                             │
   │ ip_addr        = 192.168.1.1     prefix_len = 24               │
   │ mtu            = 1500                                          │
   │ up             = 1               state = IFACE_OK              │
   │                                                                │
   │ link ─────────────────────────────► Link (other end is peer)   │
   │ device ───────────────────────────► Device (owning router)     │
   │                                                                │
   │ rx_handler  ─────────────────────► ethernet_receive callback   │
   │ handler_ctx ─────────────────────► sim                         │
   │                                                                │
   │ stats: tx_bytes, rx_bytes, rx_dropped, rx_errors, tx_errors    │
   │ time:  last_rx_time, last_tx_time, last_error_time             │
   └────────────────────────────────────────────────────────────────┘
```

---

## Header File — `interface.h`

### RxHandler typedef

```c
typedef void (*RxHandler)(struct Interface *iface,
                          struct Packet    *pkt,
                          uint16_t          ethertype,
                          void             *ctx);
```

Ethernet calls this after stripping the L2 header — the next layer (ARP
or IP) chooses what to do based on `ethertype`.

### State enum

```c
typedef enum {
    IFACE_OK,
    IFACE_ERR_DISABLED   // too many errors → stop forwarding
} InterfaceState;
```

### Struct (illustrated with field roles)

```c
typedef struct Interface {
    char            name[16];        // "eth0", "ge0/0"
    uint8_t         mac[6];          // 48-bit MAC
    uint32_t        ip_addr;         // IPv4, network order
    uint8_t         prefix_len;      // CIDR 0..32
    uint16_t        mtu;             // default 1500
    int             up;              // 1=up, 0=down

    struct Link    *link;            // peer connection or NULL
    struct Device  *device;          // ◄── back-pointer, set by device_add_interface

    uint64_t        tx_bytes;
    uint64_t        rx_bytes;

    RxHandler       rx_handler;      // protocol callback
    void           *handler_ctx;     // ctx passed to it
    struct ArpCache *arp_cache;      // borrowed cache pointer, optional

    uint64_t        rx_dropped;
    uint64_t        rx_errors;
    uint64_t        tx_errors;
    InterfaceState  state;

    uint64_t        last_rx_time;
    uint64_t        last_tx_time;
    uint64_t        last_error_time;
} Interface;
```

### The Critical Back-pointer

```
   Interface ──device──► owning Device subtype
       │
       └─arp_cache──► borrowed ArpCache owned by Host or Router
```

`iface->device` and `iface->arp_cache` are separate links with different
meanings.

`device_add_interface(dev, iface)` sets `iface->device = dev`, so code that has
only an `Interface *` can still find the owning node. `interface_set_arp_cache`
sets `iface->arp_cache` to a borrowed cache pointer owned by Host or Router.
ARP and IP use that borrowed cache pointer directly; Interface does not allocate
or free the cache.

### Public API

| Function                       | Purpose                                  |
|--------------------------------|------------------------------------------|
| `interface_create`             | Allocate + initialise NIC.               |
| `interface_free`               | Release.                                 |
| `interface_set_up / is_up`     | Admin state.                             |
| `interface_set_link / get_link`| Bind to a Link.                          |
| `interface_get_mac / get_ip`   | Read identity.                           |
| `interface_set_rx_handler`     | Register callback (Ethernet does this).  |
| `interface_set_arp_cache`      | Attach borrowed ARP cache pointer.       |
| `interface_add_tx_bytes / rx_bytes` | Counter helpers.                    |

---

## Call Sequence — Frame arrives

```
peer NIC: link_transmit
   │  scheduler_schedule(EVT_PACKET_RECEIVE event)
   │  event.handler     = ethernet_receive_event
   │  event.handler_ctx = sim
   │
[time advances]
   │
scheduler_step → event.handler(e, event.handler_ctx)
   │
   ▼
ethernet_receive_event(e, sim):
   │  iface = (Interface *) e->dst_device
   │  frame = (Packet    *) e->packet
   │
   ├─► ethernet_receive(iface, frame, &ethertype)
   │       (drop if MAC mismatch and not broadcast; strip 14 bytes)
   │
   │   iface->last_rx_time = e->timestamp
   │   iface->rx_bytes    += frame->len   (handled inside ethernet_receive)
   │
   └─► iface->rx_handler(iface, frame, ethertype, iface->handler_ctx)
            │
            └─► ip_receive  / arp_receive  / etc.
```

## Call Sequence — Building one

```
mac = {0xAA,0xBB,0xCC,0x11,0x22,0x33}
iface = interface_create("eth0", mac, htonl(0xC0A80101), 24, 1500)
interface_set_up(iface, 1)
interface_set_link(iface, link)
interface_set_rx_handler(iface, ip_receive, sim)
device_add_interface(dev, iface)        ← also sets iface->device = dev
```

---

## Design Notes

- **`mac[6]` is fixed-size:** all addressing in the simulator uses
  6-byte MACs explicitly, no Ethernet-vs-something-else polymorphism.
- **Counters are 64-bit:** they never wrap in any realistic simulation.
- **`IFACE_ERR_DISABLED`** mirrors a Cisco err-disable: ethernet checks
  it on both send and receive and refuses to forward.
- **All getters return `0`/`NULL` for null input** — see ACSL contracts.
- **Identity fields are immutable after `create`** — admin state and the
  link binding can change at runtime.

## Implementation Guide

1. `interface_create`: validate `name`, `mac`, `prefix_len <= 32`, and
   `mtu > 0`; allocate; copy/truncate name into 15 bytes plus NUL; copy
   the six-byte MAC; initialize counters, timestamps, link, device,
   callbacks, and `arp_cache` to zero/NULL.
2. `interface_set_up`: set only the administrative `up` flag. It does
   not touch `state`.
3. `interface_is_up`: return `1` only when `iface != NULL` and
   `iface->up != 0`.
4. `interface_set_link`: assign a borrowed `Link *`. Link ownership is
   handled by topology, not by interface.
5. `interface_set_rx_handler`: store both function pointer and context.
   Ethernet calls this callback after stripping L2.
6. `interface_set_arp_cache`: store a borrowed cache pointer. This is
   the current code-level route for ARP cache access; do not assume
   `Device` embeds an ARP cache.

## ACSL Contract Plan

Useful postconditions:

- `interface_create`: success returns an initialized object with no link,
  no device, no handler, no ARP cache, zero counters, `state == IFACE_OK`,
  and `up == 0`.
- Setter functions: NULL input is a no-op; valid input changes exactly
  the named field.
- Getter functions: NULL input returns `NULL` or `0`; valid input returns
  the corresponding field.
- Counter helpers: NULL input is a no-op; valid input adds exactly `n`
  to the selected 64-bit counter.
