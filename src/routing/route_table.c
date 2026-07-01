#include "route_table.h"
#include <string.h>

static uint32_t route_prefix_mask(uint8_t prefix_len) {
    if (prefix_len == 0) {
        return 0;
    } else if (prefix_len >= 32) {
        return 0xFFFFFFFF;
    } else {
        return 0xFFFFFFFF << (32 - prefix_len);
    }
}

static int route_admin_distance(uint8_t proto) {
    switch (proto) {
        case ROUTE_PROTO_DIRECT:
            return 0;
        case ROUTE_PROTO_STATIC:
            return 1;
        case ROUTE_PROTO_BGP:
            return 20;
        case ROUTE_PROTO_EIGRP:
            return 90;
        case ROUTE_PROTO_OSPF:
            return 110;
        case ROUTE_PROTO_ISIS:
            return 115;
        case ROUTE_PROTO_RIP:
            return 120;
        default:
            return 255;
    }
}

void           route_table_init(RouteTable *table) {
    if (!table) {
        return;
    }

    memset(table, 0, sizeof(RouteTable));
    table->next_sequence = 1;
}

int            route_table_add(RouteTable *table,
                               uint32_t    prefix,
                               uint8_t     prefix_len,
                               uint32_t    next_hop,
                               Interface  *iface,
                               uint32_t    metric,
                               uint8_t     proto) {
    if (!table || !iface || prefix_len > 32 || proto == 0) {
        return -1;
    }

    uint32_t normalized_prefix = prefix & route_prefix_mask(prefix_len);

    /*
     * Check if the route already exists in the RIB (duplicate key)
     */
    for (int i = 0; i < ROUTE_RIB_SIZE; i++) {
        RouteRibEntry *entry = &table->rib[i];
        if (entry->valid &&
            entry->prefix == normalized_prefix &&
            entry->prefix_len == prefix_len &&
            entry->proto == proto) {
            entry->next_hop = next_hop;
            entry->iface    = iface;
            entry->metric   = metric;
            int res = route_table_rebuild_fib(table);
            return res == 0 ? 0 : -1;
        }
    }

    /*
     * Add new entry to the RIB
     */
    for (int i = 0; i < ROUTE_RIB_SIZE; i++) {
        RouteRibEntry *entry = &table->rib[i];
        if (!entry->valid) {
            entry->prefix     = normalized_prefix;
            entry->prefix_len = prefix_len;
            entry->proto      = proto;
            entry->valid      = 1;
            entry->selected   = 0;
            entry->next_hop   = next_hop;
            entry->iface      = iface;
            entry->metric     = metric;
            entry->sequence   = table->next_sequence++;
            table->rib_count++;
            int res = route_table_rebuild_fib(table);
            if (res == 0) {
                return 0;
            } else {
                entry->valid    = 0;
                entry->selected = 0;
                entry->iface    = NULL;
                table->next_sequence--;
                table->rib_count--;
                return -1;
            }   
        }
    }

    /*
     * RIB is full
     */
    return -1;
}

int            route_table_delete(RouteTable *table,
                                  uint32_t    prefix,
                                  uint8_t     prefix_len,
                                  uint8_t     proto) {
    if (!table || prefix_len > 32 || proto == 0) {
        return -1;
    }

    uint32_t normalized_prefix = prefix & route_prefix_mask(prefix_len);
    for (int i = 0; i < ROUTE_RIB_SIZE; i++) {
        RouteRibEntry *entry = &table->rib[i];
        if (entry->valid                           &&
            entry->prefix     == normalized_prefix &&
            entry->prefix_len == prefix_len        &&
            entry->proto      == proto) {
            entry->valid    = 0;
            entry->selected = 0;
            entry->iface    = NULL;
            if (table->rib_count > 0) {
                table->rib_count--;
            }
            int res = route_table_rebuild_fib(table);
            return res == 0 ? 0 : -1;
        }
    }

    return -1;
}

