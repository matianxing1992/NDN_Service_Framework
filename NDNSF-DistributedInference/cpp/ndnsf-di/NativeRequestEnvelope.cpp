#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <openssl/evp.h>
#include <algorithm>

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

NativeEncodedRequest encodeNativeRequestEnvelope(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs)
{
  model.validate();
  if (contract.serviceName.empty() || contract.serviceName.front() != '/' ||
      contract.taskName.empty() || contract.taskName != input.taskName || requestId.empty() ||
      !attempt || !deadlineMs || contract.generationMode.empty() ||
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
  const auto wire = nativeCanonicalJson({
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
      {"generation_mode", contract.generationMode}, {"placement_profile", "DI_PLACEMENT_V3"}}}});
  if (wire.size() > MAX_WIRE) throw std::invalid_argument("DI request envelope exceeds limit");
  result.wire.assign(wire.begin(), wire.end());
  result.requestContractDigest = nativePlanningDigest(wire);
  return result;
}

} // namespace ndnsf::di
