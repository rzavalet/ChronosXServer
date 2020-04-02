#ifndef ADMISION_CONTROL_H
#define ADMISION_CONTROL_H

typedef struct chronos_ac_env_t chronos_ac_env_t;

chronos_ac_env_t *chronos_ac_env_alloc(int (*timeToDie)(void));
void chronos_ac_env_free(chronos_ac_env_t *envP);

int chronos_ac_wait_decrease(chronos_ac_env_t *envP);
int chronos_ac_wait_set(int num_waits, chronos_ac_env_t *envP);
int chronos_ac_wait(chronos_ac_env_t *envP);

#endif
