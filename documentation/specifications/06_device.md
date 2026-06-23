# Module 06 — Device

**Files:** `src/network/device.c`, `src/network/device.h`
**Status:** ✅ Implemented (93% / 83%)
**Depends on:** interface, packet, arp_cache

---

## The Problem

`Interface` is one NIC. A real network node has **several** of them, plus
node-wide state (ARP cache, routing table, hostname). We need a `Device`
container that:

1. Owns an array of `Interface *`.
2. Owns the lifetime of interfaces added to it.
3. Sets the **back-pointer** on every interface so handlers can navigate
   `iface → device` without a global.
4. Provides lookups by interface name and by IP address.

## Mental Model

```
   ┌──────────────────────────────────────────────────────┐
   │              Device "R1"                             │
   │   name           = "R1"                              │
   │   iface_count    = 2 / iface_max = 4                 │
   │                                                      │
   │   interfaces[0] ─► Interface "eth0"  (192.168.1.1)   │
   │   interfaces[1] ─► Interface "eth1"  (10.0.0.1)      │
   │   interfaces[2] ─► NULL                              │
   │   interfaces[3] ─► NULL                              │
   │                                                      │
   └──────────────────────────────────────────────────────┘
            ▲                                ▲
            │                                │
       iface[0]->device                 iface[1]->device
```

---

## Header File — `device.h`

### Struct

```c
typedef struct Device {
    char        name[32];      // "Router-A"
    Interface **interfaces;    // heap array of Interface*
    int         iface_count;
    int         iface_max;     // capacity, fixed at create
} Device;
```


### Public API

| Function                            | Purpose                              |
|-------------------------------------|--------------------------------------|
| `device_create(name, iface_max)`    | Allocate device.                     |
| `device_free`                       | Free interfaces + arrays + device.   |
| `device_add_interface`              | Insert NIC, set back-pointer.        |
| `device_get_interface_by_name`      | Linear scan by `name`.               |
| `device_get_interface_by_ip`        | Linear scan by `ip_addr`.            |
| `device_receive_packet`             | Future: hand to L2/L3 stack.         |
| `device_send_packet`                | Future: emit via a chosen iface.     |

### ACSL highlight (`device_add_interface`)

```
result == 0  ⇒ dev->iface_count incremented
             AND dev->interfaces[old count] == iface
             AND iface->device              == dev      ← back-pointer
result == -1 ⇒ count unchanged
```

Ownership note (from the header comment): on success, the device
**owns** the interface — `device_free` will release it. Do not free
externally.

---

## Call Sequence — Adding an interface

```
dev = device_create("R1", 4)        ┐  arp_cache zeroed
                                    │  interfaces[] = malloc(4 * sizeof*)
                                    │  iface_count  = 0
                                    ┘
iface = interface_create("eth0", mac, ip, 24, 1500)
   │
   └─► device_add_interface(dev, iface)
            │
            │   if iface_count >= iface_max ⇒ -1
            │
            │   dev->interfaces[count] = iface
            │   iface->device          = dev          ← critical back-pointer
            │   dev->iface_count++
            ▼
           return 0
```

After this call, any handler that holds `iface` can reach the owning
device through `iface->device`. ARP cache access currently belongs to
`Interface.arp_cache`, not to `Device`.

## Call Sequence — IP-based lookup

```
device_get_interface_by_ip(dev, target_ip):
    for i in 0..iface_count:
        if interfaces[i]->ip_addr == target_ip: return interfaces[i]
    return NULL
```

---

## Design Notes

- **Fixed `iface_max`.** A router knows how many ports it has when it is
  built; growing the array at runtime adds no value here.
- **Linear scans (`get_*_by_name`, `get_*_by_ip`)** are fine for the
  handful of interfaces per device.
- **Forward declaration** of `struct Device` in `interface.h` keeps the
  back-pointer cycle from becoming a circular include.
- The (currently stub) `device_receive_packet` / `device_send_packet` are
  the seam where IP routing will plug in once #13 ships.

## Implementation Guide

1. `device_create`: validate `name` and positive `iface_max`; allocate
   `Device`; copy/truncate name; allocate the interface pointer array;
   set `iface_count = 0`.
2. `device_add_interface`: reject NULLs, full device, and duplicate
   interface names. On success, set `iface->device = dev`, append the
   pointer, and increment `iface_count`.
3. `device_free`: free each owned interface, then the interface pointer
   array, then the device. Links borrow interface pointers, so topology
   should free links before devices if link teardown ever dereferences
   endpoints.
4. Lookup functions are linear scans over live interfaces only:
   `0 .. iface_count - 1`.
5. `device_receive_packet` and `device_send_packet` are intentional
   stubs for now: validate inputs and return `0`.

## ACSL Contract Plan

- `device_create`: success produces a valid device with `iface_count ==
  0`, fixed `iface_max`, and a valid interface array.
- `device_add_interface`: success increments count by one, stores the
  interface at the old count, and sets the back-pointer. Failure leaves
  count unchanged.
- Duplicate-name failure should be an explicit behavior if proof coverage
  is expanded.
- Lookup hit can be specified with an existential over live interfaces;
  miss with a universal over live interfaces.
