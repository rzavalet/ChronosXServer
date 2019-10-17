#ifndef _CHRONOS_AUP_H
#define _CHRONOS_AUP_H

typedef void * chronos_aup_env_h;

typedef struct chronos_aup_data_t {
  float       access_update_rate;
  long long   access_frequency;
  long long   update_frequency;
  float       flexible_validity_interval;
  float       absolute_validity_interval;
  float       beta;

  long long   period;       /* in MS */
} chronos_aup_data_t;

typedef struct chronos_aup_env_t {
  int                        magic;
  float                      beta;
  int                        num_elements;
  chronos_aup_data_t         data_array[1];
} chronos_aup_env_t;


chronos_aup_env_h
chronos_aup_env_alloc(int num_elements, float avi, float beta);

int
chronos_aup_env_free(chronos_aup_env_h envH);

int
chronos_aup_af_incr(chronos_aup_env_h envH, int element_idx);

int
chronos_aup_af_reset(chronos_aup_env_h envH, int element_idx);

int
chronos_aup_uf_incr(chronos_aup_env_h envH, int element_idx);

int
chronos_aup_uf_reset(chronos_aup_env_h envH, int element_idx);

int
chronos_aup_relax(chronos_aup_env_h env, float ds_k);

const chronos_aup_data_t *
chronos_aup_data_get(chronos_aup_env_h envH, int element_idx);

int
chronos_aup_data_dump(chronos_aup_env_h envH, int element_idx);

int 
chronos_aup_reset_all(chronos_aup_env_h envH);
#endif
