#ifndef TRACE_H
#define TRACE_H

#include "event.h"
#include "../network/packet.h"
#include <stdint.h>
#include <stddef.h>

typedef enum TraceAction {
    TRACE_ACTION_UNSPECIFIED       = 0,
    TRACE_EVENT_SCHEDULED          = 1,
    TRACE_EVENT_STARTED            = 2,
    TRACE_EVENT_FINISHED           = 3,
    TRACE_PACKET_CREATED           = 4,
    TRACE_PACKET_CLONED            = 5,
    TRACE_PACKET_TX                = 6,
    TRACE_PACKET_RX                = 7,
    TRACE_PACKET_DROPPED           = 8,
    TRACE_HEADER_ADDED             = 9,
    TRACE_HEADER_REMOVED           = 10,
    TRACE_ARP_LOOKUP               = 11,
    TRACE_ARP_REQUEST              = 12,
    TRACE_ARP_REPLY                = 13,
    TRACE_ARP_CACHE_UPDATE         = 14,
    TRACE_MAC_LEARN                = 15,
    TRACE_MAC_LOOKUP               = 16,
    TRACE_SWITCH_FORWARD           = 17,
    TRACE_SWITCH_FLOOD             = 18,
    TRACE_IP_LOCAL_DELIVERY        = 19,
    TRACE_ROUTE_LOOKUP             = 20,
    TRACE_ROUTE_SELECTED           = 21,
    TRACE_TTL_CHANGED              = 22,
    TRACE_TRANSPORT_DELIVERY       = 23,
    TRACE_PROTOCOL_STATE_CHANGED   = 24,
    TRACE_TIMER_FIRED              = 25,
    TRACE_ROUTE_CHANGED            = 26,
    TRACE_ACTION_COUNT             = 27
} TraceAction;

#define TRACE_NAME_LEN            32
#define TRACE_SUMMARY_LEN         128
#define TRACE_PACKET_SNAPSHOT_MAX 256

typedef enum TraceResult {
    TRACE_RESULT_FAILURE = -1,
    TRACE_RESULT_NONE    = 0,
    TRACE_RESULT_SUCCESS = 1
} TraceResult;

typedef struct TraceRecord {
    uint64_t    timestamp;
    uint64_t    end_timestamp;
    uint64_t    event_timestamp;
    uint64_t    sequence;
    uint64_t    event_sequence;
    TraceAction action;
    EventType   event_type;

    uint32_t    packet_id;
    uint32_t    trace_id;
    uint32_t    parent_packet_id;

    uint8_t     protocol;
    uint8_t     layer;
    TraceResult result;

    char        source_device[TRACE_NAME_LEN];
    char        source_iface[TRACE_NAME_LEN];
    char        destination_device[TRACE_NAME_LEN];
    char        destination_iface[TRACE_NAME_LEN];
    char        summary[TRACE_SUMMARY_LEN];

    size_t      packet_length;
    size_t      snapshot_length;
    uint8_t     snapshot_truncated;
    uint8_t     snapshot[TRACE_PACKET_SNAPSHOT_MAX];
} TraceRecord;

typedef struct TraceLog {
    TraceRecord *records;
    size_t       count;
    size_t       capacity;
    uint64_t     next_sequence;
    int          enabled;
} TraceLog;

TraceLog          *trace_log_create(size_t initial_capacity);

int                trace_record_init(TraceRecord *record,
                                     TraceAction  action,
                                     uint64_t     timestamp);

void               trace_log_free(TraceLog *log);

void               trace_log_clear(TraceLog *log);

void               trace_log_set_enabled(TraceLog *log, int enabled);

int                trace_log_append(TraceLog          *log,
                                    const TraceRecord *record);

size_t             trace_log_count(const TraceLog *log);

const TraceRecord *trace_log_get(const TraceLog *log,
                                 size_t          index);

int                trace_record_capture_packet(TraceRecord  *record,
                                               const Packet *pkt);

#endif // TRACE_H
