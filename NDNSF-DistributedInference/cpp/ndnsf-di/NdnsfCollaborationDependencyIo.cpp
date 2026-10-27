#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <ndn-cxx/util/scope.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

using CollaborationTransferMetrics =
  ndn_service_framework::CollaborationTransferMetrics;

// Keep published Interest bursts bounded independently of the object byte budget.
constexpr std::uint64_t MAX_OUTSTANDING_PUBLISHED_SEGMENTS = 8;

bool
isCanonicalYoloMergeInput(const DependencyEdge& edge)
{
  auto consumerRole = edge.consumerRole;
  while (!consumerRole.empty() && consumerRole.front() == '/') {
    consumerRole.erase(consumerRole.begin());
  }
  return consumerRole == "Merge" && edge.tensors.size() == 1 &&
    edge.tensors.front().rfind("/model/model.23/one2one_", 0) == 0;
}

void
validateCanonicalYoloMergeInput(const DependencyEdge& edge,
                                const TensorBundle& bundle)
{
  if (!isCanonicalYoloMergeInput(edge)) {
    return;
  }
  if (!isEncodedTensorBundle(bundle.payload)) {
    throw std::runtime_error(
      "YOLO_MERGE_DEPENDENCY_NOT_ENCODED: " + edge.tensors.front());
  }
  const auto tensors = decodeTensorBundle(bundle.payload);
  auto expectedNames = edge.bundleTensorNames.empty() ?
    edge.tensors : edge.bundleTensorNames;
  auto actualNames = std::vector<std::string>{};
  actualNames.reserve(tensors.size());
  for (const auto& tensor : tensors) {
    actualNames.push_back(tensor.name);
  }
  std::sort(expectedNames.begin(), expectedNames.end());
  std::sort(actualNames.begin(), actualNames.end());
  auto endpointNames = edge.tensors;
  std::sort(endpointNames.begin(), endpointNames.end());
  if (std::adjacent_find(expectedNames.begin(), expectedNames.end()) !=
        expectedNames.end() ||
      std::adjacent_find(endpointNames.begin(), endpointNames.end()) !=
        endpointNames.end() ||
      !std::all_of(expectedNames.begin(), expectedNames.end(), [] (const auto& name) {
        return name.rfind("/model/model.23/one2one_", 0) == 0;
      }) ||
      !std::all_of(endpointNames.begin(), endpointNames.end(), [&] (const auto& name) {
        return std::binary_search(expectedNames.begin(), expectedNames.end(), name);
      }) ||
      actualNames != expectedNames) {
    throw std::runtime_error(
      "YOLO_MERGE_DEPENDENCY_TENSOR_SELECTION_MISMATCH: " + edge.tensors.front());
  }
}

const GroupMemberV1*
memberForRank(const GroupCapabilityV1& capability,
              const std::string& producerRank)
{
  const auto member = std::find_if(
    capability.orderedMembers.begin(), capability.orderedMembers.end(),
    [&producerRank] (const auto& item) {
      return std::to_string(item.rank) == producerRank;
    });
  return member == capability.orderedMembers.end() ? nullptr : &*member;
}

void
addTransferMetric(std::optional<std::size_t>& target,
                  const std::optional<std::size_t>& value)
{
  if (!value) {
    return;
  }
  target = target.value_or(0) + *value;
}

void
mergeTransferMetrics(CollaborationTransferMetrics& target,
                     const CollaborationTransferMetrics& source)
{
  addTransferMetric(target.transportPayloadBytes, source.transportPayloadBytes);
  addTransferMetric(target.metadataBytes, source.metadataBytes);
  addTransferMetric(target.wireBytes, source.wireBytes);
  addTransferMetric(target.interestCount, source.interestCount);
  addTransferMetric(target.retryCount, source.retryCount);
  if (source.localCopyBytes) {
    target.localCopyBytes = target.localCopyBytes.value_or(0) +
      *source.localCopyBytes;
  }
}

void
populateLineageObservation(StageTransferObservation& observation,
                           const TensorBundle& bundle)
{
  observation.lineagePresent = false;
  observation.lineageIdentity.clear();
  observation.positionDigest.clear();
  if (const auto lineage = extractGenerationEpochLineage(bundle)) {
    observation.lineagePresent = true;
    observation.positionDigest = lineage->positionDigest;
    observation.lineageIdentity = lineage->requestId + "|" +
      lineage->generationId + "|" + std::to_string(lineage->streamEpoch) +
      "|" + std::to_string(lineage->inferenceEpoch) + "|" +
      lineage->transitionKind + "|" + lineage->positionDigest + "|" +
      lineage->producerRole + "|" + lineage->consumerRole + "|" +
      std::to_string(lineage->operationIndex);
  }
}

void
attachTransferObservation(const DependencyEdge& edge,
                          const TensorBundle& bundle,
                          const std::string& direction,
                          const CollaborationTransferMetrics& metrics)
{
  if (!bundle.transferObservation) {
    bundle.transferObservation = std::make_shared<StageTransferObservation>();
  }
  auto& observation = *bundle.transferObservation;
  observation.edgeScope = edge.scope;
  observation.plannedDataName = edge.plannedDataName;
  observation.actualDataName = metrics.actualDataName;
  observation.direction = direction;
  observation.phase = canonicalStagePhase(edge.operationKind);
  observation.identity = edge.requestId + "|" +
    std::to_string(edge.attemptEpoch) + "|" + edge.scope + "|" +
    edge.plannedDataName + "|" + direction + "|" + edge.operationKind +
    "|" + std::to_string(edge.round) + "|" +
    std::to_string(edge.microbatch);
  populateLineageObservation(observation, bundle);
  if (!observation.lineageIdentity.empty()) {
    observation.identity += "|" + observation.lineageIdentity;
  }
  observation.tensorBytes = tensorPayloadBytes(bundle);
  observation.encodedPayloadBytes = bundle.payload.size();
  observation.transportPayloadBytes = metrics.transportPayloadBytes;
  observation.metadataBytes = metrics.metadataBytes;
  observation.wireBytes = metrics.wireBytes;
  observation.interestCount = metrics.interestCount;
  observation.retryCount = metrics.retryCount;
  if (metrics.localCopyBytes) {
    observation.transportLocalCopyBytes = metrics.localCopyBytes;
  }
}

std::string
producerRankForEdge(const GroupCapabilityV1& capability,
                    const DependencyEdge& edge)
{
  if (!edge.producerProvider.empty()) {
    const auto member = std::find_if(
      capability.orderedMembers.begin(), capability.orderedMembers.end(),
      [&edge] (const auto& item) {
        const ndn::Name producer(edge.producerProvider);
        return ndn::Name(item.provider) == producer;
      });
    if (member != capability.orderedMembers.end()) {
      if (!edge.collectiveProducerRank.empty() &&
          edge.collectiveProducerRank != std::to_string(member->rank))
        throw std::runtime_error("tensor Producer rank conflicts with its complete namespace");
      return std::to_string(member->rank);
    }
    throw std::runtime_error("tensor Producer is not present in the capability");
  }
  return edge.collectiveProducerRank;
}

const GroupOperationV1&
operationForEdge(const GroupCapabilityV1& capability,
              std::uint64_t operationIndex)
{
  const auto found = std::find_if(
    capability.permittedOperations.begin(), capability.permittedOperations.end(),
    [operationIndex] (const auto& operation) {
      return operation.operationIndex == operationIndex;
    });
  if (found == capability.permittedOperations.end()) {
    throw std::runtime_error("NDNSF_DATA_V1 operation is not permitted");
  }
  return *found;
}

bool
dependencyObjectTraceEnabled()
{
  const auto enabled = [] (const char* key) {
    const auto* value = std::getenv(key);
    return value != nullptr && *value != '\0' && *value != '0';
  };
  return enabled("NDNSF_DI_RUNTIME_TIMING") ||
         enabled("NDNSF_DI_DEPENDENCY_OBJECT_TRACE");
}

std::uint64_t
nowEpochMs()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<
    std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string
canonicalSha256(std::string value)
{
  if (value.compare(0, 7, "sha256:") != 0) {
    value = "sha256:" + value;
  }
  return value;
}

ndn::Name
exactTensorSegmentName(const DependencyEdge& edge, std::size_t segment)
{
  if (edge.plannedDataName.empty() || edge.maxSegments == 0 ||
      segment >= edge.maxSegments) {
    throw std::invalid_argument("V3 tensor segment is outside mayPublish/mustFetch");
  }
  return makeTensorSegmentDataName(ndn::Name(edge.plannedDataName), segment);
}

