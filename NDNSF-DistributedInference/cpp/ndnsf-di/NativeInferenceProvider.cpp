#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "ndn-service-framework/NetworkProbeCachePolicy.hpp"

#include <ndn-cxx/name.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ndnsf::di {

using ServiceProvider = ndn_service_framework::ServiceProvider;

// One served native service of the host. host->targets keeps at most one
// entry per serviceName: a closed target survives as the draining record
// until a same-name re-serve replaces it (closing-service semantics of the
// native-provider lifecycle design).  The record lives as long as the map
// entry, in-flight router copies, or a registration handle hold it.
struct Target
{
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  std::shared_ptr<ExecutionLeaseService> lease;
  std::shared_ptr<std::atomic<bool>> draining;
};

// Host singleton state (see NativeInferenceProvider.hpp). Lease resolvers use
// weak references so the host's target graph cannot form a self-cycle through
// ExecutionLeaseService. Core handlers retain the host while their runtime
// state may still refer to the host-owned lease table.
struct HostState
{
  std::string providerName;
  std::string providerBootId;
  std::size_t workerSlots = 1;
  std::shared_ptr<SharedExecutionLeaseState> sharedLease;
  std::shared_ptr<ServiceProvider::ServiceRegistration> fixedLease;
  std::mutex routerMutex;
  std::map<std::string, std::shared_ptr<Target>> targets;
};

struct NativeServiceRegistration::State
{
  std::string serviceName;
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  std::shared_ptr<std::atomic<bool>> draining;
};

