#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>

#include <chronos_transactions.h>

#include "chronos_queue.h"
#include "chronos_packets.h"
#include "server_config.h"
#include "common.h"

/* This is the structure of a transaction request in Chronos */
typedef struct txn_info_t
{
  struct timeval txn_enqueue;

  unsigned long long ticket;

  volatile int *txn_done;
  volatile int *txn_rc;

  chronosRequestPacket_t  request;
} txn_info_t;
  
/* This is the structure of a ready queue in Chronos. 
 */
typedef struct chronos_queue_t
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

  int (*timeToDieFp)(void);
} chronos_queue_t;


void
chronos_queue_free(chronos_queue_t *queueP)
{
  if (queueP == NULL) {
    return;
  }

  pthread_cond_destroy(&queueP->more);
  pthread_cond_destroy(&queueP->less);
  pthread_mutex_destroy(&queueP->mutex);

  memset(queueP, 0, sizeof(*queueP));
  free(queueP);

  return;

}

chronos_queue_t *
chronos_queue_alloc(int (*timeToDieFp)(void))
{
  chronos_queue_t *queueP = NULL;

  queueP = calloc(1, sizeof(chronos_queue_t));
  if (queueP == NULL) {
    goto failXit;
  }

  if (pthread_mutex_init(&queueP->mutex, NULL) != 0) {
    server_error("Failed to init mutex");
    goto failXit;
  }

  if (pthread_cond_init(&queueP->more, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  if (pthread_cond_init(&queueP->less, NULL) != 0) {
    server_error("Failed to init condition variable");
    goto failXit;
  }

  queueP->timeToDieFp = timeToDieFp;

  return queueP;

failXit:
  chronos_queue_free(queueP);
  return NULL;
}

/*--------------------------------------------------------
 * Dequeue a transaction request from the queue specified
 * by the caller. The transaction information is stored
 * at the address specified by the txnInfoP pointer.
 *------------------------------------------------------*/
static int
chronos_dequeue_transaction(txn_info_t      *txnInfoP, 
                            int            (*timeToDieFp)(void), 
                            chronos_queue_t *txnQueueP)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  struct timespec  ts; /* For the timed wait */

  if (txnQueueP == NULL || txnInfoP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  pthread_mutex_lock(&txnQueueP->mutex);
  while(txnQueueP->occupied <= 0) {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    pthread_cond_timedwait(&txnQueueP->more, &txnQueueP->mutex, &ts);

    if (timeToDieFp && timeToDieFp()) {
      server_warning("Process asked to die");
      pthread_mutex_unlock(&txnQueueP->mutex);
      goto failXit;
    }
  }

  assert(txnQueueP->occupied > 0);

  memcpy(txnInfoP, &(txnQueueP->txnInfoArr[txnQueueP->nextout]), sizeof(*txnInfoP));

  txnQueueP->nextout++;
  txnQueueP->nextout %= CHRONOS_READY_QUEUE_SIZE;
  txnQueueP->occupied--;

  server_debug(CHRONOS_DEBUG_LEVEL_MAX, 
               "dequeued with ticket: %llu (occupied %d)", 
               txnInfoP->ticket, txnQueueP->occupied);

  /* now: either txnQueueP->occupied > 0 and txnQueueP->nextout is the index
       of the next occupied slot in the buffer, or
       txnQueueP->occupied == 0 and txnQueueP->nextout is the index of the next
       (empty) slot that will be filled by a producer (such as
       txnQueueP->nextout == txnQueueP->nextin) */

  pthread_cond_signal(&txnQueueP->less);
  pthread_mutex_unlock(&txnQueueP->mutex);
  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

static int
chronos_enqueue_transaction(txn_info_t         *txnInfoP, 
                            unsigned long long *ticket_ret, 
                            int               (*timeToDieFp)(void), 
                            chronos_queue_t    *txnQueueP)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  struct timespec  ts; /* For the timed wait */

  if (txnQueueP == NULL || txnInfoP == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  pthread_mutex_lock(&txnQueueP->mutex);
   
  while (txnQueueP->occupied >= CHRONOS_READY_QUEUE_SIZE) {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;

    pthread_cond_timedwait(&txnQueueP->less, &txnQueueP->mutex, &ts);

    if (timeToDieFp && timeToDieFp()) {
      server_warning("Process asked to die");
      pthread_mutex_unlock(&txnQueueP->mutex);
      goto failXit;
    }
  }

  assert(txnQueueP->occupied < CHRONOS_READY_QUEUE_SIZE);
  
  memcpy(&(txnQueueP->txnInfoArr[txnQueueP->nextin]), txnInfoP, sizeof(*txnInfoP));

  txnQueueP->ticketReq ++;
  txnQueueP->txnInfoArr[txnQueueP->nextin].ticket = txnQueueP->ticketReq;
  if (ticket_ret) {
    *ticket_ret = txnQueueP->ticketReq;
  }
  
  txnQueueP->nextin ++;
  txnQueueP->nextin %= CHRONOS_READY_QUEUE_SIZE;
  txnQueueP->occupied++;

  server_debug(CHRONOS_DEBUG_LEVEL_MAX,
              "enqueued with ticket: %llu (occupied %d)", 
              txnQueueP->ticketReq, txnQueueP->occupied);

  /* now: either b->occupied < CHRONOS_READY_QUEUE_SIZE and b->nextin is the index
       of the next empty slot in the buffer, or
       b->occupied == CHRONOS_READY_QUEUE_SIZE and b->nextin is the index of the
       next (occupied) slot that will be emptied by a dequeueTransaction
       (such as b->nextin == b->nextout) */

  pthread_cond_signal(&txnQueueP->more);

  pthread_mutex_unlock(&txnQueueP->mutex);

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

/*-------------------------------------------------------------------
 * Dequeues a request from the 'System Transaction Queue'.
 *-----------------------------------------------------------------*/
int
chronos_dequeue_system_transaction(void                   *requestP_ret,
                                   struct timeval         *ts, 
                                   chronos_queue_t        *systemTxnQueueP)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  txn_info_t       txn_info;

  if (requestP_ret == NULL || ts == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  rc = chronos_dequeue_transaction(&txn_info, 
                                   systemTxnQueueP->timeToDieFp, 
                                   systemTxnQueueP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Could not dequeue update transaction");
    goto failXit;
  }

  memcpy(requestP_ret, &txn_info.request, sizeof(txn_info.request));
  *ts = txn_info.txn_enqueue;

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}


/*-------------------------------------------------------------------
 * Enqueues a request into the 'System Transaction Queue'.
 *-----------------------------------------------------------------*/
int
chronos_enqueue_system_transaction(void                   *requestP, 
                                   struct timeval         *ts, 
                                   chronos_queue_t        *systemTxnQueueP)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  txn_info_t       txn_info;

  if (requestP == NULL || ts == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  /* Set the transaction information */
  memset(&txn_info, 0, sizeof(txn_info));
  memcpy(&txn_info.request, requestP, sizeof(txn_info.request));
  txn_info.txn_enqueue = *ts;

  rc = chronos_enqueue_transaction(&txn_info, 
                                   NULL /* ticket_ret */, 
                                   systemTxnQueueP->timeToDieFp, 
                                   systemTxnQueueP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Could not enqueue update transaction");
    goto failXit;
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

int
chronos_dequeue_user_transaction(void                   *requestP_ret, 
                                 struct timeval         *ts, 
                                 unsigned long long     *ticket_ret,
                                 volatile int           **txn_done_ret,
                                 volatile int           **txn_rc_ret,
                                 chronos_queue_t         *userTxnQueueP)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  txn_info_t       txn_info;

  if (ts == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  rc = chronos_dequeue_transaction(&txn_info, 
                                   userTxnQueueP->timeToDieFp, 
                                   userTxnQueueP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Could not dequeue user transaction");
    goto failXit;
  }

  memcpy(requestP_ret, &txn_info.request, sizeof(txn_info.request));
  *ts = txn_info.txn_enqueue;
  *ticket_ret = txn_info.ticket;
  *txn_done_ret = txn_info.txn_done;
  *txn_rc_ret = txn_info.txn_rc;

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

int
chronos_enqueue_user_transaction(void *requestP,
                                 struct timeval *ts, 
                                 unsigned long long *ticket_ret, 
                                 volatile int *txn_done,
                                 volatile int *txn_rc,
                                 chronos_queue_t *userTxnQueueP)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  txn_info_t       txn_info;

  if (ts == NULL || ticket_ret == NULL) {
    server_error("Invalid argument");
    goto failXit;
  }

  /* Set the transaction information */
  memset(&txn_info, 0, sizeof(txn_info));
  memcpy(&txn_info.request, requestP, sizeof(txn_info.request));
  txn_info.txn_enqueue = *ts;
  txn_info.txn_done = txn_done;
  txn_info.txn_rc = txn_rc;

  rc = chronos_enqueue_transaction(&txn_info, 
                                   ticket_ret, 
                                   userTxnQueueP->timeToDieFp, 
                                   userTxnQueueP);
  if (rc != CHRONOS_SERVER_SUCCESS) {
    server_error("Could not enqueue update transaction");
    goto failXit;
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}


int 
chronos_queue_size(chronos_queue_t    *txnQueueP)
{
  if (txnQueueP == NULL) {
    return 0;
  }

  return txnQueueP->occupied;
}

int
chronos_system_queue_size(chronos_queue_t *systemTxnQueueP) 
{
  return chronos_queue_size(systemTxnQueueP);
}

int
chronos_user_queue_size(chronos_queue_t *userTxnQueueP)
{
  return chronos_queue_size(userTxnQueueP);
}

