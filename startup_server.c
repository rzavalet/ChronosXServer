#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sched.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>

#ifdef LITMUS_RT
#include <litmus.h>
#endif

#include <chronos_packets.h>
#include <chronos_transactions.h>
#include <chronos_environment.h>
#include <benchmark.h>

#include "server_config.h"
#include "common.h"
#include "chronos_server.h"
#include "chronos_queue.h"
#include "performance_monitor.h"
#include "stats.h"
#include "thpool.h"

#define xstr(s) str(s)
#define str(s) #s

#define CHRONOS_SERVER_CTX_MAGIC      (0xBACA)
#define CHRONOS_SERVER_THREAD_MAGIC   (0xCACA)

#define CHRONOS_SERVER_CTX_CHECK(_ctxt)    assert((_ctxt)->magic == CHRONOS_SERVER_CTX_MAGIC)
#define CHRONOS_SERVER_THREAD_CHECK(_thr)  assert((_thr)->magic == CHRONOS_SERVER_THREAD_MAGIC)

#define CHRONOS_SERVER_THREAD_NAME(_txn_type) \
  ((CHRONOS_SERVER_THREAD_MIN<=(_txn_type) && (_txn_type) < CHRONOS_SERVER_THREAD_MAX) ? (chronosServerThreadNames[(_txn_type)]) : "INVALID")

/* These are the available server modes */
#define CHRONOS_SERVER_MODE_BASE                (0)
#define CHRONOS_SERVER_MODE_AC                  (1)
#define CHRONOS_SERVER_MODE_AUP                 (2)
#define CHRONOS_SERVER_MODE_AC_AUP              (3)

#define CHRONOS_SERVER_MODE_NAME_BASE           "base"
#define CHRONOS_SERVER_MODE_NAME_AC             "ac"
#define CHRONOS_SERVER_MODE_NAME_AUP            "aup"
#define CHRONOS_SERVER_MODE_NAME_AC_AUP         "ac_aup"

#define IS_CHRONOS_MODE_BASE(_ctxt)   ((_ctxt)->runningMode == CHRONOS_SERVER_MODE_BASE)
#define IS_CHRONOS_MODE_AC(_ctxt)     ((_ctxt)->runningMode == CHRONOS_SERVER_MODE_AC)
#define IS_CHRONOS_MODE_AUP(_ctxt)    ((_ctxt)->runningMode == CHRONOS_SERVER_MODE_AUP)
#define IS_CHRONOS_MODE_FULL(_ctxt)   ((_ctxt)->runningMode == CHRONOS_SERVER_MODE_AC_AUP)

/* These are the different running modes */
#define CHRONOS_SERVER_XACT_EVALUATION_MODE     (4)
#define CHRONOS_SERVER_XACT_NAME_VIEW_STOCK     "view_stock"
#define CHRONOS_SERVER_XACT_NAME_VIEW_PORTFOLIO "view_portfolio"
#define CHRONOS_SERVER_XACT_NAME_PURCHASE       "purchase_stock"
#define CHRONOS_SERVER_XACT_NAME_SELL           "sell_stock"
#define CHRONOS_SERVER_XACT_NAME_REFRESH_STOCK  "refresh_stock"

static const char *program_name = "startup_server";

#if 1
int benchmark_debug_level = CHRONOS_DEBUG_LEVEL_MIN;
int server_debug_level = CHRONOS_DEBUG_LEVEL_MIN;
extern int chronos_debug_level;
#else
int benchmark_debug_level = CHRONOS_DEBUG_LEVEL_MAX;
int server_debug_level = CHRONOS_DEBUG_LEVEL_MAX;
extern int chronos_debug_level;
#endif

const char *chronosServerThreadNames[] ={
  "CHRONOS_SERVER_THREAD_LISTENER",
  "CHRONOS_SERVER_THREAD_UPDATE"
};

/*---------------------------------------------------------------
 *              STATIC FUNCTIONS 
 *-------------------------------------------------------------*/
static int
processArguments(int                     argc, 
                 char                   *argv[], 
                 chronosServerContext_t *contextP);

static void
chronos_usage();

static void *
daListener(void *argP);

static void
daHandler(void *argP);

static void *
updateThread(void *argP);

#if 0
static void *
samplingThread(void *argP);
#endif

static void *
processThread(void *argP);

static int
dispatchTableFn (chronosRequestPacket_t    *reqPacketP, 
                 int                       *txn_rc, 
                 chronosServerThreadInfo_t *infoP);

static int
waitPeriod(double updatePeriodMS);

static int 
runTxnEvaluation(chronosServerContext_t *serverContextP);

static inline double
in_period_ds(chronosServerContext_t *contextP);

static void
aggregateStats(chronosServerContext_t *contextP);

static int
performanceMonitor(chronosServerContext_t *contextP);

static void
qosManager(chronosServerContext_t *contextP);

static int
adaptive_update_policy(chronosServerContext_t *contextP);

static void
admission_control(chronosServerContext_t *contextP);

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
static int
processUserTransaction(volatile int *txn_rc,
                       chronosRequestPacket_t    *reqPacketP,
                       struct timeval             enqueued_time,
                       chronosServerThreadInfo_t *infoP);
#endif


/*---------------------------------------------------------------
 *                  GLOBAL VARIABLES
 *-------------------------------------------------------------*/
volatile int              sample_num = -1;
volatile int              num_started_server_threads = 0;
volatile int              warming_up = 1;
unsigned long             warmup_duration_ms = CHRONOS_SEC_TO_MS(CHRONOS_WARMUP_DURATION_SEC);
volatile int              time_to_die = 0;
chronosServerContext_t    *serverContextP = NULL;
chronosServerThreadInfo_t *samplingThreadInfoP = NULL;
chronosServerThreadInfo_t *listenerThreadInfoP = NULL;
chronosServerThreadInfo_t *processingThreadInfoArrP = NULL;
chronosServerThreadInfo_t *updateThreadInfoArrP = NULL;


static int 
isTimeToDie()
{
  return time_to_die;
}

