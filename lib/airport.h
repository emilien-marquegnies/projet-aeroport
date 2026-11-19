/**
 * @file airport.h
 * @brief API for a real-time single-runway airport simulation.
 *
 * The airport has a single runway shared by three tasks:
 *  - runway inspection,
 *  - plane landings,
 *  - snow plowing.
 *
 * Each task is made of "steps". A step occupies the runway for
 * STEP_TIME - JITTER ms. Only one step can use the runway at any time.
 * Otherwise, a crash happens.
 *
 * Periods start when open_airport is
 * called. If not enough steps were done in a period, an incident is
 * recorded. After MAX_INCIDENTS incidents, the airport is closed.
 *
 * Landings are planned periodically but do not happen at every step. Each time
 * a plane arrives, the library sends the signal ARRIVAL_SIGNAL (SIGUSR1) to the
 * process. Landing a plane takes LANDING_STEPS successful steps. The goal is to
 * land as many planes as possible.
 *
 * Some landing steps take too long (overrun). They must then be interrupted
 * with abort_landing before the end of their slot , otherwise the next activity
 * will crash into them. The landing in progress will then have to be restarted
 * (go around) and require LANDING_STEPS steps again.
 *
 * If everything goes well, the airport closes normally after OPENING_STEPS
 * steps (the end of the day).
 */

#pragma once

#include <signal.h>


// --- Constants and Macros ---

/**
 * @def STEP_TIME
 * @brief Duration of a time step (in ms).
 */
#ifndef STEP_TIME
#define STEP_TIME 500ULL
#endif

/**
 * @def JITTER
 * @brief Safety margin (in ms). A step actually occupies the runway for
 * STEP_TIME - JITTER ms, to account for delays from the OS.
 */
#ifndef JITTER
#define JITTER 100ULL
#endif

/**
 * @def INSPECTION_PERIOD
 * @brief Period of the runway inspection (in number of steps).
 */
#ifndef INSPECTION_PERIOD
#define INSPECTION_PERIOD 4
#endif

/**
 * @def INSPECTION_STEPS
 * @brief Number of inspection steps required in each inspection period.
 */
#ifndef INSPECTION_STEPS
#define INSPECTION_STEPS 1
#endif

/**
 * @def LANDING_PERIOD
 * @brief Period of the slots reserved for landings (in number of steps).
 *
 * The schedule must reserves LANDING_STEPS steps
 * for landing in each LANDING_PERIOD slots but not all of them will be spent
 * landing a plane (if none is waiting for example).
 */
#ifndef LANDING_PERIOD
#define LANDING_PERIOD 6
#endif

/**
 * @def LANDING_STEPS
 * @brief Number of successful steps needed to land one plane.
 *
 * The steps of a landing do not need to be consecutive. If a step is
 * interrupted (go around), the landing starts again and LANDING_STEPS new
 * successful steps are needed.
 */
#ifndef LANDING_STEPS
#define LANDING_STEPS 2
#endif

/**
 * @def PLOW_PERIOD
 * @brief Period of the snow plowing (in number of steps).
 */
#ifndef PLOW_PERIOD
#define PLOW_PERIOD 12
#endif

/**
 * @def PLOW_STEPS
 * @brief Number of plowing steps required in each plowing period.
 */
#ifndef PLOW_STEPS
#define PLOW_STEPS 3
#endif

/**
 * @def OPENING_STEPS
 * @brief Duration of a day at the airport (in number of steps).
 * Must be a multiple of every period.
 */
#ifndef OPENING_STEPS
#define OPENING_STEPS 120
#endif

/**
 * @def MAX_INCIDENTS
 * @brief Number of incidents (missed deadlines) after which the airport closes.
 */
#ifndef MAX_INCIDENTS
#define MAX_INCIDENTS 3
#endif

/**
 * @def ARRIVAL_SIGNAL
 * @brief Signal sent to the process each time a plane arrives.
 *
 * A handler must be installed (sigaction) before calling open_airport. This is
 * to prevent the program from stopping, which is the default result of SIGUSR1.
 * The signal is sent to the whole process, so any thread may execute the
 * handler.
 */
#define ARRIVAL_SIGNAL SIGUSR1

/**
 * @def ARRIVAL_MIN_INTERVAL
 * @brief Minimum time between two plane arrivals (in number of steps).
 *
 * The time between two arrivals is drawn at random between
 * ARRIVAL_MIN_INTERVAL and 2 * ARRIVAL_MIN_INTERVAL steps. It can be lowered
 * for testing, but never below LANDING_PERIOD.
 */
#ifndef ARRIVAL_MIN_INTERVAL
#define ARRIVAL_MIN_INTERVAL 12
#endif

/**
 * @def OVERRUN_PERCENT
 * @brief Probability (in percent) that a landing step takes too long.
 */
#ifndef OVERRUN_PERCENT
#define OVERRUN_PERCENT 10
#endif

/**
 * @def OVERRUN_TIME
 * @brief Duration (in ms) of a landing step that takes too long, if it is
 * not interrupted with abort_landing.
 */
#ifndef OVERRUN_TIME
#define OVERRUN_TIME (2ULL * STEP_TIME)
#endif

/**
 * @def RANDOM_SEED
 * @brief Seed of the random arrivals and overruns.
 *
 * If not defined, the seed changes at each run and is printed
 * when the airport opens.
 */


// --- Enumerations and Typedefs ---

/**
 * @enum error
 * @brief Possible results and errors returned by functions.
 */
