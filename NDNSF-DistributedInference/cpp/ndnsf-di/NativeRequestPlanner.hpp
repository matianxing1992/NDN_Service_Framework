#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

namespace ndnsf::di {

struct NativeConversationTurn;

/** Operator-owned request runtime policy. Cryptographic/key owners are concrete
 * native clients; caller strategies cannot replace admission or grant checks. */
struct NativeRequestRuntime
{
  NativeRequestContract contract;
  std::string requesterIdentity;
  std::string protectionEpoch;
  std::string inputLayoutDigest;
  NativeSecurityPolicySnapshot security;
  NativeCandidateBudget budget;
  std::shared_ptr<const NativeAuthenticatedGrantClient> grants;
  std::shared_ptr<const NativeCanonicalPreparationCatalog> catalog;
  NativeStateTensorMapping stateMapping;
  std::uint64_t noProgressMs = 5000;
  std::size_t maxSegments = 4096;
};

/** Parse an operator-owned runtime policy and bind it to an already checked
 * catalog and native grant owner. JSON carries policy only; catalog/source
 * and key material remain owned by their native loaders. */
NativeRequestRuntime nativeRequestRuntimeFromJson(
  const std::string& configurationJson,
  const NativeRequestCatalog& catalog,
  std::shared_ptr<const NativeAuthenticatedGrantClient> grants);

struct NativePlannedRequest
{
  NativeSealedPlan sealed;
  ndn_service_framework::CollaborationPlan corePlan;
  std::string terminalProvider;
};

/** Worker-only ACK-closed planning. No plan is committed here; the operation
 * owner rechecks cancellation/deadline before posting the returned Core plan. */
NativePlannedRequest planNativeRequest(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const NativeModelSplitStrategy& splitter, const NativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn = nullptr);

} // namespace ndnsf::di
