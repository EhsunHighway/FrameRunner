#include "ospf.h"
#include "ip.h"
#include "../common/ip_utils.h"
#include <string.h>

static void ospf_schedule_periodic(Simulator     *sim,
                                   EventType      type,
                                   uint64_t       delay,
                                   EventCallback  handler,
                                   OspfState     *state) {
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
    if (event && scheduler_schedule(sim->sched, event) != 0) {
        event_free(event);
    }
}

static int ospf_ensure_spf_vertex(OspfSpfVertex *vertices, 
                                  int           *vertex_count,
                                  uint32_t       router_id) {
    for (int i = 0;i < *vertex_count; i++) {
        if (vertices[i].router_id == router_id) {
            return i;
        }
    }

    if (*vertex_count >= OSPF_SPF_MAX_NODES) {
        return -1;
    }

    int index = *vertex_count;

    vertices[index].router_id   = router_id;
    vertices[index].distance    = OSPF_INFINITY;
    vertices[index].predecessor = -1;
    vertices[index].first_hop   = -1;
    vertices[index].visited     = 0;

    (*vertex_count)++;
    return index;
}

static int ospf_find_spf_vertex(const OspfSpfVertex *vertices,
                                int                  vertex_count,
                                uint32_t             router_id) {
    for (int i = 0; i < vertex_count; i++) {
        if (vertices[i].router_id == router_id) {
            return i;
        }
    }

    return -1;
}

void  ospf_init(OspfState *state,
                Simulator *sim,
                Router    *router,
                uint32_t   router_id) {
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(OspfState));
    state->sim            = sim;
    state->router         = router;
    state->router_id      = router_id;
    state->area_id        = 0;
    state->iface_count    = 0;
    state->lsdb_count     = 0;
    state->neighbor_count = 0;

    ospf_schedule_periodic(sim,
                           EVT_OSPF_HELLO,
                           OSPF_HELLO_INTERVAL_US,
                           ospf_hello_timer,
                           state);
    ospf_schedule_periodic(sim,
                           EVT_OSPF_DEAD,
                           OSPF_DEAD_INTERVAL_US,
                           ospf_dead_timer,
                           state);
}

int  ospf_enable_iface(OspfState *state,
                       Interface *iface,
                       uint16_t   cost) {
    if (!state || !iface) {
        return -1;
    }

    if (cost == 0) {
        return -1;
    }  
    
    for (int i = 0; i < OSPF_MAX_IFACES; i++) {
        OspfIface *slot = &state->ifaces[i];
        if (slot->iface == iface && slot->valid == 1) {
            slot->cost = cost;
            return 0;
        }
    }

    if (state->iface_count >= OSPF_MAX_IFACES) {
        return -1;
    }  

    for (int i = 0; i < OSPF_MAX_IFACES; i++) {
        OspfIface *slot = &state->ifaces[i];
        if (slot->valid == 0) {
            slot->iface = iface;
            slot->cost  = cost;
            slot->valid = 1;
            state->iface_count++;
            return 0;
        }
    }

    return -1;
}

