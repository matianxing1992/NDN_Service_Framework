#ifndef NDNSF_DI_PREPARED_MODEL_PACKAGE_HPP
#define NDNSF_DI_PREPARED_MODEL_PACKAGE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-preparation.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf_distributed_repo {
struct RepoArtifactTarget;
}

namespace ndnsf::di {

/** Immutable operator registration retained by a prepared package. */
struct FrozenPreparationRegistration
{
  std::string key;
  std::filesystem::path baseDirectory;
  std::string configurationJson;
  std::string configurationDigest;
  std::string taskName;
  std::string taskContractDigest;
  std::string inputLayoutDigest;
};

/** Private stored-model capability plus its admitted immutable object map.
 * This is retained only by the requesting Runtime package; it is never serialized
 * into plans or passed to a Provider.
 */
struct NativeStoredModelBinding
{
  NativeStoredModelBinding(const NativeStoredModelHandle& modelHandle,
                           NativeStoredModelCatalog admittedObjects,
                           std::shared_ptr<const ndnsf_distributed_repo::RepoArtifactTarget> target)
    : handle(modelHandle)
    , objects(std::move(admittedObjects))
    , repositoryTarget(std::move(target))
  {
  }

  NativeStoredModelHandle handle;
  NativeStoredModelCatalog objects;
  std::shared_ptr<const ndnsf_distributed_repo::RepoArtifactTarget> repositoryTarget;
};

/**
 * Fully verified preparation result.  The object is private to the
 * preparation owner and is never published until every identity check has
 * completed.  It contains no request, grant, ACK, Selection, or Provider
 * state.
 */
struct PreparedModelPackage
{
  NativeRequestCatalog catalog;
  std::shared_ptr<const FrozenPreparationRegistration> registration;
  ModelManifest manifest;
  ModelCapabilities capabilities;
  std::string preparationKeyDigest;
  std::size_t retainedBytes = 0;
  /** Built-in cooperative placement used when the caller supplies no opaque
   * Runtime strategy handle. It is immutable package state, never request
   * authorization or a cached plan. */
  std::shared_ptr<const CooperativePlacementStrategy> defaultPlacement;
  // Opaque Runtime identity used to reject a placement handle borrowed from a
  // different Runtime state. It carries no request or authorization data.
  std::shared_ptr<void> runtimeBinding;
  /** Reference-only source identity emitted by User::request. */
  std::optional<NativeModelArtifactReference> modelReference;
  /** Prepare-time canonical publication receipt; request binding only derives
   * role names from it and never performs another Core publication. */
  std::optional<NativePreparedCanonicalPublication> preparedPublication;
  /** Present only for source-free stored mode. Owns the private handle and
   * root-authenticated object index for the lifetime of this prepared view.
   */
  std::shared_ptr<const NativeStoredModelBinding> storedModel;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PREPARED_MODEL_PACKAGE_HPP
