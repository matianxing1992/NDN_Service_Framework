#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"

#include <optional>

namespace ndnsf::di {

struct Spec185PreparedModelTestAccess;

/** Bootstrap supplies authenticated catalog facts and pinned source bytes.
 * Construction checks the source against those facts; it does not authenticate
 * remote metadata. Sources are transferred by value into immutable ownership. */
struct NativeCanonicalCatalogEntry
{
  NativeInspectedModel model;
  NativeCanonicalSource source;
  NativeRoleRecipeProfile recipe;
  NativeCanonicalRolePreparer::NodeMap nodes;
  std::optional<NativeOnnxGraphInspection> sourceGraphInspection;
  NativeCanonicalPublicationOptions publication;
  NativeCatalogModelAdapter::Format format = NativeCatalogModelAdapter::Format::OpaqueBytes;
  std::size_t maxPayloadBytes = 0;
  NativeCatalogModelAdapter::ConversationTokenEncoder conversationTokenEncoder;
};

class NativeCanonicalPreparationCatalog
{
public:
  NativeCanonicalPreparationCatalog(std::vector<NativeCanonicalCatalogEntry> entries,
    const NativeAssemblyControl& control);
  std::shared_ptr<const NativeAdapterRegistry> adapters() const;
  /** Return an owning view of the verified source without copying its
   * multi-gigabyte canonical buffers.  The view keeps the immutable source
   * alive while a publisher or identity check is using it, even if the
   * catalog releases its transient owner concurrently. */
  std::shared_ptr<const NativeCanonicalSource> sourceRefFor(
    const NativeModelDescriptor& model) const;
  /** Return a copy of the verified owned source for preparation identity checks. */
  NativeCanonicalSource sourceFor(const NativeModelDescriptor& model) const;
  /** Return the immutable publication profile frozen with the model entry. */
  NativeCanonicalPublicationOptions publicationFor(const NativeModelDescriptor& model) const;
  NativeSplitCandidate bindStateContracts(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
    const NativeRequestControl& control) const;
  std::vector<NativeSelectionRoleV3> prepareRoles(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeRequestControl& control) const;
  std::shared_ptr<NativeRequestPreparation> makePreparation(
    std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName) const;
  std::shared_ptr<NativeRequestPreparation> makePreparation(
    std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName,
    std::optional<NativePreparedCanonicalPublication> preparedPublication) const;

  NativePreparedCanonicalPublication preparePublication(
    std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName,
    const NativeModelDescriptor& model, const NativeRequestControl& control) const;

private:
  friend class ModelPreparationCache;
  friend class NativeCanonicalCatalogTestAccess;
  friend struct Spec185PreparedModelTestAccess;
  using PublisherFactory = std::function<NativeCanonicalArtifactPublisher(
    NativeCanonicalPublicationOptions, NativeCanonicalArtifactPublisher::SourcePort)>;
  std::shared_ptr<NativeRequestPreparation> makePreparation(PublisherFactory factory) const;
  std::shared_ptr<NativeRequestPreparation> makePreparation(PublisherFactory factory,
    std::optional<NativePreparedCanonicalPublication> preparedPublication) const;
  struct State;
  // Test-only lifetime probe. The returned weak pointer observes the exact
  // catalog source owner; it is never used by production request paths.
  std::weak_ptr<const NativeCanonicalSource> sourceLifetimeForTest(
    const NativeModelDescriptor& model) const;
  /** Drop the transient preparation source after publication/package creation. */
  void releaseTransientSource() const noexcept;
  /** Drop full source/initializer buffers while retaining the authenticated
   * material manifest for a material-backed publication. */
  void releaseTransientSourceBytes() const;
  std::shared_ptr<const State> m_state;
};

} // namespace ndnsf::di