int  ospf_receive(Interface *iface,
                  Packet    *pkt,
                  void      *ctx) {
    if (!iface) {
        return -1;
    }

    if (!pkt) {
        iface->rx_errors++;
        return -1;
    }

    if (!ctx) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    OspfState *state = (OspfState *)ctx;

    if (iface->prefix_len > 32) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (!state->sim || !state->sim->sched || !state->router) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (packet_validate_view(pkt,
                             IP_HDR_LEN,
                             sizeof(OspfHeader)) != 0) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    IpHeader *ip_header = (IpHeader *)(pkt->data - IP_HDR_LEN);
    if (ip_header->protocol != OSPF_PROTO_NUM) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    uint32_t sender_ip = ns_ntohl(ip_header->src_ip);
    
    OspfHeader *header           = (OspfHeader *)pkt->data;
    uint16_t    pkt_len          = ns_ntohs(header->pkt_len);
    uint32_t    sender_router_id = ns_ntohl(header->router_id);
    uint32_t    area_id          = ns_ntohl(header->area_id);
    uint16_t    checksum         = ns_ntohs(header->checksum);
    uint16_t    au_type          = ns_ntohs(header->au_type);

    if (header->version != OSPF_VERSION) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (area_id != state->area_id) {
        packet_free(pkt);
        iface->rx_dropped++;
        return -1;
    }

    if (!(pkt_len == pkt->len) || !(pkt_len >= sizeof(OspfHeader))) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (checksum != 0) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    if (!(au_type == 0) || !(header->auth_data == 0)) {
        packet_free(pkt);
        iface->rx_errors++;
        return -1;
    }

    switch (header->type)
    {
        /*
         * Hello receive
         */
        case OSPF_TYPE_HELLO:
            if (pkt_len < (sizeof(OspfHeader) + sizeof(OspfHello))) {
                packet_free(pkt);
                iface->rx_errors++;
                return -1;
            }

            OspfHello *hello           = (OspfHello *)(pkt->data + sizeof(OspfHeader));
            uint32_t   network_mask    = ns_ntohl(hello->network_mask);
            uint16_t   hello_interval  = ns_ntohs(hello->hello_interval);
            uint32_t   dead_interval   = ns_ntohl(hello->dead_interval);

            size_t neighbor_bytes = pkt_len - sizeof(OspfHeader) - sizeof(OspfHello);
            if (neighbor_bytes % sizeof(uint32_t) != 0) {
                packet_free(pkt);
                iface->rx_errors++;
                return -1;
            }

            size_t    neighbor_id_count = neighbor_bytes / sizeof(uint32_t);
            uint32_t *neighbor_ids      = (uint32_t *)(pkt->data          +
                                                       sizeof(OspfHeader) +
                                                       sizeof(OspfHello));
            
            if (sender_router_id == state->router_id                    ||
                network_mask     != ipv4_prefix_mask(iface->prefix_len) ||
                hello_interval   != OSPF_HELLO_INTERVAL_US / 1000000    ||
                dead_interval    != OSPF_DEAD_INTERVAL_US / 1000000) {
                packet_free(pkt);
                iface->rx_dropped++;
                return -1;
            }

            int local_id_seen = 0;
            
            for (size_t i = 0; i < neighbor_id_count; i++) {
                uint32_t neighbor_router_id = ns_ntohl(neighbor_ids[i]);

                if (neighbor_router_id == state->router_id) {
                    local_id_seen = 1;
                }
            }

            int neighbor_index = -1;
            for (size_t i = 0; i < OSPF_MAX_NEIGHBORS; i++) {
                OspfNeighbor *slot = &state->neighbors[i];
                if (slot->valid && slot->router_id == sender_router_id) {
                    neighbor_index = i;
                    break;
                }
            }

            if (neighbor_index == -1) {
                for (size_t i = 0; i < OSPF_MAX_NEIGHBORS; i++) {
                    OspfNeighbor *slot = &state->neighbors[i];
                    if (!slot->valid) {
                        neighbor_index = i;
                        state->neighbor_count++;
                        break;
                    }
                }
            }

            if (neighbor_index == -1) {
                packet_free(pkt);
                iface->rx_dropped++;
                return -1;
            }

            OspfNeighbor *neighbor  = &state->neighbors[neighbor_index];
            neighbor->router_id     =  sender_router_id;
            neighbor->ip_addr       =  sender_ip;
            neighbor->iface         =  iface;
            neighbor->last_hello_ts =  scheduler_now(state->sim->sched);
            
            if (local_id_seen != 0) {
                neighbor->state = OSPF_NBR_FULL;
            } else {
                neighbor->state = OSPF_NBR_INIT;
            }

            neighbor->valid = 1;

            packet_free(pkt);
            return 0;

        /*
         * Simplified LSU receive
         */
        case OSPF_TYPE_LSU:
            if (pkt_len < (sizeof(OspfHeader) + sizeof(OspfLsuHeader))) {
                packet_free(pkt);
                iface->rx_errors++;
                return -1;
            }

            OspfLsuHeader *lsu_header = (OspfLsuHeader *)(pkt->data + sizeof(OspfHeader));
            uint16_t       lsa_count  = ns_ntohs(lsu_header->lsa_count);

            if (lsa_count > OSPF_LSDB_SIZE) {
                packet_free(pkt);
                iface->rx_errors++;
                return -1;
            }

            OspfLsaEntry parsed_lsas[OSPF_LSDB_SIZE];
            int parsed_count = 0;

            uint8_t *cursor     = pkt->data          +
                                  sizeof(OspfHeader) +
                                  sizeof(OspfLsuHeader);
            size_t   remaining  = pkt_len            -
                                  sizeof(OspfHeader) -
                                  sizeof(OspfLsuHeader);

            for (uint16_t lsa_index = 0; lsa_index < lsa_count; lsa_index++) {
                if (remaining < sizeof(OspfLsaWire)) {
                    packet_free(pkt);
                    iface->rx_errors++;
                    return -1;
                }

                OspfLsaWire  *wire    = (OspfLsaWire *)cursor;

                OspfLsaEntry *parsed  = &parsed_lsas[lsa_index];
                memset(parsed, 0, sizeof(*parsed));
                parsed->lsa_id        = ns_ntohl(wire->lsa_id);
                parsed->adv_router    = ns_ntohl(wire->adv_router);
                parsed->seq_num       = ns_ntohl(wire->seq_num);
                parsed->checksum      = ns_ntohs(wire->checksum);
                parsed->lsa_type      = wire->lsa_type;
                parsed->link_count    = wire->link_count;
                

                cursor    += sizeof(OspfLsaWire);
                remaining -= sizeof(OspfLsaWire);

                if (parsed->link_count > OSPF_MAX_LSA_LINKS) {
                    packet_free(pkt);
                    iface->rx_errors++;
                    return -1;
                }

                size_t link_bytes = (size_t)parsed->link_count * sizeof(OspfLsaLink);
                if (remaining < link_bytes) {
                    packet_free(pkt);
                    iface->rx_errors++;
                    return -1;
                }

                OspfLsaLink *wire_links = (OspfLsaLink *)cursor;

                for (uint16_t link_index = 0; link_index < parsed->link_count; link_index++) {
                    OspfLsaLink *wire_link   = &wire_links[link_index];
                    OspfLsaLink *parsed_link = &parsed->links[link_index];
                    parsed_link->link_id     = ns_ntohl(wire_link->link_id);
                    parsed_link->link_data   = ns_ntohl(wire_link->link_data);
                    parsed_link->type        = wire_link->type;
                    parsed_link->num_tos     = wire_link->num_tos;
                    parsed_link->metric      = ns_ntohs(wire_link->metric);
                }

                cursor    += link_bytes;
                remaining -= link_bytes;
            }

            parsed_count = lsa_count;

            if (remaining) {
                packet_free(pkt);
                iface->rx_errors++;
                return -1;
            }

            int changed_slots[OSPF_LSDB_SIZE];
            int changed_count = 0;

            for (int parsed_index = 0; parsed_index < parsed_count; parsed_index++) {
                OspfLsaEntry *parsed   = &parsed_lsas[parsed_index];

                int matching_index      = -1;
                int first_invalid_index = -1;
                
                for (int i = 0; i < OSPF_LSDB_SIZE; i++) {
                    OspfLsaEntry *slot = &state->lsdb[i];

                    if (slot->valid == 0) {
                        if (first_invalid_index == -1) {
                            first_invalid_index = i;
                        }

                        continue;
                    }

                    if (slot->lsa_id == parsed->lsa_id &&
                        slot->adv_router == parsed->adv_router) {
                        matching_index = i;
                        break;
                    }
                }

                if (matching_index != -1) {
                    OspfLsaEntry *matching = &state->lsdb[matching_index];

                    if (matching->seq_num >= parsed->seq_num) {
                        continue; /* Move to the next parsed LSA. */
                    }
                }

                int lsdb_index;

                if (matching_index != -1) {
                    lsdb_index = matching_index;
                } else {
                    lsdb_index = first_invalid_index;
                }

                if (lsdb_index == -1) {
                    continue;
                }

                int slot_was_invalid = state->lsdb[lsdb_index].valid == 0;
                
                OspfLsaEntry *target = &state->lsdb[lsdb_index];

                *target = *parsed;
                target->valid = 1;

                if (slot_was_invalid) {
                    state->lsdb_count++;
                }

                int changed_already_recorded = 0;
                for (int i = 0; i < changed_count; i++) {
                    if (changed_slots[i] == lsdb_index) {
                        changed_already_recorded = 1;
                        break;
                    }
                }

                if (changed_already_recorded == 0) {
                    changed_slots[changed_count] = lsdb_index;
                    changed_count++;
                }
            }

            int postprocess_failed = 0;
            for (int i = 0; i < changed_count; i++) {
                int           lsdb_index = changed_slots[i];
                OspfLsaEntry *slot       = &state->lsdb[lsdb_index];
                if (ospf_flood_lsa(state, slot, iface) == -1) {
                    postprocess_failed = 1;
                    continue;
                }
            }

            if (changed_count > 0) {
                uint64_t now = scheduler_now(state->sim->sched) + OSPF_SPF_DELAY_US;
                Event   *e   = event_create_callback(EVT_OSPF_SPF,
                                                     now,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     ospf_spf_timer,
                                                     state);
                if (!e) {
                    postprocess_failed = 1;
                } else if (scheduler_schedule(state->sim->sched, e) != 0) {
                    event_free(e);
                    postprocess_failed = 1;
                }
            }
            
            packet_free(pkt);
            
            if (postprocess_failed != 0) {
                iface->rx_errors++;
                return -1;
            }

            return 0;
            
        case OSPF_TYPE_DBD:
            packet_free(pkt);
            return 0;

        case OSPF_TYPE_LSR:
            packet_free(pkt);
            return 0;

        case OSPF_TYPE_LSACK:
            packet_free(pkt);
            return 0;

        default:
            packet_free(pkt);
            iface->rx_errors++;
            return -1;
    }
}

