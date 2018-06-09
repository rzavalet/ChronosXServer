#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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

#include <chronos_packets.h>
#include <chronos_transactions.h>
#include <chronos_environment.h>
#include <benchmark.h>

#include "server_config.h"
#include "chronos_server.h"

#define CHRONOS_TCP_QUEUE   1024
static const char *program_name = "startup_server";

#define CHRONOS_SERVER_SUCCESS  (0)
#define CHRONOS_SERVER_FAIL     (1)

#define server_msg(_prefix, ...) \
  do {                     \
    char _local_buf_[256];             \
    snprintf(_local_buf_, sizeof(_local_buf_), __VA_ARGS__); \
    fprintf(stderr,"%s: %s: at %s:%d", _prefix,_local_buf_, __FILE__, __LINE__);   \
    fprintf(stderr,"\n");              \
  } while(0)

#define server_info(...) \
  server_msg("INFO", __VA_ARGS__)

#define server_error(...) \
  server_msg("ERROR", __VA_ARGS__)

#define server_warning(...) \
  server_msg("WARN", __VA_ARGS__)

#define server_debug(level,...) \
  do {                                                         \
    if (server_debug_level >= level) {                        \
      char _local_buf_[256];                                   \
      snprintf(_local_buf_, sizeof(_local_buf_), __VA_ARGS__); \
      fprintf(stderr, "DEBUG %s:%d: %s", __FILE__, __LINE__, _local_buf_);    \
      fprintf(stderr, "\n");     \
    } \
  } while(0)

int benchmark_debug_level = CHRONOS_DEBUG_LEVEL_MAX;
int server_debug_level = CHRONOS_DEBUG_LEVEL_MAX;

const char *chronosServerThreadNames[] ={
  "CHRONOS_SERVER_THREAD_LISTENER",
  "CHRONOS_SERVER_THREAD_UPDATE"
};

static int
processArguments(int argc, char *argv[], chronosServerContext_t *contextP);

static void
chronos_usage();

static void *
daListener(void *argP);

static void *
daHandler(void *argP);

static void *
updateThread(void *argP);

#if 0
static void *
processThread(void *argP);
#endif

static int
dispatchTableFn (chronosRequestPacket_t *reqPacketP, int *txn_rc, chronosServerThreadInfo_t *infoP);

static int
waitPeriod(double updatePeriodMS);

static int 
runTxnEvaluation(chronosServerContext_t *serverContextP);

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
static int
processUserTransaction(int *txn_rc,
                       chronosRequestPacket_t *reqPacketP,
                       chronosServerThreadInfo_t *infoP);
#endif

#if 0
static int
startExperimentTimer(chronosServerContext_t *serverContextP);

static int 
createExperimentTimer(chronosServerContext_t *serverContextP);
#endif

#ifdef CHRONOS_SAMPLING_ENABLED
static int
startSamplingTimer(chronosServerContext_t *serverContextP);

static int 
createSamplingTimer(chronosServerContext_t *serverContextP);
#endif

/*===================== TODO LIST ==============================
 * 1) Destroy mutexes --- DONE
 * 2) Add overload detection -- DONE
 * 3) Add Admision control -- DONE
 * 4) Add Adaptive update
 * 5) Improve usage of Berkeley DB API -- DONE
 *=============================================================*/
volatile int time_to_die = 0;
chronosServerContext_t *serverContextP = NULL;
chronosServerThreadInfo_t *listenerThreadInfoP = NULL;
chronosServerThreadInfo_t *processingThreadInfoArrP = NULL;
chronosServerThreadInfo_t *updateThreadInfoArrP = NULL;
chronos_queue_t *userTxnQueueP = NULL;
chronos_queue_t *sysTxnQueueP = NULL;

void handler_sigint(int sig);

void handler_sampling(void *arg);

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
  int rc;
  int i;
  int thread_num = 0;
  pthread_attr_t attr;
  int *thread_rc = NULL;
  const int stack_size = 0x100000; // 1 MB
  char **pkeys_list = NULL;
  int    num_pkeys = 0;
  chronos_time_t  system_start;
  unsigned long long initial_update_time_ms;

  serverContextP = malloc(sizeof(chronosServerContext_t));
  if (serverContextP == NULL) {
    server_error("Failed to allocate server context");
    goto failXit;
  }
  
  memset(serverContextP, 0, sizeof(chronosServerContext_t));

  /* Process command line arguments which include:
   *
   *    - # of client threads it can accept.
   *    - validity interval of temporal data
   *    - # of update threads
   *    - update period
   *
   */
  if (processArguments(argc, argv, serverContextP) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to process arguments");
    goto failXit;
  }

  if (serverContextP->runningMode == 4) {
    server_info("Running in transaction evaluation mode");
    if (runTxnEvaluation(serverContextP) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to run transaction evaluation mode");
      goto failXit;    
    }
     goto cleanup;
  }

  /* set the signal handler for sigint */
  if (signal(SIGINT, handler_sigint) == SIG_ERR) {
    server_error("Failed to set signal handler");
    goto failXit;    
  }  

#ifdef CHRONOS_SAMPLING_ENABLED
  /* Init the timer for sampling */
  if (createSamplingTimer(serverContextP) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to create timer");
    goto failXit;    
  }
#endif

#if 0
  /* Init the timer to check for experiment end */
  if (createExperimentTimer(serverContextP) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to create timer");
    goto failXit;    
  }
