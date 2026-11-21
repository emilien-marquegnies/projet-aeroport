#include "airport.h"
#include "simple_signal.h"
#include "delay.h"
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

#define HYPERPERIOD 12
volatile sig_atomic_t total_plane_arrived = 0;
volatile sig_atomic_t total_plane_landed = 0;

volatile sig_atomic_t consecutive_landing_steps = 0;

static const int inspection_activation_offsets[3][1] = {{0}, {0}, {1}};
static const int landing_activation_offsets[2][1] = {{1,2}, {1,2}};
static const int plowing_activation_offsets[1][3] = {{3,5,6}};

airport_t *airport;

// Important: this code ignores most errors! Do not do this.

// Called each time a plane arrives (signal ARRIVAL_SIGNAL, i.e. SIGUSR1).
// The landing is done by the landing thread so keep this as short (in runtime) as possible.
static void on_plane(int sig) {
    printf("New plane  arriving !");
    total_plane_arrived++;
}

static void* inspection_task(void* args){
    TaskArgs* taskArgs = args;

    __uint32_t instances_in_cycles = HYPERPERIOD / INSPECTION_PERIOD;

    // Loop infinitly on cycles
    for (__uint32_t cycle = 0; ; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (__uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (__uint32_t step = 0; step < INSPECTION_STEPS; step++) {
                __uint32_t activationStep = cycle * HYPERPERIOD + instance * INSPECTION_PERIOD;
                __uint32_t delayInSteps = activationStep + inspection_activation_offsets[instance][step];
                delay_until(taskArgs->t0, delayInSteps * STEP_TIME);

                //TODO : handle runway error
                //TODO : change infinite loop to either be stopped by error handling or by global flag
                inspect_runway(airport);
            }
        }
    }
}

static void* plowing_task(void* args){
    TaskArgs* taskArgs = args;

    __uint32_t instances_in_cycles = HYPERPERIOD / PLOW_PERIOD;

    // Loop infinitly on cycles
    for (__uint32_t cycle = 0; ; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (__uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (__uint32_t step = 0; step < PLOW_STEPS; step++) {
                __uint32_t activationStep = cycle * HYPERPERIOD + instance * PLOW_PERIOD;
                __uint32_t delayInSteps = activationStep + plowing_activation_offsets[instance][step];
                delay_until(taskArgs->t0, delayInSteps * STEP_TIME);

                //TODO : handle plowing error (if any)
                //TODO : change infinite loop to either be stopped by error handling or by global flag
                plow_step(airport);
            }
        }
    }
}

static void* landing_task(void* args){
    TaskArgs* taskArgs = args;

    __uint32_t instances_in_cycles = HYPERPERIOD / LANDING_PERIOD;

    // Loop infinitly on cycles
    for (__uint32_t cycle = 0; ; cycle++) {
        //Loop on the instances (aka the numbers of time the task is executed in one hyperperiod)
        for (__uint32_t instance = 0; instance < instances_in_cycles; instance++) {
            //Loop on the step inside an execution of the task
            for (__uint32_t step = 0; step < LANDING_STEPS; step++) {
                __uint32_t activationStep = cycle * HYPERPERIOD + instance * LANDING_PERIOD;
                __uint32_t delayInSteps = activationStep + landing_activation_offsets[instance][step];
                delay_until(taskArgs->t0, delayInSteps * STEP_TIME);

                //TODO : handle landing error, prevent consecutive_step from incrementing in case of error
                //TODO : change infinite loop to either be stopped by error handling or by global flag
                //TODO : add watchdog 
                if(total_plane_arrived > total_plane_landed){
                    land_step(airport);
                    consecutive_landing_steps++;
                    if(consecutive_landing_steps == LANDING_STEPS){
                        total_plane_landed++;
                        consecutive_landing_steps = 0;
                    }
                }
            }
        }
    }
}



typedef struct TaskArgs {
    struct timespec t0;
} TaskArgs;

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
    TaskArgs args = {

    };

    clock_gettime(CLOCK_REALTIME, &args.t0);

    pthread_create(&inspection_thread,NULL, &inspection_task, &args);
    pthread_create(&landing_thread,NULL, &landing_task, &args);
    pthread_create(&plowing_thread,NULL, &plowing_task, &args);


    // Free resources
    pthread_join(&inspection_thread,NULL);
    pthread_join(&landing_thread,NULL);
    pthread_join(&plowing_thread,NULL);

    free_airport(airport);


    return 0;
}
