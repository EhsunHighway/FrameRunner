#include <string.h>
#include <stdlib.h>
#include "../common/byte_order.h"
#include "arp.h"
#include "ethernet.h"
#include "ip.h"

static void arp_request_handler(const Event *e, void *ctx) {
    Simulator *sim   = (Simulator *)ctx;
    Interface *iface = (Interface *)e->dst_device;
    Packet    *pkt   = (Packet *)e->packet;
    if (!iface || !pkt) {
        return;
    }

    ArpPacket *arp_pkt = (ArpPacket *)pkt->data;
    if (ns_ntohs(arp_pkt->opcode) != ARP_OPCODE_REQUEST) {
        return; // Not an ARP request, ignore
    }

    if (ns_ntohl(arp_pkt->target_protocol_addr) != ns_ntohl(iface->ip_addr)) {
        return; // ARP request not for this interface, ignore
    }

    /*
     * When Host A calls arp_send_request, the broadcast frame travels through 
     * ethernet_send → link_transmit → scheduler_schedule(EVT_ARP_REQUEST). 
     * The event is created with dst_device = Host B's interface (the receiving end of the link).
     * When the event is processed, arp_request_handler runs with 
     * e->dst_device = Host B's interface and e->packet = the ARP request packet.
     * The handler checks the target IP in the ARP request and sees that it matches Host B's IP, 
     * so it prepares an ARP reply and calls arp_send_reply. 
     * 
     * Host A (192.168.1.1)                    Host B (192.168.1.10)
     *                                         ← arp_request_handler runs here
     *                                            on Host B's interface
     *
     *  ── ARP REQUEST (broadcast) ────────────►
     *  "Who has 192.168.1.10?"
     *  sender_mac = AA:AA:AA:AA:AA:AA         ← Host A put its OWN mac here
     *  sender_ip  = 192.168.1.1
     *
     *                                         1. arp_send_reply(sim, iface, pkt)
     *                                              Host B answers: "I am 192.168.1.10,
     *                                              my mac is BB:BB:BB:BB:BB:BB"
     *
     *                                         2. arp_cache_add(sender_ip, sender_mac)
     *                                               Host B caches AA:AA:AA:AA:AA:AA
     *                                               "for free" — no extra round trip needed
     *
     *  ◄── ARP REPLY (unicast) ───────────────
     *      sender_mac = BB:BB:BB:BB:BB:BB
     */
    int result = arp_send_reply(sim, iface, pkt);
    if (iface->arp_cache) {
        uint32_t sender_ip = ns_ntohl(arp_pkt->sender_protocol_addr);
        arp_cache_add(iface->arp_cache, sender_ip, arp_pkt->sender_hardware_addr, e->timestamp);
        arp_pending_flush(sim, iface->arp_cache, sender_ip, arp_pkt->sender_hardware_addr);
    }

    if (result == 0) {
        iface->last_tx_time = e->timestamp;
    } else {
        iface->tx_errors++;
        iface->last_error_time = e->timestamp;
    }
}

static void arp_reply_handler(const Event *e, void *ctx) {
    Simulator *sim = (Simulator *)ctx;
    Interface *iface = (Interface *)e->dst_device;
    Packet    *pkt   = (Packet *)e->packet;
    if (!iface || !pkt) {
        return;
    }

    ArpPacket *arp_pkt = (ArpPacket *)pkt->data;
    if (ns_ntohs(arp_pkt->opcode) != ARP_OPCODE_REPLY) {
        return; // Not an ARP reply, ignore
    }

    // Populate the cache with the sender's IP -> MAC mapping from the reply
    if (iface->arp_cache) {
        uint32_t sender_ip = ns_ntohl(arp_pkt->sender_protocol_addr);
        arp_cache_add(iface->arp_cache, sender_ip, arp_pkt->sender_hardware_addr, e->timestamp);
        arp_pending_flush(sim, iface->arp_cache, sender_ip, arp_pkt->sender_hardware_addr);
    }

    iface->last_rx_time = e->timestamp;
}

