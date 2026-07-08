# Networking Simulator — Architecture

## 1. High-Level System Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        networking_simulator                         │
│                                                                     │
│  ┌──────────┐    ┌──────────────────┐    ┌──────────────────────┐   │
│  │   CLI    │───>│  Simulation Core │───>│    Display Engine    │   │
│  │  (REPL)  │    │  (Event Engine)  │    │  (ASCII Renderer)    │   │
│  └──────────┘    └────────┬─────────┘    └──────────────────────┘   │
│                           │                                         │
│              ┌────────────┼────────────┐                            │
│              ▼            ▼            ▼                            │
│       ┌────────────┐ ┌─────────┐ ┌──────────┐                       │
│       │  Network   │ │Protocol │ │ Routing  │                       │
│       │  Layer     │ │Modules  │ │  Engine  │                       │
│       │ (Topology, │ │(ETH,ARP,│ │(Tables,  │                       │
│       │  Devices,  │ │IP,ICMP, │ │ LPM,     │                       │
│       │  Links)    │ │TCP,UDP) │ │ RIP)     │                       │
│       └────────────┘ └─────────┘ └──────────┘                       │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Module Breakdown

### 2.1 CLI Module (`src/cli/`)

```
┌────────────────────────────────────────────────────┐
│                    CLI Module                      │
│                                                    │
│  cli_run()          ← main REPL loop               │
│  cli_parse()        ← tokenize input               │
│  cli_dispatch()     ← map command → handler        │
│                                                    │
│  Commands:                                         │
│  ┌──────────────────────────────────────────────┐  │
│  │ cmd_send()       → inject send event         │  │
│  │ cmd_step()       → advance one event         │  │
│  │ cmd_run()        → drain event queue         │  │
│  │ cmd_topology()   → show ASCII topology       │  │
│  │ cmd_show_route() → print routing table       │  │
│  │ cmd_show_arp()   → print ARP cache           │  │
│  │ cmd_show_packet()→ print packet journey      │  │
│  │ cmd_inject()     → inject error event        │  │
│  │ cmd_load()       → load topology file        │  │
│  │ cmd_reset()      → clear simulation          │  │
│  └──────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────┘
         │
         ▼ emits events into
┌────────────────────┐
│    Event Queue     │
└────────────────────┘
```

---

### 2.2 Simulation Core / Event Engine (`src/engine/`)

```
┌────────────────────────────────────────────────────────────────┐
│                      Simulation Engine                         │
│                                                                │
│  simulator_step()   ← pop one event, dispatch it               │
│  simulator_run()    ← loop until queue empty or paused         │
│  simulator_reset()  ← free all state                           │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                     Event Queue                          │  │
│  │                                                          │  │
│  │  event_queue_push(event)   ← ordered by timestamp        │  │
│  │  event_queue_pop()         ← returns next event          │  │
│  │  event_queue_peek()        ← inspect without removing    │  │
│  │                                                          │  │
│  │  Internal: min-heap or sorted linked list                │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                   Event Dispatcher                       │  │
│  │                                                          │  │
│  │  dispatch(event) → routes to correct device handler      │  │
│  │                                                          │  │
│  │  EVT_PACKET_SEND     → device_send_handler()             │  │
│  │  EVT_PACKET_RECEIVED → device_recv_handler()             │  │
│  │  EVT_ARP_REQUEST     → arp_request_handler()             │  │
│  │  EVT_ARP_REPLY       → arp_reply_handler()               │  │
│  │  EVT_ROUTING_UPDATE  → rip_update_handler()              │  │
│  │  EVT_LINK_DOWN       → link_down_handler()               │  │
│  │  EVT_TIMER_EXPIRED   → timer_handler()                   │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                   Event Log                              │  │
│  │  Append-only list of all processed events                │  │
│  │  Used by display engine for history view                 │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

---

### 2.3 Network Layer (`src/network/`)

```
┌───────────────────────────────────────────────────────────────┐
│                       Network Layer                           │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Topology  (topology.c/h)                               │  │
│  │                                                         │  │
│  │  topology_load(file)    ← parse config                  │  │
│  │  topology_add_device()  ← add host/switch/router        │  │
│  │  topology_add_link()    ← connect two interfaces        │  │
│  │  topology_find_device() ← lookup by name or IP          │  │
│  │  topology_render()      ← emit to display engine        │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Device  (device.c/h)                                   │  │
│  │                                                         │  │
│  │  device_create(name, type)                              │  │
│  │  device_add_interface(device, mac, ip, prefix_len)      │  │
│  │  device_recv(device, packet, iface) ← entry point       │  │
│  │  device_send(device, packet, iface) ← entry point       │  │
│  │                                                         │  │
│  │  DeviceType: HOST | SWITCH | ROUTER                     │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Interface  (interface.c/h)                             │  │
│  │                                                         │  │
│  │  uint8_t  mac[6]                                        │  │
│  │  uint32_t ip_addr                                       │  │
│  │  uint8_t  prefix_len                                    │  │
│  │  Link    *link           ← connected link               │  │
│  │  char     name[16]       ← e.g. "eth0"                  │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Link  (link.c/h)                                       │  │
│  │                                                         │  │
│  │  Interface *end_a, *end_b                               │  │
│  │  uint32_t   bandwidth_mbps                              │  │
│  │  uint32_t   delay_ms                                    │  │
│  │  float      loss_rate        ← 0.0 to 1.0               │  │
│  │  bool       up                                          │  │
│  │                                                         │  │
│  │  link_transmit(link, packet, src_iface)                 │  │
│  │    → schedules EVT_PACKET_RECEIVED at (now + delay)     │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Packet Buffer  (packet.c/h)                            │  │
│  │                                                         │  │
│  │  packet_create(capacity)                                │  │
│  │  packet_prepend(packet, header, len)  ← encapsulate     │  │
│  │  packet_strip(packet, len)            ← decapsulate     │  │
│  │  packet_clone(packet)                                   │  │
│  │  packet_free(packet)                                    │  │
│  │  packet_checksum(data, len)           ← RFC 1071        │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