static int
stats_open_file(chronosServerContext_t *contextP) 
{
  int    rc = CHRONOS_SERVER_SUCCESS;
  FILE  *stats_fp = NULL;

  if (contextP == NULL) {
    goto failXit;
  }

  stats_fp = fopen("/tmp/stats.dat", "w");
  if (stats_fp == NULL) {
    server_error("Failed to open stats file");
    goto failXit;
  }

  contextP->stats_fp = stats_fp;

  fprintf(contextP->stats_fp, 
          "count,duration,tpm,ttpm,ttpm_rate,update_xacts,update_duration,average_service_delay_ms,degree_timing_violation,smoth_degree_timing_violation,p_ext,sys_queue_size,user_queue_size,inter_xact_sleep_ms\n");

  goto cleanup;

failXit:
  contextP->stats_fp = NULL;
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int
stats_close_file(chronosServerContext_t *contextP) 
{
  int    rc = CHRONOS_SERVER_SUCCESS;
  
  if (contextP == NULL || contextP->stats_fp == NULL) {
    goto failXit;
  }

  fclose(contextP->stats_fp);
  contextP->stats_fp = NULL;

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int
stats_print(chronosServerContext_t *contextP) 
{
  int    rc = CHRONOS_SERVER_SUCCESS;
  long long last_user_xacts_history;
  long long last_user_xacts_duration;
  long long last_user_xacts_delay;
  long long last_user_xacts_slack;
  long long last_user_xacts_tpm;
  long long last_user_xacts_ttpm;
  float last_user_xacts_ttpm_rate;
  float last_user_avg_slack;
  float last_user_avg_delay;
  long long last_refresh_xacts_history;
  long long last_refresh_xacts_duration;
  float     p_ext;
  
  if (contextP == NULL || contextP->stats_fp == NULL) {
    goto failXit;
  }

  last_user_xacts_history = last_user_xacts_history_get(contextP->performanceStatsP);
  last_user_xacts_duration = last_user_xacts_duration_get(contextP->performanceStatsP);
  last_user_xacts_tpm = last_user_xacts_tpm_get(contextP->performanceStatsP);
  last_user_xacts_ttpm = last_user_xacts_ttpm_get(contextP->performanceStatsP);

  if (last_user_xacts_history > 0) {
    last_user_avg_slack = (float)last_user_xacts_slack / (float)last_user_xacts_history;
    last_user_avg_delay = (float)last_user_xacts_delay / (float)last_user_xacts_history;
  }
  else {
    last_user_avg_slack = 0;
    last_user_avg_delay = 0;
  }

  if (last_user_xacts_tpm > 0) {
    last_user_xacts_ttpm_rate = 100.00 * (float)last_user_xacts_ttpm / (float)last_user_xacts_tpm;
  }
  else {
    last_user_xacts_ttpm_rate = 0;
  }

  last_refresh_xacts_history = total_refresh_xacts_get(contextP->performanceStatsP);
  last_refresh_xacts_duration = total_refresh_xacts_duration_get(contextP->performanceStatsP);
  p_ext = chronos_aup_pext_get(contextP->aup_env);

  if (contextP->stats_fp != NULL) {
    fprintf(contextP->stats_fp, 
            "%lld, %lld, %lld, %lld, %.2f, %lld, %lld, %.2lf, %.2lf, %.2lf, %.2f, %d, %d, %lld\n",
            last_user_xacts_history,
            last_user_xacts_duration,
            last_user_xacts_tpm,
            last_user_xacts_ttpm,
            last_user_xacts_ttpm_rate,
            last_refresh_xacts_history, 
            last_refresh_xacts_duration,
            contextP->average_service_delay_ms,
            contextP->degree_timing_violation,
            contextP->smoth_degree_timing_violation,
            p_ext,
            chronos_queue_size(contextP->sysTxnQueue),
            chronos_queue_size(contextP->userTxnQueue),
            contextP->inter_xact_sleep_ms);

    fflush(contextP->stats_fp);
  }
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}


/* This is the starting point for the Chronos Prototype. 
 * It has to perform the following tasks:
 *
 * 1) Create the tables in the system
 * 2) Populate the tables in the system
 * 3) Select the configuration of the workload.
 *    According to the original paper, these are the knobs of the system
 *    - # of client threads. In the original paper, they generated 300-900
 *                          client threads per machine -- they had 2 machines.
 *    - validity interval of temporal data
 *    - # of update threads
 *    - update period
 *    - thinking time for client threads
 *
 *    Additionally, in the original paper, there were several variables
 *    used in the experiments. For example:
 *
 *    - Running mode, which could be:
 *      . Base
 *      . Admission control
 *      . Adaptive updates
 *
 *  4) Spawn the update threads and start refreshing the data according to
 *     the update period. These threads will die when the run finishes
 *
 *     The validity interval in the original paper was 1s.
 *     The update period in the original paper was 0.5s
 *
 *  5) Spawn the client threads. Client threads will randomly pick a type of 
 *     workload:
 *      - 60% of client requests are View-Stock
 *      - 40% are uniformly selected among the other three types of user
 *        transactions.
 *
 *     The number of data accesses varies from 50 to 100.
 *     The think time in the paper is uniformly distributed in [0.3s, 0.5s]
 */ 
int main(int argc, char *argv[]) 
{
  int rc = CHRONOS_SERVER_SUCCESS;
  int i;

  const int        stack_size = 0x100000; // 1 MB
  int             *thread_rc = NULL;
  pthread_attr_t   attr;

  char           **pkeys_list = NULL;
  int              num_pkeys = 0;

  chronos_debug_level = CHRONOS_DEBUG_LEVEL_MIN;


  rc = init_stats_struct();

  serverContextP = malloc(sizeof(chronosServerContext_t));
  if (serverContextP == NULL) {
    server_error("Failed to allocate server context");
    goto failXit;
  }
  
  memset(serverContextP, 0, sizeof(chronosServerContext_t));

  /*----------------------------------------------------
   * Process command line arguments which include:
   *
   *    - # of client threads it can accept.
   *    - validity interval of temporal data
   *    - # of update threads
   *    - update period
   *
   *--------------------------------------------------*/
  if (processArguments(argc, argv, serverContextP) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to process arguments");
    goto failXit;
  }


  /*----------------------------------------------------
   * Run the transaction evaluation mode.
   *--------------------------------------------------*/
  if (serverContextP->runningMode == CHRONOS_SERVER_XACT_EVALUATION_MODE) {
    server_info("Running in transaction evaluation mode");
    rc = runTxnEvaluation(serverContextP);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to run transaction evaluation mode");
      goto failXit;    
    }

    goto cleanup;
  }


  /*----------------------------------------------------
   * Run the soft real-time mode
   *--------------------------------------------------*/
  serverContextP->magic = CHRONOS_SERVER_CTX_MAGIC;
  CHRONOS_SERVER_CTX_CHECK(serverContextP);
  
  serverContextP->userTxnQueue = chronos_queue_alloc(isTimeToDie);
  if (serverContextP->userTxnQueue == NULL) {
    server_error("Failed to create user queue");
    goto failXit;
  }

  serverContextP->sysTxnQueue = chronos_queue_alloc(isTimeToDie);
  if (serverContextP->sysTxnQueue == NULL) {
    server_error("Failed to create system queue");
    goto failXit;
  }

  serverContextP->ac_env = chronos_ac_env_alloc(isTimeToDie);
  if (serverContextP->ac_env == NULL) {
    server_error("Failed to admission control environment");
    goto failXit;
  }

  if (pthread_mutex_init(&serverContextP->startThreadsMutex, NULL) != 0) {
    server_error("Failed to init mutex");
    goto failXit;
  }

  if (pthread_cond_init(&serverContextP->startThreadsWait, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }


  chronosServerThreadStats_t *threadStatsArr = chronosServerThreadStatsAlloc(serverContextP->numServerThreads);
  if (threadStatsArr == NULL) {
    server_error("Failed to allocate thread stats structure");
    goto failXit;
  }
  serverContextP->threadStatsArr = threadStatsArr;

  chronosServerStats_t *performanceStatsP = chronosServerStatsAlloc(serverContextP->desiredDelayBoundMS);
  if (performanceStatsP == NULL) {
    server_error("Failed to allocate server stats structure");
    goto failXit;
  }
  serverContextP->performanceStatsP = performanceStatsP;

  rc = stats_open_file(serverContextP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Could not open stats file");
    goto failXit;
  }

  if (serverContextP->initialLoad) {
    /* Create the system tables */
    serverContextP->benchmarkCtxtP = benchmark_initial_load(program_name, 
                                                            CHRONOS_SERVER_HOME_DIR, 
                                                            CHRONOS_SERVER_DATAFILES_DIR);
    if (serverContextP->benchmarkCtxtP == NULL) 
    {
      server_error("Failed to perform initial load");
      goto failXit;
    }
  }
  else {
    server_info("*** Skipping initial load");
    
    /* Obtain a benchmark handle */
    rc = benchmark_handle_alloc(&serverContextP->benchmarkCtxtP, 
                                0, 
                                program_name,
                                CHRONOS_SERVER_HOME_DIR, 
                                CHRONOS_SERVER_DATAFILES_DIR);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to allocate handle");
      goto failXit;
    }
  }
 
  rc = benchmark_portfolios_stats_get(serverContextP->benchmarkCtxtP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to get portfolios stat");
    goto failXit;
  }

  /* Get a reference to the list of stock names */
  rc = benchmark_stock_list_get(serverContextP->benchmarkCtxtP,
                                &pkeys_list,
                                &num_pkeys);
  if (rc != 0) {
    server_error("failed to get list of stocks");
    goto failXit;
  }
  server_info("num_pkeys: %d, pkeys_list: %p",
              num_pkeys, pkeys_list);


  /*----------------------------------------------------
   * Initialize the AUP environment
   *--------------------------------------------------*/
  serverContextP->aup_env = chronos_aup_env_alloc(num_pkeys  /* num_elements */, 
                                                  serverContextP->initialValidityIntervalMS,
                                                  CHRONOS_BETA  /* beta */);
  if (serverContextP->aup_env == NULL) {
    goto failXit;
  }


#ifdef LITMUS_RT
  /*----------------------------------------------------
   * Litmus RT initialization
   *--------------------------------------------------*/
  rc = init_litmus();
  if (rc != 0) {
    server_error("failed to init litmus environment");
    goto failXit;
  }
#endif

  /*=====================================================================
   * Spawn each of the required threads
   *===================================================================*/
  rc = pthread_attr_init(&attr);
  if (rc != 0) {
    server_error("failed to init thread attributes");
    goto failXit;
  }
  
  rc = pthread_attr_setstacksize(&attr, stack_size);
  if (rc != 0) {
    server_error("failed to set stack size");
    goto failXit;
  }

  /*------------------------------------------------------------------------
   * Spawn processing thread 
   *------------------------------------------------------------------------*/
  processingThreadInfoArrP = calloc(serverContextP->numServerThreads, sizeof(chronosServerThreadInfo_t));
  if (processingThreadInfoArrP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }

  for (i=0; i<serverContextP->numServerThreads; i++) {
    processingThreadInfoArrP[i].thread_type = CHRONOS_SERVER_THREAD_PROCESSING;
    processingThreadInfoArrP[i].contextP = serverContextP;
    processingThreadInfoArrP[i].thread_num = i;
    processingThreadInfoArrP[i].magic = CHRONOS_SERVER_THREAD_MAGIC;
    
    rc = pthread_create(&processingThreadInfoArrP[i].thread_id,
                        &attr,
                        &processThread,
                        &(processingThreadInfoArrP[i]));
    if (rc != 0) {
      server_error("failed to spawn thread: %s", strerror(rc));
      goto failXit;
    }

    server_debug(2,"Spawned processing thread");
 }

#ifdef CHRONOS_UPDATE_TRANSACTIONS_ENABLED
  /*------------------------------------------------------------------------
   * Spawn the update threads
   *------------------------------------------------------------------------*/
  updateThreadInfoArrP = calloc(serverContextP->numUpdateThreads, sizeof(chronosServerThreadInfo_t));
  if (updateThreadInfoArrP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }

  for (i=0; i<serverContextP->numUpdateThreads; i++) {
    /* Set the generic data */
    updateThreadInfoArrP[i].thread_type = CHRONOS_SERVER_THREAD_UPDATE;
    updateThreadInfoArrP[i].contextP = serverContextP;
    updateThreadInfoArrP[i].thread_num = i;
    updateThreadInfoArrP[i].magic = CHRONOS_SERVER_THREAD_MAGIC;

    server_debug(5,"Thread: %d, will handle from %d to %d",
                  updateThreadInfoArrP[i].thread_num, 
                  i * serverContextP->numUpdatesPerUpdateThread, 
                  (i+1) * serverContextP->numUpdatesPerUpdateThread - 1);

    rc = pthread_create(&updateThreadInfoArrP[i].thread_id,
                        &attr,
                        &updateThread,
                        &(updateThreadInfoArrP[i]));
    if (rc != 0) {
      server_error("failed to spawn thread: %s", strerror(rc));
      goto failXit;
    }

    server_debug(2,"Spawned update thread: %d", updateThreadInfoArrP[i].thread_num);
  }
#endif

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
  /*------------------------------------------------------------------------
   * Spawn daListener thread
   *------------------------------------------------------------------------*/
  listenerThreadInfoP = malloc(sizeof(chronosServerThreadInfo_t));
  if (listenerThreadInfoP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }
  
  memset(listenerThreadInfoP, 0, sizeof(chronosServerThreadInfo_t));
  listenerThreadInfoP->thread_type = CHRONOS_SERVER_THREAD_LISTENER;
  listenerThreadInfoP->contextP = serverContextP;
  listenerThreadInfoP->thread_num = 0;
  listenerThreadInfoP->magic = CHRONOS_SERVER_THREAD_MAGIC;
    
  rc = pthread_create(&listenerThreadInfoP->thread_id,
                      &attr,
                      &daListener,
                      listenerThreadInfoP);
  if (rc != 0) {
    server_error("failed to spawn thread: %s", strerror(rc));
    goto failXit;
  }

  server_debug(2,"Spawned listener thread");
#endif


#if 0
  /*------------------------------------------------------------------------
   * Spawn sampling thread
   *------------------------------------------------------------------------*/
  samplingThreadInfoP = malloc(sizeof(chronosServerThreadInfo_t));
  if (samplingThreadInfoP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }
  
  memset(samplingThreadInfoP, 0, sizeof(chronosServerThreadInfo_t));
  samplingThreadInfoP->thread_type = CHRONOS_SERVER_THREAD_SAMPLING;
  samplingThreadInfoP->contextP = serverContextP;
  samplingThreadInfoP->thread_num = 0;
  samplingThreadInfoP->magic = CHRONOS_SERVER_THREAD_MAGIC;
    
  rc = pthread_create(&samplingThreadInfoP->thread_id,
                      &attr,
                      &samplingThread,
                      samplingThreadInfoP);
  if (rc != 0) {
    server_error("failed to spawn thread: %s", strerror(rc));
    goto failXit;
  }

  server_debug(2,"Spawned sampling thread");
#endif

  /* ===================================================================
   *
   * At this point all required threads are up and running. They will continue
   * to run until they are requested to finish by the timer or by a user signal
   *
   * =================================================================== */



#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
  rc = pthread_join(listenerThreadInfoP->thread_id, (void **)&thread_rc);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed while joining thread %s", CHRONOS_SERVER_THREAD_NAME(listenerThreadInfoP->thread_type));
  }
#endif

#ifdef CHRONOS_UPDATE_TRANSACTIONS_ENABLED
  for (i=0; i<serverContextP->numUpdateThreads; i++) {
    rc = pthread_join(updateThreadInfoArrP[i].thread_id, (void **)&thread_rc);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed while joining thread %s", CHRONOS_SERVER_THREAD_NAME(updateThreadInfoArrP[i].thread_type));
    }
  }
