#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>

namespace ndnsf::di {
namespace {
bool contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}
bool sameAssemblyTensors(const std::vector<NativeAssemblyTensorContractV3>& left,
                         const std::vector<NativeAssemblyTensorContractV3>& right)
{
  return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
    [](const auto& a, const auto& b) {
      return a.name == b.name && a.dtype == b.dtype && a.shape == b.shape;
    });
}

bool samePreparedAssemblyMetadata(const NativeSelectionRoleV3& prepared,
                                  const NativeSelectionRoleV3& proposed)
{
  // Match the assembly wire fields in NativeSelectionJson.cpp. Placement
  // intentionally substitutes backend/deviceSet; selectedRole is checked
  // separately, and materializedRole is not part of this wire identity.
  return prepared.role == proposed.role && prepared.rank == proposed.rank &&
    prepared.layerBegin == proposed.layerBegin && prepared.layerEnd == proposed.layerEnd &&
    prepared.requiredDeviceMemoryMb == proposed.requiredDeviceMemoryMb &&
    prepared.artifactDigest == proposed.artifactDigest &&
    prepared.recipeDigest == proposed.recipeDigest && prepared.roleKind == proposed.roleKind &&
    prepared.adapterId == proposed.adapterId && prepared.adapterVersion == proposed.adapterVersion &&
    prepared.modelManifestDigest == proposed.modelManifestDigest &&
    prepared.artifactProfileDigest == proposed.artifactProfileDigest &&
    prepared.graphDigest == proposed.graphDigest &&
    prepared.canonicalInitializerDigest == proposed.canonicalInitializerDigest &&
    prepared.adapterDescriptorDigest == proposed.adapterDescriptorDigest &&
    prepared.assemblerDescriptorDigest == proposed.assemblerDescriptorDigest &&
    prepared.backendAbi == proposed.backendAbi && prepared.nodeIndices == proposed.nodeIndices &&
    sameAssemblyTensors(prepared.expectedInputs, proposed.expectedInputs) &&
    sameAssemblyTensors(prepared.expectedOutputs, proposed.expectedOutputs) &&
    prepared.precision == proposed.precision && prepared.quantization == proposed.quantization &&
    prepared.layout == proposed.layout && prepared.padding == proposed.padding &&
    prepared.protectionEpoch == proposed.protectionEpoch && prepared.mergeKind == proposed.mergeKind &&
    prepared.postprocessIdentity == proposed.postprocessIdentity &&
    prepared.postprocessOutputName == proposed.postprocessOutputName &&
    std::isfinite(prepared.postprocessConfidenceThreshold) &&
    std::isfinite(proposed.postprocessConfidenceThreshold) &&
    prepared.postprocessConfidenceThreshold == proposed.postprocessConfidenceThreshold &&
    std::signbit(prepared.postprocessConfidenceThreshold) ==
      std::signbit(proposed.postprocessConfidenceThreshold) &&
    prepared.postprocessSort == proposed.postprocessSort &&
    prepared.maxSourceBytes == proposed.maxSourceBytes &&
    prepared.maxAssembledBytes == proposed.maxAssembledBytes &&
    prepared.maxNodes == proposed.maxNodes;
}
bool cpuBackend(std::string value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value == "cpu" || (value.size() >= 4 && value.substr(value.size() - 4) == "-cpu");
}
bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::optional<std::uint64_t> jsonUnsigned(const NativeJson& value);
std::optional<double> jsonNonnegativeNumber(const NativeJson& value);
using Reuse = std::tuple<int, std::uint64_t, double, double>;
Reuse reuseCost(const NativeSelectionRoleV3& role, const NativeObservedProviderOfferV3& offer,
                const std::string& device, const std::string& backend, std::uint64_t nowMs,
                const ExtensionControl* control)
{
  const auto inf = std::numeric_limits<double>::infinity();
  Reuse best{3, std::numeric_limits<std::int64_t>::max(), inf, inf};
  for (const auto& p : offer.residency) {
    if (control) control->requireActive();
    if (p.role != role.role || p.rank != role.rank || p.capturedAtMs > nowMs || p.expiresAtMs <= nowMs ||
        (!role.modelManifestDigest.empty() && p.modelManifestDigest != role.modelManifestDigest) ||
        (!role.artifactProfileDigest.empty() && p.artifactProfileDigest != role.artifactProfileDigest) ||
        p.graphDigest != (role.graphDigest.empty() ? offer.graphDigest : role.graphDigest) ||
        p.protectionEpoch != role.protectionEpoch) continue;
    if (p.residencyClass == "CANONICAL") {
      if (p.tier == "CANONICAL") best = std::min(best, Reuse{2, p.missingVerifiedBytes,
        p.estimatedAssemblyMs, p.estimatedLoadMs});
      continue;
    }
    const bool common = !p.identityDigest.empty() && !p.assemblySpecDigest.empty() &&
      !p.modelManifestDigest.empty() && !p.artifactProfileDigest.empty() && !p.graphDigest.empty() &&
      !p.backend.empty() && !p.protectionEpoch.empty();
    if (!common || p.artifactDigest != role.artifactDigest || p.assemblySpecDigest != role.recipeDigest) continue;
    if (p.residencyClass == "ASSEMBLED_FRAGMENT") {
      if ((p.tier == "DISK" || p.tier == "RAM") && (p.backend == role.backend || p.backend == backend))
        best = std::min(best, Reuse{1, 0, 0.0, p.estimatedLoadMs});
      continue;
    }
    const std::vector<std::string> devices = device == "cpu" ? std::vector<std::string>{}
      : std::vector<std::string>{device};
    if ((p.tier == "GPU" || p.tier == "RAM") && p.runtimeGeneration && !p.fencingToken.empty() &&
        !p.processEpoch.empty() && p.backend == backend && p.deviceSet == devices &&
        p.bootEpoch == offer.bootEpoch && p.topologyDigest == offer.topologyDigest)
      best = std::min(best, Reuse{0, 0, 0.0, 0.0});
  }
  return best;
}
Reuse v4FallbackReuseCost(const NativeSelectionRoleV3& role,
                          const NativeObservedProviderOfferV3& offer,
                          const std::string& device, const std::string& backend,
                          const NativeJson& providerOffer, std::uint64_t nowMs,
                          const ExtensionControl* control)
{
  const auto inf = std::numeric_limits<double>::infinity();
  Reuse best{3, std::numeric_limits<std::uint64_t>::max(), inf, inf};
  if (!providerOffer.is_object() || !providerOffer.contains("values") ||
      !providerOffer.at("values").is_object())
    return best;
  const auto& values = providerOffer.at("values");
  if (!values.contains("residency") || !values.at("residency").is_array())
    return best;
  const std::vector<std::string> expectedDevices = device == "cpu" ?
    std::vector<std::string>{} : std::vector<std::string>{device};
  const auto expectedGraph = role.graphDigest.empty() ? offer.graphDigest : role.graphDigest;
  const auto bootEpoch = providerOffer.value("boot_epoch", std::string{});
  for (const auto& row : values.at("residency")) {
    if (control) control->requireActive();
    if (!row.is_object() || row.value("artifact_digest", std::string{}) != role.artifactDigest ||
        row.value("role", std::string{}) != role.role ||
        row.value("rank", std::uint64_t{std::numeric_limits<std::uint64_t>::max()}) != role.rank ||
        (!role.modelManifestDigest.empty() &&
         row.value("model_manifest_digest", std::string{}) != role.modelManifestDigest) ||
        row.value("graph_digest", std::string{}) != expectedGraph ||
        row.value("protection_epoch", std::string{}) != role.protectionEpoch ||
        !row.contains("device_set") || !row.at("device_set").is_array())
      continue;
    std::vector<std::string> devices;
    bool validDevices = true;
    for (const auto& value : row.at("device_set")) {
      if (!value.is_string()) {
        validDevices = false;
        break;
      }
      devices.push_back(value.get<std::string>());
    }
    if (!validDevices || devices != expectedDevices)
      continue;
    if (!row.contains("captured_at_ms") || !row.at("captured_at_ms").is_number_unsigned() ||
        !row.contains("expires_at_ms") || !row.at("expires_at_ms").is_number_unsigned())
      continue;
    const auto capturedAt = row.at("captured_at_ms").get<std::uint64_t>();
    const auto expiresAt = row.at("expires_at_ms").get<std::uint64_t>();
    if (capturedAt == 0 || capturedAt > nowMs || expiresAt <= nowMs ||
        expiresAt <= capturedAt ||
        row.value("process_epoch", std::string{}) != bootEpoch ||
        (row.value("backend", std::string{}) != backend &&
         row.value("backend", std::string{}) != role.backend))
      continue;
    const auto state = row.value("state", std::string{});
    const auto missing = row.contains("missing_verified_bytes") ?
      jsonUnsigned(row.at("missing_verified_bytes")) : std::nullopt;
    const auto assembly = row.contains("estimated_assembly_ms") ?
      jsonNonnegativeNumber(row.at("estimated_assembly_ms")) : std::nullopt;
    const auto load = row.contains("estimated_load_ms") ?
      jsonNonnegativeNumber(row.at("estimated_load_ms")) : std::nullopt;
    const auto missingValue = missing.value_or(std::numeric_limits<std::uint64_t>::max());
    const auto assemblyValue = assembly.value_or(inf);
    const auto loadValue = load.value_or(inf);
    if (state == "EXACT_LOADED") {
      const auto runtime = row.value("runtime_generation", std::string{});
      if (runtime.empty() || row.value("backend", std::string{}) != backend)
        continue;
      best = std::min(best, Reuse{0, 0, 0.0, 0.0});
    }
    else if (state == "ASSEMBLED") {
      best = std::min(best, Reuse{1, missingValue, assemblyValue, loadValue});
    }
    else if (state == "CANONICAL_ONLY" || state == "KNOWN_MISS") {
      best = std::min(best, Reuse{2, missingValue, assemblyValue, loadValue});
    }
  }
  return best;
}
  using Cost = std::tuple<int, std::uint64_t, double, double, std::uint64_t,
                          double, double, double, std::string, std::string, std::string>;