/*
 * OSPF uses protocol number 89 instead of UDP to bypass transport layer overhead. 
 * This design allows OSPF to build its own reliability mechanisms, conserve bandwidth, 
 * and maintain strict control over its routing traffic without relying on 
 * general-purpose transport protocols.
 */
int  ospf_send_hello(OspfState *state, Interface *iface) {
    if (!state || !iface) {
        return -1;
    }

    if (!state->sim) {
        return -1;
    }

    if (iface->prefix_len > 32) {
        return -1;
    }

    int neighbor_count = 0;
    for (int i = 0; i < OSPF_MAX_NEIGHBORS; i++) {
        OspfNeighbor *neighbor = &state->neighbors[i];
        if (neighbor->valid               &&
            neighbor->iface == iface      &&
            neighbor->state != OSPF_NBR_DOWN) {
            neighbor_count++;
        }
    }

    uint16_t payload_len = sizeof(OspfHeader) + 
                            sizeof(OspfHello)  + 
                            neighbor_count * sizeof(uint32_t);
    Packet  *pkt         = packet_create(payload_len);
    if (!pkt) {
        return -1;
    }

    pkt->len = payload_len;

    OspfHeader *header = (OspfHeader *)pkt->data;
    OspfHello  *hello  = (OspfHello  *)(pkt->data + sizeof(OspfHeader));

    header->version   = OSPF_VERSION;
    header->type      = OSPF_TYPE_HELLO;
    header->pkt_len   = ns_htons(payload_len);
    header->router_id = ns_htonl(state->router_id);
    header->area_id   = ns_htonl(state->area_id);
    header->checksum  = 0;
    header->au_type   = ns_htons(0);
    header->auth_data = 0;

    uint32_t mask = ipv4_prefix_mask(iface->prefix_len);

    hello->network_mask    = ns_htonl(mask);
    hello->hello_interval  = ns_htons(OSPF_HELLO_INTERVAL_US / 1000000);
    hello->options         = 0;
    hello->router_priority = 0;
    hello->dead_interval   = ns_htonl(OSPF_DEAD_INTERVAL_US / 1000000);
    hello->dr              = ns_htonl(0);
    hello->bdr             = ns_htonl(0);
    
    uint32_t *neighbor_ids = (uint32_t *)(pkt->data          + 
                                          sizeof(OspfHeader) + 
                                          sizeof(OspfHello));

    int neighbor_idx = 0;
    for (int i = 0; i < OSPF_MAX_NEIGHBORS; i++) {
        OspfNeighbor *neighbor = &state->neighbors[i];
        if (!neighbor->valid) {
            continue;
        }

        if (neighbor->iface != iface) {
            continue;
        }

        if (neighbor->state == OSPF_NBR_DOWN) {
            continue;
        }

        neighbor_ids[neighbor_idx] = ns_htonl(neighbor->router_id);
        neighbor_idx++;
    }

    if (neighbor_idx != neighbor_count) {
        packet_free(pkt);
        return -1;
    }

    uint32_t src_ip = ns_ntohl(iface->ip_addr);
    int res = ip_output(state->sim, 
                        src_ip, 
                        OSPF_ALLSPFROUTERS, 
                        OSPF_PROTO_NUM, 
                        pkt);
    packet_free(pkt);
    return res < 0 ? -1 : 0;
}

