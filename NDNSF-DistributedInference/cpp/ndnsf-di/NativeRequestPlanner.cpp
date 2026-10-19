#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "ndn-service-framework/NetworkProbeCachePolicy.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePlacementObservationExpired.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeTensorTransportDeclaration.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <future>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <openssl/evp.h>

namespace ndnsf::di {
// Only this translation unit can construct the process-local scope snapshot.
// The public lease remains a value, so also compare it with the original scope.
class NativeGrantLeaseFixedContext final
{
public:
  NativeGrantLeaseFixedContext(const NativePlacementPlanCore& core, NativeGrantLeaseScope scope)
    : m_scope(std::move(scope)), m_model(core.modelDigest), m_graph(core.graphDigest),
      m_manifest(core.artifacts.manifestDigest), m_artifacts(core.artifactDigestByRole),
      m_fixedCore(core.fixedCoreOwner),
      m_roles(core.fixedCoreOwner ? std::map<std::string, NativeSelectionRoleV3>{} : core.roleContracts()) {}

  bool matches(const NativePlacementPlanCore& core, const NativeConversationTurn& turn,
               const NativeSecurityPolicySnapshot& security, const NativeGrantLeaseScope& scope) const
  {
    const auto& roles = m_fixedCore ? m_fixedCore->roles() : m_roles;
    if (m_scope.conversationId != turn.parent.conversationId ||
        m_scope.expiresAtMs != turn.parent.retentionDeadlineMs ||
        m_scope.requesterIdentity != core.requesterIdentity ||
        m_scope.serviceName != core.executionPlan.serviceName ||
        m_scope.securityPolicySnapshotDigest != security.policyDigest ||
        m_scope.protectionEpoch != core.protectionEpoch ||
        m_scope.providerByRole != core.assignment.providerByRole ||
        m_model != core.modelDigest || m_graph != core.graphDigest ||
        m_manifest != core.artifacts.manifestDigest || m_artifacts != core.artifactDigestByRole ||
        roles.size() != core.roleContracts().size() ||
        scope.conversationId != m_scope.conversationId || scope.requesterIdentity != m_scope.requesterIdentity ||
        scope.serviceName != m_scope.serviceName || scope.requestId != m_scope.requestId ||
        scope.attempt != m_scope.attempt || scope.planCoreDigest != m_scope.planCoreDigest ||
        scope.scopeDigest != m_scope.scopeDigest || scope.expiresAtMs != m_scope.expiresAtMs ||
        scope.securityPolicySnapshotDigest != m_scope.securityPolicySnapshotDigest ||
        scope.protectionEpoch != m_scope.protectionEpoch || scope.providerByRole != m_scope.providerByRole)
      return false;
    for (const auto& [name, role] : roles) {
      const auto current = core.roleContracts().find(name);
      if (current == core.roleContracts().end() || !nativeSelectionRoleContractsEqualV3(role, current->second))
        return false;
    }
    return true;
  }

  const NativeGrantLeaseScope& scope() const { return m_scope; }

private:
  NativeGrantLeaseScope m_scope;
  std::string m_model, m_graph, m_manifest;
  std::map<std::string, std::string> m_artifacts;
  std::shared_ptr<const NativeFixedPlacementCoreV1> m_fixedCore;
  std::map<std::string, NativeSelectionRoleV3> m_roles;
};

namespace {
constexpr std::uint64_t MAX_REQUESTER_SAMPLE_AGE_MS = 5000;

bool isDigestValue(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::pair<NativeJson, std::string> dependencyByteEstimate(
  const NativeDependencySpec& dependency, const NativeGraphSnapshot* modelGraph)
{
  if (dependency.expectedBytes != 0)
    return {NativeJson(dependency.expectedBytes), "execution-plan"};
  if (!modelGraph || dependency.tensors.empty())
    return {NativeJson(nullptr), {}};

  std::uint64_t total = 0;
  for (const auto& tensorName : dependency.tensors) {
    const NativeGraphEdge* match = nullptr;
    for (const auto& edge : modelGraph->edges) {
      if (edge.tensor.name != tensorName)
        continue;
      if (match)
        return {NativeJson(nullptr), {}};
      match = &edge;
    }
    if (!match || !match->tensor.estimatedBytes)
      return {NativeJson(nullptr), {}};
    match->tensor.validate();
    if (*match->tensor.estimatedBytes >
        std::numeric_limits<std::uint64_t>::max() - total)
      return {NativeJson(nullptr), {}};
    total += *match->tensor.estimatedBytes;
  }
  return {NativeJson(total), "graph-tensor-contract"};
}

std::string placementProbeScopeDigest(const NativeOfferBindingContext& context)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", context.requestId}, {"attempt", context.attempt},
    {"requester_identity", context.requesterIdentity},
    {"deadline_ms", context.deadlineMs},
    {"service", context.serviceName}, {"model_identity_hash", context.modelDigest},
    {"graph_digest", context.graphDigest},
    {"candidate_set_digest", context.candidateSetDigest},
    {"candidate_artifact_set_digest", context.candidateArtifactSetDigest},
    {"rtt_pairs", context.allowedRttPairs},
    {"goodput_edges", context.allowedGoodputEdges}}));
}

bool sameStrategyIdentity(const NativeStrategyIdentity& left,
                          const NativeStrategyIdentity& right)
{
  return left.name == right.name && left.version == right.version &&
    left.configurationDigest == right.configurationDigest &&
    left.deterministic == right.deterministic;
}

bool sameOfferBindingContext(const NativeOfferBindingContext& left,
                             const NativeOfferBindingContext& right)
{
  return left.requestId == right.requestId && left.attempt == right.attempt &&
    left.serviceName == right.serviceName && left.modelDigest == right.modelDigest &&
    left.graphDigest == right.graphDigest && left.deadlineMs == right.deadlineMs &&
    left.preferredProvidersByRole == right.preferredProvidersByRole &&
    left.candidateSetDigest == right.candidateSetDigest &&
    left.candidateArtifactSetDigest == right.candidateArtifactSetDigest &&
    left.requesterIdentity == right.requesterIdentity &&
    left.candidateArtifacts == right.candidateArtifacts &&
    left.candidateProviders == right.candidateProviders &&
    left.probeScopeDigest == right.probeScopeDigest &&
    left.allowedRttPairs == right.allowedRttPairs &&
    left.allowedGoodputEdges == right.allowedGoodputEdges &&
    left.placementObservationSnapshot == right.placementObservationSnapshot &&
    left.placementObservationSnapshotDigest == right.placementObservationSnapshotDigest &&
    left.placementObservationFallbackReason == right.placementObservationFallbackReason &&
    left.placementProbeRoundCompleted == right.placementProbeRoundCompleted;
}

NativeJson placementProbePairsJson(
  const std::vector<NativePlacementProbeEdgeV1>& edges)
{
  NativeJson result = NativeJson::array();
  for (const auto& edge : edges)
    result.push_back({{"src_provider", edge.sourceProvider},
                      {"dst_provider", edge.destinationProvider}});
  return result;
}

void validatePlacementProbeIntent(const NativePlacementProbeIntentV1& intent,
                                  const NativeOfferBindingContext& context)
{
  if (intent.observers.empty() || intent.observers.size() > 16 ||
      context.candidateProviders.empty() || context.candidateProviders.size() > 16 ||
      !std::is_sorted(context.candidateProviders.begin(), context.candidateProviders.end()) ||
      std::adjacent_find(context.candidateProviders.begin(), context.candidateProviders.end()) !=
        context.candidateProviders.end())
    throw std::invalid_argument("placement probe intent has an empty/oversized observer set or invalid roster (providers=" +
      std::to_string(context.candidateProviders.size()) + ", observers=" +
      std::to_string(intent.observers.size()) + ")");

  std::set<std::string> providers(context.candidateProviders.begin(),
                                  context.candidateProviders.end());
  std::set<std::pair<std::string, std::string>> allowedGoodputEdges;
  std::set<std::pair<std::string, std::string>> allowedRttPairs;
  for (const auto& pair : context.allowedRttPairs) {
    if (pair.is_object() && pair.contains("src_provider") &&
        pair.at("src_provider").is_string() && pair.contains("dst_provider") &&
        pair.at("dst_provider").is_string()) {
      allowedRttPairs.emplace(pair.at("src_provider").get<std::string>(),
                              pair.at("dst_provider").get<std::string>());
    }
  }
  for (const auto& edge : context.allowedGoodputEdges) {
    if (edge.is_object() && edge.contains("src_provider") &&
        edge.at("src_provider").is_string() && edge.contains("dst_provider") &&
        edge.at("dst_provider").is_string()) {
      allowedGoodputEdges.emplace(edge.at("src_provider").get<std::string>(),
                                  edge.at("dst_provider").get<std::string>());
    }
  }
  std::set<std::string> observers;
  std::size_t totalEdges = 0;
  std::string previousObserver;
  for (const auto& observer : intent.observers) {
    if (observer.observerProvider.empty() ||
        (!previousObserver.empty() && previousObserver >= observer.observerProvider) ||
        !providers.count(observer.observerProvider) ||
        !observers.insert(observer.observerProvider).second ||
        observer.rttPairs.size() > 16 || observer.goodputEdges.size() > 16 ||
        (observer.rttPairs.empty() && observer.goodputEdges.empty()))
      throw std::invalid_argument("placement probe intent has an invalid observer");
    previousObserver = observer.observerProvider;
    const auto validateEdges = [&](const auto& edges, bool rtt) {
      std::pair<std::string, std::string> previous;
      bool first = true;
      for (const auto& edge : edges) {
        const auto current = std::make_pair(edge.sourceProvider, edge.destinationProvider);
        if (edge.sourceProvider.empty() || edge.destinationProvider.empty() ||
            edge.sourceProvider == edge.destinationProvider ||
            !providers.count(edge.sourceProvider) ||
            !providers.count(edge.destinationProvider) ||
            (rtt ? edge.sourceProvider != observer.observerProvider :
                   edge.destinationProvider != observer.observerProvider) ||
            (rtt && allowedRttPairs.count(current) == 0) ||
            (!rtt && allowedGoodputEdges.count(current) == 0) ||
            (!first && previous >= current) || ++totalEdges > 16)
          throw std::invalid_argument("placement probe intent has an invalid directed edge");
        previous = current;
        first = false;
      }
    };
    validateEdges(observer.rttPairs, true);
    validateEdges(observer.goodputEdges, false);
  }
}

NativeJson targetedProbeScopeJson(
  const NativeOfferBindingContext& context,
  const NativePlacementProbeObserverV1& observer,
  const std::string& rosterDigest, const std::string& nonce)
{
  return NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", context.requestId}, {"attempt", context.attempt},
    {"requester_identity", context.requesterIdentity},
    {"observer_provider", observer.observerProvider},
    {"service", context.serviceName},
    {"provider_roster", context.candidateProviders},
    {"roster_digest", rosterDigest},
    {"rtt_pairs", placementProbePairsJson(observer.rttPairs)},
    {"goodput_edges", placementProbePairsJson(observer.goodputEdges)},
    {"deadline_ms", context.deadlineMs}, {"intent_nonce", nonce}};
}

