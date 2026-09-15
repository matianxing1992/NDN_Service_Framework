#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"

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
  /** Return a copy of the verified owned source for preparation identity checks. */
  NativeCanonicalSource sourceFor(const NativeModelDescriptor& model) const;
  NativeSplitCandidate bindStateContracts(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
    const NativeRequestControl& control) const;
  std::shared_ptr<NativeRequestPreparation> makePreparation(
    std::shared_ptr<ndn_service_framework::ServiceUser> user, std::string serviceName) const;

private:
  friend class NativeCanonicalCatalogTestAccess;
  friend struct Spec185PreparedModelTestAccess;
  using PublisherFactory = std::function<NativeCanonicalArtifactPublisher(
    NativeCanonicalPublicationOptions, NativeCanonicalArtifactPublisher::SourcePort)>;
  std::shared_ptr<NativeRequestPreparation> makePreparation(PublisherFactory factory) const;
  struct State;
  // Test-only lifetime probe. The returned weak pointer observes the exact
  // catalog source owner; it is never used by production request paths.
  std::weak_ptr<const NativeCanonicalSource> sourceLifetimeForTest(
    const NativeModelDescriptor& model) const;
  std::shared_ptr<const State> m_state;
};

} // namespace ndnsf::di
