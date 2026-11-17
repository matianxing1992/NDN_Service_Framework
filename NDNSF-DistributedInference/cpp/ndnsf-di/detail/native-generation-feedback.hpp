#ifndef NDNSF_DISTRIBUTED_INFERENCE_DETAIL_NATIVE_GENERATION_FEEDBACK_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DETAIL_NATIVE_GENERATION_FEEDBACK_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeExecutionPlan;

/** Requester-owned feedback topology derived from admitted v2 step ports. */
struct NativeGenerationFeedbackTopology
{
  std::string producerRole;
  std::vector<std::string> consumerRoles;
};

/** Require one declared sampler and deliver tokens only to actual epoch-input consumers. */
NativeGenerationFeedbackTopology
nativeGenerationFeedbackTopology(const NativeExecutionPlan& plan,
                                  const std::string& selectedEgressRole,
                                  const std::string& tokenInputName);

/** Bind requester-owned feedback scopes and bounded operation stride to a local plan. */
void
bindNativeGenerationFeedback(NativeExecutionPlan& plan,
                              const NativeGenerationFeedbackTopology& topology,
                              const std::string& tokenInputName, const std::string& requestId,
                              std::uint64_t attempt,
                              const std::map<std::string, std::string>& providerByRole);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DETAIL_NATIVE_GENERATION_FEEDBACK_HPP
