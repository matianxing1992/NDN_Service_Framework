#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace ndnsf::di {
namespace {
bool contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
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
  using Cost = std::tuple<int, std::uint64_t, double, double, std::uint64_t,
                          double, double, double, std::string, std::string, std::string>;

std::vector<Cost> feasibleChoices(const NativeSelectionRoleV3& role,
  const NativeObservedProviderOfferV3& offer, std::uint64_t nowMs,
  const ExtensionControl* control)
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
        const auto reuse = reuseCost(role, offer, device, backend, nowMs, control);
        const bool exact = std::get<0>(reuse) <= 1;
        if ((!exact && offer.executionDisposition != "ACCEPT_WITH_PREPARATION") ||
            (offer.executionDisposition == "ACCEPT_IF_EXACT_REUSE" && !exact)) continue;
        choices.emplace_back(std::tuple_cat(reuse, std::make_tuple(offer.queueDepth, offer.estimatedWaitMs,
          offer.rttMs, -offer.bandwidthMbps, offer.provider, device, backend)));
      }
  return choices;
}
std::map<std::string, std::set<std::uint64_t>> validateSnapshot(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl* control)
{
  if (control) control->requireActive();
  if (roles.empty() || offers.empty() || context.requestId.empty() || !context.attempt ||
      context.serviceName.empty() || !digest(context.modelDigest) || !digest(context.graphDigest) ||
      !digest(ackClosedDigest) || context.deadlineMs <= nowMs)
    throw std::invalid_argument("incomplete V3 planning snapshot");
  std::map<std::string, std::set<std::uint64_t>> ranks;
  for (const auto& role : roles) {
    if (control) control->requireActive();
    if (role.role.empty() || role.backend.empty() || !digest(role.artifactDigest) ||
        !digest(role.recipeDigest) || !digest(role.graphDigest) ||
        role.graphDigest != roles.front().graphDigest ||
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
}

NativeRolePlacementProposalV3 NativePreSplitFirstPlacement::proposeRolesImpl(
  const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
  const std::vector<NativeSelectionRoleV3>& roles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs,
  const ExtensionControl* control) const
{
  if (control) control->requireActive();
  const auto ranks = validateSnapshot(context, ackClosedDigest, roles, offers, nowMs, control);
  NativeRolePlacementProposalV3 result{context, ackClosedDigest, identity(), {}, {}, {}};
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
      const auto eligible = feasibleChoices(role, offer, nowMs, control);
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

void validateNativeRolePlacement(
  const NativeRolePlacementProposalV3& proposal,
  const std::vector<NativeSelectionRoleV3>& preparedRoles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs)
{
  const auto ranks = validateSnapshot(proposal.context, proposal.ackClosedDigest, preparedRoles, offers, nowMs, nullptr);
  proposal.strategy.validate();
  if (proposal.roles.size() != preparedRoles.size() || proposal.providerByRole.size() != preparedRoles.size())
    throw std::invalid_argument("V3 proposal role cover differs from prepared roles");
  std::set<std::string> seenRoles;
  std::set<std::pair<std::string, std::string>> usedDevices;
  std::map<std::pair<std::string, std::string>, std::uint64_t> reservedMemory;
  std::map<std::string, std::string> offerDigests;
  for (const auto& role : proposal.roles) {
    validateNativeAssembly(role);
    const auto original = std::find_if(preparedRoles.begin(), preparedRoles.end(), [&](const auto& r) {
      return r.role == role.role && r.rank == role.rank;
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
    const auto choices = feasibleChoices(*original, admitted->observation(), nowMs, nullptr);
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
    auto expected = *original;
    expected.backend = role.backend; expected.deviceSet = role.deviceSet;
    if (nativeCanonicalJson(nativeAssemblyJson(expected)) != nativeCanonicalJson(nativeAssemblyJson(role)))
      throw std::invalid_argument("V3 proposal modified prepared assembly metadata");
    offerDigests.emplace(assignment->second, admitted->observation().offerDigest);
  }
  if (offerDigests != proposal.offerDigestByProvider)
    throw std::invalid_argument("V3 proposal offer identities differ from admitted observations");
}
} // namespace ndnsf::di
