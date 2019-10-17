#ifndef PERFORMANCE_MONITOR_H_
#define PERFORMANCE_MONITOR_H_

double
pm_average_service_delay(long long cumulative_delay, 
                         long long num_samples);

double
pm_overload_degree(double average_service_delay,
                   double desired_delay_bound);

double
pm_smoothed_overload_degree(double current_overload_degree,
                            double previous_overload_degree,
                            double alpha);



#endif
