# Module 28 - Display Packet Headers

**Files:** `src/display/header_view.c`, `src/display/header_view.h`
**Status:** Ready for implementation; current source files are empty
**Depends on:** `packet`, `trace`, `ethernet`, `arp`, `ip`, `icmp`, `tcp`,
`udp`, `byte_order`

## Concepts First

Packet header display is a read-only decoder for packet bytes.

It is similar to the packet details pane in Wireshark:

```text
Ethernet
  dst_mac
  src_mac
  ethertype
IPv4
  src_ip
  dst_ip
  ttl
  protocol
TCP
  src_port
  dst_port
  seq
  ack
```

The module must not change the packet. It only reads bytes and prints fields.

### Current Packet Layer Versus Full Frame

`Packet.data` points to the current visible layer.

During simulation, packet functions may strip headers:

- after Ethernet receive, `data` may point to IPv4
- after IP receive, `data` may point to TCP/UDP/ICMP
- after TCP/UDP receive, `data` may point to payload

Header display must support both:

- a full Ethernet frame starting at `pkt->data`
- a packet already stripped to a higher layer

The public `header_view_print` should use `pkt->layer` as a hint only. It must
still validate bytes before decoding.

### Non-Destructive Decoding

The display module must not call:

- `packet_strip`
- `packet_prepend`
- `packet_free`
- protocol receive functions

It reads `pkt->data[0 .. pkt->len - 1]` and writes text to `FILE *out`.

### Bounds Before Cast

Every decoder must check length before casting bytes to a protocol header.

Examples:

- Ethernet requires 14 bytes.
- ARP requires 28 bytes.
- IPv4 requires at least 20 bytes and IHL bytes.
- ICMP requires 8 bytes.
- UDP requires 8 bytes.
- TCP requires at least 20 bytes and data offset bytes.

On truncation, print an explicit truncated message and return `-1`.

### Byte Order

Wire headers store multi-byte fields in network byte order.

The display must convert fields before printing numeric values.

For example:

```text
ethertype 0x0800
tcp dst_port 80
ip total_length 60
```

### Plain ASCII Output

Use plain ASCII output.

Do not use box drawing characters or ANSI color codes in the first milestone.
Plain output is easier to test, log, diff, and display everywhere.

## Purpose

The header display module decodes and prints packet headers.

It provides:

- full packet auto-print
- Ethernet header print
- ARP header print
- IPv4 header print
- ICMP header print
- TCP header print
- UDP header print
- simple layer detection
- bounded hex fallback for unknown bytes

It does not:

- mutate packets
- validate checksums as a protocol decision
- consume or free packets
- schedule events
- transmit packets
- replace protocol receive logic

## Architecture Boundary

| Responsibility | Owner |
| --- | --- |
| Packet storage | Packet module |
| Header layouts | Protocol headers |
| Human-readable decoding | Header display |
| Protocol validation/receive behavior | Protocol modules |
| Packet mutation | Protocol modules, not display |

Header display may include protocol headers for struct definitions and
constants. It must not call protocol receive/send functions.

## Data Model

### Constants

```c
#define HV_FIELD_LABEL_WIDTH 14
#define HV_HEX_DUMP_LIMIT    64
```

Use simple text section headers instead of boxes:

```text
Packet id=7 len=54 layer=2
[Ethernet]
  dst_mac       : ff:ff:ff:ff:ff:ff
  src_mac       : aa:bb:cc:11:22:33
  ethertype     : 0x0800 (IPv4)
```

### Layer Type

```c
typedef enum HvLayerType {
    HV_LAYER_ETH,
    HV_LAYER_ARP,
    HV_LAYER_IP,
    HV_LAYER_ICMP,
    HV_LAYER_TCP,
    HV_LAYER_UDP,
    HV_LAYER_UNKNOWN
} HvLayerType;
```

## Ownership And Lifetime

Display borrows the packet and output stream.