namespace {

constexpr std::size_t MAX_PLACEMENT_PROBE_REQUEST_BYTES = 4096;
constexpr std::size_t MAX_PLACEMENT_PROBE_RESPONSE_BYTES = NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES;
constexpr std::size_t MAX_PLACEMENT_PROBE_ROSTER = 17;
constexpr std::size_t MAX_PLACEMENT_PROBE_EDGES = 16;
constexpr std::size_t MAX_PLACEMENT_PROBE_RESPONSE_ROWS = 16;
constexpr std::uint64_t MAX_PLACEMENT_PROBE_BUDGET_MS = 5000;
constexpr std::uint64_t MAX_PLACEMENT_PROBE_SAMPLE_AGE_MS = 5000;

bool
isCanonicalProviderName(const std::string& value)
{
  if (value.empty() || value.front() != '/') {
    return false;
  }
  try {
    const ndn::Name name(value);
    return !name.empty() &&
      (name.toUri(ndn::name::UriFormat::CANONICAL) == value ||
       name.toUri(ndn::name::UriFormat::ALTERNATE) == value);
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::uint64_t
readPlacementProbeUint(const NativeJson& value, const char* field)
{
  if (!value.is_object() || !value.contains(field)) {
    throw std::invalid_argument("missing placement probe integer");
  }
  const auto& item = value.at(field);
  if (item.is_number_unsigned()) {
    return item.get<std::uint64_t>();
  }
  if (!item.is_number_integer() || item.get<std::int64_t>() < 0) {
    throw std::invalid_argument("invalid placement probe integer");
  }
  return static_cast<std::uint64_t>(item.get<std::int64_t>());
}

std::string
readPlacementProbeString(const NativeJson& value, const char* field)
{
  if (!value.is_object() || !value.contains(field) ||
      !value.at(field).is_string()) {
    throw std::invalid_argument("missing placement probe string");
  }
  return value.at(field).get<std::string>();
}

void
requirePlacementProbeKeys(const NativeJson& value,
                          std::initializer_list<const char*> expected)
{
  const auto hintFields = value.is_object() && value.contains("peer_boot_epochs") ? 1 : 0;
  if (!value.is_object() || value.size() != expected.size() + hintFields) {
    throw std::invalid_argument("placement probe request has wrong fields");
  }
  for (const auto* field : expected) {
    if (!value.contains(field)) {
      throw std::invalid_argument("placement probe request is incomplete");
    }
  }
}

using PlacementProbePair = std::pair<std::string, std::string>;

bool
containsPlacementProbeName(const std::set<std::string>& names, const ndn::Name& expected)
{
  return std::any_of(names.begin(), names.end(), [&expected](const auto& value) {
    return ndn::Name(value) == expected;
  });
}

std::vector<PlacementProbePair>
readPlacementProbePairs(const NativeJson& value, const char* field,
                        const std::string& localProvider,
                        const std::set<std::string>& roster,
                        bool rttPairs)
{
  if (!value.is_object() || !value.contains(field) ||
      !value.at(field).is_array() || value.at(field).size() >
        MAX_PLACEMENT_PROBE_EDGES) {
    throw std::invalid_argument("placement probe edge list exceeds bounds");
  }
  std::vector<PlacementProbePair> pairs;
  pairs.reserve(value.at(field).size());
  std::set<std::pair<ndn::Name, ndn::Name>> identities;
  for (const auto& item : value.at(field)) {
    if (!item.is_object() || item.size() != 2 ||
        !item.contains("src_provider") || !item.contains("dst_provider") ||
        !item.at("src_provider").is_string() ||
        !item.at("dst_provider").is_string()) {
      throw std::invalid_argument("placement probe edge is malformed");
    }
    const auto source = item.at("src_provider").get<std::string>();
    const auto destination = item.at("dst_provider").get<std::string>();
    if (!isCanonicalProviderName(source) ||
        !isCanonicalProviderName(destination) || ndn::Name(source) == ndn::Name(destination) ||
        !containsPlacementProbeName(roster, ndn::Name(source)) ||
        !containsPlacementProbeName(roster, ndn::Name(destination)) ||
        (rttPairs ? ndn::Name(source) != ndn::Name(localProvider) :
                    ndn::Name(destination) != ndn::Name(localProvider)) ||
        !identities.emplace(ndn::Name(source), ndn::Name(destination)).second) {
      throw std::invalid_argument("placement probe edge is outside its scope");
    }
    const PlacementProbePair pair{source, destination};
    if (!pairs.empty() && !(pairs.back() < pair)) {
      throw std::invalid_argument("placement probe edges are not canonical");
    }
    pairs.push_back(pair);
  }
  return pairs;
}

NativeJson
placementProbePairsJson(const std::vector<PlacementProbePair>& pairs)
{
  NativeJson result = NativeJson::array();
  for (const auto& pair : pairs) {
    result.push_back(NativeJson{{"src_provider", pair.first},
                                {"dst_provider", pair.second}});
  }
  return result;
}

NativeJson
placementProbeScopeIdentity(const std::string& requestId,
                            std::uint64_t attempt,
                            const std::string& requester,
                            const std::string& observer,
                            const std::string& service,
                            const std::vector<std::string>& roster,
                            const std::string& rosterDigest,
                            const std::vector<PlacementProbePair>& rttPairs,
                            const std::vector<PlacementProbePair>& goodputEdges,
                            std::uint64_t deadlineMs,
                            const std::string& intentNonce)
{
  return NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", requestId}, {"attempt", attempt},
    {"requester_identity", requester}, {"observer_provider", observer},
    {"service", service}, {"provider_roster", roster},
    {"roster_digest", rosterDigest},
    {"rtt_pairs", placementProbePairsJson(rttPairs)},
    {"goodput_edges", placementProbePairsJson(goodputEdges)},
    {"deadline_ms", deadlineMs}, {"intent_nonce", intentNonce}};
}

NativeJson
placementProbeObservationJson(
  const ndn_service_framework::NetworkTelemetrySnapshot& sample,
  std::uint64_t deadlineMs)
{
  const auto observedAt = sample.lastUpdatedMs;
  const auto sampleExpiry = observedAt >
      std::numeric_limits<std::uint64_t>::max() - MAX_PLACEMENT_PROBE_SAMPLE_AGE_MS
    ? std::numeric_limits<std::uint64_t>::max()
    : observedAt + MAX_PLACEMENT_PROBE_SAMPLE_AGE_MS;
  const auto expiresAt = std::min(deadlineMs, sampleExpiry);
  const bool isRtt = sample.kind ==
    ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt;
  const double value = isRtt ? sample.rttMs : sample.goodputMbps;
  if ((!isRtt && sample.kind !=
         ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) ||
      !std::isfinite(value) || value < 0 || observedAt == 0 || expiresAt < observedAt) {
    throw std::invalid_argument("placement probe sample is not encodable");
  }
  // RTT has no packet-train measurements. Keep its authenticated identity and
  // freshness, without spending the bounded response on empty goodput fields.
  if (isRtt) {
    return NativeJson{
      {"observer", sample.providerName.toUri()},
      {"target", sample.peerName.toUri()}, {"kind", "active_probe_rtt"},
      {"value", value}, {"observed_at_ms", observedAt},
      {"expires_at_ms", expiresAt}, {"peer_boot_epoch", sample.peerBootEpoch},
      {"data_name", sample.dataName.toUri()},
      {"target_signature_validated", sample.targetSignatureValidated}};
  }
  NativeJson probeRtt = NativeJson::array();
  for (const auto value : sample.probeRttMs)
    probeRtt.push_back(value);
  NativeJson interestGaps = NativeJson::array();
  for (const auto value : sample.probeInterestGapsMs)
    interestGaps.push_back(value);
  NativeJson dataArrivalGaps = NativeJson::array();
  for (const auto value : sample.probeDataArrivalGapsMs)
    dataArrivalGaps.push_back(value);
  NativeJson probeWireBytes = NativeJson::array();
  for (const auto value : sample.probeDataWireBytes)
    probeWireBytes.push_back(value);
  NativeJson result{
    {"observer", sample.providerName.toUri()},
    {"target", sample.peerName.toUri()},
    {"kind", isRtt ? "active_probe_rtt" : "active_probe_goodput"},
    {"value", value}, {"observed_at_ms", observedAt},
    {"expires_at_ms", expiresAt},
    {"payload_bytes", sample.payloadBytes}, {"wire_bytes", sample.wireBytes},
    {"elapsed_ms", sample.elapsedMs},
    {"peer_boot_epoch", sample.peerBootEpoch},
    {"data_name", sample.dataName.toUri()},
    {"train_complete", sample.probeComplete},
    {"target_signature_validated", sample.targetSignatureValidated},
    {"probe_profile_validated", sample.probeSignatureValid},
    {"received_segments", sample.receivedSegments},
    {"sent_segments", sample.probeSegmentsSent},
    {"probe_rtt_ms", std::move(probeRtt)},
    {"probe_interest_gaps_ms", std::move(interestGaps)},
    {"probe_data_arrival_gaps_ms", std::move(dataArrivalGaps)},
    {"probe_data_wire_bytes", std::move(probeWireBytes)},
    {"profile_digest", sample.probeProfileDigest}};
  result.update(nativeProbeTrainRateMetadataV4(sample));
  return result;
}

void
dropPlacementProbeRow(ndnsf::di::ServiceProvider::PeerProbeCollection& collection,
                      const ndn_service_framework::NetworkTelemetrySnapshot& row)
{
  if (row.kind == ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt) {
    ++collection.omittedTargetCount;
  }
  else if (row.kind ==
           ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) {
    ++collection.omittedGoodputEdgeCount;
  }
}

bool
isUsablePlacementProbeRow(
  const ndn_service_framework::NetworkTelemetrySnapshot& sample)
{
  if (sample.kind == ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt) {
    return sample.targetSignatureValidated && std::isfinite(sample.rttMs) &&
           sample.rttMs > 0;
  }
  if (sample.kind ==
      ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) {
    return sample.probeComplete && sample.targetSignatureValidated &&
           sample.probeSegmentsSent == ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS &&
           sample.receivedSegments == ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS && sample.payloadBytes > 0 &&
           !sample.probeProfileDigest.empty() && std::isfinite(sample.goodputMbps) &&
           sample.goodputMbps > 0 && std::isfinite(sample.elapsedMs) &&
           sample.elapsedMs > 0;
  }
  return false;
}

NativeJson
placementProbeResponseJson(
  const std::string& requestId, std::uint64_t attempt,
  const std::string& requester, const std::string& observer,
  const std::string& service, const std::string& observerBootEpoch,
  const std::string& scopeDigest, const std::string& intentNonce,
  std::uint64_t deadlineMs, std::uint64_t completedAtMs,
  const ndnsf::di::ServiceProvider::PeerProbeCollection& collection,
  const std::map<ndn::Name, std::uint64_t>& originalExpiries = {},
  bool cacheOnly = false)
{
  NativeJson observations = NativeJson::array();
  for (const auto& sample : collection.observations) {
    const auto expiry = originalExpiries.find(sample.dataName);
    if (cacheOnly && expiry == originalExpiries.end())
      throw std::invalid_argument("cached probe observation has no original expiry");
    observations.push_back(placementProbeObservationJson(sample,
      cacheOnly ? std::min(deadlineMs, expiry->second) : deadlineMs));
  }
  NativeJson response{
    {"schema", "ndnsf-di-placement-probe-response-v1"},
    {"request_id", requestId}, {"attempt", attempt},
    {"requester_identity", requester}, {"observer_provider", observer},
    {"service", service}, {"observer_boot_epoch", observerBootEpoch},
    {"scope_digest", scopeDigest}, {"intent_nonce", intentNonce},
    {"deadline_ms", deadlineMs}, {"completed_at_ms", completedAtMs},
    {"requested_rtt_count", collection.requestedTargetCount},
    {"omitted_rtt_count", collection.omittedTargetCount},
    {"requested_goodput_count", collection.requestedGoodputEdgeCount},
    {"omitted_goodput_count", collection.omittedGoodputEdgeCount},
    {"observations", std::move(observations)}};
  if (cacheOnly) {
    response["cache_complete"] = collection.omittedTargetCount == 0 &&
      collection.omittedGoodputEdgeCount == 0 &&
      collection.observations.size() ==
        collection.requestedTargetCount + collection.requestedGoodputEdgeCount;
  }
  return response;
}

uint64_t
hostEpochMs()
{
  const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
  const auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  return static_cast<uint64_t>(std::max<long long>(0, ms));
}

ndn_service_framework::ResponseMessage
encodePlacementProbeCollection(
  const NativeJson& request, const std::string& providerBootEpoch,
  ndnsf::di::ServiceProvider::PeerProbeCollection collection,
  const std::map<ndn::Name, std::uint64_t>& originalExpiries = {},
  bool cacheOnly = false)
{
  ndn_service_framework::ResponseMessage response;
  try {
    const auto deadlineMs = request.at("deadline_ms").get<std::uint64_t>();
    const auto completedAtMs = hostEpochMs();
    if (completedAtMs >= deadlineMs) {
      response.setStatus(false);
      response.setErrorInfo("placement probe deadline expired");
      return response;
    }
    for (auto it = collection.observations.begin(); it != collection.observations.end();) {
      const auto expiry = originalExpiries.find(it->dataName);
      const bool staleCache = cacheOnly &&
        (expiry == originalExpiries.end() || expiry->second <= completedAtMs ||
         it->lastUpdatedMs == 0 || it->lastUpdatedMs > completedAtMs ||
         completedAtMs - it->lastUpdatedMs >=
           ndn_service_framework::NETWORK_PROBE_CACHE_MAX_AGE_MS);
      if (staleCache || !isUsablePlacementProbeRow(*it)) {
        dropPlacementProbeRow(collection, *it);
        it = collection.observations.erase(it);
      }
      else ++it;
    }
    const auto dropLast = [&] {
      dropPlacementProbeRow(collection, collection.observations.back());
      collection.observations.pop_back();
    };
    while (collection.observations.size() > MAX_PLACEMENT_PROBE_RESPONSE_ROWS)
      dropLast();
    const auto encode = [&] {
      return nativeCanonicalJson(placementProbeResponseJson(
        request.at("request_id").get<std::string>(),
        request.at("attempt").get<std::uint64_t>(),
        request.at("requester_identity").get<std::string>(),
        request.at("observer_provider").get<std::string>(),
        request.at("service").get<std::string>(), providerBootEpoch,
        request.at("scope_digest").get<std::string>(),
        request.at("intent_nonce").get<std::string>(), deadlineMs, completedAtMs,
        collection, originalExpiries, cacheOnly));
    };
    auto encoded = encode();
    while (encoded.size() > MAX_PLACEMENT_PROBE_RESPONSE_BYTES &&
           !collection.observations.empty()) {
      dropLast();
      encoded = encode();
    }
    if (encoded.size() > MAX_PLACEMENT_PROBE_RESPONSE_BYTES) {
      response.setStatus(false);
      response.setErrorInfo("placement probe response exceeds the byte limit");
    }
    else {
      ndn::Buffer bytes(reinterpret_cast<const std::uint8_t*>(encoded.data()),
                        encoded.size());
      response.setStatus(true);
      response.setPayload(bytes, bytes.size());
    }
  }
  catch (const std::exception&) {
    response.setStatus(false);
    response.setErrorInfo("placement probe result could not be encoded");
  }
  return response;
}

// Host-wide compute-slot resolver: scan the host's boot-time slot range
// (host resources, never grown by later serves) and return the first slot
// without an active conflict key. An empty result means the host is
// saturated.  All targets of the same host serialize through the shared
// prepare mutex inside ExecutionLeaseService.
ExecutionLeaseService::ConflictKeyResolver
makeHostSlotResolver(const std::shared_ptr<HostState>& host)
{
  const std::weak_ptr<HostState> weakHost = host;
  return [weakHost](const LeaseOperationRequest&,
                    const ExecutionLeaseRequestContext&) {
    const auto host = weakHost.lock();
    if (!host) {
      return std::vector<std::string>{};
    }
    const auto now = hostEpochMs();
    for (std::size_t slot = 0; slot < host->workerSlots; ++slot) {
      const auto key =
        host->providerName + ":compute-slot:" + std::to_string(slot);
      if (!host->sharedLease->table.hasActiveConflictKey(key, now)) {
        return std::vector<std::string>{key};
      }
    }
    return std::vector<std::string>{};
  };
}

ServiceProvider::AckStrategyHandler
acceptingAckHandler(std::string message)
{
  return [message = std::move(message)](
           const ndn_service_framework::RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = message;
    return decision;
  };
}

// Fixed lease entry handler: route one decoded lease operation to the target
// named by request.targetServiceName. An unknown target and a draining
// target are answered here without reaching any ExecutionLeaseService; the
// target's own instance still re-checks the binding for every operation it
// accepts. No exception ever crosses the Core callback.
ServiceProvider::RequestHandler
makeLeaseRouter(const std::shared_ptr<HostState>& host)
{
  return [host](const ndn::Name& requesterIdentity,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const ndn_service_framework::RequestMessage& request) {
    const auto wirePayload = request.getPayload();
    const std::string payload(
      reinterpret_cast<const char*>(wirePayload.data()), wirePayload.size());
    const ExecutionLeaseRequestContext context{
      requesterIdentity.toUri(), providerName.toUri(), serviceName.toUri(),
      requestId.toUri()};
    const auto now = hostEpochMs();
    // Errors travel inside the encoded LeaseOperationResponse; the wire
    // Response stays status=true so a lease client always decodes one.
    ndn_service_framework::ResponseMessage response;
    response.setStatus(true);
    auto complete = [&response](const std::string& responsePayload) {
      ndn::Buffer bytes(
        reinterpret_cast<const uint8_t*>(responsePayload.data()),
        responsePayload.size());
      response.setPayload(bytes, bytes.size());
      return response;
    };
    try {
      const auto operation = decodeLeaseOperationRequest(payload);
      std::shared_ptr<Target> target;
      {
        std::lock_guard<std::mutex> lock(host->routerMutex);
        const auto it = host->targets.find(operation.targetServiceName);
        if (it != host->targets.end()) {
          target = it->second;
        }
      }
      if (!target) {
        LeaseOperationResponse failure;
        failure.status = false;
        failure.operation = operation.operation;
        failure.reasonCode = "LEASE_SERVICE_MISMATCH";
        return complete(encodeLeaseOperationResponse(failure));
      }
      // Draining fences stop new Prepare/Commit/Renew; Abort/Release keep
      // routing so in-flight records finish their cleanup.
      if (target->draining->load(std::memory_order_relaxed) &&
          operation.operation != LeaseOperation::Abort &&
          operation.operation != LeaseOperation::Release) {
        LeaseOperationResponse failure;
        failure.status = false;
        failure.operation = operation.operation;
        failure.reasonCode = "LEASE_TARGET_DRAINING";
        return complete(encodeLeaseOperationResponse(failure));
      }
      // All Core identity/state/replay validation happens inside the
      // target's ExecutionLeaseService over the shared host table.
      return complete(target->lease->handle(context, payload, now));
    }
    catch (const std::exception&) {
      // A malformed payload or an unexpected failure is answered in-band.
      LeaseOperationResponse failure;
      failure.status = false;
      failure.operation = LeaseOperation::Prepare;
      failure.reasonCode = "LEASE_INTERNAL_ERROR";
      return complete(encodeLeaseOperationResponse(failure));
    }
  };
}

} // namespace

ServiceProvider::AsyncTargetedRequestHandler
makeNativePlacementMeasurementTargetedHandler(ServiceProvider& provider)
{
  return [&provider](const ndn::Name& requesterIdentity,
                     const ndn::Name& providerName,
                     const ndn::Name& serviceName,
                     const ndn::Name&,
                     const ndn_service_framework::RequestMessage& request,
                     ServiceProvider::AsyncTargetedCompletion complete) {
    if (!complete) {
      return;
    }
    const auto fail = [&complete](const std::string& reason) {
      ndn_service_framework::ResponseMessage response;
      response.setStatus(false);
      response.setErrorInfo(reason);
      complete(std::move(response));
    };
    try {
      if (providerName.empty() || provider.getName() != providerName ||
          request.getTargetProvider() != providerName) {
        fail("placement probe target identity mismatch");
        return;
      }
      const auto payload = request.getPayload();
      if (payload.empty() || payload.size() > MAX_PLACEMENT_PROBE_REQUEST_BYTES) {
        fail("placement probe request exceeds the byte limit");
        return;
      }
      const std::string wire(reinterpret_cast<const char*>(payload.data()),
                             payload.size());
      const auto decoded = nativeParseJson(wire);
      requirePlacementProbeKeys(decoded, {
        "action", "schema", "request_id", "attempt", "requester_identity",
        "observer_provider", "service", "provider_roster", "roster_digest",
        "rtt_pairs", "goodput_edges", "measurement_kinds", "deadline_ms",
        "intent_nonce", "scope_digest"});
      const auto action = readPlacementProbeString(decoded, "action");
      if (action != "authorize" && action != "measure" && action != "cache") {
        throw std::invalid_argument("unsupported placement probe action");
      }
      if (action == "authorize" && decoded.contains("peer_boot_epochs")) {
        throw std::invalid_argument("probe epoch hints are measure-only metadata");
      }
      if (readPlacementProbeString(decoded, "schema") !=
          "ndnsf-di-placement-probe-request-v1") {
        throw std::invalid_argument("unsupported placement probe request schema");
      }

      const auto requestId = readPlacementProbeString(decoded, "request_id");
      const auto requester = readPlacementProbeString(decoded, "requester_identity");
      const auto observer = readPlacementProbeString(decoded, "observer_provider");
      const auto service = readPlacementProbeString(decoded, "service");
      const auto rosterDigest = readPlacementProbeString(decoded, "roster_digest");
      const auto intentNonce = readPlacementProbeString(decoded, "intent_nonce");
      const auto scopeDigest = readPlacementProbeString(decoded, "scope_digest");
      const auto attempt = readPlacementProbeUint(decoded, "attempt");
      const auto deadlineMs = readPlacementProbeUint(decoded, "deadline_ms");
      if (!isCanonicalProviderName(requestId) ||
          !isCanonicalProviderName(requester) || ndn::Name(requester) != requesterIdentity ||
          !isCanonicalProviderName(observer) ||
          (action != "authorize" && ndn::Name(observer) != providerName) ||
          (action == "authorize" && ndn::Name(observer) == providerName) ||
          serviceName != ndn::Name(ndn_service_framework::NETWORK_PROBE_SERVICE) ||
          !isCanonicalProviderName(service) ||
          attempt == 0 || intentNonce.size() < 16 || intentNonce.size() > 128 ||
          !isSha256Digest(rosterDigest) || !isSha256Digest(scopeDigest) ||
          !decoded.at("provider_roster").is_array() ||
          decoded.at("provider_roster").size() < 2 ||
          decoded.at("provider_roster").size() > MAX_PLACEMENT_PROBE_ROSTER) {
        throw std::invalid_argument("placement probe request binding is invalid");
      }

      std::vector<std::string> roster;
      roster.reserve(decoded.at("provider_roster").size());
      std::set<std::string> rosterSet;
      std::set<ndn::Name> rosterIdentities;
      std::string previousProvider;
      for (const auto& entry : decoded.at("provider_roster")) {
        if (!entry.is_string()) {
          throw std::invalid_argument("placement probe roster is malformed");
        }
        const auto name = entry.get<std::string>();
        if (!isCanonicalProviderName(name) ||
            (!previousProvider.empty() && previousProvider >= name) ||
            !rosterIdentities.emplace(ndn::Name(name)).second) {
          throw std::invalid_argument("placement probe roster is not canonical");
        }
        previousProvider = name;
        roster.push_back(name);
        rosterSet.insert(name);
      }
      if (!containsPlacementProbeName(rosterSet, ndn::Name(observer)) ||
          !containsPlacementProbeName(rosterSet, providerName) ||
          nativePlanningDigest(nativeCanonicalJson(decoded.at("provider_roster"))) !=
            rosterDigest) {
        throw std::invalid_argument("placement probe roster digest mismatch");
      }

      const auto rttPairs = readPlacementProbePairs(
        decoded, "rtt_pairs", observer, rosterSet, true);
      const auto goodputEdges = readPlacementProbePairs(
        decoded, "goodput_edges", observer, rosterSet, false);
      if (rttPairs.empty() && goodputEdges.empty()) {
        throw std::invalid_argument("placement probe request has no authorized edges");
      }
      std::map<ndn::Name, std::string> peerBootEpochHints;
      if (decoded.contains("peer_boot_epochs")) {
        const auto& hints = decoded.at("peer_boot_epochs");
        std::set<std::string> targets;
        for (const auto& pair : rttPairs) targets.insert(pair.second);
        for (const auto& edge : goodputEdges) targets.insert(edge.first);
        if (!hints.is_object() || hints.size() > MAX_PLACEMENT_PROBE_ROSTER) {
          throw std::invalid_argument("placement probe epoch hints are malformed");
        }
        for (auto it = hints.begin(); it != hints.end(); ++it) {
          if (!isCanonicalProviderName(it.key()) ||
              !containsPlacementProbeName(targets, ndn::Name(it.key())) || !it.value().is_string() ||
              it.value().get_ref<const std::string&>().empty() ||
              it.value().get_ref<const std::string&>().size() > 128) {
            throw std::invalid_argument("placement probe epoch hint is out of scope");
          }
          if (!peerBootEpochHints.emplace(ndn::Name(it.key()), it.value().get<std::string>()).second)
            throw std::invalid_argument("placement probe epoch hint repeats a full Name");
        }
      }
      const auto& kinds = decoded.at("measurement_kinds");
      std::vector<std::string> expectedKinds;
      if (!goodputEdges.empty()) expectedKinds.emplace_back("active_probe_goodput");
      if (!rttPairs.empty()) expectedKinds.emplace_back("active_probe_rtt");
      if (!kinds.is_array() || kinds.size() != expectedKinds.size()) {
        throw std::invalid_argument("placement probe measurement kinds mismatch");
      }
      for (std::size_t i = 0; i < expectedKinds.size(); ++i) {
        if (!kinds[i].is_string() || kinds[i].get<std::string>() != expectedKinds[i]) {
          throw std::invalid_argument("placement probe measurement kinds mismatch");
        }
      }

      const auto expectedScope = placementProbeScopeIdentity(
        requestId, attempt, requester, observer, service, roster, rosterDigest,
        rttPairs, goodputEdges, deadlineMs, intentNonce);
      if (nativePlanningDigest(nativeCanonicalJson(expectedScope)) != scopeDigest) {
        throw std::invalid_argument("placement probe scope digest mismatch");
      }

      if (action == "authorize") {
        const auto& localProvider = providerName;
        const bool isRttTarget = std::any_of(
          rttPairs.begin(), rttPairs.end(), [&localProvider](const auto& pair) {
            return ndn::Name(pair.second) == localProvider;
          });
        const bool isGoodputTarget = std::any_of(
          goodputEdges.begin(), goodputEdges.end(),
          [&localProvider](const auto& edge) {
            return ndn::Name(edge.first) == localProvider;
          });
        if (!isRttTarget && !isGoodputTarget) {
          throw std::invalid_argument(
            "placement probe authorization target is outside requested edges");
        }
      }

      const auto wallNow = std::chrono::system_clock::now();
      const auto wallEpoch = wallNow.time_since_epoch();
      const auto wholeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        wallEpoch);
      const auto nowMs = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, wholeMs.count()));
      if (deadlineMs <= nowMs) {
        fail("placement probe deadline expired");
        return;
      }
      auto remaining = std::chrono::milliseconds(std::min<std::uint64_t>(
        deadlineMs - nowMs, MAX_PLACEMENT_PROBE_BUDGET_MS));
      if (deadlineMs - nowMs <= MAX_PLACEMENT_PROBE_BUDGET_MS) {
        remaining -= std::chrono::duration_cast<std::chrono::milliseconds>(
          wallEpoch - wholeMs);
      }
      if (remaining <= std::chrono::milliseconds::zero()) {
        fail("placement probe deadline expired");
        return;
      }
      const auto collectionStart = std::chrono::steady_clock::now();
      const auto probeDeadline = collectionStart + remaining;
      if (action == "cache") {
        // This authenticated service read cannot authorize a peer, create a
        // collection scope or send probe Interests. Retain original validity.
        ServiceProvider::PeerProbeCollection collection;
        collection.requestedTargetCount = rttPairs.size();
        collection.requestedGoodputEdgeCount = goodputEdges.size();
        std::vector<ndn::Name> peers;
        for (const auto& hint : peerBootEpochHints) peers.push_back(hint.first);
        std::map<ndn::Name, std::uint64_t> originalExpiries;
        std::set<std::pair<ndn_service_framework::NetworkTelemetrySampleKind, ndn::Name>> seen;
        for (const auto& cached : provider.getCachedPeerProbeSamples(peers)) {
          const auto& sample = cached.first;
          const auto hint = peerBootEpochHints.find(sample.peerName);
          const bool isRtt = sample.kind ==
            ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt;
          const auto pair = isRtt
            ? std::make_pair(ndn::Name(observer), sample.peerName)
            : std::make_pair(sample.peerName, ndn::Name(observer));
          const auto& allowed = isRtt ? rttPairs : goodputEdges;
          if (sample.providerName != providerName || hint == peerBootEpochHints.end() ||
              sample.peerBootEpoch != hint->second || cached.second <= nowMs ||
              !std::any_of(allowed.begin(), allowed.end(), [&pair](const auto& edge) {
                return ndn::Name(edge.first) == pair.first && ndn::Name(edge.second) == pair.second;
              }) ||
              originalExpiries.count(sample.dataName) ||
              !seen.emplace(sample.kind, sample.peerName).second)
            continue;
          originalExpiries.emplace(sample.dataName, cached.second);
          collection.observations.push_back(sample);
        }
        const auto rttCount = static_cast<std::size_t>(std::count_if(
          collection.observations.begin(), collection.observations.end(),
          [] (const auto& sample) { return sample.kind ==
            ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt; }));
        collection.omittedTargetCount = rttPairs.size() - rttCount;
        collection.omittedGoodputEdgeCount = goodputEdges.size() -
          (collection.observations.size() - rttCount);
        complete(encodePlacementProbeCollection(decoded, provider.getProviderBootEpoch(),
          std::move(collection), originalExpiries, true));
        return;
      }
      const auto authorizeScope = [&] {
        return provider.authorizeCandidatePeerObservationScope(
            scopeDigest, [&] {
              std::vector<ndn::Name> names;
              names.reserve(roster.size());
              for (const auto& name : roster) names.emplace_back(name);
              return names;
            }(), [&] {
              std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
              edges.reserve(rttPairs.size());
              for (const auto& pair : rttPairs) {
                edges.push_back({ndn::Name(pair.first), ndn::Name(pair.second)});
              }
              return edges;
            }(), [&] {
              std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
              edges.reserve(goodputEdges.size());
              for (const auto& edge : goodputEdges) {
                edges.push_back({ndn::Name(edge.first), ndn::Name(edge.second)});
              }
              return edges;
            }(), probeDeadline);
      };
      if (!authorizeScope()) {
        fail("placement probe scope rejected");
        return;
      }

