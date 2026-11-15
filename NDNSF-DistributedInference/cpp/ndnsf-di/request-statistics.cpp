#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "request-work-statistics.hpp"

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

NativeJson
stepTimingsValue(const std::vector<StepBackendTiming>& timings)
{
  NativeJson records = NativeJson::array();
  for (const auto& record : timings) {
    records.push_back(NativeJson{
      {"stepId", record.stepId}, {"entryId", record.entryId},
      {"operation", record.operation},
      {"startedSteadyNs", record.execution.startedSteadyNs},
      {"finishedSteadyNs", record.execution.finishedSteadyNs},
      {"completed", record.execution.completed},
      {"identity", backendTimingValue(record.execution)}});
  }
  return records;
}

NativeJson
publicationTimingsValue(const std::vector<OutputPublishTiming>& timings)
{
  NativeJson records = NativeJson::array();
  for (const auto& record : timings) {
    const auto started = record.publishStartedAt.time_since_epoch();
    const auto finished = record.publishDoneAt.time_since_epoch();
    records.push_back(NativeJson{
      {"producerRole", record.producerRole}, {"scope", record.scope},
      {"bytes", record.bytes}, {"expectedBytes", record.expectedBytes},
      {"publishDeferred", record.publishDeferred},
      {"publishStartedSteadyNs",
       std::chrono::duration_cast<std::chrono::nanoseconds>(started).count()},
      {"publishDoneSteadyNs",
       std::chrono::duration_cast<std::chrono::nanoseconds>(finished).count()}});
  }
  return records;
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
      {"lastBackendExecution", backendTimingValue(metrics.lastBackendExecution)},
      // Every sealed step's call, so a consumer can attribute ORT time per entry. Empty for a role
      // that ran a whole-model call instead of a template.
      {"stepBackendExecutions", stepTimingsValue(observation.stepBackendTimings)},
      // Each dependency this epoch's unit published, including the partials it sent early.
      {"outputPublications", publicationTimingsValue(observation.outputTimings)}});
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
  const auto clockDomain = nativeWorkClockDomain();
  result["clockDomain"] = clockDomain ? NativeJson(*clockDomain) : NativeJson(nullptr);
  result["workStatistics"] = statistics.workStatistics
    ? nativeParseJson(nativeWorkStatisticsToJson(statistics.workStatistics->snapshot()))
    : NativeJson(nullptr);
  return result.dump();
}

} // namespace ndnsf::di
