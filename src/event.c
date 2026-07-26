#include "relay/event.h"

#include <stdint.h>
#include <stdlib.h>

/** Grow a bus subscription array to hold one additional observer. */
static bool relay_event_bus_grow(Relay_EventBus *bus)
{
    Relay_EventSubscription *subscriptions;
    size_t capacity;

    if (bus->subscription_count < bus->subscription_capacity) {
        return true;
    }
    if (bus->subscription_capacity > SIZE_MAX / 2) {
        return false;
    }
    capacity = bus->subscription_capacity == 0 ? 8 :
        bus->subscription_capacity * 2;
    subscriptions = realloc(bus->subscriptions, capacity * sizeof(*subscriptions));
    if (subscriptions == NULL) {
        return false;
    }

    bus->subscriptions = subscriptions;
    bus->subscription_capacity = capacity;
    return true;
}

/** Remove subscriptions that were disabled outside a dispatch operation. */
static void relay_event_bus_compact(Relay_EventBus *bus)
{
    size_t destination = 0;
    size_t source;

    if (bus->dispatch_depth != 0) {
        return;
    }
    for (source = 0; source < bus->subscription_count; source++) {
        if (bus->subscriptions[source].active) {
            if (destination != source) {
                bus->subscriptions[destination] = bus->subscriptions[source];
            }
            destination++;
        }
    }
    bus->subscription_count = destination;
}

bool relay_event_bus_init(Relay_EventBus *bus)
{
    if (bus == NULL) {
        return false;
    }

    *bus = (Relay_EventBus){0};
    bus->next_subscription_id = 1;
    return true;
}

size_t relay_event_bus_subscribe(Relay_EventBus *bus, Relay_EventType type,
    Relay_EventCallback callback, void *context)
{
    Relay_EventSubscription *subscription;
    size_t id;

    if (bus == NULL || type == RELAY_EVENT_NONE || callback == NULL ||
        !relay_event_bus_grow(bus)) {
        return 0;
    }

    id = bus->next_subscription_id++;
    if (id == 0) {
        id = bus->next_subscription_id++;
        if (id == 0) {
            return 0;
        }
    }
    subscription = &bus->subscriptions[bus->subscription_count++];
    subscription->id = id;
    subscription->type = type;
    subscription->callback = callback;
    subscription->context = context;
    subscription->active = true;
    return id;
}

bool relay_event_bus_unsubscribe(Relay_EventBus *bus, size_t subscription_id)
{
    size_t index;

    if (bus == NULL || subscription_id == 0) {
        return false;
    }
    for (index = 0; index < bus->subscription_count; index++) {
        if (bus->subscriptions[index].id == subscription_id &&
            bus->subscriptions[index].active) {
            bus->subscriptions[index].active = false;
            relay_event_bus_compact(bus);
            return true;
        }
    }
    return false;
}

void relay_event_bus_emit(Relay_EventBus *bus, const Relay_Event *event)
{
    size_t index;
    const size_t subscription_count = bus == NULL ? 0 : bus->subscription_count;

    if (bus == NULL || event == NULL || event->type == RELAY_EVENT_NONE) {
        return;
    }

    bus->dispatch_depth++;
    for (index = 0; index < subscription_count; index++) {
        Relay_EventSubscription *subscription = &bus->subscriptions[index];

        if (subscription->active && subscription->type == event->type) {
            subscription->callback(event, subscription->context);
        }
    }
    bus->dispatch_depth--;
    relay_event_bus_compact(bus);
}

void relay_event_bus_shutdown(Relay_EventBus *bus)
{
    if (bus == NULL) {
        return;
    }

    free(bus->subscriptions);
    bus->subscriptions = NULL;
    bus->subscription_count = 0;
    bus->subscription_capacity = 0;
    bus->dispatch_depth = 0;
    bus->next_subscription_id = 0;
}
