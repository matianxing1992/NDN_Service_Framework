#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeRequestOptionsTransport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include <openssl/evp.h>
#include <ndn-cxx/name.hpp>
#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>

namespace ndnsf::di {
namespace {
// YOLO [1,3,640,640] float32 input is 4,915,200 bytes before the envelope;
// keep the native inline request bound large enough for that production path.
constexpr std::size_t MAX_WIRE = 16 * 1024 * 1024;
bool digest(const std::string& s)
{
  return s.size() == 71 && s.compare(0, 7, "sha256:") == 0 &&
    std::all_of(s.begin() + 7, s.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string hash(const NativeJson& value)
{ return nativePlanningDigest(nativeCanonicalJson(value)); }
std::string base64(const std::vector<std::uint8_t>& bytes)
{
  if (bytes.size() > MAX_WIRE) throw std::invalid_argument("DI request payload exceeds limit");
  if (bytes.empty()) return {};
  std::string result(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                   bytes.data(), static_cast<int>(bytes.size()));
  if (size < 0) throw std::runtime_error("DI request base64 encoding failed");
  result.resize(static_cast<std::size_t>(size));
  return result;
}

NativeJson
automaticDiscoverySummary(const NativeJson& envelope, const std::string& requestDigest)
{
  auto summary = NativeJson{
    {"schema", "ndnsf-di-request-discovery-v1"}, {"schema_version", 1},
    {"request_contract_digest", requestDigest},
    {"automatic_contract_digest", envelope.at("automatic_shape_seed").at("contractDigest")},
    {"graphDigest", envelope.at("graph_digest")}};
  for (const auto* key : {"request_id", "attempt", "service", "model_identity_hash",
                          "plan_deadline_ms"})
    summary[key] = envelope.at(key);
  summary["task"] = {{"adapter", "automatic-onnx"},
    {"placement_profile", envelope.at("task").at("placement_profile")}};
  if (envelope.at("task").at("placement_profile") == "DI_PLACEMENT_V4") {
    for (const auto* key : {"graph_digest", "candidate_providers", "candidate_set_digest",
                            "candidate_artifacts", "candidate_artifact_set_digest", "rtt_pairs",
                            "goodput_edges", "probe_scope_digest", "requester_identity"})
      summary[key] = envelope.at(key);
  }
  return summary;
}
}

std::optional<NativeAutomaticShapeSeed>
nativeRequestAutomaticShapeSeed(const NativeJson& envelope)
{
  if (!envelope.is_object() || !envelope.contains("schema") ||
      !envelope.at("schema").is_string())
    throw std::invalid_argument("native request has no schema");
  const auto& schema = envelope.at("schema").get_ref<const std::string&>();
  const bool automatic = schema == "ndnsf-di-request-envelope-v3";
  if (!automatic && schema != "ndnsf-di-request-envelope-v2")
    throw std::invalid_argument("unsupported native request schema");
  const auto task = envelope.find("task");
  const bool automaticAdapter = task != envelope.end() && task->is_object() &&
    task->value("adapter", NativeJson{}) == "automatic-onnx";
  if (!automatic) {
    if (envelope.contains("automatic_shape_seed") || automaticAdapter)
      throw std::invalid_argument("automatic request cannot use legacy schema");
    return std::nullopt;
  }
  if (!envelope.contains("schema_version") ||
      !envelope.at("schema_version").is_number_integer() ||
      envelope.at("schema_version") != NativeJson(3) || !automaticAdapter ||
      !envelope.contains("automatic_shape_seed"))
    throw std::invalid_argument("automatic request version or adapter mismatch");
  return nativeAutomaticShapeSeedFromJson(envelope.at("automatic_shape_seed"));
}

std::optional<NativeJson>
parseNativeRequestEnvelope(const std::string& wire)
{
  const auto first = wire.find_first_not_of(" \t\r\n");
  if (first == std::string::npos || wire[first] != '{') return std::nullopt;
  if (wire.size() > MAX_WIRE)
    throw std::invalid_argument("native request exceeds wire limit");
  NativeJson root;
  try {
    root = nativeParseJson(wire, 64);
  }
  catch (const std::exception&) {
    // Classify decoded root keys even when strict parsing rejected a
    // duplicate. This permissive pass is bounded too; it never admits data.
    bool native = false;
    bool schemaValue = false;
    try {
      (void)NativeJson::parse(wire, [&](int depth, NativeJson::parse_event_t event,
                                      NativeJson& value) {
        if (depth > 64) throw std::invalid_argument("native request exceeds nesting limit");
        if (depth == 1 && event == NativeJson::parse_event_t::key) {
          schemaValue = value == "schema";
          if (value == "automatic_shape_seed") native = true;
        }
        else if (depth == 1 && event == NativeJson::parse_event_t::value && schemaValue) {
          if (value.is_string() &&
              value.get_ref<const std::string&>().rfind("ndnsf-di-request-", 0) == 0)
            native = true;
          schemaValue = false;
        }
        return true;
      });
    }
    catch (const std::exception&) {
      throw std::invalid_argument("malformed native request JSON");
    }
    if (native) throw std::invalid_argument("native request failed strict JSON validation");
    return std::nullopt;
  }

  if (!root.is_object()) return std::nullopt;
  const auto schema = root.find("schema");
  if (schema == root.end() || !schema->is_string() ||
      schema->get_ref<const std::string&>().rfind("ndnsf-di-request-envelope-", 0) != 0) {
    if (root.contains("automatic_shape_seed"))
      throw std::invalid_argument("automatic request has no native version");
    return std::nullopt;
  }
  (void)nativeRequestAutomaticShapeSeed(root);
  return root;
}

std::optional<NativeJson>
parseNativeRequestOffer(const std::string& wire)
{
  if (auto envelope = parseNativeRequestEnvelope(wire)) return envelope;
  const auto first = wire.find_first_not_of(" \t\r\n");
  if (first == std::string::npos || wire[first] != '{') return std::nullopt;
  NativeJson value;
  try { value = nativeParseJson(wire, 64); }
  catch (const std::exception&) { return std::nullopt; }
  if (!value.is_object() || !value.contains("schema") || !value.at("schema").is_string() ||
      value.at("schema").get_ref<const std::string&>().rfind("ndnsf-di-request-discovery-", 0) != 0)
    return std::nullopt;
  using ndn_service_framework::RequestMessage;
  if (wire.size() > RequestMessage::MAX_DISCOVERY_PAYLOAD_SIZE ||
      value.at("schema") != "ndnsf-di-request-discovery-v1" ||
      !value.contains("schema_version") || !value.at("schema_version").is_number_integer() ||
      value.at("schema_version") != 1 || !value.contains("task") ||
      !value.at("task").is_object() || value.at("task").size() != 2 ||
      value.at("task").value("adapter", NativeJson{}) != "automatic-onnx")
    throw std::invalid_argument("invalid native request discovery version or adapter");
  const auto profile = value.at("task").value("placement_profile", NativeJson{});
  const bool v4 = profile == "DI_PLACEMENT_V4";
  if ((!v4 && profile != "DI_PLACEMENT_V3") || value.size() != (v4 ? 20U : 11U))
    throw std::invalid_argument("invalid native discovery field cover");
  std::set<std::string> keys{"schema", "schema_version", "request_contract_digest",
    "automatic_contract_digest", "graphDigest", "request_id", "attempt", "service",
    "model_identity_hash", "plan_deadline_ms", "task"};
  if (v4) for (const auto* key : {"graph_digest", "candidate_providers", "candidate_set_digest",
                                 "candidate_artifacts", "candidate_artifact_set_digest",
                                 "rtt_pairs", "goodput_edges", "probe_scope_digest",
                                 "requester_identity"}) keys.emplace(key);
  for (const auto& key : keys) {
    if (!value.contains(key)) throw std::invalid_argument("native discovery omits a field");
  }
  for (const auto* key : {"request_contract_digest", "automatic_contract_digest",
                          "graphDigest", "model_identity_hash"}) {
    if (!value.at(key).is_string() || !digest(value.at(key).get<std::string>()))
      throw std::invalid_argument("native discovery digest is invalid");
  }
  for (const auto* key : {"request_id", "service"}) {
    if (!value.at(key).is_string() || value.at(key).get_ref<const std::string&>().empty())
      throw std::invalid_argument("native discovery identity is empty");
  }
  for (const auto* key : {"attempt", "plan_deadline_ms"}) {
    if (!value.at(key).is_number_integer() ||
        (!value.at(key).is_number_unsigned() && value.at(key).get<std::int64_t>() < 0) ||
        value.at(key).get<std::uint64_t>() == 0)
      throw std::invalid_argument("native discovery request scope is invalid");
  }
  if (v4 && value.at("graph_digest") != value.at("graphDigest"))
    throw std::invalid_argument("native discovery graph identities disagree");
  return value;
}

void
validateNativeAutomaticSelectionRequestBinding(
  const std::string& requestWire, const NativeJson& header)
{
  const auto request = parseNativeRequestEnvelope(requestWire);
  const auto seed = request ? nativeRequestAutomaticShapeSeed(*request) : std::nullopt;
  const auto selectedSeed = header.contains("automatic_shape_seed")
    ? std::optional<NativeAutomaticShapeSeed>(
        nativeAutomaticShapeSeedFromJson(header.at("automatic_shape_seed"))) : std::nullopt;
  if (!seed && !selectedSeed) return;
  if (!seed || !selectedSeed ||
      (header.value("schema", NativeJson{}) != "ndnsf-di-selection-v4" &&
       header.value("schema", NativeJson{}) != "ndnsf-di-selection-invocation-v4") ||
      !header.contains("schema_version") || !header.at("schema_version").is_number_integer() ||
      header.at("schema_version") != 4 ||
      nativePlanningDigest(requestWire) != header.value("request_contract_digest", NativeJson{}) ||
      nativeAutomaticShapeSeedToJson(*seed) != nativeAutomaticShapeSeedToJson(*selectedSeed) ||
      request->value("request_id", NativeJson{}) != header.value("request_id", NativeJson{}) ||
      !request->contains("attempt") || !request->at("attempt").is_number_integer() ||
      (!request->at("attempt").is_number_unsigned() && request->at("attempt").get<std::int64_t>() < 0) ||
      request->at("attempt").get<std::uint64_t>() == 0 ||
      request->at("attempt") != header.value("attempt", NativeJson{}) ||
      request->at("task").value("generation_mode", NativeJson{}) !=
        (header.contains("generation_contract") ? "TOKEN_STREAMING" : "TOKEN_DIAGNOSTIC"))
    throw std::invalid_argument("automatic Selection differs from its original request");
}

void
validateNativeAutomaticSelectionRequestBinding(
  const std::string& requestWire, const NativeSelectionProjectionV3& projection)
{
  NativeJson header{{"schema", projection.automaticShapeSeed
      ? "ndnsf-di-selection-v4" : "ndnsf-di-selection-v3"},
    {"schema_version", projection.automaticShapeSeed ? 4 : 3},
    {"request_id", projection.requestId}, {"attempt", projection.attempt},
    {"request_contract_digest", projection.requestContractDigest}};
  if (projection.automaticShapeSeed)
    header["automatic_shape_seed"] = nativeAutomaticShapeSeedToJson(*projection.automaticShapeSeed);
  if (projection.generationContract.enabled) header["generation_contract"] = NativeJson::object();
  validateNativeAutomaticSelectionRequestBinding(requestWire, header);
  if (projection.automaticShapeSeed && projection.selectedRoleContract().adapterId != "automatic-onnx")
    throw std::invalid_argument("automatic Selection adapter differs from its original request");
}

void
validateNativeAutomaticRequestBinding(
  const NativeEncodedRequest& request, const NativeModelDescriptor& model,
  const NativeRequestContract& contract,
  const std::string& requestId, std::uint64_t attempt,
  const std::optional<NativeGenerationExecutionContractV1>& generation)
{
  const auto root = parseNativeRequestEnvelope(
    std::string(request.wire.begin(), request.wire.end()));
  const auto wireSeed = root ? nativeRequestAutomaticShapeSeed(*root) : std::nullopt;
  const bool automatic = contract.adapterName == "automatic-onnx";
  if (!automatic && !request.automaticShapeSeed && !wireSeed) return;
  if (!automatic || !root || !request.automaticShapeSeed || !wireSeed ||
      nativePlanningDigest(request.wire.data(), request.wire.size()) !=
        request.requestContractDigest ||
      root->value("request_id", NativeJson{}) != requestId ||
      root->value("attempt", NativeJson{}) != attempt ||
      root->value("service", NativeJson{}) != contract.serviceName ||
      root->value("model_identity_hash", NativeJson{}) != request.modelIntentDigest ||
      request.modelIntentDigest != model.intentDigest() ||
      root->at("task").value("generation_mode", NativeJson{}) != contract.generationMode ||
      nativeAutomaticShapeSeedToJson(*wireSeed) !=
        nativeAutomaticShapeSeedToJson(*request.automaticShapeSeed) ||
      wireSeed->maxNewTokens != (generation && generation->enabled
        ? generation->maxGeneratedTokens : 0))
    throw std::invalid_argument("automatic request wire/metadata binding mismatch");
}

bool
isSupportedNativeGenerationMode(const std::string& mode) noexcept
{
  return mode == "TOKEN_DIAGNOSTIC" || mode == "TOKEN_STREAMING";
}

NativeGenerationExecutionContractV1 nativeGenerationFromOptions(
  const std::vector<std::uint8_t>& options, const std::string& generationId)
{
  if (options.size() > MAX_WIRE || generationId.size() != 32 ||
      generationId.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid generation options size or identity");
  const auto values = nativeParseJson(std::string(options.begin(), options.end()));
  auto outputMode = values.is_object() ? values.value("outputMode", std::string{}) : "";
  std::transform(outputMode.begin(), outputMode.end(), outputMode.begin(), [](unsigned char c) { return std::toupper(c); });
  if (!values.is_object() || !values.contains("useCache") || values.at("useCache") != true ||
      outputMode != "TOKEN_STREAMING")
    throw std::invalid_argument("generation requires cached TOKEN_STREAMING options");
  for (const auto name : {"generationId", "generation_id"})
    if (values.contains(name) && values.at(name) != generationId)
      throw std::invalid_argument("application generation identity mismatch");
  const auto sampling = values.contains("sampling") && !values.at("sampling").is_null()
    ? values.at("sampling") : NativeJson::object();
  if (!sampling.is_object()) throw std::invalid_argument("sampling must be an object");
  const auto get = [&](std::initializer_list<const char*> names, NativeJson fallback) {
    for (const auto name : names) {
      if (sampling.contains(name)) return sampling.at(name);
      if (values.contains(name)) return values.at(name);
    }
    return fallback;
  };
  const auto integer = [](const NativeJson& value) -> std::uint64_t {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
      throw std::invalid_argument("generation integer field has wrong type or sign");
    return value.get<std::uint64_t>();
  };
  const auto number = [](const NativeJson& value) {
    if (!value.is_number()) throw std::invalid_argument("generation sampling number has wrong type");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) throw std::invalid_argument("generation sampling number is not finite");
    return result;
  };
  NativeGenerationExecutionContractV1 result;
  result.enabled = true; result.mode = "TOKEN_STREAMING"; result.generationId = generationId;
  result.maxGeneratedTokens = integer(values.at("maxNewTokens"));
  result.tokenizerDigest = values.at("tokenizerDigest").get<std::string>();
  if (!values.at("eosTokenIds").is_array()) throw std::invalid_argument("eosTokenIds must be an array");
  for (const auto& value : values.at("eosTokenIds")) {
    const auto token = integer(value);
    if (token > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::invalid_argument("EOS token exceeds int64 range");
    result.eosTokenIds.push_back(static_cast<std::int64_t>(token));
  }
  auto mode = sampling.value("mode", values.value("samplingMode", std::string{}));
  if (mode.empty()) mode = values.value("greedy", true) ? "Greedy" : "SeededTopKTopP";
  const auto first = mode.find_first_not_of(" \t\r\n");
  mode = first == std::string::npos ? "" : mode.substr(first, mode.find_last_not_of(" \t\r\n") - first + 1);
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::tolower(c); });
  mode.erase(std::remove_if(mode.begin(), mode.end(), [](char c) { return c == '-' || c == '_'; }), mode.end());
  result.samplingMode = mode == "greedy" ? "Greedy" :
    (mode == "seededtopktopp" || mode == "topktopp" || mode == "topkp") ? "SeededTopKTopP" : "";
  result.samplingTemperature = number(get({"temperature"}, result.samplingMode == "Greedy" ? 0.0 : 1.0));
  result.samplingTopK = integer(get({"topK", "top_k"}, 1));
  result.samplingTopP = number(get({"topP", "top_p"}, 1.0));
  result.samplingRepetitionPenalty = number(get({"repetitionPenalty", "repetition_penalty"}, 1.0));
  result.samplingSeed = integer(get({"seed"}, 1'750'001));
  result.stopStrings = get({"stopStrings", "stop_strings"}, NativeJson::array()).get<std::vector<std::string>>();
  result.tokenInputName = values.value("tokenInputName", std::string("input_ids"));
  result.stateInputNames = values.value("stateInputNames", std::vector<std::string>{"attention_kv_in", "recurrent_state_in", "convolution_state_in"});
  result.stateOutputNames = values.value("stateOutputNames", std::vector<std::string>{"attention_kv_out", "recurrent_state_out", "convolution_state_out"});
  result.stateSuccessorMap = values.value("stateSuccessorMap",
    values.value("state_successor_map", values.value("kvTensorMap", std::string{})));
  result.positionInputPolicy = values.value("positionInputPolicy",
    values.value("position_input_policy", std::string{}));
  result.attentionMaskInputName = values.value("attentionMaskInputName",
    values.value("attention_mask_input_name", std::string{}));
  result.positionIdsInputName = values.value("positionIdsInputName",
    values.value("position_ids_input_name", std::string{}));
  result.cachePositionInputName = values.value("cachePositionInputName",
    values.value("cache_position_input_name", std::string{}));
  if (!result.maxGeneratedTokens || result.maxGeneratedTokens > MAX_NATIVE_GENERATED_TOKENS || !digest(result.tokenizerDigest) ||
      result.eosTokenIds.empty() || result.samplingMode.empty() || !result.samplingTopK ||
      result.samplingTopP <= 0 || result.samplingTopP > 1 ||
      result.samplingRepetitionPenalty < .1 || result.samplingRepetitionPenalty > 2 ||
      (result.samplingMode == "Greedy" ? result.samplingTemperature != 0 :
        result.samplingTemperature <= 0 || result.samplingTemperature > 5) ||
      result.stopStrings.size() > 16 || std::any_of(result.stopStrings.begin(), result.stopStrings.end(),
        [](const auto& stop) { return stop.empty() || stop.size() > 256; }))
    throw std::invalid_argument("generation options violate the execution contract");
  result.samplingDigest = hash({{"mode", result.samplingMode}, {"temperature", result.samplingTemperature},
    {"topK", result.samplingTopK}, {"topP", result.samplingTopP},
    {"repetitionPenalty", result.samplingRepetitionPenalty}, {"seed", result.samplingSeed}});
  return result;
}

std::optional<NativeCandidateArtifactInventoryV4>
buildNativeCandidateArtifactInventoryV4(
  const NativePreparedPlanningCache& planningCache, std::size_t maxCandidates)
{
  if (maxCandidates == 0 || planningCache.candidates.empty())
    return std::nullopt;
  const auto count = std::min(planningCache.candidates.size(), maxCandidates);
  std::vector<NativeJson> identities;
  for (std::size_t i = 0; i < count; ++i) {
    for (const auto& role : planningCache.candidates[i].roles) {
      if (role.role.empty() || role.backend.empty() || role.protectionEpoch.empty() ||
          !digest(role.artifactDigest) || !digest(role.modelManifestDigest) ||
          !digest(role.graphDigest))
        return std::nullopt;
      identities.push_back(NativeJson{{"artifact_digest", role.artifactDigest},
        {"model_manifest_digest", role.modelManifestDigest},
        {"graph_digest", role.graphDigest}, {"role", role.role},
        {"rank", role.rank}, {"backend", role.backend},
        {"protection_epoch", role.protectionEpoch}});
    }
  }
  if (identities.empty())
    return std::nullopt;
  std::sort(identities.begin(), identities.end(), [] (const NativeJson& left,
                                                       const NativeJson& right) {
    return nativeCanonicalJson(left) < nativeCanonicalJson(right);
  });
  identities.erase(std::unique(identities.begin(), identities.end()), identities.end());
  if (identities.empty() ||
      identities.size() > MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS)
    return std::nullopt;
  NativeCandidateArtifactInventoryV4 result;
  result.identities = std::move(identities);
  result.digest = nativePlanningDigest(nativeCanonicalJson(NativeJson(result.identities)));
  return result;
}

std::optional<NativePlacementProbeAllowlistV4>
buildNativePlacementProbeAllowlistV4(
  const NativePreparedPlanningCache& planningCache,
  std::size_t maxCandidates,
  const std::vector<std::string>& candidateProviders)
{
  constexpr std::size_t MAX_PROBE_PAIRS = 16;
  if (maxCandidates == 0 || planningCache.candidates.empty() ||
      candidateProviders.empty() || candidateProviders.size() > 16)
    return std::nullopt;

  auto roster = candidateProviders;
  for (const auto& provider : roster) {
    const ndn::Name name(provider);
    if (name.empty() || name.toUri() != provider)
      return std::nullopt;
  }
  std::sort(roster.begin(), roster.end());
  if (std::adjacent_find(roster.begin(), roster.end()) != roster.end())
    return std::nullopt;

  const auto count = std::min(planningCache.candidates.size(), maxCandidates);
  bool hasInterRoleDependency = false;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& candidate = planningCache.candidates[i].candidate;
    const std::set<std::string> roles(candidate.executionPlan.roles.begin(),
                                      candidate.executionPlan.roles.end());
    if (roles.empty() || roles.size() != candidate.executionPlan.roles.size())
      return std::nullopt;
    for (const auto& dependency : candidate.executionPlan.dependencies) {
      if (dependency.producers.empty() || dependency.consumers.empty())
        return std::nullopt;
      for (const auto& producer : dependency.producers) {
        if (roles.count(producer) == 0) return std::nullopt;
        for (const auto& consumer : dependency.consumers) {
          if (roles.count(consumer) == 0) return std::nullopt;
          hasInterRoleDependency = hasInterRoleDependency || producer != consumer;
        }
      }
    }
  }

