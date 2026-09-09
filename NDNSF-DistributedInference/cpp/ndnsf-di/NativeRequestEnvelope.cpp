#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <cctype>
#include <limits>

namespace ndnsf::di {
namespace {
constexpr std::size_t MAX_WIRE = 4 * 1024 * 1024;
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
  if (!result.maxGeneratedTokens || result.maxGeneratedTokens > 64 || !digest(result.tokenizerDigest) ||
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

NativeEncodedRequest encodeNativeRequestEnvelope(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery)
{
  model.validate();
  if (contract.serviceName.empty() || contract.serviceName.front() != '/' ||
      contract.taskName.empty() || contract.taskName != input.taskName || requestId.empty() ||
      !attempt || !deadlineMs ||
      !isSupportedNativeGenerationMode(contract.generationMode) ||
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
  result.inputManifestDigest = hash({{"input_schema_digest", input.inputSchemaDigest},
    {"options_schema_digest", input.optionsSchemaDigest}, {"input_transport", transport},
    {"input_digest", result.logicalInputDigest}, {"input_reference", reference},
    {"options_digest", nativePlanningDigest(input.options.data(), input.options.size()).substr(7)}});
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
    {"options_payload_b64", base64(input.options)}, {"plan_deadline_ms", deadlineMs},
    {"security_domain", "requester-default"}, {"input_transport", transport}, {"input_reference", reference},
    {"model", {{"name", model.modelName}, {"identity_hash", result.modelIntentDigest},
      {"content_digest", model.contentDigest}, {"semantics_digest", model.semanticsDigest}, {"source_revision", revision}}},
    {"task", {{"name", input.taskName}, {"adapter", contract.adapterName},
      {"adapter_descriptor_digest", contract.adapterDescriptorDigest},
      {"adapter_composition_digest", contract.adapterCompositionDigest},
      {"task_descriptor_digest", contract.taskDescriptorDigest},
      {"generation_mode", contract.generationMode}, {"placement_profile", "DI_PLACEMENT_V3"}}}};
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

} // namespace ndnsf::di