std::vector<Cost> feasibleChoices(const NativeSelectionRoleV3& role,
  const NativeObservedProviderOfferV3& offer, std::uint64_t nowMs,
  const ExtensionControl* control, const NativeJson* v4FallbackOffer = nullptr)
{
  std::vector<Cost> choices;
  if (control) control->requireActive();
      if (!contains(offer.acceptedRoles, role.role) ||
          (!contains(offer.backends, role.backend) && !contains(offer.backends, role.backend + "-cpu") &&
           !contains(offer.backends, role.backend + "-cuda"))) return choices;
      auto devices = offer.devices;
      const bool cpu = cpuBackend(role.backend) || contains(offer.backends, role.backend + "-cpu");
      if (devices.empty() && cpu) devices.push_back("cpu");
      for (const auto& device : devices) {
        if (control) control->requireActive();
        if (!role.deviceSet.empty() && !contains(role.deviceSet, device)) continue;
        if (device == "cpu") { if (!cpu) continue; }
        else {
          if (cpuBackend(role.backend)) continue;
          auto r = offer.resources.end();
          for (auto it = offer.resources.begin(); it != offer.resources.end(); ++it) {
            if (control) control->requireActive();
            if (it->device == device) { r = it; break; }
          }
          if ((!contains(offer.backends, role.backend) && !contains(offer.backends, role.backend + "-cuda")) ||
              r == offer.resources.end() || r->freeMemoryMb < role.requiredDeviceMemoryMb) continue;
        }
        auto backend = role.backend;
        if (device == "cpu" && contains(offer.backends, backend + "-cpu")) backend += "-cpu";
        else if (device != "cpu" && contains(offer.backends, backend + "-cuda")) backend += "-cuda";
        const auto reuse = v4FallbackOffer == nullptr ?
          reuseCost(role, offer, device, backend, nowMs, control) :
          v4FallbackReuseCost(role, offer, device, backend, *v4FallbackOffer,
                              nowMs, control);
        const bool exact = std::get<0>(reuse) <= 1;
        if ((!exact && offer.executionDisposition != "ACCEPT_WITH_PREPARATION") ||
            (offer.executionDisposition == "ACCEPT_IF_EXACT_REUSE" && !exact)) continue;
        const auto queueDepth = v4FallbackOffer == nullptr ? offer.queueDepth : [&] {
          const auto& values = v4FallbackOffer->at("values");
          const bool capacityObserved = values.contains("capacity_state") &&
            values.at("capacity_state").is_string() &&
            values.at("capacity_state").get<std::string>() == "OBSERVED";
          const auto depth = capacityObserved && values.contains("queue_depth") ?
            jsonUnsigned(values.at("queue_depth")) : std::nullopt;
          return depth.value_or(std::numeric_limits<std::uint64_t>::max());
        }();
        const auto waitMs = v4FallbackOffer == nullptr ? offer.estimatedWaitMs : [&] {
          const auto& values = v4FallbackOffer->at("values");
          const auto wait = values.contains("estimated_wait_ms") ?
            jsonNonnegativeNumber(values.at("estimated_wait_ms")) : std::nullopt;
          return wait.value_or(std::numeric_limits<double>::infinity());
        }();
        const auto rttMs = v4FallbackOffer == nullptr ? offer.rttMs :
          std::numeric_limits<double>::infinity();
        const auto negativeBandwidth = v4FallbackOffer == nullptr ? -offer.bandwidthMbps :
          std::numeric_limits<double>::infinity();
        choices.emplace_back(std::tuple_cat(reuse, std::make_tuple(queueDepth, waitMs,
          rttMs, negativeBandwidth, offer.provider, device, backend)));
      }
  return choices;
}
using RoleReferences = std::vector<const NativeSelectionRoleV3*>;
RoleReferences roleReferences(const std::vector<NativeSelectionRoleV3>& roles)
{
  RoleReferences result;
  result.reserve(roles.size());
  for (const auto& role : roles) result.push_back(&role);
  return result;
}

std::map<std::string, std::set<std::uint64_t>> validateSnapshotReferences(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const RoleReferences& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl* control)
{
  if (control) control->requireActive();
  if (roles.empty() || offers.empty() || context.requestId.empty() || !context.attempt ||
      context.serviceName.empty() || !digest(context.modelDigest) || !digest(context.graphDigest) ||
      !digest(ackClosedDigest) || context.deadlineMs <= nowMs)
    throw std::invalid_argument("incomplete V3 planning snapshot");
  std::map<std::string, std::set<std::uint64_t>> ranks;
  for (const auto* rolePointer : roles) {
    const auto& role = *rolePointer;
    if (control) control->requireActive();
    if (role.role.empty() || role.backend.empty() || !digest(role.artifactDigest) ||
        !digest(role.recipeDigest) || !digest(role.graphDigest) ||
        role.graphDigest != roles.front()->graphDigest ||
        !ranks[role.role].insert(role.rank).second)
      throw std::invalid_argument("invalid or duplicate V3 role/rank");
  }
  for (const auto& item : ranks) {
    if (control) control->requireActive();
    if (*item.second.begin() != 0 || *item.second.rbegin() != item.second.size() - 1)
      throw std::invalid_argument("incomplete V3 rank cover");
  }
  std::set<std::string> providers;
  for (const auto& admitted : offers) {
    if (control) control->requireActive();
    const auto& offer = admitted.observation();
    if (!offer.status || !providers.insert(offer.provider).second || offer.requestId != context.requestId ||
        offer.attempt != context.attempt || offer.service != context.serviceName ||
        offer.modelDigest != context.modelDigest ||
        (offer.graphDigest != context.graphDigest && offer.graphDigest != "sha256:" + std::string(64, '0')) ||
        offer.capturedAtMs > nowMs || offer.expiresAtMs <= nowMs || offer.expiresAtMs < context.deadlineMs)
      throw std::invalid_argument("invalid admitted offer planning binding");
  }
  return ranks;
}

std::map<std::string, std::set<std::uint64_t>> validateSnapshot(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl* control)
{
  return validateSnapshotReferences(context, ackClosedDigest, roleReferences(roles), offers, nowMs, control);
}
}

NativeFixedProviderPlacement::NativeFixedProviderPlacement(
  std::map<std::string, std::string> providerByRole,
  NativeStrategyIdentity identity)
  : m_providerByRole(std::move(providerByRole))
  , m_identity(std::move(identity))
{
  if (m_providerByRole.empty() ||
      std::any_of(m_providerByRole.begin(), m_providerByRole.end(), [] (const auto& item) {
        return item.first.empty() || item.second.empty();
      })) {
    throw std::invalid_argument("fixed placement provider map is incomplete");
  }
  std::ostringstream canonical;
  canonical << m_identity.name << '|' << m_identity.version << '|';
  for (const auto& item : m_providerByRole)
    canonical << item.first.size() << ':' << item.first << item.second.size() << ':' << item.second;
  m_identity.configurationDigest = nativePlanningDigest(canonical.str());
  m_identity.validate();
}

NativeStrategyIdentity
NativeFixedProviderPlacement::identity() const
{
  return m_identity;
}

NativeRolePlacementProposalV3
NativeFixedProviderPlacement::proposeRolesImpl(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl* control) const
{
  if (control) control->requireActive();
  const auto ranks = validateSnapshot(context, ackClosedDigest, roles, offers, nowMs, control);
  NativeRolePlacementProposalV3 result{
    context, ackClosedDigest, identity(), {}, {}, {}, std::nullopt, std::nullopt};
  auto ordered = roles;
  std::sort(ordered.begin(), ordered.end(), [] (const auto& left, const auto& right) {
    return std::tie(left.role, left.rank) < std::tie(right.role, right.rank);
  });
  for (auto role : ordered) {
    if (control) control->requireActive();
    const auto key = ranks.at(role.role).size() == 1
      ? role.role : role.role + "#" + std::to_string(role.rank);
    const auto fixed = m_providerByRole.find(key);
    if (fixed == m_providerByRole.end())
      throw NativeNoFeasiblePlacement("fixed placement has no Provider for role " + key);
    const auto admitted = std::find_if(offers.begin(), offers.end(), [&fixed] (const auto& item) {
      return item.observation().provider == fixed->second;
    });
    if (admitted == offers.end())
      throw NativeNoFeasiblePlacement("fixed placement Provider was not admitted: " + fixed->second);
    const auto choices = feasibleChoices(role, admitted->observation(), nowMs, control);
    if (choices.empty())
      throw NativeNoFeasiblePlacement("fixed placement Provider is not feasible for role " + key);
    const auto selected = *std::min_element(choices.begin(), choices.end());
    const auto device = std::get<9>(selected);
    role.selectedRole = key;
    role.backend = std::get<10>(selected);
    role.deviceSet = device == "cpu" ? std::vector<std::string>{}
                                      : std::vector<std::string>{device};
    validateNativeAssembly(role);
    if (!result.providerByRole.emplace(key, fixed->second).second)
      throw std::invalid_argument("fixed placement has duplicate role key " + key);
    result.offerDigestByProvider.emplace(fixed->second,
                                         admitted->observation().offerDigest);
    result.roles.push_back(std::move(role));
  }
  // NativeRequestPlanner validates every strategy result at the injection
  // boundary.  Do not repeat that full canonical/feasibility walk here: the
  // fixed strategy has already validated each selected assembly while
  // constructing the result, and the planner's boundary check remains the
  // single fail-closed check for both built-in and custom strategies.
  if (control) control->requireActive();
  return result;
}

NativeRolePlacementProposalV3
NativeFixedProviderPlacement::proposeRoles(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl& control) const
{
  control.requireActive();
  return proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs, &control);
}

