#ifndef RIP_H
#define RIP_H

#include "../network/router.h"
#include "../protocols/udp.h"
#include "../engine/simulator.h"


#define RIP_PORT               520
#define RIP_VERSION            2
#define RIP_CMD_REQUEST        1
#define RIP_CMD_RESPONSE       2
#define RIP_AFI_IPV4           2
#define RIP_MULTICAST          0xE0000009u
#define RIP_INFINITY           16
#define RIP_UPDATE_INTERVAL_US 30000000ULL
#define RIP_TIMEOUT_US         180000000ULL
#define RIP_GC_US              120000000ULL
#define RIP_MAX_IFACES         16
#define RIP_DB_SIZE            128
#define RIP_MAX_ROUTES         25
#define RIP_HDR_LEN            4
#define RIP_ENTRY_LEN          20

/*
 * RIP route states
 */
#define RIP_ROUTE_ACTIVE  1
#define RIP_ROUTE_GC      2


typedef struct __attribute__((packed)) RipHeader {
    uint8_t  command;          // 1=request, 2=response
    uint8_t  version;          // 2
    uint16_t zero;
} RipHeader;

typedef struct __attribute__((packed)) RipEntry {
    uint16_t afi;              // 2 = IPv4
    uint16_t route_tag;        // 0
    uint32_t ip_addr;          // prefix/network address
    uint32_t subnet_mask;
    uint32_t next_hop;         // next hop, zero means use sender
    uint32_t metric;           // 1..16
} RipEntry;

typedef struct RipRouteInfo {
    uint32_t   prefix;
    uint8_t    prefix_len;
    uint8_t    state;
    uint8_t    valid;
    uint8_t    _pad;
    uint32_t   metric;
    uint32_t   next_hop;
    Interface *learned_on;
    uint64_t   last_update;
} RipRouteInfo;

typedef struct RipState {
    RipRouteInfo db[RIP_DB_SIZE];
    int          db_count;

    Interface   *ifaces[RIP_MAX_IFACES];
    int          iface_count;

    Simulator   *sim;
    Router      *router;
    UdpState    *udp_state;
} RipState;


/*@
    predicate rip_db_count_valid(RipState *state) =
        0 <= state->db_count && state->db_count <= 128;

    predicate rip_iface_count_valid(RipState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate rip_route_slot_valid(RipState *state, integer i) =
        0 <= i && i < 128 ==>
            (state->db[i].valid == 0 ||
             (state->db[i].valid == 1 &&
              state->db[i].prefix_len <= 32 &&
              1 <= state->db[i].metric &&
              state->db[i].metric <= 16));

    predicate rip_state_well_formed(RipState *state) =
        \valid(state) &&
        rip_db_count_valid(state) &&
        rip_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 128 ==>
            rip_route_slot_valid(state, i);
*/


/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->db[0 .. 127],
                state->db_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->sim,
                state->router,
                state->udp_state;
        ensures state->db_count == 0;
        ensures state->iface_count == 0;
        ensures state->sim == sim;
        ensures state->router == router;
        ensures state->udp_state == udp_state;
        ensures \forall integer i; 0 <= i && i < 128 ==>
                state->db[i].valid == 0;

    complete behaviors;
    disjoint behaviors;
*/
void rip_init(RipState  *state,
              Simulator *sim,
              Router    *router,
              UdpState  *udp_state);

/*@
    behavior bad_input:
        assumes state == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes rip_state_well_formed(state);
        assumes \valid(iface);
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                state->iface_count == \old(state->iface_count) ||
                state->iface_count == \old(state->iface_count) + 1;

    complete behaviors;
    disjoint behaviors;
*/
int rip_enable_iface(RipState *state, Interface *iface);

/*@
    behavior null_payload:
        assumes payload == \null;
        assigns \nothing;

    behavior bad_input:
        assumes payload != \null && (ctx == \null || iface == \null);
        assigns \everything;

    behavior valid:
        assumes payload != \null;
        assumes ctx != \null;
        assumes iface != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns \everything;
*/
void rip_receive(Interface *iface,
                 uint32_t  src_ip,
                 uint16_t  src_port,
                 Packet    *payload,
                 void      *ctx);

/*@
    behavior bad_input:
        assumes state == \null || out_iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes rip_state_well_formed(state);
        assumes \valid(out_iface);
        assigns \everything;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int rip_send_update(RipState *state, Interface *out_iface);

/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns \everything;
*/
void rip_update_handler(const Event *e, void *ctx);

/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns \everything;
*/
void rip_timeout_handler(const Event *e, void *ctx);

/*@
    behavior null_input:
        assumes e == \null || ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes e != \null && ctx != \null;
        assumes rip_state_well_formed((RipState *)ctx);
        assigns \everything;
*/
void rip_gc_handler(const Event *e, void *ctx);


#endif // RIP_H
