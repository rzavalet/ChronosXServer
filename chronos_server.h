#ifndef _CHRONOS_SERVER_H_
#define _CHRONOS_SERVER_H_

#include <signal.h>
#include <time.h>
#include <pthread.h>

#include <benchmark.h>

#include <chronos_transactions.h>
#include <chronos_packets.h>

#include "server_config.h"
#include "aup.h"
#include "stats.h"
#include "chronos_queue.h"
#include "chronos_ac.h"



typedef enum chronosServerThreadState_t {
  CHRONOS_SERVER_THREAD_STATE_MIN = 0,
  CHRONOS_SERVER_THREAD_STATE_RUN = CHRONOS_SERVER_THREAD_STATE_MIN,
  CHRONOS_SERVER_THREAD_STATE_DIE,
  CHRONOS_SERVER_THREAD_STATE_MAX,
  CHRONOS_SERVER_THREAD_STATE_INVAL=CHRONOS_SERVER_THREAD_STATE_MAX
} chronosServerThreadState_t;

typedef enum chronosServerThreadType_t {
  CHRONOS_SERVER_THREAD_MIN = 0,
  CHRONOS_SERVER_THREAD_LISTENER = CHRONOS_SERVER_THREAD_MIN,
  CHRONOS_SERVER_THREAD_UPDATE,
  CHRONOS_SERVER_THREAD_PROCESSING,
  CHRONOS_SERVER_THREAD_SAMPLING,
  CHRONOS_SERVER_THREAD_MAX,
  CHRONOS_SERVER_THREAD_INVAL=CHRONOS_SERVER_THREAD_MAX
} chronosServerThreadType_t;

typedef struct chronosServerContext_t 
{
  int magic;

  int runningMode;

  /* For xact evaluation */
  chronosUserTransaction_t  evaluated_txn;
  unsigned int txn_size;

  int debugLevel;

  int serverPort;

  /* Whether an initial load is required or not */
  int initialLoad;

  
  /* The duration of one chronos experiment */
  long duration_sec;

  
  /* We can only create a limited number of 
   * client threads. */
  int numClientsThreads;

  /* Apart from client threads, we also have
   * update threads - threads that update the
   * data in the system periodically */
  int numUpdateThreads;
  int numUpdatesPerUpdateThread;

  /* This is the number of server threads that
   * dequeue transactions from the queue 
   * and process them */
  int numServerThreads;

  /* These two variables are used to wait till 
   * all client threads are initialized, so that
   * we can have a fair experiment*/
  pthread_mutex_t startThreadsMutex;
  pthread_cond_t startThreadsWait;

  /* Each data item is associated with a validity interval,
   * this is the initial value. */
  long long initialValidityIntervalMS;

  /* This is the "deadline" for the user txns */
  long long desiredDelayBoundMS;

  /* These are the queues holding requested txns */
  chronos_queue_t *userTxnQueue;
  chronos_queue_t *sysTxnQueue;

  /*============ These fields control the sampling task ==========*/
  FILE                        *stats_fp;
  chronosServerStats_t        *performanceStatsP;
  chronosServerThreadStats_t  *threadStatsArr;

  double                average_service_delay_ms;
  double                degree_timing_violation;
  double                smoth_degree_timing_violation;
  double                alpha;

  chronos_ac_env_t     *ac_env; 
  chronos_aup_env_h    *aup_env;
  /*==============================================================*/


  /* These fields are for the benchmark framework */
  void *benchmarkCtxtP;
  
} chronosServerContext_t;

typedef struct chronosServerThreadInfo_t {
  int       magic;
  pthread_t thread_id;
  int       thread_num;
  int       socket_fd;
  chronosServerThreadState_t state;
  chronosServerThreadType_t thread_type;
  chronosServerContext_t *contextP;
} chronosServerThreadInfo_t;

#endif