NativeRolePlacementProposalV3 NativePreSplitFirstPlacement::proposeRolesImpl(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl* control, const NativeJson* v4ProviderOffers) const
{
  if (control) control->requireActive();
  const auto ranks = validateSnapshot(context, ackClosedDigest, roles, offers, nowMs, control);
  std::map<std::string, const NativeJson*> v4OfferByProvider;
  if (v4ProviderOffers != nullptr) {
    if (!v4ProviderOffers->is_array() || v4ProviderOffers->size() != offers.size())
      throw std::invalid_argument("V4 fallback offer facts do not cover the admitted roster");
    for (const auto& offer : *v4ProviderOffers) {
      if (!offer.is_object() || !offer.contains("provider") ||
          !offer.at("provider").is_string() || !offer.contains("values") ||
          !offer.at("values").is_object() ||
          !v4OfferByProvider.emplace(offer.at("provider").get<std::string>(), &offer).second)
        throw std::invalid_argument("V4 fallback offer fact is malformed or duplicated");
    }
    for (const auto& admitted : offers) {
      if (v4OfferByProvider.count(admitted.observation().provider) == 0)
        throw std::invalid_argument("V4 fallback offer facts omit an admitted Provider");
    }
  }
  NativeRolePlacementProposalV3 result{
    context, ackClosedDigest, identity(), {}, {}, {}, std::nullopt, std::nullopt};
  auto ordered = roles;
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    return std::tie(a.role, a.rank) < std::tie(b.role, b.rank);
  });
  std::set<std::string> used;
  std::set<std::pair<std::string, std::string>> usedDevices;
  std::map<std::pair<std::string, std::string>, std::uint64_t> reservedMemory;
  for (auto role : ordered) {
    if (control) control->requireActive();
    const auto key = ranks.at(role.role).size() == 1 ? role.role : role.role + "#" + std::to_string(role.rank);
    const auto preferred = context.preferredProvidersByRole.find(key);
    std::vector<Cost> choices;
    std::vector<Cost> distinctChoices;
    std::vector<Cost> preferredChoices;
    const auto fitsReservation = [&](const Cost& choice) {
      const auto& provider = std::get<8>(choice);
    const auto& device = std::get<9>(choice);
      if (control) control->requireActive();
      if (device == "cpu" || role.requiredDeviceMemoryMb == 0) return true;
      auto offer = offers.end();
      for (auto it = offers.begin(); it != offers.end(); ++it) {
        if (control) control->requireActive();
        if (it->observation().provider == provider) { offer = it; break; }
      }
      if (offer == offers.end()) return false;
      auto resource = offer->observation().resources.end();
      for (auto it = offer->observation().resources.begin();
           it != offer->observation().resources.end(); ++it) {
        if (control) control->requireActive();
        if (it->device == device) { resource = it; break; }
      }
      if (resource == offer->observation().resources.end()) return false;
      const auto key = std::make_pair(provider, device);
      const auto reserved = reservedMemory.find(key);
      const auto alreadyReserved = reserved == reservedMemory.end() ? 0 : reserved->second;
      return alreadyReserved <= resource->freeMemoryMb &&
        role.requiredDeviceMemoryMb <= resource->freeMemoryMb - alreadyReserved;
    };
    for (const auto& admitted : offers) {
      if (control) control->requireActive();
      const auto& offer = admitted.observation();
      const auto v4Offer = v4OfferByProvider.find(offer.provider);
      const auto eligible = feasibleChoices(role, offer, nowMs, control,
        v4Offer == v4OfferByProvider.end() ? nullptr : v4Offer->second);
      for (const auto& choice : eligible) {
        if (control) control->requireActive();
        if (!fitsReservation(choice)) continue;
        const auto device = std::get<9>(choice);
        if (device != "cpu" && usedDevices.count(std::make_pair(offer.provider, device))) continue;
        choices.push_back(choice);
        if (preferred != context.preferredProvidersByRole.end() && offer.provider == preferred->second)
          preferredChoices.push_back(choice);
        if (!used.count(offer.provider)) distinctChoices.push_back(choice);
      }
    }
    // Prefer spreading roles across Providers when the topology permits it,
    // but allow a smaller deployment to co-locate roles on one admitted
    // Provider.  The per-role device/resource contract remains authoritative.
    // Conversation locality outranks spreading and immutable-model residency,
    // but only after current admission and device/resource feasibility checks.
    if (!preferredChoices.empty()) choices.swap(preferredChoices);
    else if (!distinctChoices.empty()) choices.swap(distinctChoices);
    if (choices.empty()) throw NativeNoFeasiblePlacement("no feasible Provider for V3 role " + role.role);
    const auto choice = *std::min_element(choices.begin(), choices.end());
    const auto provider = std::get<8>(choice), device = std::get<9>(choice);
    if (!result.providerByRole.emplace(key, provider).second)
      throw std::invalid_argument("ambiguous V3 role assignment key");
    used.insert(provider);
    role.selectedRole = key;
    role.backend = std::get<10>(choice);
    role.deviceSet = device == "cpu" ? std::vector<std::string>{} : std::vector<std::string>{device};
    validateNativeAssembly(role);
    if (device != "cpu" && role.requiredDeviceMemoryMb != 0) {
      const auto reservationKey = std::make_pair(provider, device);
      const auto reserved = reservedMemory.find(reservationKey);
      const auto alreadyReserved = reserved == reservedMemory.end() ? 0 : reserved->second;
      if (alreadyReserved > std::numeric_limits<std::uint64_t>::max() - role.requiredDeviceMemoryMb)
        throw std::overflow_error("V3 Provider device reservation overflows uint64");
      reservedMemory[reservationKey] = alreadyReserved + role.requiredDeviceMemoryMb;
    }
    if (device != "cpu") usedDevices.insert(std::make_pair(provider, device));
    result.roles.push_back(std::move(role));
    auto selected = offers.end();
    for (auto it = offers.begin(); it != offers.end(); ++it) {
      if (control) control->requireActive();
      if (it->observation().provider == provider) { selected = it; break; }
    }
    if (selected == offers.end())
      throw std::invalid_argument("selected Provider disappeared during placement");
    result.offerDigestByProvider.emplace(provider, selected->observation().offerDigest);
  }
  return result;
}

NativeRolePlacementProposalV3 NativePreSplitFirstPlacement::proposeRoles(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs) const
{
  return proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs, nullptr);
}

NativeRolePlacementProposalV3 NativePreSplitFirstPlacement::proposeRoles(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl& control) const
{
  control.requireActive();
  auto result = proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs, &control);
  control.requireActive();
  return result;
}

namespace {
void validateRolePlacementReferences(
  const NativeRolePlacementProposalV3& proposal,
  const RoleReferences& preparedRoles, const RoleReferences& chosenRoles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs, bool validateAssemblies)
{
  const auto ranks = validateSnapshotReferences(proposal.context, proposal.ackClosedDigest, preparedRoles, offers, nowMs, nullptr);
  proposal.strategy.validate();
  if (proposal.placementProbeIntent) {
    if (!chosenRoles.empty() || !proposal.providerByRole.empty() ||
        !proposal.offerDigestByProvider.empty() || proposal.placementDecisionV4)
      throw std::invalid_argument("V4 placement probe intent must not carry an assignment");
    return;
  }
  if (chosenRoles.size() != preparedRoles.size() || proposal.providerByRole.size() != preparedRoles.size())
    throw std::invalid_argument("V3 proposal role cover differs from prepared roles");
  std::set<std::string> seenRoles;
  std::set<std::pair<std::string, std::string>> usedDevices;
  std::map<std::pair<std::string, std::string>, std::uint64_t> reservedMemory;
  std::map<std::string, std::string> offerDigests;
  for (const auto* rolePointer : chosenRoles) {
    const auto& role = *rolePointer;
    // Custom numeric facets retain the legacy assembly conversion checks.
    if (validateAssemblies || std::locale() != std::locale::classic()) validateNativeAssembly(role);
    const auto original = std::find_if(preparedRoles.begin(), preparedRoles.end(), [&](const auto& r) {
      return r->role == role.role && r->rank == role.rank;
    });
    if (original == preparedRoles.end()) throw std::invalid_argument("V3 proposal contains a foreign role");
    const auto key = ranks.at(role.role).size() == 1 ? role.role : role.role + "#" + std::to_string(role.rank);
    const auto assignment = proposal.providerByRole.find(key);
    if (role.selectedRole != key || !seenRoles.insert(key).second || assignment == proposal.providerByRole.end() ||
        assignment->second.empty()) throw std::invalid_argument("invalid V3 proposal assignment");
    const auto admitted = std::find_if(offers.begin(), offers.end(), [&](const auto& o) {
      return o.observation().provider == assignment->second;
    });
    if (admitted == offers.end()) throw std::invalid_argument("V3 proposal Provider was not admitted");
    const auto choices = feasibleChoices(**original, admitted->observation(), nowMs, nullptr);
    const auto device = role.deviceSet.empty() ? "cpu" : role.deviceSet.front();
    if (device != "cpu" && !usedDevices.insert(std::make_pair(assignment->second, device)).second)
      throw std::invalid_argument("V3 proposal reuses an exclusive Provider device");
    if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) {
      return std::get<9>(choice) == device && std::get<10>(choice) == role.backend;
    })) throw std::invalid_argument("V3 proposal selected an infeasible device or backend");
    if (device != "cpu" && role.requiredDeviceMemoryMb != 0) {
      const auto resource = std::find_if(admitted->observation().resources.begin(),
        admitted->observation().resources.end(), [&](const auto& item) { return item.device == device; });
      const auto keyMemory = std::make_pair(assignment->second, device);
      const auto reserved = reservedMemory.find(keyMemory);
      const auto alreadyReserved = reserved == reservedMemory.end() ? 0 : reserved->second;
      if (resource == admitted->observation().resources.end() || alreadyReserved > resource->freeMemoryMb ||
          role.requiredDeviceMemoryMb > resource->freeMemoryMb - alreadyReserved)
        throw std::invalid_argument("V3 proposal exceeds Provider device memory");
      if (alreadyReserved > std::numeric_limits<std::uint64_t>::max() - role.requiredDeviceMemoryMb)
        throw std::overflow_error("V3 Provider device reservation overflows uint64");
      reservedMemory[keyMemory] = alreadyReserved + role.requiredDeviceMemoryMb;
    }
    if (!samePreparedAssemblyMetadata(**original, role)) {
      auto expected = **original;
      expected.backend = role.backend; expected.deviceSet = role.deviceSet;
      const auto expectedAssembly = nativeAssemblyJson(expected);
      const auto proposedAssembly = nativeAssemblyJson(role);
      // Retain canonical comparison and its exceptions for differing values.
      if (expectedAssembly != proposedAssembly ||
          std::signbit(expected.postprocessConfidenceThreshold) !=
            std::signbit(role.postprocessConfidenceThreshold)) {
        if (nativeCanonicalJson(expectedAssembly) != nativeCanonicalJson(proposedAssembly))
          throw std::invalid_argument("V3 proposal modified prepared assembly metadata");
      }
    }
    offerDigests.emplace(assignment->second, admitted->observation().offerDigest);
  }
  if (offerDigests != proposal.offerDigestByProvider)
    throw std::invalid_argument("V3 proposal offer identities differ from admitted observations");

  if (proposal.placementDecisionV4) {
    const auto& decision = *proposal.placementDecisionV4;
    if (!decision.is_object() ||
        decision.value("schema", std::string{}) != "ndnsf-placement-decision-v4" ||
        decision.value("policy_version", std::string{}) != "role-dag-beam-v1" ||
        !decision.contains("snapshot_digest") ||
        !decision.contains("status") || !decision.at("status").is_string() ||
        !decision.contains("fallback_reason") ||
        !decision.at("fallback_reason").is_string() ||
        !decision.contains("selected_assignments") ||
        !decision.at("selected_assignments").is_array())
      throw std::invalid_argument("V4 placement decision is incomplete");
    const auto& decisionSnapshot = decision.at("snapshot_digest");
    if (proposal.context.placementObservationSnapshotDigest.empty()) {
      if (!decisionSnapshot.is_null() ||
          decision.at("status").get<std::string>() != "BASELINE_FALLBACK")
        throw std::invalid_argument("V4 fallback decision has an invalid snapshot binding");
    }
    else if (!decisionSnapshot.is_string() ||
             decisionSnapshot.get<std::string>() !=
               proposal.context.placementObservationSnapshotDigest) {
      throw std::invalid_argument("V4 decision is not bound to the admitted snapshot");
    }
    const auto status = decision.at("status").get<std::string>();
    const auto fallbackReason = decision.at("fallback_reason").get<std::string>();
    if ((status == "SCORED" && !fallbackReason.empty()) ||
        (status == "BASELINE_FALLBACK" && fallbackReason.empty()) ||
        (status != "SCORED" && status != "BASELINE_FALLBACK") ||
        decision.at("selected_assignments").size() != chosenRoles.size())
      throw std::invalid_argument("V4 decision status or assignment cover is invalid");
    std::set<std::string> decisionRoles;
    for (const auto& item : decision.at("selected_assignments")) {
      if (!item.is_object() || !item.contains("role_key") ||
          !item.at("role_key").is_string() || !item.contains("provider") ||
          !item.at("provider").is_string() || !item.contains("device") ||
          !item.at("device").is_string() || !item.contains("backend") ||
          !item.at("backend").is_string())
        throw std::invalid_argument("V4 decision assignment row is invalid");
      const auto key = item.at("role_key").get<std::string>();
      const auto assigned = proposal.providerByRole.find(key);
      const auto selectedRole = std::find_if(chosenRoles.begin(), chosenRoles.end(),
        [&key] (const auto& role) { return role->selectedRole == key; });
      const auto device = selectedRole == chosenRoles.end() ||
          (*selectedRole)->deviceSet.empty() ? std::string("cpu") :
          (*selectedRole)->deviceSet.front();
      if (!decisionRoles.insert(key).second || assigned == proposal.providerByRole.end() ||
          selectedRole == chosenRoles.end() ||
          item.at("provider").get<std::string>() != assigned->second ||
          item.at("device").get<std::string>() != device ||
          item.at("backend").get<std::string>() != (*selectedRole)->backend)
        throw std::invalid_argument("V4 decision assignment differs from proposal");
    }
  }
}

} // namespace

