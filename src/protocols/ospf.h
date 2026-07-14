#ifndef OSPF_H
#define OSPF_H

#include "../network/router.h"
#include "../network/packet.h"
#include "../network/interface.h"
#include "../engine/simulator.h"


#define OSPF_VERSION             2
#define OSPF_PROTO_NUM           89
#define OSPF_AREA_BACKBONE       0
#define OSPF_ALLSPFROUTERS       0xE0000005u
#define OSPF_ALLDROUTERS         0xE0000006u

#define OSPF_HELLO_INTERVAL_US   10000000ULL
#define OSPF_DEAD_INTERVAL_US    40000000ULL
#define OSPF_SPF_DELAY_US        500000ULL

#define OSPF_LSDB_SIZE           256
#define OSPF_MAX_NEIGHBORS       32
#define OSPF_MAX_IFACES          16
#define OSPF_MAX_LSA_LINKS       4
#define OSPF_INFINITY            0xFFFFFFFFu
#define OSPF_LSA_TYPE_ROUTER     1
#define OSPF_SPF_MAX_NODES       \
    (1 + OSPF_LSDB_SIZE * (1 + OSPF_MAX_LSA_LINKS))

#define OSPF_TYPE_HELLO          1
#define OSPF_TYPE_DBD            2
#define OSPF_TYPE_LSR            3
#define OSPF_TYPE_LSU            4
#define OSPF_TYPE_LSACK          5

#define OSPF_LINK_P2P            1
#define OSPF_LINK_TRANSIT        2
#define OSPF_LINK_STUB           3

typedef struct __attribute__((packed)) OspfHeader {
    uint8_t  version;
    uint8_t  type;
    uint16_t pkt_len;
    uint32_t router_id;
    uint32_t area_id;
    uint16_t checksum;
    uint16_t au_type;
    uint64_t auth_data;
} OspfHeader;

typedef struct __attribute__((packed)) OspfHello {
    uint32_t network_mask;
    uint16_t hello_interval;
    uint8_t  options;
    uint8_t  router_priority;
    uint32_t dead_interval;
    uint32_t dr;
    uint32_t bdr;
} OspfHello;

typedef struct __attribute__((packed)) OspfLsaLink {
    uint32_t link_id;
    uint32_t link_data;
    uint8_t  type;
    uint8_t  num_tos;
    uint16_t metric;
} OspfLsaLink;

typedef enum OspfNbrState {
    OSPF_NBR_DOWN,
    OSPF_NBR_ATTEMPT,
    OSPF_NBR_INIT,
    OSPF_NBR_TWOWAY,
    OSPF_NBR_EXSTART,
    OSPF_NBR_EXCHANGE,
    OSPF_NBR_LOADING,
    OSPF_NBR_FULL
} OspfNbrState;

typedef struct OspfNeighbor {
    uint32_t     router_id;
    uint32_t     ip_addr;
    OspfNbrState state;
    uint64_t     last_hello_ts;
    Interface   *iface;
    int          valid;
} OspfNeighbor;

typedef struct OspfLsaEntry {
    uint32_t    lsa_id;
    uint32_t    adv_router;
    uint32_t    seq_num;
    uint16_t    checksum;
    uint8_t     lsa_type;
    uint8_t     valid;
    OspfLsaLink links[OSPF_MAX_LSA_LINKS];
    int         link_count;
} OspfLsaEntry;

typedef struct OspfSpfVertex {
    uint32_t router_id;
    uint32_t distance;
    int      predecessor;
    int      first_hop;
    uint8_t  visited;
} OspfSpfVertex;

typedef struct __attribute__((packed)) OspfLsuHeader {
    uint16_t lsa_count;
    uint16_t reserved;
} OspfLsuHeader;

typedef struct __attribute__((packed)) OspfLsaWire {
    uint32_t lsa_id;
    uint32_t adv_router;
    uint32_t seq_num;
    uint16_t checksum;
    uint8_t  lsa_type;
    uint8_t  link_count;
} OspfLsaWire;

typedef struct OspfIface {
    Interface *iface;
    uint16_t   cost;
    int        valid;
} OspfIface;

typedef struct OspfState {
    uint32_t     router_id;
    uint32_t     area_id;

    OspfNeighbor neighbors[OSPF_MAX_NEIGHBORS];
    int          neighbor_count;

    OspfLsaEntry lsdb[OSPF_LSDB_SIZE];
    int          lsdb_count;

    OspfIface    ifaces[OSPF_MAX_IFACES];
    int          iface_count;

    Simulator   *sim;
    Router      *router;
} OspfState;


