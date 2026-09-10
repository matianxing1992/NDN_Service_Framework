#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {

constexpr const char* REQUEST_SCHEMA = "ndnsf-di-request-envelope-v2";
constexpr const char* PLACEMENT_PROFILE = "DI_PLACEMENT_V3";
constexpr const char* UNBOUND_GRAPH_DIGEST =
  "sha256:0000000000000000000000000000000000000000000000000000000000000000";

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
         std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
           return std::isxdigit(ch) != 0;
         });
}

std::string
escapeJson(const std::string& value)
{
  std::ostringstream output;
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (byte < 0x20) {
          output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(byte) << std::dec;
        }
        else {
          output << ch;
        }
    }
  }
  return output.str();
}

std::string
quote(const std::string& value)
{
  return "\"" + escapeJson(value) + "\"";
}

std::string
stringArray(const std::vector<std::string>& values)
{
  std::ostringstream output;
  output << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      output << ',';
    }
    output << quote(values[i]);
  }
  output << ']';
  return output.str();
}

std::string
resourceArray(const std::vector<NativeProviderOfferV3Resource>& resources)
{
  std::ostringstream output;
  output << '[';
  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (i != 0) {
      output << ',';
    }
    const auto& resource = resources[i];
    output << "{\"active_requests\":" << resource.activeRequests
           << ",\"captured_at_ms\":" << resource.capturedAtMs
           << ",\"device\":" << quote(resource.device)
           << ",\"free_memory_mb\":" << resource.freeMemoryMb
           << ",\"resource_sequence\":" << resource.resourceSequence
           << ",\"topology_digest\":" << quote(resource.topologyDigest)
           << ",\"total_memory_mb\":" << resource.totalMemoryMb << '}';
  }
  output << ']';
  return output.str();
}

std::string
sha256Digest(const std::string& value)
{
  std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes.data());
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

void
validateConfig(const NativeProviderOfferV3Config& config)
{
  if (config.provider.empty() || config.service.empty() || config.bootEpoch.empty() ||
      !isDigest(config.signerKeyId) || config.acceptedRoles.empty() ||
      config.backends.empty() || !config.signDigest) {
    throw std::invalid_argument("invalid native V3 Provider offer configuration");
  }
  for (const auto* values : {&config.acceptedRoles, &config.backends, &config.devices}) {
    if (std::set<std::string>(values->begin(), values->end()).size() != values->size() ||
        std::any_of(values->begin(), values->end(), [] (const std::string& value) {
          return value.empty();
        })) {
      throw std::invalid_argument("native V3 Provider offer contains invalid list values");
    }
  }
  std::set<std::string> seenResources;
  for (const auto& resource : config.resources) {
    if (resource.device.empty() || !seenResources.insert(resource.device).second ||
        std::find(config.devices.begin(), config.devices.end(), resource.device) ==
          config.devices.end() || resource.totalMemoryMb == 0 ||
        resource.freeMemoryMb > resource.totalMemoryMb ||
        resource.resourceSequence == 0 || resource.capturedAtMs == 0) {
      throw std::invalid_argument("native V3 Provider offer contains invalid resources");
    }
  }
}

NativeProviderOfferV3Decision
reject(const std::string& reason)
{
  NativeProviderOfferV3Decision result;
  result.message = reason;
  return result;
}