void validateNativeRolePlacement(const NativeRolePlacementProposalV3& proposal,
  const std::vector<NativeSelectionRoleV3>& preparedRoles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs)
{
  validateRolePlacementReferences(proposal, roleReferences(preparedRoles), roleReferences(proposal.roles),
    offers, nowMs, true);
}

void validateNativeRolePlacementWithFixedCore(const NativeRolePlacementProposalV3& proposal,
  const std::vector<NativeSelectionRoleV3>& preparedRoles, const NativeFixedPlacementCoreV1& fixed,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs)
{
  bool structurallyKnown = proposal.roles.size() == fixed.roles().size();
  for (const auto& role : proposal.roles) {
    const auto found = fixed.roles().find(role.selectedRole);
    if (found == fixed.roles().end() || !digest(role.recipeDigest) || !digest(role.modelManifestDigest) ||
        role.materializedRole != found->second.materializedRole ||
        !nativeSelectionRoleContractsEqualExceptCertificatesV3(role, found->second, true, true)) {
      structurallyKnown = false;
      break;
    }
  }
  validateRolePlacementReferences(proposal, roleReferences(preparedRoles), roleReferences(proposal.roles),
    offers, nowMs, !structurallyKnown);
}

void validateNativePublishedRolePlacement(const NativeRolePlacementProposalV3& proposal,
  const NativeFixedPlacementCoreV1& fixed, const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs)
{
  RoleReferences published;
  published.reserve(proposal.roles.size());
  for (const auto& role : proposal.roles) published.push_back(&fixed.roles().at(role.selectedRole));
  validateRolePlacementReferences(proposal, published, published, offers, nowMs, false);
}

namespace {

constexpr const char* V4_POLICY_VERSION = "role-dag-beam-v1";

struct V4SearchBudgetDeadline {};

void requireV4SearchActive(const ExtensionControl* control)
{
  if (!control)
    return;
  if (control->cancelled && control->cancelled())
    throw std::runtime_error("cooperative extension cancelled");
  if (control->deadline != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() >= control->deadline)
    throw V4SearchBudgetDeadline{};
}

std::optional<std::uint64_t> jsonUnsigned(const NativeJson& value)
{
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number >= 0)
      return static_cast<std::uint64_t>(number);
  }
  return std::nullopt;
}

std::optional<double> jsonNonnegativeNumber(const NativeJson& value)
{
  if (!value.is_number())
    return std::nullopt;
  const auto number = value.get<double>();
  if (!std::isfinite(number) || number < 0.0)
    return std::nullopt;
  return number;
}

std::optional<double> jsonPositiveNumber(const NativeJson& value)
{
  const auto number = jsonNonnegativeNumber(value);
  return number && *number > 0.0 ? number : std::nullopt;
}

struct V4MeasuredRate
{
  double mbps = 0.0;
  std::string method;
  std::uint64_t capturedAtMs = 0;
  std::string sampleDigest;
};

std::optional<V4MeasuredRate> findV4MeasuredRate(
  const NativeJson& snapshot, const std::string& src, const std::string& dst,
  std::uint64_t nowMs)
{
  if (!snapshot.contains("eligible_link_observations") ||
      !snapshot.at("eligible_link_observations").is_array())
    return std::nullopt;
  for (const auto* wantedKind : {"ACTIVE_PROBE_GOODPUT"}) {
    std::optional<V4MeasuredRate> best;
    for (const auto& entry : snapshot.at("eligible_link_observations")) {
      if (!entry.is_object() || !entry.contains("row") || !entry.at("row").is_object())
        continue;
      const auto& row = entry.at("row");
      if (row.value("kind", std::string{}) != wantedKind ||
          row.value("src_peer", std::string{}) != src ||
          row.value("dst_peer", std::string{}) != dst ||
          !row.contains("captured_at_ms") || !row.contains("expires_at_ms") ||
          !row.contains("probe_train_goodput_mbps") || !row.contains("payload_bytes") ||
          !row.contains("elapsed_ms"))
        continue;
      const auto captured = jsonUnsigned(row.at("captured_at_ms"));
      const auto expires = jsonUnsigned(row.at("expires_at_ms"));
      const auto bytes = jsonUnsigned(row.at("payload_bytes"));
      const auto elapsed = jsonPositiveNumber(row.at("elapsed_ms"));
      const auto rate = jsonPositiveNumber(row.at("probe_train_goodput_mbps"));
      if (!captured || !expires || *captured == 0 || *captured > nowMs ||
          *expires <= nowMs || *captured >= *expires || !bytes || *bytes == 0 ||
          !elapsed || !rate)
        continue;
      if (wantedKind == std::string("ACTIVE_PROBE_GOODPUT")) {
        if (!row.contains("probe_complete") || !row.at("probe_complete").is_boolean() ||
            !row.at("probe_complete").get<bool>() ||
            jsonUnsigned(row.value("probe_segments_sent", NativeJson(nullptr))) !=
              std::optional<std::uint64_t>(ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS) ||
            !row.contains("probe_profile_digest") ||
            !row.at("probe_profile_digest").is_string() ||
            row.at("probe_profile_digest") != ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST ||
            row.value("probe_signature_valid", NativeJson(false)) != true ||
            row.value("probe_source_timing_validated", NativeJson(false)) != true ||
            row.value("probe_rate_status", NativeJson(nullptr)) != "MEASURED_GOODPUT")
          continue;
        const auto arrivals = row.value("probe_data_arrival_gaps_ms", NativeJson(nullptr));
        const auto source = row.value("probe_source_send_gaps_ms", NativeJson(nullptr));
        if (!arrivals.is_array() || arrivals.size() != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS - 1 ||
            !source.is_array() || source.size() != ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS - 2)
          continue;
        ndn_service_framework::NetworkProbeArrivalGaps arrivalValues{};
        ndn_service_framework::NetworkProbeSourceGaps sourceValues{};
        bool valid = true;
        for (std::size_t i = 0; i < arrivalValues.size(); ++i) {
          const auto value = jsonPositiveNumber(arrivals[i]);
          if (!value) { valid = false; break; }
          arrivalValues[i] = *value;
        }
        for (std::size_t i = 0; i < sourceValues.size() && valid; ++i) {
          const auto value = jsonNonnegativeNumber(source[i]);
          if (!value) { valid = false; break; }
          sourceValues[i] = *value;
        }
        if (!valid)
          continue;
        const auto estimate = ndn_service_framework::estimateWarmedNetworkProbeTrainRate(
          arrivalValues, sourceValues, ndn_service_framework::NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true);
        if (estimate.status != ndn_service_framework::NetworkProbeRateStatus::MeasuredGoodput ||
            std::abs(*rate - estimate.goodputMbps) > std::max(1e-9, estimate.goodputMbps * 1e-6))
          continue;
      }
      const auto digestValue = row.value("source_digest", std::string{});
      if (!digest(digestValue))
        continue;
      V4MeasuredRate candidate{*rate, wantedKind, *captured, digestValue};
      if (!best || candidate.capturedAtMs > best->capturedAtMs ||
          (candidate.capturedAtMs == best->capturedAtMs &&
           candidate.sampleDigest < best->sampleDigest))
        best = std::move(candidate);
    }
    if (best)
      return best;
  }
  return std::nullopt;
}

struct V4MeasuredRtt
{
  double milliseconds = 0.0;
  std::uint64_t capturedAtMs = 0;
  std::string sampleDigest;
};

std::optional<V4MeasuredRtt> findV4MeasuredRtt(
  const NativeJson& snapshot, const std::string& sourceProvider,
  const std::string& destinationProvider, std::uint64_t nowMs)
{
  if (!snapshot.contains("eligible_link_observations") ||
      !snapshot.at("eligible_link_observations").is_array() ||
      !snapshot.contains("provider_offers") ||
      !snapshot.at("provider_offers").is_array())
    return std::nullopt;

  std::map<std::string, std::string> bootByProvider;
  for (const auto& offer : snapshot.at("provider_offers")) {
    if (offer.is_object() && offer.contains("provider") &&
        offer.at("provider").is_string() && offer.contains("boot_epoch") &&
        offer.at("boot_epoch").is_string())
      bootByProvider.emplace(offer.at("provider").get<std::string>(),
                             offer.at("boot_epoch").get<std::string>());
  }
  const auto observerBoot = bootByProvider.find(destinationProvider);
  const auto targetBoot = bootByProvider.find(sourceProvider);
  if (observerBoot == bootByProvider.end() || targetBoot == bootByProvider.end())
    return std::nullopt;

  std::optional<V4MeasuredRtt> best;
  for (const auto& entry : snapshot.at("eligible_link_observations")) {
    if (!entry.is_object() ||
        entry.value("reported_by", std::string{}) != destinationProvider ||
        !entry.contains("row") || !entry.at("row").is_object())
      continue;
    const auto& row = entry.at("row");
    if (row.value("kind", std::string{}) != "ACTIVE_PROBE_RTT" ||
        row.value("src_peer", std::string{}) != destinationProvider ||
        row.value("dst_peer", std::string{}) != sourceProvider ||
        row.value("direction_semantics", std::string{}) !=
          "REQUESTER_RESPONDER_ROUND_TRIP" ||
        row.value("observer_peer", std::string{}) != destinationProvider ||
        row.value("observer_boot_epoch", std::string{}) != observerBoot->second ||
        row.value("src_boot_epoch", std::string{}) != observerBoot->second ||
        row.value("dst_boot_epoch", std::string{}) != targetBoot->second ||
        !row.contains("captured_at_ms") || !row.contains("expires_at_ms") ||
        !row.contains("rtt_ms"))
      continue;
    const auto captured = jsonUnsigned(row.at("captured_at_ms"));
    const auto expires = jsonUnsigned(row.at("expires_at_ms"));
    const auto rtt = jsonPositiveNumber(row.at("rtt_ms"));
    const auto sampleDigest = row.value("source_digest", std::string{});
    if (!captured || !expires || *captured == 0 || *captured > nowMs ||
        *expires <= nowMs || *captured >= *expires || !rtt ||
        !digest(sampleDigest))
      continue;
    V4MeasuredRtt candidate{*rtt, *captured, sampleDigest};
    if (!best || candidate.capturedAtMs > best->capturedAtMs ||
        (candidate.capturedAtMs == best->capturedAtMs &&
         candidate.sampleDigest < best->sampleDigest))
      best = std::move(candidate);
  }
  return best;
}