      if (action == "authorize") {
        auto authorizationAck = NativeJson{
          {"schema", "ndnsf-di-placement-probe-scope-ack-v1"},
          {"provider", providerName.toUri()},
          {"observer_provider", observer},
          {"scope_digest", scopeDigest},
          {"boot_epoch", provider.getProviderBootEpoch()},
          {"status", "authorized"}};
        auto authorizationWire = nativeCanonicalJson(authorizationAck);
        if (authorizationWire.size() > 512) {
          // Optional reuse metadata must not make a previously fitting scope
          // ACK fail; omit it and retain the original fresh-measurement path.
          authorizationAck.erase("boot_epoch");
          authorizationWire = nativeCanonicalJson(authorizationAck);
        }
        if (authorizationWire.empty() || authorizationWire.size() > 512) {
          fail("placement probe scope acknowledgement exceeds the byte limit");
          return;
        }
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_PROVIDER_PEER_PROBE_SCOPE_AUTHORIZED provider=" +
          providerName.toUri() + " observer=" + observer +
          " scope=" + scopeDigest);
        ndn_service_framework::ResponseMessage response;
        response.setStatus(true);
        ndn::Buffer responsePayload(
          reinterpret_cast<const std::uint8_t*>(authorizationWire.data()),
          authorizationWire.size());
        response.setPayload(responsePayload, responsePayload.size());
        complete(std::move(response));
        return;
      }

      const auto providerBootEpoch = provider.getProviderBootEpoch();
      provider.collectCandidatePeerObservations(
        [&] {
          std::vector<ndn::Name> names;
          names.reserve(roster.size());
          for (const auto& name : roster) names.emplace_back(name);
          return names;
        }(), [&] {
          std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
          edges.reserve(rttPairs.size());
          for (const auto& pair : rttPairs) {
            edges.push_back({ndn::Name(pair.first), ndn::Name(pair.second)});
          }
          return edges;
        }(), [&] {
          std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
          edges.reserve(goodputEdges.size());
          for (const auto& edge : goodputEdges) {
            edges.push_back({ndn::Name(edge.first), ndn::Name(edge.second)});
          }
          return edges;
        }(), scopeDigest, probeDeadline, {},
        [probeRequest = decoded, providerBootEpoch, complete = std::move(complete)]
        (ServiceProvider::PeerProbeCollection collection) mutable {
          complete(encodePlacementProbeCollection(
            probeRequest, providerBootEpoch, std::move(collection)));
        }, true, peerBootEpochHints);
    }
    catch (const std::exception&) {
      fail("invalid placement probe request");
    }
  };
}