  NativePlacementProbeAllowlistV4 result;
  if (!hasInterRoleDependency || roster.size() < 2)
    return result;
  for (const auto& source : roster) {
    for (const auto& destination : roster) {
      if (source == destination) continue;
      if (result.rttPairs.size() == MAX_PROBE_PAIRS)
        return result;
      NativePlacementProbePairV4 pair{source, destination};
      result.rttPairs.push_back(pair);
      result.goodputEdges.push_back(pair);
    }
  }
  return result;
}

namespace {
NativeEncodedRequest encodeNativeRequestEnvelopeImpl(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery,
  const std::vector<std::string>& candidateProviders,
  const std::optional<NativeCandidateArtifactInventoryV4>& candidateArtifactInventory,
  const std::optional<NativePlacementProbeAllowlistV4>& probeAllowlist,
  std::string requesterIdentity, bool selectionGenerationOptions,
  const std::shared_ptr<NativeWorkStatistics>& statistics = {},
  const std::optional<NativeAutomaticShapeSeed>& automaticShapeSeed = std::nullopt,
  bool prepareLegacyDiscovery = false)
{
  NativeWorkScope encodingWork(statistics, "request-envelope-encode", NativeWorkKind::LOCAL);
  if (selectionGenerationOptions && contract.generationMode != "TOKEN_STREAMING")
    throw std::invalid_argument("Selection generation options require TOKEN_STREAMING");
  model.validate();
  if ((contract.adapterName == "automatic-onnx") != automaticShapeSeed.has_value())
    throw std::invalid_argument("automatic request requires its bound shape seed");
  if (model.artifactReference) {
    // The descriptor graphDigest is the adapter/planning graph identity.  A
    // model reference carries the canonical source graph identity, which is
    // verified during preparation against the inspected source.  They are
    // intentionally distinct identities and must not be compared here.
    model.artifactReference->validate();
  }
  if (contract.serviceName.empty() || contract.serviceName.front() != '/' ||
      contract.taskName.empty() || contract.taskName != input.taskName || requestId.empty() ||
      !attempt || !deadlineMs ||
      !isSupportedNativeGenerationMode(contract.generationMode) ||
      (!contract.tokenizerDigest.empty() && !digest(contract.tokenizerDigest)) ||
      (contract.generationMode == "TOKEN_STREAMING" && contract.tokenizerDigest.empty()) ||
      contract.adapterName != model.adapterId ||
      contract.adapterDescriptorDigest != model.adapter.descriptorDigest() ||
      !digest(contract.adapterCompositionDigest) || !digest(contract.taskDescriptorDigest) ||
      input.inputSchemaDigest != model.adapter.inputSchemaDigest ||
      input.optionsSchemaDigest != model.adapter.optionsSchemaDigest ||
      std::find(model.adapter.tasks.begin(), model.adapter.tasks.end(), input.taskName) == model.adapter.tasks.end())
    throw std::invalid_argument("DI request catalog/task/schema binding mismatch");
  if (input.payload.size() > MAX_WIRE || input.options.size() > MAX_WIRE ||
      input.repositoryReference.size() > MAX_WIRE)
    throw std::invalid_argument("DI request input exceeds limit");

  if (automaticShapeSeed) {
    if (contract.generationMode == "TOKEN_STREAMING") {
      const auto options = nativeParseJson(std::string(input.options.begin(), input.options.end()));
      const auto count = options.is_object() ? options.find("maxNewTokens") : options.end();
      if (!options.is_object() || count == options.end() || !count->is_number_integer() ||
          (!count->is_number_unsigned() && count->get<std::int64_t>() < 0) ||
          count->get<std::uint64_t>() == 0 ||
          count->get<std::uint64_t>() > MAX_NATIVE_GENERATED_TOKENS ||
          count->get<std::uint64_t>() != automaticShapeSeed->maxNewTokens)
        throw std::invalid_argument("automatic seed token budget differs from application options");
    }
    else if (automaticShapeSeed->maxNewTokens != 0) {
      throw std::invalid_argument("automatic tensor-forward request has a generation budget");
    }
  }

  NativeJson reference = NativeJson::object();
  std::string transport;
  if (input.transportMode == NativeInputTransportMode::Inline) {
    transport = "INLINE";
    if (!input.repositoryReference.empty())
      throw std::invalid_argument("inline request contains a repository reference");
  }
  else if (input.transportMode == NativeInputTransportMode::RepositoryReference) {
    transport = "REPO_REF";
    reference = NativeJson::parse(input.repositoryReference);
    if (!reference.is_object() || !input.payload.empty() ||
        reference.value("dataName", std::string{}).find('/') != 0 ||
        reference.value("encrypted", false) != true ||
        !reference.contains("plaintextSize") || !reference.at("plaintextSize").is_number_unsigned() ||
        reference.at("plaintextSize").get<std::uint64_t>() == 0 ||
        reference.value("authorizationScope", std::string{}).empty() ||
        reference.value("protectionEpoch", std::string{}).empty() ||
        reference.value("protectionEpoch", std::string{}) == "plaintext-v1" ||
        !digest(reference.value("manifestDigest", std::string{})) ||
        !digest(reference.value("ciphertextDigest", std::string{})))
      throw std::invalid_argument("DI encrypted input reference is invalid");
  }
  else throw std::invalid_argument("DI input transport is invalid");

  NativeEncodedRequest result;
  const NativeJson revision = model.sourceRevision.empty() ? NativeJson(nullptr) : NativeJson(model.sourceRevision);
  result.modelIntentDigest = model.intentDigest();
  result.logicalInputDigest = transport == "INLINE"
    ? nativePlanningDigest(input.payload.data(), input.payload.size()) : hash(reference);
  const auto optionsDigest = nativePlanningDigest(input.options.data(), input.options.size());
  result.inputManifestDigest = hash({{"input_schema_digest", input.inputSchemaDigest},
    {"options_schema_digest", input.optionsSchemaDigest}, {"input_transport", transport},
    {"input_digest", result.logicalInputDigest}, {"input_reference", reference},
    {"options_digest", optionsDigest.substr(7)}});
  result.invocationId = "invocation:" + hash({{"request_id", requestId},
    {"model", result.modelIntentDigest}}).substr(7, 32);
  NativeJson envelope = {
    {"schema", "ndnsf-di-request-envelope-v2"}, {"schema_version", 2},
    {"canonical_encoding_version", "canonical-json-v1"},
    {"capability_version", "SELECTION_DATAFLOW_V2"}, {"acceptance_predicate_version", "DI_ACCEPTANCE_V2"},
    {"invocation_id", result.invocationId}, {"request_id", requestId}, {"attempt", attempt},
    {"service", contract.serviceName}, {"model_name", model.modelName},
    {"model_identity_hash", result.modelIntentDigest}, {"task_kind", input.taskName},
    {"input_manifest_digest", result.inputManifestDigest}, {"input_payload_b64", base64(input.payload)},
    {"options_payload_b64", selectionGenerationOptions ? std::string{} : base64(input.options)}, {"plan_deadline_ms", deadlineMs},
    {"security_domain", "requester-default"}, {"input_transport", transport}, {"input_reference", reference},
    {"model", {{"name", model.modelName}, {"identity_hash", result.modelIntentDigest},
      {"content_digest", model.contentDigest}, {"semantics_digest", model.semanticsDigest}, {"source_revision", revision}}},
    {"task", {{"name", input.taskName}, {"adapter", contract.adapterName},
      {"adapter_descriptor_digest", contract.adapterDescriptorDigest},
      {"adapter_composition_digest", contract.adapterCompositionDigest},
      {"task_descriptor_digest", contract.taskDescriptorDigest},
      {"generation_mode", contract.generationMode}, {"placement_profile", "DI_PLACEMENT_V3"}}}};
  if (automaticShapeSeed) {
    envelope["schema"] = "ndnsf-di-request-envelope-v3";
    envelope["schema_version"] = 3;
    envelope["automatic_shape_seed"] = nativeAutomaticShapeSeedToJson(*automaticShapeSeed);
    envelope["graph_digest"] = model.graphDigest;
    result.automaticShapeSeed = automaticShapeSeed;
  }
  if (selectionGenerationOptions) {
    envelope["options_transport"] = "SELECTION_GENERATION_V1";
    envelope["options_digest"] = optionsDigest;
  }
  if (!recovery && candidateArtifactInventory && probeAllowlist &&
      !candidateProviders.empty() &&
      candidateProviders.size() <= 16) {
    const ndn::Name requesterName(requesterIdentity);
    if (requesterIdentity.empty() || requesterName.empty() ||
        requesterName.toUri() != requesterIdentity)
      throw std::invalid_argument("DI V4 requester identity is not canonical");
    auto roster = candidateProviders;
    for (const auto& provider : roster) {
      const ndn::Name name(provider);
      if (name.empty() || name.toUri() != provider)
        throw std::invalid_argument("DI placement roster contains a non-canonical Provider name");
    }
    std::sort(roster.begin(), roster.end());
    if (std::adjacent_find(roster.begin(), roster.end()) != roster.end())
      throw std::invalid_argument("DI placement roster contains a duplicate Provider");
    const auto rosterJson = NativeJson(roster);
    const auto& identities = candidateArtifactInventory->identities;
    if (identities.empty() ||
        identities.size() > MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS ||
        nativePlanningDigest(nativeCanonicalJson(NativeJson(identities))) !=
          candidateArtifactInventory->digest)
      throw std::invalid_argument("DI V4 candidate artifact inventory is incomplete or mismatched");
    for (std::size_t i = 0; i < identities.size(); ++i) {
      const auto& identity = identities[i];
      const bool rankValid = identity.is_object() && identity.contains("rank") &&
        (identity.at("rank").is_number_unsigned() ||
         (identity.at("rank").is_number_integer() &&
          identity.at("rank").get<std::int64_t>() >= 0));
      if (!identity.is_object() || identity.size() != 7 ||
          !identity.contains("artifact_digest") ||
          !identity.contains("model_manifest_digest") ||
          !identity.contains("graph_digest") || !identity.contains("role") ||
          !identity.contains("rank") || !identity.contains("backend") ||
          !identity.contains("protection_epoch") || !rankValid ||
          !identity.at("artifact_digest").is_string() ||
          !identity.at("model_manifest_digest").is_string() ||
          !identity.at("graph_digest").is_string() ||
          !identity.at("role").is_string() ||
          !identity.at("backend").is_string() ||
          !identity.at("protection_epoch").is_string() ||
          !digest(identity.at("artifact_digest").get<std::string>()) ||
          !digest(identity.at("model_manifest_digest").get<std::string>()) ||
          !digest(identity.at("graph_digest").get<std::string>()) ||
          identity.at("role").get<std::string>().empty() ||
          identity.at("backend").get<std::string>().empty() ||
          identity.at("protection_epoch").get<std::string>().empty() ||
          (i != 0 && nativeCanonicalJson(identities[i - 1]) >=
                     nativeCanonicalJson(identity)))
        throw std::invalid_argument("DI V4 candidate artifact inventory is not canonical");
    }
    auto toJsonPairs = [&roster] (const std::vector<NativePlacementProbePairV4>& pairs) {
      NativeJson values = NativeJson::array();
      std::set<std::pair<std::string, std::string>> unique;
      std::optional<std::pair<std::string, std::string>> previous;
      for (const auto& pair : pairs) {
        const ndn::Name source(pair.sourceProvider);
        const ndn::Name destination(pair.destinationProvider);
        const auto current = std::make_pair(pair.sourceProvider,
                                            pair.destinationProvider);
        if (source.empty() || destination.empty() ||
            source.toUri() != pair.sourceProvider ||
            destination.toUri() != pair.destinationProvider ||
            pair.sourceProvider == pair.destinationProvider ||
            !std::binary_search(roster.begin(), roster.end(), pair.sourceProvider) ||
            !std::binary_search(roster.begin(), roster.end(), pair.destinationProvider) ||
            !unique.emplace(pair.sourceProvider, pair.destinationProvider).second ||
            (previous && !(*previous < current)))
          throw std::invalid_argument("DI V4 probe pair scope is invalid");
        previous = current;
        values.push_back(NativeJson{{"src_provider", pair.sourceProvider},
                                    {"dst_provider", pair.destinationProvider}});
      }
      if (values.size() > 16)
        throw std::invalid_argument("DI V4 probe pair scope exceeds its bound");
      return values;
    };
    const auto rttPairs = toJsonPairs(probeAllowlist->rttPairs);
    const auto goodputEdges = toJsonPairs(probeAllowlist->goodputEdges);
    const auto candidateSetDigest = hash(rosterJson);
    const auto probeScopeDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"schema", "ndnsf-di-placement-probe-scope-v1"},
      {"request_id", requestId}, {"attempt", attempt},
      {"requester_identity", requesterIdentity}, {"deadline_ms", deadlineMs},
      {"service", contract.serviceName}, {"model_identity_hash", result.modelIntentDigest},
      {"graph_digest", model.graphDigest}, {"candidate_set_digest", candidateSetDigest},
      {"candidate_artifact_set_digest", candidateArtifactInventory->digest},
      {"rtt_pairs", rttPairs}, {"goodput_edges", goodputEdges}}));
    envelope["task"]["placement_profile"] = "DI_PLACEMENT_V4";
    envelope["graph_digest"] = model.graphDigest;
    envelope["candidate_providers"] = rosterJson;
    envelope["candidate_set_digest"] = candidateSetDigest;
    envelope["candidate_artifacts"] = identities;
    envelope["candidate_artifact_set_digest"] = candidateArtifactInventory->digest;
    envelope["rtt_pairs"] = rttPairs;
    envelope["goodput_edges"] = goodputEdges;
    envelope["probe_scope_digest"] = probeScopeDigest;
    envelope["requester_identity"] = requesterIdentity;
    result.candidateArtifacts = identities;
    result.candidateProviders = roster;
    result.candidateSetDigest = candidateSetDigest;
    result.candidateArtifactSetDigest = candidateArtifactInventory->digest;
    result.allowedRttPairs = rttPairs;
    result.allowedGoodputEdges = goodputEdges;
    result.probeScopeDigest = probeScopeDigest;
    result.requesterIdentity = requesterIdentity;
  }
  if (model.artifactReference)
    envelope["model_reference"] = nativeParseJson(model.artifactReference->canonicalJson());
  if (recovery) {
    const auto& value = *recovery;
    if (attempt != 2 || value.generationId.size() != 32 ||
        value.generationId.find_first_not_of("0123456789abcdef") != std::string::npos ||
        value.originalRequestId.empty() || value.recoveryRequestId != requestId ||
        value.originalRequestId == value.recoveryRequestId ||
        value.failedProvider.empty() || value.failedProvider.front() != '/' ||
        !digest(value.priorPlanDigest) || value.originalInputManifestDigest != result.inputManifestDigest ||
        value.committedTokenIds.size() > 4094 ||
        std::any_of(value.committedTokenIds.begin(), value.committedTokenIds.end(), [](auto token) { return token < 0; }))
      throw std::invalid_argument("invalid GenerationRecoveryV1 binding");
    std::string prefix;
    for (const auto token : value.committedTokenIds) {
      if (!prefix.empty()) prefix += ',';
      prefix += std::to_string(token);
    }
    envelope["task"]["generation_recovery"] = {
      {"schema", "ndnsf-di-generation-recovery-v1"}, {"logical_generation_id", value.generationId},
      {"original_request_id", value.originalRequestId}, {"recovery_request_id", value.recoveryRequestId},
      {"attempt", 2}, {"original_input_manifest_digest", value.originalInputManifestDigest},
      {"prior_plan_digest", value.priorPlanDigest}, {"failed_provider", value.failedProvider},
      {"committed_token_ids", value.committedTokenIds}, {"committed_token_count", value.committedTokenIds.size()},
      {"committed_prefix_digest", nativePlanningDigest(prefix)}};
    result.recovery = value;
  }
  const auto encodeDiscovery = [&](const std::string& wire) {
    if (automaticShapeSeed)
      return nativeCanonicalJson(automaticDiscoverySummary(envelope, nativePlanningDigest(wire)));
    auto& inputValue = envelope.at("input_payload_b64");
    auto payload = std::move(inputValue);
    inputValue = "";
    // The ACK path validates options transport from the control fields.
    // The actual options bytes belong to the protected request or Selection.
    auto& optionsValue = envelope.at("options_payload_b64");
    auto options = std::move(optionsValue);
    optionsValue = "";
    auto discovery = nativeCanonicalJson(envelope);
    inputValue = std::move(payload);
    optionsValue = std::move(options);
    return discovery;
  };
  auto wire = nativeCanonicalJson(envelope);
  if (automaticShapeSeed || prepareLegacyDiscovery) {
    using ndn_service_framework::RequestMessage;
    auto discovery = encodeDiscovery(wire);
    if (discovery.size() > RequestMessage::MAX_DISCOVERY_PAYLOAD_SIZE &&
        envelope.at("task").at("placement_profile") == "DI_PLACEMENT_V4") {
      // Keep the optional V4 scope whole or use the existing V3 profile.
      // Never publish a partial candidate/probe inventory.
      for (const auto* key : {"candidate_providers", "candidate_set_digest", "candidate_artifacts",
                              "candidate_artifact_set_digest", "rtt_pairs", "goodput_edges",
                              "probe_scope_digest", "requester_identity"}) envelope.erase(key);
      envelope["task"]["placement_profile"] = "DI_PLACEMENT_V3";
      result.candidateArtifacts.clear(); result.candidateProviders.clear();
      result.candidateSetDigest.clear(); result.candidateArtifactSetDigest.clear();
      result.allowedRttPairs.clear(); result.allowedGoodputEdges.clear();
      result.probeScopeDigest.clear(); result.requesterIdentity.clear();
      wire = nativeCanonicalJson(envelope);
      discovery = encodeDiscovery(wire);
      if (statistics) {
        statistics->addCount(automaticShapeSeed ? "request.automaticDiscoveryV4BudgetFallbacks" :
                                                  "request.legacyDiscoveryV4BudgetFallbacks", 1);
      }
    }
    if (discovery.size() > RequestMessage::MAX_DISCOVERY_PAYLOAD_SIZE)
      throw std::invalid_argument("DI discovery exceeds the Core metadata limit");
    result.discoveryPayload.emplace(discovery.begin(), discovery.end());
  }
  if (wire.size() > MAX_WIRE) throw std::invalid_argument("DI request envelope exceeds limit");
  result.wire.assign(wire.begin(), wire.end());
  result.requestContractDigest = nativePlanningDigest(wire);
  if (statistics) {
    const auto inputBytes = envelope.at("input_payload_b64").get_ref<const std::string&>().size();
    const auto optionsBytes = envelope.at("options_payload_b64").get_ref<const std::string&>().size();
    statistics->addCount("request.envelopeRepresentationBytes", wire.size());
    statistics->addCount("request.encodedInputValueBytes", inputBytes);
    statistics->addCount("request.encodedOptionsValueBytes", optionsBytes);
    statistics->addCount("request.envelopeControlRepresentationBytes", wire.size() - inputBytes);
    statistics->addCount("request.successfulEnvelopeEncodes", 1);
    statistics->addCount(selectionGenerationOptions
      ? "request.selectionOptionsEnvelopeEncodes" : "request.inlineOptionsEnvelopeEncodes", 1);
  }
  return result;
}

} // namespace

