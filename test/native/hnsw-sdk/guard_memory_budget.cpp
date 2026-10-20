/*
 * Standalone CPU checks for source-derived cuVS HNSW/ACE memory guard math.
 * Compile with -I<patched-cuvs>/cpp/src/neighbors/detail/cagra.
 */
#include "ace_memory_guard.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>

using cuvs::neighbors::cagra::detail::memory_guard::checked_add;
using cuvs::neighbors::cagra::detail::memory_guard::checked_mul;
using cuvs::neighbors::cagra::detail::memory_guard::hnsw_order_arrays_bytes;
using cuvs::neighbors::cagra::detail::memory_guard::budget_bytes_from_gib;
using cuvs::neighbors::cagra::detail::memory_guard::checked_scale;
using cuvs::neighbors::cagra::detail::memory_guard::nn_descent_workspace_bytes;
using cuvs::neighbors::cagra::detail::memory_guard::require_within;
using cuvs::neighbors::cagra::detail::memory_guard::reorder_rows_per_buffer;

template <typename Fn>
static bool throws(Fn&& fn)
{
  try {
    fn();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

int main()
{
  assert(checked_add(17, 25) == 42);
  assert(checked_mul(6, 7) == 42);
  assert(throws([] { (void)checked_add(std::numeric_limits<size_t>::max(), 1); }));
  assert(throws([] { (void)checked_mul(std::numeric_limits<size_t>::max(), 2); }));
  assert(checked_scale(100, 1.5) == 150);
  assert(throws([] { (void)checked_scale(1, std::numeric_limits<double>::infinity()); }));
  assert(budget_bytes_from_gib(1.0).value() == (size_t{1} << 30));
  assert(!budget_bytes_from_gib(0.0).has_value());
  assert(throws([] { (void)budget_bytes_from_gib(-1.0); }));
  assert(throws([] { (void)budget_bytes_from_gib(std::numeric_limits<double>::infinity()); }));
  assert(throws([] { (void)budget_bytes_from_gib(1e300); }));

  // The disk serializer's order, reverse-order, and level arrays are 20 bytes per row.
  assert(hnsw_order_arrays_bytes(1000) == 20'000);
  assert(throws([] { (void)hnsw_order_arrays_bytes(std::numeric_limits<size_t>::max()); }));

  require_within("exact-fit", 1024, 1024);
  assert(throws([] { require_within("over-budget", 1025, 1024); }));

  // Two buffers per partition must fit at the selected rows-per-buffer value.
  const size_t partitions = 4;
  const size_t row_bytes  = 32;
  const size_t budget     = 2 * partitions * 64 * row_bytes;
  const size_t rows       = reorder_rows_per_buffer(partitions, row_bytes, budget);
  assert(rows == 64);
  assert(2 * partitions * rows * row_bytes <= budget);
  assert(throws([&] { (void)reorder_rows_per_buffer(partitions, row_bytes, budget - 1); }));

  const auto [nn_host, nn_gpu] = nn_descent_workspace_bytes(1000, 16, 32);
  assert(nn_host > 1000 * 912);
  assert(nn_gpu == 1000 * (280 + 32 * sizeof(uint32_t) + 16 * sizeof(float)));
  assert(throws([] { (void)nn_descent_workspace_bytes(
    std::numeric_limits<size_t>::max(), 16, 32); }));
}