It does not free:

- `Packet *`
- packet buffers
- `FILE *`

Helper functions borrow byte ranges. They do not allocate long-lived memory.

## Public API

```c
int header_view_print(const Packet *pkt, FILE *out);

int header_view_print_snapshot(const TraceRecord *record, FILE *out);

int header_view_print_eth(const uint8_t *data, size_t len, FILE *out);

int header_view_print_arp(const uint8_t *data, size_t len, FILE *out);

int header_view_print_ip(const uint8_t *data,
                         size_t len,
                         FILE *out,
                         uint8_t *out_protocol,
                         size_t *out_header_len);

int header_view_print_icmp(const uint8_t *data, size_t len, FILE *out);

int header_view_print_tcp(const uint8_t *data, size_t len, FILE *out);

int header_view_print_udp(const uint8_t *data, size_t len, FILE *out);

HvLayerType header_view_detect_layer(const uint8_t *data,
                                     size_t len,
                                     size_t offset);
```

All print functions return `0` on successful decode and `-1` on invalid input
or truncation.

## Function Behavior

### `header_view_print`

Purpose:

Print one packet summary or detail view using the display configuration.

Implementation task:

Implement `header_view_print` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `pkt == NULL || out == NULL`, return `-1`.
- If `pkt->data == NULL && pkt->len > 0`, return `-1`.
- Print packet ID, length, and layer.
- Choose starting decoder:
  - if `pkt->layer <= 2` and bytes look like Ethernet, decode Ethernet first
  - if `pkt->layer == 3`, decode IPv4 first
  - if `pkt->layer == 4`, decode L4 according to caller-provided context is not
    possible, so print hex unless a recognizable header is present
  - otherwise attempt Ethernet first and fall back to hex
- For Ethernet:
  - decode ethertype
  - if ARP, decode ARP
  - if IPv4, decode IP and then L4 based on protocol
- For IPv4:
  - decode IP header
  - advance by IHL
  - decode ICMP/TCP/UDP if protocol is recognized
- If a layer is unknown, print hex dump of up to 64 bytes.
- Never mutate `pkt`.

### `header_view_print_eth`

Purpose:

Print the requested eth representation.

Implementation task:

Implement `header_view_print_eth` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL || out == NULL`, return `-1`.
- If `len < ETH_HDR_LEN`, print truncated Ethernet and return `-1`.
- Print destination MAC, source MAC, and ethertype.
- Return `0`.

### `header_view_print_arp`

Purpose:

Print the requested arp representation.

Implementation task:

Implement `header_view_print_arp` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL || out == NULL`, return `-1`.
- If `len < sizeof(ArpPacket)`, print truncated ARP and return `-1`.
- Print hardware type, protocol type, address lengths, opcode, sender MAC,
  sender IP, target MAC, and target IP.
- Convert numeric fields from network order.
- Return `0`.

### `header_view_print_ip`

Purpose:

Print the requested ip representation.

Implementation task:

Implement `header_view_print_ip` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL || out == NULL`, return `-1`.
- If `len < IP_HDR_LEN`, print truncated IPv4 and return `-1`.
- Decode version and IHL.
- If version is not `4`, print unknown IP version and return `-1`.
- If IHL is less than 20 or greater than `len`, print truncated/options error
  and return `-1`.
- Print total length, TTL, protocol, checksum, source IP, and destination IP.
- If `out_protocol != NULL`, store protocol byte.
- If `out_header_len != NULL`, store IHL bytes.
- Return `0`.

The display does not reject bad checksums. It may print whether
`ip_checksum(header) == 0` if enough bytes are readable.

### `header_view_print_icmp`

Purpose:

Print the requested icmp representation.

Implementation task:

Implement `header_view_print_icmp` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL || out == NULL`, return `-1`.
- If `len < ICMP_HDR_LEN`, print truncated ICMP and return `-1`.
- Print type, code, checksum, id, and seq.
- Return `0`.

