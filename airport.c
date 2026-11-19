#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "airport.h"

// --- Compile-time checks on the parameters ---

_Static_assert(JITTER < STEP_TIME, "JITTER must be smaller than STEP_TIME");
_Static_assert(OPENING_STEPS % INSPECTION_PERIOD == 0, "OPENING_STEPS must be a multiple of INSPECTION_PERIOD");
_Static_assert(OPENING_STEPS % LANDING_PERIOD == 0, "OPENING_STEPS must be a multiple of LANDING_PERIOD");
_Static_assert(OPENING_STEPS % PLOW_PERIOD == 0, "OPENING_STEPS must be a multiple of PLOW_PERIOD");
_Static_assert(ARRIVAL_MIN_INTERVAL >= LANDING_PERIOD, "ARRIVAL_MIN_INTERVAL cannot be smaller than LANDING_PERIOD");
_Static_assert(OVERRUN_PERCENT <= 100, "OVERRUN_PERCENT is a percentage");
_Static_assert(OVERRUN_TIME >= STEP_TIME, "an overrun must last longer than a slot");
_Static_assert(LANDING_STEPS >= 1, "a landing needs at least one step");
// The atomic used by abort_landing may not hide a lock
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic_int must be lock-free");

// Periodic activities checked by the library (landings are not periodic)
#define NUM_MONITORS 2

typedef enum activity {
    NO_ACTIVITY = 0,
    INSPECTION = 1,
    LANDING = 2,
    PLOWING = 3,
} activity_t;

static const char *activity_names[] = {
    [NO_ACTIVITY] = "nothing",
    [INSPECTION] = "Inspection",
    [LANDING] = "Landing",
    [PLOWING] = "Plowing",
};

// State of the landing step on the runway, used by abort_landing
enum landing_state {
    LANDING_IDLE = 0,
    LANDING_RUNNING = 1,
    LANDING_ABORT_REQUESTED = 2,
};

typedef struct monitor {
    airport_t *airport;
    activity_t activity;
    unsigned int period;       // in slots
    unsigned int required;     // steps required per period
    unsigned int checks_left;  // remaining deadline checks before the end of the day
    unsigned int period_index; // index of the current period
    atomic_uint done;          // steps done in the current period
    timer_t timer;
    int timer_created;
} monitor_t;

struct airport {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_int opened;
    atomic_int outcome;        // outcome of wait_airport_closed (STILL_OPEN while the airport runs)
    atomic_int occupant;       // activity_t currently on the runway
    atomic_int landing_state;  // enum landing_state
    int incidents;             // protected by mutex
    int finished_monitors;     // protected by mutex
    int running_handlers;      // protected by mutex
    // Planes, protected by mutex
    unsigned int planes_arrived;
    unsigned int planes_waiting;
    unsigned int planes_landed;
    unsigned int go_arounds;
    unsigned int landing_progress; // successful steps of the landing in progress
    // Randomness, protected by mutex
    unsigned int seed;
    unsigned int arrival_rand;
    unsigned int overrun_rand;
    struct timespec opening;   // set once in open_airport
    struct timespec next_arrival; // protected by mutex
    timer_t arrival_timer;
    int arrival_timer_created;
    monitor_t monitors[NUM_MONITORS];
};


// --- Time helpers ---

static void add_ms(struct timespec *t, unsigned long long ms) {
    unsigned long long nsec = t->tv_nsec + (ms % 1000ULL) * 1000000ULL;
    t->tv_sec += ms / 1000ULL + nsec / 1000000000ULL;
    t->tv_nsec = nsec % 1000000000ULL;
}

static int is_before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static double elapsed_s(const airport_t *a) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)(now.tv_sec - a->opening.tv_sec) + (double)(now.tv_nsec - a->opening.tv_nsec) / 1e9;
}

static double diff_ms(const struct timespec *from, const struct timespec *to) {
    return (double)(to->tv_sec - from->tv_sec) * 1e3 + (double)(to->tv_nsec - from->tv_nsec) / 1e6;
}

