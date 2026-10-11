#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"

namespace ndnsf::di {

struct NativeDependencyProjectionBinding
{
  std::string groupId, groupEpoch;
  std::map<std::string, std::string> producerNamespaces;
  std::uint64_t operationIndex = 0;
};

struct NativeTensorPrefetchEstimate
{
  std::uint64_t bundleBytes = 0;
  std::vector<std::string> tensorNames;
};

/** Values frozen by the request and Core group owners, not a new authority. */
struct NativeProjectionContext
{
  std::uint64_t nowMs = 0, noProgressMs = 0;
  std::size_t maxSegments = 0;
  // Required iff the candidate declares an application input role.
  std::string logicalInputDigest, inputLayoutDigest;
  // Exact index in the sealed dependency vector; feedback edges are omitted.
  std::map<std::size_t, NativeDependencyProjectionBinding> dependencies;
  std::map<std::string, std::string> groupCapabilitiesByRole;
  // Current authenticated ACK declarations and exact input-bound estimates.
  std::map<std::string, std::uint64_t> tensorSegmentBytesByProvider;
  std::map<std::size_t, NativeTensorPrefetchEstimate> tensorPrefetchByDependency;
  bool inlineFirstSegment = false;
  std::shared_ptr<const NativeStaticProjectionLayout>* layoutOwner = nullptr;
};

class NativePlanProjectionBuilder final
{
public:
  static std::map<std::string, NativeRoleProjectionInputs> build(
    const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
    const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context);
  static std::map<std::string, NativeRoleProjectionInputs> build(
    const NativeOwnedSealedPlan& sealed, const NativeSplitCandidate& candidate,
    const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context);
  /** Refresh all endpoint, readiness and dataflow bindings after authorized remapping. */
  static void certify(std::map<std::string, NativeRoleProjectionInputs>& projections,
                      const NativeSealedPlan& sealed);

private:
  friend class NativeGroupProjectionBuilder;
  // The caller has validated this same sealed plan for this construction.
  static std::map<std::string, NativeRoleProjectionInputs> buildFromValidatedSealed(
    const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
    const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context);
};

} // namespace ndnsf::di