std::optional<double> requesterAckRttMs(
  const NativeJson& snapshot, const std::string& provider,
  const std::string& bootEpoch, std::uint64_t nowMs)
{
  if (!snapshot.contains("requester_provider_samples") ||
      !snapshot.at("requester_provider_samples").is_array())
    return std::nullopt;
  for (const auto& row : snapshot.at("requester_provider_samples")) {
    if (!row.is_object() || row.value("provider", std::string{}) != provider ||
        !row.contains("sample") || !row.at("sample").is_object())
      continue;
    const auto& sample = row.at("sample");
    if (sample.value("kind", std::string{}) != "ACK_RTT" ||
        sample.value("provider", std::string{}) != provider ||
        sample.value("peer_boot_epoch", std::string{}) != bootEpoch ||
        !sample.contains("observed_at_ms") || !sample.contains("rtt_ms"))
      continue;
    const auto captured = jsonUnsigned(sample.at("observed_at_ms"));
    const auto rtt = jsonPositiveNumber(sample.at("rtt_ms"));
    if (captured && *captured != 0 && *captured <= nowMs &&
        nowMs - *captured <= 5000 && rtt)
      return rtt;
  }
  return std::nullopt;
}

struct V4RoleChoice
{
  Cost v3Choice;
  std::string provider;
  std::string device;
  std::string backend;
};

using V4Assignment = std::map<std::string, V4RoleChoice>;

struct V4BeamState
{
  V4Assignment assignments;
  std::set<std::string> usedProviders;
  std::set<std::pair<std::string, std::string>> usedDevices;
};

struct V4RoleEdge
{
  std::string producerRole;
  std::string consumerRole;
  std::uint64_t bytes = 0;
  bool bytesKnown = false;
  std::string operationKind;
};

struct V4AssignmentEvaluation
{
  bool scoreable = true;
  std::string reason;
  double controlPathMs = 0.0;
  double dataFlowCriticalPathMs = 0.0;
  double scoreMs = 0.0;
  std::uint64_t queueDepthSum = 0;
  bool queueDepthComplete = true;
  std::size_t unknownWaitCount = 0;
  std::vector<std::pair<std::string, std::string>> missingGoodputEdges;
  std::vector<NativeJson> roleCosts;
  std::vector<NativeJson> edgeCosts;
};

void makeUnscoreable(V4AssignmentEvaluation& evaluation, const std::string& reason)
{
  evaluation.scoreable = false;
  if (evaluation.reason.empty())
    evaluation.reason = reason;
}

std::optional<double> residencyPreparationMs(
  const NativeJson& providerOffer, const NativeSelectionRoleV3& role,
  const V4RoleChoice& choice)
{
  const auto& values = providerOffer.at("values");
  if (!values.contains("residency") || !values.at("residency").is_array())
    return std::nullopt;
  const auto expectedDevices = choice.device == "cpu" ?
    std::vector<std::string>{} : std::vector<std::string>{choice.device};
  for (const auto& row : values.at("residency")) {
    if (!row.is_object() || row.value("artifact_digest", std::string{}) != role.artifactDigest ||
        row.value("role", std::string{}) != role.role ||
        row.value("rank", std::uint64_t{std::numeric_limits<std::uint64_t>::max()}) != role.rank ||
        row.value("model_manifest_digest", std::string{}) != role.modelManifestDigest ||
        row.value("graph_digest", std::string{}) != role.graphDigest ||
        row.value("backend", std::string{}) != choice.backend ||
        row.value("protection_epoch", std::string{}) != role.protectionEpoch ||
        !row.contains("device_set") || !row.at("device_set").is_array())
      continue;
    std::vector<std::string> devices;
    for (const auto& device : row.at("device_set")) {
      if (!device.is_string()) return std::nullopt;
      devices.push_back(device.get<std::string>());
    }
    if (devices != expectedDevices)
      continue;
    const auto state = row.value("state", std::string{});
    if (state == "EXACT_LOADED")
      return 0.0;
    const auto assembly = row.contains("estimated_assembly_ms") ?
      jsonNonnegativeNumber(row.at("estimated_assembly_ms")) : std::nullopt;
    const auto load = row.contains("estimated_load_ms") ?
      jsonNonnegativeNumber(row.at("estimated_load_ms")) : std::nullopt;
    if (state == "ASSEMBLED")
      return load;
    if (state == "CANONICAL_ONLY" || state == "KNOWN_MISS") {
      if (!assembly || !load)
        return std::nullopt;
      const auto sum = *assembly + *load;
      return std::isfinite(sum) ? std::optional<double>(sum) : std::nullopt;
    }
  }
  return std::nullopt;
}

NativeJson assignmentRows(const V4BeamState& state,
                          const std::vector<NativeSelectionRoleV3>& orderedRoles,
                          const std::map<std::string, std::string>& keyByLogicalRole,
                          const V4AssignmentEvaluation* evaluation)
{
  auto rows = NativeJson::array();
  for (const auto& role : orderedRoles) {
    const auto key = keyByLogicalRole.at(role.role + "#" + std::to_string(role.rank));
    const auto selected = state.assignments.find(key);
    if (selected == state.assignments.end())
      continue;
    NativeJson row{{"role_key", key}, {"provider", selected->second.provider},
      {"device", selected->second.device}, {"backend", selected->second.backend}};
    if (evaluation) {
      const auto cost = std::find_if(evaluation->roleCosts.begin(), evaluation->roleCosts.end(),
        [&key] (const auto& item) { return item.value("role_key", std::string{}) == key; });
      if (cost != evaluation->roleCosts.end()) {
        row["preparation_ms"] = cost->at("preparation_ms");
        row["queue_wait_ms"] = cost->at("queue_wait_ms");
        row["queue_depth"] = cost->at("queue_depth");
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

NativeJson makeV4Decision(
  const std::string& snapshotDigest, const std::string& status,
  const std::string& fallbackReason, std::size_t beamWidth,
  std::size_t maxStates, std::size_t statesExamined,
  const NativeJson& selectedAssignments,
  const V4AssignmentEvaluation* evaluation,
  const NativeJson& trainEstimates, const std::string& scoreMode)
{
  const auto nullable = [] (const std::optional<double>& value) -> NativeJson {
    return value ? NativeJson(*value) : NativeJson(nullptr);
  };
  return NativeJson{
    {"schema", "ndnsf-placement-decision-v4"},
    {"policy_version", V4_POLICY_VERSION},
    {"score_mode", scoreMode},
    {"score_is_end_to_end_latency", false},
    {"snapshot_digest", snapshotDigest.empty() ? NativeJson(nullptr) : NativeJson(snapshotDigest)},
    {"status", status}, {"fallback_reason", fallbackReason},
    {"beam_width", beamWidth}, {"max_assignment_states", maxStates},
    {"states_examined", statesExamined},
    {"control_path_ms", evaluation ? nullable(evaluation->controlPathMs) : NativeJson(nullptr)},
    {"data_flow_critical_path_ms", evaluation ? nullable(evaluation->dataFlowCriticalPathMs) : NativeJson(nullptr)},
    {"score_ms", evaluation ? nullable(evaluation->scoreMs) : NativeJson(nullptr)},
    {"queue_depth_sum", evaluation && evaluation->queueDepthComplete
      ? NativeJson(evaluation->queueDepthSum) : NativeJson(nullptr)},
    {"unknown_wait_count", evaluation ? NativeJson(evaluation->unknownWaitCount) : NativeJson(nullptr)},
    {"selected_assignments", selectedAssignments},
    {"edge_costs", evaluation ? NativeJson(evaluation->edgeCosts) : NativeJson::array()},
    {"probe_train_estimates", trainEstimates}};
}

NativeJson v4ProbeTrainEstimates(const NativeJson& snapshot)
{
  auto result = NativeJson::array();
  if (!snapshot.contains("eligible_link_observations") ||
      !snapshot.at("eligible_link_observations").is_array())
    return result;
  for (const auto& entry : snapshot.at("eligible_link_observations")) {
    if (!entry.is_object() || !entry.contains("row") || !entry.at("row").is_object())
      continue;
    const auto& row = entry.at("row");
    if (row.value("kind", std::string{}) != "ACTIVE_PROBE_GOODPUT")
      continue;
    ndn_service_framework::NetworkProbeRateEstimate estimate;
    estimate.reason = "PROBE_PROFILE_INVALID";
    const bool trusted = row.value("probe_complete", NativeJson(false)) == true &&
      jsonUnsigned(row.value("probe_segments_sent", NativeJson(nullptr))) ==
        std::optional<std::uint64_t>(ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS) &&
      row.value("probe_signature_valid", NativeJson(false)) == true &&
      row.value("probe_source_timing_validated", NativeJson(false)) == true &&
      row.value("probe_profile_digest", NativeJson(nullptr)) ==
        ndn_service_framework::NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
    if (trusted && row.contains("probe_data_arrival_gaps_ms") &&
        row.at("probe_data_arrival_gaps_ms").is_array() &&
        row.at("probe_data_arrival_gaps_ms").size() == ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS - 1 &&
        row.contains("probe_source_send_gaps_ms") &&
        row.at("probe_source_send_gaps_ms").is_array() &&
        row.at("probe_source_send_gaps_ms").size() == ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS - 2) {
      ndn_service_framework::NetworkProbeArrivalGaps arrivals{};
      ndn_service_framework::NetworkProbeSourceGaps sends{};
      bool valid = true;
      for (std::size_t i = 0; i < arrivals.size(); ++i) {
        const auto arrival = jsonPositiveNumber(row.at("probe_data_arrival_gaps_ms")[i]);
        if (!arrival) { valid = false; break; }
        arrivals[i] = *arrival;
      }
      for (std::size_t i = 0; i < sends.size() && valid; ++i) {
        const auto send = jsonNonnegativeNumber(row.at("probe_source_send_gaps_ms")[i]);
        if (!send) { valid = false; break; }
        sends[i] = *send;
      }
      if (valid)
        estimate = ndn_service_framework::estimateWarmedNetworkProbeTrainRate(
          arrivals, sends, ndn_service_framework::NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true);
      else
        estimate.reason = "INVALID_GAP";
    }
    const bool measured = estimate.status ==
      ndn_service_framework::NetworkProbeRateStatus::MeasuredGoodput;
    result.push_back(NativeJson{
      {"src_peer", row.value("src_peer", std::string{})},
      {"dst_peer", row.value("dst_peer", std::string{})},
      {"sample_digest", row.value("source_digest", std::string{})},
      {"status", measured ? "MEASURED_GOODPUT" : "UNKNOWN"}, {"reason", estimate.reason},
      {"goodput_mbps", measured ? NativeJson(estimate.goodputMbps) : NativeJson(nullptr)},
      {"source_timing_validated", row.value("probe_source_timing_validated", NativeJson(false))},
      {"capacity_status", "UNKNOWN"}});
  }
  return result;
}

} // namespace

NativeChirpBandwidthEstimateV4 estimateNativeChirpBandwidthV4(
  const std::array<double, 3>& interestSendGapsMs,
  const std::array<double, 3>& dataArrivalGapsMs,
  const std::array<std::uint64_t, 4>& dataWireBytes,
  const bool complete, const bool signatureValid)
{
  NativeChirpBandwidthEstimateV4 result;
  if (!complete || !signatureValid) {
    result.reason = "INCOMPLETE_OR_INVALID_SIGNATURE";
    return result;
  }
  double lower = 0.0;
  for (std::size_t i = 0; i < interestSendGapsMs.size(); ++i) {
    const auto send = interestSendGapsMs[i];
    const auto arrival = dataArrivalGapsMs[i];
    const auto conservativeWireBytes = std::min(dataWireBytes[i], dataWireBytes[i + 1]);
    if (!std::isfinite(send) || !std::isfinite(arrival) || send <= 0.0 ||
        arrival <= 0.0 || conservativeWireBytes == 0) {
      result.reason = "INVALID_GAP_OR_WIRE_SIZE";
      return result;
    }
    const auto toleranceMs = std::max(0.5, send * 0.25);
    if (arrival > send + toleranceMs) {
      // The current wire carries no target-side send timestamp, so response
      // processing/signing jitter cannot be separated from path queueing.
      result.reason = "PROVIDER_DELAY_AMBIGUOUS";
      return result;
    }
    const auto rate = static_cast<double>(conservativeWireBytes) * 8.0 /
                      (send * 1000.0);
    if (!std::isfinite(rate) || rate <= 0.0) {
      result.reason = "INVALID_OFFERED_RATE";
      return result;
    }
    lower = std::max(lower, rate);
  }
  result.status = "LOWER_BOUND_ONLY";
  result.reason = "NO_OBSERVED_GAP_EXPANSION";
  result.lowerMbps = lower;
  return result;
}

NativeObservationAwarePlacementV4::NativeObservationAwarePlacementV4(
  const std::size_t beamWidth, const std::size_t maxAssignmentStates,
  std::string scoreMode)
  : m_beamWidth(beamWidth)
  , m_maxAssignmentStates(maxAssignmentStates)
  , m_scoreMode(std::move(scoreMode))
{
  if (m_scoreMode != "network-only" && m_scoreMode != "preparation-and-network")
    throw std::invalid_argument("V4 placement score mode is unsupported");
  if (m_beamWidth == 0 || m_beamWidth > 256 || m_maxAssignmentStates == 0 ||
      m_maxAssignmentStates > 65536)
    throw std::invalid_argument("V4 placement search bounds are invalid");
}

NativeStrategyIdentity NativeObservationAwarePlacementV4::identity() const
{
  NativeStrategyIdentity result{"native-observation-aware-v4", "1", {}, true};
  result.configurationDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"policy_version", V4_POLICY_VERSION}, {"beam_width", m_beamWidth},
    {"max_assignment_states", m_maxAssignmentStates}, {"score_mode", m_scoreMode}}));
  result.validate();
  return result;
}