/*@
    predicate ospf_neighbor_count_valid(OspfState *state) =
        0 <= state->neighbor_count && state->neighbor_count <= 32;

    predicate ospf_lsdb_count_valid(OspfState *state) =
        0 <= state->lsdb_count && state->lsdb_count <= 256;

    predicate ospf_iface_count_valid(OspfState *state) =
        0 <= state->iface_count && state->iface_count <= 16;

    predicate ospf_lsa_slot_valid(OspfState *state, integer i) =
        0 <= i && i < 256 ==>
            (state->lsdb[i].valid == 0 ||
             (state->lsdb[i].valid == 1 &&
              0 <= state->lsdb[i].link_count &&
              state->lsdb[i].link_count <= 4));

    predicate ospf_state_well_formed(OspfState *state) =
        \valid(state) &&
        ospf_neighbor_count_valid(state) &&
        ospf_lsdb_count_valid(state) &&
        ospf_iface_count_valid(state) &&
        \forall integer i; 0 <= i && i < 256 ==>
            ospf_lsa_slot_valid(state, i);
*/


/*@
    behavior null:
        assumes state == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(state);
        assigns state->router_id,
                state->area_id,
                state->neighbors[0 .. 31],
                state->neighbor_count,
                state->lsdb[0 .. 255],
                state->lsdb_count,
                state->ifaces[0 .. 15],
                state->iface_count,
                state->sim,
                state->router;
        ensures state->router_id == router_id;
        ensures state->area_id == 0;
        ensures state->neighbor_count == 0;
        ensures state->lsdb_count == 0;
        ensures state->iface_count == 0;
        ensures state->sim == sim;
        ensures state->router == router;

    complete behaviors;
    disjoint behaviors;
*/
void  ospf_init(OspfState *state,
                Simulator *sim,
                Router    *router,
                uint32_t   router_id);

/*@
    behavior bad_input:
        assumes state == \null || iface == \null || cost == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid(iface);
        assumes cost != 0;
        assigns state->ifaces[0 .. 15],
                state->iface_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int  ospf_enable_iface(OspfState *state,
                       Interface *iface,
                       uint16_t   cost);

/*@
    behavior null_iface:
        assumes iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior null_pkt:
        assumes iface != \null && pkt == \null;
        assigns iface->rx_errors;
        ensures \result == -1;

    behavior bad_ctx:
        assumes iface != \null && pkt != \null && ctx == \null;
        assigns \everything;
        ensures \result == -1;

    behavior valid:
        assumes \valid(iface);
        assumes pkt != \null;
        assumes ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns \everything;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int  ospf_receive(Interface *iface,
                  Packet    *pkt,
                  void      *ctx);

/*@
    behavior bad_input:
        assumes state == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid(iface);
        assigns \everything;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int  ospf_send_hello(OspfState *state, Interface *iface);

/*@
    behavior bad_input:
        assumes state == \null || lsa == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes \valid_read(lsa);
        assigns \everything;
        ensures \result >= -1;

    complete behaviors;
    disjoint behaviors;
*/
int  ospf_flood_lsa(OspfState          *state,
                    const OspfLsaEntry *lsa,
                    Interface          *except_iface);

/*@
    behavior bad_input:
        assumes state == \null || state->router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes ospf_state_well_formed(state);
        assumes state->router != \null;
        assigns state->router->route_tbl.rib[0 .. 255],
                state->router->route_tbl.rib_count,
                state->router->route_tbl.fib[0 .. 255],
                state->router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int  ospf_run_spf(OspfState *state);

/*@
    behavior null_input:
        assumes ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns \everything;

    complete behaviors;
    disjoint behaviors;
*/
void ospf_hello_timer(const Event *e, void *ctx);

/*@
    behavior null_input:
        assumes ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns \everything;

    complete behaviors;
    disjoint behaviors;
*/
void ospf_dead_timer(const Event *e, void *ctx);

/*@
    behavior null_input:
        assumes ctx == \null;
        assigns \nothing;

    behavior valid:
        assumes ctx != \null;
        assumes ospf_state_well_formed((OspfState *)ctx);
        assigns \everything;

    complete behaviors;
    disjoint behaviors;
*/
void ospf_spf_timer(const Event *e, void *ctx);

#endif /* OSPF_H */
