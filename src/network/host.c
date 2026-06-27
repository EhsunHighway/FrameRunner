#include "host.h"
#include "../protocols/arp.h"
#include "../protocols/icmp.h"
#include <stdlib.h>
#include <string.h>

Host *host_create(const char *name,
                  Simulator  *sim,
                  uint32_t    gateway_ip) {
    if (name == NULL || sim == NULL) {
        return NULL;
    }

    Host *host = (Host *)malloc(sizeof(Host));
    if (!host) {
        return NULL;
    }
    memset(host, 0, sizeof(Host));
    strncpy(host->base.name, name, sizeof(host->base.name) - 1);
    host->sim             = sim;
    host->base.iface_max  = HOST_MAX_PORTS;
    host->base.interfaces = (Interface **)malloc(sizeof(Interface *) * HOST_MAX_PORTS);
    if (!host->base.interfaces) {
        host_free(host);
        return NULL;
    }
    host->base.iface_count = 0;

    host->arp_cache = malloc(sizeof(ArpCache));
    if (!host->arp_cache) {
        host_free(host);
        return NULL;
    }
    arp_cache_init(host->arp_cache);

    host->ip_stack = malloc(sizeof(IpStack));
    if (!host->ip_stack) {
        host_free(host);
        return NULL;
    }
    ip_stack_init(host->ip_stack, sim);

    host->udp_state = malloc(sizeof(UdpState));
    if (!host->udp_state) {
        host_free(host);
        return NULL;
    }
    udp_init(host->udp_state);

    host->tcp_table = malloc(sizeof(TcpTable));
    if (!host->tcp_table) {
        host_free(host);
        return NULL;
    }
    tcp_init(host->tcp_table);

    host->udp_context = malloc(sizeof(UdpContext));
    if (!host->udp_context) {
        host_free(host);
        return NULL;
    }
    host->udp_context->sim   = sim;
    host->udp_context->state = host->udp_state;

    host->tcp_context = malloc(sizeof(TcpContext));
    if (!host->tcp_context) {
        host_free(host);
        return NULL;
    }
    host->tcp_context->sim   = sim;
    host->tcp_context->table = host->tcp_table;

    if (ip_stack_register_protocol(host->ip_stack,
                               IPPROTO_ICMP,
                               icmp_receive,
                               sim) != 0) {
        host_free(host);
        return NULL;
    }

    if (ip_stack_register_protocol(host->ip_stack,
                               IPPROTO_UDP,
                               udp_receive,
                               host->udp_context) != 0) {
        host_free(host);
        return NULL;
    }

    if (ip_stack_register_protocol(host->ip_stack,
                               IPPROTO_TCP,
                               tcp_receive,
                               host->tcp_context) != 0) {
        host_free(host);
        return NULL;
    }

    host->gateway_ip = gateway_ip;
    return host;
}

void  host_free(Host *host) {
    if (!host) {
        return;
    }

    if (host->base.interfaces) {
        for (int i = 0; i < host->base.iface_count; i++) {
            interface_free(host->base.interfaces[i]);
        }
        free(host->base.interfaces);
    }
    free(host->arp_cache);
    free(host->ip_stack);
    free(host->udp_state);
    free(host->tcp_table);
    free(host->udp_context);
    free(host->tcp_context);

    free(host);
}

int   host_add_interface(Host *host, Interface *iface) {
    if (!host || !iface || !host->ip_stack || !host->arp_cache) {
        return -1;
    }

    if (host->base.iface_count >= host->base.iface_max) {
        return -1; // No more space for new interfaces
    }

    if (device_add_interface(&host->base, iface) != 0) {
        return -1;
    }

    interface_set_arp_cache(iface, host->arp_cache);
    ip_stack_bind_interface(host->ip_stack, iface);

    return 0;
}

int   host_receive(Host      *host,
                   Interface *iface,
                   Packet    *pkt,
                   uint16_t   ethertype) {
    if (!host || !iface || !pkt || !host->ip_stack) {
        return -1;
    }

    if (ethertype != ETHERTYPE_IPV4 ) {
        packet_free(pkt);
        return -1; // Only IP packets are handled by the host
    }

    return ip_receive(iface, pkt, ethertype, host->ip_stack);
}

int   host_send_ip(Host    *host,
                   uint32_t src_ip,
                   uint32_t dst_ip,
                   uint8_t  protocol,
                   Packet  *payload) {
    if (!host || !host->sim) {
        return -1;
    }

    if (!payload) {
        return -1;
    }

    if (src_ip == 0 || dst_ip == 0) {
        return -1; // Invalid IP addresses
    }

    return ip_output(host->sim, src_ip, dst_ip, protocol, payload);
}
