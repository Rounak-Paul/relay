#include "relay/job_system.h"

#include <stdint.h>
#include <stdlib.h>

#if RELAY_PLATFORM_WINDOWS

/** Return a worker count that leaves one logical CPU for the caller. */
static size_t relay_job_system_default_worker_count(void)
{
    SYSTEM_INFO information;

    GetSystemInfo(&information);
    return information.dwNumberOfProcessors > 1 ?
        (size_t)information.dwNumberOfProcessors - 1 : 1;
}

/** Wake worker threads after work arrives or shutdown begins. */
static void relay_job_system_signal_workers(Relay_JobSystem *system)
{
    WakeAllConditionVariable(&system->work_available);
}

/** Wake callers waiting for the worker pool to become idle. */
static void relay_job_system_signal_idle(Relay_JobSystem *system)
{
    WakeAllConditionVariable(&system->idle);
}

/** Run queued work until Relay requests the worker thread to stop. */
static DWORD WINAPI relay_job_system_worker(void *context)
{
    Relay_JobSystem *system = context;

    for (;;) {
        Relay_Job job;

        EnterCriticalSection(&system->mutex);
        while (system->queue_count == 0 && system->accepting) {
            SleepConditionVariableCS(&system->work_available, &system->mutex,
                INFINITE);
        }
        if (system->queue_count == 0 && !system->accepting) {
            LeaveCriticalSection(&system->mutex);
            return 0;
        }
        job = system->queue[system->queue_head];
        system->queue_head = (system->queue_head + 1) % system->queue_capacity;
        system->queue_count--;
        system->active_count++;
        LeaveCriticalSection(&system->mutex);

        job.function(job.context);

        EnterCriticalSection(&system->mutex);
        system->active_count--;
        if (system->queue_count == 0 && system->active_count == 0) {
            relay_job_system_signal_idle(system);
        }
        LeaveCriticalSection(&system->mutex);
    }
}

/** Initialize the Windows synchronization primitives for a worker pool. */
static bool relay_job_system_initialize_sync(Relay_JobSystem *system)
{
    InitializeCriticalSection(&system->mutex);
    InitializeConditionVariable(&system->work_available);
    InitializeConditionVariable(&system->idle);
    return true;
}

/** Destroy the Windows synchronization primitives for a worker pool. */
static void relay_job_system_destroy_sync(Relay_JobSystem *system)
{
    DeleteCriticalSection(&system->mutex);
}

/** Lock a worker-pool state transition. */
static void relay_job_system_lock(Relay_JobSystem *system)
{
    EnterCriticalSection(&system->mutex);
}

/** Unlock a worker-pool state transition. */
static void relay_job_system_unlock(Relay_JobSystem *system)
{
    LeaveCriticalSection(&system->mutex);
}

/** Wait until a worker-pool state transition occurs. */
static void relay_job_system_wait_for_idle(Relay_JobSystem *system)
{
    (void)SleepConditionVariableCS(&system->idle, &system->mutex, INFINITE);
}

#else

#include <unistd.h>

/** Return a worker count that leaves one logical CPU for the caller. */
static size_t relay_job_system_default_worker_count(void)
{
    const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (processor_count <= 1) {
        return 1;
    }
    return (size_t)processor_count - 1;
}

/** Wake worker threads after work arrives or shutdown begins. */
static void relay_job_system_signal_workers(Relay_JobSystem *system)
{
    (void)pthread_cond_broadcast(&system->work_available);
}

/** Wake callers waiting for the worker pool to become idle. */
static void relay_job_system_signal_idle(Relay_JobSystem *system)
{
    (void)pthread_cond_broadcast(&system->idle);
}

/** Run queued work until Relay requests the worker thread to stop. */
static void *relay_job_system_worker(void *context)
{
    Relay_JobSystem *system = context;

    for (;;) {
        Relay_Job job;

        (void)pthread_mutex_lock(&system->mutex);
        while (system->queue_count == 0 && system->accepting) {
            (void)pthread_cond_wait(&system->work_available, &system->mutex);
        }
        if (system->queue_count == 0 && !system->accepting) {
            (void)pthread_mutex_unlock(&system->mutex);
            return NULL;
        }
        job = system->queue[system->queue_head];
        system->queue_head = (system->queue_head + 1) % system->queue_capacity;
        system->queue_count--;
        system->active_count++;
        (void)pthread_mutex_unlock(&system->mutex);

        job.function(job.context);

        (void)pthread_mutex_lock(&system->mutex);
        system->active_count--;
        if (system->queue_count == 0 && system->active_count == 0) {
            relay_job_system_signal_idle(system);
        }
        (void)pthread_mutex_unlock(&system->mutex);
    }
}

/** Initialize the POSIX synchronization primitives for a worker pool. */
static bool relay_job_system_initialize_sync(Relay_JobSystem *system)
{
    if (pthread_mutex_init(&system->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&system->work_available, NULL) != 0) {
        (void)pthread_mutex_destroy(&system->mutex);
        return false;
    }
    if (pthread_cond_init(&system->idle, NULL) != 0) {
        (void)pthread_cond_destroy(&system->work_available);
        (void)pthread_mutex_destroy(&system->mutex);
        return false;
    }
    return true;
}

/** Destroy the POSIX synchronization primitives for a worker pool. */
static void relay_job_system_destroy_sync(Relay_JobSystem *system)
{
    (void)pthread_cond_destroy(&system->idle);
    (void)pthread_cond_destroy(&system->work_available);
    (void)pthread_mutex_destroy(&system->mutex);
}