### `header_view_print_tcp`

Purpose:

Print the requested tcp representation.

Implementation task:

Implement `header_view_print_tcp` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL || out == NULL`, return `-1`.
- If `len < TCP_HDR_LEN`, print truncated TCP and return `-1`.
- Decode ports, sequence number, ACK number, data offset, flags, window,
  checksum, and urgent pointer.
- If TCP data offset is less than 20 bytes or greater than `len`, return `-1`.
- Print individual flag names for FIN, SYN, RST, PSH, ACK, URG.
- Return `0`.

### `header_view_print_udp`

Purpose:

Print the requested udp representation.

Implementation task:

Implement `header_view_print_udp` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL || out == NULL`, return `-1`.
- If `len < UDP_HDR_LEN`, print truncated UDP and return `-1`.
- Decode source port, destination port, length, and checksum.
- If UDP length is less than 8, print invalid length and return `-1`.
- Return `0`.

### `header_view_detect_layer`

Purpose:

Detect the highest protocol header that can be safely displayed from the packet bytes.

Implementation task:

Implement `header_view_detect_layer` using the supplied arguments and the module state identified by this specification. The ordered steps below define the required validation, state changes, ownership actions, and failure exits; do not infer additional responsibilities from the function name.

Inputs and existing state:

Use the parameters in the declared public or internal signature and only the existing objects reachable through those parameters, except where the ordered steps explicitly identify module-owned state.

Result:

Produce the return value, state transition, output, and ownership outcome stated by the ordered steps and postconditions below.

Required behavior:

Follow every validation, capacity, ordering, byte-order, and ownership rule in this function section. A failure path must stop at the point stated below and must not perform later success-path actions.

Implementation order:

- If `data == NULL`, return `HV_LAYER_UNKNOWN`.
- If `offset >= len`, return `HV_LAYER_UNKNOWN`.
- If enough bytes exist and ethertype at Ethernet offset is IPv4 or ARP, return
  `HV_LAYER_ETH`.
- If byte at offset looks like IPv4 version/IHL and at least 20 bytes remain,
  return `HV_LAYER_IP`.
- Otherwise return `HV_LAYER_UNKNOWN`.

This helper is best-effort. Print functions still perform full validation.

## Persistent Snapshot Display

Live packet printing remains available while a caller owns a valid `Packet`.
Animation and history use copied `TraceRecord` snapshots because live packets
may already be stripped, cloned, or freed.

`header_view_print_snapshot` implementation order:

1. If `record == NULL || out == NULL`, return `-1`.
2. Print packet ID, trace ID, parent ID, original visible length, captured
   length, and layer.
3. If `snapshot_length == 0`, print that no bytes were captured and return `0`.
4. Validate `snapshot_length <= TRACE_PACKET_SNAPSHOT_MAX`.
5. Decode from `record->snapshot` using the same non-destructive bounded helper
   sequence as live packet display.
6. When `snapshot_truncated != 0`, print an explicit truncation marker. Decode
   only complete headers within captured bytes; never infer missing bytes from
   `packet_length`.
7. Return `0` after a complete supported display or `-1` after printing a
   bounded truncation/invalid-snapshot explanation.

The display does not parse `TraceRecord.summary` to reconstruct headers.

## Flow Charts

### Full Print

```text
header_view_print(pkt, out)
  |
  +-- reject NULL inputs
  +-- print packet summary
  |
  +-- start at Ethernet?
  |     print Ethernet
  |     ethertype ARP -> print ARP
  |     ethertype IPv4 -> print IP -> print L4
  |
  +-- start at IPv4?
  |     print IP -> print L4
  |
  +-- unknown:
        print hex fallback
```

### IPv4 To L4

```text
print_ip(data, len, &proto, &ihl)
  |
  +-- proto ICMP: print_icmp(data + ihl)
  +-- proto TCP:  print_tcp(data + ihl)
  +-- proto UDP:  print_udp(data + ihl)
  +-- other:      hex dump remaining bytes
```

