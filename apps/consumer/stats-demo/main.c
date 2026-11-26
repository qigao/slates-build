#include "stats.h"

#include <stdio.h>

int main(void) {
  const int values[] = {2, 4, 6};
  const double mean = stats_mean(values, sizeof(values) / sizeof(values[0]));
  printf("Mean = %.1f\n", mean);
  return mean == 4.0 ? 0 : 1;
}
