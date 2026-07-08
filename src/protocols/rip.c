#include "rip.h"
#include "ip.h"
#include "stdlib.h"
#include <string.h>

static uint32_t rip_prefix_mask(uint8_t prefix_len) {
    if (prefix_len == 0) {
        return 0;
    } else if (prefix_len >= 32) {
        return 0xFFFFFFFFu;
    } else {
        return 0xFFFFFFFFu << (32 - prefix_len);
    }
}

static void rip_schedule_periodic(Simulator     *sim,
                                  EventType      type,
                                  uint64_t       delay,
                                  EventCallback  handler,
                                  RipState      *state) {
    if (!sim || !sim->sched) {
        return;
    }

    uint64_t now   = scheduler_now(sim->sched);
    Event   *event = event_create_callback(type,
                                           now + delay,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           handler,
                                           state);
    if (event) {
        if (scheduler_schedule(sim->sched, event) != 0) {
            event_free(event);
        }
    }
}

void rip_init(RipState  *state,
              Simulator *sim,
              Router    *router,
              UdpState  *udp_state) {
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(RipState));

    state->sim       = sim;
    state->router    = router;
    state->udp_state = udp_state;

    if (udp_state) {
        if (udp_bind(udp_state, RIP_PORT, rip_receive, state) == -1) {
            return;
        }
    }

    if (sim && sim->sched) {
        rip_schedule_periodic(sim,
                              EVT_RIP_UPDATE,
                              RIP_UPDATE_INTERVAL_US,
                              rip_update_handler,
                              state);
        rip_schedule_periodic(sim,
                              EVT_RIP_TIMEOUT,
                              RIP_TIMEOUT_US,
                              rip_timeout_handler,
                              state);
        rip_schedule_periodic(sim,
                              EVT_RIP_GC,
                              RIP_GC_US,
                              rip_gc_handler,
                              state);
    }
}

int rip_enable_iface(RipState *state, Interface *iface) {
    if (!state || !iface) {
        return -1;
    }

    for (int i = 0; i < RIP_MAX_IFACES; i++) {
        if (state->ifaces[i] == iface) {
            return 0;
        }
    }

    if (state->iface_count >= RIP_MAX_IFACES) {
        return -1;
    }

    for (int i = 0; i < RIP_MAX_IFACES; i++) {
        if (!state->ifaces[i]) {
            state->ifaces[i] = iface;
            state->iface_count++;
            return 0;
        }
    }

    return -1;
}