void mergeTargetedPlacementProbeRound(
  NativePlacementObservationSnapshotV4& snapshot,
  const NativePlacementProbeRoundV1& round,
  const NativeOfferBindingContext& context,
  const std::uint64_t nowMs)
{
  validatePlacementProbeIntent(round.intent, context);
  if (round.intentNonceByObserver.size() != round.intent.observers.size())
    throw std::invalid_argument("placement probe nonce set does not match its intent");
  for (const auto& response : round.responseByObserver) {
    if (round.intentNonceByObserver.count(response.first) == 0)
      throw std::invalid_argument("placement probe response observer was not requested");
  }
  if (!snapshot.value.is_object() ||
      !snapshot.value.contains("eligible_link_observations") ||
      !snapshot.value.at("eligible_link_observations").is_array() ||
      !snapshot.value.contains("provider_offers") ||
      !snapshot.value.at("provider_offers").is_array())
    throw std::invalid_argument("placement probe snapshot is incomplete");

  const auto rosterDigest = nativePlanningDigest(
    nativeCanonicalJson(NativeJson(context.candidateProviders)));
  const auto hasPositiveNumberArray = [] (const NativeJson& object,
                                          const char* field,
                                          const std::size_t size) {
    if (!object.contains(field) || !object.at(field).is_array() ||
        object.at(field).size() != size)
      return false;
    return std::all_of(object.at(field).begin(), object.at(field).end(),
      [] (const NativeJson& value) {
        return value.is_number() && std::isfinite(value.get<double>()) &&
               value.get<double>() > 0.0;
      });
  };
  const auto hasPositiveUnsignedArray = [] (const NativeJson& object,
                                            const char* field,
                                            const std::size_t size) {
    if (!object.contains(field) || !object.at(field).is_array() ||
        object.at(field).size() != size)
      return false;
    return std::all_of(object.at(field).begin(), object.at(field).end(),
      [] (const NativeJson& value) {
        return value.is_number_unsigned() &&
               value.get<std::uint64_t>() > 0;
      });
  };
  std::map<std::string, const NativeJson*> offerByProvider;
  for (const auto& offer : snapshot.value.at("provider_offers")) {
    if (offer.is_object() && offer.contains("provider") &&
        offer.at("provider").is_string())
      offerByProvider.emplace(offer.at("provider").get<std::string>(), &offer);
  }
  if (offerByProvider.size() != context.candidateProviders.size())
    throw std::invalid_argument("placement probe snapshot provider roster mismatch");

  std::set<std::string> dataNames;
  auto& eligible = snapshot.value["eligible_link_observations"];
  for (const auto& requested : round.intent.observers) {
    const auto nonce = round.intentNonceByObserver.find(requested.observerProvider);
    if (nonce == round.intentNonceByObserver.end() || nonce->second.size() < 16 ||
        nonce->second.size() > 128)
      throw std::invalid_argument("placement probe intent nonce is invalid");
    const auto logRejectedResponse = [&] (std::string reason) noexcept {
      try {
        std::replace(reason.begin(), reason.end(), ' ', '_');
        logRuntimeEvidence("NDNSF_DI_NATIVE_PLACEMENT_PROBE_MERGE_UNAVAILABLE requestId=" +
          context.requestId + " observer=" + requested.observerProvider +
          " reason=" + reason);
      }
      catch (...) {
        // Merge diagnostics must never alter the safe fallback behavior.
      }
    };
    const auto responseIt = round.responseByObserver.find(requested.observerProvider);
    if (responseIt == round.responseByObserver.end()) {
      logRejectedResponse("response_missing");
      continue; // Timeout, rejection, or malformed wire: retain unknown link data.
    }

    try {
      const auto& response = responseIt->second;
      const auto encoded = nativeCanonicalJson(response);
      const auto scope = targetedProbeScopeJson(context, requested, rosterDigest, nonce->second);
      const auto scopeDigest = nativePlanningDigest(nativeCanonicalJson(scope));
      const auto& observerOffer = *offerByProvider.at(requested.observerProvider);
      const auto observerBoot = observerOffer.at("boot_epoch").get<std::string>();
      if (encoded.size() > NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES || !response.is_object() ||
          response.value("schema", std::string{}) != "ndnsf-di-placement-probe-response-v1" ||
          response.value("request_id", std::string{}) != context.requestId ||
          response.value("attempt", std::uint64_t{0}) != context.attempt ||
          response.value("requester_identity", std::string{}) != context.requesterIdentity ||
          response.value("observer_provider", std::string{}) != requested.observerProvider ||
          response.value("service", std::string{}) != context.serviceName ||
          response.value("observer_boot_epoch", std::string{}) != observerBoot ||
          response.value("scope_digest", std::string{}) != scopeDigest ||
          response.value("intent_nonce", std::string{}) != nonce->second ||
          response.value("deadline_ms", std::uint64_t{0}) != context.deadlineMs ||
          !response.contains("completed_at_ms") ||
          !response.at("completed_at_ms").is_number_unsigned() ||
          response.at("completed_at_ms").get<std::uint64_t>() > nowMs ||
          response.at("completed_at_ms").get<std::uint64_t>() >= context.deadlineMs ||
          !response.contains("observations") || !response.at("observations").is_array() ||
          response.at("observations").size() > 16 ||
          response.value("requested_rtt_count", std::uint64_t{0}) != requested.rttPairs.size() ||
          response.value("requested_goodput_count", std::uint64_t{0}) != requested.goodputEdges.size() ||
          response.value("omitted_rtt_count", std::uint64_t{0}) > requested.rttPairs.size() ||
          response.value("omitted_goodput_count", std::uint64_t{0}) > requested.goodputEdges.size() ||
          response.at("observations").size() >
            requested.rttPairs.size() + requested.goodputEdges.size())
      {
        logRejectedResponse("response_envelope_scope_or_freshness_rejected");
        continue;
      }

      std::set<std::pair<std::string, std::string>> requestedGoodput;
      for (const auto& edge : requested.goodputEdges)
        requestedGoodput.emplace(edge.sourceProvider, edge.destinationProvider);
      std::set<std::pair<std::string, std::string>> seenGoodput;
      std::set<std::pair<std::string, std::string>> requestedRtt;
      for (const auto& edge : requested.rttPairs)
        requestedRtt.emplace(edge.sourceProvider, edge.destinationProvider);
      std::set<std::pair<std::string, std::string>> seenRtt;
      std::vector<NativeJson> acceptedRows;
      std::uint64_t rowExpiry = snapshot.expiresAtMs;
      for (const auto& observation : response.at("observations")) {
        if (!observation.is_object() ||
            observation.value("observer", std::string{}) != requested.observerProvider ||
            !observation.contains("target") || !observation.at("target").is_string() ||
            !observation.contains("kind") || !observation.at("kind").is_string() ||
            !observation.contains("observed_at_ms") ||
            !observation.at("observed_at_ms").is_number_unsigned() ||
            !observation.contains("expires_at_ms") ||
            !observation.at("expires_at_ms").is_number_unsigned())
          throw std::invalid_argument("targeted placement observation identity is malformed");
        const auto target = observation.at("target").get<std::string>();
        const auto captured = observation.at("observed_at_ms").get<std::uint64_t>();
        const auto expires = observation.at("expires_at_ms").get<std::uint64_t>();
        if (captured == 0 || captured > nowMs || nowMs - captured > 5000 ||
            expires <= nowMs || expires <= captured || expires > context.deadlineMs)
          throw std::invalid_argument("targeted placement observation is stale");
        const auto targetOffer = offerByProvider.find(target);
        if (targetOffer == offerByProvider.end() ||
            !targetOffer->second->contains("boot_epoch") ||
            !observation.contains("peer_boot_epoch") ||
            observation.at("peer_boot_epoch") != targetOffer->second->at("boot_epoch"))
          throw std::invalid_argument("targeted placement peer boot does not match its offer");
        const auto dataName = observation.value("data_name", std::string{});
        if (dataName.empty() || dataName.size() > 512 || !dataNames.insert(dataName).second)
          throw std::invalid_argument("targeted placement Data name is missing or reused");

        const auto kind = observation.at("kind").get<std::string>();
        if (kind == "active_probe_goodput") {
          // Total payload/elapsed remains diagnostic. Only the dedicated
          // burst timing metadata can supply a placement rate.
          const bool profileValidated = observation.value("probe_profile_validated", false);
          const auto edge = std::make_pair(target, requested.observerProvider);
          if (!requestedGoodput.count(edge) || !seenGoodput.insert(edge).second)
            throw std::invalid_argument("targeted_goodput_edge_does_not_match_intent");
          if (!observation.value("train_complete", false) ||
              !observation.value("target_signature_validated", false) ||
              observation.value("sent_segments", std::uint64_t{0}) != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS ||
              observation.value("received_segments", std::uint64_t{0}) != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS)
            throw std::invalid_argument("targeted_goodput_train_or_signature_invalid");
          if (!observation.contains("value") || !observation.at("value").is_number() ||
              !std::isfinite(observation.at("value").get<double>()) ||
              observation.at("value").get<double>() <= 0 ||
              !observation.contains("payload_bytes") ||
              !observation.at("payload_bytes").is_number_unsigned() ||
              observation.at("payload_bytes").get<std::uint64_t>() == 0 ||
              !observation.contains("elapsed_ms") ||
              !observation.at("elapsed_ms").is_number() ||
              !std::isfinite(observation.at("elapsed_ms").get<double>()) ||
              observation.at("elapsed_ms").get<double>() <= 0)
            throw std::invalid_argument("targeted_goodput_rate_or_payload_invalid");
          if (!observation.contains("profile_digest") ||
              !observation.at("profile_digest").is_string())
            throw std::invalid_argument("targeted_goodput_profile_digest_invalid");
          constexpr auto segments = ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS;
          if (!hasPositiveNumberArray(observation, "probe_rtt_ms", segments) ||
              !hasPositiveNumberArray(observation, "probe_interest_gaps_ms", segments - 1) ||
              !hasPositiveNumberArray(observation, "probe_data_arrival_gaps_ms", segments - 1) ||
              !hasPositiveUnsignedArray(observation, "probe_data_wire_bytes", segments)) {
            const auto describeArray = [&] (const char* field, std::size_t expected,
                                            bool unsignedValues) {
              if (!observation.contains(field))
                return std::string("missing");
              if (!observation.at(field).is_array())
                return std::string("not_array");
              const auto size = observation.at(field).size();
              const auto valid = unsignedValues
                ? hasPositiveUnsignedArray(observation, field, expected)
                : hasPositiveNumberArray(observation, field, expected);
              return std::to_string(size) + (valid ? "_valid" : "_invalid");
            };
            throw std::invalid_argument(
              "targeted_goodput_chirp_samples_invalid_rtt=" +
              describeArray("probe_rtt_ms", segments, false) + "_interest=" +
              describeArray("probe_interest_gaps_ms", segments - 1, false) + "_arrival=" +
              describeArray("probe_data_arrival_gaps_ms", segments - 1, false) + "_wire=" +
              describeArray("probe_data_wire_bytes", segments, true));
          }

          NativeJson row{
            {"kind", "ACTIVE_PROBE_GOODPUT"},
            {"src_peer", target}, {"dst_peer", requested.observerProvider},
            {"src_boot_epoch", targetOffer->second->at("boot_epoch")},
            {"dst_boot_epoch", observerBoot},
            {"captured_at_ms", captured}, {"expires_at_ms", expires},
            {"goodput_mbps", observation.at("value")},
            {"payload_bytes", observation.at("payload_bytes")},
            {"elapsed_ms", observation.at("elapsed_ms")},
            {"probe_complete", true}, {"probe_segments_sent", segments},
            {"probe_signature_valid", true},
            {"probe_profile_validated", profileValidated},
            {"probe_rtt_ms", observation.at("probe_rtt_ms")},
            {"probe_interest_gaps_ms", observation.at("probe_interest_gaps_ms")},
            {"probe_data_arrival_gaps_ms",
             observation.at("probe_data_arrival_gaps_ms")},
            {"probe_data_wire_bytes", observation.at("probe_data_wire_bytes")},
            {"probe_profile_digest", observation.at("profile_digest")}};
          if (observation.at("profile_digest") ==
              ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST) {
            const auto& source = observation.at("probe_source_send_gaps_ms");
            if (!source.is_array() || source.size() != segments - 2 ||
                !observation.at("probe_source_timing_validated").is_boolean())
              throw std::invalid_argument("targeted_goodput_source_timing_shape_invalid");
            ndn_service_framework::NetworkTelemetrySnapshot timing;
            timing.kind = ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput;
            timing.probeComplete = true;
            timing.probeSegmentsSent = timing.receivedSegments = segments;
            timing.targetSignatureValidated = true;
            timing.probeSignatureValid = profileValidated;
            timing.probeProfileDigest = observation.at("profile_digest").get<std::string>();
            timing.probeSourceTimingValidated =
              observation.at("probe_source_timing_validated").get<bool>();
            for (std::size_t i = 0; i < segments - 2; ++i) {
              if (!source[i].is_number() || !std::isfinite(source[i].get<double>()) ||
                  source[i].get<double>() < 0 || source[i].get<double>() > 500)
                throw std::invalid_argument("targeted_goodput_source_timing_invalid");
              timing.probeSourceSendGapsMs[i] = source[i].get<double>();
            }
            for (std::size_t i = 0; i < segments - 1; ++i)
              timing.probeDataArrivalGapsMs[i] =
                observation.at("probe_data_arrival_gaps_ms")[i].get<double>();
            const auto metadata = nativeProbeTrainRateMetadataV4(timing);
            for (auto field = metadata.begin(); field != metadata.end(); ++field) {
              if (!observation.contains(field.key()) || observation.at(field.key()) != field.value())
                throw std::invalid_argument("targeted_goodput_train_metadata_inconsistent");
              row[field.key()] = field.value();
            }
          }
          row["source_digest"] = nativePlanningDigest(nativeCanonicalJson(observation));
          acceptedRows.push_back({{"reported_by", requested.observerProvider},
                                  {"source_kind", "TARGETED_RESPONSE"},
                                  {"row", std::move(row)}});
        }
        else if (kind == "active_probe_rtt") {
          const auto edge = std::make_pair(requested.observerProvider, target);
          if (!requestedRtt.count(edge) || !seenRtt.insert(edge).second ||
              !observation.value("target_signature_validated", false) ||
              !observation.contains("value") || !observation.at("value").is_number() ||
              !std::isfinite(observation.at("value").get<double>()) ||
              observation.at("value").get<double>() <= 0)
            throw std::invalid_argument("targeted RTT observation does not match its intent");

          NativeJson row{
            {"src_peer", requested.observerProvider}, {"dst_peer", target},
            {"direction_semantics", "REQUESTER_RESPONDER_ROUND_TRIP"},
            {"kind", "ACTIVE_PROBE_RTT"},
            {"observer_peer", requested.observerProvider},
            {"observer_boot_epoch", observerBoot},
            {"src_boot_epoch", observerBoot},
            {"dst_boot_epoch", targetOffer->second->at("boot_epoch")},
            {"topology_digest", nullptr},
            {"captured_at_ms", captured}, {"expires_at_ms", expires},
            {"sample_count", std::uint64_t{1}},
            {"rtt_ms", observation.at("value")},
            {"first_byte_ms", nullptr}, {"elapsed_ms", nullptr},
            {"payload_bytes", nullptr}, {"wire_bytes", nullptr},
            {"encoded_bytes", nullptr}, {"received_segments", nullptr},
            {"probe_profile_digest", nullptr}, {"probe_segments_sent", nullptr},
            {"probe_complete", nullptr}, {"probe_signature_valid", nullptr},
            {"probe_rtt_ms", nullptr}, {"probe_interest_gaps_ms", nullptr},
            {"probe_data_arrival_gaps_ms", nullptr},
            {"probe_data_wire_bytes", nullptr},
            {"timeout_count", std::uint64_t{0}}, {"nack_count", std::uint64_t{0}},
            {"goodput_mbps", nullptr}};
          row["source_digest"] = nativePlanningDigest(
            nativeCanonicalJson(row));
          acceptedRows.push_back({{"reported_by", requested.observerProvider},
                                  {"source_kind", "TARGETED_RESPONSE"},
                                  {"row", std::move(row)}});
        }
        else {
          throw std::invalid_argument("targeted placement observation kind is unsupported");
        }
        rowExpiry = std::min(rowExpiry, expires);
      }
      const auto omittedRtt = response.value("omitted_rtt_count", std::uint64_t{0});
      const auto omittedGoodput =
        response.value("omitted_goodput_count", std::uint64_t{0});
      // Missing rows are unavailable for their own kind. Keep independently
      // validated rows from the other kind; missing values never create cost.
      if (seenRtt.size() + omittedRtt > requested.rttPairs.size() ||
          seenGoodput.size() + omittedGoodput > requested.goodputEdges.size())
        continue;
      for (const auto& row : acceptedRows)
        eligible.push_back(row);
      snapshot.expiresAtMs = rowExpiry;
    }
    catch (const std::exception& error) {
      logRejectedResponse(error.what());
      // Optional measurement failures do not prevent the safe existing placement fallback.
      continue;
    }
  }
  snapshot.value["targeted_probe_round_completed_at_ms"] = nowMs;
  snapshot.value["targeted_probe_intent_digest"] =
    nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"observers", [&] {
        NativeJson rows = NativeJson::array();
        for (const auto& observer : round.intent.observers) {
          rows.push_back({{"observer", observer.observerProvider},
            {"rtt_pairs", placementProbePairsJson(observer.rttPairs)},
            {"goodput_edges", placementProbePairsJson(observer.goodputEdges)}});
        }
        return rows;
      }()}}));
  snapshot.value["expires_at_ms"] = snapshot.expiresAtMs;
  snapshot.digest = nativePlanningDigest(nativePlacementObservationCanonicalJson(
    snapshot.value, context.preparedPlanningTemplate.get()));
}

NativeJson snapshotAckTelemetry(
  const ndn_service_framework::AckSelectionCandidate& ack, std::uint64_t nowMs,
  const std::string& providerBootEpoch)
{
  NativeJson result{{"provider", ack.providerName.toUri()}, {"sample", nullptr}};
  if (ack.ackRttMs) {
    if (!std::isfinite(*ack.ackRttMs) || *ack.ackRttMs <= 0.0 ||
        ack.ackObservedAtMs == 0 || ack.ackObservedAtMs > nowMs ||
        nowMs - ack.ackObservedAtMs > MAX_REQUESTER_SAMPLE_AGE_MS ||
        providerBootEpoch.empty())
      return result;
    result["sample"] = NativeJson{
      {"provider", ack.providerName.toUri()},
      {"service", ack.serviceName.toUri()},
      {"kind", "ACK_RTT"},
      {"rtt_ms", *ack.ackRttMs},
      {"elapsed_ms", *ack.ackRttMs},
      {"observed_at_ms", ack.ackObservedAtMs},
      {"peer_boot_epoch", providerBootEpoch},
      {"sample_count", std::uint64_t{1}},
      {"confidence", 1.0}};
    return result;
  }
  if (!ack.telemetry)
    return result;
  const auto& sample = *ack.telemetry;
  if (sample.providerName != ack.providerName || sample.serviceName != ack.serviceName ||
      sample.stale || sample.lastUpdatedMs == 0 || sample.lastUpdatedMs > nowMs ||
      nowMs - sample.lastUpdatedMs > 5000)
    return result;

  NativeJson probeRtt = NativeJson::array();
  for (const auto value : sample.probeRttMs) {
    if (!std::isfinite(value) || value < 0) return result;
    probeRtt.push_back(value);
  }
  NativeJson interestGaps = NativeJson::array();
  for (const auto value : sample.probeInterestGapsMs) {
    if (!std::isfinite(value) || value < 0) return result;
    interestGaps.push_back(value);
  }
  NativeJson dataGaps = NativeJson::array();
  for (const auto value : sample.probeDataArrivalGapsMs) {
    if (!std::isfinite(value) || value < 0) return result;
    dataGaps.push_back(value);
  }
  NativeJson probeWireBytes = NativeJson::array();
  for (const auto value : sample.probeDataWireBytes)
    probeWireBytes.push_back(value);
  if (!std::isfinite(sample.rttMs) || sample.rttMs < 0 ||
      !std::isfinite(sample.firstByteMs) || sample.firstByteMs < 0 ||
      !std::isfinite(sample.elapsedMs) || sample.elapsedMs < 0 ||
      !std::isfinite(sample.goodputMbps) || sample.goodputMbps < 0 ||
      !std::isfinite(sample.confidence) || sample.confidence < 0 ||
      sample.confidence > 1)
    return result;
  result["sample"] = NativeJson{
    {"provider", sample.providerName.toUri()},
    {"service", sample.serviceName.toUri()},
    {"peer", sample.peerName.toUri()},
    {"edge", sample.edgeName},
    {"kind", sample.kind ==
        ndn_service_framework::NetworkTelemetrySampleKind::AckRtt ?
          "ACK_RTT" : ndn_service_framework::toString(sample.kind)},
    {"rtt_ms", sample.rttMs},
    {"first_byte_ms", sample.firstByteMs},
    {"elapsed_ms", sample.elapsedMs},
    {"encoded_bytes", sample.encodedBytes},
    {"payload_bytes", sample.payloadBytes},
    {"wire_bytes", sample.wireBytes},
    {"goodput_mbps", sample.goodputMbps},
    {"received_segments", sample.receivedSegments},
    {"probe_segments_sent", sample.probeSegmentsSent},
    {"probe_rtt_ms", std::move(probeRtt)},
    {"probe_interest_gaps_ms", std::move(interestGaps)},
    {"probe_data_arrival_gaps_ms", std::move(dataGaps)},
    {"probe_data_wire_bytes", std::move(probeWireBytes)},
    {"probe_profile_digest", sample.probeProfileDigest},
    {"probe_complete", sample.probeComplete},
    {"probe_signature_valid", sample.probeSignatureValid},
    {"timeout_count", sample.timeoutCount},
    {"nack_count", sample.nackCount},
    {"sample_count", sample.sampleCount},
    {"observed_at_ms", sample.lastUpdatedMs},
    {"confidence", sample.confidence},
    {"peer_boot_epoch", sample.peerBootEpoch},
    {"data_name", sample.dataName.toUri()}};
  result["sample"].update(nativeProbeTrainRateMetadataV4(sample));
  return result;
}

void expandStateContractsFromCatalog(NativeSplitCandidate& candidate,
                                     const NativeStateTensorMapping& mapping,
                                     const NativeModelDescriptor& model)
{
  const auto expand = [&model] (const NativeStateTensorMapping::Roles& mapped,
                                auto& declared) {
    if (mapped.empty()) return;
    for (const auto& item : mapped) {
      const auto found = declared.find(item.first);
      if (found == declared.end() || item.second.size() == found->second.size()) {
        continue;
      }
      std::vector<NativeTensorContract> replacement;
      for (const auto& semantic : item.second) {
        for (const auto& name : semantic.second) {
          replacement.push_back({name, model.precision,
                                 {"batch", "heads", "sequence", "head-dimension"},
                                 std::nullopt});
        }
      }
      if (!replacement.empty()) {
        found->second = std::move(replacement);
      }
    }
  };
  expand(mapping.inputs, candidate.roleStateInputsByRole);
  expand(mapping.outputs, candidate.roleStateOutputsByRole);
  // The mapped tensor names are part of SplitCandidate's authenticated
  // identity. Recompute the digest before the catalog binder validates the
  // candidate; otherwise a legitimate dynamic-past mapping is rejected as a
  // foreign candidate after this local expansion.
  candidate.candidateDigest = candidate.computedDigest();
}

void requireObject(const NativeJson& value, const char* field)
{
  if (!value.is_object())
    throw std::invalid_argument(std::string(field) + " must be an object");
}

void requireExactKeys(const NativeJson& value,
                      std::initializer_list<const char*> required,
                      const char* field,
                      std::initializer_list<const char*> optional = {})
{
  requireObject(value, field);
  std::set<std::string> expected;
  for (const auto* key : required) expected.emplace(key);
  for (const auto* key : optional) expected.emplace(key);
  for (const auto& item : value.items()) {
    if (!expected.count(item.key()))
      throw std::invalid_argument(std::string(field) + " contains an unknown field");
  }
  for (const auto* key : required) {
    if (!value.contains(key))
      throw std::invalid_argument(std::string(field) + " is missing a required field");
  }
}

void requireDigestValue(const NativeJson& value, const char* field)
{
  if (!value.is_string() || !isDigestValue(value.get<std::string>()))
    throw std::invalid_argument(std::string(field) + " must be a lowercase sha256 digest");
}

std::string readString(const NativeJson& value, const char* field)
{
  if (!value.is_string())
    throw std::invalid_argument(std::string(field) + " must be a string");
  return value.get<std::string>();
}

bool readBoolean(const NativeJson& value, const char* field)
{
  if (!value.is_boolean())
    throw std::invalid_argument(std::string(field) + " must be a boolean");
  return value.get<bool>();
}

std::uint64_t epochMs()
{
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (value <= 0) throw std::runtime_error("invalid request wall clock");
  return static_cast<std::uint64_t>(value);
}

// This measures only the current planning thread. In particular, grant worker
// CPU is excluded; wall minus CPU must not be labelled as network duration.
std::int64_t threadCpuTimeNs() noexcept
{
#ifdef CLOCK_THREAD_CPUTIME_ID
  const auto* enabled = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (enabled == nullptr || *enabled == '\0' || *enabled == '0')
    return -1;
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0 ||
      value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L ||
      value.tv_sec > (std::numeric_limits<std::int64_t>::max() - value.tv_nsec) / 1000000000LL)
    return -1;
  return static_cast<std::int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
#else
  return -1;
#endif
}

struct PlanPhaseTiming {
  std::chrono::steady_clock::time_point wall;
  std::int64_t threadCpuNs;
};

PlanPhaseTiming planPhaseStarted() noexcept
{
  return {std::chrono::steady_clock::now(), threadCpuTimeNs()};
}