void arp_init(Simulator *sim) {
    if (!sim) {
        return;
    }

    scheduler_register(sim->sched, EVT_ARP_REQUEST, arp_request_handler, sim);
    scheduler_register(sim->sched, EVT_ARP_REPLY, arp_reply_handler, sim);
}

void arp_cache_add(ArpCache *cache, uint32_t ip_addr, const uint8_t mac_addr[6], uint64_t timestamp){
    if (!cache || ip_addr == 0) {
        return;
    }

    for (int i = 0; i < ARP_MAX_CACHE_SIZE; i++) {
        if ((cache->entries[i].valid == 1) && (cache->entries[i].ip_addr == ip_addr)) {
            memcpy(cache->entries[i].mac_addr, mac_addr, HARDWARE_ADDR_LEN);
            cache->entries[i].timestamp = timestamp;
            return;
        } 
    }

    for (int i = 0; i < ARP_MAX_CACHE_SIZE; i++) {
        if (cache->entries[i].valid == 0) {
            cache->entries[i].ip_addr   = ip_addr;
            memcpy(cache->entries[i].mac_addr, mac_addr, HARDWARE_ADDR_LEN);
            cache->entries[i].timestamp = timestamp;
            cache->entries[i].valid     = 1;
            cache->count++;
            return;
        }
    }
}

int  arp_cache_lookup(const ArpCache *cache, uint32_t ip_addr, uint8_t out_mac[6]) {
    if (!cache || !ip_addr || !out_mac) {
        return -1;
    }

    for (int i = 0;i < ARP_MAX_CACHE_SIZE;i++) {
        if ((cache->entries[i].valid == 1) && (cache->entries[i].ip_addr == ip_addr)) {
            memcpy(out_mac, cache->entries[i].mac_addr, HARDWARE_ADDR_LEN);
            return 0;
        } 
    }
    return -1;
}

int arp_pending_enqueue(ArpCache *cache,
                        Interface *iface,
                        uint32_t target_ip,
                        uint32_t src_ip,
                        uint32_t dst_ip,
                        uint8_t protocol,
                        Packet *payload) {
    if (!cache || !iface || !target_ip || !payload) {
        return -1;
    }

    for (int i = 0; i < ARP_MAX_PENDING_PACKETS; i++) {
        if (cache->pending[i].valid == 0) {
            cache->pending[i].target_ip = target_ip;
            cache->pending[i].src_ip = src_ip;
            cache->pending[i].dst_ip = dst_ip;
            cache->pending[i].protocol = protocol;
            cache->pending[i].iface = iface;
            cache->pending[i].payload = payload;
            cache->pending[i].valid = 1;
            cache->pending_count++;
            return 0;
        }
    }

    return -1;
}

int arp_pending_flush(Simulator *sim,
                      ArpCache *cache,
                      uint32_t target_ip,
                      const uint8_t mac_addr[6]) {
    if (!sim || !cache || !target_ip || !mac_addr) {
        return 0;
    }

    int sent = 0;
    for (int i = 0; i < ARP_MAX_PENDING_PACKETS; i++) {
        ArpPendingPacket *pending = &cache->pending[i];
        if (!pending->valid || pending->target_ip != target_ip) {
            continue;
        }

        Packet *payload = pending->payload;
        Interface *iface = pending->iface;
        uint32_t src_ip = pending->src_ip;
        uint32_t dst_ip = pending->dst_ip;
        uint8_t protocol = pending->protocol;

        pending->valid = 0;
        pending->payload = NULL;
        pending->iface = NULL;
        if (cache->pending_count > 0) {
            cache->pending_count--;
        }

        if (iface && payload &&
            ip_send(sim, iface, (uint8_t *)mac_addr, src_ip, dst_ip, protocol, payload) >= 0) {
            sent++;
        } else {
            packet_free(payload);
        }
    }

    return sent;
}

void arp_cache_cleanup(ArpCache *cache, uint64_t current_time) {
    if (!cache) {
        return;
    }

    for (int i = 0;i < ARP_MAX_CACHE_SIZE;i++) {
        if ((cache->entries[i].valid == 1) && (current_time - cache->entries[i].timestamp >= ARP_CACHE_TIMEOUT_MS)) {
            cache->entries[i].valid = 0;
            cache->count--;
        } 
    }
}