void
validateTensorReference(const DependencyEdge& edge, const GroupMemberV1& member)
{
  using namespace ndn_service_framework;
  if (!edge.declaredByV3 || edge.tensors.size() != 1 || edge.producerRole.empty() ||
      edge.endpointDigest.size() != 71 || edge.endpointDigest.substr(0, 7) != "sha256:" ||
      edge.namingOperationStride > (1U << 20) ||
      edge.round != edge.collectiveOperationIndex || edge.plannedDataName.empty())
    throw std::invalid_argument("C04 tensor reference is incomplete");
  const ndn::Name base(edge.plannedDataName);
  const auto parsed = parseProducerDataName(base, ndn::Name(member.provider));
  if (!parsed || parsed->nameSpace != "NDNSF-DI" || parsed->category != "tensor" ||
      parsed->suffix.size() != 18 ||
      ndn::Name(edge.manifestDataName) != ndn::Name(base).append("MANIFEST"))
    throw std::invalid_argument("C04 tensor reference category or Producer mismatch");
  const auto endpoint = makeNamingDigestComponent(edge.endpointDigest.substr(7));
  const auto object = makeNamingDigestComponent(edge.executionObjectDigest);
  const auto key = computeNamingDigest("ndnsf-naming/tensor-key", {
    NamingDigestField{1, 4, {endpoint.value_begin(), endpoint.value_end()}},
    NamingDigestField{2, 4, {object.value_begin(), object.value_end()}}});
  const auto stride = edge.namingOperationStride;
  const auto round = stride == 0 ? 0 : edge.round / stride;
  const auto index = stride == 0 ? edge.round : edge.round % stride;
  const auto& suffix = parsed->suffix;
  const char* markers[] = {"REQUEST", "ATTEMPT", "ROUND", "POSITION",
                          "OP", "INDEX", "RANK", "BATCH"};
  for (std::size_t i = 0; i < 8; ++i)
    if (suffix.get(i * 2) != ndn::name::Component(markers[i]))
      throw std::invalid_argument("C04 tensor reference field order mismatch");
  if (suffix.get(1) != makeNamingLabelComponent(
        ndn::Name(edge.requestId).toUri(ndn::name::UriFormat::CANONICAL)) ||
      suffix.get(3) != makeNamingDecimalComponent(edge.attemptEpoch) ||
      suffix.get(5) != makeNamingDecimalComponent(round) ||
      suffix.get(7) != makeNamingLabelComponent(edge.producerRole) ||
      suffix.get(9) != ndn::name::Component(ndn::tlv::GenericNameComponent,
        edge.operationKind.begin(), edge.operationKind.end()) ||
      suffix.get(11) != makeNamingDecimalComponent(index) ||
      suffix.get(13) != makeNamingDecimalComponent(member.rank) ||
      suffix.get(15) != makeNamingDecimalComponent(edge.microbatch) ||
      suffix.get(16) != makeNamingLabelComponent(edge.tensors.front()) ||
      suffix.get(17) != makeNamingDigestComponent(key))
    throw std::invalid_argument("C04 tensor readable fields or commitment mismatch");
  checkNamingNameSize(base);
}