void
logPlanPhase(const std::string& requestId, const char* phase,
             PlanPhaseTiming started) noexcept
{
  const auto* enabled = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (enabled == nullptr || *enabled == '\0' || *enabled == '0')
    return;
  try {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started.wall).count();
    const auto cpuEnded = threadCpuTimeNs();
    std::ostringstream record;
    record << "NDNSF_DI_NATIVE_PLAN_PHASE requestId=" << requestId
           << " phase=" << phase << " duration_us=" << elapsed;
    if (started.threadCpuNs >= 0 && cpuEnded >= started.threadCpuNs)
      record << " thread_cpu_us=" << (cpuEnded - started.threadCpuNs) / 1000;
    logRuntimeEvidence(record.str());
  }
  catch (...) {
    // Timing is diagnostic only and must never change planning behavior.
  }
}

void
logPlacementObservationSnapshot(const std::string& requestId,
                                const NativeJson& snapshot,
                                const std::string& digest,
                                std::uint64_t checkedAtMs,
                                const std::string& unavailableReason) noexcept
{
  const auto* enabled = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (enabled == nullptr || *enabled == '\0' || *enabled == '0')
    return;
  try {
    std::ostringstream record;
    record << "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT requestId=" << requestId
           << " status=" << (digest.empty() ? "unavailable" : "accepted")
           << " snapshotDigest=" << (digest.empty() ? "none" : digest);
    if (digest.empty() && !unavailableReason.empty())
      record << " fallbackReason=" << unavailableReason;
    if (!digest.empty() && snapshot.is_object()) {
      const auto capturedAtMs = snapshot.value("captured_at_ms", std::uint64_t{0});
      const auto expiresAtMs = snapshot.value("expires_at_ms", std::uint64_t{0});
      std::size_t offerCount = 0;
      auto providerOfferProviders = NativeJson::array();
      auto providerOfferBindings = NativeJson::array();
      if (snapshot.contains("provider_offers") && snapshot["provider_offers"].is_array()) {
        offerCount = snapshot["provider_offers"].size();
        for (const auto& offer : snapshot["provider_offers"]) {
          if (offer.is_object() && offer.contains("provider") &&
              offer["provider"].is_string()) {
            providerOfferProviders.push_back(offer["provider"]);
            const auto& values = offer.at("values");
            const auto& resources = values.at("resources");
            providerOfferBindings.push_back(NativeJson{
              {"provider", offer.at("provider")},
              {"boot_epoch", offer.at("boot_epoch")},
              {"captured_at_ms", offer.at("captured_at_ms")},
              {"expires_at_ms", offer.at("expires_at_ms")},
              {"offer_digest", offer.at("offer_digest")},
              {"ack_wire_digest", offer.at("ack_wire_digest")},
              {"signer_identity", offer.at("signer_identity")},
              {"topology_digest", resources.is_object()
                ? resources.value("topology_digest", NativeJson(nullptr)) : NativeJson(nullptr)}});
          }
        }
      }
      auto activeProbeRttRows = NativeJson::array();
      auto activeProbeGoodputRows = NativeJson::array();
      std::size_t rttCount = 0;
      std::size_t goodputCount = 0;
      if (snapshot.contains("eligible_link_observations") &&
          snapshot["eligible_link_observations"].is_array()) {
        for (const auto& observation : snapshot["eligible_link_observations"]) {
          if (!observation.is_object() || !observation.contains("row") ||
              !observation["row"].is_object())
            continue;
          const auto kind = observation["row"].value("kind", std::string{});
          const auto& row = observation["row"];
          const auto provenance = [&] (NativeJson summary) {
            summary["source_kind"] = observation.value("source_kind", std::string{});
            for (const auto* key : {"captured_at_ms", "expires_at_ms", "observer_peer",
                 "observer_boot_epoch", "src_boot_epoch", "dst_boot_epoch", "topology_digest"})
              // Targeted rows have authenticated response bindings rather
              // than all ACK resource fields. Preserve missing facts as null.
              summary[key] = row.value(key, NativeJson(nullptr));
            return summary;
          };
          if (kind == "ACTIVE_PROBE_RTT") {
            ++rttCount;
            activeProbeRttRows.push_back(provenance(NativeJson{
              {"reported_by", observation.value("reported_by", std::string{})},
              {"src_provider", observation["row"].value("src_peer", std::string{})},
              {"dst_provider", observation["row"].value("dst_peer", std::string{})}}));
          }
          else if (kind == "ACTIVE_PROBE_GOODPUT") {
            ++goodputCount;
            activeProbeGoodputRows.push_back(provenance(NativeJson{
              {"reported_by", observation.value("reported_by", std::string{})},
              {"src_provider", row.value("src_peer", std::string{})},
              {"dst_provider", row.value("dst_peer", std::string{})},
              {"sample_digest", row.value("source_digest", std::string{})},
              {"probe_complete", row.value("probe_complete", false)},
              {"probe_segments_sent", row.value("probe_segments_sent",
                                                  std::uint64_t{0})},
              {"probe_signature_valid", row.value("probe_signature_valid", false)},
              {"probe_source_send_gaps_ms", row.value(
                "probe_source_send_gaps_ms", NativeJson::array())},
              {"probe_source_timing_validated", row.value("probe_source_timing_validated", false)},
              {"probe_train_goodput_mbps", row.value("probe_train_goodput_mbps", NativeJson(nullptr))},
              {"probe_rate_status", row.value("probe_rate_status", NativeJson("UNKNOWN"))},
              {"probe_rate_reason", row.value("probe_rate_reason", NativeJson("SOURCE_TIMING_UNVERIFIED"))},
              {"probe_interest_gaps_ms", row.value(
                "probe_interest_gaps_ms", NativeJson::array())},
              {"probe_data_arrival_gaps_ms", row.value(
                "probe_data_arrival_gaps_ms", NativeJson::array())},
              {"probe_data_wire_bytes", row.value(
                "probe_data_wire_bytes", NativeJson::array())}}));
          }
        }
      }
      const auto ineligibleCount = snapshot.value(
        "ineligible_link_observation_count", std::uint64_t{0});
      const auto boundedJsonSummary = [](const NativeJson& value,
                                         const std::size_t maxItems) {
        return value.is_array() && value.size() <= maxItems
          ? value.dump() : std::string("truncated");
      };
      record << " capturedAtMs=" << capturedAtMs
             << " checkedAtMs=" << checkedAtMs
             << " ageMs=" << (capturedAtMs <= checkedAtMs ? checkedAtMs - capturedAtMs : 0)
             << " expiresAtMs=" << expiresAtMs
             << " expiresInMs=" << (expiresAtMs > checkedAtMs ? expiresAtMs - checkedAtMs : 0)
             << " providerOffers=" << offerCount
             << " providerRoster="
             << boundedJsonSummary(snapshot.value("provider_roster", NativeJson::array()), 32)
             << " providerOfferProviders="
             << boundedJsonSummary(providerOfferProviders, 32)
             << " providerOfferBindings=" << boundedJsonSummary(providerOfferBindings, 32)
             << " rttPairs="
             << boundedJsonSummary(snapshot.value("rtt_pairs", NativeJson::array()), 16)
             << " goodputEdges="
             << boundedJsonSummary(snapshot.value("goodput_edges", NativeJson::array()), 16)
             << " activeProbeRtt=" << rttCount
             << " activeProbeRttRows=" << boundedJsonSummary(activeProbeRttRows, 16)
             << " activeProbeGoodput=" << goodputCount
             << " activeProbeGoodputRows=" << boundedJsonSummary(activeProbeGoodputRows, 16)
             << " ineligibleLinkObservations=" << ineligibleCount;
      if (snapshot.contains("request_transfer_profile")) {
        const auto profile = nativeCanonicalJson(snapshot.at("request_transfer_profile"));
        record << " requestTransferProfileDigest=" << nativePlanningDigest(profile)
               << " requestTransferProfileJson=" << profile;
      }
    }
    logRuntimeEvidence(record.str());
  }
  catch (...) {
    // Snapshot diagnostics must never change placement behavior.
  }
}

class FrozenSelection final : public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  explicit FrozenSelection(std::vector<ndn_service_framework::SelectedParticipant> selected)
    : m_selected(std::move(selected)) {}
  std::vector<ndn_service_framework::SelectedParticipant> select(
    const std::vector<ndn_service_framework::AckCandidate>&,
    const std::vector<ndn_service_framework::CollaborationRoleSpec>&) const override
  {
    // Core independently compares every returned ACK with its immutable closure.
    return m_selected;
  }
private:
  const std::vector<ndn_service_framework::SelectedParticipant> m_selected;
};

std::string conversationRoleMapDigest(
  const std::map<std::string, std::string>& providersByRole)
{
  NativeJson roleMap = NativeJson::array();
  for (const auto& [role, provider] : providersByRole)
    roleMap.push_back(NativeJson::array({role, provider}));
  return nativePlanningDigest(nativeCanonicalJson(roleMap));
}

NativeGrantLeaseScope makeGrantLeaseScope(
  const NativePlacementPlanCore& core, const NativeConversationTurn& turn,
  const NativeSecurityPolicySnapshot& security)
{
  NativeGrantLeaseScope scope;
  scope.conversationId = turn.parent.conversationId;
  scope.requesterIdentity = core.requesterIdentity;
  scope.serviceName = core.executionPlan.serviceName;
  scope.requestId = "/NDNSF-DI/CONVERSATION/" + scope.conversationId + "/GRANT-LEASE";
  scope.attempt = 1;
  scope.securityPolicySnapshotDigest = security.policyDigest;
  scope.protectionEpoch = core.protectionEpoch;
  scope.expiresAtMs = turn.parent.retentionDeadlineMs;
  scope.providerByRole = core.assignment.providerByRole;
  NativeJson roles = NativeJson::array();
  for (const auto& [role, provider] : core.assignment.providerByRole) {
    const auto& assembly = core.roleContracts().at(role);
    roles.push_back(NativeJson{{"role", role}, {"provider", provider},
      {"assembly", nativeAssemblyJson(assembly)},
      {"artifact_digest", core.artifactDigestByRole.at(role)}});
  }
  scope.scopeDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"conversation_id", scope.conversationId}, {"requester_identity", scope.requesterIdentity},
    {"service_name", scope.serviceName}, {"model_digest", core.modelDigest},
    {"graph_digest", core.graphDigest}, {"manifest_digest", core.artifacts.manifestDigest},
    {"policy_digest", scope.securityPolicySnapshotDigest},
    {"protection_epoch", scope.protectionEpoch}, {"provider_by_role", scope.providerByRole},
    {"roles", std::move(roles)}}));
  // The lease request itself is a stable cryptographic context, independent
  // of the current turn's ACK closure, input digest, attempt or plan digest.
  scope.planCoreDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"scope", scope.scopeDigest}, {"request_id", scope.requestId},
    {"attempt", scope.attempt}}));
  return scope;
}

bool grantLeaseCovers(const NativeGrantLease& lease,
                      const NativeGrantLeaseScope& wanted,
                      std::uint64_t currentDeadlineMs,
                      const NativePlacementPlanCore& core)
{
  if (lease.scope.scopeDigest != wanted.scopeDigest ||
      lease.scope.providerByRole != wanted.providerByRole ||
      lease.scope.requestId != wanted.requestId || lease.scope.attempt != wanted.attempt ||
      lease.scope.planCoreDigest != wanted.planCoreDigest ||
      lease.scope.requesterIdentity != wanted.requesterIdentity ||
      lease.scope.serviceName != wanted.serviceName ||
      lease.scope.securityPolicySnapshotDigest != wanted.securityPolicySnapshotDigest ||
      lease.scope.protectionEpoch != wanted.protectionEpoch ||
      lease.scope.expiresAtMs <= currentDeadlineMs ||
      lease.scope.expiresAtMs < core.expiresAtMs)
    return false;
  std::set<std::string> seen;
  for (const auto& grant : lease.grants) {
    const auto assigned = core.assignment.providerByRole.find(grant.role);
    if (assigned == core.assignment.providerByRole.end() || assigned->second != grant.provider ||
        grant.grantName.empty() || !isDigestValue(grant.grantDigest) ||
        grant.expiresAtMs < currentDeadlineMs || !seen.insert(grant.role).second)
      return false;
  }
  return seen.size() == core.executionPlan.roles.size();
}

void bindConversationProjections(
  std::map<std::string, NativeRoleProjectionInputs>& projections,
  const NativeSealedPlan& sealed,
  const NativeConversationTurn& turn,
  const std::string& requestContractDigest,
  const std::string& serviceName,
  const std::map<std::string, std::string>& providersByRole)
{
  if (turn.requestId.empty() || turn.executionRequestId.empty() ||
      turn.executionRequestId != sealed.core.requestId ||
      turn.attempt != sealed.core.attempt || turn.parent.serviceName != serviceName ||
      turn.parent.requestContractDigest != requestContractDigest)
    throw std::invalid_argument("conversation turn/request binding mismatch");
  const bool unboundInitialPlan = turn.attempt == 1 && turn.parent.parentContextEpoch == 0 &&
    turn.parent.planRoleMapDigest.empty() && turn.parent.expectedRoles.empty();
  if (!unboundInitialPlan && turn.parent.expectedRoles.size() != providersByRole.size())
    throw std::invalid_argument("conversation turn role map is incomplete");
  std::set<std::string> expected(turn.parent.expectedRoles.begin(), turn.parent.expectedRoles.end());
  std::set<std::string> actual;
  for (const auto& [role, provider] : providersByRole) {
    (void)provider;
    actual.insert(role);
  }
  if (!unboundInitialPlan && expected != actual)
    throw std::invalid_argument("conversation turn role set does not match placement");
  const auto roleMapDigest = conversationRoleMapDigest(providersByRole);
  // Attempt 1 must use the parent's immutable placement. A replacement may
  // switch to an alternate Provider; the coordinator binds that new map after
  // planning while retaining the old map for the parent CAS.
  if (turn.attempt == 1 && !unboundInitialPlan &&
      roleMapDigest != turn.parent.planRoleMapDigest) {
    try {
      std::ostringstream diagnostic;
      diagnostic << "DEBUG-SPEC194-R16 CONVERSATION_ROLE_MAP_MISMATCH requestId="
                 << turn.requestId
                 << " parentDigest=" << turn.parent.planRoleMapDigest
                 << " actualDigest=" << roleMapDigest << " roleMap=";
      bool first = true;
      for (const auto& [role, provider] : providersByRole) {
        diagnostic << (first ? "" : ",") << role << "=" << provider;
        first = false;
      }
      logRuntimeEvidence(diagnostic.str());
    }
    catch (...) {
      // The diagnostic must never change conversation binding behavior.
    }
    throw std::invalid_argument("conversation turn plan-role map mismatch");
  }

  ConversationTurnBindingV1 binding;
  binding.conversationId = turn.parent.conversationId;
  binding.parentContextEpoch = turn.parent.parentContextEpoch;
  binding.successorContextEpoch = turn.successorContextEpoch;
  binding.serviceName = serviceName;
  binding.planRoleMapDigest = roleMapDigest;
  binding.requestContractDigest = requestContractDigest;
  binding.retentionDeadlineMs = turn.parent.retentionDeadlineMs;
  binding.parentCheckpointDigest = turn.parent.parentCheckpointDigest;
  binding.validate();

  std::map<std::string, std::string> parentReceipts;
  if (binding.parentContextEpoch > 0) {
    if (turn.parent.parentCheckpointWire.empty())
      throw std::invalid_argument("conversation append parent checkpoint is missing");
    const auto checkpoint = NativeJson::parse(turn.parent.parentCheckpointWire);
    if (!checkpoint.is_object() || !checkpoint.contains("roleReceiptDigests") ||
        !checkpoint.at("roleReceiptDigests").is_object())
      throw std::invalid_argument("conversation append parent receipt map is missing");
    for (const auto& item : checkpoint.at("roleReceiptDigests").items()) {
      if (!item.value().is_string())
        throw std::invalid_argument("conversation append parent receipt is malformed");
      parentReceipts.emplace(item.key(), item.value().get<std::string>());
    }
    if (parentReceipts.size() != providersByRole.size())
      throw std::invalid_argument("conversation append parent receipt set is incomplete");
  }

  for (const auto& [role, provider] : providersByRole) {
    (void)provider;
    auto projection = projections.find(role);
    if (projection == projections.end())
      throw std::invalid_argument("conversation projection role is missing");
    projection->second.conversationTurnBinding = binding;
    if (binding.parentContextEpoch > 0) {
      ConversationStateReferenceV1 reference;
      reference.conversationId = binding.conversationId;
      reference.contextEpoch = binding.parentContextEpoch;
      reference.serviceName = binding.serviceName;
      reference.planRoleMapDigest = binding.planRoleMapDigest;
      reference.checkpointDigest = binding.parentCheckpointDigest;
      reference.roleName = role;
      const auto receipt = parentReceipts.find(role);
      if (receipt == parentReceipts.end())
        throw std::invalid_argument("conversation append role receipt is missing");
      reference.roleReceiptDigest = receipt->second;
      reference.expiresAtMs = turn.parent.retentionDeadlineMs;
      reference.validate();
      projection->second.conversationStateReference = std::move(reference);
    }
  }
}
}

