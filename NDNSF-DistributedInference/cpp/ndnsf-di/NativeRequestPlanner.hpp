#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeConversationTurn;

/** Type-erased cooperative ports captured by one request operation. */
struct NativeStrategyPorts
{
  NativeStrategyIdentity splitterIdentity;
  NativeStrategyIdentity placementIdentity;
  std::function<std::vector<NativeSplitCandidate>(
    const NativeModelDescriptor&, const NativeGraphSnapshot&,
    const NativeCandidateBudget&, const ExtensionControl&)> enumerate;
  std::function<NativeRolePlacementProposalV3(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl&)> proposeRoles;
};

/** Operator-owned request runtime policy. Cryptographic/key owners are concrete
 * native clients; caller strategies cannot replace admission or grant checks. */
struct NativeRequestRuntime
{
  NativeRequestContract contract;
  // Explicit operator policy; compatibility deployments keep raw options.
  std::string optionsTransport = "INLINE";
  std::string requesterIdentity;
  std::string protectionEpoch;
  std::string inputLayoutDigest;
  NativeSecurityPolicySnapshot security;
  NativeCandidateBudget budget;
  std::shared_ptr<const NativeAuthenticatedGrantClient> grants;
  std::shared_ptr<const NativeCanonicalPreparationCatalog> catalog;
  std::shared_ptr<const NativePreparedPlanningCache> planningCache;
  NativeStateTensorMapping stateMapping;
  std::uint64_t noProgressMs = 5000;
  std::size_t maxSegments = 4096;
  // Shared by this runtime's request operations; never an authorization lease.
  std::shared_ptr<NativeFixedRoleAssignmentCache> fixedRoleAssignments =
    std::make_shared<NativeFixedRoleAssignmentCache>();
  std::function<NativeFixedRoleSourceV1(
    const NativeSelectionProjectionV3&, const NativeFixedRoleAssignmentV1&,
    const NativeRequestControl&)> fixedRoleSourcePublisher;
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
  std::optional<NativeGrantLease> grantLease;
  /** Set on the first pass to suspend planning before artifact publication. */
  std::optional<NativePlacementProbeIntentV1> placementProbeIntent;
  /** Owned admission result for a cache read; never supplied by the strategy. */
  std::shared_ptr<const NativeOfferBindingContext> placementProbeCacheContext;
  // Keep source serving pins alive through this operation and replacements.
  std::vector<std::shared_ptr<const NativeFixedRoleSourceV1>> fixedRoleSources;
  // A committed request keeps its assignment for every token, even if a later
  // request replaces the reusable cache entry.
  std::vector<std::shared_ptr<const NativeFixedRoleAssignmentV1>> fixedAssignments;
};

/** The exact first-pass intent plus responses received before the attempt deadline. */
struct NativePlacementProbeRoundV1
{
  NativePlacementProbeIntentV1 intent;
  std::map<std::string, std::string> intentNonceByObserver;
  std::map<std::string, NativeJson> responseByObserver;
};

/** Select only fully admitted observers with estimator-confirmed unstable trains.
 * A caller may remeasure each selected observer once using a fresh scope;
 * invalid, missing, expired and other unknown samples do not qualify. */
NativePlacementProbeIntentV1 selectNativePlacementProbeRemeasurementV1(
  const NativeOfferBindingContext& context,
  const NativePlacementProbeRoundV1& round,
  std::uint64_t nowMs);

struct NativePlacementObservationSnapshotV4
{
  NativeJson value;
  std::string digest;
  std::uint64_t capturedAtMs = 0;
  std::uint64_t expiresAtMs = 0;
};

/** Complete source-bound bundle payload estimate for one exact initial input.
 * Missing/foreign members, static dimension conflicts and overflow stay unknown.
 * This is planning metadata, never an execution or encrypted-wire byte limit. */
std::optional<std::uint64_t> estimateNativeRequestTransferBytesV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const std::string& inputDigest, const NativeDependencySpec& dependency,
  const std::vector<NativeSelectionRoleV3>& roles);

/** Resolve an operator-owned source shape template from this request's inline
 * tensor input. Returns unknown on any missing binding or invalid geometry.
 * The caller must restrict use to initial context; no payload authority changes. */
std::optional<NativeJson> bindNativeRequestTransferProfileV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const NativeEncodedRequest& request);

