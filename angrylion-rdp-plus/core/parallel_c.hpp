#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void parallel_init(uint32_t num);
void parallel_run(void task(void));
uint32_t parallel_worker_id();
uint32_t parallel_worker_num();
void parallel_close();

#ifdef __cplusplus
}
#endif