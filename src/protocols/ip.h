#ifndef IP_H
#define IP_H    

#include <stdint.h>
#include <stddef.h>
#include "../network/packet.h"
#include "../network/interface.h"
#include "../engine/simulator.h"
#include "../network/device.h"
#include "ethernet.h"

#define IP_HDR_LEN         20
#define IP_VERSION         4
#define IPPROTO_ICMP       1
#define IPPROTO_TCP        6
#define IPPROTO_UDP        17
#define IPPROTO_OSPF       89
#define IP_DEFAULT_TTL     64
#define IP_FLAG_DF         0x4000   // Don't Fragment flag
#define IP_MAX_PACKET_SIZE 65535    // Maximum size of an IP packet (header + payload)
#define IP_MULTICAST_BASE  0xE0000000u
#define IP_MULTICAST_MASK  0xF0000000u


typedef int (*IpProtocolHandler)(Interface *iface,
                                 Packet    *pkt,
                                 void      *ctx);

typedef struct IpProtocolEntry {
    IpProtocolHandler handler;
    void             *ctx;
} IpProtocolEntry;

typedef struct IpStack {
    Simulator       *sim;
    IpProtocolEntry  protocols[256];
} IpStack;

typedef struct __attribute__((packed)) IpHeader {
    uint8_t  version_ihl;           // Version (4 bits) + Internet Header Length (4 bits)
    uint8_t  dscp_ecn;              // Differentiated Services Code Point (DSCP) (6 bits) + Explicit Congestion Notification (ECN) (2 bits)
    uint16_t total_length;          // Total length of the IP packet (header + payload)
    uint16_t identification;        // Identification for fragmentation
    uint16_t flags_fragment_offset; // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t  ttl;                   // Time to Live - hop limit
    uint8_t  protocol;              // Protocol number (e.g., 6 for TCP, 17 for UDP)
    uint16_t header_checksum;       // Header checksum
    uint32_t src_ip;                // Source IP address (network byte order)
    uint32_t dst_ip;                // Destination IP address (network byte order)
} IpHeader;

/*@
    axiomatic IpLogicPredicates {
        predicate ip_header_checksum_valid{L}(IpHeader *ip_hdr);
        predicate ip_addr_is_multicast(uint32_t ip_addr) =
            ((ip_addr & IP_MULTICAST_MASK) == IP_MULTICAST_BASE);
        predicate ip_subnet_mask_contiguous(uint32_t mask);
        predicate ip_mask_prefix_length(uint32_t mask, uint8_t prefix_len);
    }
*/


/*@ 
    behavior null:
        assumes sim == \null || sim->topo == \null || stack == \null;
        assigns \nothing;
    behavior registers_handlers:
        assumes \valid(sim) && \valid(sim->topo) && \valid(stack);
        assigns sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->rx_handler,
                sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->handler_ctx,
                stack->sim,
                stack->protocols[0 .. 255];
    complete behaviors;
    disjoint behaviors;
*/
void ip_init(Simulator *sim, IpStack *stack);

/*@
    behavior null:
        assumes stack == \null;
        assigns \nothing;
    behavior valid:
        assumes \valid(stack);
        assigns stack->sim, stack->protocols[0 .. 255];
        ensures stack->sim == sim;
    complete behaviors;
    disjoint behaviors;
*/

void ip_stack_init(IpStack *stack, Simulator *sim);

