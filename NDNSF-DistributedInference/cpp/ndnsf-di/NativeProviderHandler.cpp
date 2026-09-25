#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DistributedExecutionConsistency.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include "ndn-service-framework/utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ndnsf::di {

std::map<std::string, std::string>
parseNativeProviderAssignmentFields(const ndn::Buffer& payload,
                                    const std::string& selectedRole)
{
  std::map<std::string, std::string> fields;
  const std::string text(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first != std::string::npos && text[first] == '{') {
    boost::property_tree::ptree root;
    try {
      std::istringstream input(text);
      boost::property_tree::read_json(input, root);
    }
    catch (const boost::property_tree::json_parser::json_parser_error& exc) {
      throw std::invalid_argument(
        std::string("malformed V3 Selection projection: ") + exc.what());
    }
    const auto schema = root.get<std::string>("schema", "");
    const auto schemaVersion = root.get<int>("schema_version", 0);
    if (schema != "ndnsf-di-selection-v3" || schemaVersion != 3) {
      throw std::invalid_argument(
        "V3 Selection projection schema mismatch: schema=" + schema +
        " schema_version=" + std::to_string(schemaVersion));
    }
    std::istringstream projectionInput(text);
    const auto projection = nativeSelectionProjectionV3FromJson(
      projectionInput, selectedRole);
    const auto assign = [&] (const char* outputName, const char* inputName) {
      const auto value = root.get_optional<std::string>(inputName);
      if (value && !value->empty()) {
        fields[outputName] = *value;
      }
    };
    assign("provider", "provider");
    assign("executionRequestId", "request_id");
    assign("planCoreDigest", "plan_core_digest");
    assign("executionPlanDigest", "plan_digest");
    assign("securityPolicySnapshotDigest",
           "security_policy_snapshot_digest");
    assign("groupCapabilityV1", "group_capability_v1");
    const auto attempt = root.get_optional<std::uint64_t>("attempt");
    if (attempt) {
      fields["executionAttemptEpoch"] = std::to_string(*attempt);
    }
    const auto deadline = root.get_optional<std::uint64_t>("deadline_ms");
    if (deadline) {
      fields["deadlineMs"] = std::to_string(*deadline);
    }

    const auto& selected = projection.selectedRole;
    fields["role"] = selected.selectedRole;
    fields["backend"] = selected.backend;
    fields["artifactDigest"] = selected.artifactDigest;
    fields["fragmentDigest"] = selected.artifactDigest;
    fields["recipeDigest"] = selected.recipeDigest;
    fields["roleKind"] = selected.roleKind;
    fields["adapterId"] = selected.adapterId;
    fields["adapterVersion"] = selected.adapterVersion;
    fields["modelManifestDigest"] = selected.modelManifestDigest;
    fields["artifactProfileDigest"] = selected.artifactProfileDigest;
    fields["protectionEpoch"] = selected.protectionEpoch;
    if (projection.hasGrantBinding) {
      fields["grantName"] = projection.grantName;
      fields["grantDigest"] = projection.grantDigest;
    }
    fields["graphDigest"] = selected.graphDigest;
    fields["canonicalInitializerDigest"] = selected.canonicalInitializerDigest;
    fields["adapterDescriptorDigest"] = selected.adapterDescriptorDigest;
    fields["assemblerDescriptorDigest"] = selected.assemblerDescriptorDigest;
    fields["backendAbi"] = selected.backendAbi;
    fields["precision"] = selected.precision;
    fields["quantization"] = selected.quantization;
    fields["layout"] = selected.layout;
    fields["padding"] = selected.padding;
    fields["maxSourceBytes"] = std::to_string(selected.maxSourceBytes);
    fields["maxAssembledBytes"] = std::to_string(selected.maxAssembledBytes);
    fields["maxNodes"] = std::to_string(selected.maxNodes);
    fields["rank"] = std::to_string(selected.rank);
    if (selected.deviceSet.size() == 1) {
      fields["device"] = selected.deviceSet.front();
    }
    else if (projection.deviceBinding.mode == "CPU") {
      fields["device"] = "cpu:0";
    }

    // A DATA_V1 Selection may also carry execution-lease and activation
    // bindings. These are scoped to the projection's single local role. Do
    // not fall back to the legacy semicolon assignment for this path.
    if (const auto executionBindings =
          root.get_child_optional("execution_bindings")) {
        const auto& bindingRole = selected.selectedRole;
        const boost::property_tree::ptree* binding = nullptr;
        for (const auto& item : *executionBindings) {
          if (item.first == bindingRole) {
            if (binding != nullptr) {
              throw std::invalid_argument(
                "V3 Selection projection contains duplicate execution binding");
            }
            binding = &item.second;
          }
        }
        if (binding == nullptr) {
          throw std::invalid_argument(
            "V3 Selection projection is missing current execution binding");
        }
        const auto assignBinding = [&] (const char* outputName,
                                        const char* inputName) {
          const auto value = binding->get_optional<std::string>(inputName);
          if (value && !value->empty()) {
            fields[outputName] = *value;
          }
        };
        assignBinding("executionProviderBootId", "provider_boot_id");
        assignBinding("executionLeaseId", "lease_id");
        assignBinding("executionLeaseEpoch", "lease_epoch");
        assignBinding("executionLeasePlanDigest", "lease_plan_digest");
        assignBinding("executionLeaseBindingProof", "lease_binding_proof");
        assignBinding("executionLeaseProviderRoleCount",
                      "lease_provider_role_count");
        assignBinding("executionActivationDigest", "activation_digest");
        assignBinding("executionActivationMembers", "activation_members");
        assignBinding("executionActivationLocalMember",
                      "activation_local_member");
        assignBinding("executionFencingToken", "fencing_token");
    }
    return fields;
  }

  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto eq = text.find('=', pos);
    if (eq == std::string::npos) {
      break;
    }
    const auto end = text.find(';', eq + 1);
    fields[text.substr(pos, eq - pos)] =
      text.substr(eq + 1, (end == std::string::npos ? text.size() : end) - eq - 1);
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return fields;
}

bool
nativeRequestContractDigestMatches(const std::string& expectedDigest,
                                   const ndn::Buffer& requestPayload)
{
  if (expectedDigest.size() != 71 ||
      expectedDigest.compare(0, 7, "sha256:") != 0 ||
      !std::all_of(expectedDigest.begin() + 7, expectedDigest.end(),
                   [] (unsigned char value) {
                     return (value >= '0' && value <= '9') ||
                            (value >= 'a' && value <= 'f');
                   })) {
    return false;
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(requestPayload.data(), requestPayload.size(), digest.data());
  std::ostringstream actual;
  actual << "sha256:" << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    actual << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return expectedDigest == actual.str();
}

bool
nativeGenerationIdMatchesStreamOptions(
  const std::string& generationId,
  const ndn_service_framework::StreamGenerationId& expected)
{
  if (generationId.size() != expected.size() * 2) {
    return false;
  }
  static constexpr char digits[] = "0123456789abcdef";
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto byte = expected[index];
    if (generationId[index * 2] != digits[(byte >> 4) & 0x0F] ||
        generationId[index * 2 + 1] != digits[byte & 0x0F]) {
      return false;
    }
  }
  return true;
}

NativeProviderExecutionBindingResult
validateNativeProviderExecutionBinding(
  const std::map<std::string, std::string>& fields,
  const std::string& expectedProviderBootId,
  const std::string& expectedPlanDigest,
  ExecutionAttemptAuthority& authority)
{
  NativeProviderExecutionBindingResult result;
  result.attempt.requestId = nativeProviderFieldValue(
    fields, {"executionRequestId"});
  const auto epochText = nativeProviderFieldValue(
    fields, {"executionAttemptEpoch"});
  const auto providerBootId = nativeProviderFieldValue(
    fields, {"executionProviderBootId"});
  const auto planDigest = nativeProviderFieldValue(
    fields, {"executionPlanDigest", "executionLeasePlanDigest"});
  if (result.attempt.requestId.empty() || epochText.empty()) {
    result.reason = "DI_ATTEMPT_BINDING_MISSING";
    return result;
  }
  try {
    std::size_t consumed = 0;
    result.attempt.attemptEpoch = std::stoull(epochText, &consumed);
    if (consumed != epochText.size()) {
      throw std::invalid_argument("trailing epoch text");
    }
    result.attempt.validate();
  }
  catch (const std::exception&) {
    result.reason = "DI_ATTEMPT_EPOCH_INVALID";
    return result;
  }
  if (expectedProviderBootId.empty() || providerBootId != expectedProviderBootId) {
    result.reason = "DI_PROVIDER_BOOT_MISMATCH";
    return result;
  }
  if (expectedPlanDigest.empty() || planDigest != expectedPlanDigest) {
    result.reason = "DI_PLAN_BINDING_MISMATCH";
    return result;
  }
  const auto admission = authority.admit(result.attempt);
  if (admission != ExecutionAttemptAdmission::Accepted) {
    result.reason = std::string("DI_ATTEMPT_") + toString(admission);
    return result;
  }
  result.status = true;
  result.reason = "OK";
  return result;
}

NativeProviderExecutionControlResult
applyNativeProviderExecutionControl(
  const std::map<std::string, std::string>& fields,
  ExecutionAttemptAuthority& authority)
{
  NativeProviderExecutionControlResult result;
  const auto schema = nativeProviderFieldValue(fields, {"schema"});
  if (schema != "ndnsf-di-execution-control-v2" &&
      schema != "ndnsf-di-execution-control-v1") {
    return result;
  }
  result.recognized = true;
  if (schema == "ndnsf-di-execution-control-v1") {
    logRuntimeInfo(
      "NDNSF_DI_LEGACY_IMPORT kind=execution-control-v1 count=1");
  }
  const auto operation = nativeProviderFieldValue(fields, {"operation"});
  result.attempt.requestId = nativeProviderFieldValue(fields, {"requestId"});
  try {
    result.attempt.attemptEpoch = std::stoull(
      nativeProviderFieldValue(fields, {"attemptEpoch"}));
    result.attempt.validate();
    if (operation == "CANCEL") {
      result.status = authority.cancel(result.attempt);
      result.reason = result.status ? "CANCELLED" : "CANCEL_REJECTED";
      return result;
    }
    if (operation == "SUPERSEDE") {
      const auto nextEpoch = std::stoull(nativeProviderFieldValue(
        fields, {"supersededByAttemptEpoch"}));
      authority.cancel(result.attempt);
      ExecutionAttemptKey replacement{result.attempt.requestId, nextEpoch};
      replacement.validate();
      const auto admitted = authority.admit(replacement);
      result.status = admitted == ExecutionAttemptAdmission::Accepted;
      result.reason = result.status ? "SUPERSEDED" :
        std::string("SUPERSEDE_") + toString(admitted);
      return result;
    }
    result.reason = "CONTROL_OPERATION_UNSUPPORTED";
  }
  catch (const std::exception&) {
    result.reason = "CONTROL_BINDING_INVALID";
  }
  return result;
}

