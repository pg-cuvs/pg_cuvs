// CPU regression for the HNSW serializer batch-size expression in detail/hnsw.hpp.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

size_t serializer_batch_size(uint32_t n_rows,
                             size_t target_batch_bytes,
                             size_t row_size_bytes)
{
  return std::min<size_t>(
    static_cast<size_t>(n_rows), std::max<size_t>(1, target_batch_bytes / row_size_bytes));
}

int main()
{
  constexpr size_t row_size_bytes = 24;
  assert(serializer_batch_size(uint32_t{100}, 17 * row_size_bytes, row_size_bytes) == 17);
  assert(serializer_batch_size(uint32_t{4}, 1024, row_size_bytes) == 4);
}