void rip_receive(Interface *iface,
                 uint32_t   src_ip,
                 uint16_t   src_port,
                 Packet    *payload,
                 void      *ctx) {
    (void)src_port;

    if (!payload) {
        return;
    }

    if (!ctx) {
        packet_free(payload);
        return;
    }

    if (!iface) {
        packet_free(payload);
        return;
    }

    RipState *state = (RipState *)ctx;
    if (!state->router) {
        packet_free(payload);
        return;
    }

    if (!state->sim) {
        packet_free(payload);
        return;
    }

    if (!state->sim->sched) {
        packet_free(payload);
        return;
    }

    if (payload->len < RIP_HDR_LEN) {
        packet_free(payload);
        return;
    }

    if ((payload->len - RIP_HDR_LEN) % RIP_ENTRY_LEN != 0) {
        packet_free(payload);
        return;
    }

    uint32_t num_entries = (payload->len - RIP_HDR_LEN) / RIP_ENTRY_LEN;
    if (num_entries > RIP_MAX_ROUTES) {
        packet_free(payload);
        return;
    }

    RipHeader *header = (RipHeader *)payload->data;
    if (header->version != RIP_VERSION) {
        packet_free(payload);
        return;
    }

    if (header->command != RIP_CMD_RESPONSE) {
        packet_free(payload);
        return;
    }

    uint64_t last_update = scheduler_now(state->sim->sched);
    for (uint32_t i = 0; i < num_entries; i++) {
        RipEntry *pkt_entry = (RipEntry *)(payload->data + RIP_HDR_LEN + i * RIP_ENTRY_LEN);
        if (ns_ntohs(pkt_entry->afi) != RIP_AFI_IPV4) {
            continue;
        }
        uint32_t prefix     = ns_ntohl(pkt_entry->ip_addr);
        uint32_t sub_mask   = ns_ntohl(pkt_entry->subnet_mask);
        uint32_t next_hop   = ns_ntohl(pkt_entry->next_hop);
        uint32_t metric     = ns_ntohl(pkt_entry->metric);

        uint8_t  prefix_len = 0;
        if (ip_mask_to_prefix_len(sub_mask, &prefix_len) != 0) {
            continue;
        }
        prefix &= rip_prefix_mask(prefix_len);

        if (metric > RIP_INFINITY) {
            metric = RIP_INFINITY;
        }

        uint32_t candidate_metric = (metric + 1 > RIP_INFINITY) ? RIP_INFINITY : metric + 1;

        if (next_hop == 0) {
            next_hop = src_ip;
        }

        RipRouteInfo *found_match = NULL;
        for (int j = 0; j < RIP_DB_SIZE; j++) {
            RipRouteInfo *entry = &state->db[j];
            if (entry->valid                 && 
                entry->prefix     == prefix  && 
                entry->prefix_len == prefix_len) {
                found_match = entry;
                break;
            }
        }  
        


        if (!found_match                   && 
             candidate_metric == RIP_INFINITY) {
            continue;
        }

        if (candidate_metric == RIP_INFINITY) {
            found_match->prefix      = prefix;
            found_match->prefix_len  = prefix_len;
            found_match->state       = RIP_ROUTE_GC;
            found_match->last_update = last_update;
            found_match->learned_on  = iface;
            found_match->metric      = RIP_INFINITY;
            found_match->valid       = 1;
            router_del_route(state->router, 
                             prefix, 
                             prefix_len, 
                             ROUTE_PROTO_RIP);
            continue;
        } 
            
        if (!found_match                  && 
             candidate_metric < RIP_INFINITY) {
            for (int k = 0; k < RIP_DB_SIZE; k++) {
                RipRouteInfo *empty_entry = &state->db[k];
                if (!empty_entry->valid) {
                    found_match = empty_entry;
                    state->db_count++;
                    break;
                }
            }
        }

        if (!found_match) {
            continue;
        }

        found_match->prefix      = prefix;
        found_match->prefix_len  = prefix_len;
        found_match->metric      = candidate_metric;
        found_match->next_hop    = next_hop;
        found_match->learned_on  = iface;
        found_match->last_update = last_update;
        found_match->state       = RIP_ROUTE_ACTIVE;
        found_match->valid       = 1;
        router_add_route(state->router, 
                         prefix, 
                         prefix_len, 
                         next_hop, 
                         iface, 
                         candidate_metric, 
                         ROUTE_PROTO_RIP);
    }

    packet_free(payload);
    return;
}

int rip_send_update(RipState *state, Interface *out_iface) {
    if (!state || !out_iface) {
        return -1;
    }

    if (!state->sim) {
        return -1;
    }

    uint8_t  payload[RIP_HDR_LEN + RIP_MAX_ROUTES * RIP_ENTRY_LEN];   
    size_t   entry_count = 0;
    uint32_t src_ip      = ns_ntohl(out_iface->ip_addr);
    uint32_t dst_ip      = RIP_MULTICAST;

    RipHeader *response = (RipHeader *)payload;
    response->command   = RIP_CMD_RESPONSE;
    response->version   = RIP_VERSION;
    response->zero      = ns_htons(0);
    entry_count         = 0;

    for (int i = 0; i < RIP_DB_SIZE; i++) {
        RipRouteInfo *entry = &state->db[i];
        if (!entry->valid) {
            continue;
        }

        if (entry->learned_on == out_iface) {
            continue;
        }

        RipEntry *rip_entry    = (RipEntry *)(payload + RIP_HDR_LEN + entry_count * RIP_ENTRY_LEN);
        rip_entry->afi         = ns_htons(RIP_AFI_IPV4);
        rip_entry->route_tag   = ns_htons(0);
        rip_entry->ip_addr     = ns_htonl(entry->prefix);
        rip_entry->subnet_mask = ns_htonl(rip_prefix_mask(entry->prefix_len));
        rip_entry->next_hop    = ns_htonl(entry->next_hop);
        rip_entry->metric      = ns_htonl(entry->metric > RIP_INFINITY ? RIP_INFINITY : entry->metric);
        entry_count++;

        if (entry_count == RIP_MAX_ROUTES) {
            size_t payload_len = RIP_HDR_LEN + entry_count * RIP_ENTRY_LEN;
            int res = udp_send(state->sim, 
                               src_ip, 
                               dst_ip, 
                               RIP_PORT,
                               RIP_PORT,
                               payload,
                               payload_len); 
        
            if (res == -1) {
                return -1;
            }

            entry_count             = 0;
            response->command       = RIP_CMD_RESPONSE;
            response->version       = RIP_VERSION;
            response->zero          = ns_htons(0);
        }

    }
    
    if (entry_count > 0) {
        size_t payload_len = RIP_HDR_LEN + entry_count * RIP_ENTRY_LEN;
        if (udp_send(state->sim, 
                     src_ip, 
                     dst_ip, 
                     RIP_PORT,
                     RIP_PORT,
                     payload,
                     payload_len) == -1) {
            return -1;
        }

    }

    return 0;
}