NativeRequestRuntime nativeRequestRuntimeFromJson(
  const std::string& configurationJson,
  const NativeRequestCatalog& catalog,
  std::shared_ptr<const NativeAuthenticatedGrantClient> grants)
{
  if (configurationJson.size() > 64 * 1024)
    throw std::invalid_argument("native request runtime configuration exceeds limit");
  if (!catalog.preparation || !catalog.splitter)
    throw std::invalid_argument("native request runtime requires a complete catalog");
  if (!grants)
    throw std::invalid_argument("native request runtime requires a native grant client");
  catalog.model.validate();

  const auto root = nativeParseJson(configurationJson);
  requireExactKeys(root,
    {"schema", "contract", "requester_identity", "protection_epoch", "input_layout_digest",
     "security", "budget", "state_mapping", "no_progress_ms", "max_segments"}, "runtime",
    {"options_transport"});
  if (!root.at("schema").is_string() ||
      root.at("schema").get<std::string>() != "ndnsf-di-native-request-runtime-v1")
    throw std::invalid_argument("unsupported native request runtime schema");

  const auto& contract = root.at("contract");
  requireExactKeys(contract,
    {"service_name", "task_name", "adapter_name", "adapter_descriptor_digest",
     "adapter_composition_digest", "task_descriptor_digest", "generation_mode",
     "tokenizer_digest"}, "runtime.contract");
  NativeRequestRuntime runtime;
  runtime.contract.serviceName = readString(contract.at("service_name"), "runtime.contract.service_name");
  runtime.contract.taskName = readString(contract.at("task_name"), "runtime.contract.task_name");
  runtime.contract.adapterName = readString(contract.at("adapter_name"), "runtime.contract.adapter_name");
  runtime.contract.adapterDescriptorDigest = readString(contract.at("adapter_descriptor_digest"), "runtime.contract.adapter_descriptor_digest");
  runtime.contract.adapterCompositionDigest = readString(contract.at("adapter_composition_digest"), "runtime.contract.adapter_composition_digest");
  runtime.contract.taskDescriptorDigest = readString(contract.at("task_descriptor_digest"), "runtime.contract.task_descriptor_digest");
  runtime.contract.generationMode = readString(contract.at("generation_mode"), "runtime.contract.generation_mode");
  runtime.contract.tokenizerDigest = readString(contract.at("tokenizer_digest"), "runtime.contract.tokenizer_digest");
  if (root.contains("options_transport"))
    runtime.optionsTransport = readString(root.at("options_transport"), "runtime.options_transport");
  if ((runtime.optionsTransport != "INLINE" &&
       runtime.optionsTransport != "SELECTION_GENERATION_V1") ||
      (runtime.optionsTransport != "INLINE" && runtime.contract.generationMode != "TOKEN_STREAMING"))
    throw std::invalid_argument("unsupported native runtime options transport");
  if (runtime.contract.serviceName.empty() || runtime.contract.serviceName.front() != '/' ||
      runtime.contract.taskName.empty() || runtime.contract.adapterName.empty() ||
      runtime.contract.generationMode.empty())
    throw std::invalid_argument("native request runtime contract identity is incomplete");
  if (!isSupportedNativeGenerationMode(runtime.contract.generationMode))
    throw std::invalid_argument(
      "native request runtime generation mode must be TOKEN_DIAGNOSTIC or TOKEN_STREAMING");
  if (!runtime.contract.tokenizerDigest.empty())
    requireDigestValue(contract.at("tokenizer_digest"), "runtime.contract.tokenizer_digest");
  if (runtime.contract.generationMode == "TOKEN_STREAMING" &&
      runtime.contract.tokenizerDigest.empty())
    throw std::invalid_argument(
      "native request runtime TOKEN_STREAMING requires tokenizer digest");
  requireDigestValue(contract.at("adapter_descriptor_digest"), "runtime.contract.adapter_descriptor_digest");
  requireDigestValue(contract.at("adapter_composition_digest"), "runtime.contract.adapter_composition_digest");
  requireDigestValue(contract.at("task_descriptor_digest"), "runtime.contract.task_descriptor_digest");
  if (runtime.contract.adapterName != catalog.model.descriptor.adapterId ||
      runtime.contract.adapterDescriptorDigest != catalog.model.descriptor.adapter.descriptorDigest())
    throw std::invalid_argument("native request runtime adapter identity does not match catalog");
  if (std::find(catalog.model.descriptor.adapter.tasks.begin(),
                catalog.model.descriptor.adapter.tasks.end(), runtime.contract.taskName) ==
      catalog.model.descriptor.adapter.tasks.end())
    throw std::invalid_argument("native request runtime task is not supported by catalog adapter");

  runtime.requesterIdentity = readString(root.at("requester_identity"), "runtime.requester_identity");
  runtime.protectionEpoch = readString(root.at("protection_epoch"), "runtime.protection_epoch");
  runtime.inputLayoutDigest = readString(root.at("input_layout_digest"), "runtime.input_layout_digest");
  if (runtime.requesterIdentity.empty() || runtime.requesterIdentity.front() != '/' ||
      runtime.protectionEpoch.empty() || runtime.protectionEpoch == "plaintext-v1")
    throw std::invalid_argument("native request runtime requester or protection identity is invalid");
  requireDigestValue(root.at("input_layout_digest"), "runtime.input_layout_digest");
  if (runtime.requesterIdentity != grants->requesterIdentity() ||
      runtime.protectionEpoch != grants->protectionEpoch())
    throw std::invalid_argument("native request runtime identity does not match grant client");

  const auto& security = root.at("security");
  requireExactKeys(security, {"policy_digest", "require_protected_artifacts"}, "runtime.security");
  runtime.security.policyDigest = readString(security.at("policy_digest"), "runtime.security.policy_digest");
  runtime.security.requireProtectedArtifacts = readBoolean(
    security.at("require_protected_artifacts"), "runtime.security.require_protected_artifacts");
  requireDigestValue(security.at("policy_digest"), "runtime.security.policy_digest");
  if (!runtime.security.requireProtectedArtifacts)
    throw std::invalid_argument("native request runtime cannot disable protected artifacts");

  const auto& budget = root.at("budget");
  requireExactKeys(budget, {"max_candidates", "max_policy_ms", "max_reentries"}, "runtime.budget");
  const auto readUnsigned = [] (const NativeJson& value, const char* field) -> std::uint64_t {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
      return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
  };
  const auto maxCandidates = readUnsigned(budget.at("max_candidates"), "runtime.budget.max_candidates");
  const auto maxPolicyMs = readUnsigned(budget.at("max_policy_ms"), "runtime.budget.max_policy_ms");
  const auto maxReentries = readUnsigned(budget.at("max_reentries"), "runtime.budget.max_reentries");
  if (maxCandidates > std::numeric_limits<std::size_t>::max() ||
      maxReentries > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument("native request runtime budget exceeds host size limit");
  runtime.budget = {static_cast<std::size_t>(maxCandidates), maxPolicyMs,
                    static_cast<std::size_t>(maxReentries)};
  runtime.budget.validate();

  const auto& state = root.at("state_mapping");
  requireExactKeys(state, {"inputs", "outputs"}, "runtime.state_mapping");
  try {
    runtime.stateMapping.inputs = state.at("inputs").get<NativeStateTensorMapping::Roles>();
    runtime.stateMapping.outputs = state.at("outputs").get<NativeStateTensorMapping::Roles>();
  }
  catch (const std::exception& error) {
    throw std::invalid_argument(std::string("runtime.state_mapping is malformed: ") + error.what());
  }
  if (runtime.stateMapping.inputs != catalog.stateMapping.inputs ||
      runtime.stateMapping.outputs != catalog.stateMapping.outputs)
    throw std::invalid_argument("native request runtime state mapping does not match catalog");

  const auto noProgressMs = readUnsigned(root.at("no_progress_ms"), "runtime.no_progress_ms");
  const auto maxSegments = readUnsigned(root.at("max_segments"), "runtime.max_segments");
  if (noProgressMs == 0 || noProgressMs > 24ULL * 60ULL * 60ULL * 1000ULL ||
      maxSegments == 0 || maxSegments > (1ULL << 20))
    throw std::invalid_argument("native request runtime progress or segment limit is out of range");
  runtime.noProgressMs = noProgressMs;
  runtime.maxSegments = static_cast<std::size_t>(maxSegments);
  runtime.grants = std::move(grants);
  runtime.catalog = catalog.preparation;
  runtime.planningCache = catalog.planningCache;
  return runtime;
}

namespace {

NativePlannedRequest planNativeRequestImpl(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const NativeStrategyPorts& ports,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn,
  const NativePlacementProbeRoundV1* placementProbeRound,
  const CooperativeModelSplitStrategy* preparedSplitter,
  bool observationFallback = false,
  NativeOfferDecodeCache* decodeCache = nullptr)
{
  const detail::ScopedFixedAssignmentWork fixedWork(control.requestId, "user-plan");
  const auto contextCopyTiming = planPhaseStarted();
  control.requireActive();
  runtime.budget.validate();
  ports.splitterIdentity.validate();
  ports.placementIdentity.validate();
  if (!ports.enumerate || !ports.proposeRoles)
    throw std::invalid_argument("native strategy ports are incomplete");
  if (closure.requestId != ndn::Name(control.requestId) || closure.digest.empty() ||
      wireDeadlineMs <= epochMs() || !runtime.grants || !runtime.security.requireProtectedArtifacts ||
      runtime.requesterIdentity.empty() ||
      (!encoded.probeScopeDigest.empty() &&
       runtime.requesterIdentity != encoded.requesterIdentity) ||
      runtime.protectionEpoch.empty())
    throw std::invalid_argument("native request runtime/ACK binding is incomplete");
  NativeOfferBindingContext context;
  context.requestId = control.requestId;
  context.attempt = control.attempt;
  context.serviceName = runtime.contract.serviceName;
  context.modelDigest = encoded.modelIntentDigest;
  context.graphDigest = model.graph.graphDigest;
  context.deadlineMs = wireDeadlineMs;
  context.candidateArtifacts = encoded.candidateArtifacts;
  context.candidateArtifactSetDigest = encoded.candidateArtifactSetDigest;
  context.candidateProviders = encoded.candidateProviders;
  context.candidateSetDigest = encoded.candidateSetDigest;
  context.probeScopeDigest = encoded.probeScopeDigest;
  context.requesterIdentity = encoded.requesterIdentity;
  context.allowedRttPairs = encoded.allowedRttPairs;
  context.allowedGoodputEdges = encoded.allowedGoodputEdges;
  context.placementProbeRoundCompleted = placementProbeRound != nullptr || observationFallback;
  if (observationFallback)
    context.placementObservationFallbackReason = "SNAPSHOT_STALE";
  if (conversationTurn && conversationTurn->attempt == 1 &&
      conversationTurn->parent.parentContextEpoch > 0)
    context.preferredProvidersByRole = conversationTurn->providersByRole;
  const bool reusePinnedConversationPlacement = [&] {
    if (!conversationTurn || conversationTurn->attempt != 1 || encoded.recovery ||
        conversationTurn->parent.parentContextEpoch == 0 ||
        conversationTurn->providersByRole.empty() || !conversationTurn->grantLease)
      return false;
    const auto& parent = conversationTurn->parent;
    const auto& scope = conversationTurn->grantLease->scope;
    return parent.serviceName == runtime.contract.serviceName &&
      parent.requestContractDigest == encoded.requestContractDigest &&
      conversationTurn->executionRequestId == context.requestId &&
      conversationRoleMapDigest(conversationTurn->providersByRole) == parent.planRoleMapDigest &&
      scope.providerByRole == conversationTurn->providersByRole &&
      scope.conversationId == parent.conversationId &&
      scope.requesterIdentity == runtime.requesterIdentity &&
      scope.serviceName == runtime.contract.serviceName &&
      scope.securityPolicySnapshotDigest == runtime.security.policyDigest &&
      scope.protectionEpoch == runtime.protectionEpoch &&
      scope.expiresAtMs > context.deadlineMs;
  }();
  logPlanPhase(control.requestId, "request_context_copy", contextCopyTiming);
  std::vector<NativeAdmittedOfferV3> offers;
  std::map<std::string, std::uint64_t> authenticatedTransportByProvider;
  auto phaseStarted = planPhaseStarted();
  for (const auto& ack : closure.candidates) {
    if (!ack.ack.getStatus() ||
        (encoded.recovery && ack.providerName.toUri() == encoded.recovery->failedProvider))
      continue;
    offers.push_back(admission.verify(ack, context, epochMs()));
    if (reusePinnedConversationPlacement && ack.ack.hasProviderCapabilityOffer()) {
      const auto& fields = ack.ack.getProviderCapabilityOffer();
      if (fields.hasField(NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD)) {
        const auto bytes = detail::nativeTensorTransportSegmentBytesV1(
          fields.getField(NATIVE_PROVIDER_TENSOR_TRANSPORT_V1_FIELD), ndn::MAX_NDN_PACKET_SIZE);
        if (bytes != 0)
          authenticatedTransportByProvider.emplace(
            offers.back().observation().provider, bytes);
      }
    }
  }
  // V4 is a whole-request profile. A missing, duplicate, stale, or invalid
  // Provider offer drops the complete observation set and leaves the request
  // on its existing V3 admission path.
  std::string snapshotUnavailableReason;
  if (runtime.planningCache &&
      sameStrategyIdentity(runtime.planningCache->splitter, ports.splitterIdentity))
    context.preparedPlanningTemplate =
      runtime.planningCache->validatedTemplate(model, preparedSplitter);
  std::optional<NativePlacementObservationSnapshotV4> placementSnapshot;
  NativeJson requestTransferProfile;
  if (runtime.planningCache && options.generation && options.generation->enabled &&
      !encoded.recovery) {
    for (const auto& profile : runtime.planningCache->requestTransferProfiles) {
      if ((!conversationTurn || conversationTurn->parent.parentContextEpoch == 0) &&
          profile.value("input_digest", std::string{}) == encoded.logicalInputDigest &&
          profile.value("source_digest", std::string{}) == model.canonicalSourceDigest) {
        requestTransferProfile = profile;
        requestTransferProfile["request_contract_digest"] = encoded.requestContractDigest;
        break;
      }
    }
    if (requestTransferProfile.is_null()) {
      for (const auto& profile : runtime.planningCache->requestTransferProfiles) {
        if (auto bound = bindNativeRequestTransferProfileV1(
              profile, model.canonicalSourceDigest, encoded, conversationTurn)) {
          requestTransferProfile = std::move(*bound);
          requestTransferProfile["request_contract_digest"] = encoded.requestContractDigest;
          break;
        }
      }
    }
  }
  if (!reusePinnedConversationPlacement && !observationFallback && !encoded.recovery && runtime.planningCache &&
      sameStrategyIdentity(runtime.planningCache->splitter, ports.splitterIdentity)) {
    const auto decodeReusesBefore = decodeCache ? decodeCache->decodeReuseCount() : 0;
    placementSnapshot = trySelectNativePlacementObservationSnapshotV4(
      context, closure, admission, *runtime.planningCache,
      runtime.budget.maxCandidates, epochMs(), &model.graph,
      &snapshotUnavailableReason,
      requestTransferProfile.is_null() ? nullptr : &requestTransferProfile, decodeCache);
    if (decodeCache)
      logRuntimeEvidence("NDNSF_DI_NATIVE_V4_DECODE_REUSE requestId=" + control.requestId +
        " reused=" + std::to_string(decodeCache->decodeReuseCount() - decodeReusesBefore));
    if (placementSnapshot) {
      // Keep the JSON request-owned until freshness and probe merging finish.
      // These checks consume context's scope fields, not its snapshot payload.
      context.placementObservationSnapshotDigest = placementSnapshot->digest;
    }
  }
  else if (reusePinnedConversationPlacement) {
    snapshotUnavailableReason = "PINNED_CONVERSATION_PLAN_REUSED";
  }
  else if (observationFallback) {
    snapshotUnavailableReason = "SNAPSHOT_STALE";
  }
  else if (encoded.recovery) {
    snapshotUnavailableReason = "RECOVERY_REQUEST";
  }
  else if (!runtime.planningCache) {
    snapshotUnavailableReason = "PREPARED_PLANNING_CACHE_UNAVAILABLE";
  }
  else {
    snapshotUnavailableReason = "SPLITTER_CACHE_IDENTITY_MISMATCH";
  }
  logPlanPhase(control.requestId, "admission_verify", phaseStarted);
  if (offers.empty()) throw std::runtime_error("DI_NATIVE_NO_ADMITTED_PROVIDER");
  const auto observationMergeTiming = planPhaseStarted();
  const auto policyStart = std::chrono::steady_clock::now();
  const auto budgetDeadline = policyStart +
    std::chrono::milliseconds(runtime.budget.maxPolicyMs);
  auto strategyDeadline = control.deadline == std::chrono::steady_clock::time_point{} ?
    budgetDeadline : std::min(control.deadline, budgetDeadline);
  const auto snapshotCheckedAtMs = epochMs();
  if (!context.placementObservationSnapshotDigest.empty()) {
    const auto nowMs = snapshotCheckedAtMs;
    const auto snapshotExpiry = placementSnapshot->value.value(
      "expires_at_ms", std::uint64_t{0});
    if (snapshotExpiry > context.deadlineMs)
      throw std::runtime_error("DI_NATIVE_OBSERVATION_SNAPSHOT_EXPIRED");
    if (snapshotExpiry <= nowMs) {
      context.placementObservationSnapshot = nullptr;
      context.placementObservationSnapshotDigest.clear();
      context.placementObservationFallbackReason = "SNAPSHOT_STALE";
      snapshotUnavailableReason = "SNAPSHOT_STALE";
    }
  }
  if (placementProbeRound && !observationFallback) {
    validatePlacementProbeIntent(placementProbeRound->intent, context);
    if (placementSnapshot && !context.placementObservationSnapshotDigest.empty()) {
      mergeTargetedPlacementProbeRound(
        *placementSnapshot, *placementProbeRound, context, snapshotCheckedAtMs);
      context.placementObservationSnapshotDigest = placementSnapshot->digest;
      if (placementSnapshot->expiresAtMs <= snapshotCheckedAtMs) {
        context.placementObservationSnapshot = nullptr;
        context.placementObservationSnapshotDigest.clear();
        context.placementObservationFallbackReason = "TARGETED_PROBE_SNAPSHOT_STALE";
        snapshotUnavailableReason = "TARGETED_PROBE_SNAPSHOT_STALE";
      }
    }
  }
  if (placementSnapshot && !context.placementObservationSnapshotDigest.empty())
    context.placementObservationSnapshot = std::move(placementSnapshot->value);
  // Policy/cancellation and observation freshness are distinct bounds. Check
  // the original epoch expiry at each decision/publication boundary; never
  // turn an old sample into a shorter deadline for the whole request.
  const auto requireFreshObservation = [&] {
    if (!context.placementObservationSnapshotDigest.empty() &&
        context.placementObservationSnapshot.at("expires_at_ms").get<std::uint64_t>() <= epochMs())
      throw detail::NativePlacementObservationExpired();
  };
  const ExtensionControl extensionControl{strategyDeadline, control.cancelled};
  extensionControl.requireActive();
  logPlanPhase(control.requestId, "observation_merge_control", observationMergeTiming);
  bool usingPreparedPlanningCache = false;
  std::shared_ptr<const NativePreparedPlanningTemplate> preparedTemplate;
  std::vector<NativeSplitCandidate> candidates;
  phaseStarted = planPhaseStarted();
  if (runtime.planningCache &&
      sameStrategyIdentity(runtime.planningCache->splitter, ports.splitterIdentity)) {
    usingPreparedPlanningCache = true;
    preparedTemplate = runtime.planningCache->validatedTemplate(model, preparedSplitter);
    if (!preparedTemplate) {
      const auto& prepared = runtime.planningCache->candidates;
      const auto count = std::min(prepared.size(), runtime.budget.maxCandidates);
      candidates.reserve(count);
      for (std::size_t i = 0; i < count; ++i)
        candidates.push_back(prepared[i].candidate);
    }
  }
  else {
    candidates = ports.enumerate(model.descriptor, model.graph, runtime.budget,
                                 extensionControl);
  }
  logPlanPhase(control.requestId, "candidate_enumeration", phaseStarted);
  control.requireActive();
  auto policyUsed = std::chrono::steady_clock::now() - policyStart;
  const auto policyLimit = std::chrono::milliseconds(runtime.budget.maxPolicyMs);
  if (policyUsed > policyLimit)
    throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
  const auto candidateCount = preparedTemplate
    ? std::min(preparedTemplate->candidates().size(), runtime.budget.maxCandidates)
    : candidates.size();
  if (candidateCount == 0 || candidateCount > runtime.budget.maxCandidates)
    throw std::runtime_error("DI_NATIVE_CANDIDATE_BUDGET_REJECTED");
  if (preparedTemplate)
    logRuntimeEvidence("NDNSF_DI_NATIVE_PLANNING_TEMPLATE requestId=" + control.requestId +
      " reused=1 candidates=" + std::to_string(candidateCount));

  // The splitter supplies its deterministic candidate preference order. No
  // artifact publication happens until one complete placement is validated.
  try {
  for (std::size_t candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
    const auto candidateCopyTiming = planPhaseStarted();
    // Keep one request-local candidate before handing it to publication
    // callbacks. Their mutable aliases must never poison the static template.
    std::optional<NativeSplitCandidate> candidateCopy;
    candidateCopy.emplace(preparedTemplate
      ? preparedTemplate->candidates()[candidateIndex].candidate : candidates[candidateIndex]);
    const auto& candidate = *candidateCopy;
    control.requireActive();
    if (!sameStrategyIdentity(candidate.splitter, ports.splitterIdentity))
      throw std::invalid_argument("native splitter returned a foreign strategy identity");
    std::vector<NativeSelectionRoleV3> roles;
    if (preparedTemplate) {
      // The factory owns exactly this model/splitter/candidate/role contract.
      // Keep a local role copy before invoking the external placement port.
      roles = preparedTemplate->candidates()[candidateIndex].roles;
    }
    else if (usingPreparedPlanningCache) {
      const auto found = std::find_if(runtime.planningCache->candidates.begin(),
        runtime.planningCache->candidates.end(), [&](const auto& prepared) {
          return prepared.candidate.candidateDigest == candidate.candidateDigest;
        });
      if (found == runtime.planningCache->candidates.end())
        throw std::invalid_argument("native prepared planning cache candidate is missing");
      candidate.validate(model.graph);
      roles = found->roles;
      NativeRequestPreparation::validateRoles(model, candidate, roles);
    }
    else {
      expandStateContractsFromCatalog(*candidateCopy, runtime.stateMapping, model.descriptor);
      if (runtime.catalog)
        *candidateCopy = runtime.catalog->bindStateContracts(model, candidate, runtime.stateMapping, control);
      roles = preparation.prepareRoles(model, candidate, control);
    }
    logPlanPhase(control.requestId, "candidate_role_copy", candidateCopyTiming);
    NativeRolePlacementProposalV3 proposal;
    auto expectedPlacementIdentity = ports.placementIdentity;
    const auto placementStart = std::chrono::steady_clock::now();
    const PlanPhaseTiming placementTiming{placementStart, threadCpuTimeNs()};
    try {
      if (reusePinnedConversationPlacement) {
        // The Conversation owner already pinned this assignment. Current ACK
        // admission and feasibility remain authoritative for this new turn.
        const NativeFixedProviderPlacement pinnedPlacement(conversationTurn->providersByRole);
        expectedPlacementIdentity = pinnedPlacement.identity();
        proposal = pinnedPlacement.proposeRoles(
          context, closure.digest, roles, offers, epochMs(), extensionControl);
        logRuntimeEvidence("NDNSF_DI_NATIVE_WARM_PLAN_REUSE requestId=" + control.requestId +
          " status=REUSED_PINNED_CONVERSATION_PLACEMENT");
      }
      else {
        proposal = ports.proposeRoles(context, closure.digest, roles, offers, epochMs(),
                                      extensionControl);
      }
    }
    catch (const NativeNoFeasiblePlacement&) {
      policyUsed += std::chrono::steady_clock::now() - placementStart;
      extensionControl.requireActive();
      if (policyUsed > policyLimit) throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
      continue;
    }
    policyUsed += std::chrono::steady_clock::now() - placementStart;
    const bool policyBudgetFallback = proposal.placementDecisionV4 &&
      proposal.placementDecisionV4->value("fallback_reason", std::string{}) ==
        "POLICY_BUDGET_EXHAUSTED";
    if (policyUsed > policyLimit &&
        !(proposal.placementDecisionV4 &&
          proposal.placementDecisionV4->value("status", std::string{}) ==
            "BASELINE_FALLBACK"))
      throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
    // A strategy can observe cancellation/deadline through ExtensionControl
    // and still return a partial proposal. Honor the control outcome before
    // interpreting that proposal as a request-snapshot violation.
    if (policyBudgetFallback) control.requireActive();
    else extensionControl.requireActive();
    if (!sameStrategyIdentity(proposal.strategy, expectedPlacementIdentity))
      throw std::invalid_argument("native placement returned a foreign strategy identity");
    if (!sameOfferBindingContext(proposal.context, context))
      throw std::invalid_argument("native placement modified the admitted request snapshot");
    // A legacy strategy need not carry the optional encoding hint. Reattach
    // our pin only after its actual current context has passed equality.
    proposal.context.preparedPlanningTemplate = context.preparedPlanningTemplate;
    // Pin one immutable cache candidate through the current publication and
    // sealing checks; replacement of the shared cache cannot change it.
    const auto previousFixedCore = runtime.fixedRoleAssignments
      ? runtime.fixedRoleAssignments->placementCore(runtime.requesterIdentity, runtime.contract.serviceName)
      : std::shared_ptr<const NativeFixedPlacementCoreV1>{};
    if (previousFixedCore)
      validateNativeRolePlacementWithFixedCore(proposal, roles, *previousFixedCore, offers, epochMs());
    else validateNativeRolePlacement(proposal, roles, offers, epochMs());
    requireFreshObservation();
    logPlanPhase(control.requestId, "placement_and_role_validation", placementTiming);
    const auto executionPrepareTiming = planPhaseStarted();
    if (proposal.placementProbeIntent) {
      if (placementProbeRound || context.placementProbeRoundCompleted)
        throw std::invalid_argument("placement strategy requested a second probe round");
      validatePlacementProbeIntent(*proposal.placementProbeIntent, context);
      NativePlannedRequest deferred;
      deferred.placementProbeIntent = proposal.placementProbeIntent;
      if (!context.placementObservationSnapshotDigest.empty())
        deferred.placementProbeCacheContext =
          std::make_shared<const NativeOfferBindingContext>(std::move(context));
      logPlanPhase(control.requestId, "execution_prepare", executionPrepareTiming);
      return deferred; // No artifacts, grants, or plan publication before targeted facts arrive.
    }
    const auto matchesPreparedExecution = [&] {
      if (!preparedTemplate) return false;
      const auto& expected = preparedTemplate->execution(candidateIndex).roles;
      if (expected.size() != proposal.roles.size()) return false;
      for (std::size_t i = 0; i < expected.size(); ++i)
        if (expected[i] != proposal.roles[i].selectedRole) return false;
      return true;
    }();
    NativeExecutionPlan execution = matchesPreparedExecution
      ? preparedTemplate->execution(candidateIndex) : candidate.executionPlan;
    execution.serviceName = runtime.contract.serviceName;
    execution.modelName = model.descriptor.modelName;
    if (!matchesPreparedExecution) {
      execution.roles.clear();
      std::map<std::string, std::vector<std::string>> selectedByStage;
      for (const auto& role : proposal.roles) {
        execution.roles.push_back(role.selectedRole);
        selectedByStage[role.role].push_back(role.selectedRole);
      }
      const auto expand = [&](const std::vector<std::string>& stages) {
        std::vector<std::string> result;
        for (const auto& stage : stages) {
          const auto& selected = selectedByStage.at(stage);
          result.insert(result.end(), selected.begin(), selected.end());
        }
        return result;
      };
      for (auto& dependency : execution.dependencies) {
        dependency.producers = expand(dependency.producers);
        dependency.consumers = expand(dependency.consumers);
      }
    }
    if (options.generation) {
      if (!options.stream || !options.generation->enabled ||
          options.generation->mode != "TOKEN_STREAMING")
        throw std::invalid_argument("generation requires the authenticated streaming request path");
      std::set<std::string> sources(execution.roles.begin(), execution.roles.end());
      std::set<std::string> terminals = sources;
      for (const auto& edge : execution.dependencies) {
        if (edge.operationKind == "TOKEN_FEEDBACK")
          throw std::invalid_argument("candidate cannot supply requester-owned generation feedback");
        for (const auto& role : edge.consumers) sources.erase(role);
        for (const auto& role : edge.producers) terminals.erase(role);
      }
      if (sources.size() != 1 || terminals.size() != 1)
        throw std::invalid_argument("generation requires one pipeline source and terminal");
      const auto& generation = *options.generation;
      if (generation.tokenInputName.empty() || generation.stateInputNames.empty() ||
          generation.stateInputNames.size() != generation.stateOutputNames.size() ||
          std::set<std::string>(generation.stateInputNames.begin(), generation.stateInputNames.end()).size() != generation.stateInputNames.size() ||
          std::set<std::string>(generation.stateOutputNames.begin(), generation.stateOutputNames.end()).size() != generation.stateOutputNames.size())
        throw std::invalid_argument("generation state input/output contract is incomplete");
      std::set<std::string> observedStateInputs;
      std::set<std::string> observedStateOutputs;
      for (const auto& role : proposal.roles) {
        const auto includes = [](const auto& tensors, const std::string& name) {
          return std::any_of(tensors.begin(), tensors.end(), [&](const auto& tensor) { return tensor.name == name; });
        };
        if (role.selectedRole == *sources.begin() && !includes(role.expectedInputs, generation.tokenInputName))
          throw std::invalid_argument("generation source omits the token input");
        std::size_t localStateCount = 0;
        for (std::size_t index = 0; index < generation.stateInputNames.size(); ++index) {
          const auto hasInput = includes(role.expectedInputs, generation.stateInputNames[index]);
          const auto hasOutput = includes(role.expectedOutputs, generation.stateOutputNames[index]);
          if (hasInput != hasOutput)
            throw std::invalid_argument("generation role has an incomplete state pair");
          if (hasInput) {
            ++localStateCount;
            observedStateInputs.insert(generation.stateInputNames[index]);
            observedStateOutputs.insert(generation.stateOutputNames[index]);
          }
        }
        if (localStateCount == 0)
          throw std::invalid_argument("generation role omits all sealed state pairs");
      }
      if (observedStateInputs.size() != generation.stateInputNames.size() ||
          observedStateOutputs.size() != generation.stateOutputNames.size()) {
        throw std::invalid_argument("generation state contract has an unbound role state");
      }
      const auto hash = [](const NativeJson& value) {
        return nativePlanningDigest(nativeCanonicalJson(value));
      };
      NativeDependencySpec feedback;
      feedback.producers = {*terminals.begin()}; feedback.consumers = {*sources.begin()};
      feedback.keyScope = "token-feedback-" + hash({{"request", control.requestId},
        {"attempt", control.attempt}, {"producer", feedback.producers.front()},
        {"consumer", feedback.consumers.front()}}).substr(7, 16);
      feedback.topicPrefix = "/token-feedback";
      feedback.objectNameTemplate = "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
      feedback.tensors = {"input_ids"}; feedback.operationKind = "TOKEN_FEEDBACK";
      feedback.useNdnsfDataV1 = true;
      // A dependency can carry several tensor transfers. Reserve a round for
      // each transfer before the feedback round, rather than overlapping the
      // next epoch whenever transfer count exceeds dependency count.
      std::uint64_t transferCount = 0;
      for (const auto& edge : execution.dependencies) {
        transferCount += edge.redistributions.empty() ? edge.tensors.size() : edge.redistributions.size();
        if (transferCount >= (1U << 20)) throw std::invalid_argument("generation operation bound exceeded");
      }
      feedback.collectiveOperationIndex = transferCount;
      // A validated single-source/single-terminal DAG is connected. Group
      // membership therefore uses this same sorted provider set, including a
      // one-provider self-feedback group for a full-model request.
      std::set<std::string> providers;
      std::string terminalProvider;
      for (const auto& role : proposal.roles) {
        const auto& provider = proposal.providerByRole.at(role.selectedRole);
        providers.insert(provider);
        if (role.selectedRole == feedback.producers.front()) terminalProvider = provider;
      }
      if (terminalProvider.empty()) throw std::invalid_argument("generation terminal has no provider");
      feedback.collectiveProducerRank = std::to_string(std::distance(providers.begin(), providers.find(terminalProvider)));
      feedback.collectiveSourceLayoutDigest = feedback.collectiveTargetLayoutDigest =
        hash({{"tensor", "input_ids"}, {"layout", "int64[1,1]"}, {"operation", "TOKEN_FEEDBACK"}});
      feedback.collectiveTensorDigest = hash(NativeJson::array({"input_ids"}));
      execution.dependencies.push_back(std::move(feedback));
      execution.streamingOperationStride = transferCount + 1;
    }
    if (policyBudgetFallback) control.requireActive();
    else extensionControl.requireActive();
    requireFreshObservation();
    NativePlanSealingInputs sealing;
    logPlanPhase(control.requestId, "execution_prepare", executionPrepareTiming);
    phaseStarted = planPhaseStarted();
    sealing.artifacts = preparation.ensureArtifacts(model, candidate, proposal, control);
    logPlanPhase(control.requestId, "artifact_binding", phaseStarted);
    const auto sealingInputTiming = planPhaseStarted();
    sealing.requesterIdentity = runtime.requesterIdentity;
    sealing.protectionEpoch = runtime.protectionEpoch;
    // Observation expiry bounds the placement decision. Once that decision is
    // sealed while the snapshot is fresh, the selected request keeps the
    // original wire deadline for its execution and dataflow leases.
    sealing.expiresAtMs = context.deadlineMs;
    sealing.requestContractDigest = encoded.requestContractDigest;
    sealing.placementObservationSnapshotDigest =
      context.placementObservationSnapshotDigest;
    if (options.generation) {
      sealing.generationContract = *options.generation;
      sealing.generationContract.streamingOperationStride = execution.streamingOperationStride;
    }
    // The sealer consumes this isolated vector synchronously; it never stores
    // the borrowed pointer in the returned core or its immutable cache.
    sealing.borrowedPreparedRoles = &roles;
    sealing.previousFixedCore = previousFixedCore;
    if (policyBudgetFallback) control.requireActive();
    else extensionControl.requireActive();
    requireFreshObservation();
    logPlanPhase(control.requestId, "sealing_input_prepare", sealingInputTiming);
    phaseStarted = planPhaseStarted();
    auto core = NativePlanSealer::sealCore(model, candidate, proposal, execution, offers, closure.digest, sealing);
    if (runtime.fixedRoleAssignments) {
      const auto& previous = sealing.previousFixedCore;
      core = NativePlanSealer::shareFixedCore(std::move(core), previous);
      runtime.fixedRoleAssignments->rememberPlacementCore(runtime.requesterIdentity, runtime.contract.serviceName,
        core.fixedCoreOwner);
      logRuntimeEvidence("NDNSF_DI_FIXED_CORE_OWNER requestId=" + control.requestId +
        " reused=" + (previous == core.fixedCoreOwner ? "1" : "0") +
        " sealed_from_cache=" + (core.reusedFixedCore ? "1" : "0"));
    }
    requireFreshObservation();
    logPlanPhase(control.requestId, "plan_seal_core", phaseStarted);
    const auto evidenceRenderTiming = planPhaseStarted();
    // Only an accepted sealed decision is final evidence. Discarded attempts
    // must not be mistaken for the baseline pass's committed placement.
    logPlacementObservationSnapshot(control.requestId,
      context.placementObservationSnapshot,
      context.placementObservationSnapshotDigest, epochMs(),
      snapshotUnavailableReason);
    if (proposal.placementDecisionV4) {
      logRuntimeEvidence(
        "NDNSF_DI_NATIVE_PLACEMENT_DECISION requestId=" + control.requestId +
        " strategy=" + proposal.strategy.name +
        " decisionJson=" + nativeCanonicalJson(*proposal.placementDecisionV4));
    }
    logPlanPhase(control.requestId, "placement_evidence_render", evidenceRenderTiming);
    std::optional<NativeGrantLeaseScope> grantLeaseScope;
    std::optional<NativeGrantLease> grantLease;
    bool reusedGrantLease = false;
    if (conversationTurn) {
      const auto previous = conversationTurn->grantLease ? &*conversationTurn->grantLease : nullptr;
      const bool scopeReused = previous && previous->fixedContext && previous->fixedContext->matches(
        core, *conversationTurn, runtime.security, previous->scope);
      auto wanted = scopeReused ? previous->fixedContext->scope()
        : makeGrantLeaseScope(core, *conversationTurn, runtime.security);
      logRuntimeEvidence("NDNSF_DI_GRANT_SCOPE requestId=" + control.requestId +
        " reused=" + (scopeReused ? "1" : "0"));
      if (wanted.expiresAtMs <= sealing.expiresAtMs)
        throw std::runtime_error("DI_NATIVE_CONVERSATION_RETENTION_SHORTER_THAN_REQUEST");
      if (conversationTurn->grantLease && grantLeaseCovers(
            *conversationTurn->grantLease, wanted, core.expiresAtMs, core)) {
        grantLease = *conversationTurn->grantLease;
        if (!scopeReused)
          grantLease->fixedContext = std::make_shared<const NativeGrantLeaseFixedContext>(core, wanted);
        grantLeaseScope = grantLease->scope;
        reusedGrantLease = true;
      }
      else {
        grantLeaseScope = std::move(wanted);
        grantLease.emplace();
        grantLease->scope = *grantLeaseScope;
        grantLease->fixedContext = scopeReused ? previous->fixedContext
          : std::make_shared<const NativeGrantLeaseFixedContext>(core, *grantLeaseScope);
      }
    }
    std::vector<NativeGrantBinding> grants;
    NativeGrantControl grantControl{std::chrono::system_clock::time_point(
      std::chrono::milliseconds(core.expiresAtMs)), cancelled};
    phaseStarted = planPhaseStarted();
    if (reusedGrantLease) {
      grants = grantLease->grants;
      logPlanPhase(control.requestId, "grant_lease_hit", phaseStarted);
    }
    else {
      // Grant acquisition is independently bound to each selected role. Start
      // all role-local authority/publication operations together, but collect
      // them in execution order so the sealed plan and every downstream
      // digest remain deterministic.
      std::vector<std::future<NativeGrantBinding>> grantFutures;
      grantFutures.reserve(execution.roles.size());
      for (const auto& role : execution.roles) {
        control.requireActive();
        const auto offer = std::find_if(offers.begin(), offers.end(), [&](const auto& value) {
          return value.observation().provider == core.assignment.providerByRole.at(role);
        });
        if (offer == offers.end()) {
          throw std::invalid_argument("selected role Provider is outside the admitted offers");
        }
        const auto grantsClient = runtime.grants;
        const auto selectedOffer = *offer;
        const auto security = runtime.security;
        const auto selectedRole = role;
        const auto leaseScope = grantLeaseScope;
        grantFutures.emplace_back(std::async(std::launch::async,
          [grantsClient, &core, selectedOffer, security, grantControl, selectedRole, leaseScope] {
            if (leaseScope)
              return grantsClient->acquire(core, selectedOffer, security, grantControl,
                                           selectedRole, *leaseScope);
            return grantsClient->acquire(core, selectedOffer, security, grantControl,
                                         selectedRole);
          }));
      }
      grants.resize(grantFutures.size());
      std::exception_ptr firstGrantError;
      for (std::size_t index = 0; index < grantFutures.size(); ++index) {
        try {
          grants[index] = grantFutures[index].get();
        }
        catch (...) {
          if (!firstGrantError)
            firstGrantError = std::current_exception();
        }
      }
      if (firstGrantError) std::rethrow_exception(firstGrantError);
      if (grantLease) grantLease->grants = grants;
    }
    control.requireActive();
    logPlanPhase(control.requestId,
                 reusedGrantLease ? "grant_lease_reuse" : "grant_acquire",
                 phaseStarted);
    NativePlannedRequest result;
    phaseStarted = planPhaseStarted();
    const auto ownedSealed = NativePlanSealer::finalizeSecurityOwned(
      core, grants, runtime.security, grantLeaseScope);
    // Only current values are copied; fixed roles stay pinned in their owner.
    // This public context does not carry the owned sealed-plan proof.
    result.sealed = ownedSealed.invocationContext();
    logPlanPhase(control.requestId, "security_finalize", phaseStarted);
    NativeProjectionContext projection{epochMs(), runtime.noProgressMs, runtime.maxSegments};
    projection.logicalInputDigest = encoded.logicalInputDigest;
    projection.inputLayoutDigest = runtime.inputLayoutDigest;
    projection.inlineFirstSegment = true;
    if (grantLease) projection.layoutOwner = &grantLease->projectionLayout;
    projection.tensorSegmentBytesByProvider = std::move(authenticatedTransportByProvider);
    if (context.placementObservationSnapshot.is_object() &&
        context.placementObservationSnapshot.contains("provider_offers")) {
      for (const auto& offer : context.placementObservationSnapshot.at("provider_offers")) {
        if (offer.contains("tensor_segment_payload_bytes") &&
            offer.at("tensor_segment_payload_bytes").is_number_unsigned()) {
          const auto bytes = offer.at("tensor_segment_payload_bytes").get<std::uint64_t>();
          if (bytes != 0 && bytes <= ndn::MAX_NDN_PACKET_SIZE)
            projection.tensorSegmentBytesByProvider.emplace(
              offer.at("provider").get<std::string>(), bytes);
        }
      }
    }
    if (!requestTransferProfile.is_null()) {
      for (std::size_t index = 0; index < execution.dependencies.size(); ++index) {
        const auto& dependency = execution.dependencies[index];
        const auto bytes = estimateNativeRequestTransferEncodedBytesV1(
          requestTransferProfile, model.canonicalSourceDigest,
          encoded.logicalInputDigest, dependency, proposal.roles);
        if (!bytes) continue;
        for (const auto& row : requestTransferProfile.at("dependencies")) {
          if (row.at("producer_role") != dependency.producers.front() ||
              row.at("consumer_role") != dependency.consumers.front()) continue;
          std::vector<std::string> names;
          for (const auto& tensor : row.at("tensors"))
            names.push_back(tensor.at("name").get<std::string>());
          std::sort(names.begin(), names.end());
          projection.tensorPrefetchByDependency.emplace(index,
            NativeTensorPrefetchEstimate{*bytes, std::move(names)});
        }
      }
    }
    std::map<std::string, NativeRoleProjectionInputs> projections;
    phaseStarted = planPhaseStarted();
    if (execution.dependencies.empty())
      projections = NativePlanProjectionBuilder::build(ownedSealed, candidate, offers, projection);
    else {
      std::vector<ndn_service_framework::AckSelectionCandidate> selectedAcks;
      for (const auto& ack : closure.candidates)
        if (core.offerDigestByProvider.count(ack.providerName.toUri())) selectedAcks.push_back(ack);
      NativeGroupKeyAdmission keys(admission, selectedAcks, context, epochMs());
      projections = NativeGroupProjectionBuilder::build(ownedSealed, candidate, keys, projection);
    }
    logPlanPhase(control.requestId, "projection_build", phaseStarted);
    result.grantLease = grantLease;
    if (conversationTurn) {
      phaseStarted = planPhaseStarted();
      bindConversationProjections(projections, result.sealed, *conversationTurn,
        encoded.requestContractDigest, runtime.contract.serviceName,
        core.assignment.providerByRole);
      logPlanPhase(control.requestId, "conversation_projection_binding", phaseStarted);
    }
    phaseStarted = planPhaseStarted();
    auto& plan = result.corePlan;
    plan.ackCollectionTimeMs = static_cast<int>(options.ackTimeoutMs);
    plan.timeoutMs = static_cast<int>(options.timeoutMs);
    std::vector<ndn_service_framework::SelectedParticipant> selected;
    std::map<std::string, std::vector<std::uint8_t>> roleWires;
    if (!runtime.fixedRoleSourcePublisher) {
      roleWires = NativePlanSealer::projectAndEncodeAll(ownedSealed, projections);
    }
    else {
      if (!runtime.fixedRoleAssignments)
        throw std::invalid_argument("fixed role publisher requires its runtime cache");
      for (const auto& role : execution.roles) {
        control.requireActive();
        const auto& provider = core.assignment.providerByRole.at(role);
        auto fixed = runtime.fixedRoleAssignments->findMatching(
          runtime.requesterIdentity, runtime.contract.serviceName, provider, core.roleContracts().at(role));
        auto invocation = NativePlanSealer::projectInvocation(
          ownedSealed, provider, projections.at(role), fixed.get());
        if (fixed && !fixed->matchesExecutionTemplate(invocation)) fixed.reset();
        const bool hit = bool(fixed);
        if (!fixed) {
          fixed = runtime.fixedRoleAssignments->remember(runtime.requesterIdentity,
            runtime.contract.serviceName, nativeFixedRoleAssignmentV1FromProjection(invocation));
          invocation.fixedRoleOwner = fixed->roleOwner();
          invocation.fixedExecutionTemplateOwner = fixed->executionTemplate();
          invocation.selectedRole = {};
          invocation.assembly = {};
        }
        auto source = runtime.fixedRoleAssignments->source(
          runtime.requesterIdentity, runtime.contract.serviceName, *fixed);
        if (!source) {
          auto published = runtime.fixedRoleSourcePublisher(invocation, *fixed, control);
          control.requireActive();
          source = runtime.fixedRoleAssignments->rememberSource(runtime.requesterIdentity,
            runtime.contract.serviceName, *fixed, std::move(published));
        }
        const auto wire = nativeSelectionInvocationV1ToJson(invocation, *fixed, source.get());
        if (wire.size() > (4U << 20))
          throw std::invalid_argument("invocation exceeds external assignment wire limit");
        roleWires.emplace(role, std::vector<std::uint8_t>(wire.begin(), wire.end()));
        result.fixedRoleSources.push_back(source);
        result.fixedAssignments.push_back(fixed);
        logRuntimeEvidence("NDNSF_DI_FIXED_ROLE_USER requestId=" + control.requestId +
          " role=" + role + " cacheHit=" + (hit ? "1" : "0") + " digest=" + fixed->digest());
      }
    }
    for (const auto& role : execution.roles) {
      const auto& provider = core.assignment.providerByRole.at(role);
      const auto ack = std::find_if(closure.candidates.begin(), closure.candidates.end(), [&](const auto& value) {
        return value.providerName.toUri() == provider;
      });
      if (ack == closure.candidates.end()) throw std::invalid_argument("selected provider is outside ACK closure");
      ndn_service_framework::CollaborationRoleSpec spec;
      spec.role = role; spec.service = ndn::Name(runtime.contract.serviceName);
      spec.requiredArtifact = ndn::Name(core.artifacts.artifactNameByRole.at(role));
      spec.assignmentPayload.swap(roleWires.at(role));
      spec.terminalResponseOwner = projections.at(role).dataflow.terminalResponseOwner;
      if (spec.terminalResponseOwner) result.terminalProvider = provider;
      ndn_service_framework::SelectedParticipant participant;
      participant.role = role; participant.service = spec.service; participant.provider = ndn::Name(provider);
      participant.assignedArtifact = spec.requiredArtifact; participant.assignmentPayload = spec.assignmentPayload;
      participant.ack = *ack;
      participant.artifactDataName = core.artifacts.artifactPrefetchRequired
        ? ndn::Name(core.artifacts.sourceByRole.at(role)) : ndn::Name();
      plan.roles.push_back(std::move(spec));
      selected.push_back(std::move(participant));
    }
    logPlanPhase(control.requestId, "selection_roles_build", phaseStarted);
    phaseStarted = planPhaseStarted();
    std::map<std::string, std::set<std::string>> scopes;
    for (const auto& edge : execution.dependencies) {
      plan.dependencies.push_back({edge.producers, edge.consumers, edge.keyScope, ndn::Name(edge.topicPrefix), true});
      scopes[edge.keyScope].insert(edge.producers.begin(), edge.producers.end());
      scopes[edge.keyScope].insert(edge.consumers.begin(), edge.consumers.end());
    }
    for (const auto& scope : scopes)
      plan.keyScopes.push_back({scope.first, std::vector<std::string>(scope.second.begin(), scope.second.end())});
    if (conversationTurn) {
      plan.keyScopes.push_back({"ndnsf-di-conversation-state-v1", execution.roles});
    }
    plan.participantSelector = std::make_shared<const FrozenSelection>(std::move(selected));
    control.requireActive();
    logPlanPhase(control.requestId, "core_plan_finalize", phaseStarted);
    return result;
  }
  throw std::runtime_error("DI_NATIVE_NO_FEASIBLE_CANDIDATE");
  }
  catch (const detail::NativePlacementObservationExpired&) {
    // One full, independently validated baseline pass. Publication can already
    // have happened, but no grants or final plan have been issued before seal.
    // Carry the original absolute policy deadline into the fallback pass.
    control.requireActive();
    if (observationFallback) throw;
    extensionControl.requireActive();
    auto fallbackControl = control;
    fallbackControl.deadline = strategyDeadline;
    logRuntimeEvidence("NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_FALLBACK requestId=" +
      control.requestId + " reason=SNAPSHOT_STALE");
    return planNativeRequestImpl(runtime, options, model, encoded, ports, preparation,
      admission, closure, fallbackControl, wireDeadlineMs, cancelled,
      conversationTurn, nullptr, preparedSplitter, true, decodeCache);
  }
}

} // namespace

std::optional<NativeJson> bindNativeRequestTransferProfileV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const NativeEncodedRequest& request)
{
  return bindNativeRequestTransferProfileV1(profile, sourceDigest, request, nullptr);
}

