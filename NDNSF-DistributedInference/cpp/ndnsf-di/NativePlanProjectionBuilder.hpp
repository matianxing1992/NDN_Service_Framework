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
};

class NativePlanProjectionBuilder final
{
public:
  static std::map<std::string, NativeRoleProjectionInputs> build(
    const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
    const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context);
};

} // namespace ndnsf::di