/** Lock a worker-pool state transition. */
static void relay_job_system_lock(Relay_JobSystem *system)
{
    (void)pthread_mutex_lock(&system->mutex);
}

/** Unlock a worker-pool state transition. */
static void relay_job_system_unlock(Relay_JobSystem *system)
{
    (void)pthread_mutex_unlock(&system->mutex);
}

/** Wait until a worker-pool state transition occurs. */
static void relay_job_system_wait_for_idle(Relay_JobSystem *system)
{
    (void)pthread_cond_wait(&system->idle, &system->mutex);
}

#endif

/** Expand a full job queue while its mutex is held. */
static bool relay_job_system_grow_queue(Relay_JobSystem *system)
{
    Relay_Job *queue;
    size_t capacity;
    size_t index;

    if (system->queue_count < system->queue_capacity) {
        return true;
    }
    if (system->queue_capacity > SIZE_MAX / 2) {
        return false;
    }
    capacity = system->queue_capacity * 2;
    queue = calloc(capacity, sizeof(*queue));
    if (queue == NULL) {
        return false;
    }
    for (index = 0; index < system->queue_count; index++) {
        queue[index] = system->queue[(system->queue_head + index) %
            system->queue_capacity];
    }
    free(system->queue);
    system->queue = queue;
    system->queue_capacity = capacity;
    system->queue_head = 0;
    return true;
}

/** Release heap allocations and synchronization primitives after failed setup. */
static void relay_job_system_cleanup_failed_init(Relay_JobSystem *system,
    size_t created_workers)
{
    size_t index;

    relay_job_system_lock(system);
    system->accepting = false;
    relay_job_system_signal_workers(system);
    relay_job_system_unlock(system);
    for (index = 0; index < created_workers; index++) {
#if RELAY_PLATFORM_WINDOWS
        (void)WaitForSingleObject(system->workers[index], INFINITE);
        (void)CloseHandle(system->workers[index]);
#else
        (void)pthread_join(system->workers[index], NULL);
#endif
    }
    relay_job_system_destroy_sync(system);
    free(system->workers);
    free(system->queue);
    system->workers = NULL;
    system->queue = NULL;
}

bool relay_job_system_init(Relay_JobSystem *system, size_t worker_count)
{
    size_t index;

    if (system == NULL || system->initialized) {
        return false;
    }
    if (worker_count == 0) {
        worker_count = relay_job_system_default_worker_count();
    }
    if (worker_count == 0) {
        return false;
    }

    system->queue_capacity = 64;
    system->queue = calloc(system->queue_capacity, sizeof(*system->queue));
    system->workers = calloc(worker_count, sizeof(*system->workers));
    if (system->queue == NULL || system->workers == NULL ||
        !relay_job_system_initialize_sync(system)) {
        free(system->workers);
        free(system->queue);
        system->workers = NULL;
        system->queue = NULL;
        return false;
    }

    system->worker_count = worker_count;
    system->accepting = true;
    for (index = 0; index < worker_count; index++) {
#if RELAY_PLATFORM_WINDOWS
        system->workers[index] = CreateThread(NULL, 0, relay_job_system_worker,
            system, 0, NULL);
        if (system->workers[index] == NULL) {
#else
        if (pthread_create(&system->workers[index], NULL, relay_job_system_worker,
                system) != 0) {
#endif
            relay_job_system_cleanup_failed_init(system, index);
            return false;
        }
    }

    system->initialized = true;
    return true;
}

bool relay_job_system_submit(Relay_JobSystem *system, Relay_JobFunction function,
    void *context)
{
    size_t tail;

    if (system == NULL || function == NULL || !system->initialized) {
        return false;
    }

    relay_job_system_lock(system);
    if (!system->accepting || !relay_job_system_grow_queue(system)) {
        relay_job_system_unlock(system);
        return false;
    }
    tail = (system->queue_head + system->queue_count) % system->queue_capacity;
    system->queue[tail].function = function;
    system->queue[tail].context = context;
    system->queue_count++;
    relay_job_system_signal_workers(system);
    relay_job_system_unlock(system);
    return true;
}

void relay_job_system_wait_idle(Relay_JobSystem *system)
{
    if (system == NULL || !system->initialized) {
        return;
    }

    relay_job_system_lock(system);
    while (system->queue_count != 0 || system->active_count != 0) {
        relay_job_system_wait_for_idle(system);
    }
    relay_job_system_unlock(system);
}

void relay_job_system_shutdown(Relay_JobSystem *system)
{
    size_t index;

    if (system == NULL || !system->initialized) {
        return;
    }

    relay_job_system_lock(system);
    system->accepting = false;
    relay_job_system_signal_workers(system);
    relay_job_system_unlock(system);
    for (index = 0; index < system->worker_count; index++) {
#if RELAY_PLATFORM_WINDOWS
        (void)WaitForSingleObject(system->workers[index], INFINITE);
        (void)CloseHandle(system->workers[index]);
#else
        (void)pthread_join(system->workers[index], NULL);
#endif
    }
    relay_job_system_destroy_sync(system);
    free(system->workers);
    free(system->queue);
    system->workers = NULL;
    system->queue = NULL;
    system->queue_capacity = 0;
    system->queue_head = 0;
    system->queue_count = 0;
    system->active_count = 0;
    system->worker_count = 0;
    system->initialized = false;
}
