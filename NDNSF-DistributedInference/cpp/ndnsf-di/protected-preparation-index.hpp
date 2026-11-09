#ifndef NDNSF_DI_PROTECTED_PREPARATION_INDEX_HPP
#define NDNSF_DI_PROTECTED_PREPARATION_INDEX_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include <filesystem>
#include <optional>

namespace ndnsf::di {

/** Private pointers to original protected root/metadata objects, not a model store.
 * Core owner ports authenticate objects and restore their owned serving leases.
 * No callback, credential, plaintext model or body key is retained here.
 */
class ProtectedPreparationIndex
{
public:
  ProtectedPreparationIndex(std::filesystem::path sourceCacheRoot, ndn::Name owner);

  std::optional<NativePreparedCanonicalPublication>
  lookup(const RepositoryPreparedLookupRequest& request) const;

  /** On a normal miss, admit a same-owner legacy pointer and copy only its
   * verified complete reference into this durable index. No model is published.
   */
  std::optional<NativePreparedCanonicalPublication>
  lookupWithLegacy(const RepositoryPreparedLookupRequest& request,
                   const std::filesystem::path& legacyRoot) const;

  void
  remember(const RepositoryPreparedLookupRequest& request,
           const NativePreparedCanonicalPublication& publication) const;

private:
  std::filesystem::path m_directory;
  ndn::Name m_owner;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PROTECTED_PREPARATION_INDEX_HPP