#endif
  
  for (i=0; i<serverContextP->numServerThreads; i++) {
    rc = pthread_join(processingThreadInfoArrP[i].thread_id, (void **)&thread_rc);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed while joining thread %s", CHRONOS_SERVER_THREAD_NAME(processingThreadInfoArrP[i].thread_type));
    }
  }

  rc = CHRONOS_SERVER_SUCCESS;
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;
  
cleanup:

#ifdef LITMUS_RT
  exit_litmus();
#endif

  if (serverContextP) {
    chronos_queue_free(serverContextP->userTxnQueue);
    serverContextP->userTxnQueue = NULL;

    chronos_queue_free(serverContextP->sysTxnQueue);
    serverContextP->sysTxnQueue = NULL;

    chronos_ac_env_free(serverContextP->ac_env);

    pthread_cond_destroy(&serverContextP->startThreadsWait);
    pthread_mutex_destroy(&serverContextP->startThreadsMutex);

    chronosServerThreadStatsFree(serverContextP->threadStatsArr);
    threadStatsArr = NULL;

    chronosServerStatsFree(serverContextP->performanceStatsP);
    performanceStatsP = NULL;

    if (serverContextP->aup_env) {
      chronos_aup_env_free(serverContextP->aup_env);
      serverContextP->aup_env = NULL;
    }

    if (benchmark_handle_free(serverContextP->benchmarkCtxtP) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to free handle");
    }

    stats_close_file(serverContextP);

    free(serverContextP);
    serverContextP = NULL;
  }

  return rc;
}

static int
initProcessArguments(chronosServerContext_t *contextP)
{
  contextP->numServerThreads = CHRONOS_NUM_SERVER_THREADS;
  contextP->numClientsThreads = CHRONOS_NUM_CLIENT_THREADS;
  contextP->numUpdateThreads = CHRONOS_NUM_UPDATE_THREADS;
  contextP->numUpdatesPerUpdateThread = CHRONOS_NUM_STOCK_UPDATES_PER_UPDATE_THREAD;
  contextP->serverPort = CHRONOS_SERVER_PORT;
  contextP->initialValidityIntervalMS = CHRONOS_INITIAL_VALIDITY_INTERVAL_MS;
  contextP->duration_sec = CHRONOS_EXPERIMENT_DURATION_SEC;
  contextP->desiredDelayBoundMS = CHRONOS_DESIRED_DELAY_BOUND_MS;
  contextP->alpha = CHRONOS_ALPHA;
  contextP->initialLoad = 0;

  contextP->debugLevel = CHRONOS_DEBUG_LEVEL_MIN;

  contextP->evaluated_txn = CHRONOS_USER_TXN_VIEW_STOCK;
  contextP->txn_size = CHRONOS_MIN_DATA_ITEMS_PER_XACT;

  contextP->runningMode = CHRONOS_SERVER_MODE_BASE;

  return 0;
}


/*
 * Process the command line arguments
 */
static int
processArguments(int argc, char *argv[], chronosServerContext_t *contextP) 
{
  int c;
  int option_index = 0;

  static struct option long_options[] = {
                   {"evaluate-wcet",          no_argument,       0,  0  },
                   {"xact-name",              required_argument, 0,  0  },
                   {"print-samples",          no_argument      , &print_samples,  1 },
                   {"initial-load",           no_argument      , 0,  0  },
                   {"help",                   no_argument      , 0,  'h'},
                   {"xact-size",              required_argument, 0,  0  },
                   {"server-mode",            required_argument, 0,  'm'},
                   {"update-threads",         required_argument, 0,  'u'},
                   {"experiment-duration",    required_argument, 0,  'r'},
                   {"delay-bound",            required_argument, 0,  'D'},
                   {"num-clients",            required_argument, 0,  'c'},
                   {"server-threads",         required_argument, 0,  't'},
                   {"validity-interval",      required_argument, 0,  'v'},
                   {"port",                   required_argument, 0,  'p'},
                   {0,                        0,                 0,  0  }
               };


  if (contextP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  memset(contextP, 0, sizeof(*contextP));
  (void) initProcessArguments(contextP);

  while (1) {

    option_index = 0;
    c = getopt_long(argc, argv, 
                    "m:c:v:s:u:r:p:d:t:h",
                    long_options, &option_index);
    if (c == -1) {
      break;
    }

    switch(c) {
      case 0:
        if (long_options[option_index].flag != 0) {
          break;
        }
        if (option_index == 0) {
          contextP->runningMode = CHRONOS_SERVER_XACT_EVALUATION_MODE;
          server_debug(2,"*** Running mode: %s", str(CHRONOS_SERVER_XACT_EVALUATION_MODE));
        }

        if (option_index == 1) {
          if (strncmp(CHRONOS_SERVER_XACT_NAME_VIEW_STOCK, 
                      optarg, 
                      strlen(CHRONOS_SERVER_XACT_NAME_VIEW_STOCK)) == 0) {
            contextP->evaluated_txn = CHRONOS_USER_TXN_VIEW_STOCK;
          }
          else if (strncmp(CHRONOS_SERVER_XACT_NAME_VIEW_PORTFOLIO, 
                      optarg, 
                      strlen(CHRONOS_SERVER_XACT_NAME_VIEW_PORTFOLIO)) == 0) {
            contextP->evaluated_txn = CHRONOS_USER_TXN_VIEW_PORTFOLIO;
          }
          else if (strncmp(CHRONOS_SERVER_XACT_NAME_PURCHASE, 
                      optarg, 
                      strlen(CHRONOS_SERVER_XACT_NAME_PURCHASE)) == 0) {
            contextP->evaluated_txn = CHRONOS_USER_TXN_PURCHASE;
          }
          else if (strncmp(CHRONOS_SERVER_XACT_NAME_SELL, 
                      optarg, 
                      strlen(CHRONOS_SERVER_XACT_NAME_SELL)) == 0) {
            contextP->evaluated_txn = CHRONOS_USER_TXN_SALE;
          }
          else if (strncmp(CHRONOS_SERVER_XACT_NAME_REFRESH_STOCK, 
                      optarg, 
                      strlen(CHRONOS_SERVER_XACT_NAME_REFRESH_STOCK)) == 0) {
            contextP->evaluated_txn = CHRONOS_USER_TXN_MAX;
          }
          else {
            server_error("Invalid argument for %s", long_options[option_index].name);
            goto failXit;
          }
        }

        if (option_index == 3) {
          contextP->initialLoad = 1;
          server_debug(2, "*** Initial load requested");
        }

        if (option_index == 5) {
          contextP->txn_size = atoi(optarg);
          if (contextP->txn_size > CHRONOS_MAX_DATA_ITEMS_PER_XACT) {
            server_warning("*** Requested size too large. Setting to %d", CHRONOS_MAX_DATA_ITEMS_PER_XACT);
            contextP->txn_size = CHRONOS_MAX_DATA_ITEMS_PER_XACT;
          }
          server_debug(2, "*** Transaction size requested: %u", contextP->txn_size);
        }

        break;

      case 'm':
        if (strncmp(CHRONOS_SERVER_MODE_NAME_BASE, 
                    optarg, 
                    strlen(CHRONOS_SERVER_MODE_NAME_BASE)) == 0) {
          contextP->runningMode = CHRONOS_SERVER_MODE_BASE;
        }
        else if (strncmp(CHRONOS_SERVER_MODE_NAME_AC, 
                    optarg, 
                    strlen(CHRONOS_SERVER_MODE_NAME_AC)) == 0) {
          contextP->runningMode = CHRONOS_SERVER_MODE_AC;
        }
        else if (strncmp(CHRONOS_SERVER_MODE_NAME_AUP, 
                    optarg, 
                    strlen(CHRONOS_SERVER_MODE_NAME_AUP)) == 0) {
          contextP->runningMode = CHRONOS_SERVER_MODE_AUP;
        }
        else if (strncmp(CHRONOS_SERVER_MODE_NAME_AC_AUP, 
                    optarg, 
                    strlen(CHRONOS_SERVER_MODE_NAME_AC_AUP)) == 0) {
          contextP->runningMode = CHRONOS_SERVER_MODE_AC_AUP;
        }
        else {
          server_error("Invalid mode");
          goto failXit;
        }

        server_debug(2,"*** Running mode: %d", contextP->runningMode);
        break;
      
      case 'c':
        contextP->numClientsThreads = atoi(optarg);
        server_debug(2,"*** Num clients: %d", contextP->numClientsThreads);
        break;
      
      case 't':
        contextP->numServerThreads = atoi(optarg);
        server_debug(2,"*** Num server threads: %d", contextP->numServerThreads);
        break;
      
      case 'v':
        contextP->initialValidityIntervalMS = atoi(optarg);
        server_debug(2, "*** Validity interval: %lld", contextP->initialValidityIntervalMS);
        break;

      case 'u':
        contextP->numUpdateThreads = atoi(optarg);
        server_debug(2, "*** Num update threads: %d", contextP->numUpdateThreads);
        break;

      case 'r':
        contextP->duration_sec = atoi(optarg);
        server_debug(2, "*** Duration: %ld", contextP->duration_sec);
        break;

      case 'p':
        contextP->serverPort = atoi(optarg);
        server_debug(2, "*** Server port: %d", contextP->serverPort);
        break;

      case 'D':
        contextP->desiredDelayBoundMS = atoi(optarg);
        server_debug(2, "*** Desired Delay Bound: %lld", contextP->desiredDelayBoundMS);
        break;

      case 'd':
        contextP->debugLevel = atoi(optarg);
        server_debug(2, "*** Debug Level: %d", contextP->debugLevel);
        break;

      case 'h':
        chronos_usage();
        exit(0);
	      break;

      default:
        server_error("Invalid argument");
        goto failXit;
    }
  }

  if (contextP->numClientsThreads < 1) {
    server_error("number of clients must be > 0");
    goto failXit;
  }

  if (contextP->initialValidityIntervalMS <= 0) {
    server_error("validity interval must be > 0");
    goto failXit;
  }

  if (contextP->numUpdateThreads <= 0) {
    server_error("number of update threads must be > 0");
    goto failXit;
  }

  if (contextP->serverPort <= 0) {
    server_error("port must be a valid one");
    goto failXit;
  }

  if (contextP->runningMode == CHRONOS_SERVER_XACT_EVALUATION_MODE) {
    server_debug(2, "*** Evaluating transaction: %s", chronos_user_transaction_str[serverContextP->evaluated_txn]);
  }

#ifdef LITMUS_RT
  if (contextP->runningMode == CHRONOS_SERVER_XACT_EVALUATION_MODE
      || contextP->runningMode == CHRONOS_SERVER_MODE_AC
      || contextP->runningMode == CHRONOS_SERVER_MODE_AUP
      || contextP->runningMode == CHRONOS_SERVER_MODE_AC_AUP) {
    server_error("Could not use this mode when running in Litmus Mode");
    goto failXit;
  }
#endif

  return CHRONOS_SERVER_SUCCESS;

failXit:
  chronos_usage();
  return CHRONOS_SERVER_FAIL;
}

static int
dispatchTableFn (chronosRequestPacket_t    *reqPacketP, 
                 int                       *txn_rc_ret, 
                 chronosServerThreadInfo_t *infoP)
{
  int rc;
  struct timeval current_time;
  struct timeval timeout;
  unsigned long long ticket = 0;
  volatile int txn_done = 0;
  volatile int txn_rc = CHRONOS_SERVER_FAIL; 
  pthread_t tid = pthread_self();

  if (infoP == NULL || infoP->contextP == NULL || txn_rc_ret == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  /*===========================================
   * Put a new transaction in the txn queue
   *==========================================*/
  server_debug(2, "%lu: Processing transaction: %s", 
               tid, CHRONOS_TXN_NAME(reqPacketP->txn_type));

  *txn_rc_ret = CHRONOS_SERVER_FAIL;
  getTime(&current_time);

  rc = chronos_enqueue_user_transaction(reqPacketP, 
                                        &current_time, 
                                        &ticket,
                                        &txn_done, 
                                        &txn_rc, 
                                        infoP->contextP->userTxnQueue);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to enqueue request");
    goto failXit;
  }

  /* Wait until the transaction is processed by the processThread */
  while (!txn_done && !time_to_die) {
    milliSleep(100, timeout);
  }

  *txn_rc_ret = txn_rc;

  server_debug(2, "%lu: Done processing transaction: %s, ticket: %llu, rc: %d", 
               tid, CHRONOS_TXN_NAME(reqPacketP->txn_type), ticket, txn_rc);

  return CHRONOS_SERVER_SUCCESS;

failXit:
  return CHRONOS_SERVER_FAIL;
}

/*
 * Starting point for a handlerThread.
 * handle a transaction request
 */
static void
daHandler(void *argP) 
{
  int num_bytes;
  int written, to_write;
  int txn_rc = 0;
  chronosResponsePacket_t resPacket;
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  chronosRequestPacket_t reqPacket;
  pthread_t tid = pthread_self();

#ifdef LITMUS_RT
  int rc = CHRONOS_SERVER_SUCCESS;
  struct rt_task params;
#endif

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

#ifdef LITMUS_RT
  rc = init_rt_thread();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to initialize litmus thread");
    goto cleanup;
  }

	memset(&params, 0, sizeof(params));

	init_rt_task_param(&params);
  params.exec_cost = CLIENT_THREAD_EXEC_COST;
  params.period = CLIENT_THREAD_PERIOD;
  params.relative_deadline = CLIENT_THREAD_RELATIVE_DEADLINE;
  params.cpu = 1001;

  rc = set_rt_task_param(gettid(), &params);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set real time parameters");
    goto cleanup;
  }

  if (infoP->thread_num % 5 == 0) {
    server_info("Migrating to core: %d....", 1);
    rc = be_migrate_to_domain(1);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to migrate to rid");
      goto cleanup;
    }
  }
  else if (infoP->thread_num % 5 == 1) {
    server_info("Migrating to core: %d....", 2);
    rc = be_migrate_to_domain(2);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to migrate to rid");
      goto cleanup;
    }
  }
  else if (infoP->thread_num % 5 == 2) {
    server_info("Migrating to core: %d....", 3);
    rc = be_migrate_to_domain(3);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to migrate to rid");
      goto cleanup;
    }
  }
  else if (infoP->thread_num % 5 == 3) {
    server_info("Migrating to core: %d....", 4);
    rc = be_migrate_to_domain(4);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to migrate to rid");
      goto cleanup;
    }
  }
  else if (infoP->thread_num % 5 == 4) {
    server_info("Migrating to core: %d....", 5);
    rc = be_migrate_to_domain(5);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to migrate to rid");
      goto cleanup;
    }
  }

  rc = task_mode(LITMUS_RT_TASK);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set litmus task model");
    goto cleanup;
  }

  server_info("Waiting for synchronous release for handler threads....");
  server_info("RZ_DEBUG: exec_cost: %llu, period: %llu, deadlile: %llu, cpu: %u",
              params.exec_cost, params.period, params.relative_deadline, params.cpu);
  rc = wait_for_ts_release();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to release real-time task");
    goto cleanup;
  }
