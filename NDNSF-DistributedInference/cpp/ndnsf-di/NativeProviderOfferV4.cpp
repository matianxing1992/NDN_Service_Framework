#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRunnerReuseCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <tuple>

namespace ndnsf::di {
NDN_LOG_INIT(ndnsf.di.NativeProviderOfferV4);

namespace {

constexpr std::size_t MAX_V4_OFFER_BYTES = 4096;
constexpr std::size_t MAX_V4_LINK_ROWS = 16;
constexpr std::uint64_t MAX_TELEMETRY_AGE_MS = 5000;

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool
isCanonicalName(const std::string& value)
{
  if (value.empty() || value.front() != '/')
    return false;
  try {
    const ndn::Name name(value);
    return !name.empty() && name.toUri() == value;
  }
  catch (const std::exception&) {
    return false;
  }
}

using ProviderPair = std::pair<std::string, std::string>;

std::vector<ProviderPair>
parseProviderPairs(const NativeJson& value,
                   const std::vector<std::string>& roster)
{
  if (!value.is_array() || value.size() > 16)
    throw std::invalid_argument("V4 probe pair list is invalid");
  std::vector<ProviderPair> pairs;
  pairs.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_object() || item.size() != 2 ||
        !item.contains("src_provider") || !item.contains("dst_provider") ||
        !item.at("src_provider").is_string() || !item.at("dst_provider").is_string())
      throw std::invalid_argument("V4 probe pair is malformed");
    const auto source = item.at("src_provider").get<std::string>();
    const auto destination = item.at("dst_provider").get<std::string>();
    if (!isCanonicalName(source) || !isCanonicalName(destination) ||
        source == destination ||
        !std::binary_search(roster.begin(), roster.end(), source) ||
        !std::binary_search(roster.begin(), roster.end(), destination))
      throw std::invalid_argument("V4 probe pair endpoint is outside the roster");
    const ProviderPair pair{source, destination};
    if (!pairs.empty() && !(pairs.back() < pair))
      throw std::invalid_argument("V4 probe pairs are not uniquely ordered");
    pairs.push_back(pair);
  }
  return pairs;
}

bool
containsPair(const std::vector<ProviderPair>& pairs,
             const std::string& source,
             const std::string& destination)
{
  return std::binary_search(pairs.begin(), pairs.end(),
                            ProviderPair{source, destination});
}

std::string
computeProbeScopeDigest(const NativeJson& request,
                        const std::string& candidateArtifactSetDigest,
                        const NativeJson& rttPairs,
                        const NativeJson& goodputEdges)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", request.at("request_id")},
    {"attempt", request.at("attempt")},
    {"requester_identity", request.at("requester_identity")},
    {"deadline_ms", request.at("plan_deadline_ms")},
    {"service", request.at("service")},
    {"model_identity_hash", request.at("model_identity_hash")},
    {"graph_digest", request.at("graph_digest")},
    {"candidate_set_digest", request.at("candidate_set_digest")},
    {"candidate_artifact_set_digest", candidateArtifactSetDigest},
    {"rtt_pairs", rttPairs}, {"goodput_edges", goodputEdges}}));
}

template<typename T>
T
required(const NativeJson& value, const char* name)
{
  if (!value.is_object() || !value.contains(name))
    throw std::invalid_argument("missing V4 Provider offer field");
  const auto& field = value.at(name);
  if constexpr (std::is_same_v<T, std::uint64_t>) {
    if (!field.is_number_unsigned() &&
        (!field.is_number_integer() || field.get<std::int64_t>() < 0))
      throw std::invalid_argument("V4 Provider offer integer is not uint64");
  }
  else if constexpr (std::is_same_v<T, double>) {
    if (!field.is_number() || !std::isfinite(field.get<double>()) ||
        field.get<double>() < 0)
      throw std::invalid_argument("V4 Provider offer number is invalid");
  }
  else if constexpr (std::is_same_v<T, bool>) {
    if (!field.is_boolean())
      throw std::invalid_argument("V4 Provider offer boolean is invalid");
  }
  else if constexpr (std::is_same_v<T, std::string>) {
    if (!field.is_string())
      throw std::invalid_argument("V4 Provider offer string is invalid");
  }
  return field.get<T>();
}

void
requireKeys(const NativeJson& value, const std::set<std::string>& allowed)
{
  if (!value.is_object())
    throw std::invalid_argument("V4 Provider offer value is not an object");
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (allowed.count(it.key()) == 0)
      throw std::invalid_argument("V4 Provider offer has an unknown field");
  }
}

NativeJson
resourceValue(const ProviderResourceSnapshot& resources)
{
  NativeJson topologyDigest = resources.topologyDigest.empty()
    ? NativeJson(nullptr) : NativeJson(resources.topologyDigest);
  return NativeJson{
    {"source", resources.source},
    {"sequence", resources.sequence},
    {"measured_at_ms", static_cast<std::uint64_t>(resources.measuredAtMs)},
    {"host_total_memory_bytes", resources.hostTotalMemoryBytes},
    {"host_available_memory_bytes", resources.hostAvailableMemoryBytes},
    {"process_rss_bytes", resources.processRssBytes},
    {"visible_devices", resources.visibleDevices},
    {"topology_digest", std::move(topologyDigest)}};
}

NativeJson
capacityValue(const ProviderRoleWorkerSnapshot& capacity)
{
  return NativeJson{
    {"worker_count", capacity.workerCount},
    {"active_worker_count", capacity.activeWorkerCount},
    {"ready_queue_depth", capacity.readyQueueDepth},
    {"ready_queue_capacity", capacity.readyQueueCapacity},
    {"waiting_for_input_count", capacity.waitingForInputCount},
    {"dependency_wait_worker_count", capacity.dependencyWaitWorkerCount},
    {"dependency_wait_queued_count", capacity.dependencyWaitQueuedCount},
    {"dependency_wait_active_count", capacity.dependencyWaitActiveCount},
    {"stopping", capacity.stopping}};
}

bool
isFreshTelemetry(const NativeProviderTelemetrySnapshot& telemetry,
                 const NativeProviderOfferV4Config& config,
                 std::uint64_t nowMs)
{
  if (nowMs > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      config.telemetryMaxAgeMs >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      telemetry.sampledAtMs <= 0 ||
      static_cast<std::uint64_t>(telemetry.sampledAtMs) > nowMs ||
      nowMs - static_cast<std::uint64_t>(telemetry.sampledAtMs) >
        config.telemetryMaxAgeMs)
    return false;
  if (telemetry.resources.isMeasured() &&
      (!telemetry.resources.matchesIdentity(config.provider, config.bootEpoch) ||
       !telemetry.resources.isFresh(static_cast<std::int64_t>(nowMs),
                                    static_cast<std::int64_t>(config.telemetryMaxAgeMs))))
    return false;
  return true;
}

void
validateResourceValue(const NativeJson& value)
{
  requireKeys(value, {"source", "sequence", "measured_at_ms",
    "host_total_memory_bytes", "host_available_memory_bytes", "process_rss_bytes",
    "visible_devices", "topology_digest"});
  const auto source = required<std::string>(value, "source");
  const auto sequence = required<std::uint64_t>(value, "sequence");
  const auto measuredAt = required<std::uint64_t>(value, "measured_at_ms");
  const auto total = required<std::uint64_t>(value, "host_total_memory_bytes");
  const auto available = required<std::uint64_t>(value, "host_available_memory_bytes");
  (void)required<std::uint64_t>(value, "process_rss_bytes");
  if (source.empty() || sequence == 0 || measuredAt == 0 || total == 0 || available > total ||
      !value.at("visible_devices").is_array())
    throw std::invalid_argument("V4 Provider resource row is invalid");
  std::set<std::string> devices;
  for (const auto& device : value.at("visible_devices")) {
    if (!device.is_string() || device.get<std::string>().empty() ||
        !devices.insert(device.get<std::string>()).second)
      throw std::invalid_argument("V4 Provider device list is invalid");
  }
  const auto& topology = value.at("topology_digest");
  if (!topology.is_null() && (!topology.is_string() || !isDigest(topology.get<std::string>())))
    throw std::invalid_argument("V4 Provider topology digest is invalid");
}

