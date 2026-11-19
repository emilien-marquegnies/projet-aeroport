#include "simple_signal.h"
#include <stdio.h>
#include <signal.h>


void handle_signal(int signum, void (*handler)(int)){
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(signum, &sa, NULL);
}

