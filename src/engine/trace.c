#include <stdlib.h>
#include <string.h>
#include "trace.h"

TraceLog          *trace_log_create(size_t initial_capacity) {
    if (initial_capacity == 0) {
        return NULL;
    }

    if (initial_capacity > SIZE_MAX / sizeof(TraceRecord)) {
        return NULL;
    }

    TraceLog *t_log = malloc(sizeof(TraceLog));
    if (!t_log) {
        return NULL;
    }
    *t_log = (TraceLog){0};

    TraceRecord *t_records = malloc(sizeof(TraceRecord) * initial_capacity);
    if (!t_records) {
        trace_log_free(t_log);
        return NULL;
    }
    memset(t_records, 0, initial_capacity * sizeof(TraceRecord));

    t_log->records        = t_records;
    t_log->count          = 0;
    t_log->capacity       = initial_capacity;
    t_log->next_sequence  = 1;
    t_log->enabled        = 1;

    return t_log;
}

int                trace_record_init(TraceRecord *record,
                                     TraceAction  action,
                                     uint64_t     timestamp) {
    if (!record) {
        return -1;
    }

    if (action <= TRACE_ACTION_UNSPECIFIED ||
        action >= TRACE_ACTION_COUNT) {
        return -1;
    }

    memset(record, 0, sizeof(TraceRecord));
    record->timestamp     = timestamp;
    record->end_timestamp = timestamp;
    record->action        = action;
    record->event_type    = EVT_TYPE_COUNT;
    record->result        = TRACE_RESULT_NONE;

    return 0;
}

void               trace_log_free(TraceLog *log) {
    if (!log) {
        return;
    }

    if (log->records) {
        free(log->records);
    }

    free(log);
}

void               trace_log_clear(TraceLog *log) {
    if (!log) {
        return;
    }

    if (!log->records) {
        return;
    }

    if (log->capacity == 0) {
        return;
    }

    if (log->capacity < log->count) {
        return;
    }

    log->count         = 0;
    log->next_sequence = 1;
}

void               trace_log_set_enabled(TraceLog *log, int enabled) {
    if (!log) {
        return;
    }

    if (!log->records) {
        return;
    }

    if (log->capacity == 0) {
        return;
    }

    if (log->capacity < log->count) {
        return;
    }

    if (enabled != 0) {
        log->enabled = 1;
    } else {
        log->enabled = 0;
    }
}

