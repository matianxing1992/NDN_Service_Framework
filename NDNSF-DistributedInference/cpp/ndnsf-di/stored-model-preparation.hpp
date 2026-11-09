#ifndef NDNSF_DI_STORED_MODEL_PREPARATION_HPP
#define NDNSF_DI_STORED_MODEL_PREPARATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"

#include <functional>

namespace ndnsf::di {

/** Canonical identity stored with a reusable publication. Excludes local paths
 * and advisory transfer estimates while retaining immutable model policy.
 */
std::string
nativeStoredModelConfigurationIdentity(const std::string& configurationJson);

/** Admitted model facts and canonical assembly references, without Core Owner-key objects.
 * The caller owns the private handle separately. No request/grant or transport worker is cached.
 */
struct NativeStoredPreparedModel
{
  NativeRequestCatalog catalog;
  NativePreparedCanonicalPublication publication;
  NativeStoredModelCatalog objects;
  std::string configurationJson;
};

/** Publish existing inspected materials directly into the stored format, once.
 * Bundle only payloads with identical semantic consumers. Sources remain owned by catalog
 * until this synchronous operation returns; at most one bounded bundle is materialized.
 */
NativeStoredModelPublication
nativePublishStoredPreparedModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const NativeRequestCatalog& catalog, const std::string& configurationJson,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState = {});

/** Runtime preparation entry: defer source loading until the journal has admitted unfinished
 * work and resolved its prefix. READY needs no source factory, export or material inspection.
 */
NativeStoredModelPublication
nativePublishStoredPreparedModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::function<NativeRequestCatalog()>& loadCatalog, const std::string& configurationJson,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState);

/** Recover the real native catalog from admitted encrypted metadata, without source files.
 * Only metadata is fetched here. Weight bundles remain lazy Provider inputs.
 */
NativeStoredPreparedModel
nativeOpenStoredPreparedModel(const NativeStoredModelHandle& handle,
  const NativeStoredModelPacketIo& io, const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_STORED_MODEL_PREPARATION_HPP
