#ifndef NDNSF_DETAIL_IN_MEMORY_STORAGE_RANGE_HPP
#define NDNSF_DETAIL_IN_MEMORY_STORAGE_RANGE_HPP

#include <ndn-cxx/ims/in-memory-storage.hpp>

#include <algorithm>

namespace ndn_service_framework::detail {

/** Return the first cached full Data name that is not less than name.
 *
 * Uses the public, full-name-ordered IMS iterators. Forward iterator traversal
 * is linear; callers retain their cache lock while inspecting the range.
 */
inline ndn::InMemoryStorage::const_iterator
imsLowerBound(const ndn::InMemoryStorage& storage, const ndn::Name& name)
{
  if (storage.size() == 0) {
    return storage.end();
  }
  return std::lower_bound(storage.begin(), storage.end(), name,
    [] (const ndn::Data& data, const ndn::Name& query) {
      return data.getFullName() < query;
    });
}

} // namespace ndn_service_framework::detail

#endif // NDNSF_DETAIL_IN_MEMORY_STORAGE_RANGE_HPP
