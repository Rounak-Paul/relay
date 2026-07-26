#ifndef RELAY_JOB_SYSTEM_H
#define RELAY_JOB_SYSTEM_H

#include "relay/platform.h"

#include <stdbool.h>
#include <stddef.h>

#if RELAY_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#endif

/** Background task function executed by one Relay worker thread. */
typedef void (*Relay_JobFunction)(void *context);

/** One queued background task and its caller-owned context. */
typedef struct Relay_Job {
    Relay_JobFunction function;
    void *context;
} Relay_Job;

/** Cross-platform worker pool owned by the Relay application. */
typedef struct Relay_JobSystem {
    Relay_Job *queue;
    size_t queue_capacity;
    size_t queue_head;
    size_t queue_count;
    size_t active_count;
    size_t worker_count;
    bool accepting;
    bool initialized;
#if RELAY_PLATFORM_WINDOWS
    HANDLE *workers;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE work_available;
    CONDITION_VARIABLE idle;
#else
    pthread_t *workers;
    pthread_mutex_t mutex;
    pthread_cond_t work_available;
    pthread_cond_t idle;
#endif
} Relay_JobSystem;

/** Start a worker pool; pass zero to reserve one logical CPU for the caller. */
bool relay_job_system_init(Relay_JobSystem *system, size_t worker_count);

/** Queue a background task; the caller retains ownership of its context. */
bool relay_job_system_submit(Relay_JobSystem *system, Relay_JobFunction function,
    void *context);

/** Block until no queued or running background task remains. */
void relay_job_system_wait_idle(Relay_JobSystem *system);

/** Drain queued work, join workers, and release all worker-pool resources. */
void relay_job_system_shutdown(Relay_JobSystem *system);

#endif
