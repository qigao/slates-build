#include "sum.h"

double sample_sum(const int *values, size_t count) {
  double total = 0.0;
  for (size_t i = 0; i < count; ++i)
    total += values[i];
  return total;
}
