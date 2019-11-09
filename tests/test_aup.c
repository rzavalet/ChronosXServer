#include <stdio.h>
#include "../aup.h"

int main()
{
  int rc = 0;
  int i = 0;
  chronos_aup_env_h *envH = NULL;

  printf("Starting test...\n");
  printf("\tCreating environment...\n");
  envH = chronos_aup_env_alloc(10  /* num_elements */, 
                               1.0 /* avi */, 
                               2.0 /* beta */);
  if (envH == NULL) {
    goto failXit;
  }

  for (i=0; i<2; i++) {
    printf("\tIncrementing af...\n");
    chronos_aup_af_incr(envH, 0);
    chronos_aup_data_dump(envH, 0);

    printf("\tRelaxing...\n");
    chronos_aup_relax(envH, 1);
    chronos_aup_data_dump(envH, 0);
  }

  /* n = log 2/log 1.1 ~= 8 */
  for (i=0; i<10; i++) {
    printf("\tIncrementing uf...\n");
    chronos_aup_uf_incr(envH, 0);
    chronos_aup_data_dump(envH, 0);

    printf("\tRelaxing...\n");
    chronos_aup_relax(envH, 1);
    chronos_aup_data_dump(envH, 0);
  }

  
  printf("\tReseting af...\n");
  chronos_aup_af_reset(envH, 0);
  chronos_aup_data_dump(envH, 0);

  printf("\tRelaxing...\n");
  chronos_aup_relax(envH, 1);
  chronos_aup_data_dump(envH, 0);

  printf("\tReseting uf...\n");
  chronos_aup_uf_reset(envH, 0);
  chronos_aup_data_dump(envH, 0);

  printf("\tRelaxing...\n");
  chronos_aup_relax(envH, 1);
  chronos_aup_data_dump(envH, 0);

  printf("\nTest passed!\n");
  goto cleanup;

failXit:
  rc = 1;

cleanup:
  if (envH != NULL) {
    chronos_aup_env_free(envH);
  }

  return rc;
}