#endif

  while (!time_to_die) {
    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);



    /*------------ Read the request -----------------*/
    server_debug(3,"%lu: waiting new request", tid);

    memset(&reqPacket, 0, sizeof(reqPacket));
    char *buf = (char *) &reqPacket;
    int   to_read = sizeof(reqPacket);

    while (to_read > 0) {
      num_bytes = read(infoP->socket_fd, buf, 1);
      if (num_bytes < 0) {
        server_error("Failed while reading request from client");
        goto cleanup;
      }

      to_read -= num_bytes;
      buf += num_bytes;
    }

    assert(CHRONOS_TXN_IS_VALID(reqPacket.txn_type));
    server_debug(3, "%lu: Received transaction request: %s", 
                 tid, CHRONOS_TXN_NAME(reqPacket.txn_type));
#if 0
    (void) chronosRequestDump((CHRONOS_REQUEST_H)&reqPacket);
#endif
    /*-----------------------------------------------*/


    /*----------- do admission control ---------------*/
    if (IS_CHRONOS_MODE_AC(infoP->contextP) || IS_CHRONOS_MODE_FULL(infoP->contextP)) {
      admission_control(infoP->contextP);
    }
    /*-----------------------------------------------*/

    if (time_to_die == 1) {
      server_info("Requested to die");
      goto cleanup;
    }

    /*----------- Process the request ----------------*/
    if (dispatchTableFn(&reqPacket, &txn_rc, infoP) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to handle request");
      goto cleanup;
    }
    /*-----------------------------------------------*/




    /*---------- Reply to the request ---------------*/
    server_debug(3, "%lu: Replying to client", tid);

    memset(&resPacket, 0, sizeof(resPacket));
    resPacket.txn_type = reqPacket.txn_type;
    resPacket.rc = txn_rc;

    buf = (char *)&resPacket;
    to_write = sizeof(resPacket);

    while(to_write >0) {
      written = write(infoP->socket_fd, buf, to_write);
      if (written < 0) {
        server_error("Failed to write to socket");
        goto cleanup;
      }

      to_write -= written;
      buf += written;
    }
    server_debug(3, "%lu: Replied to client: txn rc %d", tid, txn_rc);
    /*-----------------------------------------------*/


    if (time_to_die == 1) {
      server_info("%lu: Requested to die", tid);
      goto cleanup;
    }
  }

cleanup:

#ifdef LITMUS_RT
  task_mode(BACKGROUND_TASK);
#endif

  close(infoP->socket_fd);

  free(infoP);

  server_info("%lu: daHandler exiting", tid);

  return;
}

/*
 * listen for client requests
 */
static void *
daListener(void *argP) 
{
  int rc;
  struct sockaddr_in server_address;
  struct sockaddr_in client_address;
  struct pollfd fds[1];
  int socket_fd;
  int accepted_socket_fd;
  int on = 1;
  int counter = 0;
  socklen_t client_address_len;
  pthread_attr_t attr;
  const int stack_size = 0x100000; // 1 MB
  chronosServerThreadInfo_t *handlerInfoP = NULL;
  threadpool thpoolH = NULL;
  pthread_t tid = pthread_self();

  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  server_debug(1, "%lu: Starting listener thread...", tid);

  rc = pthread_attr_init(&attr);
  if (rc != 0) {
    perror("failed to init thread attributes");
    goto cleanup;
  }
  
  rc = pthread_attr_setstacksize(&attr, stack_size);
  if (rc != 0) {
    perror("failed to set stack size");
    goto cleanup;
  }

  rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (rc != 0) {
    perror("failed to set detachable attribute");
    goto cleanup;
  }

  server_info("Creating pool of %d threads", infoP->contextP->numClientsThreads);
  thpoolH = thpool_init(infoP->contextP->numClientsThreads);
  if (thpoolH == NULL) {
    server_error("Failed to init thread pool");
    goto cleanup;
  }

  /* Create socket to receive incoming connections */
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    perror("Failed to create socket");
    goto cleanup;
  }

  /* Make socket reusable */
  rc = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
  if (rc == -1) {
    perror("setsockopt() failed");
    goto cleanup;
  }

  /* Make non-blocking socket */
  rc = ioctl(socket_fd, FIONBIO, (char *)&on);
  if (rc < 0) {
    perror("ioctl() failed");
    goto cleanup;
  }

  server_address.sin_family = AF_INET;
  // Perhaps I need to change the ip address in the following line
  server_address.sin_addr.s_addr = inet_addr(CHRONOS_SERVER_ADDRESS);
  server_address.sin_port = htons(infoP->contextP->serverPort);

  rc = bind(socket_fd, (struct sockaddr *)&server_address, sizeof(server_address));
  if (rc < 0) {
    perror("bind() failed");
    goto cleanup;
  }

  rc = listen(socket_fd, CHRONOS_TCP_QUEUE);
  if (rc < 0) {
    perror("listen() failed");
    goto cleanup;
  }

  fds[0].events = POLLIN;
  fds[0].fd = socket_fd;

  server_debug(4, "%lu: Waiting for incoming connections...", tid);

  /* Keep listening for incoming connections till we
   * complete all threads for the experiment
   */
  while(!time_to_die) {

    server_debug(4, "%lu: Polling for new connection", tid);
    
    /* wait for a new connection */
    rc = poll(fds, 1, 1000 /* one second */);
    if (rc < 0) {
      perror("poll() failed");
      goto cleanup;
    }
    else if (rc == 0) {
      server_debug(4, "%lu: poll() timed out", tid);
      continue;
    }
    else {
      server_debug(4, "%lu: %d descriptors are ready", tid, rc);
    }

    /* We were only interested on the socket fd */
    assert(fds[0].revents);

    /* Accept all incoming connections that are queued up 
     * on the listening socket before we loop back and
     * call select again
     */
    do {
      client_address_len = sizeof(client_address); 
      accepted_socket_fd = accept(socket_fd, (struct sockaddr *)&client_address, &client_address_len);

      if (accepted_socket_fd == -1) {
        if (errno != EWOULDBLOCK) {
          perror("accept() failed");
          goto cleanup;
        }
        break;
      }

      handlerInfoP  = NULL;
      handlerInfoP = calloc(1, sizeof(chronosServerThreadInfo_t));
      if (handlerInfoP == NULL) {
        server_error("Failed to allocate space for thread info");
        goto cleanup;
      }

      handlerInfoP->socket_fd = accepted_socket_fd;
      handlerInfoP->contextP = infoP->contextP;
      handlerInfoP->state = CHRONOS_SERVER_THREAD_STATE_RUN;
      handlerInfoP->magic = CHRONOS_SERVER_THREAD_MAGIC;
      handlerInfoP->thread_num = counter ++;

      rc = thpool_add_work(thpoolH, daHandler, handlerInfoP);
      if (rc != 0) {
        server_error("failed to spawn thread");
        goto cleanup;
      }

      server_info("%lu: Spawned handler thread", tid);

    } while (accepted_socket_fd != -1);

  }

  if (time_to_die == 1) {
    server_info("%lu: Requested to die", tid);
    goto cleanup;
  }

