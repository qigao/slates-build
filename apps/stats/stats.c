#include "stats.h"
#include "sum.h"

double stats_mean(const int *values, size_t count) {
  return count ? sample_sum(values, count) / (double)count : 0.0;
}