NativeEncodedRequest encodeNativeRequestEnvelope(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery,
  const std::vector<std::string>& candidateProviders,
  const std::optional<NativeCandidateArtifactInventoryV4>& candidateArtifactInventory,
  const std::optional<NativePlacementProbeAllowlistV4>& probeAllowlist,
  std::string requesterIdentity,
  const std::optional<NativeAutomaticShapeSeed>& automaticShapeSeed)
{
  return encodeNativeRequestEnvelopeImpl(model, input, contract, requestId, attempt,
    deadlineMs, recovery, candidateProviders, candidateArtifactInventory,
    probeAllowlist, std::move(requesterIdentity), false, {}, automaticShapeSeed);
}

namespace detail {
NativeEncodedRequest encodeNativeRequestEnvelopeForRuntime(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery,
  const std::vector<std::string>& candidateProviders,
  const std::optional<NativeCandidateArtifactInventoryV4>& candidateArtifactInventory,
  const std::optional<NativePlacementProbeAllowlistV4>& probeAllowlist,
  std::string requesterIdentity, const std::string& optionsTransport,
  const std::shared_ptr<NativeWorkStatistics>& statistics,
  const std::optional<NativeAutomaticShapeSeed>& automaticShapeSeed)
{
  if (optionsTransport != "INLINE" && optionsTransport != "SELECTION_GENERATION_V1")
    throw std::invalid_argument("unsupported native request options transport");
  return encodeNativeRequestEnvelopeImpl(model, input, contract, requestId, attempt,
    deadlineMs, recovery, candidateProviders, candidateArtifactInventory,
    probeAllowlist, std::move(requesterIdentity), optionsTransport != "INLINE", statistics,
    automaticShapeSeed, true);
}

bool requiresSelectionGenerationOptions(const std::string& requestWire,
                                        bool allowSelectionGenerationOptions)
{
  // Classify and validate automatic metadata before the INLINE early return.
  const auto parsed = parseNativeRequestEnvelope(requestWire);
  if (!parsed) return false;
  const auto& root = *parsed;
  const auto mode = root.value("options_transport", std::string("INLINE"));
  if (mode == "INLINE" && !root.contains("options_digest")) return false;
  if (mode != "SELECTION_GENERATION_V1" || !allowSelectionGenerationOptions ||
      root.value("options_payload_b64", NativeJson{}) != "" ||
      !root.contains("options_digest") || !root.at("options_digest").is_string() ||
      !digest(root.at("options_digest").get<std::string>()) ||
      !root.contains("task") || !root.at("task").is_object() ||
      root.at("task").value("generation_mode", NativeJson{}) != "TOKEN_STREAMING")
    throw std::invalid_argument("invalid or disabled native request options transport");
  return true;
}
} // namespace detail

} // namespace ndnsf::di
