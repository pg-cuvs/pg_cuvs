// CPU regression for the ACE reorder metadata expression in cagra_build.cuh.
#include "ace_memory_guard.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace cuvs::neighbors::cagra::detail::memory_guard;

template <typename Fn>
bool throws(Fn&& fn)
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
  const size_t metadata_bytes = 128;
  const size_t expected = checked_sum({metadata_bytes,
                                       checked_mul(size_t{2}, sizeof(size_t)),
                                       checked_mul(size_t{2}, sizeof(std::vector<int>))});
  assert(reorder_metadata_with_ends<std::vector<int>>(metadata_bytes) == expected);
  assert(throws([] {
    (void)reorder_metadata_with_ends<std::vector<int>>(std::numeric_limits<size_t>::max());
  }));
}
