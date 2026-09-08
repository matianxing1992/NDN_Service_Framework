#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupKeyAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanProjectionBuilder.hpp"

namespace ndnsf::di {
/** Connect sealed dependencies to Core-wrapped group capabilities and dataflow.
 * This initial-request owner rejects TOKEN_FEEDBACK until generation supplies
 * its dedicated endpoint contract; it never silently omits that authorization.
 */
class NativeGroupProjectionBuilder
{
public:
  static std::map<std::string, NativeRoleProjectionInputs> build(
    const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
    const NativeGroupKeyAdmission& keys, NativeProjectionContext context,
    std::uint64_t maxBytes = 64ULL << 20);
};
} // namespace ndnsf::di