std::string
canonicalOffer(const NativeProviderOfferV3Config& config,
               const std::vector<NativeProviderOfferV3Resource>& resources,
               const std::string& requestId,
               std::uint64_t attempt,
               const std::string& modelDigest,
               const std::string& graphDigest,
               std::uint64_t capturedAtMs,
               std::uint64_t expiresAtMs,
               const std::optional<std::string>& signature)
{
  const auto topologyBackend = std::any_of(
    config.devices.begin(), config.devices.end(), [] (const std::string& device) {
      return device.rfind("cuda:", 0) == 0;
    }) ? "cuda" : "cpu";
  std::ostringstream output;
  output << "{\"accepted_roles\":" << stringArray(config.acceptedRoles)
         << ",\"ack_reservation\":false"
         << ",\"attempt\":" << attempt
         << ",\"backends\":" << stringArray(config.backends)
         << ",\"bandwidth_mbps\":0.0"
         << ",\"boot_epoch\":" << quote(config.bootEpoch)
         << ",\"can_provision\":" << (config.canProvision ? "true" : "false")
         << ",\"captured_at_ms\":" << capturedAtMs
         << ",\"estimated_wait_ms\":0.0"
         << ",\"execution_disposition\":\"ACCEPT_WITH_PREPARATION\""
         << ",\"expires_at_ms\":" << expiresAtMs
         << ",\"graph_digest\":" << quote(graphDigest)
         << ",\"has_model\":" << (config.hasModel ? "true" : "false")
         << ",\"model_digest\":" << quote(modelDigest)
         << ",\"preparation_accepted\":true"
         << ",\"provider\":" << quote(config.provider)
         << ",\"queue_depth\":0"
         << ",\"request_id\":" << quote(requestId)
         << ",\"residency\":[]"
         << ",\"resources\":" << resourceArray(resources)
         << ",\"rtt_ms\":0.0"
         << ",\"schema\":\"DI_PLACEMENT_V3\""
         << ",\"schema_version\":3"
         << ",\"service\":" << quote(config.service);
  if (signature) {
    output << ",\"signature\":" << quote(*signature);
  }
  output << ",\"signer_key_id\":" << quote(config.signerKeyId)
         << ",\"status\":true"
         << ",\"topology\":{\"backend\":" << quote(topologyBackend)
         << ",\"devices\":" << stringArray(config.devices)
         << ",\"provider\":" << quote(config.provider)
         << ",\"topology_digest\":\"\"}}";
  return output.str();
}

} // namespace

std::optional<NativeProviderOfferV3Decision>
issueNativeProviderOfferV3(const std::vector<std::uint8_t>& requestPayload,
                           const NativeProviderOfferV3Config& config,
                           std::uint64_t nowMs)
{
  validateConfig(config);
  const std::string wire(requestPayload.begin(), requestPayload.end());
  boost::property_tree::ptree root;
  try {
    std::istringstream input(wire);
    boost::property_tree::read_json(input, root);
  }
  catch (const boost::property_tree::json_parser::json_parser_error&) {
    return std::nullopt;
  }
  if (root.get<std::string>("schema", "") != REQUEST_SCHEMA) {
    return std::nullopt;
  }
  const auto task = root.get_child_optional("task");
  const auto placement = root.get<std::string>("placementProfile", "");
  const auto taskPlacement = task
    ? task->get<std::string>("placement_profile",
                             task->get<std::string>("placementProfile", ""))
    : std::string{};
  if (placement != PLACEMENT_PROFILE && taskPlacement != PLACEMENT_PROFILE) {
    return std::nullopt;
  }

  const auto requestId = root.get<std::string>("request_id", "");
  const auto service = root.get<std::string>("service", "");
  const auto modelDigest = root.get<std::string>("model_identity_hash", "");
  const auto attempt = root.get<std::uint64_t>("attempt", 0);
  const auto deadlineMs = root.get<std::uint64_t>("plan_deadline_ms", 0);
  if (requestId.empty() || service != config.service || !isDigest(modelDigest) ||
      attempt == 0 || deadlineMs == 0) {
    return reject("DI_V3_REQUEST_REJECTED");
  }
  if (deadlineMs <= nowMs) {
    return reject("DI_V3_REQUEST_EXPIRED");
  }
  auto graphDigest = root.get<std::string>("graphDigest", "");
  if (graphDigest.empty() && task) {
    graphDigest = task->get<std::string>("graph_digest", "");
  }
  if (!isDigest(graphDigest)) {
    graphDigest = UNBOUND_GRAPH_DIGEST;
  }

  const auto resources = config.resourceSnapshot ? config.resourceSnapshot() : config.resources;
  // Validate the callback result against the same topology that is signed.
  NativeProviderOfferV3Config snapshotConfig = config;
  snapshotConfig.resources = resources;
  validateConfig(snapshotConfig);

  const auto unsignedOffer = canonicalOffer(
    snapshotConfig, resources, requestId, attempt, modelDigest, graphDigest,
    nowMs, deadlineMs, std::nullopt);
  const auto signature = config.signDigest(sha256Digest(unsignedOffer));
  if (signature.empty()) {
    throw std::runtime_error("native V3 Provider offer signer returned no signature");
  }
  NativeProviderOfferV3Decision result;
  result.status = true;
  result.message = "DI_PLACEMENT_V3_OFFER";
  result.payload = canonicalOffer(
    snapshotConfig, resources, requestId, attempt, modelDigest, graphDigest,
    nowMs, deadlineMs, signature);
  result.pendingStateTtlMs = deadlineMs - nowMs;
  return result;
}

} // namespace ndnsf::di
