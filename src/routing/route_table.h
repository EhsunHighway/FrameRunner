#ifndef ROUTE_TABLE_H
#define ROUTE_TABLE_H

#include "../network/interface.h"
#include <stdint.h>

#define ROUTE_RIB_SIZE              256
#define ROUTE_FIB_SIZE              256
#define ROUTE_LONG_PREFIX_THRESHOLD 24

#define ROUTE_PROTO_STATIC          1
#define ROUTE_PROTO_DIRECT          2
#define ROUTE_PROTO_RIP             3
#define ROUTE_PROTO_OSPF            4
#define ROUTE_PROTO_BGP             5
#define ROUTE_PROTO_EIGRP           6
#define ROUTE_PROTO_ISIS            7

typedef struct RouteRibEntry {
    uint32_t   prefix;      /* host-order normalized prefix */
    uint8_t    prefix_len;  /* CIDR length, 0..32 */
    uint8_t    proto;       /* ROUTE_PROTO_* */
    uint8_t    valid;       /* 1 means this RIB slot is used */
    uint8_t    selected;    /* 1 if this candidate is installed in FIB */
    uint32_t   next_hop;    /* host order; 0 means directly connected */
    Interface *iface;       /* borrowed egress interface */
    uint32_t   metric;      /* protocol-specific cost */
    uint32_t   sequence;    /* insertion order, lower is older */
} RouteRibEntry;

typedef struct RouteFibEntry {
    uint32_t   prefix;      /* host-order normalized prefix */
    uint8_t    prefix_len;  /* CIDR length, 0..32 */
    uint8_t    proto;       /* selected route protocol */
    uint8_t    valid;       /* 1 means this FIB slot participates in lookup */
    uint8_t    _pad;
    uint32_t   next_hop;    /* host order; 0 means directly connected */
    Interface *iface;       /* borrowed egress interface */
    uint32_t   metric;      /* copied from selected RIB entry */
    int        rib_index;   /* selected RIB slot index */
} RouteFibEntry;

typedef struct RouteTable {
    RouteRibEntry rib[ROUTE_RIB_SIZE];
    int           rib_count;

    RouteFibEntry fib[ROUTE_FIB_SIZE];
    int           fib_count;

    uint32_t      next_sequence;
} RouteTable;



/*@
    predicate route_rib_index(integer i) =
        0 <= i && i < 256;

    predicate route_fib_index(integer i) =
        0 <= i && i < 256;

    predicate route_valid_prefix_len(uint8_t prefix_len) =
        prefix_len <= 32;

    predicate route_prefix_is_normalized(uint32_t prefix,
                                         uint8_t prefix_len);

    predicate route_prefix_normalizes(uint32_t input,
                                      uint8_t prefix_len,
                                      uint32_t stored);

    predicate route_prefix_matches(uint32_t ip,
                                   uint32_t prefix,
                                   uint8_t prefix_len);

    predicate route_valid_rib_count(RouteTable *table) =
        0 <= table->rib_count && table->rib_count <= 256;

    predicate route_valid_fib_count(RouteTable *table) =
        0 <= table->fib_count && table->fib_count <= 256;

    predicate route_valid_rib_slot(RouteTable *table, integer i) =
        route_rib_index(i) ==>
            ((table->rib[i].valid == 0 &&
              table->rib[i].selected == 0 &&
              table->rib[i].iface == \null) ||
             (table->rib[i].valid == 1 &&
              route_valid_prefix_len(table->rib[i].prefix_len) &&
              route_prefix_is_normalized(table->rib[i].prefix,
                                         table->rib[i].prefix_len) &&
              table->rib[i].proto != 0 &&
              table->rib[i].iface != \null));

    predicate route_valid_fib_slot(RouteTable *table, integer i) =
        route_fib_index(i) ==>
            ((table->fib[i].valid == 0 &&
              table->fib[i].iface == \null) ||
             (table->fib[i].valid == 1 &&
              route_valid_prefix_len(table->fib[i].prefix_len) &&
              route_prefix_is_normalized(table->fib[i].prefix,
                                         table->fib[i].prefix_len) &&
              table->fib[i].proto != 0 &&
              table->fib[i].iface != \null &&
              0 <= table->fib[i].rib_index &&
              table->fib[i].rib_index < 256));

    predicate route_table_basic_valid(RouteTable *table) =
        \valid(table) &&
        route_valid_rib_count(table) &&
        route_valid_fib_count(table) &&
        (\forall integer i; 0 <= i < 256 ==> route_valid_rib_slot(table, i)) &&
        (\forall integer i; 0 <= i < 256 ==> route_valid_fib_slot(table, i));
*/


/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(table);
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count,
                table->next_sequence;
        ensures table->rib_count == 0;
        ensures table->fib_count == 0;
        ensures table->next_sequence == 1;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].valid == 0;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].selected == 0;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].iface == \null;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->fib[i].valid == 0;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->fib[i].iface == \null;
        ensures route_table_basic_valid(table);

    complete behaviors;
    disjoint behaviors;
*/
void           route_table_init(RouteTable *table);

