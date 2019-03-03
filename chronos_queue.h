#ifndef _CHRONOS_QUEUE_H_
#define _CHRONOS_QUEUE_H_

#include "chronos_server.h"

int
chronos_dequeue_system_transaction(void *requestP_ret, chronos_time_t *ts, chronosServerContext_t *contextP);

int
chronos_enqueue_system_transaction(void *requestP, const chronos_time_t *ts, chronosServerContext_t *contextP);

int
chronos_enqueue_user_transaction(void                 *requestP,
                                 const chronos_time_t *ts, 
                                 unsigned long long   *ticket_ret, 
                                 volatile int         *txn_done,
                                 volatile int         *txn_rc,
                                 chronosServerContext_t *contextP);

int
chronos_dequeue_user_transaction(void               *requestP_ret,
                                 chronos_time_t     *ts, 
                                 unsigned long long *ticket_ret,
                                 volatile int       **txn_done_ret,
                                 volatile int       **txn_rc_ret,
                                 chronosServerContext_t *contextP);
#endif