/*@
    behavior null:
        assumes stack == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(stack) && \valid(iface);
        assigns iface->rx_handler, iface->handler_ctx;
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_stack_bind_interface(IpStack *stack, Interface *iface);

/*@
    behavior null:
        assumes stack == \null || handler == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(stack) && handler != \null;
        assigns stack->protocols[protocol];
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_stack_register_protocol(IpStack           *stack,
                                uint8_t            protocol,
                                IpProtocolHandler  handler,
                                void              *ctx);

/*@
    behavior null:
        assumes stack == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(stack);
        assigns stack->protocols[protocol];
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_stack_unregister_protocol(IpStack *stack, uint8_t protocol);

/*@ 
    behavior null:
        assumes iface == \null || frame == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior too_short:
        assumes \valid(iface) && \valid(frame) && frame->len < IP_HDR_LEN;
        assigns \nothing;
        ensures \result == -1;  
    behavior bad_version:
        assumes \valid(iface) && \valid(frame) && frame->len >= IP_HDR_LEN;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 != IP_VERSION;
        assigns iface->rx_errors;
        ensures \result == -1;
    behavior ttl_zero:
        assumes \valid(iface) && \valid(frame) && frame->len >= IP_HDR_LEN;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 == IP_VERSION;
        assumes ((IpHeader *)frame->data)->ttl == 0;
        assigns iface->rx_dropped;
        ensures \result == -1;
    behavior bad_checksum:
        assumes \valid(iface) && \valid(frame) && frame->len >= IP_HDR_LEN;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 == IP_VERSION;
        assumes ((IpHeader *)frame->data)->ttl != 0;
        assumes ((IpHeader *)frame->data)->header_checksum == 0xFFFF;
        assigns iface->rx_errors;
        ensures \result == -1;
    behavior valid:
        assumes \valid(iface) && \valid(frame) && frame->len >= 20;
        assumes ((IpHeader *)frame->data)->version_ihl >> 4 == IP_VERSION;
        assumes ((IpHeader *)frame->data)->ttl != 0;
        assumes ((IpHeader *)frame->data)->header_checksum != 0xFFFF;
        assigns iface->rx_bytes, iface->last_rx_time, frame->data, frame->len, frame->layer;
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/

int  ip_receive(Interface *iface,
                 Packet   *frame,
                 uint16_t  ethertype,
                 void     *ctx);

/*@
    behavior null:
        assumes sim == \null || iface == \null || dst_mac == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior valid:
        assumes \valid(sim) && \valid(iface) && \valid_read(dst_mac+(0..5)) && \valid(payload);
        assigns payload->data, payload->len, payload->capacity, iface->tx_bytes;
        ensures \result >= 0 || \result == -1;
        ensures \result >= 0 ==> payload->layer == 2;
        ensures \result >= 0 ==> payload->len == \old(payload->len) + 20 + ETH_HDR_LEN;
    behavior prepend_failed:
        assumes \valid(sim) && \valid(iface) && \valid_read(dst_mac+(0..5)) && \valid(payload);
        assumes (size_t)(payload->data - payload->head) < sizeof(IpHeader);
        assigns \nothing;
        ensures \result == -1;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_send(Simulator *sim,
             Interface *iface,
             uint8_t    dst_mac[6],
             uint32_t   src_ip,
             uint32_t   dst_ip,
             uint8_t    protocol,
             Packet    *payload);

/*@
    behavior null:
        assumes sim == \null || payload == \null;
        assigns \nothing;
        ensures \result == -1;
    behavior no_topology:
        assumes \valid(sim) && sim->topo == \null && \valid(payload);
        assigns \nothing;
        ensures \result == -1;
    behavior direct_output:
        assumes \valid(sim) && sim->topo != \null && \valid(payload);
        assigns payload->data, payload->len, payload->capacity, payload->layer,
                sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->tx_bytes,
                sim->topo->devices[0 .. sim->topo->dev_count-1]->interfaces[0 ..]->last_tx_time;
        ensures \result >= 0 || \result == -1;
        ensures \result > 0 ==> payload->layer == 2;
        ensures \result > 0 ==> payload->len == \old(payload->len) + IP_HDR_LEN + ETH_HDR_LEN;
        ensures \result == 0 ==> payload->layer == \old(payload->layer);
        ensures \result == 0 ==> payload->len == \old(payload->len);
    complete behaviors;
    disjoint behaviors;
*/
int  ip_output(Simulator *sim,
                uint32_t  src_ip,
                uint32_t  dst_ip,
                uint8_t   protocol,
                Packet   *payload);

/*@
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 <==> ip_addr_is_multicast(ip_addr);
*/
int       ip_is_multicast(uint32_t ip_addr);

