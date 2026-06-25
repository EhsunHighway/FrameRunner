# Module 28 — Display: Packet Header View

**Files:** `src/display/header_view.c`, `src/display/header_view.h`
**Status:** ⬜ Not started
**Depends on:** packet, ethernet, ip, icmp, tcp, udp, arp

---

## The Problem

When debugging a simulation, the user needs to see **the exact bytes of
a packet decoded into human-readable fields** — similar to Wireshark's
"Packet Details" pane. This module:

1. Peeks at the raw bytes in a `Packet` buffer.
2. Detects layer boundaries using ethertype / protocol fields.
3. Prints each header in an aligned, labeled table.
4. Does **not** strip or modify the packet.

## Example Output

```
┌─────────────────────────────────────────────┐
│ Packet  len=98 bytes                        │
├─────────────────────────────────────────────┤
│ [L2] Ethernet                               │
│   dst_mac   :  ff:ff:ff:ff:ff:ff            │
│   src_mac   :  aa:bb:cc:11:22:33            │
│   ethertype :  0x0806  (ARP)                │
├─────────────────────────────────────────────┤
│ [L3] ARP                                    │
│   htype     :  1 (Ethernet)                 │
│   ptype     :  0x0800 (IPv4)                │
│   operation :  1 (REQUEST)                  │
│   sender_ip :  192.168.1.1                  │
│   target_ip :  192.168.1.10                 │
└─────────────────────────────────────────────┘
```

---

## Header File — `header_view.h`

### Constants

| Macro                   | Value | Use                                |
|-------------------------|-------|------------------------------------|
| `HV_BOX_WIDTH`          | `47`  | Inner box width                    |
| `HV_FIELD_LABEL_WIDTH`  | `12`  | Left-column label padding          |
| `HV_MAX_LAYERS`         | `5`   | Max protocol layers to decode      |

### Layer type enum

```c
typedef enum {
    HV_LAYER_ETH,
    HV_LAYER_ARP,
    HV_LAYER_IP,
    HV_LAYER_ICMP,
    HV_LAYER_TCP,
    HV_LAYER_UDP,
    HV_LAYER_UNKNOWN
} HvLayerType;
```

### Public API

| Function                                  | Purpose                                      |
|-------------------------------------------|----------------------------------------------|
| `header_view_print(pkt, FILE *out)`       | Detect and print all layers.                 |
| `header_view_print_eth(data, len, FILE*)` | Print Ethernet header fields.                |
| `header_view_print_arp(data, len, FILE*)` | Print ARP header fields.                     |
| `header_view_print_ip(data, len, FILE*)`  | Print IP header fields.                      |
| `header_view_print_icmp(data, len, FILE*)`| Print ICMP header fields.                    |
| `header_view_print_tcp(data, len, FILE*)` | Print TCP header fields.                     |
| `header_view_print_udp(data, len, FILE*)` | Print UDP header fields.                     |
| `header_view_detect_layer(data, len, offset)` | Return `HvLayerType` at byte offset.    |

### Return codes

`0` on success, `-1` on NULL or truncated buffer.

### ACSL Highlights

```
header_view_print_eth:
  len >= 14 ⇒ result == 0
  len < 14  ⇒ result == -1  (too short to decode)

header_view_print_ip:
  len >= 20 && (data[0] >> 4) == 4 ⇒ result == 0

header_view_detect_layer:
  \result == HV_LAYER_UNKNOWN ⇒ no further recursion
```

---

## Function Call Sequence — Auto-detect and print all layers

```
header_view_print(pkt, stdout):
   │
   │   offset = 0
   │   fprintf(out, "┌─...─┐\n│ Packet  len=%d bytes...│\n", pkt->len)
   │
   │   // Layer 2 — always start with Ethernet
   │   if pkt->len >= 14:
   │       fprintf(out, "├─ [L2] Ethernet ─...─┤\n")
   │       header_view_print_eth(pkt->data + 0, pkt->len, out)
   │           │  field-by-field: dst_mac, src_mac, ethertype
   │           │  returns next_layer type (ARP / IP / UNKNOWN)
   │       ethertype = ntohs(*(uint16_t*)(pkt->data + 12))
   │       offset = 14
   │
   │   // Layer 3
   │   if ethertype == 0x0806 (ARP) && pkt->len - offset >= 28:
   │       header_view_print_arp(pkt->data + offset, ...)
   │       offset += 28
   │
   │   elif ethertype == 0x0800 (IP) && pkt->len - offset >= 20:
   │       header_view_print_ip(pkt->data + offset, ...)
   │           │  prints: ver, ihl, dscp, total_len, id, flags,
   │           │          ttl, protocol, checksum, src_ip, dst_ip
   │       proto   = pkt->data[offset + 9]
   │       ihl     = (pkt->data[offset] & 0x0F) * 4
   │       offset += ihl
   │
   │   // Layer 4
   │   if proto == 1 (ICMP):   header_view_print_icmp(pkt->data + offset, ...)
   │   elif proto == 6 (TCP):  header_view_print_tcp (pkt->data + offset, ...)
   │   elif proto == 17 (UDP): header_view_print_udp (pkt->data + offset, ...)
   │
   └─► fprintf(out, "└─...─┘\n")
```

---

## Design Notes

- **Non-destructive** — the function reads `pkt->data` with pointer
  casts but never modifies the packet. Safe to call mid-simulation.
- **Bounds-checked before cast** — every `header_view_print_*` checks
  `len >= expected_hdr_size` before casting `data` to the header struct.
- **`packet->layer` field** (if maintained by the simulation) could
  accelerate detection, but we don't rely on it — the view re-derives
  layers from raw bytes to match what's actually on the wire.
- **Hex fallback** — if `HV_LAYER_UNKNOWN`, print a hex dump of the
  remaining bytes (up to 64 bytes).

## Test Plan (kleva)

- `print_eth_correct_fields`, `print_arp_request_fields`
- `print_ip_flags_and_checksum`, `print_icmp_echo_fields`
- `print_tcp_flags_decoded`, `print_udp_ports`
- `too_short_returns_error`, `null_pkt_returns_error`
- `unknown_ethertype_shows_hex_dump`
