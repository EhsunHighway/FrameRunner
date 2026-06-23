# Module 24 — Display: Topology View

**Files:** `src/display/topology_view.c`, `src/display/topology_view.h`
**Status:** ⬜ Not started
**Depends on:** topology, device, interface, link

---

## The Problem

After wiring a network in code, the user needs to **see what they built**
without running Wireshark or tracing events. The topology view renders
the network graph as ASCII art on `stdout`, showing:

- Every device (name, type: router/switch/host).
- Every link (bandwidth, delay, loss).
- IP addresses and MAC addresses on each interface.
- Link state (up/down).

This module is **pure display** — read-only access to topology data,
no events, no packet generation.

## Example Output

```
Topology "lab1" — 4 devices, 3 links
────────────────────────────────────────────────────
[Router] R1
  eth0  IP: 192.168.1.1/24  MAC: aa:bb:cc:11:22:33  UP
  eth1  IP: 10.0.0.1/30     MAC: aa:bb:cc:11:22:44  UP

[Host]   H1
  eth0  IP: 192.168.1.10/24 MAC: dd:ee:ff:00:00:01  UP

[Host]   H2
  eth0  IP: 192.168.1.20/24 MAC: dd:ee:ff:00:00:02  UP

Links:
  R1:eth0 ──[1000 Mbps / 1 ms / 0.0% loss]── H1:eth0
  R1:eth0 ──[1000 Mbps / 1 ms / 0.0% loss]── H2:eth0
────────────────────────────────────────────────────
```

---

## Header File — `topology_view.h`

### Constants

| Macro                    | Value | Use                              |
|--------------------------|-------|----------------------------------|
| `TV_LINE_WIDTH`          | `80`  | Column width for separators      |
| `TV_IFACE_INDENT`        | `2`   | Spaces before interface lines    |
| `TV_MAX_LABEL_LEN`       | `64`  | Truncation limit for long names  |

### Public API

| Function                                   | Purpose                                       |
|--------------------------------------------|-----------------------------------------------|
| `topology_view_print(topo, FILE *out)`     | Render full topology to `out`.                |
| `topology_view_print_device(dev, FILE *out)`| Print one device block.                      |
| `topology_view_print_links(topo, FILE *out)`| Print all links with endpoints.              |
| `topology_view_print_iface(iface, FILE *out)`| Print one interface line.                   |
| `topology_view_sprint_ip(ip, buf, bufsz)`  | `"192.168.1.1"` from `uint32_t`.              |
| `topology_view_sprint_mac(mac, buf, bufsz)`| `"aa:bb:cc:11:22:33"` from `uint8_t[6]`.     |

### Return codes

All functions return `0` on success, `-1` on NULL input.

### ACSL Highlights

```
topology_view_sprint_ip:
  result == 0 ⇒ strlen(buf) <= 15  (max "255.255.255.255")
             && buf[0] != '\0'

topology_view_sprint_mac:
  result == 0 ⇒ strlen(buf) == 17  ("xx:xx:xx:xx:xx:xx")
```

---

## Function Call Sequence — Full topology print

```
topology_view_print(topo, stdout):
   │
   │   fprintf(out, "Topology — %d devices, %d links\n",
   │           topo->dev_count, topo->link_count)
   │   print separator line (TV_LINE_WIDTH '─')
   │
   ├─► for i in 0..topo->dev_count:
   │       topology_view_print_device(topo->devices[i], out)
   │           │
   │           │   detect type: if dev->arp_cache used → Router; else
   │           │                if mac_table present → Switch; else Host
   │           │   fprintf(out, "[Router] %s\n", dev->name)
   │           │
   │           └─► for j in 0..dev->iface_count:
   │                   topology_view_print_iface(dev->interfaces[j], out)
   │                       │
   │                       ├─► topology_view_sprint_ip(iface->ip_addr, ip_buf, 16)
   │                       ├─► topology_view_sprint_mac(iface->mac, mac_buf, 18)
   │                       └─► fprintf(out, "  %s  IP: %s/%d  MAC: %s  %s\n",
   │                                   iface->name, ip_buf, iface->prefix_len,
   │                                   mac_buf, iface->up ? "UP" : "DOWN")
   │
   │   fprintf(out, "\nLinks:\n")
   │
   └─► for i in 0..topo->link_count:
           topology_view_print_links (inline per link):
               │
               └─► topology_view_sprint_ip(link->end_a->ip_addr, ...)
                   fprintf(out,
                     "  %s:%s ──[%u Mbps / %u ms / %.1f%% loss]── %s:%s\n",
                     dev_a->name, end_a->name,
                     link->bandwidth_mbps, link->delay_ms,
                     link->loss_rate * 100.0f,
                     dev_b->name, end_b->name)
```

---

## Design Notes

- **Device type detection**: at this milestone, check presence of
  `Device.arp_cache` (non-zero entries) for routers; a cleaner approach
  would add a `DeviceType` enum to `Device`. Improve in a later version.
- **NULL links on interfaces**: `iface->link` may be NULL (unconnected
  NIC). Skip link display for that interface.
- **`FILE *out` parameter** makes the functions unit-testable with
  `fmemopen` or redirectable to a log file.
- **No color codes**: plain ASCII output works in all terminals and in
  log files. Add ANSI color as a compile-time option if wanted.
- **Thread safety**: this module is read-only; it never modifies
  topology state. Safe to call from any context.

## Test Plan (kleva)

- `sprint_ip_loopback`, `sprint_ip_max_octet`, `sprint_ip_zero`
- `sprint_mac_correct_format`
- `print_device_writes_name_and_ifaces`
- `print_links_writes_bandwidth_delay`
- NULL guards: `print_null_topo`, `sprint_ip_null_buf`
