#ifndef _CHRONOS_QUEUE_H_
#define _CHRONOS_QUEUE_H_

typedef struct chronos_queue_t chronos_queue_t;

chronos_queue_t *
chronos_queue_alloc(int (*timeToDieFp)(void));

void
chronos_queue_free(chronos_queue_t *queueP);

int
chronos_dequeue_system_transaction(void *requestP_ret, 
                                   struct timeval *ts, 
                                   chronos_queue_t *systemTxnQueueP);

int
chronos_enqueue_system_transaction(void *requestP, 
                                   struct timeval *ts, 
                                   chronos_queue_t *systemTxnQueueP);

int
chronos_enqueue_user_transaction(void                 *requestP,
                                 struct timeval       *ts, 
                                 unsigned long long   *ticket_ret, 
                                 volatile int         *txn_done,
                                 volatile int         *txn_rc,
                                 chronos_queue_t      *userTxnQueueP);

int
chronos_dequeue_user_transaction(void               *requestP_ret,
                                 struct timeval     *ts, 
                                 unsigned long long *ticket_ret,
                                 volatile int       **txn_done_ret,
                                 volatile int       **txn_rc_ret,
                                 chronos_queue_t     *userTxnQueueP);

#if 0
int
chronos_system_queue_size(chronos_queue_t *queueP);

int
chronos_user_queue_size(chronos_queue_t *queueP);
#endif

int 
chronos_queue_size(chronos_queue_t    *txnQueueP);
#endif