---

### 2.4 Protocol Modules (`src/protocols/`)

```
┌───────────────────────────────────────────────────────────────┐
│                    Protocol Modules                           │
│                                                               │
│  Each module exposes:                                         │
│    proto_build_header(packet, params) → prepend header        │
│    proto_parse_header(packet)         → read + strip header   │
│    proto_handle_event(event)          → event handler         │
│    proto_describe(header_ptr)         → return display info   │
│                                                               │
│  ┌─────────────────┐  ┌─────────────────┐                     │
│  │  ethernet.c/h   │  │    arp.c/h      │                     │
│  │                 │  │                 │                     │
│  │  ETH header     │  │  ARP request    │                     │
│  │  EtherType      │  │  ARP reply      │                     │
│  │  CRC check      │  │  ARP table      │                     │
│  │  MAC lookup     │  │  Gratuitous ARP │                     │
│  └─────────────────┘  └─────────────────┘                     │
│                                                               │
│  ┌─────────────────┐  ┌─────────────────┐                     │
│  │    ip.c/h       │  │   icmp.c/h      │                     │
│  │                 │  │                 │                     │
│  │  IPv4 header    │  │  Echo req/reply │                     │
│  │  TTL decrement  │  │  TTL exceeded   │                     │ 
│  │  Checksum       │  │  Dest unreach   │                     │ 
│  │  Fragmentation  │  │                 │                     │
│  │  (display only) │  │                 │                     │
│  └─────────────────┘  └─────────────────┘                     │
│                                                               │
│  ┌─────────────────┐  ┌─────────────────┐                     │
│  │    tcp.c/h      │  │    udp.c/h      │                     │
│  │                 │  │                 │                     │
│  │  TCP header     │  │  UDP header     │                     │
│  │  3-way handshake│  │  Stateless send │                     │
│  │  Seq/Ack nums   │  │  Checksum       │                     │
│  │  Flags (SYN/ACK)│  │                 │                     │
│  └─────────────────┘  └─────────────────┘                     │
│                                                               │
│  ┌─────────────────┐                                          │
│  │    rip.c/h      │  ← Phase 1 dynamic routing               │
│  │                 │                                          │
│  │  RIP message    │                                          │
│  │  Distance vector│                                          │
│  │  Periodic timer │                                          │
│  │  Convergence    │                                          │
│  └─────────────────┘                                          │
│                                                               │
│  ┌─────────────────┐                                          │
│  │  [ospf.c/h]     │  ← Phase 2 module (stub registered)      │
│  │  [bgp.c/h]      │  ← Phase 3 module                        │
│  └─────────────────┘                                          │
└───────────────────────────────────────────────────────────────┘
```

