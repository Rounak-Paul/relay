#ifndef RELAY_EVENT_H
#define RELAY_EVENT_H

#include <stdbool.h>
#include <stddef.h>

/** Events emitted by Relay's main-thread runtime services. */
typedef enum Relay_EventType {
    RELAY_EVENT_NONE,
    RELAY_EVENT_QUIT_REQUESTED,
    RELAY_EVENT_TERMINAL_RESIZED,
    RELAY_EVENT_JOB_COMPLETED
} Relay_EventType;

/** Immutable message delivered to registered event observers. */
typedef struct Relay_Event {
    Relay_EventType type;
    const void *sender;
    const void *data;
} Relay_Event;

/** Callback invoked for each matching event subscription. */
typedef void (*Relay_EventCallback)(const Relay_Event *event, void *context);

/** Internal subscription record owned by an event bus. */
typedef struct Relay_EventSubscription {
    size_t id;
    Relay_EventType type;
    Relay_EventCallback callback;
    void *context;
    bool active;
} Relay_EventSubscription;

/** Main-thread observer registry for Relay runtime events. */
typedef struct Relay_EventBus {
    Relay_EventSubscription *subscriptions;
    size_t subscription_count;
    size_t subscription_capacity;
    size_t dispatch_depth;
    size_t next_subscription_id;
} Relay_EventBus;

/** Initialize an empty main-thread event bus. */
bool relay_event_bus_init(Relay_EventBus *bus);

/** Register an observer and return its non-zero subscription identifier. */
size_t relay_event_bus_subscribe(Relay_EventBus *bus, Relay_EventType type,
    Relay_EventCallback callback, void *context);

/** Disable an observer; it is safe to call while an event is being dispatched. */
bool relay_event_bus_unsubscribe(Relay_EventBus *bus, size_t subscription_id);

/** Deliver an event synchronously to all matching active observers. */
void relay_event_bus_emit(Relay_EventBus *bus, const Relay_Event *event);

/** Release all subscriptions owned by the bus. */
void relay_event_bus_shutdown(Relay_EventBus *bus);

#endif
