#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

#include "common.h"
#include "chronos_ac.h"

typedef struct chronos_ac_env_t 
{
  volatile int num_txn_to_wait;
  int          (*timeToDie)(void);
}

chronos_ac_env_t;
chronos_ac_env_t *chronos_ac_env_alloc(int (*timeToDie)(void))
{
  chronos_ac_env_t *envP = NULL;

  envP = calloc(1, sizeof(*envP));
  if (envP == NULL) {
    goto failXit;
  }

  envP->num_txn_to_wait = 0;
  envP->timeToDie = timeToDie;
  goto cleanup;

failXit:
  envP = NULL;

cleanup:
  return envP;
}

void chronos_ac_env_free(chronos_ac_env_t *envP)
{
  if (envP != NULL) {
    memset(envP, 0, sizeof(*envP));
    free(envP);
  }

  return;
}

int chronos_ac_wait_decrease(chronos_ac_env_t *envP)
{
  int rc = CHRONOS_SERVER_SUCCESS;

  if (envP->num_txn_to_wait > 0) {
    envP->num_txn_to_wait --;
  }
  else {
    envP->num_txn_to_wait = 0;
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

  if (num_waits > 0) {
    envP->num_txn_to_wait = num_waits;
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

#define MSG_FREQ 10
  while (envP->num_txn_to_wait > 0) {
    if (cnt_msg >= MSG_FREQ-1) {
      server_warning("### [AC] Doing admission control (%d) ###",
                     envP->num_txn_to_wait);
    }
    cnt_msg = (cnt_msg + 1) % MSG_FREQ;
    if (envP->timeToDie() == 1) {
      server_info("Requested to die");
      goto cleanup;
    }

    (void) sched_yield();
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