---

### 2.5 Routing Engine (`src/routing/`)

```
┌───────────────────────────────────────────────────────────────┐
│                      Routing Engine                           │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Route Table  (route_table.c/h)                         │  │
│  │                                                         │  │
│  │  route_table_add(prefix, prefix_len, nexthop, iface,    │  │
│  │                  metric, proto)                         │  │
│  │  route_table_remove(prefix, prefix_len)                 │  │
│  │  route_table_lookup(dst_ip)  ← Longest Prefix Match     │  │
│  │  route_table_dump(device)    ← print to display         │  │
│  │                                                         │  │
│  │  Internal: sorted array or trie                         │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Static Route  (static_route.c/h)                       │  │
│  │                                                         │  │
│  │  static_route_init(table)                               │  │
│  │  static_route_add(table, router, prefix, prefix_len,    │  │
│  │                   next_hop, iface, metric)              │  │
│  │  static_route_delete(table, router, prefix, prefix_len) │  │
│  │  static_route_apply(table, router)                      │  │
│  │  static_route_flush(table, router)                      │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

---

### 2.6 Display Engine (`src/display/`)

```
┌───────────────────────────────────────────────────────────────┐
│                      Display Engine                           │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Topology View  (topology_view.c/h)                     │  │
│  │                                                         │  │
│  │  topology_view_render(topology)                         │  │
│  │                                                         │  │
│  │  Example output:                                        │  │
│  │    [host_a]──eth0──[sw1]──eth1──[router1]──[sw2]──[host_b] │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Header View  (header_view.c/h)                         │  │
│  │                                                         │  │
│  │  header_view_render(proto_type, header_ptr)             │  │
│  │                                                         │  │
│  │  Example output:                                        │  │
│  │  ┌────────────────────────────────┐                     │  │
│  │  │ IPv4 Header                    │                     │  │
│  │  │  Version : 4                   │                     │  │
│  │  │  IHL     : 5 (20 bytes)        │                     │  │
│  │  │  TTL     : 64                  │                     │  │
│  │  │  Protocol: 6 (TCP)             │                     │  │
│  │  │  Src IP  : 192.168.1.2         │                     │  │
│  │  │  Dst IP  : 192.168.2.5         │                     │  │
│  │  │  Checksum: 0x1A2B [valid]      │                     │  │
│  │  └────────────────────────────────┘                     │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Event Log  (event_log.c/h)                             │  │
│  │                                                         │  │
│  │  event_log_append(event)                                │  │
│  │  event_log_dump(count)     ← show last N events         │  │
│  │                                                         │  │
│  │  Example output:                                        │  │
│  │  [t=0us]  host_a → sw1     EVT_PACKET_SEND              │  │
│  │  [t=1us]  sw1    → router1 EVT_PACKET_RECEIVED          │  │
│  │  [t=2us]  router1          EVT_ARP_REQUEST              │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

---

## 3. Data Flow — Packet Send (Host A → Host B)

```
User: "send host_a host_b tcp"
         │
         ▼
    CLI Module
    cmd_send()
         │ creates EVT_PACKET_SEND
         ▼
    Event Queue
         │ simulator_step()
         ▼
    host_a device handler
         │
         ├─ [Transport] tcp_build_header(packet, src_port, dst_port, ...)
         │    → packet_prepend(packet, tcp_hdr, sizeof(TCPHeader))
         │    → display: TCP header box
         │
         ├─ [Network] ip_build_header(packet, src_ip, dst_ip, ...)
         │    → packet_prepend(packet, ip_hdr, sizeof(IPHeader))
         │    → display: IP header box
         │
         ├─ [ARP check] Does host_a know dst MAC?
         │    NO → push EVT_ARP_REQUEST, suspend packet
         │    YES ↓
         │
         ├─ [Data Link] eth_build_header(packet, src_mac, dst_mac, ...)
         │    → packet_prepend(packet, eth_hdr, sizeof(EtherHeader))
         │    → display: Ethernet header box
         │
         └─ [Physical] link_transmit(link, packet)
              → push EVT_PACKET_RECEIVED at (now + link.delay_ms)
                        │
                        ▼
               sw1 device handler
                        │
                  [Data Link] eth_parse_header(packet)
                  MAC table lookup → forward out port
                  eth_build_header (same headers pass through)
                        │
                        ▼
               router1 device handler
                        │
                  [Data Link] eth_parse_header, strip header
                  [Network]   ip_parse_header
                              TTL--
                              route_table_lookup(dst_ip)
                              → next hop found: sw2/eth0
                  [Data Link] eth_build_header (new MACs)
                        │
                        ▼
               sw2 → host_b device handler
                        │
                  [Data Link] eth_parse_header, strip
                  [Network]   ip_parse_header, strip
                  [Transport] tcp_parse_header, strip
                  Payload delivered → display: "Data received"
```