NativeRolePlacementProposalV3 NativeObservationAwarePlacementV4::proposeRoles(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, const std::uint64_t nowMs) const
{
  return proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs, nullptr);
}

NativeRolePlacementProposalV3 NativeObservationAwarePlacementV4::proposeRoles(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, const std::uint64_t nowMs,
  const ExtensionControl& control) const
{
  return proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs, &control);
}

NativeRolePlacementProposalV3 NativeObservationAwarePlacementV4::proposeRolesImpl(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, const std::uint64_t nowMs,
  const ExtensionControl* control) const
{
  const auto policy = identity();
  const NativeJson* v4FallbackProviderOffers = nullptr;
  const auto baseline = [&](const std::string& reason, std::size_t examined,
                            const NativeJson& chirps) {
    NativePreSplitFirstPlacement fallback;
    auto result = control && reason != "POLICY_BUDGET_EXHAUSTED" ?
      fallback.proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs,
                                 control, v4FallbackProviderOffers) :
      fallback.proposeRolesImpl(context, ackClosedDigest, roles, offers, nowMs,
                                 nullptr, v4FallbackProviderOffers);
    result.strategy = policy;
    V4BeamState state;
    std::map<std::string, std::string> keyByLogicalRole;
    std::map<std::string, std::set<std::uint64_t>> rankSets;
    for (const auto& role : roles) rankSets[role.role].insert(role.rank);
    for (const auto& role : result.roles) {
      const auto key = rankSets.at(role.role).size() == 1 ? role.role :
        role.role + "#" + std::to_string(role.rank);
      keyByLogicalRole.emplace(role.role + "#" + std::to_string(role.rank), key);
      const auto provider = result.providerByRole.at(key);
      const auto device = role.deviceSet.empty() ? "cpu" : role.deviceSet.front();
      state.assignments.emplace(key, V4RoleChoice{Cost{}, provider, device, role.backend});
    }
    result.placementDecisionV4 = makeV4Decision(
      context.placementObservationSnapshotDigest, "BASELINE_FALLBACK", reason,
      m_beamWidth, m_maxAssignmentStates, examined,
      assignmentRows(state, result.roles, keyByLogicalRole, nullptr), nullptr, chirps,
      m_scoreMode);
    return result;
  };

  const auto emptyChirps = NativeJson::array();
  NativeJson chirps = emptyChirps;
  std::size_t statesExamined = 0;
  const auto searchStarted = std::chrono::steady_clock::now();
  try {
  requireV4SearchActive(control);
  (void)validateSnapshot(context, ackClosedDigest, roles, offers, nowMs, nullptr);
  if (!context.placementObservationFallbackReason.empty())
    return baseline(context.placementObservationFallbackReason, 0, emptyChirps);
  if (context.placementObservationSnapshotDigest.empty() ||
      context.placementObservationSnapshot.is_null())
    return baseline("SNAPSHOT_UNAVAILABLE", 0, emptyChirps);

  const auto& snapshot = context.placementObservationSnapshot;
  if (!snapshot.is_object() ||
      nativePlanningDigest(nativePlacementObservationCanonicalJson(
        snapshot, context.preparedPlanningTemplate.get())) !=
        context.placementObservationSnapshotDigest ||
      snapshot.value("schema", std::string{}) !=
        "ndnsf-placement-observation-snapshot-v4" ||
      snapshot.value("request_id", std::string{}) != context.requestId ||
      snapshot.value("attempt", std::uint64_t{0}) != context.attempt ||
      snapshot.value("requester_identity", std::string{}) != context.requesterIdentity ||
      snapshot.value("ack_closed_digest", std::string{}) != ackClosedDigest ||
      snapshot.value("graph_digest", std::string{}) != context.graphDigest)
    throw std::invalid_argument("V4 placement snapshot does not match the request binding");
  const auto snapshotExpiry = snapshot.value("expires_at_ms", std::uint64_t{0});
  if (snapshotExpiry <= nowMs)
    return baseline("SNAPSHOT_STALE", 0, v4ProbeTrainEstimates(snapshot));

  if (snapshot.contains("provider_offers") &&
      snapshot.at("provider_offers").is_array() &&
      snapshot.at("provider_offers").size() == offers.size() &&
      snapshot.contains("provider_roster") &&
      snapshot.at("provider_roster").is_array()) {
    std::vector<std::string> admittedProviders;
    admittedProviders.reserve(offers.size());
    for (const auto& admitted : offers)
      admittedProviders.push_back(admitted.observation().provider);
    std::sort(admittedProviders.begin(), admittedProviders.end());
    std::vector<std::string> snapshotProviders;
    for (const auto& provider : snapshot.at("provider_roster")) {
      if (!provider.is_string()) {
        snapshotProviders.clear();
        break;
      }
      snapshotProviders.push_back(provider.get<std::string>());
    }
    std::sort(snapshotProviders.begin(), snapshotProviders.end());
    bool allOffersBound = snapshotProviders == admittedProviders;
    std::set<std::string> seenProviders;
    for (const auto& offer : snapshot.at("provider_offers")) {
      if (!offer.is_object() || !offer.contains("provider") ||
          !offer.at("provider").is_string()) {
        allOffersBound = false;
        break;
      }
      const auto provider = offer.at("provider").get<std::string>();
      const auto admitted = std::find_if(offers.begin(), offers.end(),
        [&](const auto& candidate) {
          return candidate.observation().provider == provider;
        });
      if (!offer.contains("captured_at_ms") ||
          !offer.at("captured_at_ms").is_number_unsigned() ||
          !offer.contains("expires_at_ms") ||
          !offer.at("expires_at_ms").is_number_unsigned()) {
        allOffersBound = false;
        break;
      }
      const auto captured = offer.at("captured_at_ms").get<std::uint64_t>();
      const auto expires = offer.at("expires_at_ms").get<std::uint64_t>();
      if (!seenProviders.insert(provider).second || admitted == offers.end() ||
          offer.value("signer_identity", std::string{}) != provider ||
          offer.value("signer_key_locator", std::string{}).empty() ||
          !digest(offer.value("ack_wire_digest", std::string{})) ||
          !digest(offer.value("offer_digest", std::string{})) ||
          offer.value("boot_epoch", std::string{}) != admitted->observation().bootEpoch ||
          captured == 0 || captured > nowMs || expires <= nowMs ||
          !offer.contains("values") ||
          !offer.at("values").is_object()) {
        allOffersBound = false;
        break;
      }
    }
    if (allOffersBound)
      v4FallbackProviderOffers = &snapshot.at("provider_offers");
  }

  chirps = v4ProbeTrainEstimates(snapshot);
  const auto ranks = validateSnapshot(context, ackClosedDigest, roles, offers, nowMs, nullptr);
  const NativeJson* candidateGraph = nullptr;
  if (snapshot.contains("prepared_role_graph") &&
      snapshot.at("prepared_role_graph").is_array()) {
    for (const auto& candidate : snapshot.at("prepared_role_graph")) {
      if (candidate.is_object() && candidate.contains("roles") &&
          candidate.at("roles").is_array() && candidate.at("roles").size() == roles.size() &&
          std::equal(roles.begin(), roles.end(), candidate.at("roles").begin(),
            [](const auto& role, const auto& value) { return nativeAssemblyJsonMatches(role, value); })) {
        candidateGraph = &candidate;
        break;
      }
    }
  }
  if (!candidateGraph)
    return baseline("PREPARED_ROLE_GRAPH_MISSING", 0, chirps);

  std::map<std::string, std::vector<std::string>> roleKeysByStage;
  std::map<std::string, std::string> keyByLogicalRole;
  auto roleOrder = roles;
  std::sort(roleOrder.begin(), roleOrder.end(), [] (const auto& left, const auto& right) {
    return std::tie(left.role, left.rank) < std::tie(right.role, right.rank);
  });
  for (const auto& role : roleOrder) {
    const auto key = ranks.at(role.role).size() == 1 ? role.role :
      role.role + "#" + std::to_string(role.rank);
    roleKeysByStage[role.role].push_back(key);
    keyByLogicalRole.emplace(role.role + "#" + std::to_string(role.rank), key);
  }
  if (!candidateGraph->contains("stage_order") ||
      !candidateGraph->at("stage_order").is_array())
    return baseline("ROLE_DAG_MISSING", 0, chirps);
  std::vector<std::string> orderedRoleKeys;
  std::set<std::string> seenStages;
  for (const auto& stage : candidateGraph->at("stage_order")) {
    if (!stage.is_string())
      return baseline("ROLE_DAG_INVALID", 0, chirps);
    const auto stageName = stage.get<std::string>();
    const auto found = roleKeysByStage.find(stageName);
    if (found == roleKeysByStage.end() || !seenStages.insert(stageName).second)
      return baseline("ROLE_DAG_ROLE_COVER_MISMATCH", 0, chirps);
    orderedRoleKeys.insert(orderedRoleKeys.end(), found->second.begin(), found->second.end());
  }
  if (orderedRoleKeys.size() != roles.size())
    return baseline("ROLE_DAG_ROLE_COVER_MISMATCH", 0, chirps);
  std::map<std::string, std::size_t> roleOrderIndex;
  for (std::size_t i = 0; i < orderedRoleKeys.size(); ++i)
    roleOrderIndex.emplace(orderedRoleKeys[i], i);
  const auto expandDependencyEndpoint = [&] (const std::string& endpoint) {
    const auto exactRole = keyByLogicalRole.find(endpoint);
    if (exactRole != keyByLogicalRole.end())
      return std::vector<std::string>{exactRole->second};
    const auto stage = roleKeysByStage.find(endpoint);
    return stage == roleKeysByStage.end() ? std::vector<std::string>{} : stage->second;
  };

  std::vector<V4RoleEdge> edges;
  std::vector<V4RoleEdge> feedbackEdges;
  if (!candidateGraph->contains("dependencies") ||
      !candidateGraph->at("dependencies").is_array())
    return baseline("ROLE_DAG_DEPENDENCIES_MISSING", 0, chirps);
  for (const auto& dependency : candidateGraph->at("dependencies")) {
    requireV4SearchActive(control);
    if (!dependency.is_object() ||
        !dependency.contains("producer_role") || !dependency.at("producer_role").is_string() ||
        !dependency.contains("consumer_role") || !dependency.at("consumer_role").is_string())
      return baseline("ROLE_DAG_DEPENDENCY_INVALID", 0, chirps);
    const auto producer = dependency.at("producer_role").get<std::string>();
    const auto consumer = dependency.at("consumer_role").get<std::string>();
    const auto producers = expandDependencyEndpoint(producer);
    const auto consumers = expandDependencyEndpoint(consumer);
    if (producers.empty() || consumers.empty())
      return baseline("ROLE_DAG_ENDPOINT_UNMAPPED", 0, chirps);
    const auto estimatedBytes = dependency.contains("estimated_bytes") ?
      jsonUnsigned(dependency.at("estimated_bytes")) : std::nullopt;
    const auto operation = dependency.value("operation_kind", std::string{});
    for (const auto& producerKey : producers) {
      for (const auto& consumerKey : consumers) {
        if (operation != "TOKEN_FEEDBACK" &&
            roleOrderIndex.at(producerKey) >= roleOrderIndex.at(consumerKey))
          return baseline("ROLE_DAG_NOT_TOPOLOGICAL", statesExamined, chirps);
        V4RoleEdge edge{producerKey, consumerKey,
          estimatedBytes.value_or(0), estimatedBytes.has_value(), operation};
        if (operation == "TOKEN_FEEDBACK")
          feedbackEdges.push_back(std::move(edge));
        else
          edges.push_back(std::move(edge));
      }
    }
  }

  std::map<std::string, const NativeJson*> offerV4ByProvider;
  if (!snapshot.contains("provider_offers") || !snapshot.at("provider_offers").is_array())
    return baseline("PROVIDER_OFFERS_MISSING", 0, chirps);
  for (const auto& offer : snapshot.at("provider_offers")) {
    if (offer.is_object() && offer.contains("provider") && offer.at("provider").is_string())
      offerV4ByProvider.emplace(offer.at("provider").get<std::string>(), &offer);
  }
  if (offerV4ByProvider.size() != offers.size())
    return baseline("PROVIDER_ROSTER_MISMATCH", 0, chirps);

  std::map<std::string, const NativeSelectionRoleV3*> roleByKey;
  std::map<std::string, std::optional<double>> ackRttByProvider;
  for (const auto& role : roleOrder) {
    const auto key = keyByLogicalRole.at(role.role + "#" + std::to_string(role.rank));
    roleByKey.emplace(key, &role);
  }

  const auto evaluate = [&](const V4BeamState& state, bool complete) {
    V4AssignmentEvaluation result;
    const auto assigned = [&state] (const std::string& key) {
      return state.assignments.find(key) != state.assignments.end();
    };
    std::set<std::string> distinctProviders;
    for (const auto& assignment : state.assignments)
      distinctProviders.insert(assignment.second.provider);
    for (const auto& provider : distinctProviders) {
      const auto offerIt = offerV4ByProvider.find(provider);
      if (offerIt == offerV4ByProvider.end()) {
        makeUnscoreable(result, "PROVIDER_ROSTER_MISMATCH");
        continue;
      }
      const auto& offer = *offerIt->second;
      const auto boot = offer.value("boot_epoch", std::string{});
      auto rtt = requesterAckRttMs(snapshot, provider, boot, nowMs);
      if (!rtt) {
        makeUnscoreable(result, "ACK_RTT_UNKNOWN");
      }
      else {
        result.controlPathMs = std::max(result.controlPathMs, *rtt);
      }
      if (offer.contains("values") && offer.at("values").is_object()) {
        const auto& values = offer.at("values");
        if (values.contains("queue_depth")) {
          const auto depth = jsonUnsigned(values.at("queue_depth"));
          if (depth && *depth <= std::numeric_limits<std::uint64_t>::max() -
              result.queueDepthSum)
            result.queueDepthSum += *depth;
          else
            result.queueDepthComplete = false;
        }
        else {
          result.queueDepthComplete = false;
        }
      }
    }

    std::map<std::string, double> nodeCostMs;
    for (const auto& key : orderedRoleKeys) {
      if (!assigned(key))
        continue;
      const auto& role = *roleByKey.at(key);
      const auto& choice = state.assignments.at(key);
      const auto offerIt = offerV4ByProvider.find(choice.provider);
      if (offerIt == offerV4ByProvider.end()) {
        makeUnscoreable(result, "PROVIDER_ROSTER_MISMATCH");
        continue;
      }
      const auto prep = residencyPreparationMs(*offerIt->second, role, choice);
      if (!prep && m_scoreMode == "preparation-and-network")
        makeUnscoreable(result, "PREPARATION_COST_UNKNOWN");
      double waitMs = 0.0;
      NativeJson waitValue(nullptr);
      const auto& values = offerIt->second->at("values");
      if (values.contains("estimated_wait_ms")) {
        const auto observedWait = jsonNonnegativeNumber(values.at("estimated_wait_ms"));
        if (observedWait) {
          waitMs = *observedWait;
          waitValue = *observedWait;
        }
        else {
          ++result.unknownWaitCount;
        }
      }
      else {
        ++result.unknownWaitCount;
      }
      // Network-only compares one consistent objective across all candidates.
      // Report preparation/wait observations, but exclude them from the score;
      // an unavailable preparation observation remains null in roleCosts.
      const auto localCost = m_scoreMode == "network-only" ? 0.0 :
        prep.value_or(0.0) + waitMs;
      if (!std::isfinite(localCost))
        makeUnscoreable(result, "NODE_COST_OVERFLOW");
      nodeCostMs.emplace(key, std::isfinite(localCost) ? localCost : 0.0);
      NativeJson queueDepth(nullptr);
      if (values.contains("queue_depth")) {
        const auto depth = jsonUnsigned(values.at("queue_depth"));
        if (depth) queueDepth = *depth;
      }
      result.roleCosts.push_back(NativeJson{
        {"role_key", key}, {"provider", choice.provider},
        {"preparation_ms", prep ? NativeJson(*prep) : NativeJson(nullptr)},
        {"queue_wait_ms", waitValue}, {"queue_depth", queueDepth}});
    }

    std::map<std::string, double> finishAt;
    for (const auto& key : orderedRoleKeys) {
      if (!assigned(key))
        continue;
      double predecessorFinish = 0.0;
      for (const auto& edge : edges) {
        if (edge.consumerRole != key || !assigned(edge.producerRole))
          continue;
        const auto& src = state.assignments.at(edge.producerRole).provider;
        const auto& dst = state.assignments.at(edge.consumerRole).provider;
        double transferMs = 0.0;
        NativeJson edgeCost{{"producer_role", edge.producerRole},
          {"consumer_role", edge.consumerRole}, {"src_peer", src}, {"dst_peer", dst},
          {"estimated_bytes", edge.bytesKnown ? NativeJson(edge.bytes) : NativeJson(nullptr)}};
        if (src != dst) {
          const auto measuredRate = findV4MeasuredRate(snapshot, src, dst, nowMs);
          const auto measuredRtt = findV4MeasuredRtt(snapshot, src, dst, nowMs);
          if (!edge.bytesKnown) {
            if (!measuredRate)
              result.missingGoodputEdges.emplace_back(src, dst);
            makeUnscoreable(result, "TRANSFER_BYTES_UNKNOWN");
            continue;
          }
          if (edge.bytes != 0) {
            if (!measuredRate) {
              result.missingGoodputEdges.emplace_back(src, dst);
              makeUnscoreable(result, "TRANSFER_RATE_UNKNOWN");
              continue;
            }
            const long double transfer = static_cast<long double>(edge.bytes) * 8.0L /
              (static_cast<long double>(measuredRate->mbps) * 1000000.0L) * 1000.0L;
            if (!std::isfinite(static_cast<double>(transfer)) || transfer < 0.0L ||
                transfer > std::numeric_limits<double>::max()) {
              makeUnscoreable(result, "TRANSFER_COST_OVERFLOW");
              continue;
            }
            transferMs = static_cast<double>(transfer);
            if (measuredRate->method == "ACTIVE_PROBE_GOODPUT" && measuredRtt)
              transferMs = std::max(transferMs, measuredRtt->milliseconds);
            edgeCost["rate_method"] = measuredRate->method;
            edgeCost["goodput_mbps"] = measuredRate->mbps;
            edgeCost["sample_digest"] = measuredRate->sampleDigest;
            edgeCost["round_trip_ms"] = measuredRtt
              ? NativeJson(measuredRtt->milliseconds) : NativeJson(nullptr);
            edgeCost["round_trip_digest"] = measuredRtt
              ? NativeJson(measuredRtt->sampleDigest) : NativeJson(nullptr);
          }
          else {
            edgeCost["rate_method"] = "KNOWN_ZERO_BYTES";
            edgeCost["goodput_mbps"] = nullptr;
            edgeCost["sample_digest"] = nullptr;
            edgeCost["round_trip_ms"] = nullptr;
            edgeCost["round_trip_digest"] = nullptr;
          }
        }
        else {
          edgeCost["rate_method"] = "LOCAL";
          edgeCost["goodput_mbps"] = nullptr;
          edgeCost["sample_digest"] = nullptr;
        }
        edgeCost["transfer_ms"] = transferMs;
        result.edgeCosts.push_back(std::move(edgeCost));
        const auto upstream = finishAt.find(edge.producerRole);
        if (upstream != finishAt.end())
          predecessorFinish = std::max(predecessorFinish, upstream->second + transferMs);
      }
      const auto node = nodeCostMs.find(key);
      const auto local = node == nodeCostMs.end() ? 0.0 : node->second;
      finishAt[key] = predecessorFinish + local;
      result.dataFlowCriticalPathMs = std::max(result.dataFlowCriticalPathMs,
                                               finishAt[key]);
    }

    if (complete) {
      for (const auto& edge : feedbackEdges) {
        if (!assigned(edge.producerRole) || !assigned(edge.consumerRole))
          continue;
        if (state.assignments.at(edge.producerRole).provider !=
            state.assignments.at(edge.consumerRole).provider)
          makeUnscoreable(result, "REPEATED_FEEDBACK_TRANSFER_UNMODELED");
      }
      if (state.assignments.size() != orderedRoleKeys.size())
        makeUnscoreable(result, "PARTIAL_ASSIGNMENT");
    }
    result.scoreMs = result.controlPathMs + result.dataFlowCriticalPathMs;
    if (!std::isfinite(result.scoreMs))
      makeUnscoreable(result, "PLACEMENT_SCORE_OVERFLOW");
    return result;
  };

  const auto stateLexical = [](const V4BeamState& state) {
    std::vector<std::string> result;
    for (const auto& item : state.assignments)
      result.push_back(item.first + "|" + item.second.provider + "|" +
                       item.second.device + "|" + item.second.backend);
    return result;
  };
  const auto betterPartial = [&](const V4BeamState& left, const V4BeamState& right) {
    const auto leftEval = evaluate(left, false);
    const auto rightEval = evaluate(right, false);
    const auto leftKey = std::make_tuple(!leftEval.scoreable, leftEval.scoreMs,
      leftEval.unknownWaitCount, leftEval.queueDepthComplete ? 0 : 1,
      leftEval.queueDepthSum, stateLexical(left));
    const auto rightKey = std::make_tuple(!rightEval.scoreable, rightEval.scoreMs,
      rightEval.unknownWaitCount, rightEval.queueDepthComplete ? 0 : 1,
      rightEval.queueDepthSum, stateLexical(right));
    return leftKey < rightKey;
  };

  std::vector<V4BeamState> beam(1);
  statesExamined = 1;
  for (const auto& roleKey : orderedRoleKeys) {
    requireV4SearchActive(control);
    const auto& role = *roleByKey.at(roleKey);
    std::vector<V4BeamState> next;
    for (const auto& state : beam) {
      requireV4SearchActive(control);
      std::vector<Cost> choices;
      std::vector<Cost> preferredChoices;
      std::vector<Cost> distinctChoices;
      const auto preferred = context.preferredProvidersByRole.find(roleKey);
      for (const auto& admitted : offers) {
        requireV4SearchActive(control);
        const auto& offer = admitted.observation();
        const auto feasible = feasibleChoices(role, offer, nowMs, nullptr);
        for (const auto& choice : feasible) {
          const auto& device = std::get<9>(choice);
          if (device != "cpu" && state.usedDevices.count(
                std::make_pair(offer.provider, device)) != 0)
            continue;
          choices.push_back(choice);
          if (preferred != context.preferredProvidersByRole.end() &&
              offer.provider == preferred->second)
            preferredChoices.push_back(choice);
          if (state.usedProviders.count(offer.provider) == 0)
            distinctChoices.push_back(choice);
        }
      }
      if (!preferredChoices.empty()) choices.swap(preferredChoices);
      else if (!distinctChoices.empty()) choices.swap(distinctChoices);
      std::sort(choices.begin(), choices.end(), [] (const auto& left, const auto& right) {
        return std::tie(std::get<8>(left), std::get<9>(left), std::get<10>(left)) <
               std::tie(std::get<8>(right), std::get<9>(right), std::get<10>(right));
      });
      for (const auto& choice : choices) {
        requireV4SearchActive(control);
        if (statesExamined >= m_maxAssignmentStates)
          return baseline("MAX_ASSIGNMENT_STATES", statesExamined, chirps);
        ++statesExamined;
        V4BeamState child = state;
        const auto provider = std::get<8>(choice);
        const auto device = std::get<9>(choice);
        const auto backend = std::get<10>(choice);
        child.assignments.emplace(roleKey,
          V4RoleChoice{choice, provider, device, backend});
        child.usedProviders.insert(provider);
        if (device != "cpu") child.usedDevices.insert({provider, device});
        next.push_back(std::move(child));
      }
    }
    if (next.empty())
      return baseline("NO_COMPLETE_FEASIBLE_ASSIGNMENT", statesExamined, chirps);
    std::stable_sort(next.begin(), next.end(), betterPartial);
    if (next.size() > m_beamWidth)
      next.resize(m_beamWidth);
    beam = std::move(next);
  }

  std::optional<V4BeamState> bestState;
  std::optional<V4AssignmentEvaluation> bestEvaluation;
  std::optional<V4BeamState> bestProbeState;
  std::optional<V4AssignmentEvaluation> bestProbeEvaluation;
  std::string firstUnscoreableReason;
  for (const auto& state : beam) {
    requireV4SearchActive(control);
    const auto current = evaluate(state, true);
    if (!current.missingGoodputEdges.empty() &&
        (!bestProbeEvaluation || current.scoreMs < bestProbeEvaluation->scoreMs ||
         (current.scoreMs == bestProbeEvaluation->scoreMs &&
          stateLexical(state) < stateLexical(*bestProbeState)))) {
      bestProbeState = state;
      bestProbeEvaluation = current;
    }
    if (!current.scoreable) {
      if (firstUnscoreableReason.empty()) firstUnscoreableReason = current.reason;
      continue;
    }
    if (!bestEvaluation || current.scoreMs < bestEvaluation->scoreMs ||
        (current.scoreMs == bestEvaluation->scoreMs &&
         std::make_tuple(current.unknownWaitCount, !current.queueDepthComplete,
                         current.queueDepthSum, stateLexical(state)) <
         std::make_tuple(bestEvaluation->unknownWaitCount,
                         !bestEvaluation->queueDepthComplete,
                         bestEvaluation->queueDepthSum, stateLexical(*bestState)))) {
      bestState = state;
      bestEvaluation = current;
    }
  }

  // Probe only directed links in the best optimistic assignment that could
  // beat the currently scoreable assignment. The destination Provider is the
  // observer, so the requester contacts only Providers involved in this
  // candidate rather than asking every Provider to scan its roster.
  if (!context.placementProbeRoundCompleted && bestProbeState &&
      bestProbeEvaluation &&
      (!bestEvaluation || bestProbeEvaluation->scoreMs < bestEvaluation->scoreMs) &&
      !context.allowedGoodputEdges.empty()) {
    std::set<std::pair<std::string, std::string>> allowedGoodputEdges;
    std::set<std::pair<std::string, std::string>> allowedRttPairs;
    for (const auto& edge : context.allowedGoodputEdges) {
      if (edge.is_object() && edge.contains("src_provider") &&
          edge.at("src_provider").is_string() && edge.contains("dst_provider") &&
          edge.at("dst_provider").is_string()) {
        allowedGoodputEdges.emplace(edge.at("src_provider").get<std::string>(),
                                    edge.at("dst_provider").get<std::string>());
      }
    }
    for (const auto& pair : context.allowedRttPairs) {
      if (pair.is_object() && pair.contains("src_provider") &&
          pair.at("src_provider").is_string() && pair.contains("dst_provider") &&
          pair.at("dst_provider").is_string()) {
        allowedRttPairs.emplace(pair.at("src_provider").get<std::string>(),
                                pair.at("dst_provider").get<std::string>());
      }
    }
    std::map<std::string, std::set<std::pair<std::string, std::string>>> edgesByObserver;
    for (const auto& edge : bestProbeEvaluation->missingGoodputEdges) {
      if (allowedGoodputEdges.count(edge) != 0)
        edgesByObserver[edge.second].insert(edge);
    }

    NativePlacementProbeIntentV1 intent;
    std::size_t totalEdges = 0;
    for (const auto& observer : edgesByObserver) {
      if (intent.observers.size() >= 16 || totalEdges >= 16) break;
      NativePlacementProbeObserverV1 request;
      request.observerProvider = observer.first;
      for (const auto& edge : observer.second) {
        if (totalEdges >= 16) break;
        request.goodputEdges.push_back({edge.first, edge.second});
        ++totalEdges;
        const auto reverseRtt = std::make_pair(edge.second, edge.first);
        if (totalEdges < 16 && allowedRttPairs.count(reverseRtt) != 0) {
          request.rttPairs.push_back({reverseRtt.first, reverseRtt.second});
          ++totalEdges;
        }
      }
      if (!request.goodputEdges.empty() || !request.rttPairs.empty())
        intent.observers.push_back(std::move(request));
    }
    if (!intent.observers.empty()) {
      NativeRolePlacementProposalV3 deferred{
        context, ackClosedDigest, policy, {}, {}, {}, std::nullopt, std::nullopt};
      deferred.placementProbeIntent = std::move(intent);
      return deferred;
    }
  }

  if (!bestState || !bestEvaluation) {
    const auto reason = firstUnscoreableReason.empty() ?
      "NO_COMPLETE_SCORED_ASSIGNMENT" : firstUnscoreableReason;
    return baseline(reason, statesExamined, chirps);
  }

  NativeRolePlacementProposalV3 result{
    context, ackClosedDigest, policy, {}, {}, {}, std::nullopt, std::nullopt};
  for (const auto& roleKey : orderedRoleKeys) {
    requireV4SearchActive(control);
    const auto& choice = bestState->assignments.at(roleKey);
    auto role = *roleByKey.at(roleKey);
    role.selectedRole = roleKey;
    role.backend = choice.backend;
    role.deviceSet = choice.device == "cpu" ?
      std::vector<std::string>{} : std::vector<std::string>{choice.device};
    validateNativeAssembly(role);
    result.roles.push_back(std::move(role));
    result.providerByRole.emplace(roleKey, choice.provider);
    const auto admitted = std::find_if(offers.begin(), offers.end(),
      [&choice] (const auto& item) { return item.observation().provider == choice.provider; });
    if (admitted == offers.end())
      throw std::invalid_argument("V4 selected Provider disappeared during placement");
    result.offerDigestByProvider.emplace(choice.provider,
      admitted->observation().offerDigest);
  }
  result.placementDecisionV4 = makeV4Decision(
    context.placementObservationSnapshotDigest, "SCORED", "", m_beamWidth,
    m_maxAssignmentStates, statesExamined,
    assignmentRows(*bestState, roleOrder, keyByLogicalRole, &*bestEvaluation),
    &*bestEvaluation, chirps, m_scoreMode);
  requireV4SearchActive(control);
  return result;
  }
  catch (const V4SearchBudgetDeadline&) {
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - searchStarted).count();
    const auto requestBudgetMs = context.deadlineMs > nowMs ?
      context.deadlineMs - nowMs : 0;
    if (elapsedMs < 0 || static_cast<std::uint64_t>(elapsedMs) >= requestBudgetMs)
      throw std::runtime_error("native placement request deadline exceeded");
    return baseline("POLICY_BUDGET_EXHAUSTED", statesExamined, chirps);
  }
}
} // namespace ndnsf::di
