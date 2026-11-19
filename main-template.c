#include "airport.h"
#include <signal.h>
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

    // Initialize
    init_airport(&airport);

    // Install the handler BEFORE opening the airport otherwise the airport won't start.
    struct sigaction sa = {0};
    sa.sa_handler = on_plane;
    sigemptyset(&sa.sa_mask);
    sigaction(ARRIVAL_SIGNAL, &sa, NULL);

    // Opens the airport. This is the start of periods of all tasks.
    open_airport(airport);

    // We run one step for each task
    // Each call uses the runway for STEP_TIME - JITTER ms.
    inspect_runway(airport);
    if (land_step(airport) == NO_PLANE) {
        printf("No plane waiting: nothing to land\n");
    }
    plow_step(airport);

    // We wait for the end of the day (or a crash, or too many incidents).
    outcome_t outcome;
    wait_airport_closed(airport, &outcome);

    if (outcome == DAY_COMPLETED) {
        printf("The day is over\n");
    } else {
        printf("The airport closed early\n");
    }

    // Free resources
    free_airport(airport);
    return 0;
}