NativeServiceRegistration::NativeServiceRegistration(std::shared_ptr<State> state)
  : m_state(std::move(state))
{
}

NativeServiceRegistration::~NativeServiceRegistration() noexcept
{
  close();
}

void
NativeServiceRegistration::close() noexcept
{
  if (!m_state) {
    return;
  }
  if (m_state->draining) {
    m_state->draining->store(true, std::memory_order_relaxed);
  }
  if (m_state->core) {
    m_state->core->close();
  }
}

bool
NativeServiceRegistration::closed() const noexcept
{
  return !m_state || !m_state->core || m_state->core->closed();
}

bool
NativeServiceRegistration::valid() const noexcept
{
  return m_state != nullptr;
}

uint64_t
NativeServiceRegistration::generation() const noexcept
{
  return (m_state && m_state->core) ? m_state->core->generation() : 0;
}

const std::string&
NativeServiceRegistration::serviceName() const noexcept
{
  static const std::string empty;
  return m_state ? m_state->serviceName : empty;
}

NativeInferenceProvider::NativeInferenceProvider(
  std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
  std::shared_ptr<const NativeAdapterRegistry> adapters)
  : m_provider(std::move(provider)), m_adapters(std::move(adapters))
{
  if (!m_provider) {
    throw std::invalid_argument(
      "NativeInferenceProvider requires a Core ServiceProvider");
  }
  if (!m_adapters) {
    throw std::invalid_argument(
      "NativeInferenceProvider requires the NativeAdapterRegistry");
  }
}

