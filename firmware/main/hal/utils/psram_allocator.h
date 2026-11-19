#pragma once
// Explicit PSRAM allocator for non-DMA containers (subtitle and receive queues).
// Keep task stacks, DMA buffers and driver-owned storage on their existing paths.
#include <esp_heap_caps.h>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace stackchan::memory {

template <typename T>
class PsramAllocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;

    PsramAllocator() noexcept = default;

    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        void* ptr = heap_caps_malloc(count * sizeof(T),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ptr && count != 0) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t) noexcept {
        heap_caps_free(ptr);
    }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
    return false;
}

} // namespace stackchan::memory