namespace {

struct LocalGenerationStateNames
{
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
};

LocalGenerationStateNames
localGenerationStateNames(const NativeSelectionProjectionV3& projection)
{
  const auto contains = [] (const auto& tensors, const std::string& name) {
    return std::any_of(tensors.begin(), tensors.end(),
                       [&name] (const auto& tensor) {
                         return tensor.name == name;
                       });
  };
  LocalGenerationStateNames result;
  for (const auto& name : projection.generationContract.stateInputNames) {
    if (contains(projection.assembly.expectedInputs, name)) {
      result.inputs.push_back(name);
    }
  }
  for (const auto& name : projection.generationContract.stateOutputNames) {
    if (contains(projection.assembly.expectedOutputs, name)) {
      result.outputs.push_back(name);
    }
  }
  if (result.inputs.empty() || result.inputs.size() != result.outputs.size()) {
    throw std::invalid_argument(
      "authenticated generation state contract does not match local role");
  }
  return result;
}

std::vector<uint8_t>
bufferToVector(const ndn::Buffer& buffer)
{
  return std::vector<uint8_t>(buffer.begin(), buffer.end());
}

double
durationMs(std::chrono::steady_clock::time_point start,
           std::chrono::steady_clock::time_point end)
{
  return std::chrono::duration<double, std::milli>(end - start).count();
}

long long
epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

struct ConversationPromotionControl
{
  std::string action;
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::uint64_t successorContextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string roleName;
  std::string receiptDigest;
  std::string checkpointDigest;
  std::uint64_t expiresAtMs = 0;
};

ConversationPromotionControl
parseConversationPromotionControl(const ndn::Buffer& payload)
{
  const std::string text(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
  boost::property_tree::ptree root;
  try {
    std::istringstream input(text);
    boost::property_tree::read_json(input, root);
  }
  catch (const std::exception& exc) {
    throw std::invalid_argument(
      std::string("malformed conversation promotion control: ") + exc.what());
  }
  static const std::set<std::string> allowedFields = {
    "schema", "action", "conversationId", "parentContextEpoch",
    "successorContextEpoch", "serviceName", "planRoleMapDigest",
    "roleName", "receiptDigest", "checkpointDigest", "expiresAtMs",
  };
  std::set<std::string> seenFields;
  for (const auto& field : root) {
    if (!allowedFields.count(field.first) ||
        !seenFields.insert(field.first).second) {
      throw std::invalid_argument(
        "conversation promotion control contains unknown or duplicate fields");
    }
  }
  if (seenFields.size() != allowedFields.size()) {
    throw std::invalid_argument(
      "conversation promotion control is missing required fields");
  }
  if (root.get<std::string>("schema", "") !=
        "ndnsf-di-conversation-promotion-control-v1") {
    throw std::invalid_argument("conversation promotion control schema mismatch");
  }
  ConversationPromotionControl control;
  control.action = root.get<std::string>("action", "");
  control.conversationId = root.get<std::string>("conversationId", "");
  control.parentContextEpoch = root.get<std::uint64_t>("parentContextEpoch", 0);
  control.successorContextEpoch = root.get<std::uint64_t>("successorContextEpoch", 0);
  control.serviceName = root.get<std::string>("serviceName", "");
  control.planRoleMapDigest = root.get<std::string>("planRoleMapDigest", "");
  control.roleName = root.get<std::string>("roleName", "");
  control.receiptDigest = root.get<std::string>("receiptDigest", "");
  control.checkpointDigest = root.get<std::string>("checkpointDigest", "");
  control.expiresAtMs = root.get<std::uint64_t>("expiresAtMs", 0);
  if (control.action != "COMMIT" && control.action != "ROLLBACK" && control.action != "FINALIZE") {
    throw std::invalid_argument("conversation promotion control action is invalid");
  }
  if (control.conversationId.empty() || control.conversationId.find('/') !=
        std::string::npos || control.successorContextEpoch !=
        control.parentContextEpoch + 1 || control.serviceName.empty() ||
      control.serviceName.front() != '/' || control.roleName.empty() ||
      control.expiresAtMs == 0) {
    throw std::invalid_argument("conversation promotion control identity is incomplete");
  }
  using decode_state_identity_detail::requireDigest;
  requireDigest(control.planRoleMapDigest, "conversation control plan-role map");
  requireDigest(control.receiptDigest, "conversation control receipt");
  requireDigest(control.checkpointDigest, "conversation control checkpoint");
  return control;
}

long long
approxEpochMs(std::chrono::steady_clock::time_point baseSteady,
              long long baseEpochMs,
              std::chrono::steady_clock::time_point point)
{
  return baseEpochMs + static_cast<long long>(durationMs(baseSteady, point));
}

std::string
plannedNameOrFalse(const std::string& plannedDataName)
{
  return plannedDataName.empty() ? "false" : plannedDataName;
}

std::string
plannedSegmentOrFalse(const std::vector<std::string>& plannedSegmentNames,
                      bool last = false)
{
  if (plannedSegmentNames.empty()) {
    return "false";
  }
  return last ? plannedSegmentNames.back() : plannedSegmentNames.front();
}

void
appendStageTransferObservation(std::ostringstream& record,
                               const std::shared_ptr<StageTransferObservation>& observation)
{
  if (!observation) {
    record << " stage_edge=unknown"
           << " stage_phase=unknown"
           << " stage_direction=unknown"
           << " stage_identity=unknown"
           << " actual_data_name=unknown"
           << " lineage_present=unknown"
           << " tensor_bytes=unknown"
           << " tensor_names=unknown"
           << " encoded_payload_bytes=unknown"
           << " transport_payload_bytes=unknown"
           << " metadata_bytes=unknown"
           << " wire_bytes=unknown"
           << " interest_count=unknown"
           << " retry_count=unknown"
           << " local_copy_bytes=unknown"
           << " transport_local_copy_bytes=unknown";
    return;
  }
  const auto optionalSize = [] (const auto& value) {
    return value ? std::to_string(*value) : std::string("unknown");
  };
  std::string tensorNames;
  for (const auto& name : observation->tensorNames) {
    if (!tensorNames.empty()) {
      tensorNames += ",";
    }
    tensorNames += name;
  }
  record << " stage_edge=" << observation->edgeScope
         << " stage_phase=" << observation->phase
         << " stage_direction=" << observation->direction
         << " stage_identity=" << observation->identity
         << " actual_data_name="
         << (observation->actualDataName.empty() ? "unknown" : observation->actualDataName)
         << " lineage_present=" << (observation->lineagePresent ? "true" : "false")
         << " tensor_bytes=" << observation->tensorBytes
         << " tensor_count=" << observation->tensorNames.size()
         << " tensor_names=" << (tensorNames.empty() ? "none" : tensorNames)
         << " encoded_payload_bytes=" << observation->encodedPayloadBytes
         << " transport_payload_bytes="
         << optionalSize(observation->transportPayloadBytes)
         << " metadata_bytes=" << optionalSize(observation->metadataBytes)
         << " wire_bytes=" << optionalSize(observation->wireBytes)
         << " interest_count=" << optionalSize(observation->interestCount)
         << " retry_count=" << optionalSize(observation->retryCount)
         << " local_copy_bytes=" << optionalSize(observation->localCopyBytes)
         << " transport_local_copy_bytes="
         << optionalSize(observation->transportLocalCopyBytes);
}

bool
runtimeTimingEnabled()
{
  const char* value = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (value == nullptr) {
    return false;
  }
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
           text == "off" || text == "OFF");
}

bool
nativeTraceEnabled()
{
  return runtimeTimingEnabled() || std::getenv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE") != nullptr;
}

void
logProviderStageMarker(const char* stage,
                       const std::string& requestId,
                       const std::string& provider,
                       const std::string& role,
                       const std::string& planDigest,
                       const char* status = "observed",
                       const std::string& reason = {},
                       const std::string& attemptEpoch = {},
                       const std::string& preparationId = {})
{
  std::ostringstream record;
  record << "NDNSF_DI_PROVIDER_STAGE"
         << " stage=" << stage
         << " status=" << status
         << " requestId=" << requestId
         << " provider=" << provider
         << " role=" << role
         << " planDigest=" << planDigest
         << " attemptEpoch=" << attemptEpoch;
  if (!reason.empty()) {
    record << " reason=" << reason;
  }
  if (!preparationId.empty()) {
    record << " preparationId=" << preparationId;
  }
  logRuntimeEvidence(record.str());
}

void
logProviderBoundaryStdout(const char* stage,
                          const std::string& requestId,
                          const std::string& provider,
                          const std::string& role)
{
  std::ostringstream record;
  record << "NDNSF_DI_PROVIDER_BOUNDARY"
            << " stage=" << stage
            << " requestId=" << requestId
            << " provider=" << provider
            << " role=" << role;
  logRuntimeEvidence(record.str());
}

std::string
metadataValue(const NativeModelRunnerSpec& spec,
              std::initializer_list<const char*> names)
{
  for (const auto* name : names) {
    const auto found = spec.metadata.find(name);
    if (found != spec.metadata.end()) {
      return found->second;
    }
  }
  return "";
}

std::string
fragmentDigestFor(const NativeModelRunnerSpec& spec)
{
  auto digest = metadataValue(
    spec,
    {"fragmentDigest", "fragment_digest", "sha256", "digest"});
  if (!digest.empty()) {
    return digest;
  }
  return spec.role.empty() ? "unknown" : "role:" + spec.role;
}

std::string
loadedResidencyFor(const NativeModelRunnerSpec& spec)
{
  auto device = metadataValue(
    spec,
    {"device", "runtimeDevice", "runtime_device", "executionProvider", "execution_provider"});
  std::transform(device.begin(), device.end(), device.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (device.find("cuda") != std::string::npos ||
      device.find("gpu") != std::string::npos) {
    return "GPU_LOADED";
  }
  return "CPU_RESIDENT";
}

void
logFragmentInventoryEvent(const char* event,
                          const NativeModelRunnerSpec& spec,
                          const std::string& provider = "")
{
  if (!nativeTraceEnabled()) {
    return;
  }
  std::ostringstream record;
  record << "NDNSF_DI_FRAGMENT_INVENTORY"
         << " event=" << event
         << " provider=" << (provider.empty() ? "unknown" : provider)
         << " role=" << spec.role
         << " fragmentDigest=" << fragmentDigestFor(spec)
         << " backend=" << (spec.backend.empty() ? "unknown" : spec.backend)
         << " path=" << (spec.path.empty() ? "none" : spec.path)
         << " residency="
         << (std::string(event) == "EVICTED" ||
             std::string(event) == "DISK_RESIDENT" ? "DISK_RESIDENT" : loadedResidencyFor(spec))
         << " epoch_ms=" << epochMs();
  logRuntimeInfo(record.str());
}

const NativeModelRunnerSpec*
runnerSpecForRole(const std::vector<NativeModelRunnerSpec>& specs,
                  const std::string& role)
{
  const auto found = std::find_if(specs.begin(), specs.end(),
                                  [&role] (const NativeModelRunnerSpec& spec) {
                                    return spec.role == role;
                                  });
  return found == specs.end() ? nullptr : &*found;
}

int
boundedProviderTimeoutMs(int configured)
{
  return std::max(50, configured);
}

int
collaborationFetchTimeoutMs(int configured)
{
  const char* value = std::getenv("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS");
  if (value == nullptr || std::string(value).empty()) {
    return boundedProviderTimeoutMs(configured);
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed <= 0) {
    return boundedProviderTimeoutMs(configured);
  }
  return static_cast<int>(std::max<long>(50, parsed));
}

NativeProviderTimeoutBudget
makeNativeProviderTimeoutBudget(const NativeProviderHandlerConfig& config)
{
  // The deployment override applies to the collaboration data-plane fetch;
  // the configured sources remain separate so a large model fetch budget
  // cannot silently extend readiness or conversation control deadlines.
  return {
    collaborationFetchTimeoutMs(config.dependencyFetchTimeoutMs),
    boundedProviderTimeoutMs(config.fetchTimeoutMs),
    boundedProviderTimeoutMs(config.fetchTimeoutMs),
  };
}

class NativeProviderHandlerState
{
public:
  explicit NativeProviderHandlerState(const NativeProviderHandlerConfig& config)
    : plan(config.plan)
    , baseAssignment(config.assignment)
    , runnerSpecs(config.runnerSpecs)
    , runnerFactory(config.runnerFactory)
    , localProviderName(config.localProviderName)
    // Preserve the runtime's existing byte/entry capacities; only the explicit
    // per-host conversation TTL differs from its default construction.
    , runtime(config.workerCount, config.workerQueueCapacity,
              512ULL * 1024ULL * 1024ULL, 128,
              512ULL * 1024ULL * 1024ULL, 128,
              512ULL * 1024ULL * 1024ULL, 128,
              config.conversationRetentionMs)
    , executionLeaseTable(config.executionLeaseTable)
    , executionLeaseCleanupIntervalMs(config.executionLeaseCleanupIntervalMs)
  {
    if (!runnerFactory) {
      throw std::invalid_argument(
        "NativeProviderHandlerState requires NativeModelRunnerFactory");
    }
    if (!config.runnerPreparationFactory) {
      for (const auto& spec : runnerSpecs) {
        logFragmentInventoryEvent("DISK_RESIDENT", spec, localProviderName);
        auto runner = runnerFactory->create(spec);
        if (auto evidence = runner->executionEvidenceSnapshot()) {
          executionEvidence.push_back(std::move(*evidence));
        }
        runtime.registerRunner(spec, std::move(runner));
        logFragmentInventoryEvent(loadedResidencyFor(spec).c_str(), spec, localProviderName);
      }
    }
    if (executionLeaseTable != nullptr && executionLeaseCleanupIntervalMs > 0) {
      executionLeaseCleanupThread = std::thread([this] {
        std::unique_lock<std::mutex> lock(executionLeaseCleanupMutex);
        const auto interval = std::chrono::milliseconds(
          executionLeaseCleanupIntervalMs);
        while (!executionLeaseCleanupStop) {
          if (executionLeaseCleanupCondition.wait_for(
                lock, interval, [this] { return executionLeaseCleanupStop; })) {
            break;
          }
          lock.unlock();
          const auto now = static_cast<uint64_t>(
            std::max<long long>(0, epochMs()));
          executionLeaseTable->cleanupExpired(now);
          lock.lock();
        }
      });
    }
  }

  ~NativeProviderHandlerState()
  {
    {
      std::lock_guard<std::mutex> lock(executionLeaseCleanupMutex);
      executionLeaseCleanupStop = true;
    }
    executionLeaseCleanupCondition.notify_all();
    if (executionLeaseCleanupThread.joinable()) {
      executionLeaseCleanupThread.join();
    }
    if (executionLeaseTable != nullptr) {
      const auto now = static_cast<uint64_t>(
        std::max<long long>(0, epochMs()));
      executionLeaseTable->cleanupExpired(now);
    }
    if (!runnerSpecs.empty()) {
      for (const auto& spec : runnerSpecs) {
        logFragmentInventoryEvent("EVICTED", spec, localProviderName);
      }
    }
  }

  void
  completeExecutionLease(
    ndn_service_framework::ProviderExecutionLeaseTable* table,
    const std::string& leaseId,
    const std::string& providerEpoch,
    const std::string& requesterName,
    const std::string& role,
    std::size_t expectedRoles,
    bool completedLocalPlan)
  {
    if (table == nullptr || leaseId.empty()) {
      return;
    }
    bool shouldRelease = false;
    {
      std::lock_guard<std::mutex> lock(executionLeaseMutex);
      auto& completed = completedRolesByLease[leaseId];
      completed.insert(role);
      shouldRelease = completedLocalPlan ||
        completed.size() >= std::max<std::size_t>(1, expectedRoles);
      if (shouldRelease) {
        completedRolesByLease.erase(leaseId);
      }
    }
    if (shouldRelease) {
      const auto now = static_cast<uint64_t>(std::max<long long>(0, epochMs()));
      table->release(leaseId,
                     providerEpoch,
                     requesterName,
                     "provider-complete:" + leaseId,
                     now);
    }
  }

  NativeExecutionPlan plan;
  NativeProviderAssignment baseAssignment;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  std::string localProviderName;
  NativeProviderRuntime runtime;
  ndn_service_framework::ProviderExecutionLeaseTable* executionLeaseTable = nullptr;
  uint64_t executionLeaseCleanupIntervalMs = 0;
  std::mutex executionLeaseCleanupMutex;
  std::condition_variable executionLeaseCleanupCondition;
  bool executionLeaseCleanupStop = false;
  std::thread executionLeaseCleanupThread;
  std::vector<ExecutionEvidence> executionEvidence;
  std::mutex executionLeaseMutex;
  std::map<std::string, std::set<std::string>> completedRolesByLease;
  ExecutionAttemptAuthority attemptAuthority;
};

void
logProviderTiming(const std::string& sessionId,
                  const std::string& role,
                  const ProviderRoleResult& result,
                  std::chrono::steady_clock::time_point baseSteady,
                  long long baseEpochMs)
{
  if (!runtimeTimingEnabled()) {
    return;
  }

  const auto workerQueueWaitMs = durationMs(result.timing.queuedAt,
                                            result.timing.workerStartedAt);
  const auto inputFetchWaitMs = durationMs(result.timing.workerStartedAt,
                                           result.timing.startedAt);
  const auto runnerPublishMs = durationMs(result.timing.startedAt,
                                          result.timing.finishedAt);
  const auto handlerMs = durationMs(result.timing.workerStartedAt,
                                    result.timing.finishedAt);
  const auto totalMs = durationMs(result.timing.queuedAt,
                                  result.timing.finishedAt);
  const auto workerStartEpoch = approxEpochMs(baseSteady, baseEpochMs,
                                              result.timing.workerStartedAt);
  const auto startEpoch = approxEpochMs(baseSteady, baseEpochMs,
                                        result.timing.startedAt);
  const auto endEpoch = approxEpochMs(baseSteady, baseEpochMs, result.timing.finishedAt);

  std::ostringstream record;
  record << std::fixed << std::setprecision(3)
         << "NDNSF_DI_PROVIDER_HANDLER_TIMING"
         << " event=start"
         << " session=" << sessionId
         << " role=" << role
         << " submitted_epoch_ms=" << baseEpochMs
         << " worker_start_epoch_ms=" << workerStartEpoch
         << " start_epoch_ms=" << startEpoch
         << " queue_wait_ms=" << workerQueueWaitMs
         << " worker_queue_wait_ms=" << workerQueueWaitMs
         << " input_fetch_wait_ms=" << inputFetchWaitMs
         << " runner_publish_ms=0"
         << " total_ms=0"
         << " handler_ms=0";
  logRuntimeEvidence(record.str());

  record.str({});
  record.clear();
  record << std::fixed << std::setprecision(3)
         << "NDNSF_DI_PROVIDER_HANDLER_TIMING"
         << " event=end"
         << " session=" << sessionId
         << " role=" << role
         << " submitted_epoch_ms=" << baseEpochMs
         << " worker_start_epoch_ms=" << workerStartEpoch
         << " start_epoch_ms=" << startEpoch
         << " end_epoch_ms=" << endEpoch
         << " queue_wait_ms=" << workerQueueWaitMs
         << " worker_queue_wait_ms=" << workerQueueWaitMs
         << " input_fetch_wait_ms=" << inputFetchWaitMs
         << " runner_publish_ms=" << runnerPublishMs
         << " total_ms=" << totalMs
         << " handler_ms=" << handlerMs;
  logRuntimeEvidence(record.str());

  for (const auto& timing : result.inputTimings) {
    const auto fetchMs = durationMs(timing.prefetchStartedAt, timing.fetchCompletedAt);
    const auto prefetchTotalMs = fetchMs;
    const auto prefetchOverlapMs = std::max(
      0.0,
      durationMs(timing.prefetchStartedAt, result.timing.startedAt));
    record.str({});
    record.clear();
    record << std::fixed << std::setprecision(3)
           << "NDNSF_DI_DEPENDENCY_INPUT_TIMING"
           << " session=" << sessionId
           << " role=" << role
           << " producer=" << timing.producerRole
           << " scope=" << timing.scope
           << " future_wait_ms=" << fetchMs
           << " ref_wait_ms=0"
           << " fetch_ms=" << fetchMs
           << " decode_ms=0"
           << " prefetch_total_ms=" << prefetchTotalMs
           << " prefetch_overlap_ms=" << prefetchOverlapMs
           << " bytes=" << timing.bytes
           << " expected_segments=" << timing.expectedSegments
           << " expected_bytes=" << timing.expectedBytes
           << " planned_segment_count=" << timing.plannedSegmentNames.size()
           << " first_planned_segment="
           << plannedSegmentOrFalse(timing.plannedSegmentNames)
           << " last_planned_segment="
           << plannedSegmentOrFalse(timing.plannedSegmentNames, true)
           << " data_name=" << plannedNameOrFalse(timing.plannedDataName)
           << " planned_name=" << plannedNameOrFalse(timing.plannedDataName);
    appendStageTransferObservation(record, timing.transferObservation);
    logRuntimeEvidence(record.str());
  }

  for (const auto& timing : result.outputTimings) {
    const auto publishMs = durationMs(timing.outputReadyAt, timing.publishDoneAt);
    record.str({});
    record.clear();
    record << std::fixed << std::setprecision(3)
           << "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING"
           << " session=" << sessionId
           << " role=" << role
           << " producer=" << timing.producerRole
           << " scope=" << timing.scope
           << " publish_ms=" << publishMs
           << " bytes=" << timing.bytes
           << " expected_segments=" << timing.expectedSegments
           << " expected_bytes=" << timing.expectedBytes
           << " planned_segment_count=" << timing.plannedSegmentNames.size()
           << " first_planned_segment="
           << plannedSegmentOrFalse(timing.plannedSegmentNames)
           << " last_planned_segment="
           << plannedSegmentOrFalse(timing.plannedSegmentNames, true)
           << " data_name=" << plannedNameOrFalse(timing.plannedDataName)
           << " output_ready_epoch_ms="
           << approxEpochMs(baseSteady, baseEpochMs, timing.outputReadyAt)
           << " publish_done_epoch_ms="
           << approxEpochMs(baseSteady, baseEpochMs, timing.publishDoneAt)
           << " planned_name=" << plannedNameOrFalse(timing.plannedDataName);
    appendStageTransferObservation(record, timing.transferObservation);
    logRuntimeEvidence(record.str());
  }

  record.str({});
  record.clear();
  const auto budgetSize = [] (std::size_t value, bool observed) {
    return observed ? std::to_string(value) : std::string("unknown");
  };
  record << "NDNSF_DI_STAGE_TRANSFER_BUDGET"
         << " session=" << sessionId
         << " role=" << role
         << " tensor_bytes=" << result.transferBudget.tensorBytes
         << " encoded_payload_bytes=" << result.transferBudget.encodedPayloadBytes
         << " transport_payload_bytes="
         << budgetSize(result.transferBudget.transportPayloadBytes,
                       result.transferBudget.transportPayloadObserved)
         << " metadata_bytes="
         << budgetSize(result.transferBudget.metadataBytes,
                       result.transferBudget.metadataObserved)
         << " wire_bytes="
         << budgetSize(result.transferBudget.wireBytes,
                       result.transferBudget.wireObserved)
         << " interest_count="
         << budgetSize(result.transferBudget.interestCount,
                       result.transferBudget.interestObserved)
         << " retry_count="
         << budgetSize(result.transferBudget.retryCount,
                       result.transferBudget.retryObserved)
         << " local_copy_bytes="
         << budgetSize(result.transferBudget.localCopyBytes,
                       result.transferBudget.localCopyObserved)
         << " transport_local_copy_bytes="
         << budgetSize(result.transferBudget.transportLocalCopyBytes,
                       result.transferBudget.transportLocalCopyObserved)
         << " transport_payload_observed="
         << (result.transferBudget.transportPayloadObserved ? "true" : "false")
         << " metadata_observed="
         << (result.transferBudget.metadataObserved ? "true" : "false")
         << " wire_observed="
         << (result.transferBudget.wireObserved ? "true" : "false")
         << " interest_observed="
         << (result.transferBudget.interestObserved ? "true" : "false")
         << " retry_observed="
         << (result.transferBudget.retryObserved ? "true" : "false")
         << " local_copy_observed="
         << (result.transferBudget.localCopyObserved ? "true" : "false")
         << " transport_local_copy_observed="
         << (result.transferBudget.transportLocalCopyObserved ? "true" : "false")
         << " observation_count=" << result.transferBudget.seenIdentities.size()
         << " snapshot_identity="
         << (result.transferBudget.snapshotIdentity.empty() ?
             "unknown" : result.transferBudget.snapshotIdentity);
  logRuntimeEvidence(record.str());
}

void
logProviderCapacity(const std::string& sessionId,
                    const std::string& role,
                    const char* event,
                    const ProviderRoleWorkerSnapshot& snapshot)
{
  if (!nativeTraceEnabled()) {
    return;
  }
  std::ostringstream record;
  record << "NDNSF_DI_PROVIDER_CAPACITY"
         << " event=" << event
         << " session=" << sessionId
         << " role=" << role
         << " workers=" << snapshot.workerCount
         << " active_workers=" << snapshot.activeWorkerCount
         << " idle_workers=" << snapshot.idleWorkerCount()
         << " ready_queue=" << snapshot.readyQueueDepth
         << " waiting_inputs=" << snapshot.waitingForInputCount
         << " pending_work=" << snapshot.pendingWorkCount()
         << " stopping=" << (snapshot.stopping ? "true" : "false");
  logRuntimeInfo(record.str());
}

std::map<std::string, TensorBundle>
initialInputsFromRequest(ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
                         const ndn_service_framework::RequestMessage& request,
                         bool applicationInputAuthorized)
{
  // Apply the ownership check at the fetch boundary, before reading any
  // request bytes or invoking the repository. Fault injection calls this
  // same boundary; it must not manufacture a successful rejection marker.
  if (!applicationInputAuthorized) {
    throw std::invalid_argument("DI_INPUT_FETCH_ROLE_MISMATCH");
  }
  auto payload = request.getPayload();
  if (const auto reference = ndn_service_framework::parseLargeDataReferencePayload(payload)) {
    auto fetched = ctx.fetchEncryptedLargeData(reference->dataName);
    if (!fetched) {
      throw std::runtime_error("failed to fetch request input large-data reference: " +
                               reference->dataName.toUri());
    }
    payload = *fetched;
  }
  else {
    const std::string wire(payload.begin(), payload.end());
    const auto first = wire.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && wire[first] == '{') {
      boost::property_tree::ptree root;
      try {
        std::istringstream input(wire);
        boost::property_tree::read_json(input, root);
      }
      catch (const boost::property_tree::json_parser::json_parser_error& exc) {
        throw std::invalid_argument(
          std::string("malformed native DI request envelope: ") + exc.what());
      }
      if (root.get<std::string>("schema", "") == "ndnsf-di-request-envelope-v2") {
        const auto transport = root.get<std::string>("input_transport", "");
        if (transport == "REPO_REF") {
          const auto dataName = root.get<std::string>("input_reference.dataName", "");
          const auto plaintextSize = root.get<std::uint64_t>(
            "input_reference.plaintextSize", 0);
          if (dataName.empty() || dataName.front() != '/' || plaintextSize == 0) {
            throw std::invalid_argument(
              "native DI REPO_REF request has invalid input reference");
          }
          auto fetched = ctx.fetchEncryptedLargeData(ndn::Name(dataName));
          if (!fetched) {
            throw std::runtime_error(
              "failed to fetch native DI request input reference: " + dataName);
          }
          if (fetched->size() != plaintextSize) {
            throw std::runtime_error(
              "native DI request input plaintext size mismatch");
          }
          payload = *fetched;
        }
        else if (transport == "INLINE") {
          const auto encoded = root.get<std::string>("input_payload_b64", "");
          if (encoded.empty()) {
            payload = ndn::Buffer{};
          }
          else {
            if (encoded.size() % 4 != 0) {
              throw std::invalid_argument(
                "native DI inline input is not canonical base64");
            }
            ndn::Buffer decoded(3 * (encoded.size() / 4));
            const auto size = EVP_DecodeBlock(
              decoded.data(),
              reinterpret_cast<const unsigned char*>(encoded.data()),
              static_cast<int>(encoded.size()));
            if (size < 0) {
              throw std::invalid_argument(
                "native DI inline input is not canonical base64");
            }
            std::size_t padding = 0;
            if (!encoded.empty() && encoded.back() == '=') ++padding;
            if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
            decoded.resize(static_cast<std::size_t>(size) - padding);
            payload = std::move(decoded);
          }
        }
        else {
          throw std::invalid_argument(
            "native DI request uses unsupported input transport");
        }
      }
    }
  }

  TensorBundle bundle;
  bundle.name = "request-input";
  bundle.payload = bufferToVector(payload);
  // The request reference is one complete, already authenticated object.
  // Give the V3 APPLICATION_INPUT edge a concrete single-object witness so
  // ProviderRoleWorker can pre-satisfy that edge without issuing a second
  // dependency Interest.
  bundle.expectedSegments = 1;
  bundle.expectedBytes = bundle.payload.size();
  return {{"request-input", std::move(bundle)}};
}

class LocalDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) final
  {
    auto promise = std::make_shared<std::promise<TensorBundle>>();
    auto future = promise->get_future();
    const auto itemKey = key(sessionId, edge);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_available.find(itemKey);
      if (found != m_available.end()) {
        promise->set_value(found->second);
        return future;
      }
      m_waiters[itemKey].push_back(std::move(promise));
    }
    return future;
  }

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& bundle) final
  {
    std::vector<std::shared_ptr<std::promise<TensorBundle>>> ready;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto itemKey = key(sessionId, edge);
      m_available[itemKey] = bundle;
      const auto found = m_waiters.find(itemKey);
      if (found != m_waiters.end()) {
        ready = std::move(found->second);
        m_waiters.erase(found);
      }
    }
    for (auto& promise : ready) {
      promise->set_value(bundle);
    }
  }

private:
  static std::string
  key(const std::string& sessionId, const DependencyEdge& edge)
  {
    return sessionId + "|" + edge.plannedDataName;
  }

private:
  std::mutex m_mutex;
  std::map<std::string, TensorBundle> m_available;
  std::map<std::string, std::vector<std::shared_ptr<std::promise<TensorBundle>>>> m_waiters;
};