int  ospf_flood_lsa(OspfState          *state,
                    const OspfLsaEntry *lsa,
                    Interface          *except_iface) {
    if (!state || !lsa) {
        return -1;
    }

    if (!state->sim || !state->router) {
        return -1;
    }


    if (lsa->link_count < 0 || lsa->link_count > OSPF_MAX_LSA_LINKS) {
        return -1;
    }

    for (int i = 0; i < OSPF_MAX_IFACES; i++) {
        OspfIface *slot      = &state->ifaces[i];
        Interface *out_iface =  slot->iface;
        
        if (!slot->valid) {
            continue;
        }

        if (!out_iface) {
            continue;
        }

        uint32_t src_ip = ns_ntohl(out_iface->ip_addr);

        if (out_iface == except_iface) {
            continue;
        }

        /*
         * OSPF LSU packet
         */
        size_t  total_len = sizeof(OspfHeader)    +
                            sizeof(OspfLsuHeader) +
                            sizeof(OspfLsaWire)   +
                            lsa->link_count * sizeof(OspfLsaLink);
        Packet *pkt       = packet_create(total_len);
        if (!pkt) {
            return -1;
        }
        pkt->len = total_len;

        OspfHeader    *ospf_hdr = (OspfHeader *)pkt->data;
        OspfLsuHeader *lsu_hdr  = (OspfLsuHeader *)(pkt->data + sizeof(OspfHeader));
        OspfLsaWire   *lsa_wire = (OspfLsaWire *)(pkt->data            + 
                                                  sizeof(OspfHeader)   + 
                                                  sizeof(OspfLsuHeader));
        OspfLsaLink   *links    = (OspfLsaLink *)(pkt->data             + 
                                                  sizeof(OspfHeader)    + 
                                                  sizeof(OspfLsuHeader) + 
                                                  sizeof(OspfLsaWire));

        ospf_hdr->version   = OSPF_VERSION;
        ospf_hdr->type      = OSPF_TYPE_LSU;
        ospf_hdr->pkt_len   = ns_htons(total_len);
        ospf_hdr->router_id = ns_htonl(state->router_id);
        ospf_hdr->area_id   = ns_htonl(state->area_id);
        ospf_hdr->checksum  = 0; 
        ospf_hdr->au_type   = ns_htons(0);
        ospf_hdr->auth_data = 0;

        lsu_hdr->lsa_count  = ns_htons(1);
        lsu_hdr->reserved   = ns_htons(0);
        
        lsa_wire->lsa_id     = ns_htonl(lsa->lsa_id);
        lsa_wire->adv_router = ns_htonl(lsa->adv_router);
        lsa_wire->seq_num    = ns_htonl(lsa->seq_num);
        lsa_wire->checksum   = ns_htons(lsa->checksum);
        lsa_wire->lsa_type   = lsa->lsa_type;
        lsa_wire->link_count = lsa->link_count;

        for (int k = 0; k < lsa->link_count; k++) {
            links[k].link_id   = ns_htonl(lsa->links[k].link_id);
            links[k].link_data = ns_htonl(lsa->links[k].link_data);
            links[k].type      = lsa->links[k].type;
            links[k].num_tos   = lsa->links[k].num_tos;
            links[k].metric    = ns_htons(lsa->links[k].metric);        
        }

        int res = ip_output(state->sim, 
                            src_ip,
                            OSPF_ALLSPFROUTERS,
                            OSPF_PROTO_NUM,
                            pkt);

        packet_free(pkt);

        if (res < 0) {
            return -1;
        }
    }

    return 0;
}