## ACSL Contracts

The contracts belong in `header_view.h`. Use literal header sizes:

- Ethernet: `14`
- ARP: `28`
- IPv4: `20`
- ICMP: `8`
- TCP: `20`
- UDP: `8`

### `header_view_print`

```c
/*@
    behavior bad_input:
        assumes pkt == \null || out == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior bad_packet:
        assumes pkt != \null && out != \null;
        assumes pkt->data == \null && pkt->len > 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes pkt != \null && out != \null;
        assumes pkt->len == 0 || pkt->data != \null;
        assumes pkt->len == 0 ||
                \valid_read(pkt->data + (0 .. pkt->len - 1));
        assigns \nothing;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int header_view_print(const Packet *pkt, FILE *out);
```

### Header Printers

```c
/*@
    behavior bad_input:
        assumes data == \null || out == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior too_short:
        assumes data != \null && out != \null;
        assumes len < 14;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes data != \null && out != \null;
        assumes len >= 14;
        assumes \valid_read(data + (0 .. len - 1));
        assigns \nothing;
        ensures \result == 0;

    complete behaviors;
    disjoint behaviors;
*/
int header_view_print_eth(const uint8_t *data, size_t len, FILE *out);
```

Use the same pattern for ARP, IPv4, ICMP, TCP, and UDP with their own minimum
header lengths.

### `header_view_detect_layer`

```c
/*@
    behavior bad_input:
        assumes data == \null || offset >= len;
        assigns \nothing;
        ensures \result == HV_LAYER_UNKNOWN;

    behavior valid:
        assumes data != \null && offset < len;
        assumes \valid_read(data + (0 .. len - 1));
        assigns \nothing;
        ensures \result == HV_LAYER_ETH ||
                \result == HV_LAYER_ARP ||
                \result == HV_LAYER_IP ||
                \result == HV_LAYER_ICMP ||
                \result == HV_LAYER_TCP ||
                \result == HV_LAYER_UDP ||
                \result == HV_LAYER_UNKNOWN;

    complete behaviors;
    disjoint behaviors;
*/
HvLayerType header_view_detect_layer(const uint8_t *data,
                                     size_t len,
                                     size_t offset);
```

## KLEVA Verification Plan

Minimum KLEVA tests:

1. `header_view_print(NULL, out)` returns `-1`.
2. `header_view_print(pkt, NULL)` returns `-1`.
3. Packet with NULL data and nonzero length returns `-1`.
4. Ethernet printer rejects buffers shorter than 14.
5. Ethernet printer decodes MACs and ethertype.
6. ARP printer rejects buffers shorter than 28.
7. ARP printer decodes request fields.
8. IP printer rejects buffers shorter than 20.
9. IP printer rejects version not equal to 4.
10. IP printer rejects invalid IHL.
11. IP printer stores protocol and header length outputs.
12. ICMP printer rejects buffers shorter than 8.
13. TCP printer rejects buffers shorter than 20.
14. TCP printer decodes SYN and ACK flags.
15. TCP printer rejects invalid data offset.
16. UDP printer rejects buffers shorter than 8.
17. UDP printer rejects length smaller than 8.
18. Full Ethernet/IPv4/TCP packet prints all three layers.
19. Full Ethernet/ARP packet prints Ethernet and ARP.
20. Unknown ethertype prints hex fallback.
21. Display does not change `pkt->data`, `pkt->len`, or `pkt->layer`.

## Common Mistakes

- Do not call `packet_strip`.
- Do not free the packet.
- Do not assume `pkt->data` always points to Ethernet.
- Do not trust `pkt->layer` without validating bytes.
- Do not cast before checking length.
- Do not use non-ASCII box drawing characters.
- Do not reject packets only because checksum is invalid; this is display, not
  protocol receive.
