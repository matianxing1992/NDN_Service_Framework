#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeRequestOptionsTransport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include <openssl/evp.h>
#include <ndn-cxx/name.hpp>
#include <boost/property_tree/json_parser.hpp>
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
  std::string requesterIdentity, bool selectionGenerationOptions)
{
  if (selectionGenerationOptions && contract.generationMode != "TOKEN_STREAMING")
    throw std::invalid_argument("Selection generation options require TOKEN_STREAMING");
  model.validate();
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
  const auto wire = nativeCanonicalJson(envelope);
  if (wire.size() > MAX_WIRE) throw std::invalid_argument("DI request envelope exceeds limit");
  result.wire.assign(wire.begin(), wire.end());
  result.requestContractDigest = nativePlanningDigest(wire);
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
  std::string requesterIdentity)
{
  return encodeNativeRequestEnvelopeImpl(model, input, contract, requestId, attempt,
    deadlineMs, recovery, candidateProviders, candidateArtifactInventory,
    probeAllowlist, std::move(requesterIdentity), false);
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
  std::string requesterIdentity, const std::string& optionsTransport)
{
  if (optionsTransport != "INLINE" && optionsTransport != "SELECTION_GENERATION_V1")
    throw std::invalid_argument("unsupported native request options transport");
  return encodeNativeRequestEnvelopeImpl(model, input, contract, requestId, attempt,
    deadlineMs, recovery, candidateProviders, candidateArtifactInventory,
    probeAllowlist, std::move(requesterIdentity), optionsTransport != "INLINE");
}

bool requiresSelectionGenerationOptions(const std::string& requestWire,
                                        bool allowSelectionGenerationOptions)
{
  // Decode keys before checking the mode: escaped JSON keys must not bypass
  // deployment permission or the Selection-only contract.
  NativeJson root;
  try { root = nativeParseJson(requestWire); }
  catch (const std::exception&) {
    // Classification only: Boost preserves duplicate keys and decodes escapes.
    // Never downgrade a malformed native envelope to the legacy receiver path,
    // including when any duplicate schema value identifies the native schema.
    boost::property_tree::ptree legacy;
    try {
      std::istringstream input(requestWire);
      boost::property_tree::read_json(input, legacy);
    }
    catch (const boost::property_tree::json_parser::json_parser_error&) { return false; }
    for (const auto& item : legacy) {
      if (item.first == "schema" && item.second.data() == "ndnsf-di-request-envelope-v2")
        throw std::invalid_argument("native request envelope failed strict JSON validation");
    }
    return false;
  }
  if (!root.is_object() || root.value("schema", NativeJson{}) !=
      "ndnsf-di-request-envelope-v2")
    return false;
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
