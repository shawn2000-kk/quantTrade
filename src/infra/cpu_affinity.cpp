#include "cpu_affinity.hpp"

#ifdef __linux__
#  include <pthread.h>
#  include <sched.h>
#  include <errno.h>
#  include <cstring>
#endif

namespace hft {

#ifdef __linux__

bool pin_thread_to_core(int core_id) noexcept {
    if (core_id < 0) return false;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<size_t>(core_id), &cpuset);

    return ::pthread_setaffinity_np(::pthread_self(),
                                    sizeof(cpu_set_t),
                                    &cpuset) == 0;
}

bool pin_thread_to_cores(const std::vector<int>& cores) noexcept {
    if (cores.empty()) return false;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int c : cores) {
        if (c >= 0) CPU_SET(static_cast<size_t>(c), &cpuset);
    }

    return ::pthread_setaffinity_np(::pthread_self(),
                                    sizeof(cpu_set_t),
                                    &cpuset) == 0;
}

int get_current_core() noexcept {
    return ::sched_getcpu(); // 返回当前 CPU，失败返回 -1
}

bool set_thread_realtime(int priority) noexcept {
    if (priority < 1 || priority > 99) return false;

    struct sched_param param{};
    param.sched_priority = priority;
    return ::sched_setscheduler(0 /* calling thread */,
                                SCHED_FIFO,
                                &param) == 0;
}

#else // macOS / other — stub implementations

bool pin_thread_to_core(int /*core_id*/) noexcept  { return false; }
bool pin_thread_to_cores(const std::vector<int>& /*cores*/) noexcept { return false; }
int  get_current_core() noexcept                   { return -1;    }
bool set_thread_realtime(int /*priority*/) noexcept { return false; }

#endif // __linux__

} // namespace hft
