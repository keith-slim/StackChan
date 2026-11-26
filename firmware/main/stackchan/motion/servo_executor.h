#pragma once
#include <atomic>
#include <mutex>
#include <type_traits>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace stackchan::motion::executor {

// Exactly one synchronous request in flight. The caller owns its callable
// until completion; the worker never takes the submit mutex or a UI lock.
struct Request {
    void (*run)(void*);
    void* context;
    int64_t submitted_us;
};
inline std::atomic<TaskHandle_t> owner{nullptr};
inline std::atomic<Request*> pending{nullptr};
inline std::mutex submit_mutex;
inline StaticSemaphore_t done_storage;
inline SemaphoreHandle_t done = nullptr;
// Written and reported only by the worker.
inline uint32_t request_count = 0, wait_max_us = 0, run_max_us = 0;

inline bool external() {
    const auto task = owner.load(std::memory_order_acquire);
    return task && task != xTaskGetCurrentTaskHandle();
}
template<class F>
void invoke(F&& fn) {
    if (!external()) { fn(); return; }
    std::lock_guard<std::mutex> lock(submit_mutex);
    const auto task = owner.load(std::memory_order_acquire);
    if (!task) { fn(); return; }
    using Callable = std::remove_reference_t<F>;
    Request request{[](void* p) { (*static_cast<Callable*>(p))(); }, &fn, esp_timer_get_time()};
    pending.store(&request, std::memory_order_release);
    xTaskNotifyGive(task);
    // Never abandon a stack-backed request while it is in flight.
    xSemaphoreTake(done, portMAX_DELAY);
}
inline void service() {
    if (auto* request = pending.exchange(nullptr, std::memory_order_acq_rel)) {
        const int64_t started = esp_timer_get_time();
        const uint32_t wait = started - request->submitted_us;
        request->run(request->context);
        const uint32_t run = esp_timer_get_time() - started;
        ++request_count;
        if (wait > wait_max_us) wait_max_us = wait;
        if (run > run_max_us) run_max_us = run;
        xSemaphoreGive(done);
    }
}
} // namespace stackchan::motion::executor
