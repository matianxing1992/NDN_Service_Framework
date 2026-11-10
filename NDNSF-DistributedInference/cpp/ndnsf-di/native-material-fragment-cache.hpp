#ifndef NDNSF_DI_NATIVE_MATERIAL_FRAGMENT_CACHE_HPP
#define NDNSF_DI_NATIVE_MATERIAL_FRAGMENT_CACHE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <ndn-cxx/util/span.hpp>

namespace ndnsf::di {

/**
 * Provider-local, content-addressed cache for authenticated material payloads.
 *
 * The cache stores the individual material payloads from a canonical material
 * manifest (serialized graph node/template payloads and initializer chunks) by
 * their sha256 digest.  It contains no role or recipe identity: every role that
 * consumes the same authenticated payload digest reuses the same file.  The
 * digest and byte count are rechecked on every read, so a damaged or replaced
 * file is ignored and the caller falls back to the Repo fetch.  Authorization
 * remains outside this class: the caller only asks for payloads that the
 * authenticated canonical material manifest/receipt selected.
 */
class NativeMaterialFragmentCache
{
public:
  explicit NativeMaterialFragmentCache(std::filesystem::path root);

  /**
   * Read one fragment. Returns nullopt for a missing, malformed, symlinked or
   * digest/size-mismatched file, so the caller can fetch the payload again.
   */
  std::optional<std::vector<std::uint8_t>>
  read(const std::string& digest, std::uint64_t expectedBytes,
       std::uint64_t maxBytes) const;

  /**
   * Store one fragment atomically. Returns true when a valid file with the
   * requested content is present afterwards (including a concurrent writer's
   * file). Cache failures are not fatal; the caller keeps its fetched bytes.
   */
  bool write(const std::string& digest, ndn::span<const std::uint8_t> bytes) const;

  const std::filesystem::path& root() const noexcept
  {
    return m_root;
  }

private:
  std::filesystem::path objectPath(const std::string& digest) const;

  std::filesystem::path m_root;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_MATERIAL_FRAGMENT_CACHE_HPP