NativeInferenceProvider::~NativeInferenceProvider() noexcept
{
  stop();
}

NativeServiceRegistration
NativeInferenceProvider::serve(const NativeServiceDefinition& service,
                               const NativeProviderHandlerConfig& config)
{
  if (service.serviceName.empty()) {
    throw std::invalid_argument(
      "native service definition requires a serviceName");
  }
  if (service.allowedRoles.empty()) {
    throw std::invalid_argument(
      "native service definition requires allowedRoles");
  }

  // The identity advertised in offers, execution evidence, leases, and
  // cross-Provider data names must be the identity actually owned by this
  // ServiceProvider.  Leaving this optional let a MiniNDN fixture work by
  // accident while a multi-host deployment published a valid-looking
  // capability for a different node and then failed at Selection/data fetch.
  ndn::Name configuredProvider;
  try {
    configuredProvider = ndn::Name(config.localProviderName);
  }
  catch (const std::exception&) {
    throw std::invalid_argument(
      "native service config providerName is not a valid NDN name");
  }
  const auto actualProvider = m_provider->getName();
  if (configuredProvider.empty() || actualProvider.empty() ||
      configuredProvider != actualProvider) {
    throw std::invalid_argument(
      "native service config providerName must match ServiceProvider identity");
  }
  const auto canonicalProviderName = actualProvider.toUri();
  if (config.providerBootId.empty()) {
    throw std::invalid_argument(
      "native service config providerBootId is required");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_stopped) {
    throw std::runtime_error("NativeInferenceProvider host is stopped");
  }

  // The first serve builds the host singleton and its fixed lease entry in a
  // local transaction.  Do not publish m_host until the target runtime and
  // scoped Core registration have both succeeded; otherwise a failed first
  // serve would pin an identity/lease table that has no usable target.
  auto host = m_host;
  bool hostCreated = false;
  if (!host) {
    host = std::make_shared<HostState>();
    // Persist the canonical NDN URI, not caller spelling.  All lease keys,
    // evidence and cross-Provider data names then use one stable identity even
    // when equivalent escaped component spellings reach separate callers.
    host->providerName = canonicalProviderName;
    host->providerBootId = config.providerBootId;
    host->workerSlots = std::max<std::size_t>(1, config.workerCount);
    host->sharedLease =
      std::make_shared<SharedExecutionLeaseState>(host->providerBootId);
    try {
      host->fixedLease = std::make_shared<ServiceProvider::ServiceRegistration>(
        m_provider->addScopedService(
          ndn::Name(EXECUTION_LEASE_SERVICE_NAME),
          acceptingAckHandler("execution lease service ready"),
          makeLeaseRouter(host),
          ServiceProvider::ServiceInvocationMode::NormalAndTargeted));
    }
    catch (...) {
      throw;
    }
    hostCreated = true;
  }

  std::shared_ptr<std::atomic<bool>> draining;
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  try {
    // A serve must not silently reshape the host's shared boot identity or its
    // compute-slot range: the slot range is host resources configured at boot,
    // not a sum over serves.
    if (canonicalProviderName != host->providerName) {
      throw std::invalid_argument("native service config providerName '"
        + canonicalProviderName + "' conflicts with the host's '"
        + host->providerName + "'");
    }
    if (!config.providerBootId.empty() &&
        config.providerBootId != host->providerBootId) {
      throw std::invalid_argument(
        "native service config providerBootId conflicts with the host's");
    }
    if (std::max<std::size_t>(1, config.workerCount) != host->workerSlots) {
      throw std::invalid_argument(
        "native service config workerCount conflicts with the host's "
        "compute-slot range");
    }

    // Same-name re-serve: an active target is refused; a draining (closed)
    // record may be replaced below by a fresh registration that never reuses
    // the old target's lease instance, draining fence, or bindings.
    {
      std::lock_guard<std::mutex> routerLock(host->routerMutex);
      const auto existing = host->targets.find(service.serviceName);
      if (existing != host->targets.end() &&
          !existing->second->draining->load(std::memory_order_relaxed)) {
        throw std::logic_error("native service is already registered: "
                               + service.serviceName);
      }
    }

    // The host owns the shared lease table: when the configuration opts into
    // execution leases, inject the host table and bind the lease target to
    // this service. The resolver observes the host weakly to avoid a target
    // self-cycle, while the Core handler retains the host until its runtime
    // state has been detached and no longer needs the table.
    NativeProviderHandlerConfig effectiveConfig = config;
    // Runtime evidence must use the same canonical identity as the shared
    // host state; do not let alternate URI spellings split a Provider's
    // cross-machine bindings.
    effectiveConfig.localProviderName = canonicalProviderName;
    if (!config.executionLeaseTargetService.empty()) {
      if (config.executionLeaseTargetService != service.serviceName) {
        throw std::invalid_argument("executionLeaseTargetService '"
          + config.executionLeaseTargetService
          + "' must equal the served serviceName '" + service.serviceName
          + "'");
      }
      if (config.executionLeaseTable != nullptr) {
        throw std::invalid_argument(
          "executionLeaseTable is owned by the host; pass a null table");
      }
      effectiveConfig.executionLeaseTable = &host->sharedLease->table;
    }
    else if (config.executionLeaseTable != nullptr) {
      throw std::invalid_argument(
        "executionLeaseTable requires a non-empty executionLeaseTargetService");
    }

    // Assemble the native runtime first; nothing is registered until the
    // runtime and the observation seam both succeed.
    auto runtime = makeNativeProviderCollaborationRuntime(
      std::move(effectiveConfig));
    if (service.runtimeObserver) {
      service.runtimeObserver(runtime);
    }

    draining = std::make_shared<std::atomic<bool>>(false);
    auto guardedHandler =
      [host, draining, handler = runtime.handler](
        ServiceProvider::CollaborationContext& ctx,
        const ndn_service_framework::RequestMessage& request) {
        // The Core registration gate is authoritative; this fence only covers
        // the tiny window between draining and the Core entry detaching.
        if (draining->load(std::memory_order_relaxed)) {
          ctx.fail("native service is closing");
          return;
        }
        handler(ctx, request);
      };
    if (service.asyncAckHandler && service.ackHandler) {
      throw std::invalid_argument(
        "native service must select either synchronous or asynchronous ACK handling");
    }
    const auto ackHandler = service.ackHandler
      ? service.ackHandler
      : acceptingAckHandler("native provider ready");
    core = std::make_shared<ServiceProvider::ServiceRegistration>(
      service.asyncAckHandler
        ? m_provider->addScopedAsyncCollaborationHandler(
            ndn::Name(service.serviceName), service.allowedRoles,
            service.asyncAckHandler, guardedHandler)
        : m_provider->addScopedCollaborationHandler(
            ndn::Name(service.serviceName), service.allowedRoles,
            ackHandler, ServiceProvider::AsyncAckStrategyHandler{},
            guardedHandler));
    auto target = std::make_shared<Target>();
    target->core = core;
    target->draining = draining;
    target->lease = std::make_shared<ExecutionLeaseService>(
      host->providerName, service.serviceName,
      makeHostSlotResolver(host), host->sharedLease);
    {
      std::lock_guard<std::mutex> routerLock(host->routerMutex);
      host->targets[service.serviceName] = std::move(target);
    }
    // Publish only after the full first target is installed.  m_mutex is held
    // for the entire serve call, so a concurrent serve cannot observe a half
    // initialized host.
    if (hostCreated) {
      m_host = host;
      hostCreated = false;
    }
  }
  catch (...) {
    if (draining) {
      draining->store(true, std::memory_order_relaxed);
    }
    if (core) {
      core->close();
    }
    if (hostCreated) {
      if (host->fixedLease) {
        host->fixedLease->close();
      }
      m_host.reset();
    }
    throw;
  }

  auto state = std::make_shared<NativeServiceRegistration::State>();
  state->serviceName = service.serviceName;
  state->core = std::move(core);
  state->draining = std::move(draining);
  return NativeServiceRegistration(std::move(state));
}

void
NativeInferenceProvider::stop() noexcept
{
  std::shared_ptr<HostState> host;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopped) {
      return;
    }
    m_stopped = true;
    host = m_host;
  }
  if (!host) {
    return;
  }
  if (host->fixedLease) {
    host->fixedLease->close();
  }
  std::vector<std::shared_ptr<Target>> targets;
  {
    std::lock_guard<std::mutex> routerLock(host->routerMutex);
    targets.reserve(host->targets.size());
    for (const auto& entry : host->targets) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target->draining->store(true, std::memory_order_relaxed);
    if (target->core) {
      target->core->close();
    }
  }
}

} // namespace ndnsf::di