cleanup:
  thpool_destroy(thpoolH);
  thpoolH = NULL;

  server_info("%lu: daListener exiting", tid);
  pthread_exit(NULL);
}

/*
 * Wait till the next release time
 */
static int
waitPeriod(double updatePeriodMS)
{
  struct timespec updatePeriod;

  updatePeriod.tv_sec = updatePeriodMS / 1000;
  updatePeriod.tv_nsec = ((int)updatePeriodMS % 1000) * 1000000;

  /* TODO: do I need to check the second argument? */
  nanosleep(&updatePeriod, NULL);
  
  return CHRONOS_SERVER_SUCCESS;
}

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
static int
processUserTransaction(volatile int              *txn_rc,
                       chronosRequestPacket_t    *reqPacketP,
                       struct timeval             enqueued_time,
                       chronosServerThreadInfo_t *infoP)
{
  int               rc = CHRONOS_SERVER_SUCCESS;
  int               i;
  int               num_data_items = 0;
  struct timeval    finish_time;
  struct timeval    timeout;
  int               idx_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  const char        *pkey_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  chronosUserTransaction_t txn_type;
  BENCHMARK_DATA_PACKET_H data_packetH = NULL;
  pthread_t tid = pthread_self();
  static unsigned int msg_cnt = 0;

  if (infoP == NULL || infoP->contextP == NULL || txn_rc == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  txn_type = reqPacketP->txn_type;
  num_data_items = reqPacketP->numItems;
  
  /* dispatch a transaction */
  switch(txn_type) {

    case CHRONOS_USER_TXN_VIEW_STOCK:
      memset(pkey_list, 0, sizeof(pkey_list));
      for (i=0; i<num_data_items; i++) {
        idx_list[i] = reqPacketP->request_data.symbolInfo[i].symbolIdx;
        pkey_list[i] = reqPacketP->request_data.symbolInfo[i].symbol;
      }
      *txn_rc = benchmark_view_stock2(num_data_items, pkey_list, infoP->contextP->benchmarkCtxtP);

      if (IS_CHRONOS_MODE_FULL(infoP->contextP) || IS_CHRONOS_MODE_AUP(infoP->contextP)) {
        for (i=0; i<num_data_items; i++) {
          rc = chronos_aup_af_incr(infoP->contextP->aup_env, idx_list[i]);
          if (rc != CHRONOS_SERVER_SUCCESS) {
            server_warning("[AC]: Failed to update Access Frequency");
            continue;
          }
        } /* foreach updated symbol */
      }

      break;

    case CHRONOS_USER_TXN_VIEW_PORTFOLIO:
      memset(pkey_list, 0, sizeof(pkey_list));
      for (i=0; i<num_data_items; i++) {
        pkey_list[i] = reqPacketP->request_data.portfolioInfo[i].accountId;
      }
      *txn_rc = benchmark_view_portfolio2(num_data_items, pkey_list, infoP->contextP->benchmarkCtxtP);
      break;

    case CHRONOS_USER_TXN_PURCHASE:
      rc = benchmark_data_packet_alloc(num_data_items, &data_packetH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Could not allocate data packet");
        goto failXit;
      }

      for (i=0; i<num_data_items; i++) {
        rc = benchmark_data_packet_append(reqPacketP->request_data.purchaseInfo[i].accountId,
                                          reqPacketP->request_data.purchaseInfo[i].symbolId,
                                          reqPacketP->request_data.purchaseInfo[i].symbol,
                                          reqPacketP->request_data.purchaseInfo[i].price,
                                          reqPacketP->request_data.purchaseInfo[i].amount,
                                          data_packetH);
        if (rc != CHRONOS_SERVER_SUCCESS) {
          server_error("Could not append data to packet");
          goto failXit;
        }
      }
    
      *txn_rc = benchmark_purchase2(data_packetH,
                                    infoP->contextP->benchmarkCtxtP);

      rc = benchmark_data_packet_free(data_packetH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Could not free data packet");
        goto failXit;
      }
      data_packetH = NULL;

      break;

    case CHRONOS_USER_TXN_SALE:
      rc = benchmark_data_packet_alloc(num_data_items, &data_packetH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Could not allocate data packet");
        goto failXit;
      }

      for (i=0; i<num_data_items; i++) {
        rc = benchmark_data_packet_append(reqPacketP->request_data.purchaseInfo[i].accountId,
                                          reqPacketP->request_data.purchaseInfo[i].symbolId,
                                          reqPacketP->request_data.purchaseInfo[i].symbol,
                                          reqPacketP->request_data.purchaseInfo[i].price,
                                          reqPacketP->request_data.purchaseInfo[i].amount,
                                          data_packetH);
        if (rc != CHRONOS_SERVER_SUCCESS) {
          server_error("Could not append data to packet");
          goto failXit;
        }
      }
    
      *txn_rc = benchmark_sell2(data_packetH,
                                infoP->contextP->benchmarkCtxtP);

      rc = benchmark_data_packet_free(data_packetH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Could not free data packet");
        goto failXit;
      }
      data_packetH = NULL;
      break;

    default:
      assert(0);
  } /* switch */

  server_debug(3, "%lu: Txn rc: %d", tid, *txn_rc);

  if (*txn_rc != CHRONOS_SERVER_SUCCESS) {
    server_warning("%lu: Txn rc: %d", tid, *txn_rc);
  }

  if (IS_CHRONOS_MODE_AC(infoP->contextP) || IS_CHRONOS_MODE_FULL(infoP->contextP)) {
    chronos_ac_wait_decrease(infoP->contextP->ac_env);

#if 0
    infoP->contextP->period_smoth_degree_timing_violation = in_period_ds(infoP->contextP);
    if (infoP->contextP->smoth_degree_timing_violation > 0.0) {
      server_warning("%lu: period ds: %.2lf", tid, infoP->contextP->period_smoth_degree_timing_violation);
    }
#endif
  }
  /*--------------------------------------------*/
   


  /*---------------------------------------------
   * Update the cumulative thread stats
   *--------------------------------------------*/
  getTime(&finish_time);
  if (!warming_up) {
    update_thread_stats(&enqueued_time, 
                        &finish_time,
                        infoP->contextP->desiredDelayBoundMS,
                        txn_type,
                        *txn_rc,
                        infoP->thread_num,
                        infoP->contextP->threadStatsArr);
  }

  server_debug(1, "%lu: Done processing user txn...", tid);
  goto cleanup;

failXit:
  if (data_packetH != NULL) {
    rc = benchmark_data_packet_free(data_packetH);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Could not free data packet");
      goto failXit;
    }
    data_packetH = NULL;
  }

  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}
#endif

/*--------------------------------------------------------------
 * Executes the transaction specified by requestP against 
 * the backend database.
 *------------------------------------------------------------*/
static int
processRefreshTransaction(chronosRequestPacket_t    *requestP,
                          struct timeval             enqueued_time,
                          chronosServerThreadInfo_t *infoP)
{
  int               rc = CHRONOS_SERVER_SUCCESS;
  int               txn_rc = CHRONOS_SERVER_SUCCESS;
  chronosServerContext_t *contextP = NULL;
  int               i;
  int               num_data_items;
  int               idx_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  float             fvalues_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  const char       *pkey_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  pthread_t         tid = pthread_self();
  struct timeval    finish_time;

  if (infoP == NULL || infoP->contextP == NULL || requestP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  contextP = infoP->contextP;


  server_debug(3, "%lu: (thread %d) Processing update...", tid, infoP->thread_num);

  /* Generate the list of updates */
  num_data_items = requestP->numItems;

  for (i=0; i<num_data_items; i++) {
    idx_list[i] = requestP->request_data.updateInfo[i].symbolIdx;
    pkey_list[i] = requestP->request_data.updateInfo[i].symbol;
    fvalues_list[i] = requestP->request_data.updateInfo[i].price;
  }

  txn_rc = benchmark_refresh_quotes_list(num_data_items,
                                     pkey_list,
                                     fvalues_list,
                                     contextP->benchmarkCtxtP);

  server_debug(3, "%lu: (thread %d) Done processing update...", tid, infoP->thread_num);

#if 0
  if (IS_CHRONOS_MODE_AC(infoP->contextP) || IS_CHRONOS_MODE_FULL(infoP->contextP)) {
    chronos_ac_wait_decrease(infoP->contextP->ac_env);
  }
#endif

  /*---------------------------------------------
   * Update the cumulative thread stats
   *--------------------------------------------*/
  getTime(&finish_time);
  if (!warming_up) {
    update_thread_stats(&enqueued_time, 
                        &finish_time,
                        infoP->contextP->desiredDelayBoundMS,
                        CHRONOS_SYS_TXN_UPDATE_STOCK,
                        txn_rc,
                        infoP->thread_num,
                        infoP->contextP->threadStatsArr);
  }


  if (IS_CHRONOS_MODE_FULL(infoP->contextP) || IS_CHRONOS_MODE_AUP(infoP->contextP)) {
    for (i=0; i<num_data_items; i++) {
      rc = chronos_aup_uf_incr(contextP->aup_env, idx_list[i]);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_warning("[AC]: Failed to update Update Frequency");
        continue;
      }
    } /* foreach updated symbol */
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

/*
 * This is the driver function of process thread
 */
static void *
processThread(void *argP) 
{
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  chronosRequestPacket_t receivedRequest;
  struct timeval         received_time;
  unsigned long long     ticket;
  volatile int          *txn_doneP = NULL;
  volatile int          *txn_rcP = NULL;
  unsigned long         total_experiment_duration_ms = 0;
  long int              elapsed_experiment_time_ms;
  long int              elapsed_since_last_sample_ms;
  struct timeval        start_experiment_time;
  struct timeval        previous_sample;
  struct timeval        current_time;
  int current_rep = 0;
  int size_user_queue = 0;
  int size_sys_queue = 0;
  int rc;
  pthread_t tid = pthread_self();
#ifdef LITMUS_RT
  struct rt_task params;
#endif
  
  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  num_started_server_threads ++;
  pthread_cond_signal(&serverContextP->startThreadsWait);

  total_experiment_duration_ms = CHRONOS_SEC_TO_MS(infoP->contextP->duration_sec) + warmup_duration_ms;

#ifdef LITMUS_RT
  rc = init_rt_thread();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to initialize litmus thread");
    goto cleanup;
  }

	memset(&params, 0, sizeof(params));

	init_rt_task_param(&params);
  params.exec_cost = PROCESS_THREAD_EXEC_COST;
  params.period = PROCESS_THREAD_PERIOD;
  params.relative_deadline = PROCESS_THREAD_RELATIVE_DEADLINE;
  params.cpu = 1001;

  rc = set_rt_task_param(gettid(), &params);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set real time parameters");
    goto cleanup;
  }

  rc = be_migrate_to_domain(6);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to migrate to rid");
    goto cleanup;
  }

  rc = task_mode(LITMUS_RT_TASK);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set litmus task model");
    goto cleanup;
  }

  server_info("RZ_DEBUG: exec_cost: %llu, period: %llu, deadlile: %llu, cpu: %u",
              params.exec_cost, params.period, params.relative_deadline, params.cpu);