RouteFibEntry *route_table_lookup(RouteTable *table,
                                  uint32_t    dst_ip) {
    if (!table) {
        return NULL;
    }

    RouteFibEntry *best_match = NULL;
    /*
     * Two-pass lookup rule:
     *   1. Long prefixes (> 24) are the special fast-path candidates (Long Prefixes).
     *   2. Normal prefixes (<= 24), including /0, are fallback candidates (Normal Prefixes).
     * This scan keeps the longest matching prefix, so a long-prefix match
     * naturally wins over any normal-prefix match.
     */
    for (int i = 0; i < ROUTE_FIB_SIZE; i++) {
        RouteFibEntry *entry = &table->fib[i];
        if (entry->valid) {
            if (entry->prefix_len <= 32) {
                if ((entry->prefix_len > 24)) {
                    if ((dst_ip & route_prefix_mask(entry->prefix_len)) == entry->prefix) {
                        if (!best_match || entry->prefix_len > best_match->prefix_len) {
                            best_match = entry;
                        }
                    }
                } else if ((entry->prefix_len <= 24)) {
                    if ((dst_ip & route_prefix_mask(entry->prefix_len)) == entry->prefix) {
                        if (!best_match || entry->prefix_len > best_match->prefix_len) {
                            best_match = entry;
                        }
                    }
                }
            }   
        }
    }

    return best_match;
}

int            route_table_flush_proto(RouteTable *table,
                                       uint8_t     proto) {
    if (!table || proto == 0) {
        return 0;
    }

    int invalidated = 0;
    for (int i = 0; i < ROUTE_RIB_SIZE; i++) {
        RouteRibEntry *entry = &table->rib[i];
        if (entry->valid && entry->proto == proto) {
            entry->valid    = 0;
            entry->selected = 0;
            entry->iface    = NULL;
            if (table->rib_count > 0) {
                table->rib_count--;
            }
            invalidated++;
        }
    }
    route_table_rebuild_fib(table);

    return invalidated;
}


int            route_table_rebuild_fib(RouteTable *table) {
    if (!table ) {
        return -1;
    }

    for (int i = 0; i < ROUTE_FIB_SIZE; i++) {
        memset(&table->fib[i], 0, sizeof(RouteFibEntry));
    }
    table->fib_count = 0;

    for (int i = 0; i < ROUTE_RIB_SIZE; i++) {
        RouteRibEntry *rib_entry = &table->rib[i];
        if (rib_entry->valid) {
            rib_entry->selected = 0;
        }
    }

    for (int i = 0; i < ROUTE_RIB_SIZE; i++) {
        RouteRibEntry *rib_entry = &table->rib[i];
        if (!rib_entry->valid) {
            continue;
        }

        RouteFibEntry *matching_fib = NULL;

        for (int j = 0; j < ROUTE_FIB_SIZE; j++) {
            RouteFibEntry *fib_entry = &table->fib[j];

            if (fib_entry->valid                            &&
                fib_entry->prefix     == rib_entry->prefix  &&
                fib_entry->prefix_len == rib_entry->prefix_len) {
                matching_fib = fib_entry;
                break;
            }
        }

        if (matching_fib) {
            RouteRibEntry *current = &table->rib[matching_fib->rib_index];
            
            int rib_ad     = route_admin_distance(rib_entry->proto);
            int current_ad = route_admin_distance(current->proto);
            int replace    = 0;

            if (rib_ad < current_ad) {
                replace = 1;
            } else if (rib_ad == current_ad             &&
                       rib_entry->metric < current->metric) {
                replace = 1;
            } else if (rib_ad == current_ad                 &&
                       rib_entry->metric == current->metric &&
                       rib_entry->sequence < current->sequence) {
                replace = 1;
            }

            if (replace) {
                current->selected = 0;

                matching_fib->prefix     = rib_entry->prefix;
                matching_fib->prefix_len = rib_entry->prefix_len;
                matching_fib->proto      = rib_entry->proto;
                matching_fib->valid      = 1;
                matching_fib->next_hop   = rib_entry->next_hop;
                matching_fib->iface      = rib_entry->iface;
                matching_fib->metric     = rib_entry->metric;
                matching_fib->rib_index  = i;

                rib_entry->selected      = 1;
            }

            continue;
        }

        for (int j = 0; j < ROUTE_FIB_SIZE; j++) {
            RouteFibEntry *fib_entry = &table->fib[j];

            if (!fib_entry->valid) {
                fib_entry->prefix     = rib_entry->prefix;
                fib_entry->prefix_len = rib_entry->prefix_len;
                fib_entry->proto      = rib_entry->proto;
                fib_entry->valid      = 1;
                fib_entry->next_hop   = rib_entry->next_hop;
                fib_entry->iface      = rib_entry->iface;
                fib_entry->metric     = rib_entry->metric;
                fib_entry->rib_index  = i;

                rib_entry->selected   = 1;
                table->fib_count++;
                break;
            }
        }
    }

    return 0;
}