// Random time until the next arrival ([ARRIVAL_MIN_INTERVAL, 2 * ARRIVAL_MIN_INTERVAL])
static unsigned long long next_arrival_ms(airport_t *a) {
    unsigned long long min = (unsigned long long)ARRIVAL_MIN_INTERVAL * STEP_TIME;
    return min + (unsigned long long)rand_r(&a->arrival_rand) % (min + 1ULL);
}


// --- Closing ---

static void stop_timer(timer_t timer) {
    struct itimerspec ts = {0};
    timer_settime(timer, 0, &ts, NULL);
}

// Must be called with the mutex locked
static void close_airport_locked(airport_t *a, outcome_t outcome) {
    int expected = STILL_OPEN;
    if (!atomic_compare_exchange_strong(&a->outcome, &expected, outcome)) {
        return; // already closed
    }
    for (int i = 0; i < NUM_MONITORS; ++i) {
        if (a->monitors[i].timer_created) {
            stop_timer(a->monitors[i].timer);
        }
    }
    if (a->arrival_timer_created) {
        stop_timer(a->arrival_timer);
    }
    double t = elapsed_s(a);
    switch (outcome) {
    case DAY_COMPLETED:
        printf("[%7.3f s] End of the day: airport closed with %d incident(s).\n", t, a->incidents);
        break;
    case TOO_MANY_INCIDENTS:
        printf("[%7.3f s] Too many incidents (%d): the airport is closed!\n", t, a->incidents);
        break;
    case CRASHED:
        printf("[%7.3f s] The airport is closed after a crash.\n", t);
        break;
    default:
        break;
    }
    printf("[%7.3f s] Planes: %u arrived, %u landed, %u still waiting, %u go-around(s).\n",
           t, a->planes_arrived, a->planes_landed, a->planes_waiting, a->go_arounds);
    fflush(stdout);
    pthread_cond_broadcast(&a->cond);
}


// --- Deadline checks ---

static void deadline_handler(union sigval sv) {
    monitor_t *m = sv.sival_ptr;
    airport_t *a = m->airport;
    unsigned int done = atomic_exchange(&m->done, 0);

    if (pthread_mutex_lock(&a->mutex)) {
        return;
    }
    a->running_handlers++;
    if (atomic_load(&a->outcome) == STILL_OPEN) {
        double t = elapsed_s(a);
        unsigned int index = m->period_index++;
        if (done < m->required) {
            a->incidents++;
            printf("[%7.3f s] INCIDENT: %s did only %u/%u step(s) in period %u (incident %d/%d)\n",
                   t, activity_names[m->activity], done, m->required, index, a->incidents, MAX_INCIDENTS);
            if (a->incidents >= MAX_INCIDENTS) {
                close_airport_locked(a, TOO_MANY_INCIDENTS);
            }
        } else if (done > m->required) {
            printf("[%7.3f s] Deadline: %s %u/%u step(s) in period %u (%u useless step(s))\n",
                   t, activity_names[m->activity], done, m->required, index, done - m->required);
        } else {
            printf("[%7.3f s] Deadline: %s %u/%u step(s) in period %u\n",
                   t, activity_names[m->activity], done, m->required, index);
        }
        if (atomic_load(&a->outcome) == STILL_OPEN && --m->checks_left == 0) {
            stop_timer(m->timer);
            if (++a->finished_monitors == NUM_MONITORS) {
                close_airport_locked(a, DAY_COMPLETED);
            }
        }
        fflush(stdout);
    }
    a->running_handlers--;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->mutex);
}


// --- Plane arrivals ---

static void arrival_handler(union sigval sv) {
    airport_t *a = sv.sival_ptr;
    int send = 0;

    if (pthread_mutex_lock(&a->mutex)) {
        return;
    }
    a->running_handlers++;
    if (atomic_load(&a->outcome) == STILL_OPEN) {
        // The plane is counted before the signal is sent
        a->planes_arrived++;
        a->planes_waiting++;
        printf("[%7.3f s] Plane arrived (%u waiting)\n", elapsed_s(a), a->planes_waiting);
        fflush(stdout);
        // Next arrival computed from the previous one
        add_ms(&a->next_arrival, next_arrival_ms(a));
        struct itimerspec ts = {0};
        ts.it_value = a->next_arrival;
        timer_settime(a->arrival_timer, TIMER_ABSTIME, &ts, NULL);
        send = 1;
    }
    a->running_handlers--;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->mutex);
    if (send) {
        kill(getpid(), ARRIVAL_SIGNAL);
    }
}