std::uint64_t
remainingDeadlineMs(std::chrono::steady_clock::time_point deadline)
{
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  return static_cast<std::uint64_t>(std::max<std::int64_t>(
    1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
}

void
logDependencyObject(const std::string& sessionId,
                    const DependencyEdge& edge,
                    const char* direction,
                    std::size_t payloadBytes,
                    const char* status,
                    std::size_t encodedPayloadBytes = 0)
{
  if (!dependencyObjectTraceEnabled()) {
    return;
  }
  std::ostringstream record;
  record << "NDNSF_DI_DEPENDENCY_OBJECT"
         << " session=" << sessionId
         << " scope=" << edge.scope
         << " producer=" << edge.producerRole
         << " consumer=" << edge.consumerRole
         << " direction=" << direction
         << " payload_bytes=" << payloadBytes
         << " encoded_payload_bytes=" << (encodedPayloadBytes == 0 ?
                                             payloadBytes : encodedPayloadBytes)
         << " planned_name=" << (edge.plannedDataName.empty() ? "none" : edge.plannedDataName)
         << " status=" << status;
  logRuntimeEvidence(record.str());
}

void
logDependencyStageMarker(const char* status,
                          const std::string& sessionId,
                          const DependencyEdge& edge,
                          const std::string& provider)
{
  const auto steadyUs = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  std::ostringstream record;
  record << "NDNSF_DI_PROVIDER_STAGE"
         << " stage=DEPENDENCY_FETCH"
         << " status=" << status
         // The coordinator session is attempt-scoped (request/attempt),
         // while the provider-stage identity is the authenticated request
         // identity shared by Selection and the other stage markers.
         << " requestId=" << (edge.requestId.empty() ? sessionId : edge.requestId)
         << " provider=" << provider
         << " role=" << edge.consumerRole
         << " planDigest=" << edge.planDigest
         << " attemptEpoch=" << edge.attemptEpoch
         << " steady_us=" << steadyUs
         << " sessionId=" << sessionId
         << " scope=" << edge.scope
         << " plannedName=" << (edge.plannedDataName.empty() ? "none" : edge.plannedDataName)
         << " operationIndex=" << edge.collectiveOperationIndex
         << " round=" << edge.round
         << " microbatch=" << edge.microbatch;
  logRuntimeEvidence(record.str());
}

} // namespace

NdnsfCollaborationDependencyIo::NdnsfCollaborationDependencyIo(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  int fetchTimeoutMs,
  std::size_t maxSegmentSize,
  int freshnessMs,
  std::shared_ptr<ProviderGroupCoordinator> groupCoordinator,
  std::shared_ptr<ProtectedRuntime> protectedRuntime,
  std::size_t prefetchWorkerCount)
  : m_ctx(ctx)
  , m_fetchTimeoutMs(fetchTimeoutMs)
  , m_maxSegmentSize(maxSegmentSize)
  , m_freshnessMs(freshnessMs)
  , m_groupCoordinator(std::move(groupCoordinator))
  , m_protectedRuntime(std::move(protectedRuntime))
{
  if (prefetchWorkerCount != 0) {
    m_prefetchWorkers = std::make_unique<ndn_service_framework::BoundedWorkerPool>(
      "DI dependency prefetch", prefetchWorkerCount);
    m_prefetchWorkers->setThreadCount(prefetchWorkerCount);
  }
}

NdnsfCollaborationDependencyIo::~NdnsfCollaborationDependencyIo()
{
  if (m_prefetchWorkers) {
    cancelPendingPrefetches();
    drainPendingPrefetches();
  }
}

std::future<TensorBundle>
NdnsfCollaborationDependencyIo::prefetchInput(const std::string& sessionId,
                                              const DependencyEdge& edge)
{
  auto operation = prefetchInputWithFirstReadBarrier(sessionId, edge);
  return std::move(operation.result);
}

PromptCancellableDependencyIo::PrefetchOperation
NdnsfCollaborationDependencyIo::prefetchInputWithFirstReadBarrier(
  const std::string& sessionId,
  const DependencyEdge& edge)
{
  if (edge.plannedDataName.empty() && !edge.useNdnsfDataV1) {
    throw std::invalid_argument(
      "NdnsfCollaborationDependencyIo requires plannedDataName for input " +
      edge.scope);
  }
  const auto cancelled = m_pendingPrefetchCancelled;
  auto firstReadPromise = std::make_shared<std::promise<void>>();
  auto firstReadStarted = firstReadPromise->get_future().share();
  auto firstReadReported = std::make_shared<std::atomic<bool>>(false);
  auto reportFirstRead = [firstReadPromise, firstReadReported]
                         (std::exception_ptr error) noexcept {
    bool expected = false;
    if (!firstReadReported->compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      if (error) {
        firstReadPromise->set_exception(std::move(error));
      }
      else {
        firstReadPromise->set_value();
      }
    }
    catch (...) {
    }
  };
  auto fetch = [this, sessionId, edge, cancelled, reportFirstRead] {
   try {
    if (cancelled->load(std::memory_order_acquire)) {
      throw std::runtime_error("dependency prefetch was cancelled");
    }
    logDependencyStageMarker("begin", sessionId, edge,
                             m_ctx.localProvider().toUri());
    if (edge.useNdnsfDataV1 && !edge.declaredByV3)
      throw std::invalid_argument("C04 tensor endpoint is required for network dependency fetch");
    // Cross-role feedback is a signed endpoint; same-role feedback is served
    // exclusively by LocalDependencyIo and never reaches this network path.
    if (m_protectedRuntime) {
      m_protectedRuntime->authorizeDataflow(
        ProtectedDataflowDirection::Fetch, edge.endpointDigest,
        edge.producerRole, edge.consumerRole, nowEpochMs());
    }
    CollaborationTransferMetrics transportMetrics;
    if (edge.useNdnsfDataV1) {
      if (!m_groupCoordinator || !m_groupCoordinator->hasCapability()) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 dependency requires an installed group capability");
      }
      const auto& capability = m_groupCoordinator->capability();
      const auto& operation = operationForEdge(
        capability, edge.collectiveOperationIndex);
      if (operation.kind != edge.operationKind)
        throw std::invalid_argument("tensor operation differs from the signed capability");
      const auto producerRank = producerRankForEdge(capability, edge);
      const auto* producerMember = memberForRank(capability, producerRank);
      if (producerMember == nullptr)
        throw std::invalid_argument("tensor Producer rank is not in the capability");
      validateTensorReference(edge, *producerMember);
      if (!m_groupCoordinator->beginOperation(nowEpochMs())) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 group cannot begin dependency operation");
      }
      if (edge.declaredByV3) {
        if (producerMember == nullptr || edge.manifestDataName.empty() ||
            edge.maxSegments == 0 || edge.endpointDigest.empty() ||
            edge.manifestContractDigest.empty() ||
            edge.securityProfile != "NDNSF_DATA_V1" ||
            edge.noProgressDeadlineMs == 0 || edge.hardDeadlineMs == 0) {
          throw std::runtime_error(
            "V3 mustFetch endpoint is incomplete or targets the wrong producer");
        }
        const auto started = std::chrono::steady_clock::now();
        const auto hardDeadline = started + std::chrono::milliseconds(
          edge.hardDeadlineMs);
        auto fetchExact = [this, &edge, &hardDeadline, producerMember,
                           &transportMetrics, cancelled, reportFirstRead](
                            const ndn::Name& name,
                            bool initialProducerReadiness) {
          if (cancelled->load(std::memory_order_acquire)) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 dependency fetch was cancelled");
          }
          if (m_groupCoordinator->terminal()) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 group is terminal before exact fetch");
          }
          const auto remaining = remainingDeadlineMs(hardDeadline);
          if (remaining == 0) {
            throw std::runtime_error("NDNSF_DATA_V1 hard deadline expired");
          }
          // Before the producer publishes its first manifest, no transport
          // progress can be observed by the consumer.  That readiness wait is
          // bounded by the request hard deadline and fetch budget, but must not
          // consume the post-publication no-progress window.  Once the
          // manifest is available, every segment fetch is subject to the
          // ordinary no-progress bound.
          const auto fetchBudget = initialProducerReadiness
            ? static_cast<std::uint64_t>(m_fetchTimeoutMs)
            : std::min<std::uint64_t>(
                edge.noProgressDeadlineMs,
                static_cast<std::uint64_t>(m_fetchTimeoutMs));
          const auto bounded = static_cast<int>(std::min<std::uint64_t>(
            remaining, fetchBudget));
          CollaborationTransferMetrics segmentMetrics;
          auto content = m_ctx.fetchSignedExactData(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            name,
            ndn::Name(producerMember->provider),
            std::max(1, bounded),
            [coordinator = m_groupCoordinator, cancelled] {
              return cancelled->load(std::memory_order_acquire) ||
                     coordinator->terminal();
            },
            &segmentMetrics,
            reportFirstRead,
            initialProducerReadiness
              ? ndn_service_framework::ServiceProvider::ExactDataFetchPhase::AwaitPublication
              : ndn_service_framework::ServiceProvider::ExactDataFetchPhase::Published);
          if (!content) {
            if (cancelled->load(std::memory_order_acquire)) {
              throw std::runtime_error(
                "NDNSF_DATA_V1 dependency fetch was cancelled");
            }
            throw std::runtime_error(
              "failed to fetch signed exact Data: " + name.toUri());
          }
          mergeTransferMetrics(transportMetrics, segmentMetrics);
          if (initialProducerReadiness) {
            // The first manifest fetch is a producer-readiness wait.  It is
            // explicitly allowed to outlive noProgressMs while the producer
            // prepares its runner; only once the manifest is available does
            // the operation's transport no-progress window begin.
            if (!m_groupCoordinator->beginOperation(nowEpochMs())) {
              throw std::runtime_error(
                "NDNSF_DATA_V1 readiness ended after group deadline or cancellation");
            }
          }
          else if (!m_groupCoordinator->recordProgress(nowEpochMs())) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 no-progress/hard deadline or cancellation");
          }
          return std::vector<std::uint8_t>(content->begin(), content->end());
        };

        struct SegmentFetchResult
        {
          std::vector<std::uint8_t> wire;
          CollaborationTransferMetrics metrics;
          bool received = false;
          std::exception_ptr failure;
        };
        const auto batchCancelled = std::make_shared<std::atomic<bool>>(false);
        using SteadyClock = std::chrono::steady_clock;
        const auto speculativeDeadline =
          std::make_shared<std::atomic<SteadyClock::duration::rep>>(0);
        auto speculativeExpired = [speculativeDeadline] {
          const auto ticks = speculativeDeadline->load(std::memory_order_acquire);
          return ticks != 0 && SteadyClock::now().time_since_epoch().count() >= ticks;
        };
        using ExactOperation = ndn_service_framework::ServiceProvider::ExactDataFetchOperation;
        std::optional<ExactOperation> speculativeFirst;
        std::optional<ExactOperation> speculativeWhole;
        // Signal abort before futures drain on every exit, including an
        // exception. Workers borrow this prefetch's context and deadline.
        struct AbortOnExit
        {
          std::shared_ptr<std::atomic<bool>> cancelled;
          ~AbortOnExit() { cancelled->store(true, std::memory_order_release); }
        } abortOnExit{batchCancelled};
        auto fetchSegment =
          [this, &edge, hardDeadline, producerMember, cancelled,
           batchCancelled, reportFirstRead,
           speculativeDeadline, speculativeExpired,
           coordinator = m_groupCoordinator](const ndn::Name& dataName,
                                             bool producerReadiness = false) {
            const auto remaining = remainingDeadlineMs(hardDeadline);
            if (remaining == 0 || cancelled->load(std::memory_order_acquire) ||
                batchCancelled->load(std::memory_order_acquire) || coordinator->terminal() ||
                (producerReadiness && speculativeExpired())) {
              throw std::runtime_error("NDNSF_DATA_V1 segment fetch aborted");
            }
            auto budget = std::min<std::uint64_t>(remaining,
              producerReadiness ? static_cast<std::uint64_t>(m_fetchTimeoutMs) :
                std::min<std::uint64_t>(edge.noProgressDeadlineMs,
                  static_cast<std::uint64_t>(m_fetchTimeoutMs)));
            if (producerReadiness) {
              const auto ticks = speculativeDeadline->load(std::memory_order_acquire);
              if (ticks != 0) {
                budget = std::min(budget, remainingDeadlineMs(
                  SteadyClock::time_point(SteadyClock::duration(ticks))));
              }
              if (budget == 0) {
                throw std::runtime_error("NDNSF_DATA_V1 speculative window expired");
              }
            }
            SegmentFetchResult result;
            try {
              const auto fetchTiming = detail::nativePhaseStarted();
              auto content = m_ctx.fetchSignedExactData(
                edge.transportScope.empty() ? edge.scope : edge.transportScope,
                dataName, ndn::Name(producerMember->provider),
                std::max(1, static_cast<int>(budget)),
                [cancelled, batchCancelled, coordinator, hardDeadline,
                 producerReadiness, speculativeExpired] {
                  return cancelled->load(std::memory_order_acquire) ||
                         batchCancelled->load(std::memory_order_acquire) ||
                         coordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0 ||
                         (producerReadiness && speculativeExpired());
                }, &result.metrics,
                producerReadiness ? std::function<void(std::exception_ptr)>{} :
                  std::function<void(std::exception_ptr)>{reportFirstRead},
                producerReadiness
                  ? ndn_service_framework::ServiceProvider::ExactDataFetchPhase::AwaitPublication
                  : ndn_service_framework::ServiceProvider::ExactDataFetchPhase::Published);
              if (!content) {
                throw std::runtime_error(
                  "failed to fetch signed exact Data: " + dataName.toUri());
              }
              if (producerReadiness && content->size() > ndn::MAX_NDN_PACKET_SIZE) {
                throw std::runtime_error("speculative first segment exceeds packet budget");
              }
              result.wire = std::move(*content);
              result.received = true;
              detail::logNativeWorkPhase(edge.requestId, "tensor_segment_fetch_wait",
                                        fetchTiming, edge.consumerRole);
            }
            catch (...) {
              if (!producerReadiness) throw;
              // Early transport is optional. Preserve the failed attempt's
              // metrics; the owner decides whether the original fetch still
              // fits the fixed post-manifest window before any fallback.
              result.failure = std::current_exception();
            }
            return result;
          };
        const auto prefetchCount = edge.prefetchBundleBytes != 0 && edge.prefetchSegmentBytes != 0
          ? 1 + (edge.prefetchBundleBytes - 1) / edge.prefetchSegmentBytes : 0;
        const bool speculativeWholeStarted = prefetchCount > (edge.inlineFirstSegment ? 1U : 0U) &&
          edge.prefetchSegmentBytes <= ndn::MAX_NDN_PACKET_SIZE &&
          prefetchCount <= edge.maxSegments && prefetchCount <= operation.maxSegments &&
          edge.prefetchBundleBytes <= operation.maxBytes &&
          capability.maxInflightBytes / ndn::MAX_NDN_PACKET_SIZE > prefetchCount;
        bool speculativeWholeUsed = false;
        bool speculativeWholeFallback = false;
        if (speculativeWholeStarted) {
          std::vector<ndn::Name> names;
          names.reserve(prefetchCount);
          for (std::size_t index = edge.inlineFirstSegment ? 1U : 0U; index < prefetchCount; ++index)
            names.push_back(exactTensorSegmentName(edge, index));
          speculativeWhole.emplace(m_ctx.startSignedExactDataBatch(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            names, ndn::Name(producerMember->provider),
            std::max(1, static_cast<int>(std::min<std::uint64_t>(
              remainingDeadlineMs(hardDeadline), static_cast<std::uint64_t>(m_fetchTimeoutMs)))),
            [cancelled, batchCancelled, coordinator = m_groupCoordinator,
             hardDeadline, speculativeExpired] {
              return cancelled->load(std::memory_order_acquire) ||
                batchCancelled->load(std::memory_order_acquire) || coordinator->terminal() ||
                remainingDeadlineMs(hardDeadline) == 0 || speculativeExpired();
            }, {}, ndn_service_framework::ServiceProvider::ExactDataFetchPhase::AwaitPublication,
            [cancelled, batchCancelled, coordinator = m_groupCoordinator,
             hardDeadline, speculativeDeadline, noProgressMs = edge.noProgressDeadlineMs] {
              if (cancelled->load(std::memory_order_acquire) ||
                  batchCancelled->load(std::memory_order_acquire) ||
                  remainingDeadlineMs(hardDeadline) == 0 || coordinator->terminal()) return false;
              // Producer readiness ends only when the manifest is available.
              // The transport progress window then advances on validated Data.
              if (speculativeDeadline->load(std::memory_order_acquire) != 0) {
                if (!coordinator->recordProgress(nowEpochMs())) return false;
                speculativeDeadline->store(std::min(hardDeadline, SteadyClock::now() +
                  std::chrono::milliseconds(noProgressMs)).time_since_epoch().count(),
                  std::memory_order_release);
              }
              return true;
            }));
        }
        const bool speculativeFirstStarted = !edge.inlineFirstSegment && !speculativeWholeStarted && capability.maxInflightBytes >=
          2 * static_cast<std::uint64_t>(ndn::MAX_NDN_PACKET_SIZE);
        bool speculativeFirstUsed = false;
        bool speculativeFirstFallback = false;
        if (speculativeFirstStarted) {
          // Queue readiness on the same Face as the manifest. The operation
          // retains handlers; no extra waiting worker is needed for overlap.
          speculativeFirst.emplace(m_ctx.startSignedExactDataBatch(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            {exactTensorSegmentName(edge, 0)}, ndn::Name(producerMember->provider),
            std::max(1, static_cast<int>(std::min<std::uint64_t>(
              remainingDeadlineMs(hardDeadline), static_cast<std::uint64_t>(m_fetchTimeoutMs)))),
            [cancelled, batchCancelled, coordinator = m_groupCoordinator,
             hardDeadline, speculativeExpired] {
              return cancelled->load(std::memory_order_acquire) ||
                batchCancelled->load(std::memory_order_acquire) || coordinator->terminal() ||
                remainingDeadlineMs(hardDeadline) == 0 || speculativeExpired();
            }, {}, ndn_service_framework::ServiceProvider::ExactDataFetchPhase::AwaitPublication));
        }

        const auto expectedConsumers = edge.consumerRoles.empty()
          ? std::vector<std::string>{edge.consumerRole} : edge.consumerRoles;
        const auto manifestFetchTiming = detail::nativePhaseStarted();
        auto manifestWire = fetchExact(
          ndn::Name(edge.manifestDataName), true);
        std::vector<std::uint8_t> inlineFirstWire;
        if (edge.inlineFirstSegment) {
          auto object = decodeTensorManifestWithFirstSegment(
            manifestWire, ndn::MAX_NDN_PACKET_SIZE);
          manifestWire = std::move(object.first);
          inlineFirstWire = std::move(object.second);
        }
        const bool inlineFirstReceived = !inlineFirstWire.empty();
        detail::logNativeWorkPhase(edge.requestId, "tensor_manifest_fetch_wait",
                                  manifestFetchTiming, edge.consumerRole);
        const auto manifestFetchedAt = std::chrono::steady_clock::now();
        speculativeDeadline->store(std::min(hardDeadline,
          manifestFetchedAt + std::chrono::milliseconds(edge.noProgressDeadlineMs))
            .time_since_epoch().count(), std::memory_order_release);
        const auto manifestDecodeTiming = detail::nativePhaseStarted();
        auto manifest = decodeTensorObjectManifest(manifestWire);
        std::size_t parsedProducerRank = 0;
        try {
          parsedProducerRank = static_cast<std::size_t>(
            std::stoull(producerRank));
        }
        catch (const std::exception&) {
          throw std::runtime_error("V3 producer rank is not numeric");
        }
        if (manifest.compactContextRequired) {
          // The authenticated Selection edge and group capability are the
          // authority for these bindings.  The context-compact wire only
          // carries the content/segment commitment and producer signature;
          // restore the signed manifest before normal validation below.
          manifest.capabilityDigest = canonicalSha256(capability.capabilityDigest);
          manifest.epochKeyId = capability.epochKeyId;
          manifest.requester = m_ctx.requesterName().toUri(ndn::name::UriFormat::CANONICAL);
          manifest.requestId = edge.requestId;
          manifest.attemptId = std::to_string(edge.attemptEpoch);
          manifest.planDigest = edge.planDigest;
          manifest.groupId = capability.groupId;
          manifest.epoch = std::to_string(capability.epoch);
          manifest.operationIndex = edge.collectiveOperationIndex;
          manifest.round = edge.round;
          manifest.operationKind = edge.operationKind;
          manifest.producerRole = edge.producerRole;
          manifest.producerRank = parsedProducerRank;
          manifest.consumerRoles = expectedConsumers;
          manifest.microbatch = edge.microbatch;
          manifest.sourceLayoutDigest = edge.collectiveSourceLayoutDigest;
          manifest.targetLayoutDigest = edge.collectiveTargetLayoutDigest;
          manifest.tensorId = edge.tensors.empty() ? edge.scope : edge.tensors.front();
          manifest.tensorDigest = edge.tensorDigest;
          manifest.endpointDigest = edge.endpointDigest;
          manifest.manifestContractDigest = edge.manifestContractDigest;
          manifest.noProgressMs = edge.noProgressDeadlineMs;
          manifest.hardDeadlineMs = edge.hardDeadlineMs;
        }
        const auto consumerPresent = std::find(
          manifest.consumerRoles.begin(), manifest.consumerRoles.end(),
          edge.consumerRole) != manifest.consumerRoles.end();
        if (manifest.capabilityDigest !=
              canonicalSha256(capability.capabilityDigest) ||
            manifest.epochKeyId != capability.epochKeyId ||
            ndn::Name(manifest.requester) != m_ctx.requesterName() ||
            manifest.requestId != edge.requestId ||
            manifest.attemptId != std::to_string(edge.attemptEpoch) ||
            manifest.planDigest != edge.planDigest ||
            manifest.planDigest != capability.planDigest ||
            manifest.groupId != capability.groupId ||
            manifest.epoch != std::to_string(capability.epoch) ||
            manifest.operationIndex != edge.collectiveOperationIndex ||
            manifest.round != edge.round ||
            manifest.operationKind != edge.operationKind ||
            manifest.producerRole != edge.producerRole ||
            manifest.producerRank != parsedProducerRank ||
            manifest.consumerRoles != expectedConsumers ||
            !consumerPresent || manifest.microbatch != edge.microbatch ||
            manifest.sourceLayoutDigest != edge.collectiveSourceLayoutDigest ||
            manifest.targetLayoutDigest != edge.collectiveTargetLayoutDigest ||
            manifest.tensorId != (edge.tensors.empty() ? edge.scope : edge.tensors.front()) ||
            manifest.tensorDigest != edge.tensorDigest ||
            manifest.segmentCount == 0 ||
            manifest.segmentCount > edge.maxSegments ||
            manifest.segmentCount > operation.maxSegments ||
            manifest.totalBytes > operation.maxBytes ||
            manifest.totalBytes > capability.maxInflightBytes ||
            manifest.unpackedBytes > operation.maxBytes ||
            manifest.unpackedBytes > capability.maxInflightBytes ||
            manifest.segmentSize > ndn::MAX_NDN_PACKET_SIZE ||
            manifest.endpointDigest != edge.endpointDigest ||
            manifest.manifestContractDigest != edge.manifestContractDigest ||
            manifest.noProgressMs != edge.noProgressDeadlineMs ||
            manifest.hardDeadlineMs != edge.hardDeadlineMs ||
            (!manifest.compactContextRequired &&
             !m_groupCoordinator->verifyTensorObjectManifest(
               manifest.signingBytes(), manifest.producerSignature))) {
          throw std::runtime_error(
            "TensorObjectManifestV1 does not match mustFetch authority");
        }

        detail::logNativeWorkPhase(edge.requestId, "tensor_manifest_decode_validate",
                                  manifestDecodeTiming, edge.consumerRole);

        std::vector<std::pair<ndn::Name, std::vector<std::uint8_t>>>
          fetchedSegments;
        fetchedSegments.reserve(manifest.segmentCount);
        std::uint64_t fetchedBytes = 0;
        std::vector<std::string> ciphertextDigests;
        ciphertextDigests.reserve(manifest.segmentCount);
        const auto segmentsStartedAt = std::chrono::steady_clock::now();
        const auto segmentsCollectTiming = detail::nativePhaseStarted();
        std::vector<ndn::Buffer> batchWires;
        std::optional<ExactOperation> pendingBatch;
        std::shared_ptr<std::atomic<SteadyClock::duration::rep>> batchProgressDeadline;
        auto batchFetchTiming = detail::nativePhaseStarted();
        std::size_t batchOffset = 0;
        std::size_t maximumBatchSize = 0;
        std::size_t batchCalls = 0;
        if (speculativeWholeStarted) {
          // Hints schedule exact names, not content acceptance. Generation
          // lineage can change the encoded byte count without changing those
          // names. Authenticate and validate every packet against the actual
          // manifest below instead of discarding a useful same-count batch.
          const bool matches = manifest.segmentCount == prefetchCount;
          if (matches) {
            while (speculativeWhole->wait_for(std::chrono::milliseconds(5)) !=
                   std::future_status::ready) {
              if (cancelled->load(std::memory_order_acquire) ||
                  m_groupCoordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0 ||
                  speculativeExpired())
                throw std::runtime_error("NDNSF_DATA_V1 speculative batch fetch aborted");
            }
            auto result = speculativeWhole->get();
            const auto expectedContents = manifest.segmentCount -
              (edge.inlineFirstSegment ? 1U : 0U);
            if (result && result->contents.size() == expectedContents) {
              mergeTransferMetrics(transportMetrics, result->metrics);
              batchWires = std::move(result->contents);
              if (edge.inlineFirstSegment) {
                // Plain-manifest fallback remains compatible with this opt-in.
                auto first = inlineFirstReceived ? std::move(inlineFirstWire)
                  : fetchExact(exactTensorSegmentName(edge, 0), false);
                batchWires.insert(batchWires.begin(), ndn::Buffer(first.begin(), first.end()));
              }
              speculativeWholeUsed = true;
              maximumBatchSize = batchWires.size();
              batchCalls = 1;
            }
          }
          if (!speculativeWholeUsed) {
            speculativeWhole->cancel();
            speculativeWholeFallback = true;
          }
          speculativeWhole.reset();
        }
        auto fetchBatch = [&](std::size_t firstIndex, bool reserveFirst) {
          const auto remaining = remainingDeadlineMs(hardDeadline);
          if (remaining == 0) {
            throw std::runtime_error("NDNSF_DATA_V1 hard deadline expired");
          }
          const auto availableBytes = capability.maxInflightBytes - fetchedBytes;
          const auto reservedBytes = reserveFirst
            ? static_cast<std::uint64_t>(ndn::MAX_NDN_PACKET_SIZE) : 0;
          const auto batchBytes = availableBytes > reservedBytes
            ? availableBytes - reservedBytes : availableBytes;
          const auto count = std::min<std::uint64_t>(
            manifest.segmentCount - firstIndex,
            std::max<std::uint64_t>(1, batchBytes / ndn::MAX_NDN_PACKET_SIZE));
          std::vector<ndn::Name> names;
          names.reserve(count);
          for (std::size_t offset = 0; offset < count; ++offset) {
            names.push_back(exactTensorSegmentName(edge, firstIndex + offset));
          }
          batchProgressDeadline = std::make_shared<std::atomic<SteadyClock::duration::rep>>(
            std::min(hardDeadline, SteadyClock::now() +
              std::chrono::milliseconds(edge.noProgressDeadlineMs))
                .time_since_epoch().count());
          const auto noProgressMs = edge.noProgressDeadlineMs;
          const auto progressDeadline = batchProgressDeadline;
          batchFetchTiming = detail::nativePhaseStarted();
          pendingBatch.emplace(m_ctx.startSignedExactDataBatch(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            names, ndn::Name(producerMember->provider),
            std::max(1, static_cast<int>(std::min<std::uint64_t>(
              remaining, static_cast<std::uint64_t>(m_fetchTimeoutMs)))),
            [cancelled, batchCancelled, coordinator = m_groupCoordinator,
             hardDeadline, progressDeadline] {
              return cancelled->load(std::memory_order_acquire) ||
                batchCancelled->load(std::memory_order_acquire) ||
                coordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0 ||
                SteadyClock::now().time_since_epoch().count() >=
                  progressDeadline->load(std::memory_order_acquire);
            }, reportFirstRead,
            ndn_service_framework::ServiceProvider::ExactDataFetchPhase::Published,
            [coordinator = m_groupCoordinator, progressDeadline, hardDeadline, noProgressMs] {
              if (remainingDeadlineMs(hardDeadline) == 0 ||
                  !coordinator->recordProgress(nowEpochMs())) return false;
              progressDeadline->store(std::min(hardDeadline, SteadyClock::now() +
                std::chrono::milliseconds(noProgressMs)).time_since_epoch().count(),
                std::memory_order_release);
              return true;
            }, MAX_OUTSTANDING_PUBLISHED_SEGMENTS));
          batchWires.clear();
          batchOffset = 0;
          maximumBatchSize = std::max(maximumBatchSize,
            std::min<std::size_t>(names.size(), MAX_OUTSTANDING_PUBLISHED_SEGMENTS));
          ++batchCalls;
        };
        // Queue the rolling window before waiting for segment0.
        // Core owns the asynchronous handlers; this edge already has a worker.
        const auto firstBatchIndex = (speculativeFirstStarted ||
          (!speculativeWholeUsed && inlineFirstReceived)) ? 1U : 0U;
        if (!speculativeWholeUsed && firstBatchIndex < manifest.segmentCount) {
          fetchBatch(firstBatchIndex, speculativeFirstStarted || inlineFirstReceived);
        }
        for (std::size_t index = 0; index < manifest.segmentCount; ++index) {
          if (cancelled->load(std::memory_order_acquire) ||
              m_groupCoordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0) {
            throw std::runtime_error("NDNSF_DATA_V1 segment fetch aborted");
          }
          SegmentFetchResult fetched;
          if (index == 0 && !speculativeWholeUsed && inlineFirstReceived) {
            fetched.wire = std::move(inlineFirstWire);
            fetched.received = true;
          }
          else if (index == 0 && speculativeFirstStarted) {
            while (speculativeFirst->wait_for(std::chrono::milliseconds(5)) !=
                     std::future_status::ready) {
              if (cancelled->load(std::memory_order_acquire) ||
                  m_groupCoordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0 ||
                  speculativeExpired()) {
                throw std::runtime_error("NDNSF_DATA_V1 segment fetch aborted");
              }
            }
            auto firstResult = speculativeFirst->get();
            if (firstResult) {
              fetched.wire = std::move(firstResult->contents.front());
              fetched.metrics = std::move(firstResult->metrics);
              fetched.received = true;
              if (fetched.wire.size() > ndn::MAX_NDN_PACKET_SIZE) {
                throw std::runtime_error("speculative first segment exceeds packet budget");
              }
            }
            else {
              fetched.failure = std::make_exception_ptr(
                std::runtime_error("failed speculative exact first segment"));
            }
            if (speculativeExpired()) {
              throw std::runtime_error("NDNSF_DATA_V1 speculative window expired");
            }
            if (!fetched.received) {
              mergeTransferMetrics(transportMetrics, fetched.metrics);
              if (cancelled->load(std::memory_order_acquire) ||
                  m_groupCoordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0 ||
                  speculativeExpired()) {
                if (fetched.failure) std::rethrow_exception(fetched.failure);
                throw std::runtime_error("NDNSF_DATA_V1 speculative fetch aborted");
              }
              speculativeFirstFallback = true;
              if (dependencyObjectTraceEnabled()) {
                std::ostringstream miss;
                miss << "NDNSF_DI_DEPENDENCY_FETCH_PHASE session=" << sessionId
                     << " scope=" << edge.scope
                     << " phase=speculative_first_miss fallback=bounded-normal-fetch";
                logRuntimeEvidence(miss.str());
              }
              fetched = fetchSegment(exactTensorSegmentName(edge, 0), true);
              if (!fetched.received) {
                mergeTransferMetrics(transportMetrics, fetched.metrics);
                if (fetched.failure) std::rethrow_exception(fetched.failure);
                throw std::runtime_error("failed to fetch signed exact first segment");
              }
            }
            else {
              speculativeFirstUsed = true;
            }
          }
          else {
            if (batchOffset == batchWires.size()) {
              if (!pendingBatch || !batchWires.empty()) fetchBatch(index, false);
              while (pendingBatch->wait_for(std::chrono::milliseconds(5)) !=
                       std::future_status::ready) {
                if (cancelled->load(std::memory_order_acquire) ||
                    m_groupCoordinator->terminal() || remainingDeadlineMs(hardDeadline) == 0 ||
                    SteadyClock::now().time_since_epoch().count() >=
                      batchProgressDeadline->load(std::memory_order_acquire)) {
                  throw std::runtime_error("NDNSF_DATA_V1 batch fetch aborted");
                }
              }
              auto batchResult = pendingBatch->get();
              if (!batchResult) throw std::runtime_error("failed signed exact Data batch");
              mergeTransferMetrics(transportMetrics, batchResult->metrics);
              batchWires = std::move(batchResult->contents);
              batchOffset = 0;
              detail::logNativeWorkPhase(edge.requestId, "tensor_segment_batch_fetch_wait",
                                        batchFetchTiming, edge.consumerRole);
            }
            fetched.wire = std::move(batchWires[batchOffset++]);
            fetched.received = true;
          }
          mergeTransferMetrics(transportMetrics, fetched.metrics);
          if (!m_groupCoordinator->recordProgress(nowEpochMs())) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 no-progress/hard deadline or cancellation");
          }
          const auto dataName = exactTensorSegmentName(edge, index);
          auto wire = std::move(fetched.wire);
          if (wire.size() > capability.maxInflightBytes - fetchedBytes) {
            throw std::runtime_error("exact tensor ciphertext exceeds inflight bound");
          }
          fetchedBytes += wire.size();
          const auto ciphertextDigest = sha256TensorBytes(wire);
          if (!manifest.compactContextRequired) {
            if (ciphertextDigest != manifest.orderedSegmentDigests[index]) {
              throw std::runtime_error(
                "TensorObjectManifestV1 ciphertext segment digest mismatch");
            }
          }
          else {
            ciphertextDigests.push_back(ciphertextDigest);
          }
          fetchedSegments.emplace_back(dataName, std::move(wire));
        }
        detail::logNativeWorkPhase(edge.requestId, "tensor_segments_collect",
                                  segmentsCollectTiming, edge.consumerRole);
        if (dependencyObjectTraceEnabled()) {
          const auto segmentsFetchedAt = std::chrono::steady_clock::now();
          std::ostringstream record;
          record << "NDNSF_DI_DEPENDENCY_FETCH_PHASE"
                 << " session=" << sessionId << " scope=" << edge.scope
                 << " phase=exact_segments segment_count=" << manifest.segmentCount
                 << " max_concurrent=" << maximumBatchSize
                 << " batch_calls=" << batchCalls
                 << " speculative_first_started=" << speculativeFirstStarted
                 << " speculative_first_used=" << speculativeFirstUsed
                 << " speculative_first_fallback=" << speculativeFirstFallback
                 << " speculative_prefetch_count=" << prefetchCount
                 << " speculative_prefetch_started=" << speculativeWholeStarted
                 << " speculative_prefetch_used=" << speculativeWholeUsed
                 << " speculative_prefetch_fallback=" << speculativeWholeFallback
                 << " prefetch_bundle_bytes=" << edge.prefetchBundleBytes
                 << " prefetch_segment_bytes=" << edge.prefetchSegmentBytes
                 << " manifest_total_bytes=" << manifest.totalBytes
                 << " inline_first_received=" << inlineFirstReceived
                 << " manifest_segment_bytes=" << manifest.segmentSize
                 << " manifest_fetch_us=" << std::chrono::duration_cast<
                      std::chrono::microseconds>(manifestFetchedAt - started).count()
                 << " segment_fetch_us=" << std::chrono::duration_cast<
                      std::chrono::microseconds>(segmentsFetchedAt - segmentsStartedAt).count();
          logRuntimeEvidence(record.str());
        }
        const auto manifestCommitmentTiming = detail::nativePhaseStarted();
        if (manifest.compactContextRequired) {
          std::vector<std::uint8_t> digestCommitmentInput;
          for (const auto& digest : ciphertextDigests) {
            digestCommitmentInput.insert(
              digestCommitmentInput.end(), digest.begin(), digest.end());
          }
          if (sha256TensorBytes(digestCommitmentInput) !=
                manifest.compactSegmentDigestCommitment) {
            throw std::runtime_error(
              "TensorObjectManifestV1 ciphertext commitment mismatch");
          }
          manifest.orderedSegmentDigests = std::move(ciphertextDigests);
          manifest.compactContextRequired = false;
          manifest.objectManifestDigest = manifest.digest();
          manifest.validate();
          if (!m_groupCoordinator->verifyTensorObjectManifest(
                manifest.signingBytes(), manifest.producerSignature)) {
            throw std::runtime_error(
              "TensorObjectManifestV1 producer signature mismatch");
          }
        }
        detail::logNativeWorkPhase(edge.requestId, "tensor_manifest_commitment_validate",
                                  manifestCommitmentTiming, edge.consumerRole);

        const auto segmentsDecodeTiming = detail::nativePhaseStarted();
        std::map<std::uint64_t, std::vector<std::uint8_t>> plaintextBySegment;
        for (std::size_t segmentIndex = 0;
             segmentIndex < fetchedSegments.size(); ++segmentIndex) {
          const auto& fetched = fetchedSegments[segmentIndex];
          const auto& dataName = fetched.first;
          const auto& wire = fetched.second;
          // The outer signed TensorObject manifest is the authority for the
          // complete ciphertext object. Compact inner segments therefore
          // carry only the operation digest and crypto envelope; restore the
          // non-wire edge bindings before passing them to the coordinator.
          auto decoded = ProviderGroupCoordinator::decodeSegment(
            wire, dataName.toUri(ndn::name::UriFormat::CANONICAL));
          if (decoded.segments.size() != 1) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 segment inner manifest mismatch");
          }
          auto& decodedManifest = decoded.manifest;
          auto& decodedSegment = decoded.segments.front();
          const auto expectedBytes = segmentIndex + 1 == manifest.segmentCount
            ? manifest.totalBytes - segmentIndex * manifest.segmentSize
            : manifest.segmentSize;
          if (decodedSegment.descriptor.segmentNo != segmentIndex ||
              decodedSegment.descriptor.producerRank != producerRank ||
              decodedSegment.ciphertext.size() != expectedBytes) {
            throw std::runtime_error("exact tensor segment index/rank/size mismatch");
          }
          if (decodedManifest.externalSegmentDigests) {
            decodedManifest.capabilityDigest = capability.capabilityDigest;
            decodedManifest.epochKeyId = capability.epochKeyId;
            decodedManifest.requestId = capability.requestId;
            decodedManifest.attemptId = capability.attemptId;
            decodedManifest.planDigest = capability.planDigest;
            decodedManifest.groupId = capability.groupId;
            decodedManifest.epoch = capability.epoch;
            decodedManifest.operationIndex = edge.collectiveOperationIndex;
            decodedManifest.producerRank = producerRank;
            decodedManifest.operationKind = edge.operationKind;
            decodedManifest.segmentCount = manifest.segmentCount;
            decodedManifest.totalBytes = manifest.totalBytes;
            decodedManifest.segmentSize = manifest.segmentSize;
            decodedManifest.sourceLayoutDigest =
              edge.collectiveSourceLayoutDigest;
            decodedManifest.targetLayoutDigest =
              edge.collectiveTargetLayoutDigest;
            decodedManifest.tensorDigest = edge.tensorDigest;
            decodedManifest.createdAtMs = manifest.createdAtMs;
            // sealOperation authenticates the inner segment with capability
            // budgets. The Selection edge separately limits this fetch and
            // may have a tighter deadline; it cannot rewrite authenticated AAD.
            decodedManifest.noProgressMs = capability.noProgressMs;
            decodedManifest.hardDeadlineMs = capability.hardDeadlineMs;
            decodedSegment.descriptor.requestId = capability.requestId;
            decodedSegment.descriptor.attemptId = capability.attemptId;
            decodedSegment.descriptor.planDigest = edge.planDigest;
            decodedSegment.descriptor.groupId = capability.groupId;
            decodedSegment.descriptor.epoch = capability.epoch;
            decodedSegment.descriptor.operationKind = edge.operationKind;
            decodedSegment.descriptor.tensorDigest = edge.tensorDigest;
            decodedSegment.descriptor.operationIndex =
              decodedManifest.operationIndex;
            decodedSegment.descriptor.producerRank = decodedManifest.producerRank;
            decodedSegment.descriptor.segmentCount = decodedManifest.segmentCount;
            decodedSegment.descriptor.totalBytes = decodedManifest.totalBytes;
            decodedSegment.descriptor.segmentSize = decodedManifest.segmentSize;
            decodedSegment.descriptor.noProgressMs = decodedManifest.noProgressMs;
            decodedSegment.descriptor.hardDeadlineMs = decodedManifest.hardDeadlineMs;
          }
          if (decodedManifest.requestId != capability.requestId ||
              decodedManifest.attemptId != capability.attemptId ||
              decodedManifest.planDigest != capability.planDigest ||
              decodedManifest.groupId != capability.groupId ||
              decodedManifest.epoch != capability.epoch ||
              decodedManifest.operationIndex != edge.collectiveOperationIndex ||
              decodedManifest.producerRank != producerRank ||
              decodedManifest.sourceLayoutDigest !=
                edge.collectiveSourceLayoutDigest ||
              decodedManifest.targetLayoutDigest !=
                edge.collectiveTargetLayoutDigest ||
              decodedManifest.tensorDigest != edge.tensorDigest ||
              decodedManifest.segmentCount != manifest.segmentCount ||
              decodedManifest.totalBytes != manifest.totalBytes ||
              (decodedManifest.externalSegmentDigests &&
               decodedManifest.transportManifestDigest !=
                 decodedSegment.descriptor.manifestDigest)) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 segment inner manifest mismatch");
          }
          const auto& segment = decodedSegment;
          auto accepted = m_groupCoordinator->acceptAndOpenSegment(
            decoded.manifest, segment, dataName.toUri(ndn::name::UriFormat::CANONICAL));
          // Identical duplicates remain readable/idempotent in the exact path.
          plaintextBySegment.emplace(
            segment.descriptor.segmentNo,
            std::move(accepted.second));
        }
        if (plaintextBySegment.size() != manifest.segmentCount) {
          throw std::runtime_error(
            "NDNSF_DATA_V1 exact tensor object is incomplete");
        }
        detail::logNativeWorkPhase(edge.requestId, "tensor_segments_decode_decrypt",
                                  segmentsDecodeTiming, edge.consumerRole);
        const auto reconstructTiming = detail::nativePhaseStarted();
        TensorBundle bundle;
        bundle.name = edge.tensors.size() == 1 ?
          edge.tensors.front() : edge.plannedDataName;
        std::size_t plaintextBytes = 0;
        for (const auto& item : plaintextBySegment) {
          if (item.second.size() > bundle.payload.max_size() - plaintextBytes ||
              item.second.size() > manifest.totalBytes - plaintextBytes) {
            throw std::runtime_error(
              "TensorObjectManifestV1 reconstructed content mismatch");
          }
          plaintextBytes += item.second.size();
        }
        if (plaintextBytes != manifest.totalBytes) {
          throw std::runtime_error(
            "TensorObjectManifestV1 reconstructed content mismatch");
        }
        // Reserve from accepted plaintext, not an unchecked manifest length.
        bundle.payload.reserve(plaintextBytes);
        for (const auto& item : plaintextBySegment) {
          bundle.payload.insert(bundle.payload.end(),
                                item.second.begin(), item.second.end());
        }
        if (bundle.payload.size() != manifest.totalBytes ||
            sha256TensorBytes(bundle.payload) != manifest.contentDigest) {
          throw std::runtime_error(
            "TensorObjectManifestV1 reconstructed content mismatch");
        }
        // Authenticate the complete encoded object before allocating its
        // bounded expansion. Local consumers retain the original tensor wire.
        if (manifest.unpackedBytes != 0) {
          bundle.payload = unpackTensorBundleFromTransport(bundle.payload, manifest.unpackedBytes);
          if (bundle.payload.size() != manifest.unpackedBytes ||
              !isEncodedTensorBundle(bundle.payload)) {
            throw std::invalid_argument("packed tensor bundle does not match signed expansion");
          }
        }
        bundle.expectedSegments = manifest.segmentCount;
        bundle.expectedBytes = bundle.payload.size();
        attachTransferObservation(edge, bundle, "receive", transportMetrics);
        validateCanonicalYoloMergeInput(edge, bundle);
        detail::logNativeWorkPhase(edge.requestId, "tensor_reconstruct_validate",
                                  reconstructTiming, edge.consumerRole);
        logDependencyObject(sessionId, edge, "fetch-exact-ndn",
                            bundle.payload.size(), "ok", manifest.totalBytes);
        logDependencyStageMarker("complete", sessionId, edge,
                                 m_ctx.localProvider().toUri());
        return bundle;
      }
      throw std::invalid_argument("C04 tensor endpoint is required for network dependency fetch");
    }
    if (const auto* trace = std::getenv("NDNSF_DI_RUNTIME_TIMING");
        trace != nullptr && *trace != '\0' && *trace != '0') {
      std::ostringstream record;
      record << "NDNSF_DI_DEPENDENCY_FETCH_BEGIN session=" << sessionId
             << " scope=" << edge.scope
             << " key_scope=" << edge.transportScope
             << " planned_name=" << edge.plannedDataName
             << " use_data_v1=" << (edge.useNdnsfDataV1 ? 1 : 0);
      logRuntimeEvidence(record.str());
    }
    auto payload = m_ctx.fetchLarge(
      ndn::Name(edge.plannedDataName),
      edge.transportScope.empty() ? edge.scope : edge.transportScope,
      m_fetchTimeoutMs,
      edge.expectedSegments,
      &transportMetrics);
    if (!payload) {
      throw std::runtime_error(
        "failed to fetch planned dependency object: " +
        edge.plannedDataName);
    }
    TensorBundle bundle;
    bundle.name = edge.tensors.size() == 1 ? edge.tensors.front() : edge.plannedDataName;
    bundle.payload.assign(payload->data(), payload->data() + payload->size());
    bundle.expectedSegments = edge.expectedSegments;
    bundle.expectedBytes = edge.expectedBytes;
    attachTransferObservation(edge, bundle, "receive", transportMetrics);
    validateCanonicalYoloMergeInput(edge, bundle);
    logDependencyObject(sessionId, edge, "fetch", bundle.payload.size(), "ok");
    logDependencyStageMarker("complete", sessionId, edge,
                             m_ctx.localProvider().toUri());
    return bundle;
   }
   catch (...) {
     try {
       logDependencyStageMarker("failed", sessionId, edge,
                                m_ctx.localProvider().toUri());
     }
     catch (...) {
       // Preserve the original transport/validation exception.
     }
     reportFirstRead(std::current_exception());
     throw;
   }
  };
  if (!m_prefetchWorkers) {
    auto result = std::async(std::launch::async, std::move(fetch));
    return {std::move(result), std::move(firstReadStarted)};
  }
  auto resultPromise = std::make_shared<std::promise<TensorBundle>>();
  auto result = resultPromise->get_future();
  try {
    const bool accepted = m_prefetchWorkers->post(
      [fetch = std::move(fetch), resultPromise, reportFirstRead] () mutable {
        try {
          resultPromise->set_value(fetch());
        }
        catch (...) {
          const auto error = std::current_exception();
          reportFirstRead(error);
          resultPromise->set_exception(error);
        }
      });
    if (!accepted) {
      throw std::runtime_error("dependency prefetch worker queue is full or stopped");
    }
  }
  catch (...) {
    const auto error = std::current_exception();
    reportFirstRead(error);
    resultPromise->set_exception(error);
  }
  return {std::move(result), std::move(firstReadStarted)};
}

