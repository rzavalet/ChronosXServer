#ifndef _CHRONOS_SERVER_H_
#define _CHRONOS_SERVER_H_

#include <signal.h>
#include <time.h>
#include <pthread.h>

#include <benchmark.h>

#include <chronos_transactions.h>
#include <chronos_packets.h>

#include "server_config.h"



#define CHRONOS_SERVER_CTX_MAGIC      (0xBACA)
#define CHRONOS_SERVER_THREAD_MAGIC   (0xCACA)

#define CHRONOS_SERVER_CTX_CHECK(_ctxt)    assert((_ctxt)->magic == CHRONOS_SERVER_CTX_MAGIC)
#define CHRONOS_SERVER_THREAD_CHECK(_thr)    assert((_thr)->magic == CHRONOS_SERVER_THREAD_MAGIC)

#define CHRONOS_MODE_BASE   (0)
#define CHRONOS_MODE_AC     (1)
#define CHRONOS_MODE_AUP    (2)
#define CHRONOS_MODE_FULL   (3)

#define IS_CHRONOS_MODE_BASE(_ctxt)   ((_ctxt)->runningMode == CHRONOS_MODE_BASE)
#define IS_CHRONOS_MODE_AC(_ctxt)     ((_ctxt)->runningMode == CHRONOS_MODE_AC)
#define IS_CHRONOS_MODE_AUP(_ctxt)    ((_ctxt)->runningMode == CHRONOS_MODE_AUP)
#define IS_CHRONOS_MODE_FULL(_ctxt)   ((_ctxt)->runningMode == CHRONOS_MODE_FULL)

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
  CHRONOS_SERVER_THREAD_MAX,
  CHRONOS_SERVER_THREAD_INVAL=CHRONOS_SERVER_THREAD_MAX
} chronosServerThreadType_t;

#define CHRONOS_SERVER_THREAD_NAME(_txn_type) \
  ((CHRONOS_SERVER_THREAD_MIN<=(_txn_type) && (_txn_type) < CHRONOS_SERVER_THREAD_MAX) ? (chronosServerThreadNames[(_txn_type)]) : "INVALID")

#define CHRONOS_SERVER_THREAD_IS_VALID(_txn_type) \
  (CHRONOS_SERVER_THREAD_MIN<=(_txn_type) && (_txn_type) < CHRONOS_SERVER_THREAD_MAX)


typedef struct timespec chronos_time_t;
#define NSEC_TO_SEC (1000000000)
#define CHRONOS_TIME_FMT "%ld.%09ld"
#define CHRONOS_TIME_ARG(_t) (_t).tv_sec, (_t).tv_nsec
#define CHRONOS_TIME_CLEAR(_t) \
  do {\
    (_t).tv_sec = 0; \
    (_t).tv_nsec = 0; \
  }while (0)

#define CHRONOS_TIME_GET(_t) clock_gettime(CLOCK_REALTIME, &(_t))

#define CHRONOS_TIME_NANO_OFFSET_GET(_begin, _end, _off)    \
  do {                  \
    chronos_time_t _tf = _end;            \
    chronos_time_t _to = _begin;          \
                  \
    if (_tf.tv_sec > _to.tv_sec && _tf.tv_nsec < _to.tv_nsec) {   \
      _tf.tv_sec --;              \
      _tf.tv_nsec += NSEC_TO_SEC;         \
    }                 \
    else if (_tf.tv_sec < _to.tv_sec && _tf.tv_nsec > _to.tv_nsec) {  \
      _to.tv_sec --;              \
      _to.tv_nsec += NSEC_TO_SEC;         \
    }                 \
    (_off).tv_sec = _tf.tv_sec - _to.tv_sec;        \
    (_off).tv_nsec = _tf.tv_nsec - _to.tv_nsec;       \
  } while(0)

#define CHRONOS_TIME_SEC_OFFSET_GET(_begin, _end, _off)     \
  do {                  \
    chronos_time_t _tf = _end;            \
    chronos_time_t _to = _begin;          \
                  \
    if (_tf.tv_sec > _to.tv_sec && _tf.tv_nsec < _to.tv_nsec) {   \
      _tf.tv_sec --;              \
      _tf.tv_nsec += NSEC_TO_SEC;         \
    }                 \
    else if (_tf.tv_sec < _to.tv_sec && _tf.tv_nsec > _to.tv_nsec) {  \
      _to.tv_sec --;              \
      _to.tv_nsec += NSEC_TO_SEC;         \
    }                 \
    (_off).tv_sec = _tf.tv_sec - _to.tv_sec;        \
    (_off).tv_nsec = 0;             \
  } while(0)

#define CHRONOS_TIME_ADD(_t1, _t2, _res)      \
      do {              \
  chronos_time_t _tf = _t1;       \
  chronos_time_t _to = _t2;       \
  (_res).tv_sec = 0;          \
  (_res).tv_nsec = (_tf).tv_nsec + (_to).tv_nsec;   \
                \
  if ((_res).tv_nsec > NSEC_TO_SEC) {     \
    (_res).tv_sec = (_res).tv_nsec / NSEC_TO_SEC; \
    (_res).tv_nsec = (_res).tv_nsec % NSEC_TO_SEC;  \
  }             \
                \
  (_res).tv_sec += (_tf).tv_sec + (_to).tv_sec;   \
                \
      } while(0)

