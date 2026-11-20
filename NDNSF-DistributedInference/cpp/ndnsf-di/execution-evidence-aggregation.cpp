#include "detail/execution-evidence-aggregation.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace ndnsf::di::detail {
namespace {

using EvidenceReferences = std::vector<std::reference_wrapper<const ExecutionEvidence>>;

ExecutionEvidence
aggregateEvidence(const EvidenceReferences& items)
{
  if (items.empty()) {
    throw std::runtime_error("initialized provider runners emitted no execution evidence");
  }
  const auto& first = items.front().get();
  auto aggregate = first;
  auto appendUnique = [] (std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
      values.push_back(value);
    }
  };
  auto seedDeviceIds = [&] (const ExecutionEvidence& evidence) {
    if (!evidence.deviceIds.empty()) {
      for (const auto& value : evidence.deviceIds) appendUnique(aggregate.deviceIds, value);
    }
    else {
      appendUnique(aggregate.deviceIds, evidence.deviceId);
    }
  };
  auto seedGpuUuids = [&] (const ExecutionEvidence& evidence) {
    if (!evidence.gpuUuids.empty()) {
      for (const auto& value : evidence.gpuUuids) appendUnique(aggregate.gpuUuids, value);
    }
    else {
      appendUnique(aggregate.gpuUuids, evidence.gpuUuid);
    }
  };
  aggregate.deviceIds.clear();
  aggregate.gpuUuids.clear();
  seedDeviceIds(first);
  seedGpuUuids(first);
  for (std::size_t i = 1; i < items.size(); ++i) {
    const auto& item = items[i].get();
    if (item.providerName != aggregate.providerName ||
        item.providerBootId != aggregate.providerBootId ||
        item.runnerKind != aggregate.runnerKind ||
        item.realCompute != aggregate.realCompute ||
        item.modelDigest != aggregate.modelDigest ||
        item.planDigest != aggregate.planDigest ||
        item.runtimeVersion != aggregate.runtimeVersion ||
        item.deviceKind != aggregate.deviceKind ||
        item.cpuFallbackUsed != aggregate.cpuFallbackUsed) {
      std::string fields;
      const auto note = [&fields] (const char* name) {
        if (!fields.empty()) fields += ",";
        fields += name;
      };
      if (item.providerName != aggregate.providerName) note("providerName");
      if (item.providerBootId != aggregate.providerBootId) note("providerBootId");
      if (item.runnerKind != aggregate.runnerKind) note("runnerKind");
      if (item.realCompute != aggregate.realCompute) note("realCompute");
      if (item.modelDigest != aggregate.modelDigest) note("modelDigest");
      if (item.planDigest != aggregate.planDigest) note("planDigest");
      if (item.runtimeVersion != aggregate.runtimeVersion) note("runtimeVersion");
      if (item.deviceKind != aggregate.deviceKind) note("deviceKind");
      if (item.cpuFallbackUsed != aggregate.cpuFallbackUsed) note("cpuFallbackUsed");
      std::string detail;
      for (const auto& source : items) {
        const auto& value = source.get();
        std::string roles;
        for (const auto& role : value.roles) {
          if (!roles.empty()) roles += "|";
          roles += role;
        }
        if (!detail.empty()) detail += "; ";
        detail += "roles=" + roles + " model=" + value.modelDigest +
                  " plan=" + value.planDigest;
      }
      throw std::runtime_error(
        "provider runner execution evidence is internally inconsistent: " + fields +
        " [" + detail + "]");
    }
    seedDeviceIds(item);
    seedGpuUuids(item);
    aggregate.roles.insert(aggregate.roles.end(), item.roles.begin(), item.roles.end());
    aggregate.artifactDigests.insert(item.artifactDigests.begin(), item.artifactDigests.end());
    if (aggregate.providerProfilePath.empty()) {
      aggregate.providerProfilePath = item.providerProfilePath;
    }
  }
  if (aggregate.deviceIds.size() == 1) {
    aggregate.deviceId = aggregate.deviceIds.front();
  }
  else if (aggregate.deviceIds.size() > 1) {
    aggregate.deviceId = "multi";
  }
  if (aggregate.gpuUuids.size() == 1) {
    aggregate.gpuUuid = aggregate.gpuUuids.front();
  }
  else if (aggregate.gpuUuids.size() > 1) {
    aggregate.gpuUuid = "multi";
  }
  std::sort(aggregate.roles.begin(), aggregate.roles.end());
  aggregate.roles.erase(std::unique(aggregate.roles.begin(), aggregate.roles.end()),
                        aggregate.roles.end());
  if (items.size() > 1) {
    std::vector<std::reference_wrapper<const ExecutionEvidence::NodeProviderAssignments>> groups;
    groups.reserve(items.size());
    for (const auto& item : items) {
      groups.emplace_back(item.get().getNodeProviderAssignments());
    }
    aggregate.concatenateNodeProviderAssignments(groups);
    // Mixed readiness observations cannot claim one request's execution.
    aggregate.executionCompleted = false;
    aggregate.exactForwardCacheHit = false;
    aggregate.requestId.clear();
    aggregate.attemptEpoch = 0;
    aggregate.profileRequestId.clear();
    aggregate.profileAttemptEpoch = 0;
  }
  aggregate.validate();
  return aggregate;
}

} // namespace

ExecutionEvidence
aggregateExecutionEvidence(const std::vector<ExecutionEvidence>& items)
{
  EvidenceReferences references;
  references.reserve(items.size());
  for (const auto& item : items) references.emplace_back(item);
  return aggregateEvidence(references);
}

ExecutionEvidence
aggregateExecutionEvidence(const std::map<std::string, ExecutionEvidence>& items)
{
  EvidenceReferences references;
  references.reserve(items.size());
  for (const auto& item : items) references.emplace_back(item.second);
  return aggregateEvidence(references);
}

ExecutionEvidence
aggregateExecutionEvidence(const EvidenceReferences& items)
{
  return aggregateEvidence(items);
}

} // namespace ndnsf::di::detail
