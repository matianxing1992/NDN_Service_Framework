#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"

#include <algorithm>
#include <set>
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

// Canonical catalog data names are absolute NDN names: a leading '/',
// non-empty components, no control characters.
bool ndnName(const std::string& value)
{
  if (value.size() < 2 || value.front() != '/') return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c < 0x21 || c == 0x7f) return false;
    if (c == '/' && (i + 1 == value.size() || value[i + 1] == '/')) return false;
  }
  return true;
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
    if (!ndnName(item.second)) {
      throw std::invalid_argument("native artifact binding source is not an NDN name");
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
  // Publication must be driven by the placement of this very request/attempt
  // over the inspected model; a stale or foreign proposal must never reach
  // the catalog port.
  if (proposal.requestId != control.requestId || proposal.attempt != control.attempt ||
      proposal.modelDigest != model.descriptor.contentDigest ||
      proposal.graphDigest != model.graph.graphDigest) {
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  const auto& roles = proposal.executionPlan.roles;
  if (roles.empty() ||
      std::set<std::string>(roles.begin(), roles.end()).size() != roles.size()) {
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  if (!m_artifacts) throw std::runtime_error("DI_NATIVE_ARTIFACT_PORT_NOT_CONFIGURED");
  auto result = m_artifacts(model, proposal, control);
  control.requireActive();
  result.validate();
  // The binding must cover exactly the roles the placed plan requires: a
  // missing role leaves a provider unassemblable, an extra role would smuggle
  // provider-side assembly into requester preparation.
  if (result.sourceByRole.size() != roles.size()) {
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  for (const auto& role : roles) {
    if (result.sourceByRole.find(role) == result.sourceByRole.end()) {
      throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
    }
  }
  result.requestId = control.requestId;
  result.attempt = control.attempt;
  result.modelDigest = model.descriptor.contentDigest;
  result.graphDigest = model.graph.graphDigest;
  return result;
}

} // namespace ndnsf::di