void
validateCapacityValue(const NativeJson& value)
{
  requireKeys(value, {"worker_count", "active_worker_count", "ready_queue_depth",
    "ready_queue_capacity", "waiting_for_input_count", "dependency_wait_worker_count",
    "dependency_wait_queued_count", "dependency_wait_active_count", "stopping"});
  const auto workers = required<std::uint64_t>(value, "worker_count");
  const auto active = required<std::uint64_t>(value, "active_worker_count");
  const auto queueDepth = required<std::uint64_t>(value, "ready_queue_depth");
  const auto queueCapacity = required<std::uint64_t>(value, "ready_queue_capacity");
  (void)required<std::uint64_t>(value, "waiting_for_input_count");
  (void)required<std::uint64_t>(value, "dependency_wait_worker_count");
  (void)required<std::uint64_t>(value, "dependency_wait_queued_count");
  (void)required<std::uint64_t>(value, "dependency_wait_active_count");
  (void)required<bool>(value, "stopping");
  if (active > workers || queueDepth > queueCapacity)
    throw std::invalid_argument("V4 Provider capacity row is inconsistent");
}

bool nullableNumberIsValid(const NativeJson& value, const char* field);
bool isUnsignedInteger(const NativeJson& value);
bool nullableUnsignedIsValid(const NativeJson& value, const char* field);

NativeJson
candidateArtifactIdentity(const NativeJson& value)
{
  requireKeys(value, {"artifact_digest", "model_manifest_digest", "graph_digest",
    "role", "rank", "backend", "protection_epoch"});
  if (value.size() != 7)
    throw std::invalid_argument("V4 request candidate artifact identity has extra fields");
  for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                            "role", "backend", "protection_epoch"}) {
    if (!value.contains(field) || !value.at(field).is_string() ||
        value.at(field).get<std::string>().empty())
      throw std::invalid_argument("V4 candidate artifact identity is incomplete");
  }
  for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest"}) {
    if (!isDigest(value.at(field).get<std::string>()))
      throw std::invalid_argument("V4 candidate artifact digest is invalid");
  }
  if (!isUnsignedInteger(value.at("rank")))
    throw std::invalid_argument("V4 candidate artifact role/rank identity is invalid");
  return value;
}

NativeJson
residencyCandidateIdentity(const NativeJson& row)
{
  NativeJson identity = NativeJson::object();
  for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                            "role", "rank", "backend", "device_set", "protection_epoch"}) {
    if (!row.is_object() || !row.contains(field))
      throw std::invalid_argument("V4 residency row is missing candidate identity");
    identity[field] = row.at(field);
  }
  if (!identity.at("device_set").is_array())
    throw std::invalid_argument("V4 residency device set is invalid");
  std::string previousDevice;
  for (const auto& device : identity.at("device_set")) {
    if (!device.is_string() || device.get<std::string>().empty() ||
        (!previousDevice.empty() && previousDevice >= device.get<std::string>()))
      throw std::invalid_argument("V4 residency device set is not canonical");
    previousDevice = device.get<std::string>();
  }
  return identity;
}

NativeJson
residencyRequestIdentity(const NativeJson& rowIdentity)
{
  NativeJson identity = NativeJson::object();
  for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                            "role", "rank", "backend", "protection_epoch"})
    identity[field] = rowIdentity.at(field);
  return candidateArtifactIdentity(identity);
}

bool
residencyMatchesCandidate(const NativeJson& rowIdentity,
                          const NativeJson& candidates)
{
  const auto rowCandidate = residencyRequestIdentity(rowIdentity);
  const auto concreteBackend = rowCandidate.at("backend").get<std::string>();
  for (const auto& candidateValue : candidates) {
    const auto candidate = candidateArtifactIdentity(candidateValue);
    bool sameArtifact = true;
    for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                              "role", "rank", "protection_epoch"}) {
      if (candidate.at(field) != rowCandidate.at(field)) {
        sameArtifact = false;
        break;
      }
    }
    const auto baseBackend = candidate.at("backend").get<std::string>();
    if (sameArtifact && (concreteBackend == baseBackend ||
        concreteBackend == baseBackend + "-cpu" ||
        concreteBackend == baseBackend + "-cuda"))
      return true;
  }
  return false;
}

std::string
residencyIdentityKey(const NativeJson& row)
{
  return nativeCanonicalJson(residencyCandidateIdentity(row));
}

void
validateResidencyObservation(const NativeJson& row,
                             const NativeJson& candidates,
                             const std::string& providerBootEpoch,
                             std::uint64_t nowMs,
                             std::uint64_t offerExpiresAtMs,
                             std::uint64_t maxAgeMs)
{
  requireKeys(row, {"artifact_digest", "model_manifest_digest", "graph_digest",
    "role", "rank", "backend", "device_set", "protection_epoch", "state",
    "process_epoch", "captured_at_ms", "expires_at_ms", "missing_verified_bytes",
    "estimated_assembly_ms", "estimated_load_ms", "runtime_generation"});
  const auto identity = residencyCandidateIdentity(row);
  if (!residencyMatchesCandidate(identity, candidates))
    throw std::invalid_argument("V4 residency row is outside the request candidate set");

  const auto state = required<std::string>(row, "state");
  const auto processEpoch = required<std::string>(row, "process_epoch");
  const auto capturedAtMs = required<std::uint64_t>(row, "captured_at_ms");
  const auto expiresAtMs = required<std::uint64_t>(row, "expires_at_ms");
  if (std::set<std::string>{"EXACT_LOADED", "ASSEMBLED", "CANONICAL_ONLY",
                            "KNOWN_MISS"}.count(state) == 0 ||
      processEpoch != providerBootEpoch || capturedAtMs == 0 || capturedAtMs > nowMs ||
      expiresAtMs <= nowMs || expiresAtMs <= capturedAtMs ||
      expiresAtMs > offerExpiresAtMs || nowMs - capturedAtMs > maxAgeMs)
    throw std::invalid_argument("V4 residency state or freshness is invalid");
  if (!nullableUnsignedIsValid(row, "missing_verified_bytes") ||
      !nullableNumberIsValid(row, "estimated_assembly_ms") ||
      !nullableNumberIsValid(row, "estimated_load_ms"))
    throw std::invalid_argument("V4 residency cost evidence is invalid");
  const auto& runtimeGeneration = row.at("runtime_generation");
  if ((!runtimeGeneration.is_null() &&
       (!runtimeGeneration.is_string() || runtimeGeneration.get<std::string>().empty())) ||
      (state == "EXACT_LOADED" && runtimeGeneration.is_null()))
    throw std::invalid_argument("V4 loaded residency lacks runtime generation binding");
}

bool
nullableNumberIsValid(const NativeJson& value, const char* field)
{
  const auto& item = value.at(field);
  return item.is_null() ||
    (item.is_number() && std::isfinite(item.get<double>()) && item.get<double>() >= 0);
}

