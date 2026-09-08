#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

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

bool sameModel(const NativeModelDescriptor& a, const NativeModelDescriptor& b)
{
  return a.modelName == b.modelName && a.contentDigest == b.contentDigest &&
    a.semanticsDigest == b.semanticsDigest && a.graphDigest == b.graphDigest &&
    a.modelFormat == b.modelFormat && a.precision == b.precision &&
    a.adapterId == b.adapterId && a.adapterVersion == b.adapterVersion;
}
} // namespace

void NativePreparedInput::validate() const
{
  expectedModel.validate();
  if (modelName != expectedModel.modelName || modelDigest != expectedModel.contentDigest ||
      adapterId != expectedModel.adapterId || adapterVersion != expectedModel.adapterVersion)
    throw std::invalid_argument("prepared input model binding is inconsistent");
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
  if (!ndnName(canonicalSourceName) || !digest(canonicalSourceDigest) ||
      !digest(modelManifestDigest) || !digest(canonicalGraphDigest)) {
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
  ArtifactPort artifacts, RolePort roles)
  : m_adapters(std::move(adapters)), m_inspect(std::move(inspect)),
    m_artifacts(std::move(artifacts)), m_roles(std::move(roles))
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
                             deadline, model.adapterId, model.adapterVersion, true, model};
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
  if (!sameModel(descriptor, input.expectedModel))
    throw std::runtime_error("DI_NATIVE_MODEL_INSPECTION_BINDING_MISMATCH");
  if (!m_inspect) throw std::runtime_error("DI_NATIVE_MODEL_GRAPH_PORT_NOT_CONFIGURED");
  auto result = m_inspect(input, descriptor);
  input.validate(); // A late inspection cannot extend the request deadline.
  result.validate();
  if (!sameModel(result.descriptor, input.expectedModel))
    throw std::runtime_error("DI_NATIVE_MODEL_INSPECTION_BINDING_MISMATCH");
  return result;
}

std::vector<NativeSelectionRoleV3> NativeRequestPreparation::prepareRoles(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const NativeRequestControl& control) const
{
  model.validate();
  control.requireActive();
  candidate.validate(model.graph);
  if (!sameModel(candidate.model, model.descriptor))
    throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
  if (!m_roles) throw std::runtime_error("DI_NATIVE_ROLE_PORT_NOT_CONFIGURED");
  auto result = m_roles(model, candidate, control);
  control.requireActive();
  validateRoles(model, candidate, result);
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return std::tie(a.role, a.rank) < std::tie(b.role, b.rank);
  });
  return result;
}


void NativeRequestPreparation::validateRoles(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const std::vector<NativeSelectionRoleV3>& roles)
{
  model.validate();
  candidate.validate(model.graph);
  if (!sameModel(candidate.model, model.descriptor))
    throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
  std::set<std::pair<std::string, std::uint64_t>> expected;
  for (const auto& role : candidate.executionPlan.roles) {
    const auto degree = candidate.tensorDegreesByRole.find(role);
    if (degree == candidate.tensorDegreesByRole.end() || !degree->second || degree->second > 1024)
      throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
    for (std::uint64_t rank = 0; rank < degree->second; ++rank) expected.emplace(role, rank);
  }
  if (roles.size() != expected.size()) throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
  for (const auto& role : roles) {
    if (!expected.erase({role.role, role.rank}) || role.graphDigest != model.canonicalGraphDigest ||
        role.modelManifestDigest != model.modelManifestDigest ||
        role.adapterId != model.descriptor.adapterId || role.adapterVersion != model.descriptor.adapterVersion ||
        !digest(role.recipeDigest) || !digest(role.artifactProfileDigest) ||
        !digest(role.canonicalInitializerDigest) || !digest(role.adapterDescriptorDigest) ||
        !digest(role.assemblerDescriptorDigest) || role.backendAbi.empty() || role.precision.empty() ||
        role.protectionEpoch.empty() || role.nodeIndices.empty() || !role.maxSourceBytes ||
        !role.maxAssembledBytes || !role.maxNodes)
      throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
    const auto specific = candidate.rankArtifactDigestsByRole.find(role.role);
    const auto& artifacts = specific == candidate.rankArtifactDigestsByRole.end()
      ? candidate.artifactsByRole.at(role.role) : specific->second;
    if (role.rank >= artifacts.size() || role.artifactDigest != artifacts[role.rank])
      throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
    const auto& backends = candidate.requirementsByRole.at(role.role).backends;
    const auto& budget = candidate.requirementsByRole.at(role.role);
    const long double requiredMb = std::ceil((static_cast<long double>(budget.weightBytes) +
      budget.workspaceBytes + budget.activationBytes + budget.transientBytes) * budget.safetyMargin / (1024 * 1024));
    if (static_cast<long double>(role.requiredDeviceMemoryMb) < requiredMb)
      throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
    if (std::none_of(backends.begin(), backends.end(), [&](const auto& backend) {
          return role.backend == backend || role.backend == backend + "-cpu" || role.backend == backend + "-cuda";
        }) || std::any_of(role.nodeIndices.begin(), role.nodeIndices.end(), [&](auto index) {
          return index >= model.graph.nodes.size();
        })) throw std::runtime_error("DI_NATIVE_ROLE_BINDING_MISMATCH");
  }
}

