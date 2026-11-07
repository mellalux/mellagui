#ifndef MELLAGUI_FUZZ_COV_H
#define MELLAGUI_FUZZ_COV_H

#include <stdint.h>

#define LM_FUZZ_COV_SIZE (1u << 18)
extern uint8_t lm_fuzz_cov[LM_FUZZ_COV_SIZE];      /* 1 = this edge ran since the last clear */

#endif