std::optional<NativeJson> bindNativeRequestTransferProfileV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const NativeEncodedRequest& request, const NativeConversationTurn* turn)
{
  try {
    if (request.recovery || !isDigestValue(sourceDigest) ||
        !isDigestValue(request.logicalInputDigest) ||
        profile.value("schema", std::string{}) != "ndnsf-di-request-transfer-profile-v1" ||
        profile.at("source_digest") != sourceDigest ||
        !profile.value("initial_context", false) || !profile.contains("shape_template") ||
        request.wire.size() > (32ULL << 20)) return std::nullopt;
    const auto& shapeTemplate = profile.at("shape_template");
    const auto& bindings = shapeTemplate.at("bindings");
    const auto& constants = shapeTemplate.at("constants");
    const auto& dependencies = shapeTemplate.at("dependencies");
    if (!bindings.is_array() || bindings.size() > 32 ||
        !constants.is_object() || constants.size() > 32 ||
        !dependencies.is_array() || dependencies.empty() || dependencies.size() > 64)
      return std::nullopt;
    const auto envelope = NativeJson::parse(request.wire.begin(), request.wire.end());
    std::uint64_t pastTokens = 0;
    const bool continued = turn && turn->parent.parentContextEpoch > 0;
    if (continued) {
      if (turn->aborted || turn->ticket.empty() || turn->attempt != 1 ||
          turn->parent.mode != "APPEND_DELTA" ||
          turn->parent.requestContractDigest != request.requestContractDigest ||
          !isDigestValue(request.requestContractDigest) ||
          !isDigestValue(turn->parent.parentCheckpointDigest) ||
          turn->parent.parentCheckpointWire.size() > (4ULL << 20)) return std::nullopt;
      const auto checkpoint = NativeJson::parse(turn->parent.parentCheckpointWire);
      if (checkpoint.at("conversationId") != turn->parent.conversationId ||
          checkpoint.at("contextEpoch") != turn->parent.parentContextEpoch ||
          checkpoint.at("checkpointDigest") != turn->parent.parentCheckpointDigest ||
          checkpoint.at("planRoleMapDigest") != turn->parent.planRoleMapDigest ||
          !checkpoint.at("prefixTokenCount").is_number_integer()) return std::nullopt;
      const auto count = checkpoint.at("prefixTokenCount").get<std::int64_t>();
      if (count <= 0 || count > 1024 * 1024) return std::nullopt;
      pastTokens = static_cast<std::uint64_t>(count);
      if (envelope.at("request_id") != turn->requestId ||
          nativePlanningDigest(request.wire.data(), request.wire.size()) != request.requestContractDigest)
        return std::nullopt;
    }
    if (envelope.value("schema", std::string{}) != "ndnsf-di-request-envelope-v2" ||
        envelope.value("input_transport", std::string{}) != "INLINE") return std::nullopt;
    const auto base64 = envelope.at("input_payload_b64").get<std::string>();
    if (base64.empty() || base64.size() % 4 || base64.size() > 4 * (((16ULL << 20) + 2) / 3))
      return std::nullopt;
    std::vector<std::uint8_t> payload(3 * (base64.size() / 4));
    const auto decoded = EVP_DecodeBlock(payload.data(),
      reinterpret_cast<const unsigned char*>(base64.data()), static_cast<int>(base64.size()));
    const auto padding = (base64.back() == '=') + (base64[base64.size() - 2] == '=');
    if (decoded < padding) return std::nullopt;
    payload.resize(static_cast<std::size_t>(decoded - padding));
    if (payload.size() > (16ULL << 20) ||
        nativePlanningDigest(payload.data(), payload.size()) != request.logicalInputDigest)
      return std::nullopt;
    const auto tensors = decodeTensorBundle(payload);
    std::map<std::string, std::int64_t> values;
    for (auto it = constants.begin(); it != constants.end(); ++it) {
      if (it.key().empty() || it.key().size() > 128 || !it.value().is_number_integer())
        return std::nullopt;
      const auto value = it.value().get<std::int64_t>();
      if (value < 0) return std::nullopt;
      values.emplace(it.key(), value);
    }
    if (continued) {
      const auto symbol = shapeTemplate.at("past_sequence_symbol").get<std::string>();
      const auto value = values.find(symbol);
      if (symbol.empty() || value == values.end() || value->second != 0) return std::nullopt;
      value->second = static_cast<std::int64_t>(pastTokens);
    }
    for (const auto& binding : bindings) {
      const auto symbol = binding.at("symbol").get<std::string>();
      const auto name = binding.at("tensor").get<std::string>();
      if (symbol.empty() || symbol.size() > 128 || !binding.at("axis").is_number_integer())
        return std::nullopt;
      const auto signedAxis = binding.at("axis").get<std::int64_t>();
      if (signedAxis < 0) return std::nullopt;
      const auto axis = static_cast<std::size_t>(signedAxis);
      const auto tensor = std::find_if(tensors.begin(), tensors.end(),
        [&](const auto& candidate) { return candidate.name == name; });
      if (tensor == tensors.end() || axis >= tensor->shape.size() || tensor->shape[axis] <= 0 ||
          !values.emplace(symbol, tensor->shape[axis]).second) return std::nullopt;
    }
    std::size_t expressionNodes = 0;
    std::function<std::int64_t(const NativeJson&, unsigned)> resolve;
    resolve = [&](const NativeJson& expression, unsigned depth) -> std::int64_t {
      if (depth > 16 || ++expressionNodes > 4096)
        throw std::invalid_argument("shape expression budget exceeded");
      if (expression.is_number_integer()) {
        const auto value = expression.get<std::int64_t>();
        if (value < 0) throw std::invalid_argument("negative shape expression");
        return value;
      }
      if (expression.is_string()) return values.at(expression.get<std::string>());
      if (!expression.is_object() || expression.size() != 1 ||
          !expression.contains("add") || !expression.at("add").is_array() ||
          expression.at("add").size() != 2)
        throw std::invalid_argument("unsupported shape expression");
      const auto left = resolve(expression.at("add")[0], depth + 1);
      const auto right = resolve(expression.at("add")[1], depth + 1);
      if (right > std::numeric_limits<std::int64_t>::max() - left)
        throw std::invalid_argument("shape expression overflow");
      return left + right;
    };
    auto bound = profile;
    bound.erase("shape_template");
    bound["input_digest"] = request.logicalInputDigest;
    bound["dependencies"] = dependencies;
    if (continued) {
      bound["schema"] = "ndnsf-di-request-transfer-profile-v2";
      bound["initial_context"] = false;
      bound["request_contract_digest"] = request.requestContractDigest;
      bound["parent_context_epoch"] = turn->parent.parentContextEpoch;
      bound["parent_checkpoint_digest"] = turn->parent.parentCheckpointDigest;
      bound["past_sequence_length"] = pastTokens;
    }
    for (auto& dependency : bound["dependencies"]) {
      auto& members = dependency.at("tensors");
      if (!members.is_array() || members.empty() || members.size() > 128) return std::nullopt;
      for (auto& member : members) {
        auto& shape = member.at("shape");
        if (!shape.is_array() || shape.size() > 16) return std::nullopt;
        for (auto& dimension : shape) {
          const auto size = resolve(dimension, 0);
          if (size <= 0) return std::nullopt;
          dimension = size;
        }
      }
    }
    return bound;
  }
  catch (const std::exception&) { return std::nullopt; }
}

