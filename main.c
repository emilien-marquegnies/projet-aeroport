#include "airport.h"
#include "simple_signal.h"
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>


volatile sig_atomic_t total_plane_arrived;
volatile sig_atomic_t total_plane_landed;

airport_t *airport;

// Important: this code ignores most errors! Do not do this.

// Called each time a plane arrives (signal ARRIVAL_SIGNAL, i.e. SIGUSR1).
// The landing is done by the landing thread so keep this as short (in runtime) as possible.
static void on_plane(int sig) {
    printf("New plane  arriving !");
}

static void inspection_task(void* args){
    TaskArgs* taskArgs = args;
}

static void landing_task(void* args){
    TaskArgs* taskArgs = args;

}

static void snow_removal_task(void* args){
    TaskArgs* taskArgs = args;

}

typedef struct TaskArgs {
    struct timespec t0;
} TaskArgs;

int main(void) {

    pthread_t inspection_thread;
    pthread_t landing_thread;
    pthread_t snow_removal_thread;

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
    pthread_create(&snow_removal_thread,NULL, &snow_removal_task, &args);


    

    // Free resources
    pthread_join(&inspection_thread,NULL);
    pthread_join(&landing_thread,NULL);
    pthread_join(&snow_removal_thread,NULL);

    free_airport(airport);


    return 0;
}
