#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

#include "common.h"
#include "chronos_ac.h"

typedef struct chronos_ac_env_t 
{
  pthread_spinlock_t  lock;
  volatile int num_txn_to_wait;
  int          (*timeToDie)(void);
} chronos_ac_env_t;

chronos_ac_env_t *chronos_ac_env_alloc(int (*timeToDie)(void))
{
  chronos_ac_env_t *envP = NULL;
  int rc = 0;

  envP = calloc(1, sizeof(*envP));
  if (envP == NULL) {
    goto failXit;
  }

  envP->num_txn_to_wait = 0;
  envP->timeToDie = timeToDie;

  rc = pthread_spin_init(&envP->lock, PTHREAD_PROCESS_PRIVATE);
  if (rc != 0) {
    goto failXit;
  }

  goto cleanup;

failXit:
  envP = NULL;

cleanup:
  return envP;
}

void chronos_ac_env_free(chronos_ac_env_t *envP)
{
  if (envP != NULL) {

    pthread_spin_destroy(&envP->lock);

    memset(envP, 0, sizeof(*envP));
    free(envP);
  }

  return;
}

int chronos_ac_wait_decrease(chronos_ac_env_t *envP)
{
  int rc = CHRONOS_SERVER_SUCCESS;

  rc = pthread_spin_lock(&envP->lock);
  if (rc != 0) {
    goto failXit;
  }

  if (envP->num_txn_to_wait > 0) {
    envP->num_txn_to_wait --;
    server_warning("### current_num_txns_to_wait = %d\n", envP->num_txn_to_wait);
  }

  rc = pthread_spin_unlock(&envP->lock);
  if (rc != 0) {
    goto failXit;
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

int chronos_ac_wait_set(int num_waits, chronos_ac_env_t *envP)
{
  int rc = CHRONOS_SERVER_SUCCESS;

  rc = pthread_spin_lock(&envP->lock);
  if (rc != 0) {
    goto failXit;
  }

  if (num_waits > 0) {
    server_warning("### [AC] Setting number of transactions to wait(%d) ###",
                   num_waits);
    envP->num_txn_to_wait = num_waits;
  }


  rc = pthread_spin_unlock(&envP->lock);
  if (rc != 0) {
    goto failXit;
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

int chronos_ac_wait(chronos_ac_env_t *envP)
{
  int rc = CHRONOS_SERVER_SUCCESS;
  int cnt_msg = 0;
  int did_wait = 0;
  struct timeval timeout;

  while (envP->num_txn_to_wait > 0) {

#define MSG_FREQ  100
    cnt_msg = (cnt_msg + 1) % MSG_FREQ;
    if (envP->timeToDie() == 1) {
      server_info("Requested to die");
      goto cleanup;
    }

    milliSleep(100, timeout);
    did_wait = 1;
  }

  if (did_wait) {
    server_warning("### [AC] Done ###");
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