bool
NdnsfCollaborationDependencyIo::supportsPromptPrefetchCancellation(
  const DependencyEdge& edge) const noexcept
{
  // DataV1 exact fetches accept cancellation polling and local segment waits
  // are condition-variable-backed. Legacy fetchLarge has no cancel contract.
  return edge.useNdnsfDataV1;
}

void
NdnsfCollaborationDependencyIo::cancelPendingPrefetches() noexcept
{
  m_pendingPrefetchCancelled->store(true, std::memory_order_release);
  // Existing exact-fetch cancellation polling owns the Face handles.
}

void
NdnsfCollaborationDependencyIo::drainPendingPrefetches() noexcept
{
  if (m_prefetchWorkers) {
    m_prefetchWorkers->shutdown();
  }
}

void
NdnsfCollaborationDependencyIo::publishOutput(const std::string& sessionId,
                                              const DependencyEdge& edge,
                                              const TensorBundle& bundle)
{
  if (edge.useNdnsfDataV1 && !edge.declaredByV3)
    throw std::invalid_argument("C04 tensor endpoint is required for network publication");
  if (m_protectedRuntime) {
    m_protectedRuntime->authorizeDataflow(
      ProtectedDataflowDirection::Publish, edge.endpointDigest,
      edge.producerRole, edge.consumerRole, nowEpochMs());
  }
  if (edge.useNdnsfDataV1) {
    if (!m_groupCoordinator || !m_groupCoordinator->hasCapability()) {
      throw std::runtime_error(
        "NDNSF_DATA_V1 dependency requires an active group capability");
    }
    const auto& capability = m_groupCoordinator->capability();
    const auto& operation = operationForEdge(
      capability, edge.collectiveOperationIndex);
    if (operation.kind != edge.operationKind)
      throw std::invalid_argument("tensor operation differs from the signed capability");
    const auto producerRank = producerRankForEdge(capability, edge);
    const auto* producerMember = memberForRank(capability, producerRank);
    if (producerMember == nullptr || ndn::Name(producerMember->provider) != m_ctx.localProvider())
      throw std::invalid_argument("tensor Producer is not the local Provider");
    validateTensorReference(edge, *producerMember);
    if (!m_groupCoordinator->beginOperation(nowEpochMs())) {
      throw std::runtime_error(
        "NDNSF_DATA_V1 group cannot begin publication operation");
    }
    constexpr std::size_t MAX_ORIGINAL_BYTES = 64U * 1024U * 1024U;
    if (edge.declaredByV3 && bundle.payload.size() > MAX_ORIGINAL_BYTES)
      throw std::length_error("tensor publication exceeds original cache capacity");
    if (bundle.payload.size() > operation.maxBytes ||
        bundle.payload.size() > capability.maxInflightBytes) {
      throw std::length_error("tensor publication exceeds signed raw byte budget");
    }
    const auto packed = tryPackTensorBundleForTransport(bundle.payload);
    const auto& packedPayload = packed ? *packed : bundle.payload;
    const auto unpackedBytes = packed ? bundle.payload.size() : 0;
    const auto chunkSize = std::max<std::size_t>(1, m_maxSegmentSize);
    std::vector<std::vector<std::uint8_t>> chunks;
    for (std::size_t offset = 0; offset < packedPayload.size(); offset += chunkSize) {
      const auto count = std::min(chunkSize, packedPayload.size() - offset);
      chunks.emplace_back(packedPayload.begin() + static_cast<std::ptrdiff_t>(offset),
                          packedPayload.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    if (chunks.empty()) {
      throw std::invalid_argument("NDNSF_DATA_V1 cannot publish an empty tensor bundle");
    }
    if (edge.declaredByV3) {
      if (edge.manifestDataName.empty() || edge.maxSegments == 0 ||
          chunks.size() > edge.maxSegments || edge.endpointDigest.empty() ||
          edge.planDigest.empty() || edge.manifestContractDigest.empty() ||
          edge.securityProfile != "NDNSF_DATA_V1" ||
          edge.noProgressDeadlineMs == 0 || edge.hardDeadlineMs == 0) {
        throw std::invalid_argument(
          "V3 mayPublish tensor endpoint is incomplete");
      }
      if (producerMember == nullptr ||
          ndn::Name(producerMember->provider) != m_ctx.localProvider()) {
        throw std::runtime_error(
          "V3 mayPublish endpoint is not owned by the local Provider");
      }
      constexpr std::size_t MAX_ORIGINAL_OBJECTS = 8192;
      if (bundle.payload.size() > MAX_ORIGINAL_BYTES)
        throw std::length_error("tensor publication exceeds original cache capacity");
      const auto fingerprint = nativeCanonicalJson(NativeJson{
        {"endpoint", edge.endpointDigest}, {"object", edge.executionObjectDigest},
        {"capability", capability.capabilityDigest}, {"epoch_key", capability.epochKeyId},
        {"requester", m_ctx.requesterName().toUri(ndn::name::UriFormat::CANONICAL)},
        {"manifest", ndn::Name(edge.manifestDataName).toUri(ndn::name::UriFormat::CANONICAL)},
        {"scope", edge.transportScope.empty() ? edge.scope : edge.transportScope},
        {"chunk_bytes", chunkSize}, {"freshness_ms", m_freshnessMs},
        {"inline", edge.inlineFirstSegment}, {"operation_index", edge.collectiveOperationIndex},
        {"source_layout", edge.collectiveSourceLayoutDigest},
        {"target_layout", edge.collectiveTargetLayoutDigest},
        {"tensor", edge.tensorDigest}, {"max_segments", edge.maxSegments},
        {"request_id", edge.requestId}, {"attempt", edge.attemptEpoch},
        {"plan", edge.planDigest}, {"manifest_contract", edge.manifestContractDigest},
        {"security_profile", edge.securityProfile}, {"operation_kind", edge.operationKind},
        {"round", edge.round}, {"naming_stride", edge.namingOperationStride},
        {"microbatch", edge.microbatch}, {"producer_role", edge.producerRole},
        {"producer_rank", producerRank},
        {"consumer_roles", edge.consumerRoles.empty() ?
          std::vector<std::string>{edge.consumerRole} : edge.consumerRoles}, {"tensors", edge.tensors},
        {"bundle_tensor_names", edge.bundleTensorNames},
        {"no_progress_ms", edge.noProgressDeadlineMs}, {"hard_deadline_ms", edge.hardDeadlineMs},
        {"bundle_name", bundle.name}, {"payload_digest", sha256TensorBytes(bundle.payload)},
        {"unpacked_bytes", unpackedBytes},
        {"expected_bytes", bundle.expectedBytes}, {"expected_segments", bundle.expectedSegments}});
      std::unique_lock<std::mutex> originalLock(m_tensorPublicationMutex);
      const ndn::Name objectName(edge.plannedDataName);
      auto cached = m_tensorPublications.find(objectName);
      if (cached != m_tensorPublications.end()) {
        if (cached->second.fingerprint != fingerprint)
          throw std::invalid_argument("immutable tensor publication conflict");
        if (!cached->second.originals.empty()) {
          CollaborationTransferMetrics retryMetrics;
          if (!m_ctx.replayOwnedExactData(edge.transportScope.empty() ? edge.scope : edge.transportScope,
                                         cached->second.originals, &retryMetrics))
            throw std::runtime_error("failed to replay signed tensor originals");
          attachTransferObservation(edge, bundle, "send", retryMetrics);
          if (!m_groupCoordinator->recordProgress(nowEpochMs()))
            throw std::runtime_error("tensor retry group deadline or cancellation");
          return;
        }
      }
      else {
        if (m_tensorPublications.size() >= MAX_ORIGINAL_OBJECTS)
          throw std::length_error("tensor original cache entry capacity");
        if (m_tensorPublicationBytes >= MAX_ORIGINAL_BYTES)
          throw std::length_error("tensor original cache byte capacity");
        cached = m_tensorPublications.emplace(objectName, CachedTensorPublication{fingerprint, {}, 0}).first;
      }
      auto& cachedPublication = cached->second;
      const auto accountOriginals = [&] {
        if (cachedPublication.wireBytes == 0 && !cachedPublication.originals.empty()) {
          for (const auto& packet : cachedPublication.originals)
            cachedPublication.wireBytes += packet->wireEncode().size();
          m_tensorPublicationBytes += cachedPublication.wireBytes;
        }
      };
      const auto reservation = ndn::make_scope_exit([&] {
        if (cachedPublication.originals.empty()) m_tensorPublications.erase(cached);
        else accountOriginals();
      });
      std::vector<std::string> exactNames;
      exactNames.reserve(chunks.size());
      for (std::size_t index = 0; index < chunks.size(); ++index) {
        exactNames.push_back(exactTensorSegmentName(edge, index).toUri(ndn::name::UriFormat::CANONICAL));
      }
      const auto createdAtMs = nowEpochMs();
      const auto sealed = m_groupCoordinator->sealOperation(
        operation,
        producerRank,
        edge.collectiveSourceLayoutDigest,
        edge.collectiveTargetLayoutDigest,
        edge.tensorDigest,
        chunks,
        createdAtMs,
        exactNames);

      std::vector<std::vector<std::uint8_t>> encodedSegments;
      encodedSegments.reserve(sealed.segments.size());
      for (const auto& segment : sealed.segments) {
        encodedSegments.push_back(ProviderGroupCoordinator::encodeSegmentCompact(
          sealed.manifest, segment));
      }

      std::uint64_t numericProducerRank = 0;
      try {
        numericProducerRank = std::stoull(producerRank);
      }
      catch (const std::exception&) {
        throw std::runtime_error("V3 producer rank is not numeric");
      }
      TensorObjectManifestV1 manifest;
      manifest.capabilityDigest = canonicalSha256(capability.capabilityDigest);
      manifest.epochKeyId = capability.epochKeyId;
      manifest.requester = m_ctx.requesterName().toUri(ndn::name::UriFormat::CANONICAL);
      manifest.requestId = edge.requestId;
      manifest.attemptId = std::to_string(edge.attemptEpoch);
      manifest.planDigest = edge.planDigest;
      manifest.groupId = capability.groupId;
      manifest.epoch = std::to_string(capability.epoch);
      manifest.operationIndex = edge.collectiveOperationIndex;
      manifest.round = edge.round;
      manifest.operationKind = edge.operationKind;
      manifest.producerRole = edge.producerRole;
      manifest.producerRank = numericProducerRank;
      manifest.consumerRoles = edge.consumerRoles.empty() ?
        std::vector<std::string>{edge.consumerRole} : edge.consumerRoles;
      manifest.microbatch = edge.microbatch;
      manifest.sourceLayoutDigest = edge.collectiveSourceLayoutDigest;
      manifest.targetLayoutDigest = edge.collectiveTargetLayoutDigest;
      manifest.tensorId = edge.tensors.empty() ? edge.scope : edge.tensors.front();
      manifest.tensorDigest = edge.tensorDigest;
      manifest.contentDigest = sha256TensorBytes(packedPayload);
      manifest.totalBytes = packedPayload.size();
      manifest.unpackedBytes = unpackedBytes;
      manifest.segmentSize = 0;
      manifest.segmentCount = encodedSegments.size();
      for (std::size_t index = 0; index < encodedSegments.size(); ++index) {
        manifest.segmentSize = std::max<std::uint64_t>(
          manifest.segmentSize, chunks[index].size());
        manifest.orderedSegmentDigests.push_back(
          sha256TensorBytes(encodedSegments[index]));
      }
      manifest.createdAtMs = createdAtMs;
      manifest.noProgressMs = edge.noProgressDeadlineMs;
      manifest.hardDeadlineMs = edge.hardDeadlineMs;
      manifest.endpointDigest = edge.endpointDigest;
      manifest.manifestContractDigest = edge.manifestContractDigest;
      manifest.producerSignature =
        m_groupCoordinator->signTensorObjectManifest(manifest.signingBytes());
      manifest.objectManifestDigest = manifest.digest();
      const auto manifestWire = encodeTensorObjectManifestContextCompact(manifest);

      std::vector<std::pair<ndn::Name, ndn::Buffer>> publications;
      publications.reserve(encodedSegments.size() + 1);
      publications.emplace_back(
        ndn::Name(edge.manifestDataName),
        ndn::Buffer(manifestWire.begin(), manifestWire.end()));
      for (std::size_t index = 0; index < encodedSegments.size(); ++index) {
        publications.emplace_back(
          ndn::Name(exactNames[index]),
          ndn::Buffer(encodedSegments[index].begin(),
                      encodedSegments[index].end()));
      }
      CollaborationTransferMetrics transportMetrics;
      bool published = false;
      if (edge.inlineFirstSegment) {
        try {
          auto inlineWire = encodeTensorManifestWithFirstSegment(
            manifestWire, encodedSegments.front(), ndn::MAX_NDN_PACKET_SIZE);
          auto inlinePublications = publications;
          inlinePublications.front().second = ndn::Buffer(
            inlineWire.begin(), inlineWire.end());
          inlinePublications.erase(inlinePublications.begin() + 1);
          // Core signs and bounds the complete batch before inserting any Data.
          // An oversized envelope therefore leaves no conflicting publication.
          published = m_ctx.publishOwnedExactData(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            inlinePublications, m_freshnessMs, &transportMetrics,
            cachedPublication.originals, MAX_ORIGINAL_BYTES - m_tensorPublicationBytes);
        }
        catch (const std::invalid_argument&) {
          accountOriginals();
          if (!cachedPublication.originals.empty()) throw;
          // Only a pre-publication framing failure permits plain fallback.
        }
        catch (...) { accountOriginals(); throw; }
      }
      try {
        if (!published && !m_ctx.publishOwnedExactData(
              edge.transportScope.empty() ? edge.scope : edge.transportScope,
              publications, m_freshnessMs, &transportMetrics,
              cachedPublication.originals, MAX_ORIGINAL_BYTES - m_tensorPublicationBytes)) {
          throw std::runtime_error("failed to publish exact signed NDNSF_DATA_V1 tensor object");
        }
      }
      catch (...) { accountOriginals(); throw; }
      accountOriginals();
      attachTransferObservation(edge, bundle, "send", transportMetrics);
      if (!m_groupCoordinator->recordProgress(nowEpochMs())) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 group deadline or cancellation after publication");
      }
      logDependencyObject(sessionId, edge, "publish-exact-ndn",
                          bundle.payload.size(), "ok", packedPayload.size());
      return;
    }
    throw std::invalid_argument("C04 tensor endpoint is required for network publication");
  }
  const ndn::Buffer payload(bundle.payload.data(), bundle.payload.size());
  logDependencyObject(sessionId, edge, "publish", bundle.payload.size(), "ok");
  if (edge.plannedDataName.empty()) {
    CollaborationTransferMetrics transportMetrics;
    const auto publishedName = m_ctx.publishLarge(
      edge.transportScope.empty() ? edge.scope : edge.transportScope,
      edge.topicPrefix.empty() ? ndn::Name("/output") : ndn::Name(edge.topicPrefix),
      payload,
      m_maxSegmentSize,
      m_freshnessMs,
      &transportMetrics);
    if (publishedName.empty()) {
      throw std::runtime_error("failed to publish collaboration large output");
    }
    transportMetrics.actualDataName = publishedName.toUri();
    attachTransferObservation(edge, bundle, "send", transportMetrics);
    return;
  }
  CollaborationTransferMetrics transportMetrics;
  const auto publishedName = m_ctx.publishLargeNamed(
    edge.transportScope.empty() ? edge.scope : edge.transportScope,
    ndn::Name(edge.plannedDataName),
    payload,
    m_maxSegmentSize,
    m_freshnessMs,
    &transportMetrics);
  if (publishedName.empty()) {
    throw std::runtime_error("failed to publish named collaboration large output");
  }
  transportMetrics.actualDataName = publishedName.toUri();
  attachTransferObservation(edge, bundle, "send", transportMetrics);
}

} // namespace ndnsf::di
