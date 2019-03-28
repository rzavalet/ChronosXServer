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

#include <chronos_packets.h>
#include <chronos_transactions.h>
#include <chronos_environment.h>
#include <benchmark.h>

#include "server_config.h"
#include "common.h"
#include "chronos_server.h"
#include "chronos_queue.h"
#include "stats.h"

#define xstr(s) str(s)
#define str(s) #s

/* These are the available server modes */
#define CHRONOS_SERVER_MODE_BASE                (0)
#define CHRONOS_SERVER_MODE_AC                  (1)
#define CHRONOS_SERVER_MODE_AUP                 (2)
#define CHRONOS_SERVER_MODE_AC_AUP              (3)

#define CHRONOS_SERVER_MODE_NAME_BASE           "base"
#define CHRONOS_SERVER_MODE_NAME_AC             "ac"
#define CHRONOS_SERVER_MODE_NAME_AUP            "aup"
#define CHRONOS_SERVER_MODE_NAME_AC_AUP         "ac_aup"

/* These are the different running modes */
#define CHRONOS_SERVER_XACT_EVALUATION_MODE     (4)
#define CHRONOS_SERVER_XACT_NAME_VIEW_STOCK     "view_stock"
#define CHRONOS_SERVER_XACT_NAME_VIEW_PORTFOLIO "view_portfolio"
#define CHRONOS_SERVER_XACT_NAME_PURCHASE       "purchase_stock"
#define CHRONOS_SERVER_XACT_NAME_SELL           "sell_stock"
#define CHRONOS_SERVER_XACT_NAME_REFRESH_STOCK  "refresh_stock"

static const char *program_name = "startup_server";

int benchmark_debug_level = CHRONOS_DEBUG_LEVEL_MIN;
int server_debug_level = CHRONOS_DEBUG_LEVEL_MIN;
int chronos_debug_level = CHRONOS_DEBUG_LEVEL_MIN;

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

static void *
processThread(void *argP);

static int
dispatchTableFn (chronosRequestPacket_t *reqPacketP, int *txn_rc, chronosServerThreadInfo_t *infoP);

static int
waitPeriod(double updatePeriodMS);

static int 
runTxnEvaluation(chronosServerContext_t *serverContextP);