// --- Airport functions ---

static void init_monitor(airport_t *a, monitor_t *m, activity_t activity,
                         unsigned int period, unsigned int required) {
    m->airport = a;
    m->activity = activity;
    m->period = period;
    m->required = required;
    m->checks_left = OPENING_STEPS / period;
    m->period_index = 0;
    atomic_init(&m->done, 0);
    m->timer_created = 0;
}

static error_t create_timer(timer_t *timer, void (*handler)(union sigval), void *arg) {
    struct sigevent se = {0};
    se.sigev_notify = SIGEV_THREAD;
    se.sigev_notify_function = handler;
    se.sigev_value.sival_ptr = arg;
    se.sigev_notify_attributes = NULL;
    if (timer_create(CLOCK_REALTIME, &se, timer)) {
        return TIMER_CREATE;
    }
    return OK;
}

static monitor_t *monitor_of(airport_t *a, activity_t activity) {
    switch (activity) {
    case INSPECTION:
        return &a->monitors[0];
    case PLOWING:
        return &a->monitors[1];
    default:
        return NULL;
    }
}

error_t init_airport(airport_t **airport) {
    if (!airport) {
        return NULL_PTR;
    }
    *airport = NULL;
    airport_t *a = calloc(1, sizeof(airport_t));
    if (!a) {
        return MALLOC;
    }
    error_t res = OK;
    if (pthread_mutex_init(&a->mutex, NULL)) {
        res = MUTEX;
        goto err_mutex;
    }
    if (pthread_cond_init(&a->cond, NULL)) {
        res = MUTEX;
        goto err_cond;
    }
    atomic_init(&a->opened, 0);
    atomic_init(&a->outcome, STILL_OPEN);
    atomic_init(&a->occupant, NO_ACTIVITY);
    atomic_init(&a->landing_state, LANDING_IDLE);
#ifdef RANDOM_SEED
    a->seed = (unsigned int)(RANDOM_SEED);
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    a->seed = (unsigned int)(now.tv_nsec ^ now.tv_sec ^ getpid()) % 1000000U;
#endif
    a->arrival_rand = a->seed;
    a->overrun_rand = a->seed ^ 0x5bd1e995U;
    init_monitor(a, &a->monitors[0], INSPECTION, INSPECTION_PERIOD, INSPECTION_STEPS);
    init_monitor(a, &a->monitors[1], PLOWING, PLOW_PERIOD, PLOW_STEPS);
    int created_monitors = 0;
    for (; created_monitors < NUM_MONITORS; ++created_monitors) {
        res = create_timer(&a->monitors[created_monitors].timer, deadline_handler, &a->monitors[created_monitors]);
        a->monitors[created_monitors].timer_created = (res == OK);
        if (res != OK) {
          goto err_monitors;
        }
    }
    res = create_timer(&a->arrival_timer, arrival_handler, a);
    a->arrival_timer_created = (res == OK);
    if (res != OK) {
        goto err_monitors; 
    }
    *airport = a;
    return res;
err_monitors:
    created_monitors--;
    for (; created_monitors >= 0; --created_monitors) {
        a->monitors[created_monitors].timer_created = 0;
        timer_delete(a->monitors[created_monitors].timer);
    }
    pthread_cond_destroy(&a->cond);
err_cond:
    pthread_mutex_destroy(&a->mutex);
err_mutex:
    free(a);
    return res;
}