/*@
    behavior bad_input:
        assumes table == \null || iface == \null ||
                prefix_len > 32 || proto == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid_input:
        assumes route_table_basic_valid(table) && \valid(iface);
        assumes prefix_len <= 32;
        assumes proto != 0;
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count,
                table->next_sequence;
        ensures \result == 0 || \result == -1;
        ensures \result == -1 ==>
                table->rib_count == \old(table->rib_count);
        ensures \result == 0 ==>
                table->rib_count == \old(table->rib_count) ||
                table->rib_count == \old(table->rib_count) + 1;
        ensures \result == 0 ==>
                \exists integer i; 0 <= i < 256 &&
                    table->rib[i].valid == 1 &&
                    route_prefix_normalizes(prefix,
                                            prefix_len,
                                            table->rib[i].prefix) &&
                    table->rib[i].prefix_len == prefix_len &&
                    table->rib[i].proto == proto &&
                    table->rib[i].next_hop == next_hop &&
                    table->rib[i].iface == iface &&
                    table->rib[i].metric == metric;
        ensures route_table_basic_valid(table);

    complete behaviors;
    disjoint behaviors;
*/
int            route_table_add(RouteTable *table,
                               uint32_t    prefix,
                               uint8_t     prefix_len,
                               uint32_t    next_hop,
                               Interface  *iface,
                               uint32_t    metric,
                               uint8_t     proto);

/*@
    behavior bad_input:
        assumes table == \null || prefix_len > 32 || proto == 0;
        assigns \nothing;
        ensures \result == -1;

    behavior valid_input:
        assumes route_table_basic_valid(table);
        assumes prefix_len <= 32;
        assumes proto != 0;
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count;
        ensures \result == 0 || \result == -1;
        ensures \result == -1 ==>
                table->rib_count == \old(table->rib_count);
        ensures \result == 0 ==>
                table->rib_count == \old(table->rib_count) - 1;
        ensures \result == 0 ==>
                \forall integer i; 0 <= i < 256 ==>
                    table->rib[i].valid == 0 ||
                    !route_prefix_normalizes(prefix,
                                             prefix_len,
                                             table->rib[i].prefix) ||
                    table->rib[i].prefix_len != prefix_len ||
                    table->rib[i].proto != proto;
        ensures route_table_basic_valid(table);

    complete behaviors;
    disjoint behaviors;
*/
int            route_table_delete(RouteTable *table,
                                  uint32_t    prefix,
                                  uint8_t     prefix_len,
                                  uint8_t     proto);

/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes route_table_basic_valid(table);
        assigns \nothing;
        ensures \result == \null ||
                \exists integer i; 0 <= i < 256 &&
                    \result == &table->fib[i] &&
                    table->fib[i].valid == 1 &&
                    table->fib[i].prefix_len <= 32 &&
                    route_prefix_matches(dst_ip,
                                         table->fib[i].prefix,
                                         table->fib[i].prefix_len);

    complete behaviors;
    disjoint behaviors;
*/
RouteFibEntry *route_table_lookup(RouteTable *table,
                                  uint32_t    dst_ip);


/*@
    behavior null_or_zero:
        assumes table == \null || proto == 0;
        assigns \nothing;
        ensures \result == 0;

    behavior valid:
        assumes route_table_basic_valid(table);
        assumes proto != 0;
        assigns table->rib[0 .. 255],
                table->rib_count,
                table->fib[0 .. 255],
                table->fib_count;
        ensures \result >= 0;
        ensures \result <= \old(table->rib_count);
        ensures table->rib_count == \old(table->rib_count) - \result;
        ensures \forall integer i; 0 <= i < 256 ==>
                table->rib[i].valid == 0 ||
                table->rib[i].proto != proto;
        ensures route_table_basic_valid(table);

    complete behaviors;
    disjoint behaviors;
*/
int            route_table_flush_proto(RouteTable *table,
                                       uint8_t     proto);

/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes route_table_basic_valid(table);
        assigns table->rib[0 .. 255],
                table->fib[0 .. 255],
                table->fib_count;
        ensures \result == 0;
        ensures table->fib_count >= 0;
        ensures table->fib_count <= table->rib_count;
        ensures \forall integer i; 0 <= i < 256 ==>
                \old(table->rib[i].valid) == 1 ==>
                    table->rib[i].prefix == \old(table->rib[i].prefix) &&
                    table->rib[i].prefix_len == \old(table->rib[i].prefix_len) &&
                    table->rib[i].proto == \old(table->rib[i].proto) &&
                    table->rib[i].next_hop == \old(table->rib[i].next_hop) &&
                    table->rib[i].iface == \old(table->rib[i].iface) &&
                    table->rib[i].metric == \old(table->rib[i].metric) &&
                    table->rib[i].sequence == \old(table->rib[i].sequence);
        ensures \forall integer i; 0 <= i < 256 ==>
                table->fib[i].valid == 0 ||
                (table->fib[i].prefix_len <= 32 &&
                 0 <= table->fib[i].rib_index &&
                 table->fib[i].rib_index < 256 &&
                 table->rib[table->fib[i].rib_index].valid == 1 &&
                 table->rib[table->fib[i].rib_index].selected == 1);
        ensures route_table_basic_valid(table);

    complete behaviors;
    disjoint behaviors;
*/
int            route_table_rebuild_fib(RouteTable *table);

#endif // ROUTE_TABLE_H
