#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace ndnsf::di {
namespace {

void require(bool condition, const char* reason)
{
  if (!condition) throw std::invalid_argument(reason);
}

bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void keys(const NativeJson& value, const std::set<std::string>& allowed)
{
  require(value.is_object(), "offer contract requires an object");
  for (auto it = value.begin(); it != value.end(); ++it) {
    require(allowed.count(it.key()) != 0, "unknown offer contract field");
  }
}

template<typename T>
T field(NativeJson& value, const char* key, T fallback = {})
{
  if (!value.contains(key)) value[key] = fallback;
  const auto& item = value.at(key);
  if constexpr (std::is_same_v<T, std::uint64_t>) {
    require(item.is_number_integer() && (item.is_number_unsigned() || item.get<std::int64_t>() >= 0),
            "offer integer must be nonnegative uint64");
  }
  else if constexpr (std::is_same_v<T, double>) {
    require(item.is_number() && std::isfinite(item.get<double>()) && item.get<double>() >= 0,
            "offer cost must be a finite nonnegative number");
  }
  // Dataclass defaults are materialized, but supplied integer costs stay integers
  // in the signed identity even though the native observation uses double.
  return item.get<T>();
}

void required(const NativeJson& value, std::initializer_list<const char*> names)
{
  for (const auto* name : names) require(value.contains(name), "missing required offer field");
}

NativeDeviceResourceObservationV3 resource(NativeJson& value)
{
  keys(value, {"device", "total_memory_mb", "free_memory_mb", "active_requests",
               "resource_sequence", "captured_at_ms", "topology_digest"});
  NativeDeviceResourceObservationV3 result;
  required(value, {"device", "total_memory_mb", "free_memory_mb"});
  result.device = field<std::string>(value, "device");
  result.totalMemoryMb = field<std::uint64_t>(value, "total_memory_mb");
  result.freeMemoryMb = field<std::uint64_t>(value, "free_memory_mb");
  result.activeRequests = field<std::uint64_t>(value, "active_requests");
  result.resourceSequence = field<std::uint64_t>(value, "resource_sequence", 1);
  result.capturedAtMs = field<std::uint64_t>(value, "captured_at_ms", 1);
  result.topologyDigest = field<std::string>(value, "topology_digest");
  require(!result.device.empty() && result.freeMemoryMb <= result.totalMemoryMb &&
          result.resourceSequence && result.capturedAtMs, "invalid device resource observation");
  return result;
}

NativeResidencyObservationV3 residency(NativeJson& value)
{
  keys(value, {"artifact_digest", "role", "rank", "tier", "device_set", "boot_epoch",
    "process_epoch", "topology_digest", "captured_at_ms", "expires_at_ms", "proof_digest",
    "residency_class", "identity_digest", "assembly_spec_digest", "model_manifest_digest",
    "artifact_profile_digest", "graph_digest", "backend", "protection_epoch", "runtime_generation",
    "fencing_token", "missing_verified_bytes", "estimated_assembly_ms", "estimated_load_ms"});
  NativeResidencyObservationV3 result;
  required(value, {"artifact_digest", "role", "rank", "tier"});
#define TEXT(member, key) result.member = field<std::string>(value, key)
#define UINT(member, key) result.member = field<std::uint64_t>(value, key)
  TEXT(artifactDigest, "artifact_digest"); TEXT(role, "role"); UINT(rank, "rank"); TEXT(tier, "tier");
  result.deviceSet = field<std::vector<std::string>>(value, "device_set");
  TEXT(bootEpoch, "boot_epoch"); TEXT(processEpoch, "process_epoch"); TEXT(topologyDigest, "topology_digest");
  result.capturedAtMs = field<std::uint64_t>(value, "captured_at_ms", 1);
  result.expiresAtMs = field<std::uint64_t>(value, "expires_at_ms", 2);
  TEXT(proofDigest, "proof_digest");
  result.residencyClass = field<std::string>(value, "residency_class", "CANONICAL");
  TEXT(identityDigest, "identity_digest"); TEXT(assemblySpecDigest, "assembly_spec_digest");
  TEXT(modelManifestDigest, "model_manifest_digest"); TEXT(artifactProfileDigest, "artifact_profile_digest");
  TEXT(graphDigest, "graph_digest"); TEXT(backend, "backend"); TEXT(protectionEpoch, "protection_epoch");
  UINT(runtimeGeneration, "runtime_generation"); TEXT(fencingToken, "fencing_token");
  UINT(missingVerifiedBytes, "missing_verified_bytes");
  result.estimatedAssemblyMs = field<double>(value, "estimated_assembly_ms");
  result.estimatedLoadMs = field<double>(value, "estimated_load_ms");
#undef TEXT
#undef UINT
  require(digest(result.artifactDigest) && !result.role.empty() && !result.bootEpoch.empty() &&
          !result.processEpoch.empty() && !result.topologyDigest.empty() && result.capturedAtMs &&
          result.expiresAtMs > result.capturedAtMs, "invalid residency observation identity");
  require(std::set<std::string>{"GPU", "RAM", "DISK", "CANONICAL"}.count(result.tier) != 0 &&
          std::set<std::string>{"CANONICAL", "ASSEMBLED_FRAGMENT", "LOADED_RUNTIME"}.count(result.residencyClass) != 0,
          "invalid residency observation class or tier");
  for (const auto* value : {&result.proofDigest, &result.identityDigest, &result.assemblySpecDigest,
                          &result.modelManifestDigest, &result.artifactProfileDigest, &result.graphDigest}) {
    require(value->empty() || digest(*value), "invalid residency observation digest");
  }
  return result;
}

} // namespace