error_t free_airport(airport_t *a) {
    if (!a) {
        return NULL_PTR;
    }
    error_t res = OK;
    if (pthread_mutex_lock(&a->mutex)) {
        return MUTEX;
    }
    // Stop the timers (without changing the outcome) and wait for running handlers
    for (int i = 0; i < NUM_MONITORS; ++i) {
        stop_timer(a->monitors[i].timer);
    }
    stop_timer(a->arrival_timer);
    while (a->running_handlers > 0) {
        pthread_cond_wait(&a->cond, &a->mutex);
    }
    pthread_mutex_unlock(&a->mutex);
    for (int i = 0; i < NUM_MONITORS; ++i) {
        if (timer_delete(a->monitors[i].timer)) {
            res = TIMER_DELETE;
        }
    }
    if (timer_delete(a->arrival_timer)) {
        res = TIMER_DELETE;
    }
    if (pthread_cond_destroy(&a->cond) || pthread_mutex_destroy(&a->mutex)) {
        res = MUTEX;
    }
    free(a);
    return res;
}

error_t open_airport(airport_t *a) {
    if (!a) {
        return NULL_PTR;
    }
    // Without a handler, the first arrival would terminate the process
    struct sigaction old;
    if (sigaction(ARRIVAL_SIGNAL, NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL) {
        return NO_SIGNAL_HANDLER;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&a->opened, &expected, 1)) {
        return INVALID_STATE;
    }
    if (pthread_mutex_lock(&a->mutex)) {
        return MUTEX;
    }
    clock_gettime(CLOCK_REALTIME, &a->opening);
    error_t res = OK;
    for (int i = 0; i < NUM_MONITORS; ++i) {
        monitor_t *m = &a->monitors[i];
        unsigned long long period_ms = m->period * STEP_TIME;
        struct itimerspec ts;
        ts.it_value = a->opening;
        add_ms(&ts.it_value, period_ms);
        ts.it_interval.tv_sec = period_ms / 1000ULL;
        ts.it_interval.tv_nsec = (period_ms % 1000ULL) * 1000000ULL;
        if (timer_settime(m->timer, TIMER_ABSTIME, &ts, NULL)) {
            res = TIMER_SETTIME;
            break;
        }
    }
    if (res == OK) {
        struct itimerspec ts = {0};
        a->next_arrival = a->opening;
        add_ms(&a->next_arrival, next_arrival_ms(a));
        ts.it_value = a->next_arrival;
        if (timer_settime(a->arrival_timer, TIMER_ABSTIME, &ts, NULL)) {
            res = TIMER_SETTIME;
        }
    }
    if (res == OK) {
        printf("[%7.3f s] Airport open for %d slots of %llu ms (random seed %u).\n",
               0.0, OPENING_STEPS, STEP_TIME, a->seed);
        fflush(stdout);
    }
    pthread_mutex_unlock(&a->mutex);
    return res;
}

error_t wait_airport_closed(airport_t *a, outcome_t *outcome) {
    if (!a) {
        return NULL_PTR;
    }
    if (!atomic_load(&a->opened)) {
        return INVALID_STATE;
    }
    if (pthread_mutex_lock(&a->mutex)) {
        return MUTEX;
    }
    while (atomic_load(&a->outcome) == STILL_OPEN) {
        if (pthread_cond_wait(&a->cond, &a->mutex)) {
            pthread_mutex_unlock(&a->mutex);
            return MUTEX;
        }
    }
    if (outcome) {
        *outcome = atomic_load(&a->outcome);
    }
    pthread_mutex_unlock(&a->mutex);
    return OK;
}


// --- Runway functions ---

