#include "airport.h"
#include "simple_signal.h"
#include <signal.h>
#include <time.h>
#include <stdio.h>



airport_t *airport;

// Important: this code ignores most errors! Do not do this.

// Called each time a plane arrives (signal ARRIVAL_SIGNAL, i.e. SIGUSR1).
// The landing is done by the landing thread so keep this as short (in runtime) as possible.
static void on_plane(int sig) {
    (void)sig;
    // TODO
}

int main(void) {

    // Initialize the airport
    init_airport(&airport);

    // Install the handler BEFORE opening the airport otherwise the airport won't start.
    handle_signal(ARRIVAL_SIGNAL, on_plane);

    // Opens the airport. This is the start of periods of all tasks.
    open_airport(airport);

    // Computes the reference time t0
    struct timespec t0;
    clock_gettime(CLOCK_REALTIME, &t0);

    //TODO : next step : create a thread per activity

    // Free resources
    free_airport(airport);
    return 0;
}
