/*
 * server_config.h
 *
 *  Created on: Jan 21, 2018
 *      Author: ricardo
 */

#ifndef SERVER_CONFIG_H_
#define SERVER_CONFIG_H_

/* These are the enabled components */
#define CHRONOS_UPDATE_TRANSACTIONS_ENABLED
#define CHRONOS_USER_TRANSACTIONS_ENABLED

/* These are the directories where the databases and the datafiles live.
 * Before starting up the server, the datafiles should be moved to the
 * specified directory. The datafiles are used to populate the Chronos
 * tables */
#define CHRONOS_SERVER_HOME_DIR       "/tmp/chronos/databases"
#define CHRONOS_SERVER_DATAFILES_DIR  "/tmp/chronos/datafiles"

/* By default the Chronos server runs in port 5000 */
#define CHRONOS_SERVER_ADDRESS  "127.0.0.1"
#define CHRONOS_SERVER_PORT     5000

#define CHRONOS_DEBUG_LEVEL_MIN         (0)
#define CHRONOS_DEBUG_LEVEL_MAX         (10)

#define CHRONOS_SAMPLING_SPACE         (5)


/* In the Chronos paper, the number of server threads
 * is 350 for their linux settings
 */
#ifdef CHRONOS_DEBUG
#define CHRONOS_NUM_SERVER_THREADS    1
#else
#define CHRONOS_NUM_SERVER_THREADS    1
#endif
#define CHRONOS_MAX_NUM_SERVER_THREADS  CHRONOS_NUM_SERVER_THREADS


#define CHRONOS_MIN_DATA_ITEMS_PER_XACT   50
#define CHRONOS_MAX_DATA_ITEMS_PER_XACT   100

/* By default, updates to the quotes table is performed
 * by 100 threads
 */
#define CHRONOS_NUM_UPDATE_THREADS    1

/* Each update thread handles 30 stocks
 */
#define CHRONOS_NUM_STOCK_UPDATES_PER_UPDATE_THREAD  30

/* Chronos server has two ready queues. The default size of them is 1024 */
#define CHRONOS_READY_QUEUE_SIZE     (1024)

/* The update period is initially set to 0.5 in Chronos
 */
#define CHRONOS_INITIAL_VALIDITY_INTERVAL_MS  1000

/* By default, Chronos uses \beta=2
 */
#define CHRONOS_UPDATE_PERIOD_RELAXATION_BOUND  2


#ifdef CHRONOS_DEBUG
#define CHRONOS_DESIRED_DELAY_BOUND_MS          1
#else
#define CHRONOS_DESIRED_DELAY_BOUND_MS          1000
#endif

/* Some utility macros to perform conversion of time units */
#define CHRONOS_MS_TO_S(_ms)            ((_ms) / 1000.0)
#define CHRONOS_S_TO_MS(_s)             ((_s) * 1000.0)

#define CHRONOS_MIN_TO_S(_m)            ((_m) * 60)
#define CHRONOS_S_TO_MIN(_s)            ((_s) / 60)

/* The sampling in Chronos is done every 30 seconds
 */
#define CHRONOS_SAMPLING_PERIOD_SEC     (30)

/* Chronos experiments take 15 minutes
 */
#ifdef CHRONOS_DEBUG
#define CHRONOS_EXPERIMENT_DURATION_SEC (CHRONOS_MIN_TO_S(1))
#else
#define CHRONOS_EXPERIMENT_DURATION_SEC (CHRONOS_MIN_TO_S(15))
#endif

#define CHRONOS_ALPHA                  (0.4)

/* In the Chronos paper, the number of client threads start
 * at 900 and it can increase up to 1800
 */
#ifdef CHRONOS_DEBUG
#define CHRONOS_NUM_CLIENT_THREADS    10
#else
#define CHRONOS_NUM_CLIENT_THREADS    900
#endif

#define CHRONOS_TCP_QUEUE   1024

#endif /* SERVER_CONFIG_H_ */
