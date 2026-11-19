#pragma once

#include <time.h>
#include <signal.h>

timer_t watchdog(void);
timer_t watchdog_function(void (*fonction)(union sigval));
void pet_watchdog(timer_t dog, int durationMs);