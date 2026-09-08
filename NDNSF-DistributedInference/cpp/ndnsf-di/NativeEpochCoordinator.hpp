#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EPOCH_COORDINATOR_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EPOCH_COORDINATOR_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

// Recompute the existing Provider state commitment from a complete transcript.
// The original prefill boundary is required because this is a chained digest,
// distinct from the conversation checkpoint's canonical token-list digest.
std::string nativeGenerationStatePrefixDigest(const std::vector<std::int64_t>& tokens,
  std::size_t initialPromptTokenCount, const std::string& tokenizerDigest);

enum class NativeEpochStopReason
{
  Cancelled,
  Deadline,
};

/**
 * Request-scoped coordinator for a pipeline whose terminal role produces one
 * token per inference epoch.  It deliberately uses NativeModelRunner::run()
 * once per role/epoch.  The existing runStreamed() adapter remains the
 * one-Provider compatibility path and is never used here.
 */
struct NativeEpochCoordinatorConfig
{
  NativeEpochCoordinatorConfig(NativeProviderRuntime& runtime,
                               NativeExecutionPlan plan,
                               NativeProviderAssignment assignment,
                               std::shared_ptr<DependencyIo> io)
    : runtime(runtime)
    , plan(std::move(plan))
    , assignment(std::move(assignment))
    , io(std::move(io))
  {
  }

  NativeProviderRuntime& runtime;
  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::shared_ptr<DependencyIo> io;
  std::string sessionId;
  std::string requestId;
  std::uint64_t attemptEpoch = 1;
  // Request-scoped stream incarnation. This is distinct from the retry
  // attempt and remains constant across all token epochs in one stream.
  std::uint64_t streamEpoch = 1;
  // Digest of the authenticated PlacementPlanCoreV3/accepted projection that
  // authorized this generation. It is not inferred from model bytes.
  std::string lineagePlanDigest;
  std::string localProvider;
  std::string role;
  std::map<std::string, TensorBundle> initialInputs;
  std::string finalResponseScope = "final-response";
  std::size_t maxEpochs = 0;
  std::string tokenInputName = "input_ids";
  std::vector<std::string> stateInputNames;
  std::vector<std::string> stateOutputNames;
  // Immutable adapter/runner/authority fields. The coordinator derives the
  // exact per-epoch prefix/count/cache fields from admitted inputs and the
  // committed predecessor; it never accepts them from an application payload.
  std::optional<DecodeStateIdentityV1> stateIdentityTemplate;
  // Optional fresh-request continuation.  The binding identifies the exact
  // retained Provider-local state from the parent conversation epoch; it is
  // never reused as the new request's decode identity.
  std::optional<ConversationStateBinding> conversationStateBinding;
  std::uint64_t conversationStateLookupNowMs = 0;
  // Adapter-certified policy seed used to derive the exact runtime position
  // digest for each accepted logical token prefix.
  std::string positionPolicyDigest;
  std::set<std::int64_t> eosTokenIds;
  std::string samplingDigest;
  // Authenticated terminal-role sampling parameters.  They are copied from
  // the V3 Selection contract; a Provider may not silently substitute local
  // defaults for a sealed request.
  std::string samplingMode = "Greedy";
  double samplingTemperature = 0.0;
  std::size_t samplingTopK = 1;
  double samplingTopP = 1.0;
  double samplingRepetitionPenalty = 1.0;
  std::uint64_t samplingSeed = 1'750'001;
  std::vector<std::string> stopStrings;
  // The standalone adapter owns tokenizer implementation.  Native Core only
  // receives a bounded decoder callback and never embeds Transformers/PyTorch.
  std::function<std::string(const std::vector<std::int64_t>&)> textDecoder;
  // Stable streaming decode may withhold an incomplete replacement suffix;
  // `final=true` must be byte-identical to the full decoder result.
  std::function<std::string(const std::vector<std::int64_t>&, bool)> stableTextDecoder;
  bool requireTextOutput = false;
  // Conversation-enabled turns may need one bounded state-only pass through
  // non-terminal roles after the terminal role has emitted its final token.
  // The pass carries the final token lineage, publishes no event, and is
  // never enabled for ordinary streamed invocations.
  bool checkpointFinalize = false;
  std::size_t maxCheckpointFinalizeTokens = 32;
  // Attempt 2 recomputes these committed tokens from the original prompt,
  // verifies them exactly, and suppresses duplicate application events.
  std::vector<std::int64_t> committedPrefixTokenIds;
  // Production lifecycle check supplied by the selected collaboration.  It is
  // evaluated before every new epoch and again at publication/commit
  // boundaries so cancellation or the absolute request deadline cannot admit
  // another token or state successor.
  std::function<std::optional<NativeEpochStopReason>()> stopCheck;
  // Same request authority used by prepared roles, propagated through queued
  // execution and state staging; no generation-specific authorization owner.
  std::function<void()> executionGuard;
  RoleExecutionContext::StreamEventSink eventSink;
  std::function<void(const RoleSpec&, const ProviderRoleResult&)> resultObserver;
};

struct NativeEpochCoordinatorResult
{
  struct RuntimeMetricsObservation
  {
    std::string role;
    std::uint64_t inferenceEpoch = 0;
    NativeRuntimeMetrics metrics;
  };

  struct CacheObservation
  {
    std::uint64_t inferenceEpoch = 0;
    // Sum of non-state tensor elements actually admitted to this role/epoch.
    // Token inputs therefore count tokens; activation inputs count activation
    // elements. Provider-local decode-state tensors are deliberately excluded.
    std::size_t actualNewInputExtent = 0;
    std::size_t representedPrefixTokenCount = 0;
    std::size_t prefixWorkAvoided = 0;
    bool decodeStateHit = false;
    bool conversationStateHit = false;
  };

  std::optional<std::vector<std::uint8_t>> finalPayload;
  std::size_t epochsExecuted = 0;
  std::size_t eventsPublished = 0;
  std::size_t prefixTokensRecomputed = 0;
  bool stoppedByUpstream = false;
  // Exact role projection whose candidate decode state was last committed.
  // Callers may promote only this finalized identity into conversation state;
  // reconstructing a role after the coordinator returns can select the wrong
  // inference epoch or prefix identity.
  std::optional<RoleSpec> finalizedRole;
  std::vector<CacheObservation> cacheObservations;
  std::vector<RuntimeMetricsObservation> runtimeMetrics;
};

NativeEpochCoordinatorResult
runNativeEpochCoordinator(NativeEpochCoordinatorConfig config);

bool
nativeRoleHasOnlyInternalFeedbackOutputs(const RoleSpec& role);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EPOCH_COORDINATOR_HPP