bool
allPlanRolesAssignedToLocal(const NativeExecutionPlan& plan,
                            const NativeProviderAssignment& assignment,
                            const std::string& localProvider)
{
  if (localProvider.empty()) {
    return false;
  }
  for (const auto& role : plan.roles) {
    // A missing mapping is not evidence that this provider owns the role.
    // Treating it as local (via providerForRole's fallback) can make a
    // distributed request enter the local full-plan path and ask a provider
    // that only hosts /Merge to execute /Backbone as well.
    const auto found = assignment.providerByRole.find(role);
    if (found == assignment.providerByRole.end() ||
        found->second != localProvider) {
      return false;
    }
  }
  return true;
}

std::uint64_t
parseRequiredUint64(const std::map<std::string, std::string>& fields,
                    const char* name)
{
  const auto value = nativeProviderFieldValue(fields, {name});
  if (value.empty()) {
    throw std::invalid_argument(std::string("missing KV binding field: ") + name);
  }
  std::size_t consumed = 0;
  const auto parsed = std::stoull(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument(std::string("invalid KV binding field: ") + name);
  }
  return parsed;
}

KvStateBinding
kvBindingFromAssignment(const NativeModelRunnerSpec& spec,
                        const std::map<std::string, std::string>& fields,
                        const std::string& sessionId,
                        const std::string& role,
                        const std::string& providerName,
                        std::uint64_t expectedSecurityEpoch)
{
  KvStateBinding binding;
  binding.sessionId = nativeProviderFieldValue(fields, {"kvSessionId"});
  if (binding.sessionId.empty()) {
    binding.sessionId = sessionId;
  }
  binding.stage = role;
  binding.contextEpoch = parseRequiredUint64(fields, "kvContextEpoch");
  binding.providerName = providerName;
  binding.securityEpoch = parseRequiredUint64(fields, "kvSecurityEpoch");

  const auto expectedModel = nativeProviderFieldValue(
    spec.metadata, {"evidence.modelDigest"});
  const auto expectedPlan = nativeProviderFieldValue(
    spec.metadata, {"evidence.planDigest"});
  const auto expectedBoot = nativeProviderFieldValue(
    spec.metadata, {"evidence.providerBootId"});
  const auto requestedModel = nativeProviderFieldValue(fields, {"kvModelDigest"});
  const auto requestedPlan = nativeProviderFieldValue(fields, {"kvPlanDigest"});
  const auto requestedBoot = nativeProviderFieldValue(fields, {"kvProviderBootId"});
  if ((!requestedModel.empty() && requestedModel != expectedModel) ||
      (!requestedPlan.empty() && requestedPlan != expectedPlan) ||
      (!requestedBoot.empty() && requestedBoot != expectedBoot) ||
      binding.securityEpoch != expectedSecurityEpoch) {
    throw std::invalid_argument("KV_BINDING_MISMATCH");
  }
  binding.modelDigest = expectedModel;
  binding.planDigest = expectedPlan;
  binding.providerBootId = expectedBoot;
  binding.validate();
  return binding;
}

void
injectCachedKvInputs(std::map<std::string, TensorBundle>& inputs,
                     const NativeModelRunnerSpec& spec,
                     const TensorBundle& cached)
{
  const auto mapping = nativeProviderFieldValue(spec.metadata, {"kvTensorMap"});
  if (mapping.empty() || !isEncodedTensorBundle(cached.payload)) {
    throw std::invalid_argument("KV_STATE_UNAVAILABLE");
  }
  const auto tensors = decodeTensorBundle(cached.payload);
  std::size_t start = 0;
  while (start < mapping.size()) {
    const auto end = mapping.find(',', start);
    const auto item = mapping.substr(
      start, (end == std::string::npos ? mapping.size() : end) - start);
    const auto equals = item.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 == item.size()) {
      throw std::invalid_argument("KV_BINDING_MISMATCH");
    }
    const auto inputName = item.substr(0, equals);
    auto tensor = findTensor(tensors, item.substr(equals + 1));
    tensor.name = inputName;
    inputs[inputName] = makeEncodedTensorBundle(inputName, {std::move(tensor)});
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

std::optional<std::vector<uint8_t>>
executeLocalPlanAndFinalPayload(NativeProviderHandlerState& state,
                                const NativeProviderHandlerConfig& config,
                                const NativeExecutionPlan& plan,
                                const std::string& sessionId,
                                const std::optional<ExecutionAttemptKey>& executionAttempt,
                                const NativeProviderAssignment& assignment,
                                const std::string& localProvider,
                                const std::map<std::string, TensorBundle>& initialInputs,
                                std::chrono::steady_clock::time_point submittedSteady,
                                long long submittedEpoch,
                                ProviderRoleWorker::NativeRunnerPreparation prepareRunner = {},
                                RoleExecutionContext::StreamEventSink eventSink = {},
                                std::function<void()> executionGuard = {},
                                const std::optional<NativeSelectionProjectionV3>& selectionProjection = std::nullopt,
                                const std::shared_ptr<ProtectedRuntime>& protectedRuntime = {},
                                NativeProviderHandlerConfig::RunnerReusePublisher runnerReusePublisher = {})
{
  auto io = std::make_shared<LocalDependencyIo>();
  std::vector<std::pair<std::string, std::future<ProviderRoleResult>>> futures;
  futures.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
    auto roleSpec = executionAttempt
      ? roleSpecFor(plan, role, *executionAttempt, assignment, localProvider)
      : roleSpecFor(plan, role, sessionId, assignment, localProvider);
    auto roleInputs = roleSpec.inputs.empty()
      ? initialInputs : std::map<std::string, TensorBundle>{};
    if (prepareRunner) {
      if (plan.roles.size() != 1) {
        throw std::runtime_error(
          "post-Selection runner preparation requires one local role");
      }
      futures.emplace_back(
        role,
        state.runtime.executePreparedRoleAsync(
          sessionId, roleSpec, io, prepareRunner, std::move(roleInputs),
          roleSpec.outputs.empty() ? eventSink : RoleExecutionContext::StreamEventSink{},
          executionGuard));
    }
    else {
      futures.emplace_back(
        role,
        state.runtime.executeRoleAsync(
          sessionId, roleSpec, io, std::move(roleInputs),
          roleSpec.outputs.empty() ? eventSink : RoleExecutionContext::StreamEventSink{}));
    }
  }

  std::optional<std::vector<uint8_t>> finalPayload;
  for (auto& item : futures) {
    auto roleSpec = executionAttempt
      ? roleSpecFor(plan, item.first, *executionAttempt, assignment, localProvider)
      : roleSpecFor(plan, item.first, sessionId, assignment, localProvider);
    auto result = item.second.get();
    if (result.runner && selectionProjection && runnerReusePublisher) {
      runnerReusePublisher(*selectionProjection, protectedRuntime, result.runner);
    }
    if (result.executionEvidence && config.executionEvidenceObserver &&
        *config.executionEvidenceObserver) {
      (*config.executionEvidenceObserver)(*result.executionEvidence);
    }
    logProviderTiming(sessionId,
                      item.first,
                      result,
                      submittedSteady,
                      submittedEpoch);
    auto payload = nativeProviderFinalResponsePayload(
      roleSpec,
      result,
      config.finalResponseScope);
    if (payload) {
      finalPayload = std::move(payload);
    }
  }
  return finalPayload;
}

} // namespace

NativeProviderTimeoutBudget
nativeProviderTimeoutBudget(const NativeProviderHandlerConfig& config)
{
  return makeNativeProviderTimeoutBudget(config);
}

std::optional<std::vector<uint8_t>>
nativeProviderFinalResponsePayload(const RoleSpec& roleSpec,
                                   const ProviderRoleResult& result,
                                   const std::string& finalResponseScope)
{
  if (!roleSpec.outputs.empty() || finalResponseScope.empty()) {
    return std::nullopt;
  }

  const auto found = result.outputsByScope.find(finalResponseScope);
  if (found != result.outputsByScope.end()) {
    return found->second.payload;
  }
  return std::nullopt;
}

bool
nativeProviderShouldExecuteLocalPlan(const NativeExecutionPlan& plan,
                                     const NativeProviderAssignment& assignment,
                                     const RoleSpec& currentRole,
                                     const std::string& localProvider)
{
  // An intermediate role (for example /Backbone) normally has outputs.  Its
  // output edges describe the dataflow graph; they must not force the handler
  // back into the one-role network path when every role is assigned locally.
  if (currentRole.role.empty()) {
    return false;
  }
  return allPlanRolesAssignedToLocal(plan, assignment, localProvider);
}

void
validateNativeProviderExecutionPolicy(
  const NativeProviderHandlerConfig& config)
{
  if (config.plan.executionPolicy != config.executionPolicy) {
    throw std::invalid_argument(
      "native Provider execution policy is not plan-bound");
  }
  if (config.executionPolicy == "DATA_DRIVEN_V2") {
    if (config.requireExecutionActivation ||
        config.allowLegacyPeerReadinessBarrier) {
      throw std::invalid_argument(
        "DATA_DRIVEN_V2 rejects legacy execution activation/barrier flags");
    }
    return;
  }
  if (config.executionPolicy == "LEGACY_READY_SET_V1") {
    if (!config.requireExecutionActivation ||
        !config.allowLegacyPeerReadinessBarrier) {
      throw std::invalid_argument(
        "LEGACY_READY_SET_V1 requires explicit activation and readiness barrier");
    }
    return;
  }
  throw std::invalid_argument("unsupported NDNSF-DI execution policy");
}

std::optional<std::string>
validateNativeProviderRuntimeReadiness(
  const ExecutionEvidence& evidence,
  const std::string& expectedRole,
  const std::string& expectedBackend,
  const std::string& expectedDevice,
  const std::string& expectedArtifactDigest)
{
  if (expectedRole.empty() || expectedBackend.empty() ||
      expectedDevice.empty() || expectedArtifactDigest.empty()) {
    return "DI_RUNTIME_ASSIGNMENT_INCOMPLETE";
  }
  if (evidence.runnerKind == RunnerKind::NativeYoloPostprocess) {
    // NATIVE_POSTPROCESS is a declared CPU dependency consumer, not a CPU
    // model execution. Keep this branch explicit so a CPU ORT model-layer
    // fallback cannot satisfy the Merge readiness contract.
    const bool nativeMergeRequestsCpu = expectedBackend == "cpu" ||
      (expectedBackend.size() > 4 &&
       expectedBackend.compare(expectedBackend.size() - 4, 4, "-cpu") == 0);
    const bool nativeMergeUsesCpuDevice = expectedDevice == "cpu" ||
      (expectedDevice.size() > 4 &&
       expectedDevice.compare(0, 4, "cpu:") == 0 &&
       std::all_of(expectedDevice.begin() + 4, expectedDevice.end(),
                   [] (unsigned char ch) { return std::isdigit(ch) != 0; }));
    if (!nativeMergeRequestsCpu || !nativeMergeUsesCpuDevice) {
      logRuntimeWarn(
        "NDNSF_DI_NATIVE_MERGE_ASSIGNMENT_DIAG expected_backend=" +
        expectedBackend + " expected_device=" + expectedDevice);
      return "DI_RUNTIME_NATIVE_MERGE_ASSIGNMENT_MISMATCH";
    }
    try {
      evidence.validate();
    }
    catch (const std::exception&) {
      return "DI_RUNTIME_EVIDENCE_INVALID";
    }
    if (evidence.realCompute || evidence.cpuFallbackUsed ||
        evidence.loadCompleted || evidence.warmupCompleted ||
        evidence.deviceKind != "cpu") {
      return "DI_RUNTIME_NATIVE_MERGE_MODEL_EXECUTION";
    }
    if (std::find(evidence.roles.begin(), evidence.roles.end(), expectedRole) ==
        evidence.roles.end()) {
      return "DI_RUNTIME_ROLE_MISMATCH";
    }
    const auto artifact = evidence.artifactDigests.find(expectedRole);
    if (artifact == evidence.artifactDigests.end() ||
        !nativeProviderDigestEquals(artifact->second, expectedArtifactDigest)) {
      return "DI_RUNTIME_ARTIFACT_MISMATCH";
    }
    return std::nullopt;
  }
  const bool deviceRequestsCuda = expectedDevice.rfind("cuda:", 0) == 0;
  const bool backendRequestsCuda = expectedBackend == "onnxruntime-cuda";
  const bool backendRequestsCpu = expectedBackend == "cpu" ||
                                  expectedBackend == "onnxruntime-cpu";
  const bool backendIsGenericOnnx = expectedBackend == "onnxruntime";
  if (!backendRequestsCuda && !backendRequestsCpu && !backendIsGenericOnnx) {
    return "DI_RUNTIME_BACKEND_MISMATCH";
  }
  const bool expectsCuda = backendRequestsCuda ||
                           (backendIsGenericOnnx && deviceRequestsCuda);
  try {
    evidence.validate();
  }
  catch (const std::exception&) {
    return "DI_RUNTIME_EVIDENCE_INVALID";
  }
  if (expectsCuda) {
    if (!evidence.realCompute ||
        evidence.runnerKind != RunnerKind::OnnxRuntimeCuda ||
        evidence.deviceKind != "cuda" || evidence.cpuFallbackUsed) {
      return "DI_RUNTIME_CUDA_REQUIRED";
    }
  }
  else {
    // CPU/no-GPU is a first-class V3 execution mode. It must still be a real
    // ONNX Runtime CPU execution and may not be advertised as a silent
    // CUDA-to-CPU fallback.
    if (evidence.cpuFallbackUsed) {
      return "DI_RUNTIME_CPU_FALLBACK_USED";
    }
    if (!evidence.realCompute ||
        evidence.runnerKind != RunnerKind::OnnxRuntimeCpu ||
        evidence.deviceKind != "cpu") {
      return "DI_RUNTIME_CPU_REQUIRED";
    }
  }
  if (!evidence.loadCompleted) {
    return "DI_RUNTIME_MODEL_NOT_LOADED";
  }
  if (!evidence.warmupCompleted) {
    return "DI_RUNTIME_WARMUP_INCOMPLETE";
  }
  if (std::find(evidence.roles.begin(), evidence.roles.end(), expectedRole) ==
      evidence.roles.end()) {
    return "DI_RUNTIME_ROLE_MISMATCH";
  }
  if (expectsCuda) {
    auto expectedDeviceId = expectedDevice;
    if (expectedDeviceId.rfind("cuda:", 0) == 0) {
      expectedDeviceId = expectedDeviceId.substr(5);
    }
    if (expectedDevice.rfind("cuda:", 0) != 0 ||
        expectedDeviceId.empty() || evidence.deviceId != expectedDeviceId) {
      return "DI_RUNTIME_DEVICE_MISMATCH";
    }
  }
  else if (expectedDevice != "cpu") {
    auto expectedDeviceId = expectedDevice;
    if (expectedDeviceId.rfind("cpu:", 0) == 0) {
      expectedDeviceId = expectedDeviceId.substr(4);
    }
    if (expectedDeviceId.empty() ||
        (evidence.deviceId != expectedDeviceId &&
         evidence.deviceId != "cpu" + expectedDeviceId)) {
      return "DI_RUNTIME_DEVICE_MISMATCH";
    }
  }
  const auto artifact = evidence.artifactDigests.find(expectedRole);
  if (artifact == evidence.artifactDigests.end() ||
      !nativeProviderDigestEquals(artifact->second, expectedArtifactDigest)) {
    return "DI_RUNTIME_ARTIFACT_MISMATCH";
  }
  return std::nullopt;
}

std::optional<std::string>
validateNativePreparedRunnerSpec(
  const NativeSelectionProjectionV3& projection,
  const NativeModelRunnerSpec& spec)
{
  const auto& assembly = projection.assembly;
  if (assembly.mergeKind == "NATIVE_POSTPROCESS") {
    if (assembly.expectedOutputs.size() != 1 ||
        assembly.expectedOutputs.front().name != assembly.postprocessOutputName ||
        assembly.expectedOutputs.front().dtype != "float32") {
      return "DI_PROVIDER_NATIVE_MERGE_METADATA_MISMATCH";
    }
    std::string expectedOutputShape;
    for (const auto& dimension : assembly.expectedOutputs.front().shape) {
      if (!expectedOutputShape.empty()) expectedOutputShape += ',';
      expectedOutputShape += std::holds_alternative<std::int64_t>(dimension)
        ? std::to_string(std::get<std::int64_t>(dimension)) : std::get<std::string>(dimension);
    }
    if (spec.role != assembly.selectedRole ||
        spec.kind != "native-yolo-postprocess" ||
        spec.backend != "native-yolo-postprocess" || !spec.path.empty()) {
      return "DI_PROVIDER_NATIVE_MERGE_RUNNER_MISMATCH";
    }
    const auto matches = [&spec] (
      std::initializer_list<const char*> keys, const std::string& expected) {
      const auto actual = metadataValue(spec, keys);
      return !actual.empty() && actual == expected;
    };
    if (!matches({"fragmentDigest", "fragment_digest"}, assembly.artifactDigest) ||
        !matches({"recipeDigest", "recipe_digest"}, assembly.recipeDigest) ||
        !matches({"mergeKind", "merge_kind"}, assembly.mergeKind) ||
        !matches({"postprocessIdentity", "postprocess_identity"},
                 assembly.postprocessIdentity) ||
        !matches({"postprocessOutputName", "postprocess_output_name"},
                 assembly.postprocessOutputName) ||
        !matches({"postprocessSort", "postprocess_sort"}, assembly.postprocessSort) ||
        !matches({"expectedOutputShape"}, expectedOutputShape) ||
        !matches({"postprocessConfidenceThreshold",
                  "postprocess_confidence_threshold"},
                 std::to_string(assembly.postprocessConfidenceThreshold))) {
      return "DI_PROVIDER_NATIVE_MERGE_METADATA_MISMATCH";
    }
    return std::nullopt;
  }
  if (projection.canonicalArtifactName.empty()) {
    return "DI_PROVIDER_ASSEMBLY_ROOT_MISSING";
  }
  if (assembly.modelManifestDigest.empty() ||
      assembly.artifactProfileDigest.empty() || assembly.graphDigest.empty() ||
      assembly.canonicalInitializerDigest.empty() ||
      assembly.adapterDescriptorDigest.empty() ||
      assembly.assemblerDescriptorDigest.empty() || assembly.backendAbi.empty() ||
      assembly.nodeIndices.empty() || assembly.expectedInputs.empty() ||
      assembly.expectedOutputs.empty() || assembly.precision.empty() ||
      assembly.quantization.empty() || assembly.layout.empty() ||
      assembly.padding.empty() || assembly.maxSourceBytes == 0 ||
      assembly.maxAssembledBytes == 0 || assembly.maxNodes == 0) {
    return "DI_PROVIDER_ASSEMBLY_IDENTITY_INCOMPLETE";
  }
  if (spec.role != assembly.selectedRole || spec.backend != assembly.backend ||
      spec.path.empty()) {
    return "DI_PROVIDER_ASSEMBLY_RUNNER_MISMATCH";
  }
  const std::filesystem::path modelPath(spec.path);
  if (!modelPath.is_absolute() || modelPath.filename() != "model.onnx") {
    return "DI_PROVIDER_ASSEMBLY_PATH_UNSAFE";
  }
  for (const auto& part : modelPath) {
    if (part == "..") {
      return "DI_PROVIDER_ASSEMBLY_PATH_UNSAFE";
    }
  }
  const auto matches = [&spec] (
    std::initializer_list<const char*> keys, const std::string& expected) {
    const auto actual = metadataValue(spec, keys);
    return !actual.empty() && actual == expected;
  };
  if (!matches({"fragmentDigest", "fragment_digest"},
               assembly.artifactDigest) ||
      !matches({"recipeDigest", "recipe_digest"}, assembly.recipeDigest) ||
      !matches({"modelManifestDigest", "model_manifest_digest"},
               assembly.modelManifestDigest) ||
      !matches({"artifactProfileDigest", "artifact_profile_digest"},
               assembly.artifactProfileDigest) ||
      !matches({"graphDigest", "graph_digest"}, assembly.graphDigest) ||
      !matches({"canonicalInitializerDigest", "canonical_initializer_digest"},
               assembly.canonicalInitializerDigest) ||
      !matches({"adapterDescriptorDigest", "adapter_descriptor_digest"},
               assembly.adapterDescriptorDigest) ||
      !matches({"assemblerDescriptorDigest", "assembler_descriptor_digest"},
               assembly.assemblerDescriptorDigest) ||
      !matches({"backendAbi", "backend_abi"}, assembly.backendAbi) ||
      !matches({"precision"}, assembly.precision) ||
      !matches({"quantization"}, assembly.quantization) ||
      !matches({"layout"}, assembly.layout) ||
      !matches({"padding"}, assembly.padding) ||
      !matches({"maxSourceBytes", "max_source_bytes"},
               std::to_string(assembly.maxSourceBytes)) ||
      !matches({"maxAssembledBytes", "max_assembled_bytes"},
               std::to_string(assembly.maxAssembledBytes)) ||
      !matches({"maxNodes", "max_nodes"},
               std::to_string(assembly.maxNodes))) {
    return "DI_PROVIDER_ASSEMBLY_METADATA_MISMATCH";
  }
  return std::nullopt;
}

