#ifndef STATS_H_
#define STATS_H_

/* Track response times */
#define getTime(x) gettimeofday( (x), NULL)
#define milliSleep(t,x) \
        ((x).tv_sec=0, (x).tv_usec=(t)*1000, select(0,NULL,NULL,NULL,&(x)))

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

long int 
diff_time(struct timeval * start,struct timeval *end);

long int 
udiff_time(struct timeval * start,struct timeval *end);

#endif /* STATS_H_ */