int  arp_send_request(Simulator *sim, Interface *iface, uint32_t target_ip) {
    if (!sim || !iface || !target_ip) {
        return -1;
    }

    Packet     *pkt               = packet_create(sizeof(ArpPacket));
    ArpPacket  *arp_pkt           = malloc(sizeof(ArpPacket));
    if (!pkt || !arp_pkt) {
        packet_free(pkt);
        free(arp_pkt);
        return -1;
    }

    arp_pkt->hardware_type        = ns_htons(HARDWARE_TYPE_ETHERNET);
    arp_pkt->protocol_type        = ns_htons(PROTOCOL_TYPE_IPV4);
    arp_pkt->hardware_addr_len    = HARDWARE_ADDR_LEN;
    arp_pkt->protocol_addr_len    = PROTOCOL_ADDR_LEN;
    arp_pkt->opcode               = ns_htons(ARP_OPCODE_REQUEST);
    memcpy(arp_pkt->sender_hardware_addr, iface->mac, HARDWARE_ADDR_LEN);
    arp_pkt->sender_protocol_addr =  iface->ip_addr;
    memset(arp_pkt->target_hardware_addr, 0, HARDWARE_ADDR_LEN);
    arp_pkt->target_protocol_addr = target_ip;

    packet_prepend(pkt, arp_pkt, sizeof(ArpPacket));
    free(arp_pkt);

    /*
     * Because it doesn't know the target MAC address, 
     * ARP requests are always broadcast on the local network, 
     * so we use the Ethernet broadcast address as the destination MAC.
     */
    int res = ethernet_send(sim, iface, ETH_BROADCAST, ETHERTYPE_ARP, pkt);
    return res >= 0 ? 0 : -1;
}

int  arp_send_reply(Simulator *sim, Interface *iface, Packet *req_pkt) {
    if (!sim || !iface || !req_pkt) {
        return -1;
    }

    if (!req_pkt->data || req_pkt->len < sizeof(ArpPacket)) {
        return -1;
    }

    /*
     * We need to extract the sender's MAC and IP from the request packet to construct the reply.
     * The reply will be unicast back to the requester, 
     * so we use the sender's hardware address as the destination MAC.
     */ 
    ArpPacket  *req            = (ArpPacket *)req_pkt->data;
    uint8_t     dst_mac[HARDWARE_ADDR_LEN];
    memcpy(dst_mac, req->sender_hardware_addr, HARDWARE_ADDR_LEN);
    uint32_t    dst_ip         = req->sender_protocol_addr;

    Packet     *reply_pkt      = packet_create(sizeof(ArpPacket));
    ArpPacket  *arp_pkt        = malloc(sizeof(ArpPacket));
    if (!reply_pkt || !arp_pkt) {
        packet_free(reply_pkt);
        free(arp_pkt);
        return -1;
    }

    arp_pkt->hardware_type        = ns_htons(HARDWARE_TYPE_ETHERNET);
    arp_pkt->protocol_type        = ns_htons(PROTOCOL_TYPE_IPV4);
    arp_pkt->hardware_addr_len    = HARDWARE_ADDR_LEN;
    arp_pkt->protocol_addr_len    = PROTOCOL_ADDR_LEN;
    arp_pkt->opcode               = ns_htons(ARP_OPCODE_REPLY);
    memcpy(arp_pkt->sender_hardware_addr, iface->mac, HARDWARE_ADDR_LEN);  // we are the answer
    arp_pkt->sender_protocol_addr = iface->ip_addr;
    memcpy(arp_pkt->target_hardware_addr, dst_mac, HARDWARE_ADDR_LEN);     // reply goes back to requester
    arp_pkt->target_protocol_addr = dst_ip;

    packet_prepend(reply_pkt, arp_pkt, sizeof(ArpPacket));
    free(arp_pkt);

    int res = ethernet_send(sim, iface, dst_mac, ETHERTYPE_ARP, reply_pkt); // unicast to requester
    return res >= 0 ? 0 : -1;
}