---

## 4. Event Type Hierarchy

```
EventType
├── Packet Events
│   ├── EVT_PACKET_SEND        (user-triggered or protocol-triggered)
│   └── EVT_PACKET_RECEIVED    (scheduled by link_transmit)
│
├── ARP Events
│   ├── EVT_ARP_REQUEST
│   └── EVT_ARP_REPLY
│
├── Routing Events
│   ├── EVT_ROUTING_UPDATE     (RIP/OSPF periodic)
│   └── EVT_ROUTE_ADDED
│
├── Link Events
│   ├── EVT_LINK_UP
│   └── EVT_LINK_DOWN
│
├── Timer Events
│   └── EVT_TIMER_EXPIRED      (used by RIP hello, TCP retransmit)
│
└── Control Events
    ├── EVT_RENDER             (trigger display refresh)
    └── EVT_RESET
```

---

## 5. Protocol Plugin Interface

Every protocol module must implement this interface to integrate with the engine:

```c
typedef struct ProtocolPlugin {
    const char  *name;                     // e.g. "rip", "ospf"
    EventType    handled_events[8];        // events this plugin handles
    int          event_count;

    // Called by dispatcher for each relevant event
    void (*handle_event)(Event *event);

    // Called by display engine to render headers
    void (*render_header)(void *header_ptr, char *buf, size_t buflen);
} ProtocolPlugin;

// Register a plugin at startup
void engine_register_plugin(ProtocolPlugin *plugin);
```

Adding BGP in Phase 3 = implement `ProtocolPlugin` in `bgp.c`, register it in `main.c`. No other files change.

---

## 6. Phase 2: Per-Device Threads

```
┌──────────────────────────────────────────────────────────────────┐
│  Each Device Thread (Phase 2)                                    │
│                                                                  │
│  ┌────────────┐   push event    ┌──────────────────────────────┐ │
│  │  External  │ ─────────────>  │  Device Event Queue          │ │
│  │  (other    │                 │  (mutex + cond_var protected)│ │
│  │   devices) │                 └──────────────┬───────────────┘ │
│  └────────────┘                               │                  │
│                                               ▼                  │
│                                    ┌──────────────────┐          │
│                                    │  Device Thread   │          │
│                                    │  event loop:     │          │
│                                    │  while(running){ │          │
│                                    │    wait(cond);   │          │
│                                    │    e=queue_pop();│          │
│                                    │    dispatch(e);  │          │
│                                    │  }               │          │
│                                    └──────────────────┘          │
│                                                                  │
│  Shared state (topology, routing tables) protected by rwlock     │
└──────────────────────────────────────────────────────────────────┘
```

---

## 7. Source File Dependency Graph

```
main.c
  ├── cli/cli.c
  │     └── cli/commands.c
  │           ├── engine/simulator.c
  │           │     ├── engine/event.c
  │           │     └── engine/scheduler.c
  │           ├── network/topology.c
  │           │     ├── network/device.c
  │           │     │     ├── network/interface.c
  │           │     │     └── network/link.c
  │           │     └── network/packet.c
  │           ├── protocols/ethernet.c
  │           ├── protocols/arp.c
  │           ├── protocols/ip.c
  │           ├── protocols/icmp.c
  │           ├── protocols/tcp.c
  │           ├── protocols/udp.c
  │           ├── protocols/rip.c
  │           ├── routing/route_table.c
  │           └── routing/static_route.c
  └── display/
        ├── topology_view.c
        ├── header_view.c
        └── event_log.c
```