#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
static int
processUserTransaction(volatile int *txn_rc,
                       chronosRequestPacket_t *reqPacketP,
                       chronosServerThreadInfo_t *infoP);

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
  int rc = CHRONOS_SERVER_SUCCESS;
  int i;
  int thread_num = 0;
  pthread_attr_t attr;
  int *thread_rc = NULL;
  const int stack_size = 0x100000; // 1 MB
  char **pkeys_list = NULL;
  int    num_pkeys = 0;
  chronos_time_t  system_start;
  unsigned long long initial_update_time_ms;

  rc = init_stats_struct();

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

  if (serverContextP->runningMode == CHRONOS_SERVER_XACT_EVALUATION_MODE) {
    server_info("Running in transaction evaluation mode");
    rc = runTxnEvaluation(serverContextP);
    if (rc != CHRONOS_SERVER_SUCCESS) {
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
 
  rc = benchmark_portfolios_stats_get(serverContextP->benchmarkCtxtP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to get portfolios stat");
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

  server_debug(2,"Spawned listener thread");
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
  contextP->initialLoad = 0;

  contextP->timeToDieFp = isTimeToDie;

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
                   {"evaluate-wcet",          no_argument,       0,  0 },
                   {"xact-name",              required_argument, 0,  0 },
                   {"print-samples",          no_argument      , &print_samples,  1 },
                   {"initial-load",           no_argument      , 0,  0 },
                   {"help",                   no_argument      , 0,  'h' },
                   {"xact-size",              required_argument, 0,  0 },
                   {"server-mode",            required_argument, 0,  'm'},
                   {"update-threads",         required_argument, 0,  'u'},
                   {"experiment-duration",    required_argument, 0,  'r'},
                   {0,                        0,                 0,  0 }
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
                    "m:c:v:s:u:r:p:d:h",
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
        contextP->duration_sec = atoi(optarg);
        server_debug(2, "*** Duration: %ld", contextP->duration_sec);
        break;

      case 'p':
        contextP->serverPort = atoi(optarg);
        server_debug(2, "*** Server port: %d", contextP->serverPort);
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

  contextP->minUpdatePeriodMS = 0.5 * contextP->initialValidityIntervalMS;
  contextP->maxUpdatePeriodMS = 0.5 * CHRONOS_UPDATE_PERIOD_RELAXATION_BOUND * contextP->initialValidityIntervalMS;
  contextP->updatePeriodMS  =  0.5 * contextP->initialValidityIntervalMS;

  if (contextP->runningMode == CHRONOS_SERVER_XACT_EVALUATION_MODE) {
    server_debug(2, "*** Evaluating transaction: %s", chronos_user_transaction_str[serverContextP->evaluated_txn]);
  }

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
  chronos_time_t   current_time;
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
  CHRONOS_TIME_GET(current_time);

  rc = chronos_enqueue_user_transaction(reqPacketP, &current_time, &ticket,
                                        &txn_done, &txn_rc, infoP->contextP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Failed to enqueue request");
    goto failXit;
  }

  /* Wait until the transaction is processed by the processThread */
  while (!txn_done) {
    sleep(1);
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
  pthread_t tid = pthread_self();

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  /*------------ Read the request -----------------*/
  server_debug(3, "%lu: waiting new request", tid);

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
  /*-----------------------------------------------*/


#if 0
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
#endif



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

cleanup:

  close(infoP->socket_fd);

  free(infoP);

  server_info("%lu: daHandler exiting", tid);
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
  socklen_t client_address_len;
  pthread_attr_t attr;
  const int stack_size = 0x100000; // 1 MB
  chronosServerThreadInfo_t *handlerInfoP = NULL;
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

      server_debug(2,"%lu: Spawned handler thread", tid);

    } while (accepted_socket_fd != -1);

  }

  if (time_to_die == 1) {
    server_info("%lu: Requested to die", tid);
    goto cleanup;
  }

#ifdef CHRONOS_SAMPLING_ENABLED
  if (startSamplingTimer(infoP->contextP) != CHRONOS_SERVER_SUCCESS) {
    goto cleanup;
  }
#endif

cleanup:
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
processUserTransaction(volatile int *txn_rc,
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
  chronosUserTransaction_t txn_type;
  BENCHMARK_DATA_PACKET_H data_packetH = NULL;
  pthread_t tid = pthread_self();

  if (infoP == NULL || infoP->contextP == NULL || txn_rc == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  txn_type = reqPacketP->txn_type;
  num_data_items = reqPacketP->numItems;
  
  CHRONOS_TIME_GET(txn_begin);

  /* dispatch a transaction */
  switch(txn_type) {

    case CHRONOS_USER_TXN_VIEW_STOCK:
      memset(pkey_list, 0, sizeof(pkey_list));
      for (i=0; i<num_data_items; i++) {
        pkey_list[i] = reqPacketP->request_data.symbolInfo[i].symbol;
      }
      *txn_rc = benchmark_view_stock2(num_data_items, pkey_list, infoP->contextP->benchmarkCtxtP);
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

  server_info("%lu: Txn rc: %d", tid, *txn_rc);

  if (infoP->contextP->num_txn_to_wait > 0) {
    server_warning("### [AC] Need to wait for: %d/%d transactions to finish ###",
                  infoP->contextP->num_txn_to_wait, 
                  infoP->contextP->total_txns_enqueued);
    infoP->contextP->num_txn_to_wait --;
  }

  /*--------------------------------------------*/


  CHRONOS_TIME_GET(txn_end);
   
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

static int
processRefreshTransaction(chronosRequestPacket_t *requestP,
                          chronosServerThreadInfo_t *infoP)
{
  int               rc = CHRONOS_SERVER_SUCCESS;
  const char       *pkey = NULL;
  chronosServerContext_t *contextP = NULL;
  chronos_time_t    txn_begin;
  chronos_time_t    txn_end;
  int               i;
  int               num_data_items;
  float             fvalues_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  const char       *pkey_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
  pthread_t tid = pthread_self();

  if (infoP == NULL || infoP->contextP == NULL || requestP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  contextP = infoP->contextP;

  server_debug(3, "%lu: (thread %d) Processing update...", tid, infoP->thread_num);

  num_data_items = requestP->numItems;

  for (i=0; i<num_data_items; i++) {
    pkey_list[i] = requestP->request_data.updateInfo[i].symbol;
    fvalues_list[i] = requestP->request_data.updateInfo[i].price;
  }

  rc = benchmark_refresh_quotes_list(num_data_items,
                                     pkey_list,
                                     fvalues_list,
                                     contextP->benchmarkCtxtP);

  server_debug(3, "%lu: (thread %d) Done processing update...", tid, infoP->thread_num);
#if 0
  if (infoP->contextP->num_txn_to_wait > 0) {
    server_warning("### [AC] Need to wait for: %d/%d transactions to finish ###",
                 contextP->num_txn_to_wait, 
                 contextP->total_txns_enqueued);
    infoP->contextP->num_txn_to_wait --;
  }
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

/*
 * This is the driver function of process thread
 */
static void *
processThread(void *argP) 
{
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  chronosRequestPacket_t receivedRequest;
  chronos_time_t         enqueued_time;
  unsigned long long     ticket;
  volatile int          *txn_doneP = NULL;
  volatile int          *txn_rcP = NULL;
  long int              elapsed_time;
  struct timeval        start_time;
  struct timeval        current_time;
  int num_success = 0;
  int num_failures = 0;
  int num_total_txns = 0;
  int current_rep = 0;
  int rc;
  pthread_t tid = pthread_self();
  
  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  getTime(&start_time);

  /* Give update transactions more priority */
  /* TODO: Add scheduling technique */
  while (!time_to_die) {

    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

#ifdef CHRONOS_UPDATE_TRANSACTIONS_ENABLED
    if (infoP->contextP->sysTxnQueue.occupied > 0) {
      /*-------- Process refresh transaction ----------*/
      current_rep ++;

      rc = chronos_dequeue_system_transaction(&receivedRequest,
                                              &enqueued_time,
                                              infoP->contextP);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to dequeue request");
        goto cleanup;
      }

      CHRONOS_REQUEST_MAGIC_CHECK(&receivedRequest);

      RTCAPTURE_START();

      rc = processRefreshTransaction(&receivedRequest, infoP);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to execute refresh transactions");
      }

      RTCAPTURE_END();

      num_total_txns ++;
      if (rc == CHRONOS_SERVER_SUCCESS) {
        num_success ++;
      }
      else {
        num_failures ++;
      }

      /*--------------------------------------------*/
    }
#endif

    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);
    
#ifdef CHRONOS_USER_TRANSACTIONS_ENABLED
    if (infoP->contextP->userTxnQueue.occupied > 0) {
      /*-------- Process user transaction ----------*/
      current_rep ++;

      txn_doneP = NULL;
      txn_rcP = NULL;

      rc = chronos_dequeue_user_transaction(&receivedRequest,
                                            &enqueued_time,
                                            &ticket,
                                            &txn_doneP,
                                            &txn_rcP,
                                            infoP->contextP);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to dequeue request");
        goto cleanup;
      }

      assert(txn_doneP != NULL);
      assert(txn_rcP != NULL);

      CHRONOS_REQUEST_MAGIC_CHECK(&receivedRequest);

      RTCAPTURE_START();

      server_info("%lu: Processing ticket: %llu", tid, ticket);
      if (processUserTransaction(txn_rcP, &receivedRequest, infoP) != CHRONOS_SERVER_SUCCESS) {
        server_error("%lu: Failed to execute refresh transactions", tid);
      }

      if (txn_doneP != NULL) {
        *txn_doneP = 1;
      }

      RTCAPTURE_END();

      num_total_txns ++;
      if (*txn_rcP == CHRONOS_SERVER_SUCCESS) {
        num_success ++;
      }
      else {
        num_failures ++;
      }

      /*--------------------------------------------*/
    }
#endif

    getTime(&current_time);
    elapsed_time = diff_time(&start_time, &current_time);

#if 0
    server_info("%lu: elapsed_time: %ld, duration_time: %ld, current_rep: %d", 
                tid, elapsed_time, 1000 * infoP->contextP->duration_sec, current_rep);
#endif

    if (elapsed_time >= 1000 * infoP->contextP->duration_sec) {
      time_to_die = 1;
      break;
    }
  }
  
cleanup:

  RTCAPTURE_PRINT(); 
  fprintf(stderr, "---------------------------\n");
  fprintf(stderr, "Successful Xacts = %d\n", num_success);
  fprintf(stderr, "Failed Xacts = %d\n", num_failures);
  fprintf(stderr, "Total Xacts = %d\n", num_total_txns);
  fprintf(stderr, "---------------------------\n");

  server_info("processThread exiting");
  
  pthread_exit(NULL);
}

/*
 * This is the driver function of an update thread
 */
static void *
updateThread(void *argP) 
{
  int    i;
  int    rc = CHRONOS_SERVER_SUCCESS;
  int    num_updates = 0;
  chronosDataItem_t *dataItemArray =  NULL;
  volatile int current_slot;
  chronos_time_t   current_time;
  unsigned long long current_time_ms;
  chronos_time_t   next_update_time;
  unsigned long long next_update_time_ms;
  chronosServerThreadInfo_t *infoP = (chronosServerThreadInfo_t *) argP;
  CHRONOS_ENV_H   chronosEnvH;
  CHRONOS_CACHE_H chronosCacheH = NULL;
  CHRONOS_CLIENT_CACHE_H  clientCacheH = NULL;
  CHRONOS_REQUEST_H requestH = NULL;
  pthread_t tid = pthread_self();

  if (infoP == NULL || infoP->contextP == NULL) {
    server_error("Invalid argument");
    goto cleanup;
  }

  CHRONOS_SERVER_THREAD_CHECK(infoP);
  CHRONOS_SERVER_CTX_CHECK(infoP->contextP);

  chronosEnvH = chronosEnvAlloc(CHRONOS_SERVER_HOME_DIR, CHRONOS_SERVER_DATAFILES_DIR);
  if (chronosEnvH == NULL) {
    server_error("Failed to allocate chronos environment handle");
    goto cleanup;
  }

  chronosCacheH = chronosEnvCacheGet(chronosEnvH);
  if (chronosCacheH == NULL) {
    server_error("Invalid cache handle");
    goto cleanup;
  }

  clientCacheH = chronosClientCacheAlloc(1, 1, chronosCacheH);
  if (clientCacheH == NULL) {
    server_error("Invalid client cache handle");
    goto cleanup;
  }

  num_updates = infoP->parameters.updateParameters.num_stocks;
  assert(num_updates > 0);

  dataItemArray = infoP->parameters.updateParameters.dataItemsArray;
  assert(dataItemArray != NULL);

  while (1) {
    CHRONOS_SERVER_THREAD_CHECK(infoP);
    CHRONOS_SERVER_CTX_CHECK(infoP->contextP);


    server_debug(3, "%lu: --- Starting new cycle (thread %d) ---", 
                tid, infoP->thread_num);
    CHRONOS_TIME_GET(current_time);
    current_time_ms = CHRONOS_TIME_TO_MS(current_time);

    if (infoP->contextP->runningMode == CHRONOS_SERVER_MODE_BASE ||
        infoP->contextP->runningMode == CHRONOS_SERVER_MODE_AUP) {

      /* NEW_TODO: This does not honor the assigned stocks per thread */
      requestH = chronosRequestCreate(num_updates,
                                      CHRONOS_USER_TXN_MAX,
                                      clientCacheH, 
                                      chronosEnvH);
      if (requestH == NULL) {
        server_error("Failed to populate request");
        goto cleanup;
      }

      rc = chronos_enqueue_system_transaction(requestH,
                                              &current_time,
                                              infoP->contextP);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to enqueue request");
        goto cleanup;
      }

      rc = chronosRequestFree(requestH);
      if (rc != CHRONOS_SERVER_SUCCESS) {
        server_error("Failed to release request");
        goto cleanup;
      }
    }

#if 0
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
#endif

    if (waitPeriod(100) != CHRONOS_SERVER_SUCCESS) {
      goto cleanup;
    }

    if (time_to_die == 1) {
      server_info("%lu: Requested to die", tid);
      goto cleanup;
    }
  }

cleanup:
  server_info("%lu: updateThread exiting", tid);
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
  int txn_rc = 0;
  int num_data_items;
  int num_success = 0;
  int num_failures = 0;
  int num_total_txns = 0;
  int                values_list[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
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
      server_info("----------------------------------------------");
      fprintf(stderr, "\n\n");
  }
    
  RTCAPTURE_PRINT(); 
  fprintf(stderr, "---------------------------\n");
  fprintf(stderr, "Successful Xacts = %d\n", num_success);
  fprintf(stderr, "Failed Xacts = %d\n", num_failures);
  fprintf(stderr, "Total Xacts = %d\n", num_total_txns);
  fprintf(stderr, "---------------------------\n");

  goto cleanup; 

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
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
    "-c [num]                        number of clients it can accept (default: "xstr(CHRONOS_NUM_CLIENT_THREADS)")\n"
    "-v [num]                        validity interval [in milliseconds] (default: "xstr(CHRONOS_INITIAL_VALIDITY_INTERVAL_MS)" ms)\n"
    "-s [num]                        sampling period [in seconds] (default: "xstr(CHRONOS_SAMPLING_PERIOD_SEC)" seconds)\n"
    "-u|--update-threads [num]       number of update threads (default: "xstr(CHRONOS_NUM_UPDATE_THREADS)")\n"
    "-r|--experiment-duration [num]  duration of the experiment [in seconds] (default: "xstr(CHRONOS_EXPERIMENT_DURATION_SEC)" seconds)\n"
    "-p [num]                        port to accept new connections (default: "xstr(CHRONOS_SERVER_PORT)")\n"
    "-d [num]                        debug level\n"
    "--evaluate-wcet                 evaluate transaction worst case execution time\n"
    "--xact-name <xact>              evalulate the indicated transaction\n"
    "                                Valid transactions are: \n"
    "                                   "CHRONOS_SERVER_XACT_NAME_VIEW_STOCK"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_VIEW_PORTFOLIO"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_PURCHASE"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_SELL"\n"
    "                                   "CHRONOS_SERVER_XACT_NAME_REFRESH_STOCK"\n"
    "--xact-size <num_items>         evalulate the indicated transaction\n"
    "--print-samples                 print timing samples\n"
    "--initial-load                  perform initial load\n"
    "-h|--help                       help";

  printf("%s\n", template);
}
