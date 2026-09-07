#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"

#include <algorithm>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {
bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::string hashBytes(const std::vector<std::uint8_t>& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size(), hash);
  std::ostringstream output;
  output << "sha256:";
  for (const auto byte : hash)
    output << "0123456789abcdef"[byte >> 4] << "0123456789abcdef"[byte & 15];
  return output.str();
}
} // namespace

void NativePreparedInput::validate() const
{
  if (modelName.empty() || !digest(modelDigest) || taskName.empty() ||
      !digest(inputSchemaDigest) || !digest(optionsSchemaDigest) ||
      payload.empty() || adapterId.empty() || adapterVersion.empty() || !encoded ||
      deadline <= std::chrono::steady_clock::now()) {
    throw std::invalid_argument("native prepared input is incomplete");
  }
  if (!repositoryReference.empty()) {
    throw std::invalid_argument("prepared input cannot retain an unverified repository reference");
  }
}

void NativeInspectedModel::validate() const
{
  descriptor.validate();
  graph.validate(descriptor);
  if (canonicalSourceName.empty() || !digest(canonicalSourceDigest)) {
    throw std::invalid_argument("native inspected model source identity is incomplete");
  }
}

void NativeArtifactBinding::validate() const
{
  if (sourceByRole.empty() || sourceByRole.size() != artifactDigestByRole.size() ||
      !digest(manifestDigest) || !digest(recipeDigest)) {
    throw std::invalid_argument("native artifact binding is incomplete");
  }
  for (const auto& item : sourceByRole) {
    if (item.first.empty() || item.second.empty() ||
        !digest(artifactDigestByRole.at(item.first))) {
      throw std::invalid_argument("native artifact binding role identity is invalid");
    }
  }
}

void NativeRequestControl::requireActive() const
{
  if (requestId.empty() || attempt == 0 || deadline <= std::chrono::steady_clock::now() ||
      (cancelled && cancelled())) {
    throw std::runtime_error("DI_NATIVE_REQUEST_CANCELLED_OR_EXPIRED");
  }
}

NativeRequestPreparation::NativeRequestPreparation(
  std::shared_ptr<const NativeAdapterRegistry> adapters, InspectPort inspect,
  ArtifactPort artifacts)
  : m_adapters(std::move(adapters)), m_inspect(std::move(inspect)),
    m_artifacts(std::move(artifacts))
{
  if (!m_adapters) throw std::invalid_argument("native preparation requires adapter registry");
}

NativePreparedInput NativeRequestPreparation::prepareInput(
  const NativeModelDescriptor& model, std::string taskName,
  std::string inputSchemaDigest, std::string optionsSchemaDigest,
  std::vector<std::uint8_t> payload, std::string repositoryReference,
  std::chrono::steady_clock::time_point deadline) const
{
  model.validate();
  if (taskName.empty() || !digest(inputSchemaDigest) || !digest(optionsSchemaDigest) ||
      payload.empty() || !repositoryReference.empty()) {
    throw std::invalid_argument("native application input is invalid");
  }
  auto adapter = m_adapters->find(model.adapterId);
  if (!adapter || adapter->adapterVersion() != model.adapterVersion) {
    throw std::invalid_argument("native model adapter is unavailable");
  }
  if (deadline <= std::chrono::steady_clock::now()) {
    throw std::runtime_error("DI_NATIVE_REQUEST_DEADLINE_EXPIRED");
  }
  auto encoded = adapter->encodeInput(payload);
  if (encoded.empty()) throw std::runtime_error("DI_NATIVE_INPUT_ENCODING_EMPTY");
  NativePreparedInput result{model.modelName, model.contentDigest, std::move(taskName),
                             std::move(inputSchemaDigest),
                             std::move(optionsSchemaDigest), std::move(encoded), {},
                             deadline, model.adapterId, model.adapterVersion, true};
  result.validate();
  return result;
}

NativeInspectedModel NativeRequestPreparation::inspectModel(
  const NativePreparedInput& input) const
{
  input.validate();
  auto adapter = m_adapters->find(input.adapterId);
  if (!adapter) throw std::invalid_argument("native model adapter is unavailable");
  auto descriptor = adapter->inspect(input.modelName, input.modelDigest);
  descriptor.validate();
  if (descriptor.adapterId != input.adapterId || descriptor.adapterVersion != input.adapterVersion) {
    throw std::runtime_error("DI_NATIVE_MODEL_ADAPTER_IDENTITY_MISMATCH");
  }
  if (!m_inspect) throw std::runtime_error("DI_NATIVE_MODEL_GRAPH_PORT_NOT_CONFIGURED");
  auto graph = m_inspect(input, descriptor);
  graph.validate(descriptor);
  NativeInspectedModel result{descriptor, std::move(graph),
                              "/NDNSF/DI/MODEL/" + descriptor.contentDigest,
                              descriptor.contentDigest};
  result.validate();
  return result;
}

NativeArtifactBinding NativeRequestPreparation::ensureArtifacts(
  const NativeInspectedModel& model, const NativePlacementProposal& proposal,
  const NativeRequestControl& control) const
{
  model.validate();
  control.requireActive();
  if (!m_artifacts) throw std::runtime_error("DI_NATIVE_ARTIFACT_PORT_NOT_CONFIGURED");
  auto result = m_artifacts(model, proposal, control);
  control.requireActive();
  result.validate();
  return result;
}

NativeProviderPlanningView NativeOfferAdmission::verify(
  const NativeAckEvidence& ack, const NativeOfferPolicySnapshot& policy,
  const NativeOfferBindingContext& context, std::uint64_t nowMs) const
{
  if (!ack.coreAuthenticated || ack.requestId != context.requestId ||
      ack.attempt != context.attempt || ack.serviceName != context.serviceName ||
      ack.modelDigest != context.modelDigest || ack.graphDigest != context.graphDigest ||
      ack.provider.empty() || ack.signerIdentity.empty() || ack.controllerVersion.empty() ||
      !digest(ack.offerDigest) || !digest(policy.policyDigest) ||
      policy.expiresAtMs <= nowMs || ack.expiresAtMs <= nowMs ||
      !contains(policy.acceptedProviders, ack.provider) ||
      !contains(policy.acceptedServices, ack.serviceName) ||
      !contains(policy.acceptedSignerIdentities, ack.signerIdentity)) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED");
  }
  NativeProviderPlanningView result;
  result.provider = ack.provider;
  result.offerDigest = ack.offerDigest;
  result.acceptedRoles = policy.acceptedRoles;
  result.backends = policy.backends;
  result.residencyDigests = policy.residencyDigests;
  result.freeBytes = policy.freeBytes;
  result.resourceSequence = policy.resourceSequence;
  result.preparationAccepted = true;
  result.executionAllowed = true;
  result.validate();
  return result;
}

} // namespace ndnsf::di
