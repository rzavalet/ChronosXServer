#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "common.h"
#include "server_config.h"
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
  long long      deadline;

  long long      user_xacts_history[CHRONOS_SAMPLING_SLOTS];
  long long      user_xacts_timely[CHRONOS_SAMPLING_SLOTS];
  long long      user_xacts_duration[CHRONOS_SAMPLING_SLOTS];
  long long      user_xacts_slack[CHRONOS_SAMPLING_SLOTS];
  long long      user_xacts_delay[CHRONOS_SAMPLING_SLOTS];
  long long      user_xacts_tpm[CHRONOS_SAMPLING_SLOTS];
  long long      user_xacts_ttpm[CHRONOS_SAMPLING_SLOTS];

  long long      refresh_xacts_history[CHRONOS_SAMPLING_SLOTS];
  long long      refresh_xacts_duration[CHRONOS_SAMPLING_SLOTS];
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
chronosServerThreadStatsFree(chronosServerThreadStats_t *threadStatsArr)
{
  if (threadStatsArr != NULL) {
    free(threadStatsArr);
  }
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
chronosServerStatsAlloc(long long deadline)
{
  chronosServerStats_t *serverStatsP = NULL;

  serverStatsP = calloc(1, sizeof(chronosServerStats_t));
  if (serverStatsP == NULL) {
    goto failXit;
  }

  serverStatsP->deadline = deadline;

  goto cleanup;

failXit:
  serverStatsP = NULL;

cleanup:
  return serverStatsP;
}

void
chronosServerStatsFree(chronosServerStats_t * serverStatsP)
{
  if (serverStatsP != NULL) {
    memset(serverStatsP, 0, sizeof(*serverStatsP));
    free(serverStatsP);
  }
}

long long
last_user_xacts_duration_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->user_xacts_duration[sample_num];
}

long long
last_user_xacts_delay_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->user_xacts_delay[sample_num];
}

long long
last_user_xacts_slack_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->user_xacts_slack[sample_num];
}

long long
last_user_xacts_history_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->user_xacts_history[sample_num];
}

long long
last_user_xacts_tpm_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->user_xacts_tpm[sample_num];
}

long long
last_user_xacts_ttpm_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->user_xacts_ttpm[sample_num];
}

long long
total_refresh_xacts_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->refresh_xacts_history[sample_num];
}

long long
total_refresh_xacts_duration_get(chronosServerStats_t  *serverStatsP)
{
  int sample_num = serverStatsP->sample_num;
  return serverStatsP->refresh_xacts_duration[sample_num];
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
  long long old_count = 0;
  long long old_timely = 0;

  long long total_user_xacts = 0;
  long long total_duration_user_xacts = 0;
  long long total_delay_user_xacts = 0;
  long long total_slack_user_xacts = 0;
  long long timely_user_xacts = 0;
  long long max_duration_user_xacts = 0;

  long long timely_refresh_xacts = 0;
  long long total_refresh_xacts = 0;
  long long total_duration_refresh_xacts = 0;

  for (j=0; j<num_threads; j++) {

    /* User xacts */
    for (i=0; i<CHRONOS_TXN_TYPES-1; i++) {
      timely_user_xacts += threadStatsArr[j].xacts_executed[i][CHRONOS_XACT_TIMELY];

      total_user_xacts += threadStatsArr[j].xacts_executed[i][CHRONOS_XACT_SUCCESS]
                      + threadStatsArr[j].xacts_executed[i][CHRONOS_XACT_FAILED];

      total_duration_user_xacts += threadStatsArr[j].xacts_duration[i];

      if (threadStatsArr[j].xacts_duration[i] > serverStatsP->deadline) {
        total_delay_user_xacts += threadStatsArr[j].xacts_duration[i] - serverStatsP->deadline;
      }
      else {
        total_slack_user_xacts += serverStatsP->deadline - threadStatsArr[j].xacts_duration[i];
      }

      if (threadStatsArr[j].xacts_max_time[i] > max_duration_user_xacts) {
        max_duration_user_xacts = threadStatsArr[j].xacts_max_time[i];
      }
    }


    /* Stats for the refresh xacts */
    timely_refresh_xacts += threadStatsArr[j].xacts_executed[CHRONOS_SYS_TXN_UPDATE_STOCK][CHRONOS_XACT_TIMELY];

    total_refresh_xacts += threadStatsArr[j].xacts_executed[CHRONOS_SYS_TXN_UPDATE_STOCK][CHRONOS_XACT_SUCCESS]
                          + threadStatsArr[j].xacts_executed[CHRONOS_SYS_TXN_UPDATE_STOCK][CHRONOS_XACT_FAILED];

    total_duration_refresh_xacts += threadStatsArr[j].xacts_duration[CHRONOS_SYS_TXN_UPDATE_STOCK];
  }

  
  serverStatsP->sample_num = (serverStatsP->sample_num + 1) % CHRONOS_SAMPLING_SLOTS;
  sample_num = serverStatsP->sample_num;

  old_count = serverStatsP->user_xacts_history[sample_num];
  old_timely = serverStatsP->user_xacts_timely[sample_num];

  /* These are the cumulative xacts */
  serverStatsP->user_xacts_history[sample_num] = total_user_xacts;
  serverStatsP->user_xacts_timely[sample_num] = timely_user_xacts;
  serverStatsP->user_xacts_duration[sample_num] = total_duration_user_xacts;
  serverStatsP->user_xacts_delay[sample_num] = total_delay_user_xacts;
  serverStatsP->user_xacts_slack[sample_num] = total_slack_user_xacts;

  serverStatsP->refresh_xacts_history[sample_num] = total_refresh_xacts;
  serverStatsP->refresh_xacts_duration[sample_num] = total_duration_refresh_xacts;

  /* These are the per minute xacts 
   * total_user_xacts: contains the cumulative number of total user
   *                   transactions (failed + successful) 
   * old_count: contains the cumulative number of total user transactions
   *            (failed + successful) up to one minute ago.
   * 
   * So, total_user_xacts - old_count is the number of transactions executed
   * in the last minute.
   */
  serverStatsP->user_xacts_tpm[sample_num] = total_user_xacts - old_count;
  serverStatsP->user_xacts_ttpm[sample_num] = timely_user_xacts - old_timely;

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