int  ospf_run_spf(OspfState *state) {
    if (!state || !state->router) {
        return -1;
    }

    OspfSpfVertex vertices[OSPF_SPF_MAX_NODES];
    int vertex_count = 0;

    vertices[0].router_id   =  state->router_id;
    vertices[0].distance    =  0;
    vertices[0].predecessor = -1;
    vertices[0].first_hop   = -1;
    vertices[0].visited     =  0;

    vertex_count = 1;

    for (int i = 0; i < OSPF_LSDB_SIZE; i++) {
        OspfLsaEntry *lsa_slot = &state->lsdb[i];

        if (!lsa_slot->valid ||
            lsa_slot->lsa_type != OSPF_LSA_TYPE_ROUTER) {
            continue;
        }

        if (ospf_ensure_spf_vertex(vertices,
                                  &vertex_count,
                                   lsa_slot->adv_router) < 0) {
            return -1;
        }

        for (int j = 0; j < lsa_slot->link_count; j++) {
            OspfLsaLink *link = &lsa_slot->links[j];

            if (link->type != OSPF_LINK_P2P) {
                continue;
            }

            if (ospf_ensure_spf_vertex(vertices,
                                      &vertex_count,
                                       link->link_id) < 0) {
                return -1;
            }
        }
    }

    /*
     * Running Dijkstra interation
     */
    while (1) {
        int selected_vtx_idx = -1;
        
        for (int i = 0; i < vertex_count; i++) {
            if (vertices[i].visited != 0           || 
                vertices[i].distance == OSPF_INFINITY) {
                continue;
            }

            if (selected_vtx_idx == -1                                       ||
                vertices[i].distance < vertices[selected_vtx_idx].distance   ||
                (vertices[i].distance == vertices[selected_vtx_idx].distance &&
                 vertices[i].router_id < vertices[selected_vtx_idx].router_id)) {
                selected_vtx_idx = i;
            }
        }

        if (selected_vtx_idx == -1) {
            break;
        }
    
        OspfSpfVertex *selected = &vertices[selected_vtx_idx];
        selected->visited = 1;

        for (int i = 0; i < OSPF_LSDB_SIZE; i++) {
            OspfLsaEntry *lsa_slot = &state->lsdb[i];
            if (!lsa_slot->valid                             ||
                lsa_slot->lsa_type   != OSPF_LSA_TYPE_ROUTER ||
                lsa_slot->adv_router != selected->router_id) {
                continue;
            }
        

            for (int j = 0; j < lsa_slot->link_count; j++) {
                OspfLsaLink *link = &lsa_slot->links[j]; 

                if (link->type != OSPF_LINK_P2P) {
                    continue;
                }

                int link_vertex_index = ospf_find_spf_vertex(vertices,
                                                             vertex_count,
                                                             link->link_id);
                
                if (link_vertex_index == -1) {
                    return -1;
                }

                OspfSpfVertex *destination = &vertices[link_vertex_index];

                if (destination->visited) {
                    continue;
                }

                if (selected->distance >= OSPF_INFINITY - link->metric) {
                    continue;
                }

                uint32_t distance = selected->distance + link->metric;
                if (distance >= destination->distance) {
                    continue;
                }

                destination->distance    = distance;
                destination->predecessor = selected_vtx_idx;

                if (selected_vtx_idx == 0) {
                    destination->first_hop = link_vertex_index;
                } else {
                    if (selected->first_hop < 0          ||
                        selected->first_hop >= vertex_count) {
                        return -1;
                    }

                    destination->first_hop = selected->first_hop;
                }
            }
        }
    }

    route_table_flush_proto(&state->router->route_tbl, ROUTE_PROTO_OSPF);
    
    
    /*
     * After Dijkastra
     */
    for (int route_vertex_index = 0;
         route_vertex_index < vertex_count;
         route_vertex_index++) {
        OspfSpfVertex *route_vertex = &vertices[route_vertex_index];

        if (route_vertex_index == 0) {
            continue;
        }

        if (route_vertex->distance == OSPF_INFINITY) {
            continue;
        }

        int first_hop_index = route_vertex->first_hop;
        if (first_hop_index < 0 ||
            first_hop_index >= vertex_count) {
            return -1;
        }

        uint32_t first_hop_router_id = vertices[first_hop_index].router_id;
        
        int neighbor_index = -1;
        for (int i = 0; i <OSPF_MAX_NEIGHBORS; i++) {
            OspfNeighbor *neighbor_slot = &state->neighbors[i];
            if (neighbor_slot->valid     != 0                   &&
                neighbor_slot->router_id == first_hop_router_id &&
                neighbor_slot->state     != OSPF_NBR_DOWN       &&
                neighbor_slot->iface) {
                neighbor_index = i;
                break;
            }
        }

        if (neighbor_index == -1) {
            continue;
        }
        
        OspfNeighbor *neighbor = &state->neighbors[neighbor_index];
        
        int res = router_add_route(state->router,
                                   route_vertex->router_id,
                                   32,
                                   neighbor->ip_addr,
                                   neighbor->iface,
                                   route_vertex->distance,
                                   ROUTE_PROTO_OSPF);
        
        if (res != 0) {
            return -1;
        }
    }

    return 0;
}

