#include "router.h"
#include "../protocols/arp.h"
#include "../protocols/ip.h"
#include "../protocols/icmp.h"
#include "../protocols/ethernet.h"
#include <stdlib.h>
#include <string.h>

static int router_ip_is_local(const Router *router, uint32_t dst_ip) {
    if (!router) {
        return 0;
    }

    for (int i = 0; i < router->base.iface_count; i++) {
        Interface *iface = router->base.interfaces[i];
        if (iface && ns_ntohl(iface->ip_addr) == dst_ip) {
            return 1;
        }
    }

    return 0;
}

static int router_ip_is_control_multicast(uint32_t dst_ip) {
    return dst_ip == 0xE0000005u ||
           dst_ip == 0xE0000006u;
}

static void router_rx_shim(Interface *iface,
                           Packet    *pkt,
                           uint16_t   ethertype,
                           void      *ctx) {
    if (!pkt) {
        return;
    }

    if (!iface) {
        packet_free(pkt);
        return;
    }

    if (!ctx) {
        packet_free(pkt);
        iface->rx_dropped++;
        return;
    }

    Router *router = (Router *)ctx;
    if (ethertype != ETHERTYPE_IPV4) {
        packet_free(pkt);
        iface->rx_dropped++;
        return;
    }

    router_receive(router, iface, pkt);
}

Router *router_create(const char *name, Simulator *sim) {
    if (!name || !sim) {
        return NULL;
    }

    Router *router = malloc(sizeof(Router));
    if (!router) {
        return NULL;
    }

    memset(router, 0, sizeof(Router));

    Device *dev = device_create(name, ROUTER_MAX_PORTS);
    if (!dev) {
        free(router);
        return NULL;
    }

    router->base             = *dev;
    router->sim              =  sim;

    free(dev);

    arp_cache_init(&router->arp_cache);
    route_table_init(&router->route_tbl);
    ip_stack_init(&router->ip_stack, sim);

    return router;
}

void    router_free(Router *router) {
    if (!router) {
        return;
    }

    for (int i = 0; i < router->base.iface_count; i++) {
        interface_free(router->base.interfaces[i]);
    }
    free(router->base.interfaces);
    free(router);
}

int     router_add_interface(Router *router, Interface *iface) {
    if (!router || !iface) {
        return -1;
    }

    if (router->base.iface_count >= ROUTER_MAX_PORTS) {
        return -1;
    }

    int res = device_add_interface(&router->base, iface);
    if (res == -1) {
        return -1;
    }

    interface_set_arp_cache(iface, &router->arp_cache);
    interface_set_rx_handler(iface, router_rx_shim, router);
    return 0;
}

int     router_receive(Router    *router,
                       Interface *iface,
                       Packet    *pkt) {
    if (!router || !iface || !pkt) {
        return -1;
    }

    if (!router->sim) {
        packet_free(pkt);
        return -1;
    }

    if (!pkt->data || pkt->len < IP_HDR_LEN) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (ip_validate_header(pkt) != 0) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    IpHeader *ip_hdr = (IpHeader *)pkt->data;
    uint32_t  dst_ip = ns_ntohl(ip_hdr->dst_ip);

    if (router_ip_is_local(router, dst_ip) ||
        router_ip_is_control_multicast(dst_ip)) {
        return ip_receive(iface,
                          pkt,
                          ETHERTYPE_IPV4,
                          &router->ip_stack);
    }

    if (ip_hdr->ttl <= 1) {
        icmp_send_time_exceeded(router->sim, iface, pkt);
        iface->rx_dropped++;
        packet_free(pkt);
        return -1;
    }

    RouteFibEntry *route  = route_table_lookup(&router->route_tbl, dst_ip);
    if (!route) {
        icmp_send_unreach_net(router->sim, iface, pkt);
        iface->rx_dropped++;
        packet_free(pkt);
        return -1;
    }

    uint32_t arp_target_ip;
    if (route->next_hop != 0) {
        arp_target_ip = route->next_hop;
    } else {
        arp_target_ip = dst_ip;
    }

    ip_hdr->ttl--;
    ip_hdr->header_checksum = 0;
    ip_hdr->header_checksum = ip_checksum(ip_hdr);

    uint8_t dst_mac[6];
    if (arp_cache_lookup(&router->arp_cache, arp_target_ip, dst_mac) == 0) {
        return ethernet_send(router->sim,
                             route->iface,
                             dst_mac,
                             ETHERTYPE_IPV4,
                             pkt);
    } else {
        if (arp_send_request(router->sim,
                             route->iface,
                             ns_htonl(arp_target_ip)) != 0) {
            packet_free(pkt);
            return -1;
        }

        if (arp_pending_enqueue(&router->arp_cache,
                                route->iface,
                                arp_target_ip,
                                ETHERTYPE_IPV4,
                                pkt) == -1) {
            packet_free(pkt);
            return -1;
        } else {
            return 0;
        }
    }
}

int     router_add_route(Router    *router,
                         uint32_t   prefix,
                         uint8_t    prefix_len,
                         uint32_t   next_hop,
                         Interface *iface,
                         uint32_t   metric,
                         uint8_t    proto) {
    if (!router) {
        return -1;
    }

    return route_table_add(&router->route_tbl,
                           prefix,
                           prefix_len,
                           next_hop,
                           iface,
                           metric,
                           proto);
}

int     router_del_route(Router    *router,
                         uint32_t   prefix,
                         uint8_t    prefix_len,
                         uint8_t    proto) {
    if (!router) {
        return -1;
    }

    return route_table_delete(&router->route_tbl,
                              prefix,
                              prefix_len,
                              proto);
}
