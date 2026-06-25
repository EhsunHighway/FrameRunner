# Module 26 — NAT (Network Address Translation)

**Files:** `src/protocols/nat.c`, `src/protocols/nat.h`
**Status:** ⬜ Not started
**Depends on:** ip, udp, tcp, packet, interface, simulator

---

## The Problem

A router at an AS boundary typically has one public IP but many private
hosts behind it. NAT translates (src_ip, src_port) on outgoing packets
to (public_ip, assigned_port), and reverses the translation on incoming
replies. This allows multiple private hosts to share a single public IP.
In this simulator NAT implements **PAT (Port Address Translation)**,
the most common variant (a.k.a. NAPT / "overload").

NAT must:
1. Hook into the IP forward path **before** routing.
2. Maintain a translation table keyed by (private_ip, private_port,
   proto).
3. Recompute IP and TCP/UDP checksums after rewriting.
4. Time out idle sessions.

## Mental Model

```
   Private side                          Public side
   192.168.1.10:5000  ─────────────────►  203.0.113.1:1024
   192.168.1.11:5001  ─────────────────►  203.0.113.1:1025
   192.168.1.10:5002  ─────────────────►  203.0.113.1:1026

   NAT Table
   ┌─────────────────────┬──────────────────────┬─────────┬──────────┐
   │ private_ip:port     │ public_ip:port        │ proto   │ last_use │
   ├─────────────────────┼──────────────────────┼─────────┼──────────┤
   │ 192.168.1.10:5000   │ 203.0.113.1:1024     │ TCP=6   │ 1400 ms  │
   │ 192.168.1.11:5001   │ 203.0.113.1:1025     │ UDP=17  │ 1380 ms  │
   └─────────────────────┴──────────────────────┴─────────┴──────────┘
```

---

## Header File — `nat.h`

### Constants

| Macro                  | Value     | Use                                      |
|------------------------|-----------|------------------------------------------|
| `NAT_TABLE_SIZE`       | `512`     | Max simultaneous NAT sessions            |
| `NAT_PORT_START`       | `1024`    | First dynamic port assigned              |
| `NAT_PORT_END`         | `65535`   | Last dynamic port                        |
| `NAT_TCP_TIMEOUT_MS`   | `86400000`| 24 h — established TCP session timeout   |
| `NAT_UDP_TIMEOUT_MS`   | `300000`  | 5 min — UDP session timeout              |
| `NAT_ICMP_TIMEOUT_MS`  | `60000`   | 1 min — ICMP echo session timeout        |
| `NAT_GC_INTERVAL`      | `60000`   | 1 min — garbage collection period        |

### `NatEntry` Struct (40 bytes)

```c
typedef struct NatEntry {
    uint32_t private_ip;      //  4 B
    uint32_t public_ip;       //  4 B
    uint16_t private_port;    //  2 B
    uint16_t public_port;     //  2 B — the assigned external port
    uint8_t  proto;           //  1 B — IPPROTO_TCP/UDP/ICMP
    uint8_t  valid;           //  1 B
    uint8_t  _pad[2];         //  2 B
    uint64_t last_use_ms;     //  8 B
    uint32_t timeout_ms;      //  4 B — per-entry timeout
    uint32_t _pad2;           //  4 B
} NatEntry;                   // 40 bytes
```

### `NatState` Struct (per NAT router, ≈ 20 KB)

```c
typedef struct NatState {
    NatEntry   table[512];     // 512 × 40 = 20 480 B
    int        count;
    uint32_t   public_ip;      // the one public IP
    uint16_t   next_port;      // next port to assign (wraps)
    Interface *public_iface;   // WAN-facing NIC
    Simulator *sim;
    Device    *dev;
} NatState;                    // ≈ 20 KB
```

### Public API

| Function                                        | Purpose                                       |
|-------------------------------------------------|-----------------------------------------------|
| `nat_init(sim, dev, public_ip, public_iface)`   | Zero table; bind IP hook; schedule GC.        |
| `nat_outbound(state, pkt)`                      | Translate src; create/refresh entry.          |
| `nat_inbound(state, pkt)`                       | Translate dst; look up entry.                 |
| `nat_gc(state, now)`                            | Invalidate timed-out entries.                 |
| `nat_find_entry_outbound(state, priv_ip, priv_port, proto)` | Lookup for existing session. |
| `nat_find_entry_inbound(state, pub_port, proto)` | Lookup for reverse translation.              |
| `nat_ip_checksum_fixup(hdr, old_src, new_src)`  | Incremental checksum update.                  |