#define CHRONOS_TIME_AVERAGE(_sum, _num, _res)        \
  do {                  \
    if (_num != 0) {              \
      long long mod;              \
      (_res).tv_sec  = (_sum).tv_sec / (signed long)_num;   \
      mod =  NSEC_TO_SEC * ((_sum).tv_sec % (signed long)_num);   \
      (_res).tv_nsec = (mod + (_sum).tv_nsec) / (signed long)_num;  \
      if ((_res).tv_nsec >= NSEC_TO_SEC) {        \
  mod = (_res).tv_nsec / NSEC_TO_SEC;       \
  (_res).tv_sec += mod;           \
  mod = (_res).tv_nsec % NSEC_TO_SEC;       \
  (_res).tv_nsec = mod;           \
      }                 \
    }                 \
  }while(0)

#define CHRONOS_TIME_POSITIVE(_time)    \
  ((_time).tv_sec > 0 && (_time).tv_nsec > 0)

#define CHRONOS_TIME_ZERO(_time)    \
  ((_time).tv_sec == 0 && (_time).tv_nsec == 0)

#define CHRONOS_TIME_NEGATIVE(_time)    \
  ((_time).tv_sec < 0 && (_time).tv_nsec < 0)

#define SEC_TO_MSEC   (1000)
#define NSEC_TO_MSEC  (1000000)

#define CHRONOS_TIME_TO_MS(_time) \
  ((_time).tv_sec * SEC_TO_MSEC + (_time).tv_nsec / NSEC_TO_MSEC)

#define CHRONOS_MS_TO_TIME(_ms, _time) \
  do { \
    (_time).tv_sec = (_ms) / SEC_TO_MSEC; \
    (_time).tv_nsec = ((_ms) % SEC_TO_MSEC) * NSEC_TO_MSEC; \
  } while (0)


typedef struct chronosServerStats_t 
{
  int            num_txns;
  double         cumulative_time_ms;

  int            num_failed_txns;

  int            num_timely_txns;
} chronosServerStats_t;

/* Information required for update transactions */
typedef struct 
{
  const char *pkey;         /* Primary key */
} update_txn_info_t;

/* Information required for view_stock transactions */
typedef struct 
{
  int num_keys;
  chronosSymbol_t symbolInfo[CHRONOS_MAX_DATA_ITEMS_PER_XACT];
} view_txn_info_t;

/* This is the structure of a transaction request in Chronos */
typedef struct 
{
  chronos_time_t txn_start;
  chronos_time_t txn_enqueue;
  unsigned long long ticket;
  volatile int *txn_done;
  volatile int *txn_rc;

  chronosRequestPacket_t  request;
} txn_info_t;
  
/* This is the structure of a ready queue in Chronos. 
 */
typedef struct 
{
  txn_info_t txnInfoArr[CHRONOS_READY_QUEUE_SIZE]; /* Txn requests are stored here */
  int occupied;
  int nextin;
  int nextout;
  unsigned long long ticketReq;
  unsigned long long ticketDone;
  pthread_mutex_t mutex;
  pthread_cond_t more;
  pthread_cond_t less;
  pthread_cond_t ticketReady;
} chronos_queue_t;


typedef struct chronosDataItem_t
{
  int      index;
  char    *dataItem;

  unsigned long long nextUpdateTimeMS;
  volatile double accessFrequency[CHRONOS_SAMPLING_SPACE];
  volatile double updateFrequency[CHRONOS_SAMPLING_SPACE];
  volatile double accessUpdateRatio[CHRONOS_SAMPLING_SPACE];
  volatile double updatePeriodMS[CHRONOS_SAMPLING_SPACE];
} chronosDataItem_t;

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

  int (*timeToDieFp)(void);

  
  /* We can only create a limited number of 
   * client threads. */
  int numClientsThreads;
  int currentNumClients;

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
  double initialValidityIntervalMS;
  double minUpdatePeriodMS;
  double maxUpdatePeriodMS;

  /* Each data item is refreshed in a certain interval.
   * This update period is related to the validityIntervalms */
  double updatePeriodMS;

  /* This is the "deadline" for the user txns */
  double desiredDelayBoundMS;

  /* These are the queues holding requested txns */
  chronos_queue_t userTxnQueue;
  chronos_queue_t sysTxnQueue;

  /*============ These fields control the sampling task ==========*/
  volatile int          currentSlot;
  chronosServerStats_t  stats_matrix[CHRONOS_SAMPLING_SPACE][CHRONOS_MAX_NUM_SERVER_THREADS];
  double                average_service_delay_ms;
  double                degree_timing_violation;
  double                smoth_degree_timing_violation;
  double                alpha;
  volatile int          num_txn_to_wait;
  int                   total_txns_enqueued;

  chronosDataItem_t    *dataItemsArray;
  int                   szDataItemsArray;

  /* Metrics are obtained by sampling. This is the sampling interval */
  double samplingPeriodSec;

  /* Variables for the sampling timer */
  timer_t sampling_timer_id;
  struct itimerspec sampling_timer_et;
  struct sigevent sampling_timer_ev;
  /*==============================================================*/


  /* These fields are for the benchmark framework */
  void *benchmarkCtxtP;
  
} chronosServerContext_t;

typedef struct chronosUpdateThreadInfo_t {
  int    num_stocks;       /* How many stocks should the thread handle */
  chronosDataItem_t    *dataItemsArray;      /* Pointer to the array that contains the stocks managed by this thread */
} chronosUpdateThreadInfo_t;

typedef struct chronosServerThreadInfo_t {
  int       magic;
  pthread_t thread_id;
  int       thread_num;
  int       first_symbol_id;
  int       socket_fd;
  FILE     *trace_file;
  chronosServerThreadState_t state;
  chronosServerThreadType_t thread_type;
  chronosServerContext_t *contextP;

  /* These are fields specific to each thread type */
  union {
    chronosUpdateThreadInfo_t updateParameters;
  } parameters;
} chronosServerThreadInfo_t;

#endif
