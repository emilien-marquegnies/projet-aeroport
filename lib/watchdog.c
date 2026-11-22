#include "watchdog.h"
#include "time_helper.h"
#include <signal.h>
#include <time.h>
#include <unistd.h>


static void timer_expire(union sigval s){
    kill(getpid(), s.sival_int);
}

timer_t watchdog(void){
    return watchdog_function(timer_expire);
}

timer_t watchdog_function(void (*fonction)(union sigval)){
    struct sigevent se = {0};
    se.sigev_notify = SIGEV_THREAD;
    se.sigev_notify_function = fonction;
    se.sigev_value.sival_int = SIGALRM;


    timer_t timer;
    timer_create(CLOCK_REALTIME, &se, &timer);

    return timer;
}

void pet_watchdog(timer_t dog, int durationMs){
    struct itimerspec spec = {.it_value=ns_to_ts(durationMs*NSEC_PER_MSEC)};
    timer_settime(dog, 0, &spec, NULL);
}