NativeObservedProviderOfferV3 decodeNativeProviderOfferV3(const std::string& wire)
{
  require(wire.size() <= 1024 * 1024, "Provider offer exceeds V3 wire limit");
  try {
    auto value = nativeParseJson(wire);
    require(nativeCanonicalJson(value) == wire, "Provider offer is not canonically encoded");
    keys(value, {"schema", "schema_version", "request_id", "attempt", "service", "provider",
      "model_digest", "graph_digest", "status", "execution_disposition", "preparation_accepted",
      "topology", "resources", "residency", "accepted_roles", "backends", "queue_depth",
      "estimated_wait_ms", "rtt_ms", "bandwidth_mbps", "boot_epoch", "captured_at_ms",
      "expires_at_ms", "signer_key_id", "signature", "ack_reservation", "can_provision", "has_model"});
    require(field<std::string>(value, "schema") == "DI_PLACEMENT_V3" &&
            field<std::uint64_t>(value, "schema_version") == 3, "Provider offer schema mismatch");
    NativeObservedProviderOfferV3 result;
    required(value, {"request_id", "attempt", "service", "provider", "model_digest", "graph_digest",
                     "status", "execution_disposition", "preparation_accepted", "topology"});
    result.requestId = field<std::string>(value, "request_id");
    result.attempt = field<std::uint64_t>(value, "attempt");
    result.service = field<std::string>(value, "service");
    result.provider = field<std::string>(value, "provider");
    result.modelDigest = field<std::string>(value, "model_digest");
    result.graphDigest = field<std::string>(value, "graph_digest");
    result.status = field<bool>(value, "status");
    result.executionDisposition = field<std::string>(value, "execution_disposition");
    result.preparationAccepted = field<bool>(value, "preparation_accepted");
    result.acceptedRoles = field<std::vector<std::string>>(value, "accepted_roles");
    result.backends = field<std::vector<std::string>>(value, "backends");
    result.queueDepth = field<std::uint64_t>(value, "queue_depth");
    result.estimatedWaitMs = field<double>(value, "estimated_wait_ms");
    result.rttMs = field<double>(value, "rtt_ms");
    result.bandwidthMbps = field<double>(value, "bandwidth_mbps");
    result.bootEpoch = field<std::string>(value, "boot_epoch");
    result.capturedAtMs = field<std::uint64_t>(value, "captured_at_ms", 1);
    result.expiresAtMs = field<std::uint64_t>(value, "expires_at_ms", 2);
    result.signerKeyId = field<std::string>(value, "signer_key_id");
    result.signature = field<std::string>(value, "signature");
    result.canProvision = field<bool>(value, "can_provision");
    result.hasModel = field<bool>(value, "has_model");
    require(!field<bool>(value, "ack_reservation"), "V3 offer cannot reserve a lease");
    require(!result.requestId.empty() && result.attempt && !result.service.empty() &&
            !result.provider.empty() && digest(result.modelDigest) && digest(result.graphDigest) &&
            !result.bootEpoch.empty() && !result.signerKeyId.empty() && !result.signature.empty() &&
            result.capturedAtMs && result.expiresAtMs > result.capturedAtMs, "incomplete Provider offer");
    require((result.status && result.preparationAccepted && result.executionDisposition == "ACCEPT_WITH_PREPARATION") ||
            (result.status && !result.preparationAccepted && result.executionDisposition == "ACCEPT_IF_EXACT_REUSE") ||
            (!result.status && !result.preparationAccepted && result.executionDisposition == "REJECT"),
            "invalid V3 ACK disposition tuple");
    auto& topology = value.at("topology");
    keys(topology, {"provider", "devices", "backend", "topology_digest"});
    require(field<std::string>(topology, "provider") == result.provider, "topology Provider mismatch");
    result.devices = field<std::vector<std::string>>(topology, "devices");
    result.topologyBackend = field<std::string>(topology, "backend", "cpu");
    result.declaredTopologyDigest = field<std::string>(topology, "topology_digest");
    const std::set<std::string> devices(result.devices.begin(), result.devices.end());
    require(!result.topologyBackend.empty() && devices.size() == result.devices.size(), "invalid topology");
    for (const auto& device : devices) require(device.rfind("cuda:", 0) == 0, "invalid V3 device identity");
    result.topologyDigest = nativePlanningDigest(nativeCanonicalJson(topology));
    if (!value.contains("resources")) value["resources"] = NativeJson::array();
    if (!value.contains("residency")) value["residency"] = NativeJson::array();
    require(value.at("resources").is_array() && value.at("residency").is_array(), "invalid observation arrays");
    std::set<std::string> resourceDevices;
    for (auto& item : value["resources"]) {
      auto observation = resource(item);
      require(devices.count(observation.device) && resourceDevices.insert(observation.device).second,
              "resource observation outside topology or duplicate device");
      result.resources.push_back(std::move(observation));
    }
    for (auto& item : value["residency"]) result.residency.push_back(residency(item));
    value.erase("signature");
    result.offerDigest = nativePlanningDigest(nativeCanonicalJson(value));
    return result;
  }
  catch (const NativeJson::exception& error) {
    throw std::invalid_argument(std::string("invalid V3 Provider offer: ") + error.what());
  }
}

} // namespace ndnsf::di
