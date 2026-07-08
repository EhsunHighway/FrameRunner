#include "static_route.h"
#include "route_table.h"
#include <string.h>

static uint32_t static_route_mask(uint8_t prefix_len) {
    if (prefix_len == 0) {
        return 0;
    } else if (prefix_len >= 32) {
        return 0xFFFFFFFFu;
    } else {
        return 0xFFFFFFFFu << (32 - prefix_len);
    }
}

void static_route_init(StaticRouteTable *table) {
    if (!table) {
        return;
    }

    memset(table, 0, sizeof(StaticRouteTable));
}

int  static_route_add(StaticRouteTable *table,
                      Router           *router,
                      uint32_t          prefix,
                      uint8_t           prefix_len,
                      uint32_t          next_hop,
                      Interface        *iface,
                      uint32_t          metric) {
    if (!table || !router || !iface || prefix_len > 32) {
        return -1;
    }

    uint32_t normalized_prefix = prefix & static_route_mask(prefix_len);

    for (int i = 0; i < STATIC_ROUTE_MAX_ROUTES; i++) {
        StaticRouteEntry *entry = &table->routes[i];

        if (entry->valid                           &&
            entry->prefix     == normalized_prefix &&
            entry->prefix_len == prefix_len) {
            if (router_add_route(router,
                                 normalized_prefix,
                                 prefix_len,
                                 next_hop,
                                 iface,
                                 metric,
                                 ROUTE_PROTO_STATIC) != 0) {
                return -1;
            }

            entry->next_hop  = next_hop;
            entry->iface     = iface;
            entry->metric    = metric;
            entry->installed = 1;
            return 0;
        }
    }

    for (int i = 0; i < STATIC_ROUTE_MAX_ROUTES; i++) {
        StaticRouteEntry *entry = &table->routes[i];

        if (!entry->valid) {
            if (router_add_route(router,
                                 normalized_prefix,
                                 prefix_len,
                                 next_hop,
                                 iface,
                                 metric,
                                 ROUTE_PROTO_STATIC) != 0) {
                return -1;
            }

            entry->prefix     = normalized_prefix;
            entry->prefix_len = prefix_len;
            entry->valid      = 1;
            entry->installed  = 1;
            entry->next_hop   = next_hop;
            entry->iface      = iface;
            entry->metric     = metric;
            table->count++;
            return 0;
        }
    }

    return -1;
}

int  static_route_delete(StaticRouteTable *table,
                         Router           *router,
                         uint32_t          prefix,
                         uint8_t           prefix_len) {
    if (!table || !router || prefix_len > 32) {
        return -1;
    }

    uint32_t normalized_prefix = prefix & static_route_mask(prefix_len);

    for (int i = 0; i < STATIC_ROUTE_MAX_ROUTES; i++) {
        StaticRouteEntry *entry = &table->routes[i];

        if (entry->valid                            &&
            entry->prefix     == normalized_prefix  &&
            entry->prefix_len == prefix_len) {
            int route_res = 0;

            if (entry->installed) {
                route_res = router_del_route(router,
                                             normalized_prefix,
                                             prefix_len,
                                             ROUTE_PROTO_STATIC);
            }

            memset(entry, 0, sizeof(StaticRouteEntry));
            if (table->count > 0) {
                table->count--;
            }

            return route_res == 0 ? 0 : -1;
        }
    }

    return -1;
}

int  static_route_apply(StaticRouteTable *table, Router *router) {
    if (!table || !router) {
        return -1;
    }

    int applied = 0;

    for (int i = 0; i < STATIC_ROUTE_MAX_ROUTES; i++) {
        StaticRouteEntry *entry = &table->routes[i];

        if (!entry->valid) {
            continue;
        }

        if (router_add_route(router,
                             entry->prefix,
                             entry->prefix_len,
                             entry->next_hop,
                             entry->iface,
                             entry->metric,
                             ROUTE_PROTO_STATIC) != 0) {
            entry->installed = 0;
            return -1;
        }

        entry->installed = 1;
        applied++;
    }

    return applied;
}

int  static_route_flush(StaticRouteTable *table, Router *router) {
    if (!table || !router) {
        return -1;
    }

    int removed = 0;

    for (int i = 0; i < STATIC_ROUTE_MAX_ROUTES; i++) {
        StaticRouteEntry *entry = &table->routes[i];

        if (!entry->valid) {
            continue;
        }

        if (entry->installed) {
            router_del_route(router,
                             entry->prefix,
                             entry->prefix_len,
                             ROUTE_PROTO_STATIC);
        }

        memset(entry, 0, sizeof(StaticRouteEntry));
        removed++;
    }

    table->count = 0;
    return removed;
}
