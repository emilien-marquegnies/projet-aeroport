#pragma once

#include <stdint.h>
#include <time.h>

#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000LL
#define NSEC_PER_USEC 1000LL

int64_t ts_to_ns(struct timespec timeTs);
struct timespec ns_to_ts(int64_t timeNs);
struct timespec add_timespecs(struct timespec ts1,struct timespec ts2);