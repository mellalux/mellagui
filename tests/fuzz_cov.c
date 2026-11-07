/*
 * Edge coverage for the fuzzer (tests/fuzz_server.c). gcc has no libFuzzer, but -fsanitize-coverage=trace-pc calls
 * __sanitizer_cov_trace_pc() at every basic block; this file (compiled WITHOUT that flag) hashes (previous block, this block)
 * into a bitmap the fuzzer reads after each run.
 */
#include <stdint.h>

#include "fuzz_cov.h"

uint8_t lm_fuzz_cov[LM_FUZZ_COV_SIZE];
static uintptr_t prev_pc;

void __sanitizer_cov_trace_pc(void);
void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    uintptr_t edge = (pc ^ (prev_pc >> 1)) * 0x9e3779b1u;
    lm_fuzz_cov[(edge >> 7) & (LM_FUZZ_COV_SIZE - 1)] = 1;
    prev_pc = pc;
}