int                trace_log_append(TraceLog          *log,
                                    const TraceRecord *record) {
    if (!log) {
        return -1;
    }

    if (!record) {
        return -1;
    }

    if (!log->records) {
        return -1;
    }

    if (log->capacity == 0) {
        return -1;
    }

    if (log->count > log->capacity) {
        return -1;
    }

    if (log->enabled != 0 && log->enabled != 1) {
        return -1;
    }

    if (log->next_sequence == 0) {
        return -1;
    }

    if (log->enabled == 0) {
        return 0;
    }

    if (record->timestamp > record->end_timestamp) {
        return -1;
    }

    if (record->action <= TRACE_ACTION_UNSPECIFIED ||
        record->action >= TRACE_ACTION_COUNT) {
        return -1;
    }

    if (record->action        != TRACE_PACKET_TX &&
        record->end_timestamp != record->timestamp) {
        return -1;
    }

    if ((int)record->event_type < (int)EVT_PACKET_SEND ||
        record->event_type > EVT_TYPE_COUNT) {
        return -1;
    }

    if (record->event_type == EVT_TYPE_COUNT) {
        if (record->event_timestamp != 0 || record->event_sequence != 0) {
            return -1;
        }
    } else if (record->event_sequence == 0) {
        return -1;
    }

    if (record->layer > 4) {
        return -1;
    }

    if (record->result != TRACE_RESULT_FAILURE &&
        record->result != TRACE_RESULT_NONE    &&
        record->result != TRACE_RESULT_SUCCESS) {
        return -1;
    }

    switch (record->action) {
    case TRACE_EVENT_STARTED:
    case TRACE_EVENT_FINISHED:
    case TRACE_TIMER_FIRED:
        if (record->result != TRACE_RESULT_NONE) {
            return -1;
        }
        break;

    case TRACE_ARP_LOOKUP:
    case TRACE_MAC_LOOKUP:
    case TRACE_ROUTE_LOOKUP:
        if (record->result != TRACE_RESULT_FAILURE &&
            record->result != TRACE_RESULT_SUCCESS) {
            return -1;
        }
        break;

    case TRACE_PACKET_DROPPED:
        if (record->result != TRACE_RESULT_FAILURE) {
            return -1;
        }
        break;

    default:
        if (record->result != TRACE_RESULT_SUCCESS) {
            return -1;
        }
        break;
    }

    if (record->snapshot_length > TRACE_PACKET_SNAPSHOT_MAX ||
        record->snapshot_length > record->packet_length) {
        return -1;
    }

    uint8_t expected_truncated = record->snapshot_length < record->packet_length ? 1u : 0u;
    if (record->snapshot_truncated != expected_truncated) {
        return -1;
    }

    if (record->packet_id != 0 && record->trace_id == 0) {
        return -1;
    }

    if (record->parent_packet_id != 0 &&
        (record->packet_id == 0 || record->trace_id == 0)) {
        return -1;
    }

    switch (record->action) {
    case TRACE_EVENT_SCHEDULED:
    case TRACE_EVENT_STARTED:
    case TRACE_EVENT_FINISHED:
        if (record->event_type >= EVT_TYPE_COUNT) {
            return -1;
        }
        break;

    case TRACE_PACKET_CREATED:
    case TRACE_PACKET_CLONED:
    case TRACE_PACKET_TX:
    case TRACE_PACKET_RX:
    case TRACE_PACKET_DROPPED:
    case TRACE_HEADER_ADDED:
    case TRACE_HEADER_REMOVED:
    case TRACE_IP_LOCAL_DELIVERY:
    case TRACE_TRANSPORT_DELIVERY:
        if (record->packet_id == 0 || record->trace_id == 0) {
            return -1;
        }
        break;

    default:
        break;
    }

    if (log->next_sequence == UINT64_MAX) {
        return -1;
    }

    TraceRecord stored_record = *record;

    if (log->count == log->capacity) {
        if (log->capacity > SIZE_MAX / 2) {
            return -1;
        }

        size_t new_capacity = log->capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(TraceRecord)) {
            return -1;
        }

        TraceRecord *new_records =
            realloc(log->records, new_capacity * sizeof(TraceRecord));
        if (!new_records) {
            return -1;
        }

        log->records  = new_records;
        log->capacity = new_capacity;
    }

    log->records[log->count] = stored_record;
    TraceRecord *stored = &log->records[log->count];
    stored->source_device[TRACE_NAME_LEN - 1]      = '\0';
    stored->source_iface[TRACE_NAME_LEN - 1]       = '\0';
    stored->destination_device[TRACE_NAME_LEN - 1] = '\0';
    stored->destination_iface[TRACE_NAME_LEN - 1]  = '\0';
    stored->summary[TRACE_SUMMARY_LEN - 1]         = '\0';
    stored->sequence                               = log->next_sequence;

    log->next_sequence++;
    log->count++;

    return 0;
}

size_t             trace_log_count(const TraceLog *log) {
    if (!log) {
        return 0;
    }

    if (!log->records) {
        return 0;
    }

    if (log->capacity == 0) {
        return 0;
    }

    if (log->capacity < log->count) {
        return 0;
    }

    return log->count;
}

const TraceRecord *trace_log_get(const TraceLog *log,
                                 size_t          index) {
    if (!log) {
        return NULL;
    }

    if (!log->records) {
        return NULL;
    }

    if (log->capacity == 0) {
        return NULL;
    }

    if (log->capacity < log->count) {
        return NULL;
    }

    if (index >= log->count) {
        return NULL;
    }

    return &log->records[index];
}

int                trace_record_capture_packet(TraceRecord  *record,
                                               const Packet *pkt) {
    if (!record) {
        return -1;
    }

    record->packet_id           = 0;
    record->trace_id            = 0;
    record->parent_packet_id    = 0;
    record->layer               = 0;
    record->packet_length       = 0;
    record->snapshot_length     = 0;
    record->snapshot_truncated  = 0;

    memset(record->snapshot, 0, sizeof(record->snapshot));

    if (!pkt) {
        return 0;
    }

    if (packet_validate_view(pkt, 0 , 0) != 0) {
        return -1;
    }

    if (pkt->id == 0 || pkt->trace_id == 0) {
        return -1;
    }

    if (pkt->layer < 0 || pkt->layer > 4) {
        return -1;
    }

    record->packet_id        = pkt->id;
    record->trace_id         = pkt->trace_id;
    record->parent_packet_id = pkt->parent_id;
    record->layer            = pkt->layer;
    record->packet_length    = pkt->len;
    if (pkt->len < TRACE_PACKET_SNAPSHOT_MAX) {
        record->snapshot_length = pkt->len;
    } else {
        record->snapshot_length = TRACE_PACKET_SNAPSHOT_MAX;
    }

    if (record->snapshot_length > 0) {
        memcpy(record->snapshot,
               pkt->data,
               record->snapshot_length);
    }

    record->snapshot_truncated = pkt->len > record->snapshot_length;

    return 0;
}
