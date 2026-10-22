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
      throw std::runtime_error("provider runner execution evidence is internally inconsistent");
    }
    seedDeviceIds(item);
    seedGpuUuids(item);
    aggregate.roles.insert(aggregate.roles.end(), item.roles.begin(), item.roles.end());
    aggregate.artifactDigests.insert(item.artifactDigests.begin(), item.artifactDigests.end());
    aggregate.nodeProviderAssignments.insert(
      aggregate.nodeProviderAssignments.end(),
      item.nodeProviderAssignments.begin(), item.nodeProviderAssignments.end());
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

} // namespace ndnsf::di::detail