void ospf_hello_timer(const Event *e, void *ctx) {
    (void)e;

    if (!ctx) {
        return;
    }

    OspfState *state = (OspfState *)ctx;

    for (int i = 0; i < OSPF_MAX_IFACES; i++) {
        OspfIface *slot = &state->ifaces[i];

        if (!slot->valid) {
            continue;
        }

        if (!slot->iface) {
            continue;
        }
        
        ospf_send_hello(state, slot->iface);
    }
    
    if (!state->sim || !state->sim->sched) {
        return;
    }

    ospf_schedule_periodic(state->sim,
                           EVT_OSPF_HELLO,
                           OSPF_HELLO_INTERVAL_US,
                           ospf_hello_timer,
                           state);
}

void ospf_dead_timer(const Event *e, void *ctx) {
    (void)e;

    if (!ctx) {
        return;
    }

    OspfState *state = (OspfState *)ctx;
    if (!state->sim || !state->sim->sched) {
        return;
    }

    uint64_t now              = scheduler_now(state->sim->sched);
    int      neighbor_changed = 0;

    for (int i = 0; i < OSPF_MAX_NEIGHBORS; i++) {
        OspfNeighbor *neighbor = &state->neighbors[i];

        if (!neighbor->valid || neighbor->state == OSPF_NBR_DOWN) {
            continue;
        }

        if (now < neighbor->last_hello_ts) {
            continue;
        }

        if (now - neighbor->last_hello_ts < OSPF_DEAD_INTERVAL_US) {
            continue;
        }

        neighbor->state  = OSPF_NBR_DOWN;
        neighbor_changed = 1;
    }

    if (neighbor_changed) {
        Event *spf = event_create_callback(EVT_OSPF_SPF,
                                           now + OSPF_SPF_DELAY_US,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           ospf_spf_timer,
                                           state);
        if (spf && scheduler_schedule(state->sim->sched, spf) != 0) {
            event_free(spf);
        }
    }

    ospf_schedule_periodic(state->sim,
                           EVT_OSPF_DEAD,
                           OSPF_DEAD_INTERVAL_US,
                           ospf_dead_timer,
                           state);
}

void ospf_spf_timer(const Event *e, void *ctx) {
    (void)e;

    if (!ctx) {
        return;
    }

    OspfState *state = (OspfState *)ctx;
    ospf_run_spf(state);
}