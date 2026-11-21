#include "airport.h"
#include "simple_signal.h"
#include "delay.h"
#include "watchdog.h"
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static uint32_t gcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static uint32_t lcm(uint32_t a, uint32_t b) {
    return a / gcd(a, b) * b;
}

uint32_t hyperperiod = 0;

volatile sig_atomic_t total_plane_arrived = 0;
volatile sig_atomic_t total_plane_landed = 0;


// Used the make sure that the threads ends if the airport is closed
volatile atomic_bool open_airport_flag = 1;

static const int inspection_activation_offsets[3][1] = {{0}, {0}, {1}};
static const int landing_activation_offsets[2][2] = {{1,2}, {1,2}};
static const int plowing_activation_offsets[1][3] = {{3,5,6}};

airport_t *airport;

typedef struct TaskArgs {
    struct timespec t0;
} TaskArgs;

// Called by the watch dog if the plane was too long to land
static void landing_expire(union sigval s){
    (void)s;
    error_t err = abort_landing(airport);
    
    if(err == INVALID_STATE){
        printf("THIS SHOULD NOT HAPPEN ! ABORT_LANDING CALLED WHILE NO PLANE WAS LANDING\n");
    }
}

// Called each time a plane arrives (signal ARRIVAL_SIGNAL, i.e. SIGUSR1).
// The landing is done by the landing thread so keep this as short (in runtime) as possible.
static void on_plane(int sig) {
    (void)sig;
    total_plane_arrived++;
}

static void* inspection_task(void* args){
    TaskArgs* taskArgs = args;

    uint32_t instances_in_cycles = hyperperiod / INSPECTION_PERIOD;

    // Loop infinitly on cycles
    for (uint32_t cycle = 0; open_airport_flag; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (uint32_t step = 0; step < INSPECTION_STEPS; step++) {
                uint32_t activationStep = cycle * hyperperiod + instance * INSPECTION_PERIOD;
                uint32_t delayInSteps = activationStep + inspection_activation_offsets[instance][step];
                delay_until(taskArgs->t0, delayInSteps * STEP_TIME);

                error_t err = inspect_runway(airport);

                if(err == AIRPORT_CLOSED || err == COLLISION || !open_airport_flag){
                    return NULL;
                }
            }
        }
    }
    return NULL;
}

static void* plowing_task(void* args){
    TaskArgs* taskArgs = args;

    uint32_t instances_in_cycles = hyperperiod / PLOW_PERIOD;

    // Loop infinitly on cycles
    for (uint32_t cycle = 0; open_airport_flag; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (uint32_t step = 0; step < PLOW_STEPS; step++) {
                uint32_t activationStep = cycle * hyperperiod + instance * PLOW_PERIOD;
                uint32_t delayInSteps = activationStep + plowing_activation_offsets[instance][step];
                delay_until(taskArgs->t0, delayInSteps * STEP_TIME);

                error_t err = plow_step(airport);

                if(err == AIRPORT_CLOSED || err == COLLISION || !open_airport_flag){
                    return NULL;
                }
            }
        }
    }

    return NULL;
}

static void* landing_task(void* args){
    TaskArgs* taskArgs = args;

    uint32_t instances_in_cycles = hyperperiod / LANDING_PERIOD;

    timer_t watchdog = watchdog_function(landing_expire);

    uint32_t consecutive_landing_steps = 0;

    // Loop infinitly on cycles
    for (uint32_t cycle = 0; ; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (uint32_t step = 0; step < LANDING_STEPS; step++) {
                uint32_t activationStep = cycle * hyperperiod + instance * LANDING_PERIOD;
                uint32_t delayInSteps = activationStep + landing_activation_offsets[instance][step];
                delay_until(taskArgs->t0, delayInSteps * STEP_TIME);
                if(!open_airport_flag){
                    timer_delete(watchdog);
                    return NULL;
                }

                if(total_plane_arrived > total_plane_landed){
                    //Start the watchdog timer to be a little bit before the end of the task in order to account
                    //for the time before the task starts
                    pet_watchdog(watchdog, STEP_TIME - (JITTER / 2));
                    error_t err = land_step(airport);
                    //Pet the watchdog and do not launch it back
                    pet_watchdog(watchdog, 0);


                    if(err == OK){
                        consecutive_landing_steps++;
                        if(consecutive_landing_steps == LANDING_STEPS){
                            total_plane_landed++;
                            consecutive_landing_steps = 0;
                        }
                    }
                    else if(err == AIRPORT_CLOSED || err == COLLISION){
                        timer_delete(watchdog);
                        return NULL;
                    }
                    else{
                        consecutive_landing_steps = 0;
                    }
                }
            }
        }
    }
    return NULL;

}

int main(void) {
    int return_code = EXIT_FAILURE;
    error_t err;
    int created_threads = 0;
    int thread_code = 0;
    TaskArgs args = {0};
    outcome_t outcome;
    hyperperiod = lcm(lcm(INSPECTION_PERIOD, LANDING_PERIOD), PLOW_PERIOD);

    pthread_t threads[3];

    // Initialize the airport
    err = init_airport(&airport);
    if(err != OK){
        fprintf(stderr, "Could not initialize airport : %d\n", err);
        return return_code;
    }

    // Install the handler BEFORE opening the airport otherwise the airport won't start.
    handle_signal(ARRIVAL_SIGNAL, on_plane);

    // Opens the airport. This is the start of periods of all tasks.
    err = open_airport(airport);
    if(err != OK){
        fprintf(stderr, "Could not open airport : %d\n", err);
        goto cleanup_airport;
    }
    
    // Computes the reference time t0
    if(clock_gettime(CLOCK_REALTIME, &args.t0) != 0){
        perror("clock_gettime");
        goto cleanup_airport;
    }

    thread_code = pthread_create(&threads[0],NULL, &inspection_task, &args);
    if(thread_code != 0){
        fprintf(stderr, "Error creating inspection thread : %s\n", strerror(thread_code));
        goto stop_threads;
    }
    created_threads++;

    thread_code = pthread_create(&threads[1],NULL, &landing_task, &args);
    if(thread_code != 0){
        fprintf(stderr, "Error creating landing thread : %s\n", strerror(thread_code));
        goto stop_threads;
    }
    created_threads++;

    thread_code = pthread_create(&threads[2],NULL, &plowing_task, &args);
    if(thread_code != 0){
        fprintf(stderr, "Error creating plowing thread : %s\n", strerror(thread_code));
        goto stop_threads;
    }
    created_threads++;

    err = wait_airport_closed(airport, &outcome);
    if(err != OK){
        fprintf(stderr, "Error while waiting for the airport to close : %d\n", err);
        goto stop_threads;
    }

    switch (outcome) {
        case DAY_COMPLETED:      
            printf("Day completed\n");
            return_code = EXIT_SUCCESS;
            break;
        case TOO_MANY_INCIDENTS:
            printf("Closed: too many incidents\n"); 
            break;
        case CRASHED:
            printf("Closed: crash\n");
            break;
        default:
            printf("Unexpected outcome %d\n", outcome);
            break;
    }

    // Free resources
    stop_threads:
        open_airport_flag = 0;
        for(int i = 0; i < created_threads; i++){
            pthread_join(threads[i],NULL);
        }

    cleanup_airport:
        free_airport(airport);
        return return_code;
}