static std::optional<std::uint64_t> estimateRequestTransferBytes(
  const NativeJson& profile, const std::string& sourceDigest,
  const std::string& inputDigest, const NativeDependencySpec& dependency,
  const std::vector<NativeSelectionRoleV3>& roles, bool includeFraming)
{
  try {
    const auto schema = profile.value("schema", std::string{});
    const bool initial = schema == "ndnsf-di-request-transfer-profile-v1" &&
      profile.value("initial_context", false);
    const bool continued = schema == "ndnsf-di-request-transfer-profile-v2" &&
      !profile.value("initial_context", true) &&
      isDigestValue(profile.value("request_contract_digest", std::string{})) &&
      isDigestValue(profile.value("parent_checkpoint_digest", std::string{})) &&
      profile.at("parent_context_epoch").is_number_integer() &&
      profile.at("parent_context_epoch").get<std::int64_t>() > 0 &&
      profile.at("past_sequence_length").is_number_integer() &&
      profile.at("past_sequence_length").get<std::int64_t>() > 0 &&
      profile.at("past_sequence_length").get<std::int64_t>() <= 1024 * 1024;
    if ((!initial && !continued) || !isDigestValue(sourceDigest) ||
        !isDigestValue(inputDigest) || profile.at("source_digest") != sourceDigest ||
        profile.at("input_digest") != inputDigest || dependency.producers.size() != 1 ||
        dependency.consumers.size() != 1 || dependency.tensors.size() != 1 ||
        !dependency.redistributions.empty() || dependency.operationKind != "ACTIVATION")
      return std::nullopt;
    const NativeSelectionRoleV3* producer = nullptr;
    const NativeSelectionRoleV3* consumer = nullptr;
    for (const auto& role : roles) {
      if (role.selectedRole == dependency.producers.front()) {
        if (producer) return std::nullopt;
        producer = &role;
      }
      if (role.selectedRole == dependency.consumers.front()) {
        if (consumer) return std::nullopt;
        consumer = &role;
      }
    }
    if (!producer || !consumer) return std::nullopt;
    // Match the production projection's shared output/input bundle rule.
    std::map<std::string, NativeAssemblyTensorContractV3> shared;
    const auto append = [&](const auto& tensor) {
      const auto match = std::find_if(consumer->expectedInputs.begin(),
        consumer->expectedInputs.end(), [&](const auto& input) {
          return tensor.name == input.name && tensor.dtype == input.dtype &&
            tensor.shape == input.shape;
        });
      if (match != consumer->expectedInputs.end()) shared.emplace(tensor.name, tensor);
    };
    for (const auto& tensor : producer->expectedOutputs) append(tensor);
    for (const auto& tensor : producer->expectedInputs) append(tensor);
    if (shared.empty() || shared.size() > 128) return std::nullopt;
    const NativeJson* selected = nullptr;
    for (const auto& row : profile.at("dependencies")) {
      if (row.at("producer_role") == dependency.producers.front() &&
          row.at("consumer_role") == dependency.consumers.front()) {
        if (selected) return std::nullopt;
        selected = &row;
      }
    }
    if (!selected || !selected->at("tensors").is_array() ||
        selected->at("tensors").size() != shared.size()) return std::nullopt;
    const std::map<std::string, std::uint64_t> widths{
      {"float32", 4}, {"float16", 2}, {"int64", 8}, {"bool", 1}, {"uint8", 1}};
    std::set<std::string> seen;
    std::uint64_t total = 0;
    std::vector<TensorBundleEncodingMember> members;
    if (includeFraming) members.reserve(shared.size());
    for (const auto& tensor : selected->at("tensors")) {
      const auto name = tensor.at("name").get<std::string>();
      const auto found = shared.find(name);
      const auto dtype = tensor.at("dtype").get<std::string>();
      if (found == shared.end() || !seen.insert(name).second ||
          dtype != found->second.dtype || !widths.count(dtype) ||
          !tensor.at("shape").is_array() ||
          tensor.at("shape").size() != found->second.shape.size()) return std::nullopt;
      std::uint64_t bytes = widths.at(dtype);
      for (std::size_t i = 0; i < found->second.shape.size(); ++i) {
        const auto& dimension = tensor.at("shape")[i];
        if (!dimension.is_number_integer()) return std::nullopt;
        const auto size = dimension.get<std::int64_t>();
        if (size <= 0 || (std::holds_alternative<std::int64_t>(found->second.shape[i]) &&
            std::get<std::int64_t>(found->second.shape[i]) != size) ||
            static_cast<std::uint64_t>(size) > std::numeric_limits<std::uint64_t>::max() / bytes)
          return std::nullopt;
        bytes *= static_cast<std::uint64_t>(size);
      }
      if (bytes > std::numeric_limits<std::uint64_t>::max() - total) return std::nullopt;
      total += bytes;
      if (includeFraming)
        members.push_back({name.size(), found->second.shape.size(), bytes});
    }
    return includeFraming ? tensorBundleEncodedSize(members) : total;
  }
  catch (const std::exception&) { return std::nullopt; }
}

