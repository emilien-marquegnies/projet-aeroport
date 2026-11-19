#include <stdint.h>
#include <time.h>
#include "time_helper.h"


int64_t ts_to_ns(struct timespec timeTs){
    return timeTs.tv_sec * NSEC_PER_SEC + timeTs.tv_nsec;
}

struct timespec ns_to_ts(int64_t timeNs){
    return (struct timespec){
        .tv_sec = timeNs / NSEC_PER_SEC,
        .tv_nsec = timeNs % NSEC_PER_SEC
    };
}

struct timespec add_timespecs(struct timespec ts1,struct timespec ts2){
    int64_t ns1 = ts_to_ns(ts1);
    int64_t ns2 = ts_to_ns(ts2);
    return ns_to_ts(ns1+ns2);
}