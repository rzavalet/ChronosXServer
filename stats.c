#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"
#include "stats.h"

#define CHRONOS_TXN_TYPES     5

int print_samples = 0;
int *rtus = NULL;
long int *response_times_array = NULL;
long int rtusum = 0;
long int sample_response;
long int max_response_time = 0;
long int min_response_time = 0;
struct timeval xact_start;
struct timeval xact_end;

typedef enum chronosTransactionResult_t {
  CHRONOS_XACT_SUCCESS = 0,
  CHRONOS_XACT_FAILED,
  CHRONOS_XACT_ABORTED,
  CHRONOS_XACT_BLOCKED,
  CHRONOS_XACT_TIMELY,
  CHRONOS_XACT_RESULT_TYPES
} chronosTransactionResult_t;

typedef struct chronosServerThreadStats_t
{
  long long      xacts_executed[CHRONOS_TXN_TYPES][CHRONOS_XACT_RESULT_TYPES];
  long long      xacts_duration[CHRONOS_TXN_TYPES];
  long long      xacts_max_time[CHRONOS_TXN_TYPES];
} chronosServerThreadStats_t;

typedef struct chronosServerStats_t 
{
  int            sample_num;
  long long      xacts_history[60 / SAMPLES_PER_MINUTE];
  long long      xacts_duration[60 / SAMPLES_PER_MINUTE];
  long long      xacts_tpm[60 / SAMPLES_PER_MINUTE];
  long long      xacts_timely[60 / SAMPLES_PER_MINUTE];
  //long long      xacts_duration[CHRONOS_TXN_TYPES][60 / SAMPLES_PER_MINUTE];
  //long long      xacts_tpm[CHRONOS_TXN_TYPES][60 / SAMPLES_PER_MINUTE];
} chronosServerStats_t;



/*------------------------------------------------
 *      API for time arithmetic
 *-----------------------------------------------*/
/* Time difference in milliseconds */
long int 
diff_time(const struct timeval *start,
          const struct timeval *end)
{
  return ( (end->tv_sec*1000 + end->tv_usec/1000) -
           (start->tv_sec*1000 + start->tv_usec/1000) );
}

/* Time difference in microseconds */
long int 
udiff_time(const struct timeval *start,
           const struct timeval *end)
{
  return ( (  end->tv_sec*1000000 + end->tv_usec) -
           (start->tv_sec*1000000 + start->tv_usec) );
}
 

/*------------------------------------------------
 *      API for Thread Stats
 *----------------------------------------------*/
chronosServerThreadStats_t *
chronosServerThreadStatsAlloc(int num_threads)
{
  return calloc(num_threads, sizeof(chronosServerThreadStats_t));
}

void
update_thread_stats(const struct timeval        *start,
                    const struct timeval        *end,
                    long long                    desired_delay_ms,
                    chronosUserTransaction_t     txn_type,
                    int                          txn_rc,
                    int                          thread_num,
                    chronosServerThreadStats_t  *threadStatsArr)
{
  long long         txn_duration_ms;
  chronosServerThreadStats_t  *threadStatsP = &threadStatsArr[thread_num];

  txn_duration_ms = diff_time(start, end);

  if (txn_duration_ms > threadStatsP->xacts_max_time[txn_type]) {
    threadStatsP->xacts_max_time[txn_type] = txn_duration_ms;
  }

  threadStatsP->xacts_duration[txn_type] += txn_duration_ms;

  if (txn_rc == CHRONOS_SERVER_SUCCESS) {

    if (txn_duration_ms < desired_delay_ms) {
      threadStatsP->xacts_executed[txn_type][CHRONOS_XACT_TIMELY] ++;
    }

    threadStatsP->xacts_executed[txn_type][CHRONOS_XACT_SUCCESS] ++;
  }
  else {
    threadStatsP->xacts_executed[txn_type][CHRONOS_XACT_FAILED] ++;
  }

}