bool
isUnsignedInteger(const NativeJson& value)
{
  return value.is_number_unsigned() ||
    (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

bool
nullableUnsignedIsValid(const NativeJson& value, const char* field)
{
  const auto& item = value.at(field);
  return item.is_null() || isUnsignedInteger(item);
}

int
linkKindPriority(const std::string& kind)
{
  if (kind == "DATA_FETCH") return 0;
  if (kind == "ACTIVE_PROBE_GOODPUT") return 1;
  if (kind == "ACTIVE_PROBE_RTT") return 2;
  if (kind == "RESPONSE_RTT") return 3;
  return 4;
}

void
validateLinkObservation(const NativeJson& row,
                        const std::string& provider,
                        const std::string& bootEpoch,
                        const std::vector<std::string>& roster,
                        std::uint64_t nowMs,
                        std::uint64_t expiresAtMs,
                        std::uint64_t maxAgeMs)
{
  std::set<std::string> allowed{
    "src_peer", "dst_peer", "direction_semantics", "kind", "observer_peer",
    "observer_boot_epoch", "src_boot_epoch", "dst_boot_epoch", "topology_digest", "captured_at_ms",
    "expires_at_ms", "sample_count", "rtt_ms", "first_byte_ms", "elapsed_ms",
    "payload_bytes", "wire_bytes", "encoded_bytes", "received_segments",
    "probe_profile_digest", "probe_segments_sent", "probe_complete",
    "probe_signature_valid", "probe_rtt_ms", "probe_interest_gaps_ms",
    "probe_data_arrival_gaps_ms", "probe_data_wire_bytes", "timeout_count",
    "nack_count", "goodput_mbps", "source_digest"};
  for (const auto& field : allowed) {
    if (!row.contains(field))
      throw std::invalid_argument("V4 link observation is missing a field");
  }
  const std::set<std::string> trainFields{
    "probe_source_send_gaps_ms", "probe_source_timing_validated",
    "probe_train_goodput_mbps", "probe_rate_status", "probe_rate_reason"};
  allowed.insert(trainFields.begin(), trainFields.end());
  requireKeys(row, allowed);
  const auto trainFieldCount = std::count_if(trainFields.begin(), trainFields.end(),
    [&] (const auto& field) { return row.contains(field); });
  if (trainFieldCount != 0 && trainFieldCount != 5)
    throw std::invalid_argument("V4 packet-train metadata is incomplete");

  const auto src = required<std::string>(row, "src_peer");
  const auto dst = required<std::string>(row, "dst_peer");
  const auto direction = required<std::string>(row, "direction_semantics");
  const auto kind = required<std::string>(row, "kind");
  const auto observer = required<std::string>(row, "observer_peer");
  const auto observerBoot = required<std::string>(row, "observer_boot_epoch");
  const auto captured = required<std::uint64_t>(row, "captured_at_ms");
  const auto expires = required<std::uint64_t>(row, "expires_at_ms");
  const auto samples = required<std::uint64_t>(row, "sample_count");
  const auto sourceDigest = required<std::string>(row, "source_digest");
  const bool dataKind = kind == "DATA_FETCH" || kind == "ACTIVE_PROBE_GOODPUT";
  const bool rttKind = kind == "ACK_RTT" || kind == "RESPONSE_RTT" ||
                       kind == "ACTIVE_PROBE_RTT";
  const auto expectedObserver = dataKind ? dst : src;
  if ((!dataKind && !rttKind) || !isCanonicalName(src) || !isCanonicalName(dst) ||
      src == dst || observer != provider || observer != expectedObserver ||
      observerBoot != bootEpoch || samples == 0 ||
      captured == 0 || captured > nowMs || expires <= captured || expires <= nowMs ||
      expires > expiresAtMs || nowMs - captured > maxAgeMs || !isDigest(sourceDigest) ||
      !std::binary_search(roster.begin(), roster.end(), src) ||
      !std::binary_search(roster.begin(), roster.end(), dst) ||
      (dataKind ? direction != "PAYLOAD_SOURCE_TO_RECEIVER"
                : direction != "REQUESTER_RESPONDER_ROUND_TRIP"))
    throw std::invalid_argument("V4 link observation identity or freshness is invalid");

  const auto& srcBoot = row.at("src_boot_epoch");
  const auto& dstBoot = row.at("dst_boot_epoch");
  const auto validBoot = [] (const NativeJson& value) {
    return value.is_null() || (value.is_string() && !value.get<std::string>().empty() &&
                               value.get<std::string>().size() <= 128);
  };
  if (!validBoot(srcBoot) || !validBoot(dstBoot) ||
      (dataKind && (!dstBoot.is_string() || dstBoot.get<std::string>() != bootEpoch)) ||
      (rttKind && (!srcBoot.is_string() || srcBoot.get<std::string>() != bootEpoch)) ||
      (kind == "ACTIVE_PROBE_GOODPUT" && !srcBoot.is_string()) ||
      (kind == "ACTIVE_PROBE_RTT" && !dstBoot.is_string()))
    throw std::invalid_argument("V4 observation endpoint boot binding is invalid");
  const auto& topology = row.at("topology_digest");
  if (!topology.is_null() &&
      (!topology.is_string() || !isDigest(topology.get<std::string>())))
    throw std::invalid_argument("V4 link topology digest is invalid");

  for (const auto* field : {"rtt_ms", "first_byte_ms", "elapsed_ms", "goodput_mbps"}) {
    if (!nullableNumberIsValid(row, field))
      throw std::invalid_argument("V4 link observation number is invalid");
  }
  for (const auto* field : {"payload_bytes", "wire_bytes", "encoded_bytes",
                            "received_segments", "probe_segments_sent",
                            "timeout_count", "nack_count"}) {
    if (!nullableUnsignedIsValid(row, field))
      throw std::invalid_argument("V4 link observation counter is invalid");
  }

  const auto& profile = row.at("probe_profile_digest");
  if (!profile.is_null() && (!profile.is_string() || !isDigest(profile.get<std::string>())))
    throw std::invalid_argument("V4 probe profile digest is invalid");
  for (const auto* field : {"probe_complete", "probe_signature_valid"}) {
    if (!row.at(field).is_null() && !row.at(field).is_boolean())
      throw std::invalid_argument("V4 probe status is invalid");
  }
  for (const auto* field : {"probe_rtt_ms", "probe_interest_gaps_ms",
                            "probe_data_arrival_gaps_ms", "probe_data_wire_bytes"}) {
    const auto& items = row.at(field);
    if (!items.is_null() && !items.is_array())
      throw std::invalid_argument("V4 probe signature array is invalid");
  }

  const auto& rtt = row.at("rtt_ms");
  if (rttKind) {
    if (!rtt.is_number() || rtt.get<double>() <= 0 ||
        !row.at("first_byte_ms").is_null() || !row.at("elapsed_ms").is_null() ||
        !row.at("payload_bytes").is_null() || !row.at("goodput_mbps").is_null())
      throw std::invalid_argument("V4 RTT observation carries invalid payload metrics");
  }
  else {
    const auto& firstByte = row.at("first_byte_ms");
    const auto& elapsed = row.at("elapsed_ms");
    const auto& bytes = row.at("payload_bytes");
    const auto& goodput = row.at("goodput_mbps");
    if (!firstByte.is_number() || !elapsed.is_number() || elapsed.get<double>() <= 0 ||
        !isUnsignedInteger(bytes) || bytes.get<std::uint64_t>() == 0 ||
        !goodput.is_number() || goodput.get<double>() <= 0)
      throw std::invalid_argument("V4 data observation lacks payload goodput evidence");
    const auto expected = static_cast<double>(bytes.get<std::uint64_t>()) * 8.0 /
                          (elapsed.get<double>() * 1000.0);
    if (!std::isfinite(expected) ||
        std::abs(expected - goodput.get<double>()) >
          std::max(1e-9, std::abs(expected) * 1e-6))
      throw std::invalid_argument("V4 goodput does not match payload and elapsed time");
  }

  if (kind == "ACTIVE_PROBE_GOODPUT") {
    if (!row.at("probe_complete").is_boolean() ||
        !row.at("probe_complete").get<bool>() ||
        !isUnsignedInteger(row.at("probe_segments_sent")) ||
        row.at("probe_segments_sent").get<std::uint64_t>() != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS ||
        !profile.is_string())
      throw std::invalid_argument("V4 goodput probe is incomplete");
    const auto& rtts = row.at("probe_rtt_ms");
    const auto& sends = row.at("probe_interest_gaps_ms");
    const auto& arrivals = row.at("probe_data_arrival_gaps_ms");
    const auto& bytes = row.at("probe_data_wire_bytes");
    constexpr auto segments = ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS;
    if (!rtts.is_array() || rtts.size() != segments || !sends.is_array() || sends.size() != segments - 1 ||
        !arrivals.is_array() || arrivals.size() != segments - 1 || !bytes.is_array() || bytes.size() != segments)
      throw std::invalid_argument("V4 goodput probe arrays have invalid lengths");
    for (std::size_t i = 0; i < segments; ++i) {
      if (!rtts[i].is_number() || !std::isfinite(rtts[i].get<double>()) ||
          rtts[i].get<double>() <= 0 || !isUnsignedInteger(bytes[i]) ||
          bytes[i].get<std::uint64_t>() == 0 || bytes[i].get<std::uint64_t>() > 6144)
        throw std::invalid_argument("V4 goodput probe packet row is invalid");
    }
    for (std::size_t i = 0; i < segments - 1; ++i) {
      if (!sends[i].is_number() || !std::isfinite(sends[i].get<double>()) ||
          sends[i].get<double>() <= 0 || !arrivals[i].is_number() ||
          !std::isfinite(arrivals[i].get<double>()) || arrivals[i].get<double>() < 0)
        throw std::invalid_argument("V4 goodput probe gap is invalid");
    }
    if (profile.get<std::string>() == ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST &&
        trainFieldCount != 5)
      throw std::invalid_argument("V4 burst profile lacks source timing metadata");
    if (trainFieldCount == 5) {
      const auto& sourceGaps = row.at("probe_source_send_gaps_ms");
      const auto& sourceValid = row.at("probe_source_timing_validated");
      if (!sourceGaps.is_array() || sourceGaps.size() != segments - 2 || !sourceValid.is_boolean())
        throw std::invalid_argument("V4 source timing shape is invalid");
      ndn_service_framework::NetworkProbeSourceGaps source{};
      ndn_service_framework::NetworkProbeArrivalGaps arrival{};
      for (std::size_t i = 0; i < source.size(); ++i) {
        if (!sourceGaps[i].is_number() || !std::isfinite(sourceGaps[i].get<double>()) ||
            sourceGaps[i].get<double>() < 0 || sourceGaps[i].get<double>() > 500)
          throw std::invalid_argument("V4 source timing is outside profile bounds");
        source[i] = sourceGaps[i].get<double>();
      }
      for (std::size_t i = 0; i < arrival.size(); ++i)
        arrival[i] = arrivals[i].get<double>();
      const bool trusted = sourceValid.get<bool>() &&
        row.at("probe_signature_valid").is_boolean() &&
        row.at("probe_signature_valid").get<bool>() &&
        profile.get<std::string>() == ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
      const auto estimate = ndn_service_framework::estimateWarmedNetworkProbeTrainRate(
        arrival, source, ndn_service_framework::NETWORK_PROBE_WARMED_PAYLOAD_BYTES, trusted);
      const bool measured = estimate.status == ndn_service_framework::NetworkProbeRateStatus::MeasuredGoodput;
      if (row.at("probe_rate_status") != (measured ? "MEASURED_GOODPUT" : "UNKNOWN") ||
          row.at("probe_rate_reason") != estimate.reason ||
          (!measured && !row.at("probe_train_goodput_mbps").is_null()) ||
          (measured && (!row.at("probe_train_goodput_mbps").is_number() ||
            !std::isfinite(row.at("probe_train_goodput_mbps").get<double>()) ||
            std::abs(row.at("probe_train_goodput_mbps").get<double>() - estimate.goodputMbps) >
              std::max(1e-9, estimate.goodputMbps * 1e-6))))
        throw std::invalid_argument("V4 packet-train rate disagrees with timing evidence");
    }
  }
  else if (!profile.is_null() || !row.at("probe_segments_sent").is_null() ||
           !row.at("probe_complete").is_null() ||
           !row.at("probe_signature_valid").is_null() ||
           !row.at("probe_rtt_ms").is_null() ||
           !row.at("probe_interest_gaps_ms").is_null() ||
           !row.at("probe_data_arrival_gaps_ms").is_null() ||
           !row.at("probe_data_wire_bytes").is_null()) {
    throw std::invalid_argument("V4 non-probe observation carries chirp fields");
  }
  if (kind != "ACTIVE_PROBE_GOODPUT" && trainFieldCount == 5) {
    for (const auto& field : trainFields) {
      if (!row.at(field).is_null())
        throw std::invalid_argument("V4 non-probe observation carries train metadata");
    }
  }

  auto source = row;
  source.erase("source_digest");
  if (nativePlanningDigest(nativeCanonicalJson(source)) != sourceDigest)
    throw std::invalid_argument("V4 source digest does not match normalized sample");
}

} // namespace

NativeJson nativeProbeTrainRateMetadataV4(
  const ndn_service_framework::NetworkTelemetrySnapshot& sample)
{
  using namespace ndn_service_framework;
  const bool active = sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput;
  const bool trusted = active && sample.probeComplete &&
    sample.probeSegmentsSent == NETWORK_PROBE_WARMED_SEGMENTS && sample.receivedSegments == NETWORK_PROBE_WARMED_SEGMENTS &&
    sample.targetSignatureValidated && sample.probeSignatureValid &&
    sample.probeProfileDigest == NETWORK_PROBE_TRAIN_PROFILE_DIGEST &&
    sample.probeSourceTimingValidated;
  const auto estimate = estimateWarmedNetworkProbeTrainRate(sample.probeDataArrivalGapsMs,
    sample.probeSourceSendGapsMs, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, trusted);
  const bool measured = estimate.status == NetworkProbeRateStatus::MeasuredGoodput;
  return NativeJson{
    {"probe_source_send_gaps_ms", active ?
      NativeJson(sample.probeSourceSendGapsMs) :
      NativeJson(nullptr)},
    {"probe_source_timing_validated", active ?
      NativeJson(sample.probeSourceTimingValidated) : NativeJson(nullptr)},
    {"probe_train_goodput_mbps", measured ? NativeJson(estimate.goodputMbps) : NativeJson(nullptr)},
    {"probe_rate_status", active ? NativeJson(measured ? "MEASURED_GOODPUT" : "UNKNOWN") : NativeJson(nullptr)},
    {"probe_rate_reason", active ? NativeJson(estimate.reason) : NativeJson(nullptr)}};
}

NativeProviderResidencySnapshotV4
combineNativeProviderResidencyOwnersV4(
  const std::vector<NativeProviderResidencyOwnerResultV4>& owners)
{
  constexpr std::size_t OWNER_COUNT = 5;
  if (owners.size() != OWNER_COUNT)
    return {};

  std::array<bool, OWNER_COUNT> seen{};
  struct SelectedRow
  {
    std::size_t ownerIndex = 0;
    int statePriority = 0;
    NativeJson row;
  };
  std::map<std::string, SelectedRow> selectedRows;
  for (const auto& owner : owners) {
    std::size_t index = OWNER_COUNT;
    switch (owner.owner) {
      case NativeProviderResidencyOwnerV4::ProviderArtifactCache:
        index = 0;
        break;
      case NativeProviderResidencyOwnerV4::RunnerReuseCache:
        index = 1;
        break;
      case NativeProviderResidencyOwnerV4::OnnxRuntimeSessionCache:
        index = 2;
        break;
      case NativeProviderResidencyOwnerV4::ProtectedPlaintextCache:
        index = 3;
        break;
      case NativeProviderResidencyOwnerV4::PersistentRecipeCache:
        index = 4;
        break;
    }
    if (index >= OWNER_COUNT || seen[index] || !owner.complete)
      return {};
    seen[index] = true;
    for (const auto& row : owner.rows) {
      if (!row.is_object() || !row.contains("state") || !row.at("state").is_string())
        return {};
      const auto state = row.at("state").get<std::string>();
      const int priority = state == "EXACT_LOADED" ? 4 :
        state == "ASSEMBLED" ? 3 :
        state == "CANONICAL_ONLY" ? 2 :
        state == "KNOWN_MISS" ? 1 : 0;
      if (priority == 0)
        return {};
      const auto key = residencyIdentityKey(row);
      const auto found = selectedRows.find(key);
      if (found == selectedRows.end() || priority > found->second.statePriority ||
          (priority == found->second.statePriority && index < found->second.ownerIndex) ||
          (priority == found->second.statePriority && index == found->second.ownerIndex &&
           nativeCanonicalJson(row) < nativeCanonicalJson(found->second.row))) {
        selectedRows[key] = SelectedRow{index, priority, row};
      }
    }
  }
  if (!std::all_of(seen.begin(), seen.end(), [] (bool value) { return value; }))
    return {};

  if (selectedRows.size() > 16)
    return {};
  NativeProviderResidencySnapshotV4 result;
  for (auto& item : selectedRows)
    result.rows.push_back(std::move(item.second.row));
  result.known = true;
  return result;
}

NativeProviderResidencySnapshotV4
queryNativeProviderResidencyOwnersV4(
  const std::vector<NativeJson>& candidateArtifacts,
  const std::string& processEpoch,
  const std::uint64_t capturedAtMs,
  const std::uint64_t expiresAtMs,
  const ProviderArtifactCache* artifactCache,
  const NativeProviderRunnerReuseCache* runnerCache,
  const OnnxRuntimeSessionCache* sessionCache,
  const NativeProtectedPlaintextCache* protectedPlaintextCache,
  const NativePersistentRecipeResidencyIndex* persistentRecipeIndex)
{
  std::vector<NativeProviderResidencyOwnerResultV4> owners;
  owners.reserve(5);
  if (artifactCache) {
    const auto query = artifactCache->queryPlacementResidencyV4(
      candidateArtifacts, processEpoch, capturedAtMs, expiresAtMs);
    owners.push_back({NativeProviderResidencyOwnerV4::ProviderArtifactCache,
                      query.complete, query.rows});
  }
  else {
    owners.push_back({NativeProviderResidencyOwnerV4::ProviderArtifactCache, false, {}});
  }
  owners.push_back(runnerCache
    ? runnerCache->queryPlacementResidencyV4(
        candidateArtifacts, processEpoch, capturedAtMs, expiresAtMs)
    : NativeProviderResidencyOwnerResultV4{
        NativeProviderResidencyOwnerV4::RunnerReuseCache, false, {}});
  owners.push_back(sessionCache
    ? sessionCache->queryPlacementResidencyV4(
        candidateArtifacts, processEpoch, capturedAtMs, expiresAtMs)
    : NativeProviderResidencyOwnerResultV4{
        NativeProviderResidencyOwnerV4::OnnxRuntimeSessionCache, false, {}});
  owners.push_back(protectedPlaintextCache
    ? protectedPlaintextCache->queryPlacementResidencyV4(
        candidateArtifacts, processEpoch, capturedAtMs, expiresAtMs)
    : NativeProviderResidencyOwnerResultV4{
        NativeProviderResidencyOwnerV4::ProtectedPlaintextCache, false, {}});
  owners.push_back(persistentRecipeIndex
    ? persistentRecipeIndex->queryPlacementResidencyV4(
        candidateArtifacts, processEpoch, capturedAtMs, expiresAtMs)
    : NativeProviderResidencyOwnerResultV4{
        NativeProviderResidencyOwnerV4::PersistentRecipeCache, false, {}});
  return combineNativeProviderResidencyOwnersV4(owners);
}

std::vector<NativeJson>
makeNativeProviderLinkObservationsV4(
  const std::vector<ndn_service_framework::NetworkTelemetrySnapshot>& observations,
  const std::string& localProvider,
  const std::string& bootEpoch,
  const std::string& topologyDigest,
  std::uint64_t nowMs,
  std::uint64_t expiresAtMs)
{
  if (!isCanonicalName(localProvider) || bootEpoch.empty() ||
      bootEpoch.size() > 128 || expiresAtMs <= nowMs)
    return {};

  const auto nullableString = [] (const std::string& value) -> NativeJson {
    return value.empty() ? NativeJson(nullptr) : NativeJson(value);
  };
  const auto asJson = [] (const auto& values) {
    NativeJson result = NativeJson::array();
    for (const auto value : values)
      result.push_back(value);
    return result;
  };

  std::vector<NativeJson> rows;
  for (const auto& observation : observations) {
    std::string kind;
    std::string source;
    std::string destination;
    std::string direction;
    bool isRtt = false;
    bool isActiveGoodput = false;
    if (observation.kind == ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt) {
      kind = "ACTIVE_PROBE_RTT";
      source = observation.providerName.toUri();
      destination = observation.peerName.toUri();
      direction = "REQUESTER_RESPONDER_ROUND_TRIP";
      isRtt = true;
    }
    else if (observation.kind == ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) {
      kind = "ACTIVE_PROBE_GOODPUT";
      source = observation.peerName.toUri();
      destination = observation.providerName.toUri();
      direction = "PAYLOAD_SOURCE_TO_RECEIVER";
      isActiveGoodput = true;
    }
    else if (observation.kind == ndn_service_framework::NetworkTelemetrySampleKind::LargeDataFetch) {
      kind = "DATA_FETCH";
      source = observation.peerName.toUri();
      destination = observation.providerName.toUri();
      direction = "PAYLOAD_SOURCE_TO_RECEIVER";
    }
    else {
      continue;
    }

    const auto capturedAtMs = observation.lastUpdatedMs;
    if (observation.stale || observation.providerName.toUri() != localProvider ||
        observation.peerName.empty() || observation.peerName == observation.providerName ||
        !isCanonicalName(source) || !isCanonicalName(destination) ||
        observation.sampleCount == 0 || capturedAtMs == 0 ||
        capturedAtMs > nowMs || capturedAtMs >= expiresAtMs ||
        nowMs - capturedAtMs > MAX_TELEMETRY_AGE_MS ||
        (isRtt && (!std::isfinite(observation.rttMs) || observation.rttMs <= 0.0)) ||
        (!isRtt && (!std::isfinite(observation.firstByteMs) ||
                    !std::isfinite(observation.elapsedMs) ||
                    observation.elapsedMs <= 0.0 || observation.payloadBytes == 0 ||
                    !std::isfinite(observation.goodputMbps) ||
                    observation.goodputMbps <= 0.0)) ||
        (isActiveGoodput && (!observation.probeComplete ||
                              observation.probeSegmentsSent != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS ||
                              observation.receivedSegments != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS ||
                             observation.probeProfileDigest.empty())))
      continue;

    const auto sourceBoot = isRtt ? bootEpoch : observation.peerBootEpoch;
    const auto destinationBoot = isRtt ? observation.peerBootEpoch : bootEpoch;
    if ((isRtt && destinationBoot.empty()) ||
        (isActiveGoodput && sourceBoot.empty()))
      continue;

    NativeJson row = {
      {"src_peer", source}, {"dst_peer", destination},
      {"direction_semantics", direction}, {"kind", kind},
      {"observer_peer", localProvider}, {"observer_boot_epoch", bootEpoch},
      {"src_boot_epoch", nullableString(sourceBoot)},
      {"dst_boot_epoch", nullableString(destinationBoot)},
      {"topology_digest", nullableString(topologyDigest)},
      {"captured_at_ms", capturedAtMs},
      {"expires_at_ms", expiresAtMs}, {"sample_count", observation.sampleCount},
      {"rtt_ms", isRtt ? NativeJson(observation.rttMs) : NativeJson(nullptr)},
      {"first_byte_ms", isRtt ? NativeJson(nullptr) : NativeJson(observation.firstByteMs)},
      {"elapsed_ms", isRtt ? NativeJson(nullptr) : NativeJson(observation.elapsedMs)},
      {"payload_bytes", isRtt ? NativeJson(nullptr) : NativeJson(observation.payloadBytes)},
      {"wire_bytes", observation.wireBytes}, {"encoded_bytes", observation.encodedBytes},
      {"received_segments", observation.receivedSegments},
      {"probe_profile_digest", isActiveGoodput
        ? NativeJson(observation.probeProfileDigest) : NativeJson(nullptr)},
      {"probe_segments_sent", isActiveGoodput
        ? NativeJson(observation.probeSegmentsSent) : NativeJson(nullptr)},
      {"probe_complete", isActiveGoodput
        ? NativeJson(observation.probeComplete) : NativeJson(nullptr)},
      {"probe_signature_valid", isActiveGoodput
        ? NativeJson(observation.probeSignatureValid) : NativeJson(nullptr)},
      {"probe_rtt_ms", isActiveGoodput
        ? asJson(observation.probeRttMs) : NativeJson(nullptr)},
      {"probe_interest_gaps_ms", isActiveGoodput
        ? asJson(observation.probeInterestGapsMs) : NativeJson(nullptr)},
      {"probe_data_arrival_gaps_ms", isActiveGoodput
        ? asJson(observation.probeDataArrivalGapsMs) : NativeJson(nullptr)},
      {"probe_data_wire_bytes", isActiveGoodput
        ? asJson(observation.probeDataWireBytes) : NativeJson(nullptr)},
      {"timeout_count", observation.timeoutCount}, {"nack_count", observation.nackCount},
      {"goodput_mbps", isRtt ? NativeJson(nullptr) : NativeJson(observation.goodputMbps)}};
    row.update(nativeProbeTrainRateMetadataV4(observation));
    row["source_digest"] = nativePlanningDigest(nativeCanonicalJson(row));
    rows.push_back(std::move(row));
  }
  return rows;
}

ndn_service_framework::ServiceProvider::AsyncAckStrategyHandler
makeNativeProviderObservationAckHandlerV4(
  ndn_service_framework::ServiceProvider::AckStrategyHandler baseAckHandler,
  NativeProviderOfferV4Config config,
  std::function<NativeProviderTelemetrySnapshot()> telemetrySnapshot,
  NativeProviderResidencyQueryV4 residencyQuery)
{
  using ServiceProvider = ndn_service_framework::ServiceProvider;
  return [baseAckHandler = std::move(baseAckHandler),
          config = std::move(config),
          telemetrySnapshot = std::move(telemetrySnapshot),
          residencyQuery = std::move(residencyQuery)](
      const ServiceProvider::AckRequestContext& context,
      const ndn_service_framework::RequestMessage& request,
      ServiceProvider::AsyncAckCompletion complete) mutable {
    auto decision = baseAckHandler ? baseAckHandler(request)
                                   : ServiceProvider::AckDecision{};
    if (!complete)
      return;
    if (!decision.status || !request.hasRequestCapabilities() ||
        !request.getRequestCapabilities().hasField("DI_PLACEMENT_V4") ||
        request.getRequestCapabilities().getField("DI_PLACEMENT_V4") != "optional") {
      complete(std::move(decision));
      return;
    }

    try {
      if (context.isCancelled() ||
          std::chrono::steady_clock::now() >= context.deadline ||
          context.requesterIdentity.empty() ||
          context.registrationGeneration == 0 ||
          context.serviceName.toUri() != config.service) {
        complete(std::move(decision));
        return;
      }
      const auto wirePayload = request.getPayload();
      const std::vector<std::uint8_t> payload(wirePayload.begin(), wirePayload.end());
      const auto envelope = nativeParseJson(
        std::string(payload.begin(), payload.end()));
      if (!envelope.is_object() ||
          envelope.value("service", std::string{}) != context.serviceName.toUri() ||
          envelope.value("request_id", std::string{}) != context.requestId.toUri() ||
          !envelope.contains("requester_identity") ||
          !envelope.at("requester_identity").is_string() ||
          !isCanonicalName(envelope.at("requester_identity").get<std::string>()) ||
          ndn::Name(envelope.at("requester_identity").get<std::string>()) !=
            context.requesterIdentity ||
          !envelope.contains("candidate_artifacts") ||
          !envelope.at("candidate_artifacts").is_array() ||
          envelope.at("candidate_artifacts").empty() ||
          envelope.at("candidate_artifacts").size() >
            MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS) {
        complete(std::move(decision));
        return;
      }

      std::vector<NativeJson> candidateArtifacts;
      candidateArtifacts.reserve(envelope.at("candidate_artifacts").size());
      for (const auto& item : envelope.at("candidate_artifacts")) {
        if (!item.is_object()) {
          complete(std::move(decision));
          return;
        }
        candidateArtifacts.push_back(item);
      }
      if (context.isCancelled() ||
          std::chrono::steady_clock::now() >= context.deadline) {
        complete(std::move(decision));
        return;
      }
      const auto telemetry = telemetrySnapshot();
      const auto epoch = std::chrono::system_clock::now().time_since_epoch();
      const auto nowMs = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0,
          std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count()));
      const auto offerExpiry = std::min(
        envelope.at("plan_deadline_ms").get<std::uint64_t>(),
        nowMs > std::numeric_limits<std::uint64_t>::max() - config.offerTtlMs
          ? std::numeric_limits<std::uint64_t>::max()
          : nowMs + config.offerTtlMs);
      NativeProviderResidencySnapshotV4 residency;
      if (residencyQuery) {
        try {
          residency = residencyQuery(candidateArtifacts, config.bootEpoch,
                                     nowMs, offerExpiry);
        }
        catch (const std::exception&) {
          residency = {};
        }
      }
      if (context.isCancelled() ||
          std::chrono::steady_clock::now() >= context.deadline) {
        complete(std::move(decision));
        return;
      }
      const auto v4 = issueNativeProviderOfferV4(
        payload, config, telemetry, nowMs, {}, residency);
      if (v4) {
        decision.providerCapabilityFields[NATIVE_PROVIDER_OFFER_V4_FIELD] =
          v4->capabilityFieldValue;
        for (const auto& fallback : v4->fallbackCapabilityFieldValues) {
          decision.providerCapabilityFieldFallbacks.push_back({
            {NATIVE_PROVIDER_OFFER_V4_FIELD, fallback}});
        }
        decision.providerCapabilityFieldFallbacks.emplace_back();
        if (v4->pendingStateTtlMs != 0) {
          decision.pendingStateTtlMs = decision.pendingStateTtlMs == 0
            ? v4->pendingStateTtlMs
            : std::min(decision.pendingStateTtlMs, v4->pendingStateTtlMs);
        }
      }
      NDN_LOG_TRACE("NDNSF_DI_PROVIDER_OFFER_V4"
                    << " provider=" << config.provider
                    << " requestId=" << context.requestId.toUri()
                    << " status=" << (v4 ? "ready" : "unavailable")
                    << " linkRows=0");
    }
    catch (const std::exception&) {
      // Optional local observations never change the base admission decision.
    }
    complete(std::move(decision));
  };
}

