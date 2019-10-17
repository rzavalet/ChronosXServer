#include <stdio.h>

double
pm_average_service_delay(long long cumulative_delay, 
                         long long num_samples)
{
  if (num_samples > 0) {
    return cumulative_delay / num_samples;
  }

  return 0;
}


double
pm_overload_degree(double average_service_delay,
                   double desired_delay_bound)
{
  if (desired_delay_bound > 0) {
    return (average_service_delay - desired_delay_bound) / desired_delay_bound;
  }

  return 0.0;
}


double
pm_smoothed_overload_degree(double current_overload_degree,
                            double previous_overload_degree,
                            double alpha)
{
  return (alpha * current_overload_degree) + ((1.0 - alpha) * previous_overload_degree);
}


