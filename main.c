#include "airport.h"
#include "simple_signal.h"
#include "delay.h"
#include "watchdog.h"
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>


//TODO : improvement dymically compute it based on the values from airport.h instead of hardcoding it
#define HYPERPERIOD 12

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

// Important: this code ignores most errors! Do not do this.

// Called each time a plane arrives (signal ARRIVAL_SIGNAL, i.e. SIGUSR1).
// The landing is done by the landing thread so keep this as short (in runtime) as possible.
static void on_plane(int sig) {
    (void)sig;
    total_plane_arrived++;
}

static void* inspection_task(void* args){
    TaskArgs* taskArgs = args;

    uint32_t instances_in_cycles = HYPERPERIOD / INSPECTION_PERIOD;

    // Loop infinitly on cycles
    for (uint32_t cycle = 0; open_airport_flag; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (uint32_t step = 0; step < INSPECTION_STEPS; step++) {
                uint32_t activationStep = cycle * HYPERPERIOD + instance * INSPECTION_PERIOD;
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

    uint32_t instances_in_cycles = HYPERPERIOD / PLOW_PERIOD;

    // Loop infinitly on cycles
    for (uint32_t cycle = 0; open_airport_flag; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (uint32_t step = 0; step < PLOW_STEPS; step++) {
                uint32_t activationStep = cycle * HYPERPERIOD + instance * PLOW_PERIOD;
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

    uint32_t instances_in_cycles = HYPERPERIOD / LANDING_PERIOD;

    timer_t watchdog = watchdog_function(landing_expire);

    uint32_t consecutive_landing_steps = 0;

    // Loop infinitly on cycles
    for (uint32_t cycle = 0; ; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (uint32_t step = 0; step < LANDING_STEPS; step++) {
                uint32_t activationStep = cycle * HYPERPERIOD + instance * LANDING_PERIOD;
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

    pthread_t inspection_thread;
    pthread_t landing_thread;
    pthread_t plowing_thread;

    // Initialize the airport
    init_airport(&airport);

    // Install the handler BEFORE opening the airport otherwise the airport won't start.
    handle_signal(ARRIVAL_SIGNAL, on_plane);

    // Opens the airport. This is the start of periods of all tasks.
    open_airport(airport);
    // Computes the reference time t0
    TaskArgs args = {0};

    clock_gettime(CLOCK_REALTIME, &args.t0);

    pthread_create(&inspection_thread,NULL, &inspection_task, &args);
    pthread_create(&landing_thread,NULL, &landing_task, &args);
    pthread_create(&plowing_thread,NULL, &plowing_task, &args);

    outcome_t outcome;

    wait_airport_closed(airport, &outcome);
    open_airport_flag = 0;
    printf("Outcome : %d\n", outcome);
    // Free resources
    pthread_join(inspection_thread,NULL);
    pthread_join(landing_thread,NULL);
    pthread_join(plowing_thread,NULL);

    free_airport(airport);


    return 0;
}