#if 1
  server_info("Waiting for synchronous release for update threads....");
  rc = wait_for_ts_release();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to release real-time task");
    goto cleanup;
  }
#endif
#endif

  getTime(&start_experiment_time);
  getTime(&previous_sample);

  /* Give update transactions more priority */
  /* TODO: Add scheduling technique */
  while (!time_to_die) {

    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

#ifdef CHRONOS_UPDATE_TRANSACTIONS_ENABLED
    int num_txn_to_dequeue = chronos_queue_size(infoP->contextP->sysTxnQueue);

    while (num_txn_to_dequeue -- > 0) {
      /*-------- Process refresh transaction ----------*/
      current_rep ++;

      rc = chronos_dequeue_system_transaction(&receivedRequest,
                                              &received_time,
                                              infoP->contextP->sysTxnQueue);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to dequeue request");
        goto cleanup;
      }

      CHRONOS_REQUEST_MAGIC_CHECK(&receivedRequest);

      //server_info("%lu: (refresh xact) Processing ticket: %d", tid, 0);
      rc = processRefreshTransaction(&receivedRequest, received_time, infoP);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to execute refresh transactions");
      }

      /*--------------------------------------------*/
    }
#endif

    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);
    
#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
    if (chronos_queue_size(infoP->contextP->userTxnQueue) > 0) {
      /*-------- Process user transaction ----------*/
      current_rep ++;

      txn_doneP = NULL;
      txn_rcP = NULL;

      rc = chronos_dequeue_user_transaction(&receivedRequest,
                                            &received_time,
                                            &ticket,
                                            &txn_doneP,
                                            &txn_rcP,
                                            infoP->contextP->userTxnQueue);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to dequeue request");
        goto cleanup;
      }

      assert(txn_doneP != NULL);
      assert(txn_rcP != NULL);

      CHRONOS_REQUEST_MAGIC_CHECK(&receivedRequest);

      //server_info("%lu: (user xact) Processing ticket: %llu", tid, ticket);
      if (processUserTransaction(txn_rcP, 
                                 &receivedRequest, 
                                 received_time, 
                                 infoP) != CHRONOS_SERVER_SUCCESS) {
        server_error("%lu: Failed to execute refresh transactions", tid);
      }

      if (txn_doneP != NULL) {
        *txn_doneP = 1;
      }

      /*--------------------------------------------*/
    }
#endif

    getTime(&current_time);
    elapsed_experiment_time_ms = diff_time(&start_experiment_time, &current_time);

    server_debug(1, "%lu: elapsed_experiment_time_ms: %ld, duration_time: %ld, current_rep: %d", 
                tid, elapsed_experiment_time_ms, 
                total_experiment_duration_ms,
                current_rep);

    if (elapsed_experiment_time_ms >= warmup_duration_ms && warming_up == 1) {
      warming_up = 0;
      server_info("warming_up = %u", warming_up);
    }

    if (elapsed_experiment_time_ms >= total_experiment_duration_ms) {
      server_info("Setting time_to_die = 1");
      time_to_die = 1;
      break;
    }

    elapsed_since_last_sample_ms = diff_time(&previous_sample, &current_time);
    if (elapsed_since_last_sample_ms > 1000 * CHRONOS_SAMPLING_PERIOD_SEC) {
      aggregateStats(serverContextP);
      performanceMonitor(serverContextP);
      //qosManager(serverContextP);
      //adaptive_update_policy(serverContextP);

      stats_print(serverContextP);
      previous_sample = current_time;
    }
  }
  
cleanup:

  server_info("processThread exiting");
  
  pthread_exit(NULL);
}

/*
 * This is the driver function of an update thread
 */
static void *
updateThread(void *argP) 
{
  int    rc = CHRONOS_SERVER_SUCCESS;
  int    first_symbol = 0;
  int    num_updates = 0;
  int    update_list[CHRONOS_NUM_STOCK_UPDATES_PER_UPDATE_THREAD];
  int    num_elements;
  int    i;
  struct timeval current_time;
  struct timeval timeout;
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  CHRONOS_ENV_H   chronosEnvH;
  CHRONOS_CACHE_H chronosCacheH = NULL;
  CHRONOS_CLIENT_CACHE_H  clientCacheH = NULL;
  CHRONOS_REQUEST_H requestH = NULL;

  pthread_t tid = pthread_self();
  double update_period = 0;

#ifdef LITMUS_RT
  lt_t cycle_length;
  lt_t slot_offset;
  lt_t now;
  lt_t next_cycle_start;
  struct rt_task params;
#endif

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  num_updates = infoP->contextP->numUpdatesPerUpdateThread;
  assert(num_updates > 0);
  first_symbol = infoP->thread_num * serverContextP->numUpdatesPerUpdateThread;

  num_elements = num_updates;
  for (i=0; i<num_updates; i++) {
    update_list[i] = i + first_symbol;
  }

  /* Create a chronos environment which holds a cache
   * of the users and symbols managed by the system
   */
  chronosEnvH = chronosEnvAlloc(CHRONOS_SERVER_HOME_DIR, 
                                CHRONOS_SERVER_DATAFILES_DIR);
  if (chronosEnvH == NULL) {
    server_error("Failed to allocate chronos environment handle");
    goto cleanup;
  }

  chronosCacheH = chronosEnvCacheGet(chronosEnvH);
  if (chronosCacheH == NULL) {
    server_error("Invalid cache handle");
    goto cleanup;
  }

  rc = chronosCacheSymbolsRangeSet(first_symbol, num_updates, chronosCacheH);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set cache range");
    goto cleanup;
  }
  

  /* Create a client cache. A client cache contains a number
   * of porfolios and each portfolio contains stock information
   * about a number of stocks.
   */
  clientCacheH = chronosClientCacheAlloc(1 /* numClient */, 
                                         1 /* numClients */,
                                         chronosCacheH);
  if (clientCacheH == NULL) {
    server_error("Invalid client cache handle");
    goto cleanup;
  }

  update_period = 0.5 * infoP->contextP->initialValidityIntervalMS;

  milliSleep(warmup_duration_ms, timeout);

#ifdef LITMUS_RT
  rc = init_rt_thread();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to initialize litmus thread");
    goto cleanup;
  }

	memset(&params, 0, sizeof(params));

	init_rt_task_param(&params);
  params.exec_cost = UPDATE_THREAD_EXEC_COST;
  params.period = UPDATE_THREAD_PERIOD;
  params.relative_deadline = UPDATE_THREAD_RELATIVE_DEADLINE;
  params.cpu = ((1 + 0) * 1000) + infoP->thread_num;
	params.phase = UPDATE_THREAD_PHASE * infoP->thread_num;

  cycle_length = UPDATE_THREAD_MAJOR_CYCLE;
  slot_offset = params.phase;

  rc = set_rt_task_param(gettid(), &params);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set real time parameters");
    goto cleanup;
  }

  rc = be_migrate_to_domain(0);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to migrate to rid");
    goto cleanup;
  }

  rc = task_mode(LITMUS_RT_TASK);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set litmus task model");
    goto cleanup;
  }

  server_info("Waiting for synchronous release for update thread....");
  server_info("RZ_DEBUG: exec_cost: %llu, period: %llu, deadlile: %llu, cpu: %u, phase: %llu, cycle: %llu, offset: %llu",
              params.exec_cost, params.period, params.relative_deadline, params.cpu,
              params.phase, cycle_length, slot_offset);
  rc = wait_for_ts_release();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to release real-time task");
    goto cleanup;
  }
#endif

  while (1) {
    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);


#ifdef LITMUS_RT
    now = litmus_clock();
    next_cycle_start = ((now / cycle_length) + 1) * cycle_length;
    lt_sleep_until(next_cycle_start + slot_offset);
#endif

    server_debug(3, "%lu: --- Starting new cycle (thread %d) ---", 
                tid, infoP->thread_num);
    getTime(&current_time);

    /*--------------------------------------------------
     * TODO: According to the paper, a server thread
     * is responsible for refreshing only a certain
     * chunk of the data items. 
     *
     * This code makes the server thread pick a set
     * of n random data items.
     *-------------------------------------------------*/
    if (IS_CHRONOS_MODE_FULL(infoP->contextP) || IS_CHRONOS_MODE_AUP(infoP->contextP)) 
    {
      num_elements = chronos_aup_get_n_expired(infoP->contextP->aup_env, 
                                     first_symbol,
                                     num_updates,
                                     CHRONOS_NUM_STOCK_UPDATES_PER_UPDATE_THREAD, 
                                     update_list);
      if (num_elements <= 0) {
        goto nothingToDo;
      }

      //server_info("Creating request with %d elements", num_elements);
      requestH = chronosRequestUpdateFromListCreate(num_elements,
                                                    update_list, 
                                                    clientCacheH, 
                                                    chronosEnvH);
      chronos_aup_next_update_set(infoP->contextP->aup_env,
                                  num_elements,
                                  update_list);
#if 0
      (void) chronosRequestDump(requestH);
#endif
    }
    else 
    {
      requestH = chronosRequestUpdateFromListCreate(num_elements,
                                                    update_list, 
                                                    clientCacheH, 
                                                    chronosEnvH);
#if 0
      requestH = chronosRequestCreate(num_updates,
                                      CHRONOS_SYS_TXN_UPDATE_STOCK,
                                      clientCacheH, 
                                      chronosEnvH);
#endif
#if 0
      (void) chronosRequestDump(requestH);
#endif
    }

    if (requestH == NULL) {
      server_error("Failed to populate request");
      goto cleanup;
    }

    rc = chronos_enqueue_system_transaction(requestH,
                                            &current_time,
                                            infoP->contextP->sysTxnQueue);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to enqueue request");
      goto cleanup;
    }

    rc = chronosRequestFree(requestH);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to release request");
      goto cleanup;
    }