/*------------------------------------------------
 *      API for Server Stats
 *----------------------------------------------*/
chronosServerStats_t *
chronosServerStatsAlloc()
{
  return calloc(1, sizeof(chronosServerStats_t));
}


long long
last_xacts_duration_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->xacts_duration[sample_num];
}

long long
last_xacts_history_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->xacts_history[sample_num];
}

/*-------------------------------------------------
 * Take the per-thread-stats and aggregate them
 * into the global stats.
 *-----------------------------------------------*/
void
aggregate_thread_stats(int                          num_threads,
                       chronosServerThreadStats_t  *threadStatsArr,
                       chronosServerStats_t        *serverStatsP,
                       FILE                        *stats_fp)
{
  int i, j;
  int sample_num = 0;
  long long total_xacts = 0;
  long long total_duration = 0;
  long long max_duration = 0;
  long long old_count = 0;
  long long old_timely = 0;

  long long timely_xacts = 0;
  long long user_timely_xacts = 0;
  long long total_update_xacts = 0;
  long long total_update_duration = 0;

  for (j=0; j<num_threads; j++) {

    for (i=0; i<CHRONOS_TXN_TYPES-1; i++) {
      total_xacts += threadStatsArr[j].xacts_executed[i][CHRONOS_XACT_SUCCESS]
                      + threadStatsArr[j].xacts_executed[i][CHRONOS_XACT_FAILED];

      user_timely_xacts += threadStatsArr[j].xacts_executed[i][CHRONOS_XACT_TIMELY];

      total_duration += threadStatsArr[j].xacts_duration[i];

      if (threadStatsArr[j].xacts_max_time[i] > max_duration) {
        max_duration = threadStatsArr[j].xacts_max_time[i];
      }
    }


    /* Stats for the update transactions */
    timely_xacts += threadStatsArr[j].xacts_executed[CHRONOS_SYS_TXN_UPDATE_STOCK][CHRONOS_XACT_TIMELY];
    total_update_xacts += threadStatsArr[j].xacts_executed[CHRONOS_SYS_TXN_UPDATE_STOCK][CHRONOS_XACT_SUCCESS]
                          + threadStatsArr[j].xacts_executed[CHRONOS_SYS_TXN_UPDATE_STOCK][CHRONOS_XACT_FAILED];
    total_update_duration += threadStatsArr[j].xacts_duration[CHRONOS_SYS_TXN_UPDATE_STOCK];
  }

  
  serverStatsP->sample_num = (serverStatsP->sample_num + 1) % (60 / SAMPLES_PER_MINUTE);
  sample_num = serverStatsP->sample_num;

  old_count = serverStatsP->xacts_history[sample_num];
  old_timely = serverStatsP->xacts_timely[sample_num];

  serverStatsP->xacts_history[sample_num] = total_xacts;
  serverStatsP->xacts_duration[sample_num] = total_duration;

  serverStatsP->xacts_tpm[sample_num] = total_xacts - old_count;
  //serverStatsP->xacts_timely[sample_num] = timely_xacts - old_timely;
  serverStatsP->xacts_timely[sample_num] = user_timely_xacts - old_timely;

  if (stats_fp != NULL) {
    fprintf(stats_fp, 
            "%lld, %lld, %lld, %lld, %lld, %lld\n",
            serverStatsP->xacts_history[sample_num],
            serverStatsP->xacts_duration[sample_num],
            serverStatsP->xacts_tpm[sample_num],
            serverStatsP->xacts_timely[sample_num],
            total_update_xacts, total_update_duration);
    fflush(stats_fp);
  }

  return;
}



/*------------------------------------------------
 *      API for other type of stats
 *----------------------------------------------*/
int 
init_stats_struct()
{
  rtus = calloc(RTBINS + 1, sizeof(int));
  response_times_array = calloc(NUM_SAMPLES, sizeof(long int));

  return 0;
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