/*@
    behavior bad_input:
        assumes out_mac == \null || !ip_addr_is_multicast(ip_addr);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(out_mac + (0 .. 5));
        assumes ip_addr_is_multicast(ip_addr);
        assigns out_mac[0 .. 5];
        ensures \result == 0;
        ensures out_mac[0] == 0x01;
        ensures out_mac[1] == 0x00;
        ensures out_mac[2] == 0x5e;
        ensures out_mac[3] == ((ip_addr >> 16) & 0x7f);
        ensures out_mac[4] == ((ip_addr >> 8) & 0xff);
        ensures out_mac[5] == (ip_addr & 0xff);

    complete behaviors;
    disjoint behaviors;
*/
int       ip_multicast_to_mac(uint32_t ip_addr, uint8_t out_mac[6]);

/*@
    behavior bad_input:
        assumes out_prefix_len == \null || !ip_subnet_mask_contiguous(mask);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(out_prefix_len);
        assumes ip_subnet_mask_contiguous(mask);
        assigns *out_prefix_len;
        ensures \result == 0;
        ensures *out_prefix_len <= 32;
        ensures ip_mask_prefix_length(mask, *out_prefix_len);

    complete behaviors;
    disjoint behaviors;
*/
int       ip_mask_to_prefix_len(uint32_t mask, uint8_t *out_prefix_len);

/*@
    behavior null_or_short:
        assumes pkt == \null || pkt->data == \null || pkt->len < IP_HDR_LEN;
        assigns \nothing;
        ensures \result == -1;

    behavior bad_header:
        assumes \valid(pkt);
        assumes pkt->data != \null;
        assumes pkt->len >= IP_HDR_LEN;
        assumes \valid_read(pkt->data + (0 .. IP_HDR_LEN - 1));
        assumes (((IpHeader *)pkt->data)->version_ihl >> 4) != IP_VERSION ||
                (((IpHeader *)pkt->data)->version_ihl & 0x0F) !=
                    (IP_HDR_LEN / 4) ||
                !ip_header_checksum_valid((IpHeader *)pkt->data);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(pkt);
        assumes pkt->data != \null;
        assumes pkt->len >= IP_HDR_LEN;
        assumes \valid_read(pkt->data + (0 .. IP_HDR_LEN - 1));
        assumes ((IpHeader *)pkt->data)->version_ihl >> 4 == IP_VERSION;
        assumes (((IpHeader *)pkt->data)->version_ihl & 0x0F) ==
                (IP_HDR_LEN / 4);
        assumes ip_header_checksum_valid((IpHeader *)pkt->data);
        assigns \nothing;
        ensures \result == 0;

    complete behaviors;
    disjoint behaviors;
*/
int       ip_validate_header(Packet *pkt);

/*@
    behavior null:
        assumes ip_hdr == \null;
        assigns \nothing;
        ensures \result == 0xFFFF; 
    behavior valid:
        assumes \valid(ip_hdr);
        assigns \nothing;
        ensures \result == 0 || \result != 0;
        ensures ip_hdr->version_ihl == \old(ip_hdr->version_ihl);
        ensures ip_hdr->ttl == \old(ip_hdr->ttl);
    complete behaviors;
    disjoint behaviors;
*/
uint16_t  ip_checksum(IpHeader *ip_hdr);

/*@
    behavior null:
        assumes dev == \null;
        assigns \nothing;
        ensures \result == 0;
    behavior hit:
        assumes \valid(dev);
        assumes \exists integer i; 0 <= i < dev->iface_count &&
                dev->interfaces[i]->ip_addr == dst_ip;
        assigns \nothing;
        ensures \result == 1;
    behavior miss:
        assumes \valid(dev);
        assumes \forall integer i; 0 <= i < dev->iface_count ==>
                dev->interfaces[i]->ip_addr != dst_ip;
        assigns \nothing;
        ensures \result == 0;
    complete behaviors;
    disjoint behaviors;
*/
int  ip_is_local(Device *dev, uint32_t dst_ip);

#endif /* IP_H */
