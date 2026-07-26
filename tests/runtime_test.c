#include "relay/event.h"
#include "relay/job_system.h"

#include <stdatomic.h>

int relay_game_test(void);

/** State shared by the event-bus observer regression test. */
typedef struct Relay_EventTestState {
    Relay_EventBus *bus;
    size_t self_subscription;
    int self_invocations;
    int persistent_invocations;
} Relay_EventTestState;

/** Count an event once, then remove this observer during its own dispatch. */
static void relay_test_self_removing_listener(const Relay_Event *event,
    void *context)
{
    Relay_EventTestState *state = context;

    if (event->type == RELAY_EVENT_QUIT_REQUESTED) {
        state->self_invocations++;
        (void)relay_event_bus_unsubscribe(state->bus, state->self_subscription);
    }
}

/** Count every event delivered to the persistent observer. */
static void relay_test_persistent_listener(const Relay_Event *event,
    void *context)
{
    Relay_EventTestState *state = context;

    if (event->type == RELAY_EVENT_QUIT_REQUESTED) {
        state->persistent_invocations++;
    }
}

/** Increment a shared atomic counter from one worker-pool task. */
static void relay_test_increment_job(void *context)
{
    atomic_int *counter = context;

    (void)atomic_fetch_add(counter, 1);
}

/** Verify safe observer removal while dispatching and concurrent job execution. */
int main(void)
{
    Relay_EventBus bus = {0};
    Relay_EventTestState event_state = {0};
    Relay_Event event = {RELAY_EVENT_QUIT_REQUESTED, NULL, NULL};
    Relay_JobSystem jobs = {0};
    atomic_int job_count = 0;
    size_t index;

    if (relay_game_test() != 0 || !relay_event_bus_init(&bus)) {
        return 1;
    }
    event_state.bus = &bus;
    event_state.self_subscription = relay_event_bus_subscribe(&bus,
        RELAY_EVENT_QUIT_REQUESTED, relay_test_self_removing_listener,
        &event_state);
    if (event_state.self_subscription == 0 || relay_event_bus_subscribe(&bus,
            RELAY_EVENT_QUIT_REQUESTED, relay_test_persistent_listener,
            &event_state) == 0) {
        relay_event_bus_shutdown(&bus);
        return 1;
    }
    relay_event_bus_emit(&bus, &event);
    relay_event_bus_emit(&bus, &event);
    if (event_state.self_invocations != 1 ||
        event_state.persistent_invocations != 2) {
        relay_event_bus_shutdown(&bus);
        return 1;
    }
    relay_event_bus_shutdown(&bus);

    if (!relay_job_system_init(&jobs, 2)) {
        return 1;
    }
    for (index = 0; index < 32; index++) {
        if (!relay_job_system_submit(&jobs, relay_test_increment_job, &job_count)) {
            relay_job_system_shutdown(&jobs);
            return 1;
        }
    }
    relay_job_system_wait_idle(&jobs);
    relay_job_system_shutdown(&jobs);
    return atomic_load(&job_count) == 32 ? 0 : 1;
}