enum error {
    /// No error, normal result
    OK = 0,
    /// The airport is closed (end of the day, too many incidents or crash).
    /// The step was not performed (or was not counted).
    AIRPORT_CLOSED = 1,
    /// The runway was already in use and the airport crashed.
    COLLISION = 2,
    /// The airport is in the wrong state (e.g. opened twice), or there was
    /// no landing step to interrupt (abort_landing).
    INVALID_STATE = 3,
    /// An argument was a NULL pointer
    NULL_PTR = 4,
    /// System errors, could come from the OS.
    /// Malloc error, lack of memory
    MALLOC = 5,
    /// Error when creating a timer
    TIMER_CREATE = 6,
    /// Error when setting the time of a timer
    TIMER_SETTIME = 7,
    /// Error when deleting a timer
    TIMER_DELETE = 8,
    /// Mutex or condition variable error
    MUTEX = 9,
    /// The landing step was interrupted by abort_landing (go around).
    /// The step is not counted and the landing must start again.
    LANDING_ABORTED = 10,
    /// No plane is waiting so the landing step was not performed.
    NO_PLANE = 11,
    /// No handler is installed for ARRIVAL_SIGNAL and he airport was not opened.
    NO_SIGNAL_HANDLER = 12,
};

/**
 * @typedef error_t
 * @brief Value indicating an error.
 * @see enum error
 */
typedef enum error error_t;

/**
 * @enum outcome
 * @brief How the day at the airport ended.
 */
enum outcome {
    /// The airport is still open (value used during execution)
    STILL_OPEN = 0,
    /// The day ended normally after OPENING_STEPS steps
    DAY_COMPLETED = 1,
    /// Too many deadlines were missed
    TOO_MANY_INCIDENTS = 2,
    /// Two tasks used the runway at the same time
    CRASHED = 3,
};

/**
 * @typedef outcome_t
 * @brief How the day at the airport ended.
 * @see enum outcome
 */
typedef enum outcome outcome_t;


// --- Opaque Structure ---

/**
 * @typedef airport_t
 * @brief The airport and its runway (Opaque structure).
 */
typedef struct airport airport_t;


// --- Airport Functions ---

/**
 * @brief Initializes the airport using dynamic memory allocation.
 *
 * The airport must be freed after use with free_airport.
 *
 * @param airport Pointer to a pointer of type airport_t, which will be set.
 * @return error_t Returns an error_t (OK or error code).
 */
error_t init_airport(airport_t **airport);

/**
 * @brief Frees the resources of the airport.
 *
 * Stops the deadline checks and the arrivals (without changing the outcome
 * of the day), waits for the checks already running, then frees the
 * resources. No thread (nor watchdog) may use the airport anymore when this
 * function is called.
 *
 * @param airport Pointer to the airport.
 * @return error_t Returns an error_t (OK or error code).
 */
error_t free_airport(airport_t *airport);

/**
 * @brief Opens the airport.
 *
 * The periods of all activities start at the moment this function is called,
 * and planes start to arrive. The airport can only be opened once.
 *
 * A handler for ARRIVAL_SIGNAL must already be installed. Otherwise, the
 * airport is not opened and NO_SIGNAL_HANDLER is returned.
 *
 * @param airport Pointer to the airport.
 * @return error_t Returns an error_t (OK or error code).
 */
error_t open_airport(airport_t *airport);

/**
 * @brief Waits until the airport is closed.
 *
 * Blocks the calling thread until the end of the day, a crash, or too many
 * incidents.
 *
 * @param airport Pointer to the airport.
 * @param outcome Pointer to an outcome_t output parameter (can be NULL).
 * @return error_t Returns an error_t (OK or error code).
 */
error_t wait_airport_closed(airport_t *airport, outcome_t *outcome);


// --- Runway Functions ---
// Each function performs one step and occupies the runway for
// STEP_TIME - JITTER ms. They are NOT protected against concurrent use.
// if the runway is already in use, the airport crashes.
// Once the airport is closed, they return AIRPORT_CLOSED immediately.

/**
 * @brief Performs one step of runway inspection.
 *
 * @param airport Pointer to the airport.
 * @return error_t OK, AIRPORT_CLOSED, COLLISION or another error code.
 */
error_t inspect_runway(airport_t *airport);

/**
 * @brief Performs one step of a plane landing.
 *
 * A plane must be waiting. Otherwise, the runway is not used and NO_PLANE is
 * returned. After LANDING_STEPS successful steps, the plane has landed.
 *
 * With a probability of OVERRUN_PERCENT %, the step takes too long and keeps
 * the runway for OVERRUN_TIME ms unless it is interrupted with abort_landing.
 * An interrupted step returns LANDING_ABORTED. The runway is released
 * immediately, the step is not counted and the landing must start again.
 *
 * @param airport Pointer to the airport.
 * @return error_t OK, LANDING_ABORTED, NO_PLANE, AIRPORT_CLOSED, COLLISION
 *         or another error code.
 */
error_t land_step(airport_t *airport);

/**
 * @brief Performs one step of snow plowing.
 *
 * @param airport Pointer to the airport.
 * @return error_t OK, AIRPORT_CLOSED, COLLISION or another error code.
 */
error_t plow_step(airport_t *airport);

/**
 * @brief Interrupts the landing step in progress (go around).
 *
 * The landing step currently on the runway returns LANDING_ABORTED and
 * releases the runway immediately. If no landing step is in progress,
 * nothing happens and INVALID_STATE is returned.
 *
 * This function can be called from a signal
 * handler, or from a timer notification thread (e.g. a watchdog).
 * Note that a watchdog that fires after the end of its step could interrupt
 * a later landing step. Disarm it as soon as land_step returns.
 *
 * @param airport Pointer to the airport.
 * @return error_t OK if a step was interrupted, INVALID_STATE or NULL_PTR.
 */
error_t abort_landing(airport_t *airport);
