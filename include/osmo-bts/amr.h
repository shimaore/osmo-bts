#ifndef _OSMO_BTS_AMR_H
#define _OSMO_BTS_AMR_H

#include <osmo-bts/gsm_data.h>

void amr_log_mr_conf(int ss, int logl, const char *pfx,
		     struct amr_multirate_conf *amr_mrc);

int amr_parse_mr_conf(struct amr_multirate_conf *amr_mrc,
		      const uint8_t *mr_conf, unsigned int len);

unsigned int amr_get_initial_mode(struct gsm_lchan *lchan);

#endif /* _OSMO_BTS_AMR_H */