#if 0
    (void) chronos_aup_data_set_dump(infoP->contextP->aup_env, first_symbol, num_updates);
#endif

nothingToDo:
#ifndef LITMUS_RT
    if (IS_CHRONOS_MODE_FULL(infoP->contextP) || IS_CHRONOS_MODE_AUP(infoP->contextP)) {
      milliSleep(50, timeout);
    }
    else {
#if 0
      if (waitPeriod(update_period) != CHRONOS_SERVER_SUCCESS) {
        goto cleanup;
      }
#endif
      server_info("Sleeping %lf", update_period);
      milliSleep(update_period, timeout);
    }
#endif


    if (time_to_die == 1) {
      server_info("%lu: Requested to die", tid);
      goto cleanup;
    }
  }

cleanup:
 
#ifdef LITMUS_RT
  task_mode(BACKGROUND_TASK);
#endif

  if (clientCacheH != NULL) {
    chronosClientCacheFree(clientCacheH);
    clientCacheH = NULL;
  }

  server_info("%lu: updateThread exiting", tid);
  pthread_exit(NULL);
}


#if 0
/*----------------------------------------------------
 * This thread performs the sampling. Sampling
 * is executed a number of times per minute.
 *--------------------------------------------------*/
static void *
samplingThread(void *argP) 
{
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  struct timeval timeout;
  pthread_t tid = pthread_self();
  chronosServerContext_t *serverContextP = NULL;
  int rc = CHRONOS_SERVER_SUCCESS;

#ifdef LITMUS_RT
  lt_t cycle_length;
  lt_t slot_offset;
  lt_t now;
  lt_t next_cycle_start;
  struct rt_task params;
#endif

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  serverContextP = infoP->contextP;

  pthread_mutex_lock(&serverContextP->startThreadsMutex);
  while(num_started_server_threads < serverContextP->numServerThreads) {
    pthread_cond_wait(&serverContextP->startThreadsWait,
                      &serverContextP->startThreadsMutex);
  }
  pthread_mutex_unlock(&serverContextP->startThreadsMutex);

  server_info("%lu: Starting sampling....", tid);

  while (warming_up) {
    milliSleep(1000, timeout);
  }

#ifdef LITMUS_RT
  rc = init_rt_thread();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to initialize litmus thread");
    goto cleanup;
  }

	memset(&params, 0, sizeof(params));

	init_rt_task_param(&params);
  params.exec_cost = TIMER_THREAD_EXEC_COST;
  params.period = TIMER_THREAD_PERIOD;
  params.relative_deadline = TIMER_THREAD_RELATIVE_DEADLINE;
  params.cpu = 1001;
	params.phase = TIMER_THREAD_PHASE;

  cycle_length = TIMER_THREAD_MAJOR_CYCLE;
  slot_offset = params.phase;

  rc = set_rt_task_param(gettid(), &params);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set real time parameters");
    goto cleanup;
  }

  rc = be_migrate_to_domain(TIMER_THREAD_CORE);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to migrate to rid");
    goto cleanup;
  }

  rc = task_mode(LITMUS_RT_TASK);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to set litmus task model");
    goto cleanup;
  }

  server_info("Waiting for synchronous release....");
  server_info("RZ_DEBUG: exec_cost: %llu, period: %llu, deadlile: %llu, cpu: %u, phase: %llu, cycle: %llu, offset: %llu",
              params.exec_cost, params.period, params.relative_deadline, params.cpu,
              params.phase, cycle_length, slot_offset);
  rc = wait_for_ts_release();
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to release real-time task");
    goto cleanup;
  }
#endif

  while (1) {

#ifndef LITMUS_RT
    milliSleep(1000 * CHRONOS_SAMPLING_PERIOD_SEC, timeout);
#else
    now = litmus_clock();
    next_cycle_start = ((now / cycle_length) + 1) * cycle_length;
    lt_sleep_until(next_cycle_start + slot_offset);
#endif

    aggregateStats(serverContextP);
    performanceMonitor(serverContextP);
    qosManager(serverContextP);
    adaptive_update_policy(serverContextP);

    stats_print(serverContextP);

    if (time_to_die == 1) {
      server_info("%lu: Requested to die", tid);
      goto cleanup;
    }
  }

cleanup:
#ifdef LITMUS_RT
  task_mode(BACKGROUND_TASK);
#endif
  server_info("%lu: samplingThread exiting", tid);
  pthread_exit(NULL);
}
#endif

