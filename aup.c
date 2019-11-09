#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>

#include "aup.h"
#include "common.h"


#define CHRONOS_AUP_MAGIC                   (0xDEAD)
#define CHRONOS_AUP_MAGIC_CHECK(_env)       (assert((_env)->magic == CHRONOS_AUP_MAGIC))
#define CHRONOS_AUP_MAGIC_SET(_env)         ((_env)->magic = CHRONOS_AUP_MAGIC)

typedef struct chronos_aup_data_t {
  float       access_update_rate;
  long long   access_frequency;
  long long   update_frequency;
  float       flexible_validity_interval;
  float       absolute_validity_interval;
  float       beta;

  long long   period;       /* in MS */

  long long   time_to_next_update;
} chronos_aup_data_t;


typedef struct chronos_aup_env_t {
  int                        magic;
  float                      beta;
  int                        num_elements;
  chronos_aup_data_t         data_array[1];
} chronos_aup_env_t;


chronos_aup_env_h *
chronos_aup_env_alloc(int num_elements, float avi, float beta)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  int              i = 0;
  chronos_aup_env_t *envP = NULL;
  chronos_aup_data_t  *dataP = NULL;

  assert(num_elements > 0);
  assert(beta > 0.0f);
  assert(avi > 0.0f);

  envP = calloc(1, sizeof(chronos_aup_env_t) 
                   + (sizeof(chronos_aup_data_t) * num_elements));
  if (envP == NULL) {
    server_error("Could not allocate space for aup environment");
    goto failXit;
  }

  CHRONOS_AUP_MAGIC_SET(envP);

  envP->num_elements = num_elements;
  envP->beta = beta;

  for (i=0; i<num_elements; i++) {
    dataP = &(envP->data_array[i]);
    dataP->absolute_validity_interval = avi;
    dataP->flexible_validity_interval = avi;
    dataP->access_update_rate = 1.0f;
    dataP->access_frequency = 0;
    dataP->update_frequency = 0;
    dataP->beta = beta;

    dataP->period = 0.5 * dataP->flexible_validity_interval;
  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;
  if (envP != NULL) {
    free(envP);
    envP == NULL;
  }

cleanup:
  return envP;
}


int
chronos_aup_env_free(chronos_aup_env_h *envH)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  envP->magic = 0;
  free(envP);
  envP = NULL;

  return rc;
}

int
chronos_aup_af_incr(chronos_aup_env_h *envH, int element_idx)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  assert(0 <= element_idx && element_idx < envP->num_elements);

  envP->data_array[element_idx].access_frequency ++;

  return rc;
}

int
chronos_aup_af_reset(chronos_aup_env_h *envH, int element_idx)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  assert(0 <= element_idx && element_idx < envP->num_elements);

  envP->data_array[element_idx].access_frequency = 0;

  return rc;
}

int
chronos_aup_uf_incr(chronos_aup_env_h *envH, int element_idx)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  assert(0 <= element_idx && element_idx < envP->num_elements);

  envP->data_array[element_idx].update_frequency++;

  return rc;
}

int
chronos_aup_uf_reset(chronos_aup_env_h *envH, int element_idx)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  assert(0 <= element_idx && element_idx < envP->num_elements);

  envP->data_array[element_idx].update_frequency = 0;

  return rc;
}


int 
chronos_aup_reset_all(chronos_aup_env_h *envH)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  int              i;
  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  for (i=0; i<envP->num_elements; i++) {
    (void) chronos_aup_uf_reset(envH, i);
    (void) chronos_aup_af_reset(envH, i);
  }

  return rc;
}

int
chronos_aup_get_n_expired(chronos_aup_env_h *envH, int out_array_sz, int *out_array)
{
  int              rc = CHRONOS_SERVER_SUCCESS;
  int              i;
  chronos_aup_env_t *envP = NULL;
  struct timeval   current_time;
  long long        current_time_msec;
  chronos_aup_data_t  *dataP = NULL;
  int              num_found = 0;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  gettimeofday(&current_time, NULL);
  current_time_msec = current_time.tv_sec * 1000 + current_time.tv_usec/1000;

  for (i=0; i<out_array_sz; i++) {
    out_array[i] = -1;
  }

  for (i=0; i<envP->num_elements && num_found < out_array_sz; i++) {
    dataP = &(envP->data_array[i]);

    if (dataP->time_to_next_update <= current_time_msec) {
      out_array[num_found] = i;
      num_found ++;
    }
  }

  return rc;
}


#define MIN(a,b)      (a < b ? a : b)
int
chronos_aup_relax(chronos_aup_env_h *envH, float ds_k)
{

  int              rc = CHRONOS_SERVER_SUCCESS;
  int              i = 0;
  int              relax_num_target = 0;
  chronos_aup_env_t *envP = NULL;
  chronos_aup_data_t  *dataP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  relax_num_target = MIN(envP->num_elements, ds_k * envP->num_elements);

  for (i=0; i < envP->num_elements && relax_num_target > 0; i++) {
    dataP = &(envP->data_array[i]);

    /* AUR >= 1 --> don't do anything*/
    if (dataP->access_frequency >= dataP->update_frequency) {
      continue;
    }
    /* AUR < 1 --> relax*/
    else {
      float tmp = 1.1 * dataP->flexible_validity_interval;
      float avi = dataP->absolute_validity_interval;
      float beta = dataP->beta;

      if (avi <= tmp && tmp <= beta * avi) {
        dataP->flexible_validity_interval = tmp;
      }

      relax_num_target --;
    }

  }

  goto cleanup;

failXit:
  rc = CHRONOS_SERVER_FAIL;

cleanup:
  return rc;
}

const chronos_aup_data_t *
chronos_aup_data_get(chronos_aup_env_h *envH, int element_idx)
{

  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  assert(0 <= element_idx && element_idx < envP->num_elements);

  return &(envP->data_array[element_idx]);
}

int
chronos_aup_data_dump(chronos_aup_env_h *envH, int element_idx)
{

  chronos_aup_env_t *envP = NULL;

  envP = (chronos_aup_env_t *) envH;
  CHRONOS_AUP_MAGIC_CHECK(envP);

  assert(0 <= element_idx && element_idx < envP->num_elements);


  fprintf(stdout, "\n");
  fprintf(stdout, "magic = 0x%X\n", envP->magic);
  fprintf(stdout, "num_elements = %d\n", envP->num_elements);
  fprintf(stdout, "  access_update_rate         = %.2f\n", envP->data_array[element_idx].access_update_rate);
  fprintf(stdout, "  access_frequency           = %lld\n", envP->data_array[element_idx].access_frequency);
  fprintf(stdout, "  update_frequency           = %lld\n", envP->data_array[element_idx].update_frequency);
  fprintf(stdout, "  flexible_validity_interval = %.2f\n", envP->data_array[element_idx].flexible_validity_interval);
  fprintf(stdout, "  absolute_validity_interval = %.2f\n", envP->data_array[element_idx].absolute_validity_interval);
  fprintf(stdout, "  beta                       = %.2f\n", envP->data_array[element_idx].beta);

  return CHRONOS_SERVER_SUCCESS;
}


