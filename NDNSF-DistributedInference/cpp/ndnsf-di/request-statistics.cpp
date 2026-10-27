#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

namespace ndnsf::di {
namespace {

NativeJson
clockValue(std::int64_t value)
{
  return value < 0 ? NativeJson(nullptr) : NativeJson(value);
}

NativeJson
backendTimingValue(const std::optional<NativeBackendExecutionTiming>& timing)
{
  if (!timing || !timing->identity) return nullptr;
  const auto& identity = *timing->identity;
  return NativeJson{
    {"requestId", identity.requestId}, {"sessionId", identity.sessionId},
    {"role", identity.role}, {"providerBootId", identity.providerBootId},
    {"attemptEpoch", identity.attemptEpoch}, {"inferenceEpoch", timing->inferenceEpoch},
    {"completed", timing->completed},
    {"startedSteadyNs", timing->startedSteadyNs},
    {"finishedSteadyNs", timing->finishedSteadyNs},
    {"startedProcessCpuNs", clockValue(timing->startedProcessCpuNs)},
    {"finishedProcessCpuNs", clockValue(timing->finishedProcessCpuNs)},
    {"startedThreadCpuNs", clockValue(timing->startedThreadCpuNs)},
    {"finishedThreadCpuNs", clockValue(timing->finishedThreadCpuNs)}};
}

} // namespace

std::string
nativeRequestStatisticsToJson(const NativeRequestStatistics& statistics)
{
  NativeJson result{
    {"schema", "ndnsf-di-native-request-statistics-v1"}, {"type", "request"},
    {"requestId", statistics.requestId}, {"planDigest", statistics.planDigest},
    {"provider", statistics.provider}, {"providerBootId", statistics.providerBootId},
    {"role", statistics.role}, {"attemptEpoch", statistics.attemptEpoch},
    {"admittedModelDigest", statistics.admittedModelDigest},
    {"admittedArtifactDigest", statistics.admittedArtifactDigest},
    {"startedSteadyNs", statistics.startedSteadyNs},
    {"finishedSteadyNs", statistics.finishedSteadyNs},
    {"startedProcessCpuNs", clockValue(statistics.startedProcessCpuNs)},
    {"finishedProcessCpuNs", clockValue(statistics.finishedProcessCpuNs)},
    {"completionAdmitted", statistics.completionAdmitted},
    {"stoppedByUpstream", statistics.stoppedByUpstream},
    {"hasEpochStatistics", statistics.hasEpochStatistics},
    {"epochsExecuted", statistics.epochsExecuted},
    {"eventsPublished", statistics.eventsPublished},
    {"prefixTokensRecomputed", statistics.prefixTokensRecomputed},
    {"failureReason", statistics.failureReason.empty()
      ? NativeJson(nullptr) : NativeJson(statistics.failureReason)},
    {"counterSemantics", "cumulative-adapter-snapshots"},
    {"runtimeMetrics", NativeJson::array()}, {"cacheObservations", NativeJson::array()},
    {"finalExecutionEvidence", statistics.finalExecutionEvidence
      ? nativeParseJson(executionEvidenceToJson(*statistics.finalExecutionEvidence))
      : NativeJson(nullptr)}};
  for (const auto& observation : statistics.runtimeMetrics) {
    const auto& metrics = observation.metrics;
    result["runtimeMetrics"].push_back(NativeJson{
      {"role", observation.role}, {"inferenceEpoch", observation.inferenceEpoch},
      {"stateDeviceToHostBytes", metrics.stateDeviceToHostBytes},
      {"stateHostToDeviceBytes", metrics.stateHostToDeviceBytes},
      {"activationInputBytes", metrics.activationInputBytes},
      {"activationOutputBytes", metrics.activationOutputBytes},
      {"activationHostToDeviceBytes", metrics.activationHostToDeviceBytes},
      {"activationDeviceToHostBytes", metrics.activationDeviceToHostBytes},
      {"controlBytes", metrics.controlBytes}, {"stateInputHits", metrics.stateInputHits},
      {"stateInputMisses", metrics.stateInputMisses},
      {"stateRecomputes", metrics.stateRecomputes}, {"stateReleases", metrics.stateReleases},
      {"lastBackendExecution", backendTimingValue(metrics.lastBackendExecution)}});
  }
  for (const auto& observation : statistics.cacheObservations) {
    result["cacheObservations"].push_back(NativeJson{
      {"inferenceEpoch", observation.inferenceEpoch},
      {"actualNewInputExtent", observation.actualNewInputExtent},
      {"representedPrefixTokenCount", observation.representedPrefixTokenCount},
      {"prefixWorkAvoided", observation.prefixWorkAvoided},
      {"decodeStateHit", observation.decodeStateHit},
      {"conversationStateHit", observation.conversationStateHit}});
  }
  return result.dump();
}

} // namespace ndnsf::di
