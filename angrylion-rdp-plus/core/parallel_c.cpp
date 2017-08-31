#include "parallel_c.hpp"
#include "parallel.hpp"

#include <atomic>

static std::unique_ptr<Parallel> parallel;
static thread_local uint32_t worker_id;
static uint32_t worker_num;

void parallel_init(uint32_t num)
{
    // auto-select number of workers based on the number of cores
    if (num == 0) {
        num = std::thread::hardware_concurrency();
    }

    parallel = std::make_unique<Parallel>(num, [](uint32_t id) {
        worker_id = id;
    });

    worker_num = num;
}

void parallel_run(void task(void))
{
    parallel->run(task);
}

uint32_t parallel_worker_id()
{
    return worker_id;
}

uint32_t parallel_worker_num()
{
    return worker_num;
}

void parallel_close()
{
    parallel.reset();
}