#endif

  userTxnQueueP = &(serverContextP->userTxnQueue);
  sysTxnQueueP = &(serverContextP->sysTxnQueue);

  serverContextP->magic = CHRONOS_SERVER_CTX_MAGIC;
  CHRONOS_SERVER_CTX_CHECK(serverContextP);
  
  if (pthread_mutex_init(&userTxnQueueP->mutex, NULL) != 0) {
    server_error("Failed to init mutex");
    goto failXit;
  }

  if (pthread_mutex_init(&sysTxnQueueP->mutex, NULL) != 0) {
    server_error("Failed to init mutex");
    goto failXit;
  }

  if (pthread_mutex_init(&serverContextP->startThreadsMutex, NULL) != 0) {
    server_error("Failed to init mutex");
    goto failXit;
  }

  if (pthread_cond_init(&sysTxnQueueP->more, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  if (pthread_cond_init(&sysTxnQueueP->less, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  if (pthread_cond_init(&userTxnQueueP->more, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  if (pthread_cond_init(&userTxnQueueP->less, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  if (pthread_cond_init(&serverContextP->startThreadsWait, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  if (serverContextP->initialLoad) {
    /* Create the system tables */
    if (benchmark_initial_load(program_name, CHRONOS_SERVER_HOME_DIR, CHRONOS_SERVER_DATAFILES_DIR) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to perform initial load");
      goto failXit;
    }

#if 0
    if (benchmark_handle_alloc(&serverContextP->benchmarkCtxtP, 
                               0, 
                               program_name,
                               CHRONOS_SERVER_HOME_DIR, 
                               CHRONOS_SERVER_DATAFILES_DIR) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to allocate handle");
      goto failXit;
    }
 
    /* Populate the porfolio tables */
    if (benchmark_load_portfolio(serverContextP->benchmarkCtxtP) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to load portfolios");
      goto failXit;
    }

    if (benchmark_handle_free(serverContextP->benchmarkCtxtP) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to free handle");
      goto failXit;
    }
#endif
  }
  else {
    server_info("*** Skipping initial load");
  }
  
  /* Obtain a benchmark handle */
  if (benchmark_handle_alloc(&serverContextP->benchmarkCtxtP, 
                             0, 
                             program_name,
                             CHRONOS_SERVER_HOME_DIR, 
                             CHRONOS_SERVER_DATAFILES_DIR) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to allocate handle");
    goto failXit;
  }
 
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

  /* Create data items list */
  rc = benchmark_stock_list_get(serverContextP->benchmarkCtxtP,
                                &pkeys_list,
                                &num_pkeys);
  if (rc != 0) {
    server_error("failed to get list of stocks");
    goto failXit;
  }

  serverContextP->dataItemsArray = calloc(num_pkeys, sizeof(chronosDataItem_t));
  if (serverContextP->dataItemsArray == NULL) {
    server_error("Could not allocate data items array");
    goto failXit;
  }
  serverContextP->szDataItemsArray = num_pkeys;

  CHRONOS_TIME_GET(system_start);
  initial_update_time_ms = CHRONOS_TIME_TO_MS(system_start);
  initial_update_time_ms += serverContextP->minUpdatePeriodMS;

  for (i=0; i<num_pkeys; i++) {
    serverContextP->dataItemsArray[i].index = i;
    serverContextP->dataItemsArray[i].dataItem = pkeys_list[i];
    serverContextP->dataItemsArray[i].updatePeriodMS[0] = serverContextP->minUpdatePeriodMS;
    serverContextP->dataItemsArray[i].nextUpdateTimeMS = initial_update_time_ms;
    server_debug(3, "%d next update time: %llu", i, initial_update_time_ms);
  }

#if 0
  /* Spawn processing thread */
  processingThreadInfoArrP = calloc(serverContextP->numServerThreads, sizeof(chronosServerThreadInfo_t));
  if (processingThreadInfoArrP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }

  for (i=0; i<serverContextP->numServerThreads; i++) {
    processingThreadInfoArrP[i].thread_type = CHRONOS_SERVER_THREAD_PROCESSING;
    processingThreadInfoArrP[i].contextP = serverContextP;
    processingThreadInfoArrP[i].thread_num = thread_num ++;
    processingThreadInfoArrP[i].magic = CHRONOS_SERVER_THREAD_MAGIC;
    
    rc = pthread_create(&processingThreadInfoArrP[i].thread_id,
                        &attr,
                        &processThread,
                        &(processingThreadInfoArrP[i]));
    if (rc != 0) {
      server_error("failed to spawn thread: %s", strerror(rc));
      goto failXit;
    }

    server_debug(2,"Spawed processing thread");
 } 
#endif

#ifdef CHRONOS_UPDATE_TRANSACTIONS_ENABLED
  /* Spawn the update threads */
  updateThreadInfoArrP = calloc(serverContextP->numUpdateThreads, sizeof(chronosServerThreadInfo_t));
  if (updateThreadInfoArrP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }

  for (i=0; i<serverContextP->numUpdateThreads; i++) {
    /* Set the generic data */
    updateThreadInfoArrP[i].thread_type = CHRONOS_SERVER_THREAD_UPDATE;
    updateThreadInfoArrP[i].contextP = serverContextP;
    updateThreadInfoArrP[i].thread_num = thread_num ++;
    updateThreadInfoArrP[i].magic = CHRONOS_SERVER_THREAD_MAGIC;

    /* Set the update specific data */
    updateThreadInfoArrP[i].first_symbol_id = i * serverContextP->numUpdatesPerUpdateThread;
    updateThreadInfoArrP[i].parameters.updateParameters.dataItemsArray = &(serverContextP->dataItemsArray[i * serverContextP->numUpdatesPerUpdateThread]);
    updateThreadInfoArrP[i].parameters.updateParameters.num_stocks = serverContextP->numUpdatesPerUpdateThread;
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

    server_debug(2,"Spawed update thread: %d", updateThreadInfoArrP[i].thread_num);
  }
#endif

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
  /* Spawn daListener thread */
  listenerThreadInfoP = malloc(sizeof(chronosServerThreadInfo_t));
  if (listenerThreadInfoP == NULL) {
    server_error("Failed to allocate thread structure");
    goto failXit;
  }
  
  memset(listenerThreadInfoP, 0, sizeof(chronosServerThreadInfo_t));
  listenerThreadInfoP->thread_type = CHRONOS_SERVER_THREAD_LISTENER;
  listenerThreadInfoP->contextP = serverContextP;
  listenerThreadInfoP->thread_num = thread_num ++;
  listenerThreadInfoP->magic = CHRONOS_SERVER_THREAD_MAGIC;
    
  rc = pthread_create(&listenerThreadInfoP->thread_id,
                      &attr,
                      &daListener,
                      listenerThreadInfoP);
  if (rc != 0) {
    server_error("failed to spawn thread: %s", strerror(rc));
    goto failXit;
  }

  server_debug(2,"Spawed listener thread");
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
  
#if 0
  for (i=0; i<serverContextP->numServerThreads; i++) {
    rc = pthread_join(processingThreadInfoArrP[i].thread_id, (void **)&thread_rc);
    if (rc != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed while joining thread %s", CHRONOS_SERVER_THREAD_NAME(processingThreadInfoArrP[i].thread_type));
    }
  }
#endif

#ifdef CHRONOS_PRINT_STATS
  printStats(infoP);
#endif

  rc = CHRONOS_SERVER_SUCCESS;
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;
  
cleanup:

  if (userTxnQueueP) {
    pthread_cond_destroy(&userTxnQueueP->more);
    pthread_cond_destroy(&userTxnQueueP->less);
    pthread_mutex_destroy(&userTxnQueueP->mutex);
  }

  if (sysTxnQueueP) {
    pthread_cond_destroy(&sysTxnQueueP->more);
    pthread_cond_destroy(&sysTxnQueueP->less);
    pthread_mutex_destroy(&sysTxnQueueP->mutex);
  }
  
  if (serverContextP) {
    pthread_cond_destroy(&serverContextP->startThreadsWait);
    pthread_mutex_destroy(&serverContextP->startThreadsMutex);

    if (serverContextP->dataItemsArray) {
      free(serverContextP->dataItemsArray);
    }

    if (benchmark_handle_free(serverContextP->benchmarkCtxtP) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to free handle");
      goto failXit;
    }
  }

  return rc;
}

void handler_sigint(int sig)
{
  printf("Received signal: %d\n", sig);
  time_to_die = 1;
}

void handler_experiment_finish(void *arg)
{
  server_info("**** CHRONOS EXPERIMENT FINISHING ****");
  time_to_die = 1;
}

int isTimeToDie()
{
  return time_to_die;
}

void handler_sampling(void *arg)
{
  chronosServerContext_t *contextP = (chronosServerContext_t *) arg;
  int previousSlot;
  int newSlot;
  int i;
  int    total_failed_txns = 0;
  int    total_timely_txns = 0;
  double count = 0;
  double duration_ms = 0;

  chronosServerStats_t *statsP = NULL;
  chronosDataItem_t    *dataItem = NULL;

  server_info("****** TIMER... *****");
  CHRONOS_SERVER_CTX_CHECK(contextP);


  previousSlot = contextP->currentSlot;
  newSlot = (previousSlot + 1) % CHRONOS_SAMPLING_SPACE; 
  for (i=0; i<CHRONOS_MAX_NUM_SERVER_THREADS; i++) {
    statsP = &(contextP->stats_matrix[newSlot][i]);
    memset(statsP, 0, sizeof(*statsP));
  }

  /*=============== Obtain AUR =======================*/
  for (i=0; i<contextP->szDataItemsArray; i++) {
    dataItem = &(contextP->dataItemsArray[i]);
    
    if (dataItem->updateFrequency[previousSlot] == 0) {
      dataItem->updateFrequency[previousSlot] = 0.1;
    }

    dataItem->accessUpdateRatio[previousSlot] = dataItem->accessUpdateRatio[previousSlot] / dataItem->updateFrequency[previousSlot];

    if (IS_CHRONOS_MODE_FULL(contextP) || IS_CHRONOS_MODE_AUP(contextP)) {
      // Data is Cold: Relax the update period
      if (dataItem->accessUpdateRatio[previousSlot] < 1) {
        server_warning("### [AUP] Data Item: %d is cold", i);
        if (dataItem->updatePeriodMS[previousSlot] * 1.1 <= contextP->maxUpdatePeriodMS) {
          contextP->dataItemsArray[i].updatePeriodMS[newSlot] = dataItem->updatePeriodMS[previousSlot] * 1.1; 
          server_warning("### [AUP] Data Item: %d, changed update period", i);
        }
      }
      // Data is hot: Update more frequently
      else if (dataItem->accessUpdateRatio[previousSlot] > 1) {
        server_warning("### [AUP] Data Item: %d is hot", i);
        if (dataItem->updatePeriodMS[previousSlot] * 0.9 >= contextP->minUpdatePeriodMS) {
          contextP->dataItemsArray[i].updatePeriodMS[newSlot] = dataItem->updatePeriodMS[previousSlot] * 0.9;
          server_warning("### [AUP] Data Item: %d, changed update period", i);
        }
      }
    }
    else {
      contextP->dataItemsArray[i].updatePeriodMS[newSlot] = contextP->minUpdatePeriodMS;
    }

  }
  /*==================================================*/

  contextP->currentSlot = newSlot;


  /*======= Obtain average of the last period ========*/
  for (i=0; i<CHRONOS_MAX_NUM_SERVER_THREADS; i++) {
    statsP = &(contextP->stats_matrix[previousSlot][i]);
    count += statsP->num_txns;
    duration_ms += statsP->cumulative_time_ms;
    total_failed_txns += statsP->num_failed_txns;
    total_timely_txns += statsP->num_timely_txns;
  }
  if (count > 0) {
    contextP->average_service_delay_ms = duration_ms / count;
  }
  else {
    contextP->average_service_delay_ms = 0;
  }
  /*==================================================*/


  /*======= Obtain Overload Degree ========*/
  if (contextP->average_service_delay_ms > contextP->desiredDelayBoundMS) {
    contextP->degree_timing_violation = (contextP->average_service_delay_ms - contextP->desiredDelayBoundMS) / contextP->desiredDelayBoundMS;
  }
  else {
    contextP->degree_timing_violation = 0;
  }
  contextP->smoth_degree_timing_violation = contextP->alpha * contextP->degree_timing_violation
                                            + (1.0 - contextP->alpha) * contextP->smoth_degree_timing_violation;
  contextP->total_txns_enqueued = contextP->userTxnQueue.occupied + contextP->sysTxnQueue.occupied;
  if ((IS_CHRONOS_MODE_FULL(contextP) || IS_CHRONOS_MODE_AC(contextP))
       && contextP->smoth_degree_timing_violation > 0) 
  {
    contextP->num_txn_to_wait = contextP->total_txns_enqueued * contextP->smoth_degree_timing_violation / 100.0;
  }
  else {
    contextP->num_txn_to_wait = 0;
  }
  /*==================================================*/


  server_info("SAMPLING [ACC_DURATION_MS: %lf], [NUM_TXN: %d], [AVG_DURATION_MS: %.3lf] [NUM_FAILED_TXNS: %d], "
               "[NUM_TIMELY_TXNS: %d] [DELTA(k): %.3lf], [DELTA_S(k): %.3lf] [TNX_ENQUEUED: %d] [TXN_TO_WAIT: %d]", 
               duration_ms, (int)count, contextP->average_service_delay_ms, total_failed_txns, total_timely_txns,
               contextP->degree_timing_violation,
               contextP->smoth_degree_timing_violation,
               contextP->total_txns_enqueued,
               contextP->num_txn_to_wait);

  return;
}

#if 0
static int
ThreadTraceOpen(int thread_num, chronosServerThreadInfo_t *infoP)
{
  int  rc = CHRONOS_SERVER_SUCCESS;
  char file_name[256];

  snprintf(file_name, sizeof(file_name), "/tmp/trace_%d.tr", thread_num);

  infoP->trace_file = fopen(file_name, "w+");
  if (infoP->trace_file == NULL) {
    server_error("Failed to open trace file: %s", file_name);
    goto failXit;
  }
  
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int
ThreadTraceClose(chronosServerThreadInfo_t *infoP)
{
  int  rc = CHRONOS_SERVER_SUCCESS;

  if (infoP->trace_file == NULL) {
    server_error("Invalid trace file pointer");
    goto failXit;
  }

  fflush(infoP->trace_file);

  rc = fclose(infoP->trace_file);
  if (rc != 0) {
    server_error("Failed to close trace file");
    goto failXit;
  }
 
  infoP->trace_file = NULL;
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int
ThreadTraceTxnElapsedTimePrint(const chronos_time_t *enqueue,
                               const chronos_time_t *start,
                               const chronos_time_t *end,
                               int   txn_type,
                               chronosServerThreadInfo_t *infoP)
{
  int  rc = CHRONOS_SERVER_SUCCESS;
  long long enqueue_ms;
  long long start_ms;
  long long end_ms;
  long long txn_duration_ms;
  long long txn_execution_ms;
  chronos_time_t txn_duration;
  chronos_time_t txn_execution;
#define CHRONOS_THREAD_TRACE_FMT  "[THR: %d] [TYPE: %s] [ENQUEUE: %lld] [START: %lld] [END: %lld] [EXECUTION: %lld] [DURATION: %lld]\n"

  if (infoP->trace_file == NULL) {
    server_error("Invalid trace file pointer");
    goto failXit;
  }

  if (start == NULL || end == NULL) {
    server_error("Bad arguments provided");
    goto failXit;
  }

  CHRONOS_TIME_NANO_OFFSET_GET(*enqueue, *end, txn_duration);
  txn_duration_ms = CHRONOS_TIME_TO_MS(txn_duration);

  CHRONOS_TIME_NANO_OFFSET_GET(*start, *end, txn_execution);
  txn_execution_ms = CHRONOS_TIME_TO_MS(txn_execution);

  enqueue_ms = CHRONOS_TIME_TO_MS(*enqueue);
  start_ms = CHRONOS_TIME_TO_MS(*start);
  end_ms = CHRONOS_TIME_TO_MS(*end);

  fprintf(infoP->trace_file,
          CHRONOS_THREAD_TRACE_FMT,
          infoP->thread_num,
          txn_type == -1 ? chronos_system_transaction_str[0] : CHRONOS_TXN_NAME(txn_type),
          enqueue_ms,
          start_ms,
          end_ms,
          txn_execution_ms,
          txn_duration_ms);

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}
#endif


static int
initProcessArguments(chronosServerContext_t *contextP)
{
  contextP->numServerThreads = CHRONOS_NUM_SERVER_THREADS;
  contextP->numClientsThreads = CHRONOS_NUM_CLIENT_THREADS;
  contextP->numUpdateThreads = CHRONOS_NUM_UPDATE_THREADS;
  contextP->numUpdatesPerUpdateThread = CHRONOS_NUM_STOCK_UPDATES_PER_UPDATE_THREAD;
  contextP->serverPort = CHRONOS_SERVER_PORT;
  contextP->initialValidityIntervalMS = CHRONOS_INITIAL_VALIDITY_INTERVAL_MS;
  contextP->samplingPeriodSec = CHRONOS_SAMPLING_PERIOD_SEC;
  contextP->duration_sec = CHRONOS_EXPERIMENT_DURATION_SEC;
  contextP->desiredDelayBoundMS = CHRONOS_DESIRED_DELAY_BOUND_MS;
  contextP->alpha = CHRONOS_ALPHA;
  contextP->initialLoad = 1;

  contextP->timeToDieFp = isTimeToDie;

  contextP->debugLevel = CHRONOS_DEBUG_LEVEL_MIN;

  return 0;
}


/*
 * Process the command line arguments
 */
static int
processArguments(int argc, char *argv[], chronosServerContext_t *contextP) 
{
  int c;

  if (contextP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  memset(contextP, 0, sizeof(*contextP));
  (void) initProcessArguments(contextP);

  while ((c = getopt(argc, argv, "m:c:v:s:u:r:p:d:nh")) != -1) {
    switch(c) {
      case 'm':
        contextP->runningMode = atoi(optarg);
        server_debug(2,"*** Running mode: %d", contextP->runningMode);
        break;
      
      case 'c':
        contextP->numClientsThreads = atoi(optarg);
        server_debug(2,"*** Num clients: %d", contextP->numClientsThreads);
        break;
      
      case 'v':
        contextP->initialValidityIntervalMS = atof(optarg);
        server_debug(2, "*** Validity interval: %lf", contextP->initialValidityIntervalMS);
        break;

      case 's':
        contextP->samplingPeriodSec = atof(optarg);
        server_debug(2, "*** Sampling period: %lf [secs]", contextP->samplingPeriodSec);
        break;

      case 'u':
        contextP->numUpdateThreads = atoi(optarg);
        server_debug(2, "*** Num update threads: %d", contextP->numUpdateThreads);
        break;

      case 'r':
        contextP->duration_sec = atof(optarg);
        server_debug(2, "*** Duration: %lf", contextP->duration_sec);
        break;

      case 'p':
        contextP->serverPort = atoi(optarg);
        server_debug(2, "*** Server port: %d", contextP->serverPort);
        break;

      case 'd':
        contextP->debugLevel = atoi(optarg);
        server_debug(2, "*** Debug Level: %d", contextP->debugLevel);
        break;

      case 'n':
        contextP->initialLoad = 0;
        server_debug(2, "*** Do not perform initial load");
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

  contextP->minUpdatePeriodMS = 0.5 * contextP->initialValidityIntervalMS;
  contextP->maxUpdatePeriodMS = 0.5 * CHRONOS_UPDATE_PERIOD_RELAXATION_BOUND * contextP->initialValidityIntervalMS;
  contextP->updatePeriodMS  =  0.5 * contextP->initialValidityIntervalMS;


  return CHRONOS_SERVER_SUCCESS;

failXit:
  chronos_usage();
  return CHRONOS_SERVER_FAIL;
}

static int
dispatchTableFn (chronosRequestPacket_t *reqPacketP, int *txn_rc_ret, chronosServerThreadInfo_t *infoP)
{
  if (infoP == NULL || infoP->contextP == NULL || txn_rc_ret == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  /*===========================================
   * Put a new transaction in the txn queue
   *==========================================*/
  server_debug(2, "Processing transaction: %s", CHRONOS_TXN_NAME(reqPacketP->txn_type));

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
  processUserTransaction(txn_rc_ret, reqPacketP, infoP);
#endif
  
  server_debug(2, "Done processing transaction: %s, rc: %d", CHRONOS_TXN_NAME(reqPacketP->txn_type), *txn_rc_ret);

  return CHRONOS_SERVER_SUCCESS;

failXit:
  return CHRONOS_SERVER_FAIL;
}

/*
 * Starting point for a handlerThread.
 * handle a transaction request
 */
static void *
daHandler(void *argP) 
{
  int num_bytes;
  int cnt_msg = 0;
  int need_admission_control = 0;
  int written, to_write;
  int txn_rc = 0;
  chronosResponsePacket_t resPacket;
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  chronosRequestPacket_t reqPacket;

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  /*------------ Read the request -----------------*/
  server_debug(3, "waiting new request");

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
  server_debug(3, "Received transaction request: %s", CHRONOS_TXN_NAME(reqPacket.txn_type));
  /*-----------------------------------------------*/


  /*----------- do admission control ---------------*/
  need_admission_control = infoP->contextP->num_txn_to_wait > 0 ? 1 : 0;
  cnt_msg = 0;
#define MSG_FREQ 10
  while (infoP->contextP->num_txn_to_wait > 0)
  {
    if (cnt_msg >= MSG_FREQ) {
      server_warning("### [AC] Doing admission control (%d/%d) ###",
                   infoP->contextP->num_txn_to_wait,
                   infoP->contextP->total_txns_enqueued);
    }
    cnt_msg = (cnt_msg + 1) % MSG_FREQ;
    if (time_to_die == 1) {
      server_info("Requested to die");
      goto cleanup;
    }

    (void) sched_yield();
  }
  if (need_admission_control) {
    server_warning("### [AC] Done with admission control (%d/%d) ###",
                 infoP->contextP->num_txn_to_wait,
                 infoP->contextP->total_txns_enqueued);
  }
  /*-----------------------------------------------*/



  /*----------- Process the request ----------------*/
  if (dispatchTableFn(&reqPacket, &txn_rc, infoP) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to handle request");
    goto cleanup;
  }
  /*-----------------------------------------------*/


  /*---------- Reply to the request ---------------*/
  server_debug(3, "Replying to client");

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
  server_debug(3, "Replied to client");
  /*-----------------------------------------------*/


  if (time_to_die == 1) {
    server_info("Requested to die");
    goto cleanup;
  }

cleanup:

  close(infoP->socket_fd);

  free(infoP);

  server_info("daHandler exiting");
  pthread_exit(NULL);
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
  time_t current_time;
  time_t next_sample_time;

  socklen_t client_address_len;
  pthread_attr_t attr;
  const int stack_size = 0x100000; // 1 MB
  chronosServerThreadInfo_t *handlerInfoP = NULL;

  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  server_debug(1, "Starting listener thread...");

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

  server_debug(4, "Waiting for incoming connections...");

  current_time = time(NULL);
  next_sample_time = current_time + infoP->contextP->duration_sec;

  /* Keep listening for incoming connections till we
   * complete all threads for the experiment
   */
  while(!time_to_die) {

    server_debug(4, "Polling for new connection");
    
    /* wait for a new connection */
    rc = poll(fds, 1, 1000 /* one second */);
    if (rc < 0) {
      perror("poll() failed");
      goto cleanup;
    }
    else if (rc == 0) {
      server_debug(4, "poll() timed out");
      continue;
    }
    else {
      server_debug(4, "%d descriptors are ready", rc);
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

      // TODO: What would be needed to use a thread pool instead?
      handlerInfoP->socket_fd = accepted_socket_fd;
      handlerInfoP->contextP = infoP->contextP;
      handlerInfoP->state = CHRONOS_SERVER_THREAD_STATE_RUN;
      handlerInfoP->magic = CHRONOS_SERVER_THREAD_MAGIC;

      rc = pthread_create(&handlerInfoP->thread_id,
                          &attr,
                          &daHandler,
                          handlerInfoP);
      if (rc != 0) {
        server_error("failed to spawn thread");
        goto cleanup;
      }

      server_debug(2,"Spawed handler thread");

    } while (accepted_socket_fd != -1);

    current_time = time(NULL);
    if (current_time >= next_sample_time) {
      time_to_die = 1;
      server_info("**** CHRONOS EXPERIMENT FINISHING ****");
    }
  }

  if (time_to_die == 1) {
    server_info("Requested to die");
    goto cleanup;
  }

#ifdef CHRONOS_SAMPLING_ENABLED
  if (startSamplingTimer(infoP->contextP) != CHRONOS_SERVER_SUCCESS) {
    goto cleanup;
  }
#endif

#if 0
  if (startExperimentTimer(infoP->contextP) != CHRONOS_SERVER_SUCCESS) {
    goto cleanup;
  }
#endif

cleanup:
  server_info("daListener exiting");
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
processUserTransaction(int *txn_rc,
                       chronosRequestPacket_t *reqPacketP,
                       chronosServerThreadInfo_t *infoP)
{
  int               rc = CHRONOS_SERVER_SUCCESS;
  int               i;
  int               num_data_items = 0;
#ifdef CHRONOS_SAMPLING_ENABLED
  int               thread_num;
  int               current_slot = 0;
  long long         txn_duration_ms;
  chronos_time_t    txn_duration;
  chronosServerStats_t  *statsP = NULL;
#endif
  chronos_time_t    txn_begin;
  chronos_time_t    txn_end;
  const char        *pkey_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  benchmark_xact_data_t data[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  chronosUserTransaction_t txn_type;
  BENCHMARK_DATA_PACKET_H data_packetH = NULL;

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

    txn_type = reqPacketP->txn_type;
    num_data_items = reqPacketP->numItems;
    
    CHRONOS_TIME_GET(txn_begin);

    /* dispatch a transaction */
    switch(txn_type) {

    case CHRONOS_USER_TXN_VIEW_STOCK:
      for (i=0; i<num_data_items; i++) {
        pkey_list[i] = reqPacketP->request_data.symbolInfo[i].symbol;
      }
      *txn_rc = benchmark_view_stock2(num_data_items, pkey_list, infoP->contextP->benchmarkCtxtP);
      break;

    case CHRONOS_USER_TXN_VIEW_PORTFOLIO:
      for (i=0; i<num_data_items; i++) {
        pkey_list[i] = reqPacketP->request_data.portfolioInfo[i].accountId;
      }
      *txn_rc = benchmark_view_portfolio2(num_data_items, pkey_list, infoP->contextP->benchmarkCtxtP);
      break;

    case CHRONOS_USER_TXN_PURCHASE:
      rc = benchmark_data_packet_alloc(num_data_items, data_packetH);
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
    
      *txn_rc = benchmark_purchase2(num_data_items, 
                                   data,
                                   infoP->contextP->benchmarkCtxtP);

      rc = benchmark_data_packet_free(data_packetH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Could not free data packet");
        goto failXit;
      }
      data_packetH = NULL;

      break;

    case CHRONOS_USER_TXN_SALE:
      rc = benchmark_data_packet_alloc(num_data_items, data_packetH);
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
    
      *txn_rc = benchmark_sell2(num_data_items, 
                               data, 
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
    }

    server_info("Txn rc: %d", *txn_rc);

    if (infoP->contextP->num_txn_to_wait > 0) {
      server_warning("### [AC] Need to wait for: %d/%d transactions to finish ###",
                    infoP->contextP->num_txn_to_wait, 
                    infoP->contextP->total_txns_enqueued);
      infoP->contextP->num_txn_to_wait --;
    }

    /*--------------------------------------------*/


    CHRONOS_TIME_GET(txn_end);
   
#if 0
    ThreadTraceTxnElapsedTimePrint(&txn_enqueue, &txn_begin, &txn_end, txn_type, infoP);
#endif

#ifdef CHRONOS_SAMPLING_ENABLED
    thread_num = infoP->thread_num;
    current_slot = infoP->contextP->currentSlot;
    statsP = &(infoP->contextP->stats_matrix[current_slot][thread_num]);

    if (*txn_rc == CHRONOS_SERVER_SUCCESS) {
      /* One more transasction finished */
      statsP->num_txns ++;

      CHRONOS_TIME_NANO_OFFSET_GET(txn_begin, txn_end, txn_duration);
      txn_duration_ms = CHRONOS_TIME_TO_MS(txn_duration);
      statsP->cumulative_time_ms += txn_duration_ms;

      if (txn_duration_ms <= infoP->contextP->desiredDelayBoundMS) {
        statsP->num_timely_txns ++;
      }
      server_info("User transaction succeeded");
    }
    else {
      statsP->num_failed_txns ++;
      server_error("User transaction failed");
    }
#endif
    server_info("Done processing user txn...");
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

static int
processRefreshTransaction(chronosRequestPacket_t *requestP,
                          chronosServerThreadInfo_t *infoP)
{
  int               rc = CHRONOS_SERVER_SUCCESS;
  const char       *pkey = NULL;
  chronosServerContext_t *contextP = NULL;
  chronos_time_t    txn_begin;
  chronos_time_t    txn_end;

  if (infoP == NULL || infoP->contextP == NULL || requestP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  contextP = infoP->contextP;

  server_info("(thr: %d) Processing update...", infoP->thread_num);

  pkey = requestP->request_data.symbolInfo[0].symbol;
  assert(pkey != NULL);
  server_debug(3, "Updating value for pkey: %s...", pkey);

  CHRONOS_TIME_GET(txn_begin);
  if (benchmark_refresh_quotes2(contextP->benchmarkCtxtP, pkey, -1 /*Update randomly*/) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to refresh quotes");
    goto failXit;
  }
  CHRONOS_TIME_GET(txn_end);

  server_info("(thr: %d) Done processing update...", infoP->thread_num);
  if (infoP->contextP->num_txn_to_wait > 0) {
    server_warning("### [AC] Need to wait for: %d/%d transactions to finish ###",
                 contextP->num_txn_to_wait, 
                 contextP->total_txns_enqueued);
    infoP->contextP->num_txn_to_wait --;
  }

#if 0
  ThreadTraceTxnElapsedTimePrint(&txn_enqueue, &txn_begin, &txn_end, -1, infoP);
#endif

#if 0
  int               i;
  int               data_item = 0;
  int               current_slot = 0;
  chronos_queue_t  *sysTxnQueueP = NULL;
  chronosUserTransaction_t txn_type;
  txn_info_t       *txnInfoP     = NULL;
  sysTxnQueueP = &(infoP->contextP->sysTxnQueue);

  if (sysTxnQueueP->occupied > 0) {
    server_info("Processing update...");
    
    current_slot = infoP->contextP->currentSlot;
    txnInfoP = dequeueTransaction(sysTxnQueueP);
    txn_type = txnInfoP->txn_type;
    assert(txn_type == CHRONOS_USER_TXN_VIEW_STOCK);

    if (IS_CHRONOS_MODE_BASE(infoP->contextP) || IS_CHRONOS_MODE_AC(infoP->contextP)) {
      for (i=0; i<BENCHMARK_NUM_SYMBOLS; i++){
        data_item = i;
        if (benchmark_refresh_quotes(infoP->contextP->benchmarkCtxtP, &data_item, 0) != CHRONOS_SERVER_SUCCESS) {
          server_error("Failed to refresh quotes");
          goto failXit;
        }

        if (data_item >= 0) {
          infoP->contextP->UpdateFrequency[current_slot][data_item] ++;
        }
      }
    }
    else {
      for (i=0; i<BENCHMARK_NUM_SYMBOLS; i++){
        if (infoP->contextP->AccessUpdateRatio[current_slot][i] >= 1) {
          data_item = i;
          if (benchmark_refresh_quotes(infoP->contextP->benchmarkCtxtP, &data_item, 0) != CHRONOS_SERVER_SUCCESS) {
            server_error("Failed to refresh quotes");
            goto failXit;
          }

          if (data_item >= 0) {
            infoP->contextP->UpdateFrequency[current_slot][data_item] ++;
          }
        }
      }
    }
    
    infoP->contextP->txn_update[current_slot] += 1;

#endif
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

#if 0
/*
 * This is the driver function of process thread
 */
static void *
processThread(void *argP) 
{
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  
  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  ThreadTraceOpen(infoP->thread_num, infoP);

  /* Give update transactions more priority */
  /* TODO: Add scheduling technique */
  while (!time_to_die) {

    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

#ifdef CHRONOS_UPDATE_TRANSACTIONS_ENABLED
    if (infoP->contextP->sysTxnQueue.occupied > 0) {
      /*-------- Process refresh transaction ----------*/
      if (processRefreshTransaction(infoP) != CHRONOS_SERVER_SUCCESS) {
        server_info("Failed to execute refresh transactions");
      }
      /*--------------------------------------------*/
    }
#endif

    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);
    
#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
    if (infoP->contextP->userTxnQueue.occupied > 0) {
      /*-------- Process user transaction ----------*/
      if (processUserTransaction(infoP) != CHRONOS_SERVER_SUCCESS) {
        server_info("Failed to execute refresh transactions");
      }
      /*--------------------------------------------*/
    }
#endif

  }
  
cleanup:
  ThreadTraceClose(infoP);

  server_info("processThread exiting");
  
  pthread_exit(NULL);
}
#endif

/*
 * This is the driver function of an update thread
 */
static void *
updateThread(void *argP) 
{
  int    i;
  int    num_updates = 0;
  chronosDataItem_t *dataItemArray =  NULL;
  volatile int current_slot;
  chronos_time_t   current_time;
  unsigned long long current_time_ms;
  chronos_time_t   next_update_time;
  unsigned long long next_update_time_ms;
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  num_updates = infoP->parameters.updateParameters.num_stocks;
  assert(num_updates > 0);

  dataItemArray = infoP->parameters.updateParameters.dataItemsArray;
  assert(dataItemArray != NULL);

  while (1) {
    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);


    CHRONOS_TIME_GET(current_time);
    current_time_ms = CHRONOS_TIME_TO_MS(current_time);

    for (i=0; i<num_updates; i++) {

      if (dataItemArray[i].nextUpdateTimeMS <= current_time_ms) {
        int   index = dataItemArray[i].index;
        char *pkey = dataItemArray[i].dataItem;
        chronosRequestPacket_t request;
        request.txn_type = CHRONOS_USER_TXN_MAX; /* represents sys xact */
        request.numItems = 1;
        strncpy(request.request_data.symbolInfo[0].symbol, pkey, sizeof(request.request_data.symbolInfo[0].symbol));
        server_debug(3, "(thr: %d) (%llu <= %llu) [%d] Enqueuing update for key: %d, %s",
                      infoP->thread_num, 
                      dataItemArray[i].nextUpdateTimeMS,
                      current_time_ms,
                      i,
                      index,
                      pkey);

        processRefreshTransaction(&request, infoP);

        current_slot = infoP->contextP->currentSlot;
        dataItemArray[i].updateFrequency[current_slot]++;

        CHRONOS_TIME_GET(next_update_time);
        next_update_time_ms = CHRONOS_TIME_TO_MS(next_update_time);
        dataItemArray[i].nextUpdateTimeMS = next_update_time_ms + dataItemArray[i].updatePeriodMS[current_slot];
        server_debug(3, "(thr: %d) %d next update time: %llu",
                          infoP->thread_num, i, dataItemArray[i].nextUpdateTimeMS);
      }
    }

    if (waitPeriod(100) != CHRONOS_SERVER_SUCCESS) {
      goto cleanup;
    }

    if (time_to_die == 1) {
      server_info("Requested to die");
      goto cleanup;
    }
  }

cleanup:
  server_info("updateThread exiting");
  pthread_exit(NULL);
}

#ifdef CHRONOS_SAMPLING_ENABLED
static int
startSamplingTimer(chronosServerContext_t *serverContextP)
{
  int  rc = CHRONOS_SERVER_SUCCESS;

  server_info("Starting sampling timer %p...",
               serverContextP->sampling_timer_id);

  if (timer_settime(serverContextP->sampling_timer_id, 0, 
                    &serverContextP->sampling_timer_et, NULL) != 0) {
    server_error("Failed to start sampling timer");
    goto failXit;
  }
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int 
createSamplingTimer(chronosServerContext_t *serverContextP)
{
  int rc = CHRONOS_SERVER_SUCCESS;

  serverContextP->sampling_timer_ev.sigev_notify = SIGEV_THREAD;
  serverContextP->sampling_timer_ev.sigev_value.sival_ptr = serverContextP;
  serverContextP->sampling_timer_ev.sigev_notify_function = (void *)handler_sampling;
  serverContextP->sampling_timer_ev.sigev_notify_attributes = NULL;

  serverContextP->sampling_timer_et.it_interval.tv_sec = serverContextP->samplingPeriodSec;
  serverContextP->sampling_timer_et.it_interval.tv_nsec = 0;  
  serverContextP->sampling_timer_et.it_value.tv_sec = serverContextP->samplingPeriodSec;
  serverContextP->sampling_timer_et.it_value.tv_nsec = 0;

  server_info("Creating sampling timer. Period: %ld",
              serverContextP->sampling_timer_et.it_interval.tv_sec);

  if (timer_create(CLOCK_REALTIME, 
                   &serverContextP->sampling_timer_ev, 
                   &serverContextP->sampling_timer_id) != 0) {
    server_error("Failed to create sampling timer");
    goto failXit;
  }

  server_info("Done creating sampling timer: %p",
              serverContextP->sampling_timer_id);
  goto cleanup; 

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}
#endif

static int 
runTxnEvaluation(chronosServerContext_t *serverContextP)
{
  int rc = CHRONOS_SERVER_SUCCESS;
  int i, j;
  int txn_type;
  int txn_rc = 0;
  int num_data_items;
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
    if (benchmark_initial_load(program_name, CHRONOS_SERVER_HOME_DIR, CHRONOS_SERVER_DATAFILES_DIR) != CHRONOS_SERVER_SUCCESS) {
      server_error("Failed to perform initial load");
      goto failXit;
    }
  }
  else {
    server_info("*** Skipping initial load");
  }
  
  /* Obtain a benchmark handle */
  if (benchmark_handle_alloc(&serverContextP->benchmarkCtxtP, 
                             0, 
                             program_name,
                             CHRONOS_SERVER_HOME_DIR, 
                             CHRONOS_SERVER_DATAFILES_DIR) != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to allocate handle");
    goto failXit;
  }
  
  for (txn_type = 0; txn_type < CHRONOS_USER_TXN_MAX; txn_type++) {
    for (j = 0; j < 1000; ++ j) {

      server_info("(%d) Evaluating: %s", 
                  txn_type * 1000 + j, chronos_user_transaction_str[txn_type]);

      requestH = chronosRequestCreate(txn_type, clientCacheH, chronosEnvH);
      if (requestH == NULL) {
        server_error("Failed to populate request");
        goto cleanup;
      }

      reqPacketP = (chronosRequestPacket_t *) requestH;
      num_data_items = reqPacketP->numItems;

      switch(txn_type) {

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

      default:
        assert(0);
      }

      rc = chronosRequestFree(requestH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to release request");
        goto cleanup;
      }

    }
  }
    
  goto cleanup; 

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

#if 0
static int
startExperimentTimer(chronosServerContext_t *serverContextP)
{
  int  rc = CHRONOS_SERVER_SUCCESS;

  server_info("Starting experiment timer %p...",
               serverContextP->experiment_timer_id);

  if (timer_settime(serverContextP->experiment_timer_id, 0, 
                    &serverContextP->experiment_timer_et, NULL) != 0) {
    server_error("Failed to start experiment timer");
    goto failXit;
  }
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int 
createExperimentTimer(chronosServerContext_t *serverContextP)
{
  int rc = CHRONOS_SERVER_SUCCESS;

  serverContextP->experiment_timer_ev.sigev_notify = SIGEV_THREAD;
  serverContextP->experiment_timer_ev.sigev_value.sival_ptr = serverContextP;
  serverContextP->experiment_timer_ev.sigev_notify_function = (void *)handler_experiment_finish;
  serverContextP->experiment_timer_ev.sigev_notify_attributes = NULL;

  serverContextP->experiment_timer_et.it_interval.tv_sec = serverContextP->duration_sec;
  serverContextP->experiment_timer_et.it_interval.tv_nsec = 0;  
  serverContextP->experiment_timer_et.it_value.tv_sec = serverContextP->duration_sec;
  serverContextP->experiment_timer_et.it_value.tv_nsec = 0;

  server_info("Creating experiment timer. Period: %ld", serverContextP->experiment_timer_et.it_interval.tv_sec);
  if (timer_create(CLOCK_REALTIME, &serverContextP->experiment_timer_ev, &serverContextP->experiment_timer_id) != 0) {
    server_error("Failed to create experiment timer");
    goto failXit;
  }
  server_info("Done creating experiment timer: %p", serverContextP->experiment_timer_id); goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}
#endif

#define xstr(a) str(a)
#define str(a) #a

static void
chronos_usage() 
{
  char usage[1024];
  char template[] =
    "Usage: startup_server OPTIONS\n"
    "Starts up a chronos server \n"
    "\n"
    "OPTIONS:\n"
    "-m [mode]             running mode: 0: BASE, 1: Admission Control, 2: Adaptive Update, 3: Admission Control + Adaptive Update 4: Transaction Evaluation\n"
    "-c [num]              number of clients it can accept (default: %d)\n"
    "-v [num]              validity interval [in milliseconds] (default: %d ms)\n"
    "-s [num]              sampling period [in seconds] (default: %d seconds)\n"
    "-u [num]              number of update threads (default: %d)\n"
    "-r [num]              duration of the experiment [in seconds] (default: %d seconds)\n"
    "-p [num]              port to accept new connections (default: %d)\n"
    "-d [num]              debug level\n"
    "-n                    do not perform initial load\n"
    "-h                    help";

  snprintf(usage, sizeof(usage), template, 
          CHRONOS_NUM_CLIENT_THREADS, CHRONOS_INITIAL_VALIDITY_INTERVAL_MS, CHRONOS_SAMPLING_PERIOD_SEC,
          CHRONOS_NUM_UPDATE_THREADS, (int)CHRONOS_EXPERIMENT_DURATION_SEC, CHRONOS_SERVER_PORT);

  printf("%s\n", usage);
}