### ACSL Highlights

```
nat_outbound (new session):
  result == 0 ⇒ ∃ i: table[i].private_ip == orig_src_ip
                   && table[i].public_port != 0
                   && pkt->src_ip == public_ip
                   && pkt->src_port == table[i].public_port

nat_inbound (hit):
  ∃ i: table[i].public_port == dst_port && table[i].proto == proto
  ⇒ pkt->dst_ip == table[i].private_ip
  && pkt->dst_port == table[i].private_port

nat_inbound (miss):
  result == -1  ⇒ packet dropped (no mapping)
```

---

## Dispatch Table — Hooking into IP forward

```
ip_forward (outbound, after routing):
   if out_iface == nat_state->public_iface:
       nat_outbound(nat_state, pkt)

ip_receive (inbound on public_iface):
   if iface == nat_state->public_iface:
       nat_inbound(nat_state, pkt)
       then: deliver locally (the private host is the new dst)
```

No new EVT type — NAT intercepts existing IP forward/receive paths.

```
Scheduler.handlers[]
┌────────────────┬───────────────┬───────────┐
│ EVT_NAT_GC     │ nat_gc_handler│ nat_state │  ← fires every 60 000 ms
└────────────────┴───────────────┴───────────┘
```

---

## Function Call Sequence — Outbound (LAN → WAN)

```
H1 (192.168.1.10:5000) sends TCP SYN to 8.8.8.8:80

ip_forward(sim, eth0, pkt):
   │   route found → out_iface = wan_iface (203.0.113.1)
   │
   ├─ out_iface == nat->public_iface:
   └─► nat_outbound(nat_state, pkt)
           │
           ├─► nat_find_entry_outbound(nat, 192.168.1.10, 5000, TCP)
           │       if found: reuse public_port (refresh ts)
           │       else:     assign nat->next_port++; create NatEntry
           │
           │   rewrite pkt: src_ip = 203.0.113.1
           │                src_port = 1024 (assigned)
           ├─► nat_ip_checksum_fixup (IP header)
           ├─► tcp/udp checksum fixup (pseudo-header)
           └─► continue with ethernet_send(...)
```

## Function Call Sequence — Inbound (WAN → LAN)

```
Reply arrives at wan_iface (203.0.113.1): dst_ip=203.0.113.1 dst_port=1024

ip_receive(wan_iface, pkt, sim):
   │   iface == nat->public_iface:
   └─► nat_inbound(nat_state, pkt)
           │
           ├─► nat_find_entry_inbound(nat, 1024, TCP)
           │       found: private_ip=192.168.1.10, private_port=5000
           │
           │   rewrite pkt: dst_ip = 192.168.1.10
           │                dst_port = 5000
           ├─► nat_ip_checksum_fixup
           ├─► tcp/udp checksum fixup
           └─► route to 192.168.1.10 via LAN iface
                   ethernet_send(sim, lan_iface, ...)
```

---

## Design Notes

- **Incremental checksum update** (RFC 1624) avoids recomputing the
  full one's complement sum: `new_cksum = ~(~old + ~old_field + new_field)`.
- **Port exhaustion** — when `next_port` wraps past `NAT_PORT_END`,
  scan for an invalid entry to reclaim. If none, drop the packet.
- **ICMP NAT**: use ICMP `id` field as the "port" for session tracking.
- **FTP/SIP ALG** (application-layer gateway) is out of scope — those
  protocols embed IP addresses in payloads requiring deep inspection.

## Test Plan (kleva)

- `outbound_creates_entry`, `outbound_reuses_existing`
- `inbound_translates_dst`, `inbound_no_entry_drops`
- `gc_removes_timed_out`, `gc_keeps_active`
- `checksum_fixup_valid`, `port_exhaustion_drops`
- NULL guards: `outbound_null_pkt`, `inbound_null_state`
