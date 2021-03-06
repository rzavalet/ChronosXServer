#ifndef _CHRONOS_AUP_H
#define _CHRONOS_AUP_H

typedef void chronos_aup_env_h;

typedef struct chronos_aup_data_t chronos_aup_data_t;

chronos_aup_env_h *
chronos_aup_env_alloc(int num_elements, float avi, float beta);

int
chronos_aup_env_free(chronos_aup_env_h *envH);

int
chronos_aup_af_incr(chronos_aup_env_h *envH, int element_idx);

int
chronos_aup_af_reset(chronos_aup_env_h *envH, int element_idx);

int
chronos_aup_uf_incr(chronos_aup_env_h *envH, int element_idx);

int
chronos_aup_uf_reset(chronos_aup_env_h *envH, int element_idx);

int
chronos_aup_relax(chronos_aup_env_h *envH, float ds_k);

const chronos_aup_data_t *
chronos_aup_data_get(chronos_aup_env_h *envH, int element_idx);

int
chronos_aup_data_dump(chronos_aup_env_h *envH, int element_idx);

int
chronos_aup_data_set_dump(chronos_aup_env_h *envH, int element_idx, int num_elements);

int 
chronos_aup_reset_all(chronos_aup_env_h *envH);

int
chronos_aup_get_n_expired(chronos_aup_env_h *envH, 
                          unsigned int       first_element,
                          unsigned int       num_elements,
                          int                array_sz, 
                          int               *array);
int
chronos_aup_next_update_set(chronos_aup_env_h *envH, 
                            int                array_sz, 
                            int               *array);

float
chronos_aup_pext_get(chronos_aup_env_h *envH);
#endif