std::optional<std::uint64_t> estimateNativeRequestTransferBytesV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const std::string& inputDigest, const NativeDependencySpec& dependency,
  const std::vector<NativeSelectionRoleV3>& roles)
{
  return estimateRequestTransferBytes(profile, sourceDigest, inputDigest, dependency, roles, false);
}

std::optional<std::uint64_t> estimateNativeRequestTransferEncodedBytesV1(
  const NativeJson& profile, const std::string& sourceDigest,
  const std::string& inputDigest, const NativeDependencySpec& dependency,
  const std::vector<NativeSelectionRoleV3>& roles)
{
  return estimateRequestTransferBytes(profile, sourceDigest, inputDigest, dependency, roles, true);
}

NativePlacementObservationSnapshotV4
mergeNativePlacementProbeRoundV1(
  const NativePlacementObservationSnapshotV4& original,
  const NativePlacementProbeRoundV1& round,
  const NativeOfferBindingContext& context,
  const std::uint64_t nowMs)
{
  auto result = original;
  mergeTargetedPlacementProbeRound(result, round, context, nowMs);
  return result;
}

NativePlacementProbeIntentV1 selectNativePlacementProbeRemeasurementV1(
  const NativeOfferBindingContext& context,
  const NativePlacementProbeRoundV1& round,
  const std::uint64_t nowMs)
{
  NativePlacementProbeIntentV1 selected;
  try {
    const auto& ack = context.placementObservationSnapshot;
    if (context.deadlineMs <= nowMs ||
        ack.at("expires_at_ms").get<std::uint64_t>() <= nowMs)
      return selected;
    NativePlacementObservationSnapshotV4 snapshot;
    snapshot.value = {{"provider_offers", ack.at("provider_offers")},
                      {"eligible_link_observations", NativeJson::array()}};
    snapshot.expiresAtMs = ack.at("expires_at_ms").get<std::uint64_t>();
    const auto admitted = mergeNativePlacementProbeRoundV1(snapshot, round, context, nowMs);
    if (admitted.expiresAtMs <= nowMs) return selected;
    for (const auto& observer : round.intent.observers) {
      std::size_t rows = 0;
      bool unstable = false;
      for (const auto& item : admitted.value.at("eligible_link_observations")) {
        if (item.at("source_kind") != "TARGETED_RESPONSE" ||
            item.at("reported_by") != observer.observerProvider) continue;
        ++rows;
        const auto& row = item.at("row");
        unstable = unstable || (row.at("kind") == "ACTIVE_PROBE_GOODPUT" &&
          row.value("probe_rate_status", std::string{}) == "UNKNOWN" &&
          row.value("probe_rate_reason", std::string{}) == "ARRIVAL_DISPERSION_UNSTABLE");
      }
      if (unstable && rows == observer.rttPairs.size() + observer.goodputEdges.size())
        selected.observers.push_back(observer);
    }
  }
  catch (const std::exception&) { selected.observers.clear(); }
  return selected;
}

bool isCompleteNativePlacementProbeCacheResponseV1(
  const NativeOfferBindingContext& context,
  const NativePlacementProbeObserverV1& observer,
  const std::string& nonce, const NativeJson& response,
  const std::uint64_t nowMs)
{
  try {
    const auto& ack = context.placementObservationSnapshot;
    if (context.placementObservationSnapshotDigest.empty() ||
        context.deadlineMs <= nowMs ||
        ack.at("expires_at_ms").get<std::uint64_t>() <= nowMs ||
        !response.at("cache_complete").get<bool>() ||
        response.at("omitted_rtt_count").get<std::uint64_t>() != 0 ||
        response.at("omitted_goodput_count").get<std::uint64_t>() != 0)
      return false;
    for (const auto& offer : ack.at("provider_offers"))
      if (offer.at("expires_at_ms").get<std::uint64_t>() <= nowMs)
        return false;
    for (const auto& row : response.at("observations")) {
      const auto observed = row.at("observed_at_ms").get<std::uint64_t>();
      if (observed > nowMs || nowMs - observed >=
            ndn_service_framework::NETWORK_PROBE_CACHE_MAX_AGE_MS)
        return false;
    }

    NativePlacementProbeRoundV1 round;
    round.intent.observers.push_back(observer);
    round.intentNonceByObserver.emplace(observer.observerProvider, nonce);
    round.responseByObserver.emplace(observer.observerProvider, response);
    NativePlacementObservationSnapshotV4 snapshot;
    // Count only rows admitted from this reply, never previously cached ACK rows.
    snapshot.value = {{"provider_offers", ack.at("provider_offers")},
                      {"eligible_link_observations", NativeJson::array()}};
    snapshot.expiresAtMs = ack.at("expires_at_ms").get<std::uint64_t>();
    const auto merged = mergeNativePlacementProbeRoundV1(snapshot, round, context, nowMs);
    if (merged.expiresAtMs <= nowMs) return false;
    std::set<std::pair<std::string, std::string>> rtt, goodput;
    for (const auto& item : merged.value.at("eligible_link_observations")) {
      if (item.at("source_kind") != "TARGETED_RESPONSE" ||
          item.at("reported_by") != observer.observerProvider)
        return false;
      const auto& row = item.at("row");
      const auto edge = std::make_pair(row.at("src_peer").get<std::string>(),
                                      row.at("dst_peer").get<std::string>());
      if (row.at("kind") == "ACTIVE_PROBE_RTT") {
        if (!rtt.insert(edge).second) return false;
      }
      else if (row.at("kind") == "ACTIVE_PROBE_GOODPUT") {
        if (row.value("probe_rate_status", std::string{}) != "MEASURED_GOODPUT" ||
            !goodput.insert(edge).second)
          return false;
      }
      else return false;
    }
    std::set<std::pair<std::string, std::string>> expectedRtt, expectedGoodput;
    for (const auto& edge : observer.rttPairs)
      expectedRtt.emplace(edge.sourceProvider, edge.destinationProvider);
    for (const auto& edge : observer.goodputEdges)
      expectedGoodput.emplace(edge.sourceProvider, edge.destinationProvider);
    return rtt == expectedRtt && goodput == expectedGoodput;
  }
  catch (const std::exception&) { return false; }
}