// Occupies the runway for ms milliseconds (busy wait). Returns 1 if a landing
// step was interrupted by abort_landing, 0 otherwise. Stops early if the
// airport closes (for instance because another activity crashed into it)
static int occupy_runway(airport_t *a, activity_t activity, unsigned long long ms) {
    struct timespec end, now;
    clock_gettime(CLOCK_MONOTONIC, &end);
    add_ms(&end, ms);
    do {
        if (activity == LANDING && atomic_load(&a->landing_state) == LANDING_ABORT_REQUESTED) {
            return 1;
        }
        if (atomic_load(&a->outcome) != STILL_OPEN) {
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (is_before(&now, &end));
    return 0;
}

// Bookkeeping after a landing step, with the runway already released
static error_t end_landing_step(airport_t *a, int aborted, double duration_ms) {
    if (pthread_mutex_lock(&a->mutex)) {
        return MUTEX;
    }
    error_t res = OK;
    if (atomic_load(&a->outcome) != STILL_OPEN) {
        res = AIRPORT_CLOSED;
    } else if (aborted) {
        a->landing_progress = 0;
        a->go_arounds++;
        printf("[%7.3f s] Landing step interrupted after %.0f ms: go-around (%u waiting)\n",
               elapsed_s(a), duration_ms, a->planes_waiting);
        res = LANDING_ABORTED;
    } else if (++a->landing_progress == LANDING_STEPS) {
        a->landing_progress = 0;
        a->planes_waiting--;
        a->planes_landed++;
        printf("[%7.3f s] Plane landed (%u waiting)\n", elapsed_s(a), a->planes_waiting);
    }
    fflush(stdout);
    pthread_mutex_unlock(&a->mutex);
    return res;
}

static error_t do_step(airport_t *a, activity_t activity) {
    if (!a) {
        return NULL_PTR;
    }
    if (!atomic_load(&a->opened)) {
        return INVALID_STATE;
    }
    if (atomic_load(&a->outcome) != STILL_OPEN) {
        return AIRPORT_CLOSED;
    }
    unsigned long long duration = STEP_TIME - JITTER;
    if (activity == LANDING) {
        if (pthread_mutex_lock(&a->mutex)) {
            return MUTEX;
        }
        unsigned int waiting = a->planes_waiting;
        int overrun = waiting > 0 && (unsigned int)(rand_r(&a->overrun_rand) % 100) < OVERRUN_PERCENT;
        pthread_mutex_unlock(&a->mutex);
        if (waiting == 0) {
            return NO_PLANE;
        }
        if (overrun) {
            duration = OVERRUN_TIME;
        }
    }
    int expected = NO_ACTIVITY;
    if (!atomic_compare_exchange_strong(&a->occupant, &expected, activity)) {
        if (pthread_mutex_lock(&a->mutex)) {
            return MUTEX;
        }
        if (atomic_load(&a->outcome) == STILL_OPEN) {
            printf("[%7.3f s] CRASH! %s entered the runway while %s was on it!\n",
                   elapsed_s(a), activity_names[activity], activity_names[expected]);
        }
        close_airport_locked(a, CRASHED);
        pthread_mutex_unlock(&a->mutex);
        return COLLISION;
    }
    if (activity == LANDING) {
        atomic_store(&a->landing_state, LANDING_RUNNING);
    }
    printf("[%7.3f s] %s step\n", elapsed_s(a), activity_names[activity]);
    fflush(stdout);
    struct timespec start, stop;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int aborted = occupy_runway(a, activity, duration);
    if (activity == LANDING) {
        // An abort requested at the very end of the step still counts as a go-around
        aborted = atomic_exchange(&a->landing_state, LANDING_IDLE) == LANDING_ABORT_REQUESTED;
    }
    atomic_store(&a->occupant, NO_ACTIVITY);
    clock_gettime(CLOCK_MONOTONIC, &stop);

    if (activity == LANDING) {
        return end_landing_step(a, aborted, diff_ms(&start, &stop));
    }
    if (atomic_load(&a->outcome) != STILL_OPEN) {
        return AIRPORT_CLOSED;
    }
    atomic_fetch_add(&monitor_of(a, activity)->done, 1);
    return OK;
}

error_t inspect_runway(airport_t *a) {
    return do_step(a, INSPECTION);
}

error_t land_step(airport_t *a) {
    return do_step(a, LANDING);
}

error_t plow_step(airport_t *a) {
    return do_step(a, PLOWING);
}

error_t abort_landing(airport_t *a) {
    if (!a) {
        return NULL_PTR;
    }

    int expected = LANDING_RUNNING;
    if (atomic_compare_exchange_strong(&a->landing_state, &expected, LANDING_ABORT_REQUESTED)) {
        return OK;
    }
    return INVALID_STATE;
}
