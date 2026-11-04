#ifndef CUVS_FAILED_ALLOCATION_HPP
#define CUVS_FAILED_ALLOCATION_HPP

#include <cstddef>
#include <cstdint>
#include <exception>

/* One request thread's final failing upstream request, not its logical
 * suballocation. Unknown is -1; never infer it from formatted exception text. */
namespace cuvs_failed_allocation {
inline thread_local int64_t bytes = -1;
inline thread_local std::exception_ptr exception;
inline void clear() noexcept { bytes = -1; exception = nullptr; }
inline int64_t take() noexcept { int64_t value = bytes; clear(); return value; }
inline int64_t take_for_current_exception() noexcept
{
    auto current = std::current_exception();
    int64_t value = current && current == exception ? bytes : -1;
    clear();
    return value;
}

template <class Allocate>
auto capture(std::size_t request, Allocate &&allocate) -> decltype(allocate())
{
    try {
        return allocate();
    } catch (...) {
        bytes = request > 0 && request <= static_cast<std::size_t>(INT64_MAX)
                    ? static_cast<int64_t>(request) : -1;
        throw;
    }
}

template <class Allocate>
auto outer(Allocate &&allocate) -> decltype(allocate())
{
    clear();
    try {
        auto result = allocate();
        clear();
        return result;
    } catch (...) {
        if (bytes > 0) exception = std::current_exception();
        throw;
    }
}
} // namespace cuvs_failed_allocation
#endif