std::string
digestIdentityFields(const std::string& domain,
                     const std::vector<std::string>& fields)
{
  std::ostringstream canonical;
  canonical << domain;
  for (const auto& field : fields) {
    canonical << "\n" << field.size() << ":" << field;
  }
  const auto value = canonical.str();
  return sha256TensorBytes(
    std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::string
stateTensorContractIdentity(const NativeSelectionRoleV3& assembly,
                            const std::string& name,
                            bool input)
{
  const auto& contracts = input ? assembly.expectedInputs : assembly.expectedOutputs;
  const auto found = std::find_if(contracts.begin(), contracts.end(), [&] (const auto& item) {
    return item.name == name;
  });
  if (found == contracts.end() || found->dtype.empty() || found->shape.empty()) {
    throw std::invalid_argument(
      "DI_PROVIDER_DECODE_STATE_CONTRACT_INCOMPLETE");
  }
  std::ostringstream shape;
  for (const auto& dimension : found->shape) {
    const bool numeric = std::holds_alternative<std::int64_t>(dimension);
    const auto text = numeric ? std::to_string(std::get<std::int64_t>(dimension))
                              : std::get<std::string>(dimension);
    shape << (numeric ? 'i' : 's') << text.size() << ":" << text << ";";
  }
  return name + "|" + found->dtype + "|" + shape.str();
}

struct TrustedDecodeStateTemplate
{
  DecodeStateIdentityV1 identity;
  std::string positionPolicyDigest;
};

TrustedDecodeStateTemplate
trustedDecodeStateTemplateFor(
  const NativeSelectionProjectionV3& projection,
  const NativeModelRunnerSpec* runnerSpec,
  const std::string& providerBootId)
{
  if (runnerSpec != nullptr) {
    if (const auto error = validateNativePreparedRunnerSpec(projection, *runnerSpec)) {
      throw std::invalid_argument(*error);
    }
  }
  const auto& assembly = projection.assembly;
  const auto& generation = projection.generationContract;
  if (!generation.enabled || generation.tokenizerDigest.empty() ||
      providerBootId.empty() || projection.provider.empty() ||
      projection.requestId.empty() || projection.attempt == 0 ||
      projection.executionRole.layerBegin >
        std::numeric_limits<std::uint32_t>::max() ||
      projection.executionRole.layerEnd >
        std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(
      "DI_PROVIDER_DECODE_STATE_AUTHORITY_INCOMPLETE");
  }
  const auto localState = localGenerationStateNames(projection);

  std::vector<std::string> stateContracts;
  std::vector<std::string> stateComponentDigests;
  stateContracts.reserve(localState.inputs.size() * 2);
  stateComponentDigests.reserve(localState.inputs.size());
  for (std::size_t index = 0; index < localState.inputs.size(); ++index) {
    const auto input = stateTensorContractIdentity(
      assembly, localState.inputs[index], true);
    const auto output = stateTensorContractIdentity(
      assembly, localState.outputs[index], false);
    stateContracts.push_back(input);
    stateContracts.push_back(output);
    stateComponentDigests.push_back(digestIdentityFields(
      "NDNSF-DI-STATE-COMPONENT-V1", {input, output}));
  }

  TrustedDecodeStateTemplate result;
  auto& identity = result.identity;
  identity.modelDigest = assembly.modelManifestDigest;
  identity.graphSemanticDigest = assembly.graphDigest;
  identity.artifactDigest = assembly.artifactDigest;
  identity.adapterDigest = assembly.adapterDescriptorDigest;
  identity.tokenizerDigest = generation.tokenizerDigest;
  identity.runnerDigest = digestIdentityFields(
    "NDNSF-DI-RUNNER-V1",
    {assembly.assemblerDescriptorDigest, assembly.canonicalInitializerDigest,
     assembly.backend, assembly.backendAbi, assembly.artifactDigest});
  identity.roleName = projection.executionRole.roleId;
  identity.roleSplitDigest = assembly.recipeDigest;
  identity.layerBegin = static_cast<std::uint32_t>(
    projection.executionRole.layerBegin);
  identity.layerEnd = static_cast<std::uint32_t>(
    projection.executionRole.layerEnd);
  // Prefix/count/position are intentionally unbound in this static template.
  // NativeEpochCoordinator derives all three from admitted token lineage.
  identity.prefixDigest.clear();
  identity.prefixTokenCount = 0;
  identity.positionDigest.clear();
  identity.precision = assembly.precision;
  identity.layoutDigest = assembly.artifactProfileDigest;
  identity.stateSchemaDigest = digestIdentityFields(
    "NDNSF-DI-STATE-SCHEMA-V1", stateContracts);
  identity.stateComponentDigests = std::move(stateComponentDigests);
  identity.runtimeAbiDigest = digestIdentityFields(
    "NDNSF-DI-RUNTIME-ABI-V1", {assembly.backend, assembly.backendAbi});
  identity.securityDomainDigest = projection.securityPolicySnapshotDigest;
  identity.providerIdentity = projection.provider;
  identity.providerBootId = providerBootId;
  identity.stateInferenceEpoch = 0;
  identity.predecessorInferenceEpoch = std::nullopt;
  identity.cacheEpoch = 1;
  identity.requestId = projection.requestId;
  identity.attemptEpoch = projection.attempt;
  identity.generationId = generation.generationId.empty()
    ? projection.requestId + ":generation:" +
      std::to_string(projection.attempt)
    : generation.generationId;
  result.positionPolicyDigest = digestIdentityFields(
    "NDNSF-DI-POSITION-POLICY-V1",
    {assembly.adapterDescriptorDigest, generation.tokenInputName,
     assembly.graphDigest});
  return result;
}

ProtectedResidentIdentityV1
protectedResidentIdentityFor(const NativeSelectionProjectionV3& projection,
                             const ProtectedRuntime& runtime,
                             const std::string& providerBootId)
{
  ProtectedResidentIdentityV1 identity;
  identity.provider = projection.provider;
  identity.providerBootId = providerBootId;
  identity.role = projection.executionRole.roleId;
  identity.modelManifestDigest = projection.assembly.modelManifestDigest;
  identity.graphDigest = projection.assembly.graphDigest;
  identity.initializerDigest = projection.assembly.canonicalInitializerDigest;
  identity.artifactDigest = projection.assembly.artifactDigest;
  identity.recipeDigest = projection.assembly.recipeDigest;
  identity.backend = projection.assembly.backend;
  identity.backendAbi = projection.assembly.backendAbi;
  identity.protectionEpoch = projection.selectedRole.protectionEpoch;
  identity.planCoreDigest = projection.planCoreDigest;
  identity.planDigest = projection.planDigest;
  identity.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
  identity.grantDigest = projection.grantDigest;
  identity.fencingToken = runtime.binding().fencingToken;
  identity.revocationSequence = runtime.binding().revocationSequence;
  return identity;
}

std::optional<std::string>
validateProtectedRuntimeBinding(
  const NativeSelectionProjectionV3& projection,
  const ProtectedRuntime& runtime,
  const std::shared_ptr<ProviderGroupCoordinator>& groupCoordinator,
  const std::string& expectedProviderBootId,
  const std::string& expectedFencingToken)
{
  const auto& binding = runtime.binding();
  std::set<std::string> mayPublish;
  std::set<std::string> mustFetch;
  std::map<std::string, std::string> mayPublishConsumers;
  std::map<std::string, std::string> mustFetchProducers;
  for (const auto& endpoint : projection.dataflow.mayPublish) {
    mayPublish.insert(endpoint.endpointDigest);
    mayPublishConsumers[endpoint.endpointDigest] = endpoint.consumerRole;
  }
  for (const auto& endpoint : projection.dataflow.mustFetch) {
    // APPLICATION_INPUT is request-backed and has no Provider producer.  It
    // is authenticated and pre-satisfied by initialInputsFromRequest at the
    // selected ingress role, so it is not a protected inter-role dataflow
    // endpoint and must not be converted into an empty producer binding.
    if (endpoint.sourceKind == "APPLICATION_INPUT" ||
        endpoint.operation == "APPLICATION_INPUT") {
      continue;
    }
    mustFetch.insert(endpoint.endpointDigest);
    mustFetchProducers[endpoint.endpointDigest] = endpoint.producerRole;
  }
  if (!projection.hasGrantBinding || expectedProviderBootId.empty() ||
      expectedFencingToken.empty() || binding.provider != projection.provider ||
      binding.role != projection.executionRole.roleId ||
      binding.requestId != projection.requestId ||
      binding.attempt != projection.attempt ||
      binding.planCoreDigest != projection.planCoreDigest ||
      binding.planDigest != projection.planDigest ||
      binding.securityPolicySnapshotDigest !=
        projection.securityPolicySnapshotDigest ||
      binding.protectionEpoch != projection.selectedRole.protectionEpoch ||
      binding.grantName != projection.grantName ||
      binding.grantDigest != projection.grantDigest ||
      binding.providerBootId != expectedProviderBootId ||
      binding.fencingToken != expectedFencingToken ||
      binding.mayPublishEndpointDigests != mayPublish ||
      binding.mustFetchEndpointDigests != mustFetch ||
      binding.mayPublishConsumerByEndpoint != mayPublishConsumers ||
      binding.mustFetchProducerByEndpoint != mustFetchProducers) {
    return "DI_PROTECTED_RUNTIME_BINDING_MISMATCH";
  }
  if (groupCoordinator && groupCoordinator->hasCapability()) {
    const auto& capability = groupCoordinator->capability();
    if (binding.capabilityDigest != capability.capabilityDigest ||
        binding.groupId != capability.groupId ||
        binding.groupEpoch != capability.epoch ||
        binding.epochKeyId != capability.epochKeyId ||
        capability.requestId != projection.requestId ||
        capability.attemptId !=
          "attempt-" + std::to_string(projection.attempt) ||
        capability.planDigest != projection.planDigest) {
      return "DI_PROTECTED_CAPABILITY_BINDING_MISMATCH";
    }
  }
  else if (!binding.capabilityDigest.empty() || !binding.groupId.empty() ||
           binding.groupEpoch != 0 || !binding.epochKeyId.empty()) {
    return "DI_PROTECTED_CAPABILITY_BINDING_MISMATCH";
  }
  return std::nullopt;
}

struct NativeAuthenticatedGenerationConfig
{
  bool enabled = false;
  std::size_t maxEpochs = 0;
  std::string tokenInputName;
  std::vector<std::string> stateInputNames;
  std::vector<std::string> stateOutputNames;
  std::set<std::int64_t> eosTokenIds;
  std::string samplingDigest;
  std::string samplingMode = "Greedy";
  double samplingTemperature = 0.0;
  std::size_t samplingTopK = 1;
  double samplingTopP = 1.0;
  double samplingRepetitionPenalty = 1.0;
  std::uint64_t samplingSeed = 1'750'001;
  std::vector<std::string> stopStrings;
  std::function<std::string(const std::vector<std::int64_t>&)> textDecoder;
  std::function<std::string(const std::vector<std::int64_t>&, bool)> stableTextDecoder;
  NativeProviderHandlerConfig::GenerationTextDecoderFactory textDecoderFactory;
  NativeProviderHandlerConfig::GenerationDecodersFactory textDecodersFactory;
  bool requireTextOutput = false;
  std::vector<std::int64_t> committedPrefixTokenIds;
};

NativeAuthenticatedGenerationConfig
generationConfigFromAuthenticatedRequest(
  const NativeProviderHandlerConfig& base,
  const std::optional<NativeSelectionProjectionV3>& projection)
{
  NativeAuthenticatedGenerationConfig result{
    base.enableNativeEpochCoordinator,
    base.maxGenerationEpochs,
    base.generationTokenInputName,
    base.generationStateInputNames,
    base.generationStateOutputNames,
    base.generationEosTokenIds,
    base.generationSamplingDigest,
    base.generationSamplingMode,
    base.generationSamplingTemperature,
    base.generationSamplingTopK,
    base.generationSamplingTopP,
    base.generationSamplingRepetitionPenalty,
    base.generationSamplingSeed,
    base.generationStopStrings,
    base.generationTextDecoder,
    {},
    base.generationTextDecoderFactory,
    base.generationDecodersFactory,
    base.requireGenerationTextOutput,
    base.generationCommittedPrefixTokenIds,
  };
  if (projection && projection->generationContract.enabled) {
    const auto& sealed = projection->generationContract;
    const auto localState = localGenerationStateNames(*projection);
    result.enabled = true;
    result.maxEpochs = sealed.maxGeneratedTokens;
    result.tokenInputName = sealed.tokenInputName;
    result.stateInputNames = localState.inputs;
    result.stateOutputNames = localState.outputs;
    result.eosTokenIds = std::set<std::int64_t>(
      sealed.eosTokenIds.begin(), sealed.eosTokenIds.end());
    result.samplingDigest = sealed.samplingDigest;
    result.samplingMode = sealed.samplingMode;
    result.samplingTemperature = sealed.samplingTemperature;
    result.samplingTopK = static_cast<std::size_t>(sealed.samplingTopK);
    result.samplingTopP = sealed.samplingTopP;
    result.samplingRepetitionPenalty = sealed.samplingRepetitionPenalty;
    result.samplingSeed = sealed.samplingSeed;
    result.stopStrings = sealed.stopStrings;
    if (result.textDecodersFactory) {
      const auto decoders = result.textDecodersFactory(sealed.tokenizerDigest);
      result.textDecoder = decoders.full;
      result.stableTextDecoder = decoders.stable;
    }
    else if (result.textDecoderFactory) {
      if (result.requireTextOutput)
        throw std::invalid_argument("streamed generation requires paired full/stable token decoders");
      result.textDecoder = result.textDecoderFactory(sealed.tokenizerDigest);
    }
    result.committedPrefixTokenIds = sealed.committedPrefixTokenIds;
  }
  return result;
}

NativeProviderCollaborationRuntime
makeNativeProviderCollaborationRuntime(NativeProviderHandlerConfig config)
{
  validateNativeProviderExecutionPolicy(config);
  if (!config.runnerFactory) {
    throw std::invalid_argument(
      "NativeProviderHandlerConfig requires NativeModelRunnerFactory");
  }
  auto state = std::make_shared<NativeProviderHandlerState>(config);

  NativeProviderCollaborationRuntime runtime;
  runtime.capacitySnapshot = [state] {
    return state->runtime.snapshot();
  };
  runtime.decodeStateSnapshot = [state] {
    return state->runtime.decodeStateSnapshot();
  };
  runtime.conversationStateSnapshot = [state] {
    return state->runtime.conversationStateSnapshot();
  };
  runtime.executionEvidence = state->executionEvidence;
  runtime.handler = [config = std::move(config), state = std::move(state)] (
	           ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
	           const ndn_service_framework::RequestMessage& request) mutable {
    const auto timeoutBudget = nativeProviderTimeoutBudget(config);
    std::string activatedLeaseId;
    std::string activatedProviderEpoch;
    std::string activatedRequester;
    std::string activatedRole;
    std::size_t expectedProviderRoles = 1;
    bool completedLocalPlan = false;
    bool executionStopped = false;
    bool selectionObserved = false;
    std::string stageRequestId;
    std::string stagePlanDigest;
    std::string stageAttemptEpoch;
    std::shared_ptr<ProtectedRuntime> protectedRuntime;
    std::function<void()> waitConversationPromotion;
    std::function<void()> rollbackConversationPromotion;
    bool conversationPromotionStaged = false;
    bool conversationPromotionCommitted = false;
    auto completeExecutionLease = [&] {
      state->completeExecutionLease(config.executionLeaseTable,
                                    activatedLeaseId,
                                    activatedProviderEpoch,
                                    activatedRequester,
                                    activatedRole,
                                    expectedProviderRoles,
                                    completedLocalPlan);
      activatedLeaseId.clear();
    };
    try {
      // The request payload is normally the authenticated generic request
      // envelope (JSON ndnsf-di-request-envelope-v2).  It is not an
      // execution-control or Selection-assignment payload.  The old control
      // probe reused the assignment parser, which treats every JSON object as
      // a V3 Selection projection and consequently rejected the real request
      // before the authenticated Selection projection was examined below.
      // Execution-control messages remain the legacy semicolon field format;
      // keep that probe scoped to non-JSON payloads and leave all JSON
      // assignment validation fail-closed at the Selection boundary.
      const auto requestPayloadText = std::string(
        reinterpret_cast<const char*>(request.getPayload().data()),
        request.getPayload().size());
      const auto requestPayloadFirst = requestPayloadText.find_first_not_of(
        " \t\r\n");
      const auto controlFields =
        (requestPayloadFirst != std::string::npos &&
         requestPayloadText[requestPayloadFirst] == '{')
          ? std::map<std::string, std::string>{}
          : parseNativeProviderAssignmentFields(request.getPayload());
      const auto control = applyNativeProviderExecutionControl(
        controlFields, state->attemptAuthority);
      if (control.recognized) {
        std::ostringstream record;
        record << "NDNSF_DI_EXECUTION_ATTEMPT"
               << " decision=" << (control.status ? "control-applied" : "control-rejected")
               << " reason=" << control.reason
               << " requestId=" << control.attempt.requestId
               << " attemptEpoch=" << control.attempt.attemptEpoch;
        logRuntimeInfo(record.str());
        const auto response = std::string("schema=ndnsf-di-execution-control-v2;") +
          "status=" + (control.status ? "1;" : "0;") +
          "reason=" + control.reason + ";";
        ctx.publishFinalResponse(ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(response.data()), response.size()));
        return;
      }
      auto assignment = state->baseAssignment;
      for (const auto& item : ctx.assignment().roleProviders) {
        assignment.providerByRole[item.first] = item.second.toUri();
      }
      if (!ctx.role().empty()) {
        assignment.providerByRole[ctx.role()] = ctx.localProvider().toUri();
      }

      const auto role = ctx.role();
      const auto assignmentFields = parseNativeProviderAssignmentFields(
        ctx.assignment().assignmentPayload, role);
      std::optional<NativeSelectionProjectionV3> selectionProjection;
      const std::string assignmentText(
        reinterpret_cast<const char*>(ctx.assignment().assignmentPayload.data()),
        ctx.assignment().assignmentPayload.size());
      const auto assignmentFirst = assignmentText.find_first_not_of(" \t\r\n");
      if (assignmentFirst != std::string::npos &&
          assignmentText[assignmentFirst] == '{') {
        std::istringstream projectionInput(assignmentText);
        selectionProjection = nativeSelectionProjectionV3FromJson(
          projectionInput, role);
        if (selectionProjection->provider != ctx.localProvider().toUri()) {
          ctx.fail("DI_SELECTION_PROVIDER_MISMATCH");
          return;
        }
        if (selectionProjection->requestId != ctx.sessionId()) {
          ctx.fail("DI_SELECTION_REQUEST_MISMATCH");
          return;
        }
        if (!selectionProjection->requestContractDigest.empty()) {
          if (!nativeRequestContractDigestMatches(
                selectionProjection->requestContractDigest,
                request.getPayload())) {
            ctx.fail("DI_SELECTION_REQUEST_CONTRACT_MISMATCH");
            return;
          }
        }
        if (selectionProjection->generationContract.enabled &&
            !selectionProjection->generationContract.generationId.empty() &&
            (!request.hasStreamRequestOptions() ||
             !nativeGenerationIdMatchesStreamOptions(
               selectionProjection->generationContract.generationId,
               request.getStreamRequestOptions().generationId))) {
          ctx.fail("DI_SELECTION_GENERATION_MISMATCH");
          return;
        }
        if (!config.runnerPreparationFactory &&
            (!config.allowPreassembledV3Compatibility ||
             selectionProjection->selectedRole.protectionEpoch != "plaintext-v1")) {
          ctx.fail("DI_PROVIDER_ASSEMBLY_FACTORY_MISSING");
          return;
        }
        if (config.runnerPreparationFactory &&
            ctx.assignment().assignedArtifact.empty()) {
          ctx.fail("DI_PROVIDER_ASSEMBLY_ROOT_MISSING");
          return;
        }
        if (!ctx.assignment().assignedArtifact.empty()) {
          // The root comes from the authenticated assignment envelope, never
          // from the opaque projection JSON.  Both the normal post-Selection
          // preparation path and the explicit preassembled compatibility path
          // need the same binding because decode-state authority validation is
          // shared by both paths.
          selectionProjection->canonicalArtifactName =
            ctx.assignment().assignedArtifact.toUri();
        }
        if (config.runnerPreparationFactory) {
          // The counter belongs to this authenticated Selection, not to one
          // assembler instance. Cache eviction and runner retries must keep
          // the signed operation stream strictly monotonic.
          selectionProjection->assemblyProgressSequence =
            std::make_shared<std::atomic<std::uint64_t>>(0);
        }
        // Model/backend allow-lists remain Provider configuration.  The
        // request-scoped roles and dependency graph come only from the sealed
        // Selection projection, as required by Placement V3.
        selectionProjection->plan.serviceName = state->plan.serviceName;
        selectionProjection->plan.modelName = state->plan.modelName;
        selectionProjection->plan.modelFamily = state->plan.modelFamily;
        selectionProjection->plan.modelFormat = state->plan.modelFormat;
        selectionProjection->plan.plannerKind = state->plan.plannerKind;
      }
      if (selectionProjection) {
        std::ostringstream record;
        record << "NDNSF_DI_NATIVE_SELECTION_ACCEPTED"
               << " requestId=" << selectionProjection->requestId
               << " attemptEpoch=" << selectionProjection->attempt
               << " epochMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count()
               << " provider=" << ctx.localProvider().toUri()
               << " role=" << role
               << " planDigest=" << selectionProjection->planDigest
               << " manifestDigest="
               << selectionProjection->selectedRole.modelManifestDigest
               << " graphDigest=" << selectionProjection->selectedRole.graphDigest
               << " initializerDigest="
               << selectionProjection->selectedRole.canonicalInitializerDigest
               << " artifactDigest="
               << selectionProjection->selectedRole.artifactDigest
               << " layerBegin=" << selectionProjection->selectedRole.layerBegin
               << " layerEnd=" << selectionProjection->selectedRole.layerEnd;
        logRuntimeEvidence(record.str());
      }
      const NativeExecutionPlan& executionPlan = selectionProjection
        ? selectionProjection->plan : state->plan;
      const auto authenticatedGeneration =
        generationConfigFromAuthenticatedRequest(config, selectionProjection);
      const std::string requestPlanDigest = selectionProjection
        ? selectionProjection->planDigest : config.planDigest;
      selectionObserved = selectionProjection.has_value();
      stageRequestId = selectionProjection
        ? selectionProjection->requestId : ctx.sessionId();
      stagePlanDigest = requestPlanDigest;
      stageAttemptEpoch = selectionProjection
        ? std::to_string(selectionProjection->attempt) : "";
      auto groupCoordinator = config.groupCoordinator;
      std::future<std::shared_ptr<ProviderGroupCoordinator>> groupCoordinatorFuture;
      const bool canOverlapGroupPreparation =
        selectionProjection &&
        selectionProjection->selectedRole.protectionEpoch != "plaintext-v1" &&
        config.overlapGroupCoordinatorFactory &&
        config.groupCoordinatorFactory &&
        config.protectedRuntimeFactory;
      if (canOverlapGroupPreparation) {
        // The production group factory only reads immutable context identity and
        // assignment fields.  Copy the fields into the worker so the expensive
        // TPM unwrap cannot hold up the grant fetch.  No execution path consumes
        // the result until the explicit get() below.
        const auto groupFactory = config.groupCoordinatorFactory;
        const auto groupFields = assignmentFields;
        logProviderBoundaryStdout("GROUP_COORDINATOR_FACTORY_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        groupCoordinatorFuture = std::async(
          std::launch::async,
          [groupFactory, &ctx, groupFields] () mutable {
            return groupFactory(ctx, groupFields);
          });
      }
      else if (config.groupCoordinatorFactory) {
        groupCoordinator = config.groupCoordinatorFactory(ctx, assignmentFields);
      }
      if (selectionProjection &&
          selectionProjection->selectedRole.protectionEpoch != "plaintext-v1") {
        if (!config.protectedRuntimeFactory) {
          ctx.fail("DI_PROTECTED_RUNTIME_FACTORY_MISSING");
          return;
        }
        logProviderBoundaryStdout("PROTECTED_RUNTIME_FACTORY_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        protectedRuntime = config.protectedRuntimeFactory(
          ctx, *selectionProjection, groupCoordinator);
        logProviderBoundaryStdout("PROTECTED_RUNTIME_FACTORY_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        if (groupCoordinatorFuture.valid()) {
          // This is the authorization join: no binding validation, assembly,
          // runner creation, or execution can proceed with a missing or failed
          // request-scoped coordinator.
          groupCoordinator = groupCoordinatorFuture.get();
          logProviderBoundaryStdout("GROUP_COORDINATOR_FACTORY_DONE", stageRequestId,
                                    ctx.localProvider().toUri(), role);
        }
        if (!protectedRuntime ||
            protectedRuntime->state() != ProtectedRuntimeState::GrantVerified) {
          // Preparation requires a newly verified grant, not a drained runtime
          // or a binding-only consistency result.
          ctx.fail("DI_PROTECTED_GRANT_UNAVAILABLE");
          return;
        }
        const auto fencingToken = nativeProtectedFencingToken(
          *selectionProjection, config.providerBootId, assignmentFields);
        logProviderBoundaryStdout("PROTECTED_BINDING_VALIDATE_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
          if (const auto error = validateProtectedRuntimeBinding(
              *selectionProjection, *protectedRuntime, groupCoordinator,
              config.providerBootId, fencingToken)) {
          logProviderStageMarker("GRANT_REJECTED", stageRequestId,
                                 ctx.localProvider().toUri(), role,
                                 requestPlanDigest, "failed", *error,
                                 stageAttemptEpoch);
          if (config.protectedResidentAuthority) {
            config.protectedResidentAuthority->retire(
              protectedResidentIdentityFor(*selectionProjection, *protectedRuntime,
                                           config.providerBootId));
          }
          protectedRuntime->cancel(*error);
          ctx.fail(*error);
          return;
        }
        logProviderBoundaryStdout("PROTECTED_BINDING_VALIDATE_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        logProviderStageMarker("GRANT_VERIFIED", stageRequestId,
                               ctx.localProvider().toUri(), role,
                               requestPlanDigest, "observed", {},
                               stageAttemptEpoch);
        logProviderBoundaryStdout("GRANT_STAGE_MARKER_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        logProviderStageMarker("POST_GRANT_CONTINUING", stageRequestId,
                               ctx.localProvider().toUri(), role,
                               requestPlanDigest, "observed", {},
                               stageAttemptEpoch);
        logProviderBoundaryStdout("POST_GRANT_CONTINUING", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        if (selectionProjection && config.runnerPreparationFactory) {
          const auto adapterIdentity = selectionProjection->assembly.backend.empty()
            ? std::string("native") : selectionProjection->assembly.backend;
          auto reportAdmission = makeNativeAssemblyProgressReporter(
            ctx, *selectionProjection, adapterIdentity, 1, 0,
            selectionProjection->assemblyProgressSequence);
          const auto& boundRole = selectionProjection->assembly.selectedRole;
          const auto operationId = ctx.assignment().selectionDigest + ":" +
            boundRole + ":assembly-progress";
          try {
            logRuntimeEvidence(
              std::string("NDNSF_DI_ASSEMBLY_ADMISSION_BEGIN provider=") +
              ctx.localProvider().toUri() + " requestId=" +
              selectionProjection->requestId + " role=" + boundRole +
              " selectionDigest=" + ctx.assignment().selectionDigest +
              " operationId=" + operationId);
            logProviderBoundaryStdout("ASSEMBLY_ADMISSION_BEGIN", stageRequestId,
                                      ctx.localProvider().toUri(), role);
            reportAdmission("ASSEMBLY_ADMISSION", 0.0);
            logProviderBoundaryStdout("ASSEMBLY_ADMISSION_DONE", stageRequestId,
                                      ctx.localProvider().toUri(), role);
            std::ostringstream record;
            record << "NDNSF_DI_ASSEMBLY_ADMISSION_REPORTED"
                   << " provider=" << ctx.localProvider().toUri()
                   << " requestId=" << selectionProjection->requestId
                   << " role=" << boundRole
                   << " selectionDigest=" << ctx.assignment().selectionDigest
                   << " operationId=" << operationId;
            logRuntimeEvidence(record.str());
          }
          catch (const std::exception& error) {
            auto errorText = std::string(error.what());
            if (errorText.size() > 512) {
              errorText.resize(512);
            }
            std::replace_if(errorText.begin(), errorText.end(),
                            [] (unsigned char value) {
                              return value < 0x20 || value == 0x7f;
                            }, ' ');
            std::ostringstream record;
            record << "NDNSF_DI_ASSEMBLY_ADMISSION_FAILED"
                   << " provider=" << ctx.localProvider().toUri()
                   << " requestId=" << selectionProjection->requestId
                   << " role=" << boundRole
                   << " selectionDigest=" << ctx.assignment().selectionDigest
                   << " operationId=" << operationId
                   << " error=\"" << errorText << "\"";
            logRuntimeError(record.str());
            throw;
          }
        }
      }
      logProviderBoundaryStdout("POST_PROTECTED_ADMISSION_DONE", stageRequestId,
                                ctx.localProvider().toUri(), role);
      std::shared_ptr<DependencyIo> io =
        std::make_shared<NdnsfCollaborationDependencyIo>(
        ctx,
        timeoutBudget.dependencyFetchMs,
        config.maxSegmentSize,
        config.freshnessMs,
        groupCoordinator,
        protectedRuntime);
      const auto assignmentExecutionPolicy = nativeProviderFieldValue(
        assignmentFields, {"executionPolicy"});
      const bool legacyPolicy =
        config.executionPolicy == "LEGACY_READY_SET_V1";
      if ((!assignmentExecutionPolicy.empty() &&
           assignmentExecutionPolicy != config.executionPolicy) ||
          (legacyPolicy && assignmentExecutionPolicy != config.executionPolicy)) {
        ctx.fail("DI_EXECUTION_POLICY_MISMATCH");
        return;
      }
      std::optional<ExecutionAttemptKey> executionAttempt;
      if (config.requireExecutionAttemptBinding) {
        logProviderBoundaryStdout("EXECUTION_BINDING_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        auto binding = validateNativeProviderExecutionBinding(
          assignmentFields,
          config.providerBootId,
          requestPlanDigest,
          state->attemptAuthority);
        if (!binding.status) {
          std::ostringstream record;
          record << "NDNSF_DI_EXECUTION_ATTEMPT"
                 << " decision=reject"
                 << " reason=" << binding.reason
                 << " role=" << role;
          logRuntimeWarn(record.str());
          ctx.fail(binding.reason);
          return;
        }
        executionAttempt = std::move(binding.attempt);
        logProviderBoundaryStdout("EXECUTION_BINDING_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
      }
      if (config.executionLeaseTable != nullptr) {
        const auto& fields = assignmentFields;
        const auto leaseId = nativeProviderFieldValue(fields, {"executionLeaseId"});
        const auto providerEpoch = nativeProviderFieldValue(
          fields, {"executionLeaseEpoch"});
        const auto transactionId = nativeProviderFieldValue(
          fields, {"executionLeaseTransactionId", "executionRequestId"});
        const auto planDigest = nativeProviderFieldValue(
          fields, {"executionLeasePlanDigest"});
        const auto bindingProofText = nativeProviderFieldValue(
          fields, {"executionLeaseBindingProof"});
        const auto providerRoleCountText = nativeProviderFieldValue(
          fields, {"executionLeaseProviderRoleCount"});
        if (leaseId.empty() || providerEpoch.empty() || transactionId.empty() ||
            planDigest.empty() || bindingProofText.empty() ||
            config.executionLeaseTargetService.empty()) {
          ctx.fail("LEASE_BINDING_MISMATCH");
          return;
        }
        ndn_service_framework::ExecutionLeaseBinding binding;
        binding.requesterName = ctx.requesterName().toUri();
        binding.requestId = transactionId;
        binding.serviceName = config.executionLeaseTargetService;
        binding.planDigest = planDigest;
        binding.resourceBindingSchema = "ndnsf-di-binding-v1";
        binding.resourceBindingProof = ndn::Buffer(
          reinterpret_cast<const uint8_t*>(bindingProofText.data()),
          bindingProofText.size());
        const auto now = static_cast<uint64_t>(std::max<long long>(0, epochMs()));
        if (config.requireExecutionActivation) {
          const auto activationDigest = nativeProviderFieldValue(
            fields, {"executionActivationDigest"});
          const auto activationMembers = nativeProviderFieldValue(
            fields, {"executionActivationMembers"});
          const auto activationLocalMember = nativeProviderFieldValue(
            fields, {"executionActivationLocalMember"});
          if (activationDigest.empty() || activationMembers.empty() ||
              activationLocalMember.empty() ||
              activationMembers.find(activationLocalMember) == std::string::npos) {
            ctx.fail("DI_EXECUTION_ACTIVATION_BINDING_MISMATCH");
            return;
          }
        }
        logProviderBoundaryStdout("LEASE_ACTIVATION_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        const auto activated = config.executionLeaseTable->validateAndActivate(
          leaseId,
          providerEpoch,
          binding,
          "activate:" + transactionId,
          now,
          now + std::max<uint64_t>(1, config.executionLeaseHardDeadlineMs));
        if (!activated.status) {
          ctx.fail(activated.reasonCode);
          return;
        }
        logProviderBoundaryStdout("LEASE_ACTIVATION_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        activatedLeaseId = leaseId;
        activatedProviderEpoch = providerEpoch;
        activatedRequester = binding.requesterName;
        activatedRole = role;
        if (!providerRoleCountText.empty()) {
          try {
            expectedProviderRoles = std::max<std::size_t>(
              1, static_cast<std::size_t>(std::stoull(providerRoleCountText)));
          }
          catch (const std::exception&) {
            completeExecutionLease();
            ctx.fail("LEASE_BINDING_MISMATCH");
            return;
          }
        }
      }
      const auto bindingError = (selectionProjection &&
                                 config.runnerPreparationFactory)
        ? std::optional<std::string>{}
        : validateNativeProviderAssignmentPayload(
            state->runnerSpecs, role, ctx.assignment().assignmentPayload);
      if (bindingError) {
        if (nativeTraceEnabled()) {
          std::ostringstream record;
          record << "NDNSF_DI_RESOURCE_BINDING_REJECTED"
                 << " session=" << ctx.sessionId()
                 << " role=" << role
                 << " reason=" << *bindingError;
          logRuntimeWarn(record.str());
        }
        completeExecutionLease();
        ctx.fail(*bindingError);
        return;
      }
      // The V3 Selection projection is itself the authenticated request
      // identity.  A non-lease local full-plan execution still reconstructs
      // RoleSpec objects from the plan below; preserve the projection's
      // request/attempt binding for those objects so execution evidence is
      // correlated with the Selection that authorized the run.  Keep this
      // separate from executionAttempt: only lease-bound attempts were
      // admitted to attemptAuthority and may be completed below.
      const auto roleExecutionAttempt = executionAttempt
        ? executionAttempt
        : (selectionProjection
             ? std::optional<ExecutionAttemptKey>{ExecutionAttemptKey{
                 selectionProjection->requestId,
                 selectionProjection->attempt}}
             : std::nullopt);
      const auto executionSessionId = executionAttempt
        ? executionAttempt->scopedSessionId()
        : ctx.sessionId();
      auto roleSpec = selectionProjection
        ? roleSpecFromSelectionProjectionV3(
            *selectionProjection, ctx.localProvider().toUri())
        : (executionAttempt
            ? roleSpecFor(executionPlan,
                          role,
                          *executionAttempt,
                          assignment,
                          ctx.localProvider().toUri())
            : roleSpecFor(executionPlan,
                          role,
                          executionSessionId,
                              assignment,
                              ctx.localProvider().toUri()));
      logProviderBoundaryStdout("ROLE_SPEC_READY", stageRequestId,
                                ctx.localProvider().toUri(), role);
      if (selectionProjection && selectionProjection->conversationTurnBinding &&
          selectionProjection->conversationTurnBinding->serviceName !=
            ctx.assignment().service.toUri()) {
        completeExecutionLease();
        ctx.fail("CONVERSATION_SERVICE_BINDING_MISMATCH");
        return;
      }
      if (selectionProjection &&
          selectionProjection->conversationStateReference) {
        if (config.conversationStateKeyScope.empty()) {
          completeExecutionLease();
          ctx.fail("PROVIDER_CONVERSATION_READY_SCOPE_MISSING");
          return;
        }
        if (selectionProjection->generationContract.generationId.empty()) {
          completeExecutionLease();
          ctx.fail("PROVIDER_CONVERSATION_GENERATION_ID_MISSING");
          return;
        }
        const auto lookupNowMs = static_cast<std::uint64_t>(
          std::max<long long>(0, epochMs()));
        auto resolved = state->runtime.resolveConversationState(
          *selectionProjection->conversationStateReference, lookupNowMs);
        if (!resolved) {
          completeExecutionLease();
          ctx.fail("PROVIDER_CONVERSATION_STATE_MISSING");
          return;
        }
        roleSpec.conversationStateBinding = std::move(*resolved);
        roleSpec.conversationStateLookupNowMs = lookupNowMs;

        // Readiness is a compact, Provider-authored commitment that the exact
        // parent state reference was resolved locally.  It deliberately
        // carries no state bytes; the User may enter delta prefill only after
        // it has received one verified record from every selected role.
        boost::property_tree::ptree ready;
        ready.put("schema", "ndnsf-di-conversation-state-ready-v1");
        ready.put("requestId", selectionProjection->requestId);
        ready.put("attemptEpoch", selectionProjection->attempt);
        ready.put("generationId", selectionProjection->generationContract.generationId);
        ready.put("planDigest", selectionProjection->planDigest);
        ready.put("conversationId",
                  selectionProjection->conversationStateReference->conversationId);
        ready.put("contextEpoch",
                  selectionProjection->conversationStateReference->contextEpoch);
        ready.put("serviceName",
                  selectionProjection->conversationStateReference->serviceName);
        ready.put("planRoleMapDigest",
                  selectionProjection->conversationStateReference->planRoleMapDigest);
        ready.put("checkpointDigest",
                  selectionProjection->conversationStateReference->checkpointDigest);
        ready.put("roleName",
                  selectionProjection->conversationStateReference->roleName);
        ready.put("roleReceiptDigest",
                  selectionProjection->conversationStateReference->roleReceiptDigest);
        ready.put("providerIdentity", ctx.localProvider().toUri());
        ready.put("providerBootId", roleSpec.conversationStateBinding->identity.providerBootId);
        ready.put("cacheEpoch", roleSpec.conversationStateBinding->identity.cacheEpoch);
        ready.put("ready", true);
        ready.put("reason", "state-hit");
        ready.put("residency", "GPU_RESIDENT");
        std::ostringstream readyWire;
        boost::property_tree::write_json(readyWire, ready, false);
        const auto readyPayload = readyWire.str();
        ctx.publish(
          config.conversationStateKeyScope,
          ndn::Name("/ndnsf-di/conversation/ready"),
          ndn::Buffer(reinterpret_cast<const std::uint8_t*>(readyPayload.data()),
                      readyPayload.size()));
        std::ostringstream record;
        record << "NDNSF_DI_CONVERSATION_STATE"
               << " event=state-ready-published"
               << " requestId=" << selectionProjection->requestId
               << " conversationId="
               << selectionProjection->conversationStateReference->conversationId
               << " contextEpoch="
               << selectionProjection->conversationStateReference->contextEpoch
               << " role="
               << selectionProjection->conversationStateReference->roleName;
        logRuntimeInfo(record.str());
      }
      // DATA_DRIVEN_V2 serving deliberately keeps metadata-only runner slots
      // until authenticated Selection.  Do not validate those empty slots as
      // if they were the prepared ONNX runner; the preparation factory will
      // return and validate the certified spec immediately before execution.
      const auto* readinessRunnerSpec = config.runnerPreparationFactory
        ? nullptr : runnerSpecForRole(state->runnerSpecs, role);
      const auto deploymentRevision = nativeProviderFieldValue(
        assignmentFields, {"deploymentRevision", "revision", "planRevision"});
      const auto adapterIdentity = readinessRunnerSpec == nullptr
        ? std::string("native")
        : (!readinessRunnerSpec->backend.empty()
             ? readinessRunnerSpec->backend
             : readinessRunnerSpec->kind);
      const auto reportStatus = [&] (const std::string& operationId,
                                     const std::string& operation,
                                     const std::string& stateName,
                                     std::uint64_t sequence,
                                     double progress,
                                     const std::string& phase) {
        if (selectionProjection) {
          logProviderBoundaryStdout("STATUS_REPORT_BEGIN", stageRequestId,
                                    ctx.localProvider().toUri(), role);
          logRuntimeEvidence(
            std::string("NDNSF_DI_STATUS_REPORT_BEGIN provider=") +
            ctx.localProvider().toUri() + " requestId=" + stageRequestId +
            " role=" + role + " operationId=" + operationId +
            " state=" + stateName + " phase=" + phase);
        }
        ndn_service_framework::ServiceProvider::ServiceOperationStatus status;
        status.operationId = operationId;
        status.operation = operation;
        status.role = role;
        status.attempt = executionAttempt ? executionAttempt->attemptEpoch : 1;
        status.epoch = 1;
        status.sequence = sequence;
        status.state = stateName;
        status.progressKnown = true;
        status.progress = progress;
        status.createdAtMs = static_cast<std::uint64_t>(
          std::max<long long>(0, epochMs()));
        status.updatedAtMs = status.createdAtMs;
        status.expiresAtMs = status.createdAtMs + 120000;
        status.detailsSchema = "ndnsf-di-preparation-progress-v1";
        const auto details = std::string("{\"phase\":\"") + phase +
          "\",\"deploymentRevision\":\"" +
              (deploymentRevision.empty() ? requestPlanDigest : deploymentRevision) +
          "\",\"adapter\":\"" + adapterIdentity + "\"}";
        status.detailsPayload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(details.data()), details.size());
        ctx.reportOperationStatus(std::move(status));
        if (selectionProjection) {
          logProviderBoundaryStdout("STATUS_REPORT_DONE", stageRequestId,
                                    ctx.localProvider().toUri(), role);
          logRuntimeEvidence(
            std::string("NDNSF_DI_STATUS_REPORT_DONE provider=") +
            ctx.localProvider().toUri() + " requestId=" + stageRequestId +
            " role=" + role + " operationId=" + operationId +
            " state=" + stateName + " phase=" + phase);
        }
      };
      const auto readinessOperationId =
        ctx.assignment().selectionDigest + ":" + role + ":readiness";
      const auto executionOperationId =
        ctx.assignment().selectionDigest + ":" + role + ":execution";
      const auto preparationStatusSequence =
        std::make_shared<std::atomic<std::uint64_t>>(0);
      const auto runnerPreparationSequence =
        std::make_shared<std::atomic<std::uint64_t>>(0);
      std::function<void()> executionGuard;
      if (protectedRuntime) {
        executionGuard = [protectedRuntime] {
          protectedRuntime->withContentKey(epochMs(), [] (const auto&) {});
        };
      }
      ProviderRoleWorker::NativeRunnerPreparation prepareRunner;
      if (config.executionPolicy == "DATA_DRIVEN_V2") {
        const auto expectedBackend = nativeProviderFieldValue(
          assignmentFields, {"backend", "executionBackend"});
        const auto expectedDevice = nativeProviderFieldValue(
          assignmentFields, {"device", "executionDevice"});
        const auto expectedArtifact = nativeProviderFieldValue(
          assignmentFields,
          {"artifactDigest", "fragmentDigest", "modelFragmentDigest"});
        if (selectionProjection && config.runnerPreparationFactory) {
          const auto projection = *selectionProjection;
          const auto preparationFactory = config.runnerPreparationFactory;
          const auto runnerFactory = state->runnerFactory;
          const auto runnerReuseLookup = config.runnerReuseLookup;
          prepareRunner = [&ctx, projection, preparationFactory, runnerFactory,
                           protectedRuntime, executionGuard,
                           runnerReuseLookup,
                           expectedBackend, expectedDevice, expectedArtifact,
                           role, reportStatus, readinessOperationId,
                           preparationStatusSequence, runnerPreparationSequence,
                           stageAttemptEpoch] {
            const auto preparationId = std::to_string(
              runnerPreparationSequence->fetch_add(1, std::memory_order_relaxed) + 1);
            logProviderBoundaryStdout("RUNNER_PREPARATION_BEGIN", projection.requestId,
                                      ctx.localProvider().toUri(), role);
            logProviderStageMarker("ASSEMBLY_STARTED", projection.requestId,
                                   ctx.localProvider().toUri(), role,
                                   projection.planDigest, "observed", {},
                                   stageAttemptEpoch, preparationId);
            if (executionGuard) executionGuard();
            if (runnerReuseLookup) {
              auto runner = runnerReuseLookup(projection, protectedRuntime);
              if (runner) {
                logRuntimeEvidence(
                  std::string("NDNSF_DI_PROVIDER_PREPARATION phase=RUNNER_REUSE_HIT") +
                  " requestId=" + projection.requestId +
                  " provider=" + ctx.localProvider().toUri() +
                  " role=" + role + " detail=live-runner");
                if (executionGuard) executionGuard();
                const auto evidence = runner->executionEvidenceSnapshot();
                if (!evidence) {
                  throw std::runtime_error("DI_RUNTIME_EVIDENCE_MISSING");
                }
                if (const auto error = validateNativeProviderRuntimeReadiness(
                      *evidence, role, expectedBackend, expectedDevice,
                      expectedArtifact)) {
                  throw std::runtime_error(*error);
                }
                logProviderStageMarker("RUNNER_READY", projection.requestId,
                                       ctx.localProvider().toUri(), role,
                                       projection.planDigest, "observed", {},
                                       stageAttemptEpoch, preparationId);
                reportStatus(readinessOperationId, "ensure-deployment", "DONE",
                             preparationStatusSequence->fetch_add(
                               1, std::memory_order_relaxed) + 1,
                             1.0, "READY");
                return runner;
              }
            }
            logProviderBoundaryStdout("RUNNER_PREPARATION_FACTORY_BEGIN",
                                      projection.requestId,
                                      ctx.localProvider().toUri(), role);
            auto spec = preparationFactory(ctx, projection, protectedRuntime);
            logProviderBoundaryStdout("RUNNER_PREPARATION_FACTORY_DONE",
                                      projection.requestId,
                                      ctx.localProvider().toUri(), role);
            if (const auto error = validateNativePreparedRunnerSpec(
                  projection, spec)) {
              throw std::runtime_error(*error);
            }
            if (executionGuard) executionGuard();
            logProviderBoundaryStdout("RUNNER_CREATE_BEGIN", projection.requestId,
                                      ctx.localProvider().toUri(), role);
            auto runner = runnerFactory->create(spec);
            logProviderBoundaryStdout("RUNNER_CREATE_DONE", projection.requestId,
                                      ctx.localProvider().toUri(), role);
            if (executionGuard) executionGuard();
            const auto evidence = runner->executionEvidenceSnapshot();
            if (!evidence) {
              throw std::runtime_error("DI_RUNTIME_EVIDENCE_MISSING");
            }
            if (const auto error = validateNativeProviderRuntimeReadiness(
                  *evidence, role, expectedBackend, expectedDevice,
                  expectedArtifact)) {
              throw std::runtime_error(*error);
            }
            logProviderStageMarker("RUNNER_READY", projection.requestId,
                                   ctx.localProvider().toUri(), role,
                                   projection.planDigest, "observed", {},
                                   stageAttemptEpoch, preparationId);
            reportStatus(readinessOperationId, "ensure-deployment", "DONE",
                         preparationStatusSequence->fetch_add(
                           1, std::memory_order_relaxed) + 1,
                         1.0, "READY");
            return runner;
          };
        }
        else {
          const auto evidence = std::find_if(
            state->executionEvidence.begin(), state->executionEvidence.end(),
            [&role] (const ExecutionEvidence& item) {
              return std::find(item.roles.begin(), item.roles.end(), role) !=
                item.roles.end();
            });
          if (evidence == state->executionEvidence.end()) {
            completeExecutionLease();
            ctx.fail("DI_RUNTIME_EVIDENCE_MISSING");
            return;
          }
          const auto readinessError = validateNativeProviderRuntimeReadiness(
            *evidence, role, expectedBackend, expectedDevice, expectedArtifact);
          if (readinessError) {
            completeExecutionLease();
            ctx.fail(*readinessError);
            return;
          }
          // Preassembled compatibility is READY before queue execution because
          // its configured runner was already loaded and warmed at startup.
          reportStatus(readinessOperationId, "ensure-deployment", "DONE", 1,
                       1.0, "READY");
        }
      }
      else {
        reportStatus(readinessOperationId, "ensure-deployment", "DONE", 1,
                     1.0, "READY");
      }

      // Readiness status is observational.  Before entering any runner, every
      // selected role rendezvous over one request-scoped encrypted
      // Collaboration channel and validates exact selection/revision/plan
      // membership.  This is a DI payload above NDNSF Core, not a new base
      // protocol message.
      logProviderBoundaryStdout("READINESS_BARRIER_CHECK", stageRequestId,
                                ctx.localProvider().toUri(), role);
      if (config.allowLegacyPeerReadinessBarrier) {
        logProviderBoundaryStdout("READINESS_BARRIER_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        constexpr const char* readinessScope = "ndnsf-di-readiness-v1";
        const ndn::Name readinessTopic("/ndnsf-di/readiness");
        auto expected = ctx.assignment().roleProviders;
        expected[role] = ctx.localProvider();
        const auto activationDigest = nativeProviderFieldValue(
          assignmentFields, {"executionActivationDigest"});
        const auto activationMembersText = nativeProviderFieldValue(
          assignmentFields, {"executionActivationMembers"});
        const auto localMember = nativeProviderFieldValue(
          assignmentFields, {"executionActivationLocalMember"});
        const auto declaredBindingDigest = nativeProviderFieldValue(
          assignmentFields, {"readinessBindingDigest"});
        const auto declaredRolesText = nativeProviderFieldValue(
          assignmentFields, {"readinessRoles"});
        const auto declaredRoleCountText = nativeProviderFieldValue(
          assignmentFields, {"readinessRoleCount"});
        std::set<std::string> activationMembers;
        std::size_t memberStart = 0;
        while (memberStart <= activationMembersText.size()) {
          const auto comma = activationMembersText.find(',', memberStart);
          const auto member = activationMembersText.substr(
            memberStart,
            comma == std::string::npos ? std::string::npos : comma - memberStart);
          if (!member.empty()) {
            activationMembers.insert(member);
          }
          if (comma == std::string::npos) {
            break;
          }
          memberStart = comma + 1;
        }
        const bool activationBound = !activationDigest.empty() &&
          !localMember.empty() && !activationMembers.empty();
        if (activationBound && activationMembers.count(localMember) == 0) {
          completeExecutionLease();
          ctx.fail("DI_READINESS_ACTIVATION_MEMBER_MISMATCH");
          return;
        }
        std::set<std::string> declaredRoles;
        memberStart = 0;
        while (memberStart <= declaredRolesText.size()) {
          const auto comma = declaredRolesText.find(',', memberStart);
          const auto declaredRole = declaredRolesText.substr(
            memberStart,
            comma == std::string::npos ? std::string::npos : comma - memberStart);
          if (!declaredRole.empty()) {
            declaredRoles.insert(declaredRole);
          }
          if (comma == std::string::npos) {
            break;
          }
          memberStart = comma + 1;
        }
        std::size_t declaredRoleCount = 0;
        try {
          declaredRoleCount = declaredRoleCountText.empty()
            ? 0 : static_cast<std::size_t>(std::stoull(declaredRoleCountText));
        }
        catch (const std::exception&) {
          declaredRoleCount = 0;
        }
        const bool declaredBound = !activationBound &&
          !declaredBindingDigest.empty() && !declaredRoles.empty() &&
          declaredRoleCount == declaredRoles.size() &&
          declaredRoles.count(role) != 0;
        const auto expectedReadinessCount = activationBound
          ? activationMembers.size()
          : (declaredBound ? declaredRoles.size() : expected.size());
        const auto readinessBindingDigest = activationBound
          ? activationDigest
          : (declaredBound ? declaredBindingDigest
                           : ctx.assignment().selectionDigest);
        const auto effectiveRevision = deploymentRevision.empty()
          ? requestPlanDigest : deploymentRevision;
        const auto effectivePlanDigest = requestPlanDigest.empty()
          ? effectiveRevision : requestPlanDigest;
        const auto attemptEpoch = executionAttempt
          ? executionAttempt->attemptEpoch : 1;
        const auto artifactDigest = readinessRunnerSpec == nullptr
          ? std::string("role:") + role
          : fragmentDigestFor(*readinessRunnerSpec);
        const auto localPayload = std::string("schema=ndnsf-di-readiness-v1;") +
          "revision=" + effectiveRevision + ";" +
          "planDigest=" + effectivePlanDigest + ";" +
          "bindingDigest=" + readinessBindingDigest + ";" +
          "memberId=" + (activationBound ? localMember : role) + ";" +
          "role=" + role + ";" +
          "provider=" + ctx.localProvider().toUri() + ";" +
          "attempt=" + std::to_string(attemptEpoch) + ";" +
          "adapter=" + adapterIdentity + ";" +
          "artifactDigest=" + artifactDigest + ";";
        const auto publishReadiness = [&] {
          ctx.publish(
            readinessScope,
            readinessTopic,
            ndn::Buffer(reinterpret_cast<const std::uint8_t*>(localPayload.data()),
                        localPayload.size()));
        };
        publishReadiness();

        std::map<std::string, std::string> observed;
        observed.emplace(activationBound ? localMember : role, localPayload);
        std::map<std::string, std::string> observedRoleProviders;
        observedRoleProviders.emplace(role, ctx.localProvider().toUri());
        const auto barrierDeadline = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(timeoutBudget.readinessMs);
        auto nextReadinessPublish = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(250);
        while (observed.size() < expectedReadinessCount &&
               std::chrono::steady_clock::now() < barrierDeadline) {
          if (std::chrono::steady_clock::now() >= nextReadinessPublish) {
            // Collaboration notifications may race assignment/scope-key
            // installation.  Re-publish the same idempotent snapshot at a
            // bounded rate so late peers recover without an event log.
            publishReadiness();
            nextReadinessPublish = std::chrono::steady_clock::now() +
              std::chrono::milliseconds(250);
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            barrierDeadline - std::chrono::steady_clock::now()).count();
          const auto items = ctx.waitFor(
            readinessScope, readinessTopic, 1,
            static_cast<int>(std::max<long long>(1, std::min<long long>(100, remaining))));
          for (const auto& item : items) {
            const std::string payload(item.payload.begin(), item.payload.end());
            const auto fields = parseNativeProviderAssignmentFields(item.payload);
            const auto itemRole = nativeProviderFieldValue(fields, {"role"});
            const auto providerText = nativeProviderFieldValue(fields, {"provider"});
            const auto memberId = nativeProviderFieldValue(fields, {"memberId"});
            const auto expectedProvider = expected.find(itemRole);
            const bool exactMember = activationBound
              ? activationMembers.count(memberId) != 0
              : (declaredBound ? declaredRoles.count(itemRole) != 0
                               : expectedProvider != expected.end());
            const bool exactProvider = activationBound
              ? !providerText.empty() && item.producer.toUri() == providerText
              : (expectedProvider != expected.end()
                   ? item.producer.equals(expectedProvider->second) &&
                     providerText == expectedProvider->second.toUri()
                   : declaredBound && !providerText.empty() &&
                     item.producer.toUri() == providerText);
            if (nativeProviderFieldValue(fields, {"schema"}) !=
                  "ndnsf-di-readiness-v1" ||
                !exactMember || !exactProvider || itemRole.empty() ||
                item.producerRole != itemRole ||
                nativeProviderFieldValue(fields, {"revision"}) != effectiveRevision ||
                nativeProviderFieldValue(fields, {"planDigest"}) != effectivePlanDigest ||
                nativeProviderFieldValue(fields, {"bindingDigest"}) !=
                  readinessBindingDigest ||
                nativeProviderFieldValue(fields, {"attempt"}) !=
                  std::to_string(attemptEpoch) ||
                nativeProviderFieldValue(fields, {"adapter"}).empty() ||
                nativeProviderFieldValue(fields, {"artifactDigest"}).empty()) {
              completeExecutionLease();
              ctx.fail("DI_READINESS_BINDING_MISMATCH");
              return;
            }
            const auto roleOwner = observedRoleProviders.find(itemRole);
            if (roleOwner != observedRoleProviders.end() &&
                roleOwner->second != providerText) {
              completeExecutionLease();
              ctx.fail("DI_READINESS_ROLE_CONFLICT");
              return;
            }
            const auto previous = observed.find(memberId);
            if (previous != observed.end() && previous->second != payload) {
              completeExecutionLease();
              ctx.fail("DI_READINESS_REPLAY_CONFLICT");
              return;
            }
            observedRoleProviders[itemRole] = providerText;
            observed[memberId] = payload;
          }
          if (observed.size() < expectedReadinessCount) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }
        if (observed.size() != expectedReadinessCount) {
          completeExecutionLease();
          ctx.fail("DI_READINESS_BARRIER_TIMEOUT");
          return;
        }
        std::ostringstream record;
        record << "NDNSF_DI_READINESS_BARRIER"
               << " status=ready"
               << " session=" << ctx.sessionId()
               << " role=" << ctx.role()
               << " observed_roles=" << observed.size()
               << " revision=" << effectiveRevision
               << " binding_digest=" << readinessBindingDigest;
        logRuntimeInfo(record.str());
        logProviderBoundaryStdout("READINESS_BARRIER_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
      }
      logProviderBoundaryStdout("EXECUTION_STATUS_BEGIN", stageRequestId,
                                ctx.localProvider().toUri(), role);
      reportStatus(executionOperationId, "distributed-inference", "RUNNING", 1,
                   0.0, "EXECUTING");
      logProviderBoundaryStdout("EXECUTION_STATUS_DONE", stageRequestId,
                                ctx.localProvider().toUri(), role);
      if (const auto* spec = runnerSpecForRole(state->runnerSpecs, role)) {
        logFragmentInventoryEvent("EXECUTION_OBSERVED",
                                  *spec,
                                  ctx.localProvider().toUri());
      }
      std::optional<KvStateBinding> kvBinding;
      std::optional<TensorBundle> cachedKvState;
      const auto kvMode = nativeProviderFieldValue(assignmentFields, {"kvMode"});
      if (!kvMode.empty()) {
        if (prepareRunner) {
          completeExecutionLease();
          ctx.fail("KV_STATE_POST_SELECTION_PREPARATION_UNSUPPORTED");
          return;
        }
        const auto* runnerSpec = runnerSpecForRole(state->runnerSpecs, role);
        if (runnerSpec == nullptr || !config.kvStateStore) {
          completeExecutionLease();
          ctx.fail("KV_STATE_UNAVAILABLE");
          return;
        }
        try {
          kvBinding = kvBindingFromAssignment(
            *runnerSpec,
            assignmentFields,
            ctx.sessionId(),
            role,
            ctx.localProvider().toUri(),
            config.kvSecurityEpoch);
        }
        catch (const std::exception&) {
          completeExecutionLease();
          ctx.fail("KV_BINDING_MISMATCH");
          return;
        }
        if (kvMode == "cache-hit" || kvMode == "delta-only") {
          cachedKvState = config.kvStateStore->lookup(*kvBinding);
          std::ostringstream record;
          record << "NDNSF_DI_KV_STATE event=lookup"
                 << " session=" << kvBinding->sessionId
                 << " role=" << role
                 << " context_epoch=" << kvBinding->contextEpoch
                 << " mode=" << kvMode
                 << " status=" << (cachedKvState ? "hit" : "miss");
          logRuntimeInfo(record.str());
          if (!cachedKvState) {
            completeExecutionLease();
            ctx.fail(kvMode == "delta-only" ?
                       "CACHE_MISS_FULL_CONTEXT_REQUIRED" : "KV_STATE_UNAVAILABLE");
            return;
          }
        }
        else if (kvMode != "full-context") {
          completeExecutionLease();
          ctx.fail("KV_BINDING_MISMATCH");
          return;
        }
      }
      const bool localFullPlan = nativeProviderShouldExecuteLocalPlan(
        executionPlan,
        assignment,
        roleSpec,
        ctx.localProvider().toUri());
      if (authenticatedGeneration.enabled && localFullPlan &&
          executionPlan.roles.size() == 1) {
        // A one-role generation has a self TOKEN_FEEDBACK edge. Keep that
        // exact dependency inside the selected Provider process; an external
        // SVS publication is not a reliable loopback fetch source and is not
        // needed to cross a trust boundary.
        io = std::make_shared<LocalDependencyIo>();
      }
      completedLocalPlan = localFullPlan;
      std::map<std::string, TensorBundle> initialInputs;
      const bool hasApplicationInput = selectionProjection &&
        std::any_of(roleSpec.inputs.begin(), roleSpec.inputs.end(),
                    [] (const auto& edge) {
                      return edge.operationKind == "APPLICATION_INPUT";
                    });
      if (nativeTraceEnabled() && selectionProjection) {
        std::ostringstream record;
        record << "NDNSF_DI_INPUT_CONTRACT"
               << " role=" << role
               << " input_count=" << roleSpec.inputs.size()
               << " application_input=" << (hasApplicationInput ? "true" : "false");
        for (const auto& edge : roleSpec.inputs) {
          record << " edge=" << edge.operationKind << "@" << edge.scope;
        }
        logRuntimeInfo(record.str());
      }
      if (selectionProjection && !hasApplicationInput) {
        if (config.spec180YnMutation == "Y-N-I") {
          try {
            initialInputsFromRequest(ctx, request, false);
          }
          catch (const std::invalid_argument& error) {
            if (std::string(error.what()) != "DI_INPUT_FETCH_ROLE_MISMATCH") {
              throw;
            }
            completeExecutionLease();
            ctx.fail("DI_INPUT_FETCH_ROLE_MISMATCH");
            std::ostringstream record;
            record << "SPEC180_YN_NEGATIVE_RESULT status=PASS"
                    << " subcase=Y-N-I"
                    << " boundary=PROVIDER_EXECUTION_STARTED"
                    << " reason=NON_INGRESS_INPUT_REJECTED"
                    << " requestId=" << selectionProjection->requestId
                    << " attemptId=attempt-" << selectionProjection->attempt
                    << " observedPhase=PROVIDER_EXECUTION_STARTED"
                    << " provider=" << ctx.localProvider().toUri()
                    << " planDigest=" << selectionProjection->planDigest
                    << " errorCode=DI_INPUT_FETCH_ROLE_MISMATCH";
            logRuntimeEvidence(record.str());
            return;
          }
          completeExecutionLease();
          ctx.fail("DI_INPUT_FETCH_ROLE_MISMATCH_ACCEPTED");
          return;
        }
      }
      else {
        initialInputs = initialInputsFromRequest(
          ctx, request, !selectionProjection || hasApplicationInput);
        if (selectionProjection && hasApplicationInput) {
          const auto requestInput = initialInputs.find("request-input");
          if (requestInput == initialInputs.end()) {
            completeExecutionLease();
            ctx.fail("DI_INPUT_FETCH_ROLE_MISMATCH");
            return;
          }
          // Remove the authenticated source alias before installing the
          // certified edge scopes.  An APPLICATION_INPUT endpoint is allowed
          // to use group_id=request-input, so erasing the alias after mapping
          // would also erase the real execution input.
          const auto requestInputBundle = requestInput->second;
          initialInputs.erase(requestInput);
          for (const auto& edge : roleSpec.inputs) {
            if (edge.operationKind != "APPLICATION_INPUT") {
              continue;
            }
            auto applicationInput = requestInputBundle;
            applicationInput.name = edge.tensors.size() == 1
              ? edge.tensors.front() : edge.scope;
            applicationInput.expectedSegments = 1;
            applicationInput.expectedBytes = applicationInput.payload.size();
            initialInputs[edge.scope] = std::move(applicationInput);
          }
        }
      }
      if (nativeTraceEnabled() && selectionProjection) {
        for (const auto& item : initialInputs) {
          std::ostringstream record;
          record << "NDNSF_DI_INPUT_BUNDLE"
                 << " scope=" << item.first
                 << " name=" << item.second.name
                 << " bytes=" << item.second.payload.size()
                 << " encoded="
                 << (isEncodedTensorBundle(item.second.payload) ? "true" : "false");
          if (isEncodedTensorBundle(item.second.payload)) {
            try {
              record << " tensors=";
              const auto tensors = decodeTensorBundle(item.second.payload);
              for (std::size_t index = 0; index < tensors.size(); ++index) {
                if (index != 0) record << ',';
                record << tensors[index].name;
              }
            }
            catch (const std::exception&) {
              record << " tensors=<decode-error>";
            }
          }
          logRuntimeInfo(record.str());
        }
      }
      if (cachedKvState) {
        const auto* runnerSpec = runnerSpecForRole(state->runnerSpecs, role);
        try {
          injectCachedKvInputs(initialInputs, *runnerSpec, *cachedKvState);
        }
        catch (const std::exception&) {
          completeExecutionLease();
          ctx.fail("KV_STATE_UNAVAILABLE");
          return;
        }
      }
      if (selectionProjection) {
        logProviderStageMarker("EXECUTION_ENTERED", stageRequestId,
                               ctx.localProvider().toUri(), role,
                               requestPlanDigest, "observed", {},
                               stageAttemptEpoch);
      }
      const auto submittedSteady = std::chrono::steady_clock::now();
      const auto submittedEpoch = epochMs();
      logProviderCapacity(ctx.sessionId(),
                          role,
                          "before_submit",
                          state->runtime.snapshot());
      std::optional<std::vector<uint8_t>> finalPayload;
      std::atomic<std::size_t> streamedEventsPublished{0};
      RoleExecutionContext::StreamEventSink eventSink;
      const bool terminalRole = roleSpec.outputs.empty() ||
        nativeRoleHasOnlyInternalFeedbackOutputs(roleSpec);
      if (ctx.isStreamed() && terminalRole) {
        eventSink = [&ctx, &streamedEventsPublished] (
                      const std::vector<std::uint8_t>& payload) {
          const auto cursor = ctx.publishStreamEvent(
            ndn::Buffer(payload.data(), payload.size()));
          if (cursor != 0) {
            streamedEventsPublished.fetch_add(1, std::memory_order_relaxed);
            return true;
          }
          return false;
        };
      }
      // Stateful streamed generation must use the same request-scoped epoch
      // coordinator even when the whole selected plan is local.  The ordinary
      // one-shot local-plan executor waits for every declared input before its
      // first run; a self TOKEN_FEEDBACK edge therefore deadlocks at epoch 0
      // and bypasses Provider-owned state transactions.
      if (localFullPlan && !authenticatedGeneration.enabled) {
        finalPayload = executeLocalPlanAndFinalPayload(*state,
                                                       config,
                                                       executionPlan,
                                                       executionSessionId,
                                                       roleExecutionAttempt,
                                                       assignment,
                                                       ctx.localProvider().toUri(),
                                                       initialInputs,
                                                       submittedSteady,
                                                       submittedEpoch,
                                                       prepareRunner,
                                                       std::move(eventSink),
                                                       executionGuard,
                                                       selectionProjection,
                                                       protectedRuntime,
                                                       config.runnerReusePublisher);
        if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
          const auto elapsed = std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - submittedSteady));
          (*config.stageServiceTimeObserver)(elapsed);
        }
      }
      else if (authenticatedGeneration.enabled) {
        if (authenticatedGeneration.maxEpochs == 0 ||
            authenticatedGeneration.samplingDigest.empty() ||
            !selectionProjection) {
          throw std::runtime_error(
            "native epoch coordinator authenticated authority is incomplete");
        }
        const auto trustedState = trustedDecodeStateTemplateFor(
          *selectionProjection, readinessRunnerSpec, config.providerBootId);
        const auto streamAttemptEpoch =
          request.hasStreamRequestOptions()
            ? request.getStreamRequestOptions().attemptEpoch
            : 1;
        NativeEpochCoordinatorConfig coordinatorConfig{
          state->runtime, executionPlan, assignment, io};
        coordinatorConfig.sessionId = executionSessionId;
        coordinatorConfig.requestId = executionAttempt
          ? executionAttempt->requestId : ctx.sessionId();
        coordinatorConfig.attemptEpoch = executionAttempt
          ? executionAttempt->attemptEpoch : streamAttemptEpoch;
        coordinatorConfig.streamEpoch = 1;
        coordinatorConfig.lineagePlanDigest = selectionProjection->planDigest;
        coordinatorConfig.localProvider = ctx.localProvider().toUri();
        coordinatorConfig.role = role;
        coordinatorConfig.initialInputs = std::move(initialInputs);
        coordinatorConfig.prepareRunner = prepareRunner;
        if (selectionProjection) {
          const auto projection = *selectionProjection;
          const auto assignmentForCoordinator = assignment;
          const auto providerName = ctx.localProvider().toUri();
          coordinatorConfig.roleSpecFactory =
            [projection, assignmentForCoordinator, providerName] (std::size_t sequence) {
              auto projected = roleSpecFromSelectionProjectionV3(
                projection, providerName, sequence);
              // Generation control edges are added by the requester planner
              // after the cross-Provider V3 projection is sealed. Preserve
              // those exact TOKEN_FEEDBACK edges, but never reintroduce the
              // legacy reconstruction of protected pipeline edges.
              // V3 assignments may contain only the local role. Missing
              // remote mappings must stay empty so dependency IO resolves
              // the producer through the sealed capability rank, not self.
              const auto control = roleSpecFor(
                projection.plan, projection.executionRole.roleId,
                projection.requestId, assignmentForCoordinator, "",
                sequence);
              for (const auto& edge : control.inputs) {
                if (edge.operationKind == "TOKEN_FEEDBACK")
                  projected.inputs.push_back(edge);
              }
              for (const auto& edge : control.outputs) {
                if (edge.operationKind == "TOKEN_FEEDBACK")
                  projected.outputs.push_back(edge);
              }
              return projected;
            };
        }
        coordinatorConfig.executionGuard = executionGuard;
        coordinatorConfig.finalResponseScope = config.finalResponseScope;
        coordinatorConfig.maxEpochs = authenticatedGeneration.maxEpochs;
        coordinatorConfig.tokenInputName = authenticatedGeneration.tokenInputName;
        coordinatorConfig.stateInputNames = authenticatedGeneration.stateInputNames;
        coordinatorConfig.stateOutputNames = authenticatedGeneration.stateOutputNames;
        coordinatorConfig.stateIdentityTemplate = trustedState.identity;
        coordinatorConfig.positionPolicyDigest = trustedState.positionPolicyDigest;
        coordinatorConfig.eosTokenIds = authenticatedGeneration.eosTokenIds;
        coordinatorConfig.samplingDigest = authenticatedGeneration.samplingDigest;
        coordinatorConfig.samplingMode = authenticatedGeneration.samplingMode;
        coordinatorConfig.samplingTemperature =
          authenticatedGeneration.samplingTemperature;
        coordinatorConfig.samplingTopK = authenticatedGeneration.samplingTopK;
        coordinatorConfig.samplingTopP = authenticatedGeneration.samplingTopP;
        coordinatorConfig.samplingRepetitionPenalty =
          authenticatedGeneration.samplingRepetitionPenalty;
        coordinatorConfig.samplingSeed = authenticatedGeneration.samplingSeed;
        coordinatorConfig.stopStrings = authenticatedGeneration.stopStrings;
        coordinatorConfig.textDecoder = authenticatedGeneration.textDecoder;
        coordinatorConfig.stableTextDecoder = authenticatedGeneration.stableTextDecoder;
        coordinatorConfig.requireTextOutput =
          authenticatedGeneration.requireTextOutput;
        coordinatorConfig.checkpointFinalize =
          selectionProjection->conversationTurnBinding.has_value();
        coordinatorConfig.maxCheckpointFinalizeTokens = 32;
        coordinatorConfig.committedPrefixTokenIds =
          authenticatedGeneration.committedPrefixTokenIds;
        coordinatorConfig.conversationStateBinding =
          roleSpec.conversationStateBinding;
        coordinatorConfig.conversationStateLookupNowMs =
          roleSpec.conversationStateLookupNowMs;
        if (request.hasStreamRequestOptions()) {
          // Every selected role receives the authenticated absolute stream
          // deadline, even though only the terminal role owns the external
          // StreamEventPublisher.  Gate deadline checks on the request
          // contract, not on publisher ownership.  Cancellation lifecycle is
          // currently local to the terminal publisher, so observe it only
          // when this context owned that publisher at coordinator creation.
          const bool observesStreamCancellation = ctx.isStreamed();
          coordinatorConfig.stopCheck = [&ctx, observesStreamCancellation] ()
            -> std::optional<NativeEpochStopReason> {
            if (ctx.streamRemainingDeadline() <= std::chrono::milliseconds(0)) {
              return std::optional<NativeEpochStopReason>{
                NativeEpochStopReason::Deadline};
            }
            if (observesStreamCancellation && ctx.streamCancelled()) {
              return std::optional<NativeEpochStopReason>{
                NativeEpochStopReason::Cancelled};
            }
            return std::nullopt;
          };
        }
        coordinatorConfig.eventSink = std::move(eventSink);
        coordinatorConfig.resultObserver =
          [&] (const RoleSpec& executedRole, const ProviderRoleResult& result) {
            if (result.executionEvidence && config.executionEvidenceObserver &&
                *config.executionEvidenceObserver) {
              (*config.executionEvidenceObserver)(*result.executionEvidence);
            }
            if (result.runner && selectionProjection &&
                config.runnerReusePublisher) {
              config.runnerReusePublisher(
                *selectionProjection, protectedRuntime, result.runner);
            }
            logProviderTiming(ctx.sessionId(), executedRole.role, result,
                              submittedSteady, submittedEpoch);
            if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
              const auto elapsed = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                  result.timing.finishedAt - result.timing.startedAt));
              (*config.stageServiceTimeObserver)(elapsed);
            }
          };
        logProviderBoundaryStdout("EPOCH_COORDINATOR_BEGIN", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        const auto coordinated = runNativeEpochCoordinator(
          std::move(coordinatorConfig));
        logProviderBoundaryStdout("EPOCH_COORDINATOR_DONE", stageRequestId,
                                  ctx.localProvider().toUri(), role);
        if (config.epochCoordinatorCompletionObserver &&
            *config.epochCoordinatorCompletionObserver) {
          try {
            (*config.epochCoordinatorCompletionObserver)(role, coordinated);
          }
          catch (...) {
            // Observability must not alter native execution semantics.
          }
        }
        streamedEventsPublished.store(
          coordinated.eventsPublished, std::memory_order_relaxed);
        finalPayload = coordinated.finalPayload;
        executionStopped = coordinated.stoppedByUpstream;
        if (selectionProjection->conversationTurnBinding) {
          if (!coordinated.finalizedRole ||
              !coordinated.finalizedRole->candidateDecodeStateIdentity) {
            throw std::runtime_error(
              "PROVIDER_CONVERSATION_FINAL_STATE_MISSING");
          }
          if (ctx.assignment().scopeKeys.count(
                config.conversationStateKeyScope) == 0) {
            throw std::runtime_error(
              "PROVIDER_CONVERSATION_RECEIPT_SCOPE_MISSING");
          }
          const auto& turn = *selectionProjection->conversationTurnBinding;
          const auto& finalized = *coordinated.finalizedRole;
          const auto& identity = *finalized.candidateDecodeStateIdentity;
          ProviderConversationStateReceiptV1 receipt;
          receipt.conversationId = turn.conversationId;
          receipt.parentContextEpoch = turn.parentContextEpoch;
          receipt.successorContextEpoch = turn.successorContextEpoch;
          receipt.originRequestId = identity.requestId;
          receipt.originGenerationId = identity.generationId;
          receipt.serviceName = turn.serviceName;
          receipt.requesterIdentity = ctx.requesterName().toUri();
          receipt.securityDomainDigest = identity.securityDomainDigest;
          receipt.modelDigest = identity.modelDigest;
          receipt.graphSemanticDigest = identity.graphSemanticDigest;
          receipt.adapterDigest = identity.adapterDigest;
          receipt.roleName = identity.roleName;
          receipt.roleSplitDigest = identity.roleSplitDigest;
          receipt.layoutDigest = identity.layoutDigest;
          receipt.planRoleMapDigest = turn.planRoleMapDigest;
          receipt.providerIdentity = identity.providerIdentity;
          receipt.providerBootId = identity.providerBootId;
          receipt.cacheEpoch = identity.cacheEpoch;
          receipt.prefixDigest = identity.prefixDigest;
          receipt.prefixTokenCount = identity.prefixTokenCount;
          receipt.positionDigest = identity.positionDigest;
          receipt.stateSchemaDigest = identity.stateSchemaDigest;
          receipt.stateComponentDigests = identity.stateComponentDigests;
          // Use one time sample for the advertised receipt, binding and
          // actual store admission, so advertised retention cannot outlive KV.
          const auto promotionNowMs = static_cast<std::uint64_t>(
            std::max<long long>(0, epochMs()));
          const auto maximumDeadline = std::numeric_limits<std::uint64_t>::max();
          const auto localDeadline = promotionNowMs > maximumDeadline - config.conversationRetentionMs
            ? maximumDeadline : promotionNowMs + config.conversationRetentionMs;
          receipt.expiresAtMs = std::min(turn.retentionDeadlineMs, localDeadline);
          receipt.validate();

          ConversationStateBinding conversationBinding;
          conversationBinding.conversationId = turn.conversationId;
          conversationBinding.contextEpoch = turn.successorContextEpoch;
          conversationBinding.serviceName = turn.serviceName;
          conversationBinding.planRoleMapDigest = turn.planRoleMapDigest;
          conversationBinding.receiptDigest = receipt.computedDigest();
          conversationBinding.expiresAtMs = receipt.expiresAtMs;
          conversationBinding.identity = identity;
          if (!state->runtime.stageDecodeStatePromotion(
                executionSessionId, finalized, conversationBinding,
                promotionNowMs)) {
            // The conversation-enabled coordinator retained the final
            // request-local decode state for this handoff.  A failed stage
            // must release that state before surfacing the boundary error.
            (void)state->runtime.releaseDecodeState(executionSessionId,
                                                     finalized.role);
            throw std::runtime_error(
              "PROVIDER_CONVERSATION_PROMOTION_STAGE_FAILED");
          }
          // The receipt is only a Provider-authored candidate.  Keep the
          // candidate invisible until the requester has atomically committed
          // the complete role set.  The control record uses the existing
          // encrypted collaboration substrate; it is not a new Core wire
          // message and is accepted only from this request's requester.
          // All controls share one request-scoped encrypted topic.  The role
          // remains an authenticated field in the control payload; using one
          // canonical topic avoids URI-component ambiguity for role names that
          // themselves contain slashes.
          const ndn::Name controlTopic("/ndnsf-di/conversation/control");
          ctx.subscribe(
            config.conversationStateKeyScope,
            controlTopic,
            [] (const ndn_service_framework::ServiceProvider::CollaborationData&) {});
          const auto receiptJson = receipt.toJson();
          ndn::Name receiptTopic("/ndnsf-di/conversation/receipt");
          receiptTopic.append(ndn::Name(finalized.role));
          ctx.publish(
            config.conversationStateKeyScope,
            std::move(receiptTopic),
            ndn::Buffer(receiptJson.begin(), receiptJson.end()));
          std::ostringstream record;
          record << "NDNSF_DI_CONVERSATION_STATE"
                 << " event=staged-receipt-published"
                 << " requestId=" << ctx.sessionId()
                 << " conversationId=" << turn.conversationId
                 << " contextEpoch=" << turn.successorContextEpoch
                 << " role=" << finalized.role
                 << " receiptDigest=" << receipt.computedDigest();
          logRuntimeInfo(record.str());

          conversationPromotionStaged = true;
          rollbackConversationPromotion = [&, state, conversationBinding] {
            if (!conversationPromotionStaged || conversationPromotionCommitted) {
              return;
            }
            if (!state->runtime.rollbackStagedDecodeStatePromotion(conversationBinding))
              throw std::runtime_error("PROVIDER_CONVERSATION_STAGED_ROLLBACK_FAILED");
            (void)state->runtime.releaseDecodeState(executionSessionId,
                                                     conversationBinding.identity.roleName);
            conversationPromotionStaged = false;
          };
          waitConversationPromotion = [&, state, turn, finalized, identity, receipt,
              conversationBinding, controlTopic, selectionProjection,
              committedCheckpoint = std::string{}]() mutable {
            if (!conversationPromotionStaged || conversationPromotionCommitted) {
              return;
            }
            const auto nowForControl = static_cast<std::uint64_t>(
              std::max<long long>(0, epochMs()));
            const auto waitBudget = static_cast<std::uint64_t>(
              std::max(1, timeoutBudget.conversationControlMs));
            const auto controlDeadline = std::min(
              receipt.expiresAtMs, nowForControl + waitBudget);
            const auto publishAck = [&](bool committed, const std::string& checkpointDigest) {
              NativeJson ack{{"schema", committed ? "ndnsf-di-provider-conversation-commit-ack-v1" :
                                                     "ndnsf-di-provider-conversation-rollback-ack-v1"},
                {"requestId", selectionProjection->requestId}, {"attemptEpoch", selectionProjection->attempt},
                {"generationId", selectionProjection->generationContract.generationId},
                {"planDigest", selectionProjection->planDigest}, {"conversationId", turn.conversationId},
                {"parentContextEpoch", turn.parentContextEpoch}, {"successorContextEpoch", turn.successorContextEpoch},
                {"serviceName", turn.serviceName}, {"planRoleMapDigest", turn.planRoleMapDigest},
                {"roleName", finalized.role}, {"receiptDigest", receipt.computedDigest()},
                {"checkpointDigest", checkpointDigest}, {"providerIdentity", ctx.localProvider().toUri()},
                {"providerBootId", identity.providerBootId}, {"cacheEpoch", identity.cacheEpoch}};
              ack[committed ? "committed" : "rolledBack"] = true;
              const auto wire = nativeCanonicalJson(ack);
              ctx.publish(config.conversationStateKeyScope,
                ndn::Name(committed ? "/ndnsf-di/conversation/commit" : "/ndnsf-di/conversation/rollback"),
                ndn::Buffer(wire.begin(), wire.end()));
            };
            const auto logControl = [&](const char* event,
                                        const std::string& action,
                                        const char* reason,
                                        std::uint64_t sequence) {
              std::ostringstream record;
              record << "NDNSF_DI_CONVERSATION_CONTROL"
                     << " event=" << event
                     << " requestId=" << ctx.sessionId()
                     << " conversationId=" << turn.conversationId
                     << " role=" << finalized.role
                     << " action=" << (action.empty() ? "unknown" : action)
                     << " sequence=" << sequence
                     << " reason=" << (reason ? reason : "none");
              logRuntimeEvidence(record.str());
            };
            // waitFor returns the complete matching collaboration history on each
            // poll.  Keep control handling idempotent at the wire-sequence level so
            // a committed turn emits one acknowledgement per requester control,
            // rather than replaying the same historical COMMIT until the deadline.
            NativeProviderControlSequenceGuard controlSequenceGuard;
            while (true) {
              const auto now = static_cast<std::uint64_t>(
                std::max<long long>(0, epochMs()));
              if (now >= controlDeadline) {
                break;
              }
              const auto remaining = static_cast<int>(std::max<std::uint64_t>(
                1, std::min<std::uint64_t>(100, controlDeadline - now)));
              const auto controls = ctx.waitFor(
                config.conversationStateKeyScope, controlTopic, 1, remaining);
              for (const auto& item : controls) {
                if (!item.producer.equals(ctx.requesterName()) ||
                    item.producerRole != "user-control-v1") {
                  logControl("rejected", {}, "producer_binding", item.sequence);
                  continue;
                }
                const auto sequenceDecision = controlSequenceGuard.observe(item.sequence);
                if (sequenceDecision == NativeProviderControlSequenceGuard::Decision::Duplicate) {
                  logControl("rejected", {}, "duplicate_sequence", item.sequence);
                  continue;
                }
                if (sequenceDecision == NativeProviderControlSequenceGuard::Decision::Reordered) {
                  logControl("rejected", {}, "reordered_sequence", item.sequence);
                  continue;
                }
                ConversationPromotionControl control;
                try {
                  control = parseConversationPromotionControl(item.payload);
                }
                catch (const std::exception&) {
                  logControl("rejected", {}, "malformed", item.sequence);
                  continue;
                }
                if (control.conversationId != turn.conversationId ||
                    control.parentContextEpoch != turn.parentContextEpoch ||
                    control.successorContextEpoch != turn.successorContextEpoch ||
                    control.serviceName != turn.serviceName ||
                    control.planRoleMapDigest != turn.planRoleMapDigest ||
                    control.roleName != finalized.role ||
                    control.receiptDigest != receipt.computedDigest() ||
                    control.expiresAtMs > receipt.expiresAtMs ||
                    control.expiresAtMs <= now) {
                  logControl("rejected", control.action, "binding_or_expiry", item.sequence);
                  continue;
                }
                logControl("received", control.action, "authenticated", item.sequence);
                ConversationStateReferenceV1 reference;
                reference.conversationId = control.conversationId;
                reference.contextEpoch = control.successorContextEpoch;
                reference.serviceName = control.serviceName;
                reference.planRoleMapDigest = control.planRoleMapDigest;
                reference.checkpointDigest = control.checkpointDigest;
                reference.roleName = control.roleName;
                reference.roleReceiptDigest = control.receiptDigest;
                reference.expiresAtMs = control.expiresAtMs;
                try {
                  if (conversationPromotionCommitted) {
                    if (control.checkpointDigest != committedCheckpoint) {
                      logControl("rejected", control.action, "checkpoint_binding", item.sequence);
                      continue;
                    }
                    if (control.action == "FINALIZE") {
                      logControl("accepted", control.action, "retention_closed", item.sequence);
                      return;
                    }
                    if (control.action == "COMMIT") {
                      publishAck(true, committedCheckpoint);
                      logControl("accepted", control.action, "duplicate_commit", item.sequence);
                      continue;
                    }
                    auto committedBinding = conversationBinding;
                    committedBinding.checkpointDigest = committedCheckpoint;
                    if (!state->runtime.releaseConversationState(committedBinding))
                      throw std::runtime_error("PROVIDER_CONVERSATION_COMMITTED_ROLLBACK_FAILED");
                    conversationPromotionCommitted = false;
                    publishAck(false, committedCheckpoint);
                    logControl("accepted", control.action, "post_commit_rollback", item.sequence);
                    throw std::runtime_error("PROVIDER_CONVERSATION_PROMOTION_ROLLED_BACK");
                  }
                  if (control.action == "FINALIZE") {
                    logControl("rejected", control.action, "before_commit", item.sequence);
                    continue;
                  }
                  const auto resolved = state->runtime
                    .resolveStagedConversationState(reference, now);
                  if (!resolved || resolved->receiptDigest != receipt.computedDigest()) {
                    logControl("rejected", control.action, "staged_state_missing", item.sequence);
                    continue;
                  }
                  if (control.action == "ROLLBACK") {
                    rollbackConversationPromotion();
                    publishAck(false, control.checkpointDigest);
                    logControl("accepted", control.action, "staged_rollback", item.sequence);
                    throw std::runtime_error(
                      "PROVIDER_CONVERSATION_PROMOTION_ROLLED_BACK");
                  }
                  if (!state->runtime.commitStagedDecodeStatePromotion(
                        *resolved, control.checkpointDigest)) {
                    logControl("rejected", control.action, "commit_rejected", item.sequence);
                    continue;
                  }
                  conversationPromotionCommitted = true;
                  conversationPromotionStaged = false;
                  committedCheckpoint = control.checkpointDigest;
                  // The requester needs an authenticated per-role commit
                  // acknowledgement.  Receipt publication proves only that
                  // a candidate was staged; this record is emitted after the
                  // Provider has atomically committed the successor and
                  // released its request-local owner.
                  publishAck(true, committedCheckpoint);
                  std::ostringstream record;
                  record << "NDNSF_DI_CONVERSATION_STATE"
                         << " event=promotion-committed"
                         << " requestId=" << ctx.sessionId()
                         << " conversationId=" << turn.conversationId
                         << " contextEpoch=" << turn.successorContextEpoch
                         << " role=" << finalized.role
                         << " checkpointDigest=" << control.checkpointDigest;
                  logRuntimeInfo(record.str());
                  // Keep a bounded authenticated compensation window open
                  // until the requester confirms its durable journal commit.
                  logControl("accepted", control.action, "staged_commit", item.sequence);
                }
                catch (const std::runtime_error&) {
                  logControl("error", control.action, "state_transition", item.sequence);
                  throw;
                }
                catch (const std::exception&) {
                  logControl("error", control.action, "processing_exception", item.sequence);
                  continue;
                }
              }
            }
            // A lost FINALIZE cannot prove that the requester failed to commit.
            // Keep a committed successor until its original retention deadline.
            if (conversationPromotionCommitted) {
              logControl("timeout", "FINALIZE", "retention_preserved", 0);
              return;
            }
            rollbackConversationPromotion();
            logControl("timeout", "COMMIT", "uncommitted_rollback", 0);
            throw std::runtime_error(
              "PROVIDER_CONVERSATION_PROMOTION_COMMIT_TIMEOUT");
          };
        }
        if (coordinated.stoppedByUpstream) {
          completedLocalPlan = false;
        }
      }
      else {
        std::vector<RoleSpec> localRoleSpecs;
        bool selectionRoleMatched = false;
        for (const auto& plannedRole : executionPlan.roles) {
          const auto assigned = assignment.providerByRole.find(plannedRole);
          if (assigned == assignment.providerByRole.end() ||
              assigned->second != ctx.localProvider().toUri()) {
            continue;
          }
          if (selectionProjection) {
            if (plannedRole != selectionProjection->executionRole.roleId) {
              // A V3 assignment envelope authorizes exactly one local role.
              // Other roles owned by this Provider are launched by their own
              // envelope; they must not make this envelope fail closed.
              continue;
            }
            selectionRoleMatched = true;
            auto localSpec = roleSpecFromSelectionProjectionV3(
              *selectionProjection, ctx.localProvider().toUri());
            if (localSpec.role == roleSpec.role) {
              localSpec.conversationStateBinding =
                roleSpec.conversationStateBinding;
              localSpec.conversationStateLookupNowMs =
                roleSpec.conversationStateLookupNowMs;
            }
            localRoleSpecs.push_back(std::move(localSpec));
          }
          else {
            localRoleSpecs.push_back(
              executionAttempt
                ? roleSpecFor(executionPlan,
                              plannedRole,
                              *executionAttempt,
                              assignment,
                              ctx.localProvider().toUri())
                : roleSpecFor(executionPlan,
                              plannedRole,
                              executionSessionId,
                              assignment,
                              ctx.localProvider().toUri()));
          }
        }
        if (selectionProjection && !selectionRoleMatched) {
          throw std::invalid_argument(
            "V3 Selection cannot execute an undeclared local role");
        }
        // Register every local consumer's dependency wait before a colocated
        // source can publish.  SVSPubSub subscriptions are prospective; this
        // ordering removes a same-Provider source/consumer startup race while
        // preserving the plan's data dependencies and concurrent execution.
        std::stable_sort(
          localRoleSpecs.begin(), localRoleSpecs.end(),
          [] (const RoleSpec& lhs, const RoleSpec& rhs) {
            return !lhs.inputs.empty() && rhs.inputs.empty();
          });
        std::vector<std::pair<RoleSpec, std::future<ProviderRoleResult>>> localRoles;
        for (auto& localRoleSpec : localRoleSpecs) {
          auto roleInputs = localRoleSpec.inputs.empty()
            ? initialInputs : std::map<std::string, TensorBundle>{};
          for (const auto& edge : localRoleSpec.inputs) {
            if (edge.operationKind != "APPLICATION_INPUT") {
              continue;
            }
            const auto input = initialInputs.find(edge.scope);
            if (input == initialInputs.end()) {
              throw std::runtime_error(
                "V3 input-ingress role is missing its application input");
            }
            roleInputs.emplace(edge.scope, input->second);
          }
          if (prepareRunner) {
            if (localRoleSpecs.size() != 1 || localRoleSpec.role != role) {
              throw std::runtime_error(
                "post-Selection runner preparation requires one local role");
            }
            localRoles.emplace_back(
              localRoleSpec,
              state->runtime.executePreparedRoleAsync(
                executionSessionId, localRoleSpec, io, prepareRunner,
                std::move(roleInputs),
                localRoleSpec.outputs.empty()
                  ? eventSink : RoleExecutionContext::StreamEventSink{},
                executionGuard));
          }
          else {
            localRoles.emplace_back(
              localRoleSpec,
              state->runtime.executeRoleAsync(
                executionSessionId, localRoleSpec, io,
                std::move(roleInputs),
                localRoleSpec.outputs.empty()
                  ? eventSink : RoleExecutionContext::StreamEventSink{}));
          }
        }
        if (localRoles.empty()) {
          throw std::runtime_error(
            "V3 Selection projection assigns no executable role to Provider");
        }

        for (auto& localRole : localRoles) {
          auto result = localRole.second.get();
          const auto& executedRoleSpec = localRole.first;
          if (result.runner && selectionProjection &&
              config.runnerReusePublisher) {
            config.runnerReusePublisher(
              *selectionProjection, protectedRuntime, result.runner);
          }
          if (result.executionEvidence && config.executionEvidenceObserver &&
              *config.executionEvidenceObserver) {
            (*config.executionEvidenceObserver)(*result.executionEvidence);
          }
          if (kvBinding && config.kvStateStore) {
            auto kvOutput = result.outputsByScope.find(config.kvOutputScope);
            if (kvOutput == result.outputsByScope.end()) {
              kvOutput = std::find_if(
                result.outputsByScope.begin(), result.outputsByScope.end(), [] (const auto& item) {
                  return isEncodedTensorBundle(item.second.payload);
                });
            }
            auto storedBinding = *kvBinding;
            const auto nextEpoch = nativeProviderFieldValue(
              assignmentFields, {"kvNextContextEpoch"});
            if (!nextEpoch.empty()) {
              try {
                std::size_t consumed = 0;
                storedBinding.contextEpoch = std::stoull(nextEpoch, &consumed);
                if (consumed != nextEpoch.size() ||
                    storedBinding.contextEpoch <= kvBinding->contextEpoch) {
                  throw std::invalid_argument("invalid next epoch");
                }
              }
              catch (const std::exception&) {
                completeExecutionLease();
                ctx.fail("KV_BINDING_MISMATCH");
                return;
              }
            }
            if (kvOutput != result.outputsByScope.end() &&
                !config.kvStateStore->put(std::move(storedBinding), kvOutput->second)) {
              completeExecutionLease();
              ctx.fail("KV_STATE_CAPACITY_EXCEEDED");
              return;
            }
            if (kvOutput != result.outputsByScope.end()) {
              std::ostringstream record;
              record << "NDNSF_DI_KV_STATE event=store"
                     << " session=" << kvBinding->sessionId
                     << " role=" << executedRoleSpec.role
                     << " context_epoch="
                     << (nextEpoch.empty() ? kvBinding->contextEpoch : std::stoull(nextEpoch))
                     << " bytes=" << kvOutput->second.payload.size();
              logRuntimeInfo(record.str());
            }
          }
          logProviderTiming(ctx.sessionId(), executedRoleSpec.role,
                            result, submittedSteady, submittedEpoch);
          if (config.stageServiceTimeObserver && *config.stageServiceTimeObserver) {
            const auto elapsed = std::max(
              std::chrono::milliseconds(1),
              std::chrono::duration_cast<std::chrono::milliseconds>(
                result.timing.finishedAt - result.timing.startedAt));
            (*config.stageServiceTimeObserver)(elapsed);
          }

          auto localFinalPayload = nativeProviderFinalResponsePayload(
            executedRoleSpec,
            result,
            config.finalResponseScope);
          if (localFinalPayload) {
            finalPayload = std::move(localFinalPayload);
          }
        }
      }
      logProviderCapacity(ctx.sessionId(),
                          role,
                          "after_complete",
                          state->runtime.snapshot());
      if (protectedRuntime) {
        executionGuard();
        protectedRuntime->complete();
      }
      if (executionAttempt && !state->attemptAuthority.complete(*executionAttempt)) {
        std::ostringstream record;
        record << "NDNSF_DI_EXECUTION_ATTEMPT"
               << " decision=reject"
               << " reason=DI_ATTEMPT_DUPLICATE_TERMINAL"
               << " requestId=" << executionAttempt->requestId
               << " attemptEpoch=" << executionAttempt->attemptEpoch;
        logRuntimeWarn(record.str());
        completeExecutionLease();
        ctx.fail("DI_ATTEMPT_DUPLICATE_TERMINAL");
        return;
      }
      completeExecutionLease();
      reportStatus(executionOperationId, "distributed-inference", "DONE", 2,
                   1.0, "EXECUTED");
      if (nativeTraceEnabled()) {
        std::ostringstream record;
        record << "NDNSF_DI_NATIVE_FINAL_RESPONSE_DECISION"
               << " session=" << ctx.sessionId()
               << " role=" << role
               << " role_outputs=" << roleSpec.outputs.size()
               << " local_full_plan="
               << (localFullPlan ? "true" : "false")
               << " final_scope=" << config.finalResponseScope
               << " has_payload=" << (finalPayload ? "true" : "false");
        logRuntimeInfo(record.str());
      }
      {
        std::ostringstream record;
        record << "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED"
               << " requestId=" << ctx.sessionId()
               << " attemptEpoch=" << (executionAttempt
                     ? executionAttempt->attemptEpoch
                     : (selectionProjection ? selectionProjection->attempt : 1))
               << " epochMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count()
               << " provider=" << ctx.localProvider().toUri()
               << " planDigest=" << requestPlanDigest;
        logRuntimeEvidence(record.str());
      }
      if (finalPayload) {
        // The conversation receipt is published before this point, so the
        // requester can authorize the promotion without seeing a terminal
        // response first.  Keep the terminal response behind the authenticated
        // commit/finalize gate; an invalid control must fail the request rather
        // than expose a successful terminal result.
        if (waitConversationPromotion) {
          waitConversationPromotion();
        }
        if (ctx.isStreamed() &&
            streamedEventsPublished.load(std::memory_order_relaxed) == 0) {
          const ndn::Buffer eventPayload(finalPayload->data(), finalPayload->size());
          if (ctx.publishStreamEvent(eventPayload) == 0) {
            ctx.failStream(ndn_service_framework::StreamedInvocationErrorCode::ProviderFailure,
                           "native streamed event admission failed");
            return;
          }
        }
        ctx.publishFinalResponse(ndn::Buffer(finalPayload->data(), finalPayload->size()));
        if (selectionProjection) {
          logProviderStageMarker("EXECUTION_COMPLETED", stageRequestId,
                                 ctx.localProvider().toUri(), role,
                                 requestPlanDigest,
                                 executionStopped ? "stopped" : "observed",
                                 executionStopped ? "upstream-stop" : "",
                                 stageAttemptEpoch);
          if (!executionStopped) {
            logProviderStageMarker("TERMINAL", stageRequestId,
                                   ctx.localProvider().toUri(), role,
                                   requestPlanDigest, "observed", {},
                                   stageAttemptEpoch);
          }
        }
      }
      else {
        if (terminalRole) {
          // A terminal role that did not produce the sealed final scope is a
          // failed invocation, not a successful role completion.  Completing
          // it here would leave the user waiting for an End/Response that can
          // never be produced.
          if (selectionProjection) {
            logProviderStageMarker("EXECUTION_COMPLETED", stageRequestId,
                                   ctx.localProvider().toUri(), role,
                                   requestPlanDigest, "failed",
                                   "DI_FINAL_RESPONSE_MISSING",
                                   stageAttemptEpoch);
            logProviderStageMarker("TERMINAL", stageRequestId,
                                   ctx.localProvider().toUri(), role,
                                   requestPlanDigest, "failed",
                                   "DI_FINAL_RESPONSE_MISSING",
                                   stageAttemptEpoch);
          }
          ctx.fail("DI_FINAL_RESPONSE_MISSING");
        }
        else {
          // A non-terminal pipeline role has completed its selected work but
          // does not own the user-facing End/Response.  Release only this
          // Provider-side execution slot; terminal ownership remains with the
          // final role and the shared streamed lifecycle.
          if (waitConversationPromotion) {
            waitConversationPromotion();
          }
          if (selectionProjection) {
            logProviderStageMarker("EXECUTION_COMPLETED", stageRequestId,
                                   ctx.localProvider().toUri(), role,
                                   requestPlanDigest,
                                   executionStopped ? "stopped" : "observed",
                                   executionStopped ? "upstream-stop" : "",
                                   stageAttemptEpoch);
          }
          ctx.completeRole();
        }
      }
    }
	    catch (const std::exception& exc) {
	      if (conversationPromotionStaged && !conversationPromotionCommitted &&
	          rollbackConversationPromotion) {
	        rollbackConversationPromotion();
	      }
	      if (protectedRuntime) {
	        try {
	          protectedRuntime->cancel(exc.what());
	        }
	        catch (...) {
	          // The runtime remains FailedClosed; preserve the original failure.
	        }
	      }
	      completeExecutionLease();
	      if (config.nativeFailureObserver && *config.nativeFailureObserver) {
	        try {
	          (*config.nativeFailureObserver)(ctx.role(), exc.what());
	        }
	        catch (...) {
	          // Observability must never replace the original native failure.
	        }
	      }
	      if (nativeTraceEnabled()) {
	        std::ostringstream record;
	        record << "NDNSF_DI_NATIVE_FAILURE session=" << ctx.sessionId()
	               << " role=" << ctx.role()
	               << " reason=" << exc.what();
	        logRuntimeError(record.str());
	      }
      if (selectionObserved) {
        logProviderStageMarker("TERMINAL", stageRequestId,
                               ctx.localProvider().toUri(), ctx.role(),
                               stagePlanDigest, "failed", exc.what(),
                               stageAttemptEpoch);
	      }
	      ctx.fail(exc.what());
	    }
	  };
  return runtime;
}

ndn_service_framework::ServiceProvider::CollaborationHandler
makeNativeProviderCollaborationHandler(NativeProviderHandlerConfig config)
{
  return makeNativeProviderCollaborationRuntime(std::move(config)).handler;
}

} // namespace ndnsf::di