/** Context-aware variant. turn must be issued by the conversation owner after
 * authenticating its parent checkpoint; no new checkpoint authentication is
 * delegated to an advisory geometry estimator. */
std::optional<NativeJson> bindNativeRequestTransferProfileV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const NativeEncodedRequest& request, const NativeConversationTurn* turn);

/** Same source-bound estimate including the actual tensor-bundle framing.
 * Does not predict a producer's chunk policy or unresolved dimensions. */
std::optional<std::uint64_t> estimateNativeRequestTransferEncodedBytesV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const std::string& inputDigest, const NativeDependencySpec& dependency,
  const std::vector<NativeSelectionRoleV3>& roles);

/** Merge one bounded targeted round without mutating the ACK-frozen source snapshot. */
NativePlacementObservationSnapshotV4 mergeNativePlacementProbeRoundV1(
  const NativePlacementObservationSnapshotV4& snapshot,
  const NativePlacementProbeRoundV1& round,
  const NativeOfferBindingContext& context,
  std::uint64_t nowMs);

/** True only for a complete cache reply admitted by the normal targeted-row
 * validator against current ACK boots. Original TTL/quality stay authoritative.
 * The caller must first authenticate the response signer as this observer. */
bool isCompleteNativePlacementProbeCacheResponseV1(
  const NativeOfferBindingContext& context,
  const NativePlacementProbeObserverV1& observer,
  const std::string& nonce, const NativeJson& response,
  std::uint64_t nowMs);

/** Freeze one exact, authenticated V4 roster and only those link rows whose
 * endpoint boots and observer topology still match the signed provider offers. */
NativePlacementObservationSnapshotV4 buildNativePlacementObservationSnapshotV4(
  const NativeOfferBindingContext& context,
  const std::string& ackClosedDigest,
  const std::vector<NativeAdmittedOfferV4>& offers,
  const std::vector<NativeJson>& requesterProviderSamples,
  const NativePreparedPlanningCache& planningCache,
  std::size_t maxCandidates,
  std::uint64_t nowMs,
  const NativeGraphSnapshot* modelGraph = nullptr,
  const NativeJson* requestTransferProfile = nullptr);

/** Select the optional V4 observation profile atomically from one complete
 * authenticated ACK roster. Any missing, duplicate, stale, or invalid offer
 * returns nullopt so the caller retains the V3 path. */
std::optional<NativePlacementObservationSnapshotV4>
trySelectNativePlacementObservationSnapshotV4(
  const NativeOfferBindingContext& context,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeOfferAdmission& admission,
  const NativePreparedPlanningCache& planningCache,
  std::size_t maxCandidates,
  std::uint64_t nowMs,
  const NativeGraphSnapshot* modelGraph = nullptr,
  std::string* unavailableReason = nullptr,
  const NativeJson* requestTransferProfile = nullptr);

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
  const NativeConversationTurn* conversationTurn = nullptr,
  const NativePlacementProbeRoundV1* placementProbeRound = nullptr);

/** Cooperative strategy entry used by the prepared Runtime. */
NativePlannedRequest planNativeRequestCooperative(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const CooperativeModelSplitStrategy& splitter,
  const CooperativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn = nullptr,
  const NativePlacementProbeRoundV1* placementProbeRound = nullptr);

/** Request-owned data reuse overloads; original entry points retain fresh decode. */
std::optional<NativePlacementObservationSnapshotV4>
trySelectNativePlacementObservationSnapshotV4(
  const NativeOfferBindingContext& context,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeOfferAdmission& admission,
  const NativePreparedPlanningCache& planningCache,
  std::size_t maxCandidates, std::uint64_t nowMs,
  const NativeGraphSnapshot* modelGraph, std::string* unavailableReason,
  const NativeJson* requestTransferProfile, NativeOfferDecodeCache* decodeCache);

NativePlannedRequest planNativeRequest(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const NativeModelSplitStrategy& splitter, const NativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn,
  const NativePlacementProbeRoundV1* placementProbeRound,
  NativeOfferDecodeCache* decodeCache);

NativePlannedRequest planNativeRequestCooperative(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const CooperativeModelSplitStrategy& splitter,
  const CooperativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn,
  const NativePlacementProbeRoundV1* placementProbeRound,
  NativeOfferDecodeCache* decodeCache);

} // namespace ndnsf::di
