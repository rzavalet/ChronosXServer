#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"
#include "stats.h"

int *rtus = NULL;
long int *response_times_array = NULL;
long int rtusum = 0;
long int sample_response;
long int max_response_time = 0;
long int min_response_time = 0;
int print_samples = 0;
struct timeval xact_start;
struct timeval xact_end;

int 
init_stats_struct()
{
  rtus = calloc(RTBINS + 1, sizeof(int));
  response_times_array = calloc(NUM_SAMPLES, sizeof(long int));
}

int 
print_stats_to_csv()
{
  int   i;
  int   rc = 0;
  FILE *fileP = NULL;
  char *file_name = "stats.csv";

  fileP = fopen(file_name, "w+");
  if (fileP == NULL) {
    server_error("Failed to open stats file: %s", file_name);
    goto failXit;
  }

  if (response_times_array == NULL) {
    server_error("Response time array is not yet initialized");
    goto failXit;
  }

  fprintf(fileP, "response_time\n");
  for (i=0; i<NUM_SAMPLES; i++) {
    fprintf(fileP, "%ld\n", response_times_array[i]);
  }

  goto cleanup;

failXit:
  rc = 1;

cleanup:
  if (fileP != NULL) {
    fclose(fileP);
    fileP = NULL;
  }

  return rc;
}

/* Time difference in milliseconds */
long int 
diff_time(struct timeval * start,struct timeval *end)
{
  return ( (end->tv_sec*1000 + end->tv_usec/1000) -
           (start->tv_sec*1000 + start->tv_usec/1000) );
}

/* Time difference in microseconds */
long int 
udiff_time(struct timeval * start,struct timeval *end)
{
  return ( (  end->tv_sec*1000000 + end->tv_usec) -
           (start->tv_sec*1000000 + start->tv_usec) );
}
 
