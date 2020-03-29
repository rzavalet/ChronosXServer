#ifndef STATS_H_
#define STATS_H_

#include <chronos_transactions.h>
#include "server_config.h"

/*------------------------------------------------
 *      API for time arithmetic
 *-----------------------------------------------*/
#define getTime(x) gettimeofday( (x), NULL)

long int 
diff_time(const struct timeval *start,
          const struct timeval *end);

long int 
udiff_time(const struct timeval *start,
           const struct timeval *end);




/*------------------------------------------------
 *      API for Thread Stats
 *----------------------------------------------*/
typedef struct chronosServerThreadStats_t chronosServerThreadStats_t;

chronosServerThreadStats_t *
chronosServerThreadStatsAlloc(int num_threads);

void
update_thread_stats(const struct timeval        *start,
                    const struct timeval        *end,
                    long long                    desired_delay_ms,
                    chronosUserTransaction_t     txn_type,
                    int                          txn_rc,
                    int                          thread_num,
                    chronosServerThreadStats_t  *threadStatsArr);

/*------------------------------------------------
 *      API for Server Stats
 *----------------------------------------------*/
typedef struct chronosServerStats_t chronosServerStats_t;

chronosServerStats_t *
chronosServerStatsAlloc();

void
aggregate_thread_stats(int                          num_threads,
                       chronosServerThreadStats_t  *threadStatsP,
                       chronosServerStats_t        *serverStatsP,
                       FILE                        *stats_fp);

long long
last_xacts_duration_get(chronosServerStats_t  *serverStatsP);

long long
last_xacts_history_get(chronosServerStats_t  *serverStatsP);


/*------------------------------------------------
 *      API for other type of stats
 *----------------------------------------------*/
/* Track response times */
/* Response time -- this can use a lot of memory */
#define RTINCR 1000            /* 10 micro second increment */
#define RTBINS 10000000/RTINCR /* The bins cover the range [0,10000000] us */
#define NUM_SAMPLES   (1000)

extern int *rtus;                       /* response times sample bins */
extern long int *response_times_array;  /* response times table */
extern long int rtusum;                 /* total sum of response times */
extern long int sample_response;        /* response time of one iteration */
extern int print_samples;
extern long int max_response_time;
extern long int min_response_time;
extern struct timeval xact_start;       /* sys time at the start of the iteration */
extern struct timeval xact_end;         /* sys time at the end of the iteration */

#define RTCAPTURE_START() \
do { \
 getTime(&xact_start);\
} while (0)

#define RTCAPTURE_END() \
do { \
 getTime(&xact_end);\
 sample_response = udiff_time(&xact_start,&xact_end);\
 if (sample_response<RTBINS*RTINCR){\
    rtus[(int)(sample_response)/RTINCR]++;\
 }else{ \
    rtus[RTBINS]++;\
 }\
 rtusum += sample_response;\
 if (sample_response > max_response_time) \
   max_response_time = sample_response; \
 if (sample_response < min_response_time) \
   min_response_time = sample_response; \
 response_times_array[current_rep%NUM_SAMPLES] = sample_response; \
} while (0);

#define RTCAPTURE() \
do { \
 getTime(&xact_end);\
 sample_response = udiff_time(&xact_start,&xact_end);\
 if (sample_response<RTBINS*RTINCR){\
    rtus[(int)(sample_response)/RTINCR]++;\
 }else{ \
    rtus[RTBINS]++;\
 }\
 rtusum += sample_response;\
 if (sample_response > max_response_time) \
   max_response_time = sample_response; \
 if (sample_response < min_response_time) \
   min_response_time = sample_response; \
 response_times_array[current_rep%NUM_SAMPLES] = sample_response; \
 getTime(&xact_start);\
} while (0);
 
#define RTCAPTURE_PRINT() \
do {\
  int _ijk; \
  if (print_samples) { \
    fprintf(stderr, "### EXECUTION STATS\n"); \
    fprintf(stderr, "us,count\n"); \
    fprintf(stderr, "--------------------\n"); \
    for (_ijk=0; _ijk < RTBINS + 1; _ijk++) { \
      fprintf(stderr, "%d,%d\n", _ijk * RTINCR, rtus[_ijk]); \
    } \
  } \
  fprintf(stderr, "--------------------\n"); \
  fprintf(stderr, "total: %ld\n", rtusum); \
  fprintf(stderr, "max_response_time: %ld\n", max_response_time); \
  fprintf(stderr, "min_response_time: %ld\n", min_response_time); \
  print_stats_to_csv(); \
} while(0)

int 
print_stats_to_csv();

int 
init_stats_struct();

#endif /* STATS_H_ */
