#ifndef NDNSF_DI_NATIVE_FILESYSTEM_ARTIFACTS_HPP
#define NDNSF_DI_NATIVE_FILESYSTEM_ARTIFACTS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include <ndn-cxx/encoding/buffer.hpp>

#include <filesystem>

namespace ndnsf::di {

/** Load the explicit prepopulated source without a Repo copy or network fetch. */
class NativeFilesystemSourceProvider final : public RepositorySourceProvider
{
public:
  explicit NativeFilesystemSourceProvider(std::filesystem::path root);

  NativeCanonicalSource
  load(const RepositorySourceRequest& request, const Fallback&) const override;

private:
  std::filesystem::path m_root;
};

/** Local model transport for explicit filesystem-cache inference.
 * The native catalog still derives/adopts the canonical materials. External
 * weights remain bounded references into the verified source file. This path
 * does not qualify Repo transport or encrypted model distribution.
 */
class NativeFilesystemArtifactPublisher final : public RepositoryArtifactPublisher
{
public:
  NativeFilesystemArtifactPublisher(std::filesystem::path root,
                                    ndn::Name producer, std::string sourceNamespace);

  NativePreparedCanonicalPublication
  publish(const std::string& modelKey, const std::string& serviceName,
          const NativeInspectedModel& model, const NativeCanonicalSource& source,
          const NativeCanonicalPublicationOptions& options,
          const NativeRequestControl& control) const override;

  void rollback(const NativePreparedCanonicalPublication&) const noexcept override;

private:
  std::filesystem::path m_root;
  ndn::Name m_prefix;
};

/** Read one digest-checked local object/range under the selected namespace.
 * Missing, malformed or corrupt entries fail without network fallback.
 */
std::optional<ndn::Buffer>
readNativeFilesystemArtifact(const std::filesystem::path& root,
                             const ndn::Name& prefix, const ndn::Name& name,
                             std::uint64_t maxBytes);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_FILESYSTEM_ARTIFACTS_HPP