NativePlacementObservationSnapshotV4
buildNativePlacementObservationSnapshotV4(
  const NativeOfferBindingContext& context,
  const std::string& ackClosedDigest,
  const std::vector<NativeAdmittedOfferV4>& offers,
  const std::vector<NativeJson>& requesterProviderSamples,
  const NativePreparedPlanningCache& planningCache,
  std::size_t maxCandidates,
  std::uint64_t nowMs,
  const NativeGraphSnapshot* modelGraph,
  const NativeJson* requestTransferProfile)
{
  constexpr std::uint64_t MAX_PROVIDER_OBSERVATION_AGE_MS = 5000;
  if (context.requestId.empty() || context.attempt == 0 ||
      context.requesterIdentity.empty() ||
      ndn::Name(context.requesterIdentity).toUri() != context.requesterIdentity ||
      context.serviceName.empty() ||
      !isDigestValue(context.modelDigest) || !isDigestValue(context.graphDigest) ||
      !isDigestValue(ackClosedDigest) || context.deadlineMs <= nowMs ||
      !isDigestValue(context.candidateSetDigest) ||
      !isDigestValue(context.candidateArtifactSetDigest) ||
      !isDigestValue(context.probeScopeDigest) ||
      context.probeScopeDigest != placementProbeScopeDigest(context) ||
      context.candidateProviders.empty() || context.candidateProviders.size() > 16 ||
      !std::is_sorted(context.candidateProviders.begin(), context.candidateProviders.end()) ||
      std::adjacent_find(context.candidateProviders.begin(), context.candidateProviders.end()) !=
        context.candidateProviders.end() ||
      offers.size() != context.candidateProviders.size() ||
      requesterProviderSamples.size() != context.candidateProviders.size())
    throw std::invalid_argument("incomplete V4 observation snapshot input");
  if (modelGraph && modelGraph->graphDigest != context.graphDigest)
    throw std::invalid_argument("V4 tensor byte source has a foreign graph digest");

  const auto preparedInventory = buildNativeCandidateArtifactInventoryV4(
    planningCache, maxCandidates);
  if (!preparedInventory || preparedInventory->digest != context.candidateArtifactSetDigest)
    throw std::invalid_argument("V4 prepared graph differs from the advertised inventory");

  const auto preparedArtifactWire = nativeCanonicalJson(NativeJson(preparedInventory->identities));
  const NativeJson candidateArtifacts(context.candidateArtifacts);
  // Both checks bind the same immutable invocation-local inventory. Preserve
  // exact canonical equality and digest verification without rendering twice.
  const auto candidateArtifactWire = nativeCanonicalJson(candidateArtifacts);
  if (preparedArtifactWire != candidateArtifactWire)
    throw std::invalid_argument("V4 prepared graph differs from the advertised inventory");
  if (candidateArtifacts.empty() ||
      candidateArtifacts.size() > MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS ||
      nativePlanningDigest(candidateArtifactWire) !=
        context.candidateArtifactSetDigest ||
      nativePlanningDigest(nativeCanonicalJson(NativeJson(context.candidateProviders))) !=
        context.candidateSetDigest)
    throw std::invalid_argument("V4 observation snapshot candidate binding is invalid");

  std::vector<const NativeAdmittedOfferV4*> orderedOffers;
  orderedOffers.reserve(offers.size());
  for (const auto& offer : offers) orderedOffers.push_back(&offer);
  std::sort(orderedOffers.begin(), orderedOffers.end(), [](const auto* left, const auto* right) {
    return left->observation().provider < right->observation().provider;
  });
  std::map<std::string, std::string> bootByProvider;
  std::map<std::string, std::string> topologyByProvider;
  std::uint64_t expiresAtMs = context.deadlineMs;
  NativeJson providerOfferValues = NativeJson::array();
  for (std::size_t i = 0; i < orderedOffers.size(); ++i) {
    const auto& admitted = *orderedOffers[i];
    const auto& offer = admitted.observation();
    if (offer.provider != context.candidateProviders[i] ||
        offer.requestId != context.requestId || offer.attempt != context.attempt ||
        offer.service != context.serviceName || offer.modelDigest != context.modelDigest ||
        offer.graphDigest != context.graphDigest ||
        offer.candidateSetDigest != context.candidateSetDigest ||
        offer.candidateArtifactSetDigest != context.candidateArtifactSetDigest ||
        offer.probeScopeDigest != context.probeScopeDigest ||
        admitted.signerIdentity() != offer.provider ||
        admitted.signerKeyLocator().empty() || !isDigestValue(admitted.ackWireDigest()) ||
        offer.bootEpoch.empty() || offer.capturedAtMs == 0 || offer.capturedAtMs > nowMs ||
        nowMs - offer.capturedAtMs > MAX_PROVIDER_OBSERVATION_AGE_MS ||
        offer.expiresAtMs <= nowMs || offer.expiresAtMs > context.deadlineMs ||
        offer.capturedAtMs > std::numeric_limits<std::uint64_t>::max() -
          MAX_PROVIDER_OBSERVATION_AGE_MS ||
        !bootByProvider.emplace(offer.provider, offer.bootEpoch).second)
      throw std::invalid_argument("V4 observation snapshot offer binding is invalid");
    expiresAtMs = std::min(expiresAtMs, offer.expiresAtMs);
    expiresAtMs = std::min(expiresAtMs,
      offer.capturedAtMs + MAX_PROVIDER_OBSERVATION_AGE_MS);
    const auto& resources = offer.values.at("resources");
    if (resources.is_object()) {
      if (resources.contains("topology_digest") &&
          resources.at("topology_digest").is_string())
        topologyByProvider.emplace(offer.provider,
          resources.at("topology_digest").get<std::string>());
      const auto measuredAt = resources.value("measured_at_ms", std::uint64_t{0});
      if (measuredAt == 0 || measuredAt > nowMs || measuredAt >
            std::numeric_limits<std::uint64_t>::max() - MAX_PROVIDER_OBSERVATION_AGE_MS)
        throw std::invalid_argument("V4 provider resource sample time is invalid");
      expiresAtMs = std::min(expiresAtMs,
        measuredAt + MAX_PROVIDER_OBSERVATION_AGE_MS);
    }
    const auto& residency = offer.values.at("residency");
    if (residency.is_array()) {
      for (const auto& row : residency) {
        const auto rowExpiry = row.at("expires_at_ms").get<std::uint64_t>();
        const auto rowCapturedAt = row.at("captured_at_ms").get<std::uint64_t>();
        if (rowCapturedAt == 0 || rowCapturedAt > nowMs ||
            rowCapturedAt > std::numeric_limits<std::uint64_t>::max() -
              MAX_PROVIDER_OBSERVATION_AGE_MS)
          throw std::invalid_argument("V4 residency sample time is invalid");
        expiresAtMs = std::min(expiresAtMs, rowExpiry);
        expiresAtMs = std::min(expiresAtMs,
          rowCapturedAt + MAX_PROVIDER_OBSERVATION_AGE_MS);
      }
    }
    providerOfferValues.push_back(NativeJson{
      {"provider", offer.provider},
      {"boot_epoch", offer.bootEpoch},
      {"captured_at_ms", offer.capturedAtMs},
      {"expires_at_ms", offer.expiresAtMs},
      {"offer_digest", offer.offerDigest},
      {"signer_key_id", offer.signerKeyId},
      {"signer_identity", admitted.signerIdentity()},
      {"signer_key_locator", admitted.signerKeyLocator()},
      {"ack_wire_digest", admitted.ackWireDigest()},
      {"values", offer.values}});
    if (admitted.tensorSegmentBytes() != 0)
      providerOfferValues.back()["tensor_segment_payload_bytes"] =
        admitted.tensorSegmentBytes();
  }

  std::vector<NativeJson> orderedSamples = requesterProviderSamples;
  std::sort(orderedSamples.begin(), orderedSamples.end(), [](const auto& left, const auto& right) {
    return left.value("provider", std::string{}) < right.value("provider", std::string{});
  });
  NativeJson requesterSamples = NativeJson::array();
  for (std::size_t i = 0; i < orderedSamples.size(); ++i) {
    const auto& sample = orderedSamples[i];
    if (!sample.is_object() || sample.value("provider", std::string{}) !=
          context.candidateProviders[i] || !sample.contains("sample"))
      throw std::invalid_argument("requester telemetry snapshot roster mismatch");
    if (!sample.at("sample").is_null()) {
      const auto& value = sample.at("sample");
      if (!value.is_object() || !value.contains("observed_at_ms") ||
          !value.at("observed_at_ms").is_number_unsigned() ||
          !value.contains("provider") || !value.at("provider").is_string() ||
          value.at("provider").get<std::string>() != context.candidateProviders[i] ||
          !value.contains("service") || !value.at("service").is_string() ||
          value.at("service").get<std::string>() != context.serviceName)
        throw std::invalid_argument("requester telemetry snapshot time is invalid");
      const auto observedAt = value.at("observed_at_ms").get<std::uint64_t>();
      if (observedAt == 0 || observedAt > nowMs ||
          nowMs - observedAt > MAX_REQUESTER_SAMPLE_AGE_MS ||
          observedAt > std::numeric_limits<std::uint64_t>::max() -
            MAX_REQUESTER_SAMPLE_AGE_MS)
        throw std::invalid_argument("requester telemetry snapshot is stale");
      expiresAtMs = std::min(expiresAtMs, observedAt + MAX_REQUESTER_SAMPLE_AGE_MS);
    }
    requesterSamples.push_back(sample);
  }

  auto eligibleLinkObservations = NativeJson::array();
  std::size_t ineligibleLinkObservationCount = 0;
  for (const auto* admitted : orderedOffers) {
    const auto& offer = admitted->observation();
    const auto& rows = offer.values.at("link_observations");
    for (const auto& row : rows) {
      const auto src = row.at("src_peer").get<std::string>();
      const auto dst = row.at("dst_peer").get<std::string>();
      const auto srcBoot = row.at("src_boot_epoch");
      const auto dstBoot = row.at("dst_boot_epoch");
      const auto topology = row.at("topology_digest");
      const auto ownTopology = topologyByProvider.find(offer.provider);
      const bool endpointBootsMatch = srcBoot.is_string() && dstBoot.is_string() &&
        bootByProvider.count(src) && bootByProvider.count(dst) &&
        srcBoot.get<std::string>() == bootByProvider.at(src) &&
        dstBoot.get<std::string>() == bootByProvider.at(dst);
      const bool topologyMatches = topology.is_string() &&
        isDigestValue(topology.get<std::string>()) && ownTopology != topologyByProvider.end() &&
        ownTopology->second == topology.get<std::string>();
      if (!endpointBootsMatch || !topologyMatches) {
        ++ineligibleLinkObservationCount;
        continue;
      }
      const auto rowCapturedAt = row.at("captured_at_ms").get<std::uint64_t>();
      if (rowCapturedAt == 0 || rowCapturedAt > nowMs ||
          rowCapturedAt > std::numeric_limits<std::uint64_t>::max() -
            MAX_PROVIDER_OBSERVATION_AGE_MS)
        throw std::invalid_argument("V4 link sample time is invalid");
      eligibleLinkObservations.push_back(NativeJson{
        {"reported_by", offer.provider}, {"source_kind", "ACK_CACHE"}, {"row", row}});
      expiresAtMs = std::min(expiresAtMs,
        row.at("expires_at_ms").get<std::uint64_t>());
      expiresAtMs = std::min(expiresAtMs,
        rowCapturedAt + MAX_PROVIDER_OBSERVATION_AGE_MS);
    }
  }

  const auto preparedCandidateCount = std::min(
    planningCache.candidates.size(), maxCandidates);
  NativeJson preparedRoleGraph = NativeJson::array();
  for (std::size_t i = 0; i < preparedCandidateCount; ++i) {
    const auto& prepared = planningCache.candidates[i];
    const auto& candidate = prepared.candidate;
    if (!isDigestValue(candidate.candidateDigest) ||
        !isDigestValue(candidate.graphDigest))
      throw std::invalid_argument("V4 prepared candidate graph identity is invalid");
    NativeJson roles = NativeJson::array();
    if (const auto* cached = planningCache.preparedRoleJson(i)) roles = *cached;
    else for (const auto& role : prepared.roles)
      roles.push_back(nativeAssemblyJson(role));
    NativeJson dependencyEdges = NativeJson::array();
    for (const auto& dependency : candidate.executionPlan.dependencies) {
      auto byteEstimate = dependencyByteEstimate(dependency, modelGraph);
      if (byteEstimate.first.is_null() && requestTransferProfile) {
        const auto bytes = estimateNativeRequestTransferEncodedBytesV1(
          *requestTransferProfile,
          requestTransferProfile->value("source_digest", std::string{}),
          requestTransferProfile->value("input_digest", std::string{}),
          dependency, prepared.roles);
        if (bytes)
          byteEstimate = {NativeJson(*bytes), "request-bound-encoded-tensor-bundle"};
      }
      for (const auto& producer : dependency.producers) {
        for (const auto& consumer : dependency.consumers) {
          dependencyEdges.push_back(NativeJson{
            {"producer_role", producer}, {"consumer_role", consumer},
            {"tensors", dependency.tensors},
            {"estimated_bytes", byteEstimate.first},
            {"estimated_bytes_source", byteEstimate.second.empty()
              ? NativeJson(nullptr) : NativeJson(byteEstimate.second)},
            {"expected_segments", dependency.expectedSegments},
            {"operation_kind", dependency.operationKind}});
        }
      }
    }
    preparedRoleGraph.push_back(NativeJson{
      {"candidate_digest", candidate.candidateDigest},
      {"graph_digest", candidate.graphDigest},
      {"node_roles", candidate.nodeRoles},
      {"stage_order", candidate.executionPlan.roles},
      {"roles", std::move(roles)},
      {"dependencies", std::move(dependencyEdges)}});
  }
  if (expiresAtMs <= nowMs)
    throw std::invalid_argument("V4 observation snapshot is already expired");

  NativePlacementObservationSnapshotV4 result;
  result.capturedAtMs = nowMs;
  result.expiresAtMs = expiresAtMs;
  result.value = NativeJson{
    {"schema", "ndnsf-placement-observation-snapshot-v4"},
    {"request_id", context.requestId},
    {"attempt", context.attempt},
    {"requester_identity", context.requesterIdentity},
    {"service", context.serviceName},
    {"model_digest", context.modelDigest},
    {"graph_digest", context.graphDigest},
    {"ack_closed_digest", ackClosedDigest},
    {"candidate_set_digest", context.candidateSetDigest},
    {"candidate_artifact_set_digest", context.candidateArtifactSetDigest},
    {"probe_scope_digest", context.probeScopeDigest},
    {"candidate_artifacts", std::move(candidateArtifacts)},
    {"provider_roster", context.candidateProviders},
    {"rtt_pairs", context.allowedRttPairs},
    {"goodput_edges", context.allowedGoodputEdges},
    {"prepared_role_graph", std::move(preparedRoleGraph)},
    {"requester_provider_samples", std::move(requesterSamples)},
    {"provider_offers", std::move(providerOfferValues)},
    {"eligible_link_observations", std::move(eligibleLinkObservations)},
    {"ineligible_link_observation_count", ineligibleLinkObservationCount},
    {"plan_deadline_ms", context.deadlineMs},
    {"captured_at_ms", result.capturedAtMs},
    {"expires_at_ms", result.expiresAtMs}};
  if (requestTransferProfile) {
    if (!isDigestValue(requestTransferProfile->value("request_contract_digest", std::string{})))
      throw std::invalid_argument("request transfer profile lacks request binding");
    result.value["request_transfer_profile"] = *requestTransferProfile;
  }
  result.digest = nativePlanningDigest(nativePlacementObservationCanonicalJson(
    result.value, context.preparedPlanningTemplate.get()));
  return result;
}

std::optional<NativePlacementObservationSnapshotV4>
trySelectNativePlacementObservationSnapshotV4(
  const NativeOfferBindingContext& context,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeOfferAdmission& admission,
  const NativePreparedPlanningCache& planningCache,
  const std::size_t maxCandidates,
  const std::uint64_t nowMs,
  const NativeGraphSnapshot* modelGraph,
  std::string* unavailableReason,
  const NativeJson* requestTransferProfile)
{
  return trySelectNativePlacementObservationSnapshotV4(context, closure, admission,
    planningCache, maxCandidates, nowMs, modelGraph, unavailableReason,
    requestTransferProfile, nullptr);
}

std::optional<NativePlacementObservationSnapshotV4>
trySelectNativePlacementObservationSnapshotV4(
  const NativeOfferBindingContext& context,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeOfferAdmission& admission,
  const NativePreparedPlanningCache& planningCache,
  const std::size_t maxCandidates, const std::uint64_t nowMs,
  const NativeGraphSnapshot* modelGraph, std::string* unavailableReason,
  const NativeJson* requestTransferProfile, NativeOfferDecodeCache* decodeCache)
{
  if (unavailableReason)
    unavailableReason->clear();
  const auto reject = [unavailableReason] (const char* reason)
      -> std::optional<NativePlacementObservationSnapshotV4> {
    if (unavailableReason)
      *unavailableReason = reason;
    return std::nullopt;
  };

  const bool closureDigestPresent = !closure.digest.empty();
  const bool closureRequestMatches =
    closure.requestId == ndn::Name(context.requestId);
  const bool candidateArtifactSetDigestPresent =
    !context.candidateArtifactSetDigest.empty();
  const bool probeScopeDigestPresent = !context.probeScopeDigest.empty();
  const bool candidateProvidersNonEmpty = !context.candidateProviders.empty();
  const bool candidateProvidersWithinLimit =
    context.candidateProviders.size() <= 16;
  const bool candidateProvidersSorted = std::is_sorted(
    context.candidateProviders.begin(), context.candidateProviders.end());
  const bool candidateProvidersUnique = std::adjacent_find(
    context.candidateProviders.begin(), context.candidateProviders.end()) ==
      context.candidateProviders.end();
  const bool ackCandidateCountMatches =
    closure.candidates.size() == context.candidateProviders.size();
  if (!closureDigestPresent || !closureRequestMatches ||
      !candidateArtifactSetDigestPresent || !probeScopeDigestPresent ||
      !candidateProvidersNonEmpty || !candidateProvidersWithinLimit ||
      !candidateProvidersSorted || !candidateProvidersUnique ||
      !ackCandidateCountMatches) {
    try {
      std::ostringstream diagnostic;
      diagnostic << "DEBUG-SPEC194-R16 ACK_CLOSURE_INVALID requestId="
                 << context.requestId
                 << " closureDigestPresent=" << closureDigestPresent
                 << " closureRequestMatches=" << closureRequestMatches
                 << " candidateArtifactSetDigestPresent="
                 << candidateArtifactSetDigestPresent
                 << " probeScopeDigestPresent=" << probeScopeDigestPresent
                 << " candidateProvidersNonEmpty=" << candidateProvidersNonEmpty
                 << " candidateProvidersWithinLimit="
                 << candidateProvidersWithinLimit
                 << " candidateProvidersSorted=" << candidateProvidersSorted
                 << " candidateProvidersUnique=" << candidateProvidersUnique
                 << " ackCandidateCount=" << closure.candidates.size()
                 << " candidateProviderCount="
                 << context.candidateProviders.size()
                 << " ackCandidateCountMatches=" << ackCandidateCountMatches;
      logRuntimeEvidence(diagnostic.str());
    }
    catch (...) {
      // The diagnostic must never change V4-to-V3 fallback behavior.
    }
    return reject("ACK_CLOSURE_INVALID");
  }

  std::vector<std::string> ackProviders;
  ackProviders.reserve(closure.candidates.size());
  for (const auto& ack : closure.candidates) {
    ackProviders.push_back(ack.providerName.toUri());
  }
  std::sort(ackProviders.begin(), ackProviders.end());
  if (ackProviders != context.candidateProviders ||
      std::adjacent_find(ackProviders.begin(), ackProviders.end()) !=
        ackProviders.end()) {
    return reject("ACK_PROVIDER_ROSTER_MISMATCH");
  }

  std::vector<NativeAdmittedOfferV4> verifiedOffers;
  try {
    std::string offerUnavailableReason;
    auto admitted = admission.tryVerifyV4Batch(
      closure.candidates, context, nowMs, &offerUnavailableReason, decodeCache);
    if (!admitted) return reject(offerUnavailableReason.c_str());
    verifiedOffers = std::move(*admitted);
  }
  catch (const std::exception&) {
    return reject("PROVIDER_V4_OFFER_REJECTED");
  }

  std::vector<NativeJson> requesterSamples;
  try {
    std::map<std::string, std::string> providerBootByName;
    for (const auto& admitted : verifiedOffers) {
      const auto& offer = admitted.observation();
      providerBootByName.emplace(offer.provider, offer.bootEpoch);
    }
    std::vector<const ndn_service_framework::AckSelectionCandidate*> orderedAcks;
    orderedAcks.reserve(closure.candidates.size());
    for (const auto& ack : closure.candidates) {
      orderedAcks.push_back(&ack);
    }
    std::sort(orderedAcks.begin(), orderedAcks.end(),
      [] (const auto* left, const auto* right) {
        return left->providerName.toUri() < right->providerName.toUri();
      });
    requesterSamples.reserve(orderedAcks.size());
    for (const auto* ack : orderedAcks) {
      const auto boot = providerBootByName.find(ack->providerName.toUri());
      if (boot == providerBootByName.end())
        return reject("REQUESTER_ACK_PROVIDER_BOOT_MISSING");
      requesterSamples.push_back(snapshotAckTelemetry(*ack, nowMs, boot->second));
    }
  }
  catch (const std::exception&) {
    return reject("REQUESTER_ACK_TELEMETRY_INVALID");
  }

  try {
    return buildNativePlacementObservationSnapshotV4(
      context, closure.digest, verifiedOffers, requesterSamples,
      planningCache, maxCandidates, nowMs, modelGraph, requestTransferProfile);
  }
  catch (const std::exception&) {
    return reject("V4_SNAPSHOT_BUILD_REJECTED");
  }
}

NativePlannedRequest planNativeRequest(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const NativeModelSplitStrategy& splitter, const NativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn,
  const NativePlacementProbeRoundV1* placementProbeRound)
{
  return planNativeRequest(runtime, options, model, encoded, splitter, placement,
    preparation, admission, closure, control, wireDeadlineMs, std::move(cancelled),
    conversationTurn, placementProbeRound, nullptr);
}

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
  NativeOfferDecodeCache* decodeCache)
{
  NativeStrategyPorts ports;
  ports.splitterIdentity = splitter.identity();
  ports.placementIdentity = placement.identity();
  ports.enumerate = [&splitter](const NativeModelDescriptor& descriptor,
                                const NativeGraphSnapshot& graph,
                                const NativeCandidateBudget& budget,
                                const ExtensionControl& extension) {
    extension.requireActive();
    auto result = splitter.enumerate(descriptor, graph, budget);
    extension.requireActive();
    return result;
  };
  ports.proposeRoles = [&placement](const NativeOfferBindingContext& context,
                                    const std::string& ackClosedDigest,
                                    const std::vector<NativeSelectionRoleV3>& roles,
                                    const std::vector<NativeAdmittedOfferV3>& offers,
                                    std::uint64_t nowMs,
                                    const ExtensionControl& extension) {
    extension.requireActive();
    auto result = placement.proposeRoles(context, ackClosedDigest, roles, offers, nowMs);
    extension.requireActive();
    return result;
  };
  return planNativeRequestImpl(runtime, options, model, encoded, ports, preparation,
                               admission, closure, control, wireDeadlineMs,
                               std::move(cancelled), conversationTurn,
                               // Catalog splitters implement both interfaces. Reuse
                               // their exact owned template through the legacy SDK
                               // entry too; a foreign/custom object still fails the
                               // template's model and splitter pointer checks.
                               placementProbeRound,
                               dynamic_cast<const CooperativeModelSplitStrategy*>(&splitter),
                               false, decodeCache);
}

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
  const NativePlacementProbeRoundV1* placementProbeRound)
{
  return planNativeRequestCooperative(runtime, options, model, encoded, splitter,
    placement, preparation, admission, closure, control, wireDeadlineMs,
    std::move(cancelled), conversationTurn, placementProbeRound, nullptr);
}

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
  NativeOfferDecodeCache* decodeCache)
{
  NativeStrategyPorts ports;
  ports.splitterIdentity = splitter.identity();
  ports.placementIdentity = placement.identity();
  ports.enumerate = [&splitter](const NativeModelDescriptor& descriptor,
                                const NativeGraphSnapshot& graph,
                                const NativeCandidateBudget& budget,
                                const ExtensionControl& extensionControl) {
    return splitter.enumerate(descriptor, graph, budget, extensionControl);
  };
  ports.proposeRoles = [&placement](const NativeOfferBindingContext& context,
                                    const std::string& ackClosedDigest,
                                    const std::vector<NativeSelectionRoleV3>& roles,
                                    const std::vector<NativeAdmittedOfferV3>& offers,
                                    std::uint64_t nowMs,
                                    const ExtensionControl& extensionControl) {
    return placement.proposeRoles(context, ackClosedDigest, roles, offers, nowMs,
                                   extensionControl);
  };
  return planNativeRequestImpl(runtime, options, model, encoded, ports, preparation,
                               admission, closure, control, wireDeadlineMs,
                               std::move(cancelled), conversationTurn,
                               placementProbeRound, &splitter, false, decodeCache);
}
} // namespace ndnsf::di