std::optional<NativeProviderOfferV4Decision>
issueNativeProviderOfferV4(const std::vector<std::uint8_t>& requestPayload,
                           const NativeProviderOfferV4Config& config,
                           const NativeProviderTelemetrySnapshot& telemetry,
                           std::uint64_t nowMs,
                           const std::vector<NativeJson>& linkObservations,
                           const NativeProviderResidencySnapshotV4& residency)
{
  if (!isCanonicalName(config.provider) || !isCanonicalName(config.service) ||
      config.bootEpoch.empty() || !isDigest(config.signerKeyId) || !config.signDigest ||
      config.offerTtlMs == 0 || config.telemetryMaxAgeMs == 0)
    throw std::invalid_argument("invalid native V4 Provider offer configuration");

  NativeJson request;
  try {
    request = nativeParseJson(std::string(requestPayload.begin(), requestPayload.end()));
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
  if (!request.is_object() ||
      request.value("schema", std::string{}) != "ndnsf-di-request-envelope-v2")
    return std::nullopt;

  const auto requestId = request.value("request_id", std::string{});
  const auto service = request.value("service", std::string{});
  const auto requesterIdentity = request.value("requester_identity", std::string{});
  const auto modelDigest = request.value("model_identity_hash", std::string{});
  const auto graphDigest = request.value("graph_digest", std::string{});
  const auto candidateSetDigest = request.value("candidate_set_digest", std::string{});
  const auto deadlineMs = request.value("plan_deadline_ms", std::uint64_t{0});
  const auto attempt = request.value("attempt", std::uint64_t{0});
  if (!isCanonicalName(requestId) || !isCanonicalName(requesterIdentity) ||
      service != config.service ||
      !isDigest(modelDigest) || !isDigest(graphDigest) ||
      !isDigest(candidateSetDigest) || attempt == 0 || deadlineMs <= nowMs)
    return std::nullopt;

  const auto rosterIt = request.find("candidate_providers");
  if (rosterIt == request.end() || !rosterIt->is_array() ||
      rosterIt->empty() || rosterIt->size() > 16)
    return std::nullopt;
  std::vector<std::string> roster;
  for (const auto& item : *rosterIt) {
    if (!item.is_string() || !isCanonicalName(item.get<std::string>()))
      return std::nullopt;
    roster.push_back(item.get<std::string>());
  }
  if (!std::is_sorted(roster.begin(), roster.end()) ||
      std::adjacent_find(roster.begin(), roster.end()) != roster.end() ||
      std::find(roster.begin(), roster.end(), config.provider) == roster.end() ||
      nativePlanningDigest(nativeCanonicalJson(NativeJson(roster))) != candidateSetDigest)
    return std::nullopt;

  const auto rttPairsIt = request.find("rtt_pairs");
  const auto goodputEdgesIt = request.find("goodput_edges");
  if (rttPairsIt == request.end() || goodputEdgesIt == request.end() ||
      !request.contains("probe_scope_digest"))
    return std::nullopt;
  const auto rttPairs = *rttPairsIt;
  const auto goodputEdges = *goodputEdgesIt;
  std::vector<ProviderPair> allowedRttPairs;
  std::vector<ProviderPair> allowedGoodputEdges;
  try {
    allowedRttPairs = parseProviderPairs(rttPairs, roster);
    allowedGoodputEdges = parseProviderPairs(goodputEdges, roster);
  }
  catch (const std::exception&) {
    return std::nullopt;
  }

  const bool hasCandidateArtifacts = request.contains("candidate_artifacts");
  NativeJson candidateArtifacts = hasCandidateArtifacts
    ? request.at("candidate_artifacts") : NativeJson::array();
  if (!candidateArtifacts.is_array() ||
      candidateArtifacts.empty() ||
      candidateArtifacts.size() > MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS ||
      (residency.known && !hasCandidateArtifacts) ||
      (!residency.known && !residency.rows.empty()))
    return std::nullopt;
  std::string previousCandidate;
  for (auto& item : candidateArtifacts) {
    try {
      item = candidateArtifactIdentity(item);
    }
    catch (const std::exception&) {
      return std::nullopt;
    }
    const auto canonical = nativeCanonicalJson(item);
    if ((!previousCandidate.empty() && previousCandidate >= canonical))
      return std::nullopt;
    previousCandidate = canonical;
  }
  const auto candidateArtifactSetDigest =
    nativePlanningDigest(nativeCanonicalJson(candidateArtifacts));
  if (request.value("candidate_artifact_set_digest", std::string{}) !=
        candidateArtifactSetDigest ||
      !isDigest(request.value("probe_scope_digest", std::string{})) ||
      request.at("probe_scope_digest").get<std::string>() !=
        computeProbeScopeDigest(request, candidateArtifactSetDigest,
                                rttPairs, goodputEdges))
    return std::nullopt;

  const auto expiresAtMs = std::min(deadlineMs,
    nowMs > std::numeric_limits<std::uint64_t>::max() - config.offerTtlMs
      ? std::numeric_limits<std::uint64_t>::max() : nowMs + config.offerTtlMs);
  if (expiresAtMs <= nowMs)
    return std::nullopt;

  std::vector<NativeJson> rows;
  for (const auto& row : linkObservations) {
    if (!row.is_object() || !row.contains("kind") ||
        !row.at("kind").is_string() || !row.contains("src_peer") ||
        !row.at("src_peer").is_string() || !row.contains("dst_peer") ||
        !row.at("dst_peer").is_string())
      continue;
    const auto kind = row.at("kind").get<std::string>();
    const auto source = row.at("src_peer").get<std::string>();
    const auto destination = row.at("dst_peer").get<std::string>();
    const bool isAuthorized = kind == "ACTIVE_PROBE_RTT"
      ? containsPair(allowedRttPairs, source, destination)
      : (kind == "ACTIVE_PROBE_GOODPUT" || kind == "DATA_FETCH")
          ? containsPair(allowedGoodputEdges, source, destination)
          : false;
    if (isAuthorized)
      rows.push_back(row);
  }
  std::set<std::tuple<std::string, std::string, std::string>> edgeKinds;
  for (const auto& row : rows) {
    validateLinkObservation(row, config.provider, config.bootEpoch, roster, nowMs,
                            expiresAtMs, config.telemetryMaxAgeMs);
    const auto edgeKind = std::make_tuple(
      row.at("kind").get<std::string>(), row.at("src_peer").get<std::string>(),
      row.at("dst_peer").get<std::string>());
    if (!edgeKinds.insert(edgeKind).second)
      throw std::invalid_argument("V4 offer contains a duplicate directed observation key");
  }
  std::sort(rows.begin(), rows.end(), [] (const NativeJson& left, const NativeJson& right) {
    const auto leftKind = left.at("kind").get<std::string>();
    const auto rightKind = right.at("kind").get<std::string>();
    if (linkKindPriority(leftKind) != linkKindPriority(rightKind))
      return linkKindPriority(leftKind) < linkKindPriority(rightKind);
    const auto leftAt = left.at("captured_at_ms").get<std::uint64_t>();
    const auto rightAt = right.at("captured_at_ms").get<std::uint64_t>();
    if (leftAt != rightAt)
      return leftAt > rightAt;
    return std::tie(left.at("src_peer"), left.at("dst_peer")) <
           std::tie(right.at("src_peer"), right.at("dst_peer"));
  });
  bool observationsTruncated = rows.size() > MAX_V4_LINK_ROWS;
  if (rows.size() > MAX_V4_LINK_ROWS)
    rows.resize(MAX_V4_LINK_ROWS);

  std::vector<NativeJson> residencyRows = residency.rows;
  if (residencyRows.size() > 16)
    throw std::invalid_argument("V4 residency snapshot exceeds row limit");
  for (const auto& row : residencyRows)
    validateResidencyObservation(row, candidateArtifacts, config.bootEpoch,
                                 nowMs, expiresAtMs, config.telemetryMaxAgeMs);
  std::sort(residencyRows.begin(), residencyRows.end(), [] (
      const NativeJson& left, const NativeJson& right) {
    return residencyIdentityKey(left) < residencyIdentityKey(right);
  });
  std::string previousResidencyKey;
  for (std::size_t i = 1; i < residencyRows.size(); ++i) {
    previousResidencyKey = residencyIdentityKey(residencyRows[i - 1]);
    if (previousResidencyKey == residencyIdentityKey(residencyRows[i]))
      throw std::invalid_argument("V4 offer contains a duplicate residency candidate");
  }
  const auto residencyState = !residency.known ? "UNKNOWN" :
    residencyRows.empty() ? "KNOWN_EMPTY" : "OBSERVED";

  const bool fresh = isFreshTelemetry(telemetry, config, nowMs);
  const auto& resources = telemetry.resources;
  const bool haveResources = fresh && resources.isFresh(
    static_cast<std::int64_t>(nowMs), static_cast<std::int64_t>(config.telemetryMaxAgeMs)) &&
    resources.matchesIdentity(config.provider, config.bootEpoch);
  const bool haveCapacity = fresh &&
    telemetry.capacity.activeWorkerCount <= telemetry.capacity.workerCount &&
    telemetry.capacity.readyQueueDepth <= telemetry.capacity.readyQueueCapacity;
  const auto queueDepth = [&] {
    const auto ready = static_cast<std::uint64_t>(telemetry.capacity.readyQueueDepth);
    const auto waiting = static_cast<std::uint64_t>(
      telemetry.capacity.dependencyWaitQueuedCount);
    return waiting > std::numeric_limits<std::uint64_t>::max() - ready
      ? std::numeric_limits<std::uint64_t>::max() : ready + waiting;
  }();

  NativeJson offer = {
    {"schema", "DI_PLACEMENT_V4"},
    {"schema_version", 4},
    {"request_id", requestId},
    {"attempt", attempt},
    {"service", service},
    {"provider", config.provider},
    {"model_digest", modelDigest},
    {"graph_digest", graphDigest},
    {"candidate_set_digest", candidateSetDigest},
    {"candidate_artifact_set_digest", candidateArtifactSetDigest},
    {"probe_scope_digest", request.at("probe_scope_digest")},
    {"status", true},
    {"boot_epoch", config.bootEpoch},
    {"captured_at_ms", nowMs},
    {"expires_at_ms", expiresAtMs},
    {"resources_state", haveResources ? "OBSERVED" : "UNKNOWN"},
    {"resources", haveResources ? resourceValue(resources) : NativeJson(nullptr)},
    {"capacity_state", haveCapacity ? "OBSERVED" : "UNKNOWN"},
    {"capacity", haveCapacity ? capacityValue(telemetry.capacity) : NativeJson(nullptr)},
    {"queue_depth", haveCapacity ? NativeJson(queueDepth) : NativeJson(nullptr)},
    {"estimated_wait_ms", nullptr},
    {"residency_state", residencyState},
    {"residency", residencyState == std::string("UNKNOWN") ?
      NativeJson(nullptr) : NativeJson(residencyRows)},
    {"observations_truncated", observationsTruncated},
    {"link_observations", rows},
    {"signer_key_id", config.signerKeyId}};

  if (haveResources)
    validateResourceValue(offer.at("resources"));
  if (haveCapacity)
    validateCapacityValue(offer.at("capacity"));

  std::vector<std::string> wires;
  bool linkRowsTruncated = observationsTruncated;
  std::string activeResidencyState = residencyState;
  while (true) {
    auto candidate = offer;
    candidate["link_observations"] = rows;
    candidate["residency_state"] = activeResidencyState;
    candidate["residency"] = activeResidencyState == "UNKNOWN"
      ? NativeJson(nullptr) : NativeJson(residencyRows);
    candidate["observations_truncated"] = linkRowsTruncated;
    const auto digest = nativePlanningDigest(nativeCanonicalJson(candidate));
    const auto signature = config.signDigest(digest);
    if (signature.empty())
      throw std::runtime_error("native V4 Provider offer signer returned no signature");
    candidate["signature"] = signature;
    const auto wire = nativeCanonicalJson(candidate);
    if (wire.size() <= MAX_V4_OFFER_BYTES)
      wires.push_back(wire);
    if (!rows.empty()) {
      rows.pop_back();
      linkRowsTruncated = true;
      continue;
    }
    if (!residencyRows.empty()) {
      residencyRows.pop_back();
      linkRowsTruncated = true;
      activeResidencyState = residencyRows.empty() ? "UNKNOWN" : "OBSERVED";
      continue;
    }
    break;
  }
  if (wires.empty())
    return std::nullopt;

  NativeProviderOfferV4Decision result;
  result.capabilityFieldValue = std::move(wires.front());
  if (wires.size() > 1)
    result.fallbackCapabilityFieldValues.assign(
      std::make_move_iterator(wires.begin() + 1), std::make_move_iterator(wires.end()));
  result.pendingStateTtlMs = expiresAtMs - nowMs;
  return result;
}

NativeObservedProviderOfferV4
decodeNativeProviderOfferV4(const std::string& wire)
{
  if (wire.empty() || wire.size() > MAX_V4_OFFER_BYTES)
    throw std::invalid_argument("V4 Provider offer exceeds wire limit");
  auto value = nativeParseJson(wire);
  if (!value.is_object() || nativeCanonicalJson(value) != wire)
    throw std::invalid_argument("V4 Provider offer is not canonical JSON");
  requireKeys(value, {"schema", "schema_version", "request_id", "attempt", "service",
    "provider", "model_digest", "graph_digest", "candidate_set_digest",
    "candidate_artifact_set_digest", "probe_scope_digest", "status",
    "boot_epoch", "captured_at_ms", "expires_at_ms", "resources_state", "resources",
    "capacity_state", "capacity", "queue_depth", "estimated_wait_ms", "residency_state",
    "residency", "observations_truncated", "link_observations", "signer_key_id", "signature"});
  if (required<std::string>(value, "schema") != "DI_PLACEMENT_V4" ||
      required<std::uint64_t>(value, "schema_version") != 4 ||
      !required<bool>(value, "status"))
    throw std::invalid_argument("V4 Provider offer schema/status mismatch");

  NativeObservedProviderOfferV4 result;
  result.requestId = required<std::string>(value, "request_id");
  result.attempt = required<std::uint64_t>(value, "attempt");
  result.service = required<std::string>(value, "service");
  result.provider = required<std::string>(value, "provider");
  result.modelDigest = required<std::string>(value, "model_digest");
  result.graphDigest = required<std::string>(value, "graph_digest");
  result.candidateSetDigest = required<std::string>(value, "candidate_set_digest");
  result.candidateArtifactSetDigest = required<std::string>(value,
                                                           "candidate_artifact_set_digest");
  result.probeScopeDigest = required<std::string>(value, "probe_scope_digest");
  result.bootEpoch = required<std::string>(value, "boot_epoch");
  result.capturedAtMs = required<std::uint64_t>(value, "captured_at_ms");
  result.expiresAtMs = required<std::uint64_t>(value, "expires_at_ms");
  result.signerKeyId = required<std::string>(value, "signer_key_id");
  result.signature = required<std::string>(value, "signature");
  if (!isCanonicalName(result.requestId) || !isCanonicalName(result.service) ||
      !isCanonicalName(result.provider) || result.attempt == 0 ||
      !isDigest(result.modelDigest) || !isDigest(result.graphDigest) ||
      !isDigest(result.candidateSetDigest) ||
      !isDigest(result.candidateArtifactSetDigest) ||
      !isDigest(result.probeScopeDigest) || result.bootEpoch.empty() ||
      !isDigest(result.signerKeyId) || result.signature.empty() ||
      result.capturedAtMs == 0 || result.expiresAtMs <= result.capturedAtMs)
    throw std::invalid_argument("V4 Provider offer identity or expiry is invalid");

  const auto resourcesState = required<std::string>(value, "resources_state");
  const auto capacityState = required<std::string>(value, "capacity_state");
  const auto residencyState = required<std::string>(value, "residency_state");
  if (resourcesState == "OBSERVED") {
    validateResourceValue(value.at("resources"));
  }
  else if (resourcesState != "UNKNOWN" || !value.at("resources").is_null()) {
    throw std::invalid_argument("V4 resource state/value mismatch");
  }
  if (capacityState == "OBSERVED") {
    validateCapacityValue(value.at("capacity"));
  }
  else if (capacityState != "UNKNOWN" || !value.at("capacity").is_null()) {
    throw std::invalid_argument("V4 capacity state/value mismatch");
  }
  const auto& queueDepth = value.at("queue_depth");
  if (!queueDepth.is_null() &&
      (!queueDepth.is_number_unsigned() || capacityState != "OBSERVED"))
    throw std::invalid_argument("V4 queue depth state/value mismatch");
  const auto& wait = value.at("estimated_wait_ms");
  if (!wait.is_null() && (!wait.is_number() || !std::isfinite(wait.get<double>()) ||
                          wait.get<double>() < 0))
    throw std::invalid_argument("V4 estimated wait is invalid");
  const auto& residency = value.at("residency");
  if (residencyState == "UNKNOWN") {
    if (!residency.is_null())
      throw std::invalid_argument("V4 unknown residency must be null");
  }
  else if (residencyState == "KNOWN_EMPTY") {
    if (!residency.is_array() || !residency.empty())
      throw std::invalid_argument("V4 known-empty residency must be an empty array");
  }
  else if (residencyState == "OBSERVED") {
    if (!residency.is_array() || residency.empty() || residency.size() > 16)
      throw std::invalid_argument("V4 observed residency row set is invalid");
    NativeJson candidateRows = NativeJson::array();
    std::set<std::string> residencyKeys;
    std::string previousResidencyKey;
    for (const auto& row : residency) {
      const auto identity = residencyCandidateIdentity(row);
      candidateRows.push_back(residencyRequestIdentity(identity));
      const auto identityKey = residencyIdentityKey(row);
      if (!residencyKeys.insert(identityKey).second)
        throw std::invalid_argument("V4 offer contains a duplicate residency candidate");
      if (!previousResidencyKey.empty() && previousResidencyKey >= identityKey)
        throw std::invalid_argument("V4 residency rows are not in canonical order");
      previousResidencyKey = identityKey;
      validateResidencyObservation(row, candidateRows, result.bootEpoch,
                                   result.capturedAtMs, result.expiresAtMs,
                                   MAX_TELEMETRY_AGE_MS);
    }
  }
  else {
    throw std::invalid_argument("V4 residency state is invalid");
  }
  (void)required<bool>(value, "observations_truncated");
  if (!value.at("link_observations").is_array() ||
      value.at("link_observations").size() > MAX_V4_LINK_ROWS)
    throw std::invalid_argument("V4 link observation envelope is invalid");
  std::set<std::tuple<std::string, std::string, std::string>> edgeKinds;
  const NativeJson* previous = nullptr;
  for (const auto& row : value.at("link_observations")) {
    const auto src = row.value("src_peer", std::string{});
    const auto dst = row.value("dst_peer", std::string{});
    std::vector<std::string> rowRoster{src, dst};
    std::sort(rowRoster.begin(), rowRoster.end());
    rowRoster.erase(std::unique(rowRoster.begin(), rowRoster.end()), rowRoster.end());
    validateLinkObservation(row, result.provider, result.bootEpoch, rowRoster,
                            result.capturedAtMs, result.expiresAtMs,
                            MAX_TELEMETRY_AGE_MS);
    const auto kind = row.at("kind").get<std::string>();
    if (!edgeKinds.emplace(kind, src, dst).second)
      throw std::invalid_argument("V4 offer contains a duplicate directed observation key");
    if (previous != nullptr) {
      const auto previousKind = previous->at("kind").get<std::string>();
      const auto previousAt = previous->at("captured_at_ms").get<std::uint64_t>();
      const auto captured = row.at("captured_at_ms").get<std::uint64_t>();
      const auto previousSrc = previous->at("src_peer").get<std::string>();
      const auto previousDst = previous->at("dst_peer").get<std::string>();
      if (linkKindPriority(previousKind) > linkKindPriority(kind) ||
          (linkKindPriority(previousKind) == linkKindPriority(kind) &&
           (previousAt < captured ||
            (previousAt == captured &&
             std::tie(previousSrc, previousDst) > std::tie(src, dst)))))
        throw std::invalid_argument("V4 observations are not in deterministic priority order");
    }
    previous = &row;
  }

  value.erase("signature");
  result.offerDigest = nativePlanningDigest(nativeCanonicalJson(value));
  value["signature"] = result.signature;
  result.values = std::move(value);
  return result;
}

} // namespace ndnsf::di
