#ifndef SAMPLE_STATS_H
#define SAMPLE_STATS_H

#include <stddef.h>

#if defined(_WIN32) && defined(STATS_BUILD)
#define STATS_API __declspec(dllexport)
#elif defined(_WIN32)
#define STATS_API __declspec(dllimport)
#else
#define STATS_API __attribute__((visibility("default")))
#endif

/* An empty sequence has a mean of zero. */
STATS_API double stats_mean(const int *values, size_t count);

#endif