NativeArtifactBinding NativeRequestPreparation::ensureArtifacts(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const NativeRolePlacementProposalV3& proposal,
  const NativeRequestControl& control) const
{
  model.validate();
  control.requireActive();
  // Publication must be driven by the placement of this very request/attempt
  // over the inspected model; a stale or foreign proposal must never reach
  // the catalog port.
  const auto& context = proposal.context;
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (context.requestId != control.requestId || context.attempt != control.attempt ||
      context.modelDigest != model.descriptor.contentDigest ||
      context.graphDigest != model.graph.graphDigest || context.serviceName.empty() ||
      nowMs < 0 || context.deadlineMs <= static_cast<std::uint64_t>(nowMs) ||
      !digest(proposal.ackClosedDigest)) {
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  const auto& roles = proposal.roles;
  std::set<std::string> selected, providers;
  for (const auto& role : roles) selected.insert(role.selectedRole);
  if (roles.empty() || selected.size() != roles.size() ||
      proposal.providerByRole.size() != roles.size()) {
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  proposal.strategy.validate();
  validateRoles(model, candidate, roles);
  for (const auto& role : roles) {
    validateNativeAssembly(role);
    const auto degree = candidate.tensorDegreesByRole.at(role.role);
    const auto key = degree == 1 ? role.role : role.role + "#" + std::to_string(role.rank);
    const auto assignment = proposal.providerByRole.find(key);
    if (role.selectedRole != key || assignment == proposal.providerByRole.end() ||
        assignment->second.empty() || !providers.insert(assignment->second).second)
      throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
    const auto offer = proposal.offerDigestByProvider.find(assignment->second);
    if (offer == proposal.offerDigestByProvider.end() || !digest(offer->second))
      throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  if (providers.size() != proposal.offerDigestByProvider.size())
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  // The requester validates placement against admitted observations before
  // this call. Publication receives only the checked candidate/role contract;
  // publishing canonical objects never authorizes a Provider to execute.
  if (!m_artifacts) throw std::runtime_error("DI_NATIVE_ARTIFACT_PORT_NOT_CONFIGURED");
  control.requireActive();
  auto result = m_artifacts(model, candidate, roles, control);
  control.requireActive();
  result.validate();
  // The binding must cover exactly the roles the placed plan requires: a
  // missing role leaves a provider unassemblable, an extra role would smuggle
  // provider-side assembly into requester preparation.
  if (result.manifestDigest != model.modelManifestDigest || result.sourceByRole.size() != roles.size()) {
    throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
  }
  for (const auto& role : roles) {
    const auto artifact = result.artifactDigestByRole.find(role.selectedRole);
    if (result.sourceByRole.find(role.selectedRole) == result.sourceByRole.end() ||
        artifact == result.artifactDigestByRole.end() || artifact->second != role.artifactDigest) {
      throw std::runtime_error("DI_NATIVE_ARTIFACT_BINDING_MISMATCH");
    }
  }
  result.requestId = control.requestId;
  result.attempt = control.attempt;
  result.modelDigest = model.descriptor.contentDigest;
  result.graphDigest = model.graph.graphDigest;
  result.canonicalGraphDigest = model.canonicalGraphDigest;
  return result;
}

} // namespace ndnsf::di
