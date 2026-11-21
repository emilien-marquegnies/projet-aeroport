#include "delay.h"
#include "time_helper.h"
#include <errno.h>
#include <time.h>

void delay_until(struct timespec start, int delayMs) {
    struct timespec end = add_timespecs(start, ns_to_ts(delayMs*NSEC_PER_MSEC));

    while(clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &end, NULL) == EINTR){}
}
