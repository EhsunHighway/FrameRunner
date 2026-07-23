#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "event.h"


Event      *event_create_callback(EventType     type,
                                  uint64_t      timestamp,
                                  void         *src,
                                  void         *dst,
                                  void         *packet,
                                  void         *data,
                                  EventCallback handler,
                                  void         *handler_ctx) {
    Event *e       = malloc(sizeof(Event));
    if (!e) {
        return NULL;
    }

    e->type        = type;
    e->timestamp   = timestamp;
    e->sequence    = 0;
    e->src_device  = src;
    e->dst_device  = dst;
    e->packet      = packet;
    e->data        = data;
    e->handler     = handler;
    e->handler_ctx = handler_ctx;

    return e;
}

Event      *event_create(EventType  type,
                         uint64_t   timestamp,
                         void      *src,
                         void      *dst,
                         void      *packet,
                         void      *data) {
    return event_create_callback(type,
                                 timestamp,
                                 src,
                                 dst,
                                 packet,
                                 data,
                                 NULL,
                                 NULL);
}


void        event_free(Event *e) {
    if (e) {
        free(e);
    }
}


EventQueue *event_queue_create(size_t capacity) {
    EventQueue *eq = malloc(sizeof(EventQueue));
    if (!eq) {
        return NULL;
    }

    Event **events = malloc(sizeof(Event *) * capacity);
    if (!events) {
        free(eq);
        return NULL;
    }

    eq->events   = events;
    eq->count    = 0;
    eq->capacity = capacity;
    return eq;
}


void        event_queue_free(EventQueue *eq) {
    if (eq) {
        free(eq->events);
        free(eq);
    }
}


int         event_queue_push(EventQueue *eq, Event *e) {
    if (eq->count >= eq->capacity) {
        size_t  new_capacity = eq->capacity * 2;
        Event **new_events   = realloc(eq->events, sizeof(Event *) * new_capacity);
        if (!new_events) {
            return -1;
        }
        eq->events   = new_events;
        eq->capacity = new_capacity;
    }

    size_t insertion_index = eq->count;
    while (insertion_index > 0) {
        Event *previous = eq->events[insertion_index - 1];
        if (previous->timestamp < e->timestamp   ||
            (previous->timestamp == e->timestamp &&
             previous->sequence  <= e->sequence)) {
            break;
        }

        eq->events[insertion_index] = previous;
        insertion_index--;
    }

    eq->events[insertion_index] = e;
    eq->count++;
    return 0;
}


Event      *event_queue_pop(EventQueue *eq) {
    if (eq->count == 0) {
        return NULL;
    }

    Event *e = eq->events[0];
    memmove(eq->events, eq->events + 1, sizeof(Event *) * (eq->count - 1));
    eq->count--;
    return e;
}


Event      *event_queue_peek(const EventQueue *eq) {
    if (eq->count == 0) {
        return NULL;
    }
    return eq->events[0];
}

int         event_queue_is_empty(const EventQueue *eq) {
    return eq->count == 0;
}
