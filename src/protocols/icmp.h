#ifndef ICMP_H
#define ICMP_H

#include <stddef.h>
#include <stdint.h>

#include "ip.h"

#define ICMP_ECHO_REPLY           0   /* Echo Reply */
#define ICMP_DEST_UNREACH         3   /* Destination Unreachable */
#define ICMP_ECHO_REQUEST         8   /* Echo Request */
#define ICMP_TIME_EXCEEDED        11  /* Time Exceeded */

#define ICMP_CODE_NET_UNREACH     0   /* No route to network */
#define ICMP_CODE_HOST_UNREACH    1   /* Host unreachable */
#define ICMP_CODE_PROTO_UNREACH   2   /* Protocol unsupported */
#define ICMP_CODE_PORT_UNREACH    3   /* UDP/TCP port closed */
#define ICMP_CODE_FRAG_NEEDED     4   /* DF set and packet too large */
#define ICMP_CODE_TTL_EXCEEDED    0   /* TTL expired in transit */

#define ICMP_HDR_LEN              8
#define ICMP_ORIG_QUOTE_LEN       28  /* IPv4 header + first 8 payload bytes */

typedef struct __attribute__((packed)) IcmpHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;                      /* Echo id, or unused for most errors. */
    uint16_t seq;                     /* Echo seq, or next-hop MTU for Frag Needed. */
} IcmpHeader;


/*@
    behavior null_iface:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior null_pkt:
        assumes iface != \null && pkt == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior too_short:
        assumes \valid(iface) && \valid(pkt);
        assumes pkt->len < ICMP_HDR_LEN;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior missing_stripped_ip:
        assumes \valid(iface) && \valid(pkt);
        assumes pkt->len >= ICMP_HDR_LEN;
        assumes pkt->data < pkt->head + IP_HDR_LEN;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior readable_message:
        assumes \valid(iface) && \valid(pkt);
        assumes pkt->len >= ICMP_HDR_LEN;
        assumes pkt->data >= pkt->head + IP_HDR_LEN;
        assumes \valid_read(pkt->data + (0 .. pkt->len - 1));
        assigns iface->rx_errors, iface->rx_dropped;
        ensures \result == 0 || \result == -1;
*/
int      icmp_receive(Interface *iface,
                      Packet    *pkt,
                      void      *ctx);

/*@
    behavior null_input:
        assumes sim == \null || (payload_len > 0 && payload == \null);
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(sim);
        assumes payload_len == 0 ||
                \valid_read(payload + (0 .. payload_len - 1));
        assigns \nothing;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int      icmp_send_echo_request(Simulator     *sim,
                                uint32_t       src_ip,
                                uint32_t       dst_ip,
                                uint16_t       id,
                                uint16_t       seq,
                                const uint8_t *payload,
                                size_t         payload_len);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || req_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior too_short:
        assumes \valid(sim) && \valid(iface) && \valid(req_pkt);
        assumes req_pkt->len < ICMP_HDR_LEN;
        assigns iface->tx_errors;
        ensures \result == -1;

    behavior missing_stripped_ip:
        assumes \valid(sim) && \valid(iface) && \valid(req_pkt);
        assumes req_pkt->len >= ICMP_HDR_LEN;
        assumes req_pkt->data < req_pkt->head + IP_HDR_LEN;
        assigns iface->tx_errors;
        ensures \result == -1;

    behavior valid_echo:
        assumes \valid(sim) && \valid(iface) && \valid(req_pkt);
        assumes req_pkt->len >= ICMP_HDR_LEN;
        assumes req_pkt->data >= req_pkt->head + IP_HDR_LEN;
        assumes \valid_read(req_pkt->data + (0 .. req_pkt->len - 1));
        assumes ((IcmpHeader *)req_pkt->data)->type == ICMP_ECHO_REQUEST;
        assumes ((IcmpHeader *)req_pkt->data)->code == 0;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_echo_reply(Simulator *sim,
                              Interface *iface,
                              Packet    *req_pkt);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior readable_original:
        assumes \valid(sim) && \valid(iface) && \valid(orig_pkt);
        assumes orig_pkt->data >= orig_pkt->head + IP_HDR_LEN;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_time_exceeded(Simulator *sim,
                                 Interface *iface,
                                 Packet    *orig_pkt);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior readable_original:
        assumes \valid(sim) && \valid(iface) && \valid(orig_pkt);
        assumes orig_pkt->data >= orig_pkt->head + IP_HDR_LEN;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_unreach_net(Simulator *sim,
                               Interface *iface,
                               Packet    *orig_pkt);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior readable_original:
        assumes \valid(sim) && \valid(iface) && \valid(orig_pkt);
        assumes orig_pkt->data >= orig_pkt->head + IP_HDR_LEN;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_unreach_host(Simulator *sim,
                                Interface *iface,
                                Packet    *orig_pkt);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior readable_original:
        assumes \valid(sim) && \valid(iface) && \valid(orig_pkt);
        assumes orig_pkt->data >= orig_pkt->head + IP_HDR_LEN;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_unreach_proto(Simulator *sim,
                                 Interface *iface,
                                 Packet    *orig_pkt);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior readable_original:
        assumes \valid(sim) && \valid(iface) && \valid(orig_pkt);
        assumes orig_pkt->data >= orig_pkt->head + IP_HDR_LEN;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_unreach_port(Simulator *sim,
                                Interface *iface,
                                Packet    *orig_pkt);

/*@
    behavior null_input:
        assumes sim == \null || iface == \null || orig_pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior readable_original:
        assumes \valid(sim) && \valid(iface) && \valid(orig_pkt);
        assumes orig_pkt->data >= orig_pkt->head + IP_HDR_LEN;
        assigns iface->tx_bytes, iface->last_tx_time, iface->tx_errors;
        ensures \result == 0 || \result == -1;
*/
int      icmp_send_frag_needed(Simulator *sim,
                               Interface *iface,
                               Packet    *orig_pkt,
                               uint16_t   next_hop_mtu);

/*@
    behavior null:
        assumes data == \null;
        assigns \nothing;
        ensures \result == 0xFFFF;

    behavior empty:
        assumes data != \null && len == 0;
        assigns \nothing;
        ensures \result == 0xFFFF;

    behavior valid:
        assumes data != \null && len > 0;
        assumes \valid_read(((const uint8_t *)data) + (0 .. len - 1));
        assigns \nothing;
        ensures 0 <= \result <= 0xFFFF;

    complete behaviors;
    disjoint behaviors;
*/
uint16_t icmp_checksum(const void *data, size_t len);

#endif /* ICMP_H */