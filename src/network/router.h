#ifndef ROUTER_H
#define ROUTER_H

#include <stdint.h>
#include "device.h"
#include "interface.h"
#include "packet.h"
#include "../engine/simulator.h"
#include "../protocols/arp_cache.h"
#include "../protocols/ip.h"
#include "../routing/route_table.h"

#define ROUTER_MAX_PORTS 8

typedef struct Router {
    Device     base;
    ArpCache   arp_cache;
    RouteTable route_tbl;
    IpStack    ip_stack;    // local control-plane protocols
    Simulator *sim;
} Router;

/*@
    behavior bad_input:
        assumes name == \null || sim == \null;
        assigns \nothing;
        ensures \result == \null;

    behavior valid:
        assumes name != \null && sim != \null;
        allocates \result;
        ensures \result == \null || \valid(\result);
        ensures \result != \null ==> \result->sim == sim;
        ensures \result != \null ==> \result->base.iface_count == 0;
        ensures \result != \null ==> \result->base.iface_max == 8;
        ensures \result != \null ==> \result->base.interfaces != \null;
        ensures \result != \null ==> \result->arp_cache.count == 0;
        ensures \result != \null ==> \result->arp_cache.pending_count == 0;
        ensures \result != \null ==> \result->route_tbl.rib_count == 0;
        ensures \result != \null ==> \result->route_tbl.fib_count == 0;

    complete behaviors;
    disjoint behaviors;
*/
Router *router_create(const char *name, Simulator *sim);

/*@
    behavior null:
        assumes router == \null;
        assigns \nothing;

    behavior valid:
        assumes \valid(router);
        frees router->base.interfaces[0 .. router->base.iface_count - 1],
              router->base.interfaces,
              router;

    complete behaviors;
    disjoint behaviors;
*/
void    router_free(Router *router);

/*@
    behavior bad_input:
        assumes router == \null || iface == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior full:
        assumes \valid(router) && \valid(iface);
        assumes router->base.iface_count >= router->base.iface_max;
        assigns \nothing;
        ensures \result == -1;

    behavior added:
        assumes \valid(router) && \valid(iface);
        assumes router->base.iface_count < router->base.iface_max;
        assigns router->base.interfaces[0 .. router->base.iface_count],
                router->base.iface_count,
                iface->device,
                iface->arp_cache,
                iface->rx_handler,
                iface->handler_ctx;
        ensures \result == 0 || \result == -1;
        ensures \result == 0 ==>
                router->base.iface_count == \old(router->base.iface_count) + 1;
        ensures \result == 0 ==>
                router->base.interfaces[\old(router->base.iface_count)] == iface;
        ensures \result == 0 ==> iface->device == &router->base;
        ensures \result == 0 ==> iface->arp_cache == &router->arp_cache;

    complete behaviors;
    disjoint behaviors;
*/
int     router_add_interface(Router *router, Interface *iface);

/*@
    behavior bad_input:
        assumes router == \null || in_iface == \null || pkt == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior malformed:
        assumes \valid(router) && \valid(in_iface) && \valid(pkt);
        assumes router->sim == \null ||
                pkt->data == \null ||
                pkt->len < 20;
        assigns in_iface->rx_errors;
        ensures \result == -1;

    behavior readable_ipv4:
        assumes \valid(router) && \valid(in_iface) && \valid(pkt);
        assumes router->sim != \null;
        assumes pkt->data != \null;
        assumes pkt->len >= 20;
        assumes \valid_read(pkt->data + (0 .. pkt->len - 1));
        assigns in_iface->rx_errors,
                in_iface->rx_dropped,
                in_iface->tx_bytes,
                in_iface->last_tx_time,
                pkt->data[0 .. pkt->len - 1],
                router->arp_cache.pending[0 .. 31],
                router->arp_cache.pending_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int     router_receive(Router    *router,
                       Interface *iface,
                       Packet    *pkt);

/*@
    behavior bad_input:
        assumes router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(router);
        assigns router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count,
                router->route_tbl.next_sequence;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int     router_add_route(Router    *router,
                         uint32_t   prefix,
                         uint8_t    prefix_len,
                         uint32_t   next_hop,
                         Interface *iface,
                         uint32_t   metric,
                         uint8_t    proto);

/*@
    behavior bad_input:
        assumes router == \null;
        assigns \nothing;
        ensures \result == -1;

    behavior valid:
        assumes \valid(router);
        assigns router->route_tbl.rib[0 .. 255],
                router->route_tbl.rib_count,
                router->route_tbl.fib[0 .. 255],
                router->route_tbl.fib_count;
        ensures \result == 0 || \result == -1;

    complete behaviors;
    disjoint behaviors;
*/
int     router_del_route(Router    *router,
                         uint32_t   prefix,
                         uint8_t    prefix_len,
                         uint8_t    proto);

#endif /* ROUTER_H */
