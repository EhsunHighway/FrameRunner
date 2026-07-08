#ifndef STATIC_ROUTE_H
#define STATIC_ROUTE_H

#include <stdint.h>
#include "../network/interface.h"
#include "../network/router.h"

#define STATIC_ROUTE_MAX_ROUTES 128

typedef struct StaticRouteEntry {
    uint32_t   prefix;      /* host-order normalized prefix */
    uint8_t    prefix_len;  /* CIDR length, 0..32 */
    uint8_t    valid;       /* 1 means this configured route slot is used */
    uint8_t    installed;   /* 1 means ROUTE_PROTO_STATIC was installed */
    uint8_t    _pad;
    uint32_t   next_hop;    /* host order; 0 means directly connected */
    Interface *iface;       /* borrowed egress interface */
    uint32_t   metric;      /* static route preference within static routes */
} StaticRouteEntry;

typedef struct StaticRouteTable {
    StaticRouteEntry routes[STATIC_ROUTE_MAX_ROUTES];
    int              count;
} StaticRouteTable;

/*@
    behavior null:
        assumes table == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(table);
        assigns table->routes[0 .. 127],
                table->count;
        ensures table->count == 0;
        ensures \forall integer i; 0 <= i < 128 ==>
                table->routes[i].valid == 0;
        ensures \forall integer i; 0 <= i < 128 ==>
                table->routes[i].installed == 0;
        ensures \forall integer i; 0 <= i < 128 ==>
                table->routes[i].iface == \null;

    complete behaviors;
    disjoint behaviors;
*/
void static_route_init(StaticRouteTable *table);

/*@
    behavior bad_input:
        assumes table == \null || router == \null || iface == \null ||
                prefix_len > 32;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(table) && \valid(router) && \valid(iface);
        assumes prefix_len <= 32;
        assigns table->routes[0 .. 127],
                table->count,
                router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count,
                router->route_tbl.next_sequence;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int  static_route_add(StaticRouteTable *table,
                      Router           *router,
                      uint32_t          prefix,
                      uint8_t           prefix_len,
                      uint32_t          next_hop,
                      Interface        *iface,
                      uint32_t          metric);

/*@
    behavior bad_input:
        assumes table == \null || router == \null || prefix_len > 32;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(table) && \valid(router);
        assumes prefix_len <= 32;
        assigns table->routes[0 .. 127],
                table->count,
                router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int  static_route_delete(StaticRouteTable *table,
                         Router           *router,
                         uint32_t          prefix,
                         uint8_t           prefix_len);

/*@
    behavior bad_input:
        assumes table == \null || router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(table) && \valid(router);
        assigns table->routes[0 .. 127],
                router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count,
                router->route_tbl.next_sequence;
        ensures \result >= -1;

    complete behaviors;
    disjoint behaviors;
*/
int  static_route_apply(StaticRouteTable *table, Router *router);

/*@
    behavior bad_input:
        assumes table == \null || router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(table) && \valid(router);
        assigns table->routes[0 .. 127],
                table->count,
                router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count;
        ensures \result >= -1;

    complete behaviors;
    disjoint behaviors;
*/
int  static_route_flush(StaticRouteTable *table, Router *router);

#endif /* STATIC_ROUTE_H */