void rip_update_handler(const Event *e, void *ctx) {
    if (!e || !ctx) {
        return;
    }

    RipState *state = (RipState *)ctx;
    if (!state->sim || !state->sim->sched) {
        return;
    }

    for (int i = 0; i < RIP_MAX_IFACES; i++) {
        Interface *iface = state->ifaces[i];
        if (iface) {
            rip_send_update(state, iface);
        }
    }

    uint64_t now  = scheduler_now(state->sim->sched);
    Event   *next = event_create_callback(EVT_RIP_UPDATE,
                                          now + RIP_UPDATE_INTERVAL_US,
                                          NULL,
                                          NULL,
                                          NULL,
                                          NULL,
                                          rip_update_handler,
                                          state);
    if (next) {
        if (scheduler_schedule(state->sim->sched, next) != 0) {
            event_free(next);
        }
    }
}

void rip_timeout_handler(const Event *e, void *ctx) {
    if (!e || !ctx) {
        return;
    }

    RipState *state = (RipState *)ctx;
    if (!state->sim) {
        return;
    }

    if (!state->sim->sched) {
        return;
    }

    uint64_t current_time = scheduler_now(state->sim->sched);

    for (int i = 0; i < RIP_DB_SIZE; i++) {
        RipRouteInfo *entry = &state->db[i];
        if (entry->valid && entry->state == RIP_ROUTE_ACTIVE) {
            uint64_t time_since_update = current_time - entry->last_update;
            if (time_since_update <= RIP_TIMEOUT_US) {
                continue;
            } else {
                entry->metric = RIP_INFINITY;
                entry->state = RIP_ROUTE_GC;
                if (state->router) {
                    router_del_route(state->router, entry->prefix, entry->prefix_len, ROUTE_PROTO_RIP);
                }
            }
        }
    }

    uint64_t now  = scheduler_now(state->sim->sched);
    Event   *next = event_create_callback(EVT_RIP_TIMEOUT,
                                          now + RIP_TIMEOUT_US,
                                          NULL,
                                          NULL,
                                          NULL,
                                          NULL,
                                          rip_timeout_handler,
                                          state);

    if (next) {
        if (scheduler_schedule(state->sim->sched, next) != 0) {
            event_free(next);
        }
    }
}

void rip_gc_handler(const Event *e, void *ctx) {
    if (!e || !ctx) {
        return;
    }

    RipState *state = (RipState *)ctx;
    if (!state->sim) {
        return;
    }

    if (!state->sim->sched) {
        return;
    }

    uint64_t current_time = scheduler_now(state->sim->sched);

    for (int i = 0; i < RIP_DB_SIZE; i++) {
        RipRouteInfo *entry = &state->db[i];
        if (entry->valid && entry->state == RIP_ROUTE_GC) {
            uint64_t time_since_update = current_time - entry->last_update;
            if (time_since_update <= RIP_TIMEOUT_US + RIP_GC_US) {
                continue;
            } else {
                entry->valid      = 0;
                entry->learned_on = NULL;
                if (state->db_count > 0) {
                    state->db_count--;
                }
            }
        }
    }

    uint64_t now  = scheduler_now(state->sim->sched);
    Event   *next = event_create_callback(EVT_RIP_GC,
                                          now + RIP_GC_US,
                                          NULL,
                                          NULL,
                                          NULL,
                                          NULL,
                                          rip_gc_handler,
                                          state);

    if (next) {
        if (scheduler_schedule(state->sim->sched, next) != 0) {
            event_free(next);
        }
    }
}