static int 
runTxnEvaluation(chronosServerContext_t *serverContextP)
{
  int rc = CHRONOS_SERVER_SUCCESS;
  int i;
  int txn_rc = 0;
  int num_data_items;
  int num_success = 0;
  int num_failures = 0;
  int num_total_txns = 0;
  float              fvalues_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  const char        *pkey_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  CHRONOS_ENV_H   chronosEnvH;
  CHRONOS_CACHE_H chronosCacheH = NULL;
  CHRONOS_CLIENT_CACHE_H  clientCacheH = NULL;
  CHRONOS_REQUEST_H requestH = NULL;
  BENCHMARK_DATA_PACKET_H data_packetH = NULL;
  chronosRequestPacket_t *reqPacketP = NULL;

  chronosEnvH = chronosEnvAlloc(CHRONOS_SERVER_HOME_DIR, CHRONOS_SERVER_DATAFILES_DIR);
  if (chronosEnvH == NULL) {
    server_error("Failed to allocate chronos environment handle");
    goto failXit;
  }

  chronosCacheH = chronosEnvCacheGet(chronosEnvH);
  if (chronosCacheH == NULL) {
    server_error("Invalid cache handle");
    goto failXit;
  }

  clientCacheH = chronosClientCacheAlloc(1, 1, chronosCacheH);
  if (clientCacheH == NULL) {
    server_error("Invalid client cache handle");
    goto failXit;
  }

 if (serverContextP->initialLoad) {
    /* Create the system tables */
    serverContextP->benchmarkCtxtP = benchmark_initial_load(program_name, CHRONOS_SERVER_HOME_DIR, CHRONOS_SERVER_DATAFILES_DIR); 
    if (serverContextP->benchmarkCtxtP == NULL) {
      server_error("Failed to perform initial load");
      goto failXit;
    }
  }
  else {
    server_info("*** Skipping initial load");
    /* Obtain a benchmark handle */
    if (benchmark_handle_alloc(&serverContextP->benchmarkCtxtP, 
                               0, 
                               program_name,
                               CHRONOS_SERVER_HOME_DIR, 
                               CHRONOS_SERVER_DATAFILES_DIR) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to allocate handle");
      goto failXit;
    }
  }
  
  
  rc = benchmark_portfolios_stats_get(serverContextP->benchmarkCtxtP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to get portfolios stat");
    goto failXit;
  }

  RTCAPTURE_START();

  int current_rep = 0;
  for (current_rep = 0; current_rep < NUM_SAMPLES; current_rep ++) {

      server_info("(%d) Evaluating: %s", 
                  current_rep, chronos_user_transaction_str[serverContextP->evaluated_txn]);

      server_info("----------------------------------------------");

      requestH = chronosRequestCreate(serverContextP->txn_size,
                                      serverContextP->evaluated_txn, 
                                      clientCacheH, 
                                      chronosEnvH);
      if (requestH == NULL) {
        server_error("Failed to populate request");
        goto cleanup;
      }

      reqPacketP = (chronosRequestPacket_t *) requestH;
      num_data_items = reqPacketP->numItems;

      switch(serverContextP->evaluated_txn) {

      case CHRONOS_USER_TXN_VIEW_STOCK:
        memset(pkey_list, 0, sizeof(pkey_list));
        for (i=0; i<num_data_items; i++) {
          pkey_list[i] = reqPacketP->request_data.symbolInfo[i].symbol;
        }
        txn_rc = benchmark_view_stock2(num_data_items, pkey_list, serverContextP->benchmarkCtxtP);
        break;

      case CHRONOS_USER_TXN_VIEW_PORTFOLIO:
        memset(pkey_list, 0, sizeof(pkey_list));
        for (i=0; i<num_data_items; i++) {
          pkey_list[i] = reqPacketP->request_data.portfolioInfo[i].accountId;
        }
        txn_rc = benchmark_view_portfolio2(num_data_items, pkey_list, serverContextP->benchmarkCtxtP);
        break;

      case CHRONOS_USER_TXN_PURCHASE:
        rc = benchmark_data_packet_alloc(num_data_items, &data_packetH);
        if (rc != CHRONOS_SERVER_SUCCESS) {
          server_error("Could not allocate data packet");
          goto failXit;
        }

        for (i=0; i<num_data_items; i++) {
          rc = benchmark_data_packet_append(reqPacketP->request_data.purchaseInfo[i].accountId,
                                            reqPacketP->request_data.purchaseInfo[i].symbolId,
                                            reqPacketP->request_data.purchaseInfo[i].symbol,
                                            reqPacketP->request_data.purchaseInfo[i].price,
                                            reqPacketP->request_data.purchaseInfo[i].amount,
                                            data_packetH);
          if (rc != CHRONOS_SERVER_SUCCESS) {
            server_error("Could not append data to packet");
            goto failXit;
          }
        }
      
        txn_rc = benchmark_purchase2(data_packetH,
                                     serverContextP->benchmarkCtxtP);

        rc = benchmark_data_packet_free(data_packetH);
        if (rc != CHRONOS_SERVER_SUCCESS) {
          server_error("Could not free data packet");
          goto failXit;
        }
        data_packetH = NULL;
        break;

      case CHRONOS_USER_TXN_SALE:
        rc = benchmark_data_packet_alloc(num_data_items, &data_packetH);
        if (rc != CHRONOS_SERVER_SUCCESS) {
          server_error("Could not allocate data packet");
          goto failXit;
        }

        for (i=0; i<num_data_items; i++) {
          rc = benchmark_data_packet_append(reqPacketP->request_data.purchaseInfo[i].accountId,
                                            reqPacketP->request_data.purchaseInfo[i].symbolId,
                                            reqPacketP->request_data.purchaseInfo[i].symbol,
                                            reqPacketP->request_data.purchaseInfo[i].price,
                                            reqPacketP->request_data.purchaseInfo[i].amount,
                                            data_packetH);
          if (rc != CHRONOS_SERVER_SUCCESS) {
            server_error("Could not append data to packet");
            goto failXit;
          }
        }
      
        txn_rc = benchmark_sell2(data_packetH,
                                 serverContextP->benchmarkCtxtP);

        rc = benchmark_data_packet_free(data_packetH);
        if (rc != CHRONOS_SERVER_SUCCESS) {
          server_error("Could not free data packet");
          goto failXit;
        }
        data_packetH = NULL;
        break;

      case CHRONOS_SYS_TXN_UPDATE_STOCK:
        memset(pkey_list, 0, sizeof(pkey_list));
        for (i=0; i<num_data_items; i++) {
          pkey_list[i] = reqPacketP->request_data.updateInfo[i].symbol;
          fvalues_list[i] = reqPacketP->request_data.updateInfo[i].price;
        }
      
        txn_rc = benchmark_refresh_quotes_list(num_data_items,
                                               pkey_list,
                                               fvalues_list,
                                               serverContextP->benchmarkCtxtP);
        break;

      default:
        assert(0);
      }

      rc = chronosRequestFree(requestH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to release request");
        goto cleanup;
      }

      num_total_txns ++;
      if (txn_rc == CHRONOS_SERVER_SUCCESS) {
        num_success ++;
      }
      else {
        num_failures ++;
      }

      RTCAPTURE();
      fprintf(stderr, "#### %ld\n", sample_response);
      server_info("----------------------------------------------");
      fprintf(stderr, "\n\n");
  }
    
  RTCAPTURE_PRINT(); 
  fprintf(stderr, "---------------------------\n");
  fprintf(stderr, "### Successful Xacts = %d\n", num_success);
  fprintf(stderr, "### Failed Xacts     = %d\n", num_failures);
  fprintf(stderr, "### Total Xacts      = %d\n", num_total_txns);
  fprintf(stderr, "---------------------------\n");

  goto cleanup; 

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static inline double
in_period_ds(chronosServerContext_t *contextP)
{
  double average_service_delay_ms = 0.0;
  double degree_timing_violation = 0.0;
  double smoth_degree_timing_violation = 0.0;
  long long prev_user_xacts_duration = 0;
  long long prev_user_xacts_history = 0;
  long long cur_user_xacts_duration = 0;
  long long cur_user_xacts_history = 0;

  prev_user_xacts_duration = last_user_xacts_duration_get(contextP->performanceStatsP);
  prev_user_xacts_history = last_user_xacts_history_get(contextP->performanceStatsP);

  cur_user_xacts_duration = period_user_xacts_duration_get(contextP->numServerThreads,
                                                           contextP->threadStatsArr);
  cur_user_xacts_history = period_user_xacts_count_get(contextP->numServerThreads,
                                                       contextP->threadStatsArr);

  average_service_delay_ms = pm_average_service_delay(cur_user_xacts_duration - prev_user_xacts_duration,
                                                      cur_user_xacts_history - prev_user_xacts_history);

  degree_timing_violation = pm_overload_degree(average_service_delay_ms, 
                                               contextP->desiredDelayBoundMS);

  return degree_timing_violation;
}


static void
aggregateStats(chronosServerContext_t *contextP)
{
  return aggregate_thread_stats(contextP->numServerThreads,
                                contextP->threadStatsArr,
                                contextP->performanceStatsP,
                                contextP->stats_fp);
}

static int
performanceMonitor(chronosServerContext_t *contextP)
{
  int rc = CHRONOS_SERVER_SUCCESS;
  double average_service_delay_ms = 0.0;
  double degree_timing_violation = 0.0;
  double smoth_degree_timing_violation = 0.0;
  long long user_xacts_duration = 0;
  long long user_xacts_history = 0;
  double desired_xact_period_ms = 0.0;

  user_xacts_duration = last_user_xacts_duration_get(contextP->performanceStatsP);
  user_xacts_history = last_user_xacts_history_get(contextP->performanceStatsP);

  average_service_delay_ms = pm_average_service_delay(user_xacts_duration, user_xacts_history);

  degree_timing_violation = pm_overload_degree(average_service_delay_ms, 
                                               contextP->desiredDelayBoundMS);

  smoth_degree_timing_violation = pm_smoothed_overload_degree(degree_timing_violation, 
                                                              contextP->smoth_degree_timing_violation, 
                                                              contextP->alpha);

  contextP->average_service_delay_ms = average_service_delay_ms;
  contextP->degree_timing_violation = degree_timing_violation;
  contextP->smoth_degree_timing_violation = smoth_degree_timing_violation;
  contextP->period_smoth_degree_timing_violation = 0;


  /* If the smoth degree of timing violation is greater than 0, it means that
   * the system is under overload. In that case, the system needs to apply
   * admission control.
   *
   * In our case, we apply a delay upon arrival of the transactions before
   * adding it to the queue. The delay is proportional to the degree of timing
   * violation.
   * */
  if (contextP->smoth_degree_timing_violation > 0.001) {
    contextP->inter_xact_sleep_ms += 1;
  }
  else if (contextP->inter_xact_sleep_ms > 1) {
    contextP->inter_xact_sleep_ms -= 1;
  }
  else {
    // Keep the current 
  }

  server_warning("RZ_DEBUG: smoth_degree_timing_violation: %.2lf, sleep_ms: %lld", contextP->smoth_degree_timing_violation, contextP->inter_xact_sleep_ms);

  return rc;
}

static int
adaptive_update_policy(chronosServerContext_t *contextP) 
{
  int rc = CHRONOS_SERVER_SUCCESS;

  if (IS_CHRONOS_MODE_FULL(contextP) || IS_CHRONOS_MODE_AUP(contextP)) {
    rc = chronos_aup_relax(contextP->aup_env, 
                           contextP->smoth_degree_timing_violation);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      goto failXit;
    }

#if 0
    (void) chronos_aup_data_set_dump(contextP->aup_env, 0, 30);
#endif
    chronos_aup_reset_all(contextP->aup_env);
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static void
qosManager(chronosServerContext_t *contextP)
{
  int enqueued_xacts = 0;
  int num_waits = 0;

#if 0
  enqueued_xacts = chronos_queue_size(contextP->sysTxnQueue) + 
                   chronos_queue_size(contextP->userTxnQueue);
#endif
  enqueued_xacts = chronos_queue_size(contextP->userTxnQueue);

  if (IS_CHRONOS_MODE_FULL(contextP) || IS_CHRONOS_MODE_AC(contextP)) {

    if (contextP->smoth_degree_timing_violation > 0.0) {
      num_waits = contextP->smoth_degree_timing_violation * enqueued_xacts;
      if (num_waits > enqueued_xacts) {
        num_waits = enqueued_xacts;
      }
    }

    chronos_ac_wait_set(num_waits, contextP->ac_env);

    fprintf(stderr, "---------------------------\n");
    fprintf(stderr, "### mode                          = %d\n", contextP->runningMode);
    fprintf(stderr, "### smoth_degree_timing_violation = %lf\n", contextP->smoth_degree_timing_violation);
    fprintf(stderr, "### num_enqueued_xacts            = %d\n", enqueued_xacts);
    fprintf(stderr, "### num_txns_to_wait              = %d\n", num_waits);
    fprintf(stderr, "---------------------------\n");
  }
  return;
}

static void
admission_control(chronosServerContext_t *contextP)
{
  struct timeval timeout;

  /* Version 1: Apply the original admission control approach */
#if 0
  (void) chronos_ac_wait(contextP->ac_env);
#endif

  /* Version 2: Apply sleep proportional to degree violation */
  if (contextP->inter_xact_sleep_ms > 0) {
    milliSleep(contextP->inter_xact_sleep_ms, timeout);
  }

  /* Version 3: Sleep until the degree violation is no longer 0 */
#if 0
  if (contextP->smoth_degree_timing_violation > 0 ) {
    while (contextP->period_smoth_degree_timing_violation > 0) {
      milliSleep(100, timeout);
    }
  }
#endif

  return;
}

static void
chronos_usage() 
{
  char template[] =
    "\n"
    "Starts up a chronos server. \n"
    "\n"
    "Usage: startup_server OPTIONS\n"
    "\n"
    "OPTIONS:\n"
    "-m|--server-mode [mode]         running mode: \n"
    "                                Valid options are:\n"
    "                                   "CHRONOS_SERVER_MODE_NAME_BASE"\n"
    "                                   "CHRONOS_SERVER_MODE_NAME_AC"\n"
    "                                   "CHRONOS_SERVER_MODE_NAME_AUP"\n"
    "                                   "CHRONOS_SERVER_MODE_NAME_AC_AUP"\n"
    "-c|--num-clients [num]          number of clients it can accept (default: "xstr(CHRONOS_NUM_CLIENT_THREADS)")\n"
    "-v [num]                        validity interval [in milliseconds] (default: "xstr(CHRONOS_INITIAL_VALIDITY_INTERVAL_MS)" ms)\n"
    "-u|--update-threads [num]       number of update threads (default: "xstr(CHRONOS_NUM_UPDATE_THREADS)")\n"
    "-t|--server-threads [num]       number of server threads (default: "xstr(CHRONOS_NUM_SERVER_THREADS)")\n"
    "-r|--experiment-duration [num]  duration of the experiment [in seconds] (default: "xstr(CHRONOS_EXPERIMENT_DURATION_SEC)" seconds)\n"
    "-p|--port [num]                 port to accept new connections (default: "xstr(CHRONOS_SERVER_PORT)")\n"
    "-d [num]                        debug level\n"
    "--evaluate-wcet                 evaluate transaction worst case execution time\n"
    "--xact-name <xact>              evalulate the indicated transaction\n"
    "                                Valid transactions are: \n"
    "                                   "CHRONOS_SERVER_XACT_NAME_VIEW_STOCK"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_VIEW_PORTFOLIO"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_PURCHASE"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_SELL"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_REFRESH_STOCK"\n"
    "--xact-size <num_items>         number of operations per transaction\n"
    "--print-samples                 print timing samples\n"
    "--delay-bound [num]             desired delay bound in ms\n"
    "--initial-load                  perform initial load\n"
    "-h|--help                       help";

  printf("%s\n", template);
}
