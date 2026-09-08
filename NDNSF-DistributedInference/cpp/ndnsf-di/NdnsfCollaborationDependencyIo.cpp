#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

bool
isCanonicalYoloMergeInput(const DependencyEdge& edge)
{
  return edge.consumerRole == "Merge" && edge.tensors.size() == 1 &&
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
  if (tensors.size() != 1 || tensors.front().name != edge.tensors.front()) {
    throw std::runtime_error(
      "YOLO_MERGE_DEPENDENCY_TENSOR_SELECTION_MISMATCH: " + edge.tensors.front());
  }
}

std::optional<ndn::Name>
providerPrefixForRank(const GroupCapabilityV1& capability,
                     const std::string& producerRank)
{
  for (const auto& member : capability.orderedMembers) {
    if (member.provider.empty() || member.endpointPrefix.empty()) {
      continue;
    }
    if (member.provider == producerRank ||
        std::to_string(member.rank) == producerRank) {
      return ndn::Name(member.endpointPrefix);
    }
  }
  return std::nullopt;
}

const GroupMemberV1*
memberForRank(const GroupCapabilityV1& capability,
              const std::string& producerRank)
{
  const auto member = std::find_if(
    capability.orderedMembers.begin(), capability.orderedMembers.end(),
    [&producerRank] (const auto& item) {
      return item.provider == producerRank ||
             std::to_string(item.rank) == producerRank;
    });
  return member == capability.orderedMembers.end() ? nullptr : &*member;
}

std::string
localDataV1Key(const std::string& sessionId,
               std::uint64_t operationIndex,
               const std::string& producerRank,
               const std::string& tensorDigest)
{
  return sessionId + "|" + std::to_string(operationIndex) + "|" +
         producerRank + "|" + tensorDigest;
}

std::string
producerRankForEdge(const GroupCapabilityV1& capability,
                    const DependencyEdge& edge)
{
  if (!edge.producerProvider.empty()) {
    const auto member = std::find_if(
      capability.orderedMembers.begin(), capability.orderedMembers.end(),
      [&edge] (const auto& item) {
        return item.provider == edge.producerProvider;
      });
    if (member != capability.orderedMembers.end()) {
      return std::to_string(member->rank);
    }
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
  return std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr ||
         std::getenv("NDNSF_DI_DEPENDENCY_OBJECT_TRACE") != nullptr;
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
  return ndn::Name(edge.plannedDataName).append("SEG").appendSegment(segment);
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
                    const char* status)
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
         << " planned_name=" << (edge.plannedDataName.empty() ? "none" : edge.plannedDataName)
         << " status=" << status;
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
  OutputPublicationGate outputPublicationGate)
  : m_ctx(ctx)
  , m_fetchTimeoutMs(fetchTimeoutMs)
  , m_maxSegmentSize(maxSegmentSize)
  , m_freshnessMs(freshnessMs)
  , m_groupCoordinator(std::move(groupCoordinator))
  , m_protectedRuntime(std::move(protectedRuntime))
  , m_outputPublicationGate(std::move(outputPublicationGate))
{
}

std::future<TensorBundle>
NdnsfCollaborationDependencyIo::prefetchInput(const std::string& sessionId,
                                              const DependencyEdge& edge)
{
  if (edge.plannedDataName.empty() && !edge.useNdnsfDataV1) {
    throw std::invalid_argument(
      "NdnsfCollaborationDependencyIo requires plannedDataName for input " +
      edge.scope);
  }
  return std::async(std::launch::async, [this, sessionId, edge] {
    if (m_protectedRuntime) {
      m_protectedRuntime->authorizeDataflow(
        ProtectedDataflowDirection::Fetch, edge.endpointDigest,
        edge.producerRole, edge.consumerRole, nowEpochMs());
    }
    if (edge.useNdnsfDataV1) {
      if (!m_groupCoordinator || !m_groupCoordinator->hasCapability()) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 dependency requires an installed group capability");
      }
      const auto& capability = m_groupCoordinator->capability();
      const auto& operation = operationForEdge(
        capability, edge.collectiveOperationIndex);
      const auto producerRank = producerRankForEdge(capability, edge);
      const auto producerPrefix = providerPrefixForRank(
        capability, producerRank);
      if (!producerPrefix) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 producer rank is not present in the capability: " +
          producerRank);
      }
      const auto tensorDigest = edge.collectiveTensorDigest.empty() ?
        edge.scope : edge.collectiveTensorDigest;
      auto expectedSegments = edge.expectedSegments;
      std::optional<std::vector<ndn::Buffer>> encodedSegments;
      const auto* producerMember = memberForRank(capability, producerRank);
      if (edge.declaredByV3) {
        if (producerMember == nullptr || edge.manifestDataName.empty() ||
            edge.maxSegments == 0 || edge.endpointDigest.empty() ||
            edge.manifestContractDigest.empty() ||
            edge.securityProfile != "NDNSF_DATA_V1" ||
            edge.noProgressDeadlineMs == 0 || edge.hardDeadlineMs == 0 ||
            !ndn::Name(producerMember->endpointPrefix)
               .isPrefixOf(ndn::Name(edge.manifestDataName))) {
          throw std::runtime_error(
            "V3 mustFetch endpoint is incomplete or targets the wrong producer");
        }
        const auto started = std::chrono::steady_clock::now();
        const auto hardDeadline = started + std::chrono::milliseconds(
          edge.hardDeadlineMs);
        auto fetchExact = [this, &edge, &hardDeadline, producerMember](
                            const ndn::Name& name) {
          if (m_groupCoordinator->terminal()) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 group is terminal before exact fetch");
          }
          const auto remaining = remainingDeadlineMs(hardDeadline);
          if (remaining == 0) {
            throw std::runtime_error("NDNSF_DATA_V1 hard deadline expired");
          }
          const auto bounded = static_cast<int>(std::min<std::uint64_t>(
            remaining,
            std::min<std::uint64_t>(edge.noProgressDeadlineMs,
                                    static_cast<std::uint64_t>(m_fetchTimeoutMs))));
          auto content = m_ctx.fetchSignedExactData(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            name,
            ndn::Name(producerMember->provider),
            std::max(1, bounded),
            [coordinator = m_groupCoordinator] {
              return coordinator->terminal();
            });
          if (!content) {
            throw std::runtime_error(
              "failed to fetch signed exact Data: " + name.toUri());
          }
          if (!m_groupCoordinator->recordProgress(nowEpochMs())) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 no-progress/hard deadline or cancellation");
          }
          return std::vector<std::uint8_t>(content->begin(), content->end());
        };

        const auto expectedConsumers = edge.consumerRoles.empty()
          ? std::vector<std::string>{edge.consumerRole} : edge.consumerRoles;
        const auto manifestWire = fetchExact(ndn::Name(edge.manifestDataName));
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
          manifest.requester = m_ctx.requesterName().toUri();
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
            manifest.requester != m_ctx.requesterName().toUri() ||
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

        std::vector<std::pair<ndn::Name, std::vector<std::uint8_t>>>
          fetchedSegments;
        fetchedSegments.reserve(manifest.segmentCount);
        std::uint64_t fetchedBytes = 0;
        std::vector<std::string> ciphertextDigests;
        ciphertextDigests.reserve(manifest.segmentCount);
        for (std::size_t index = 0; index < manifest.segmentCount; ++index) {
          const auto dataName = exactTensorSegmentName(edge, index);
          const auto wire = fetchExact(dataName);
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
          fetchedSegments.emplace_back(dataName, wire);
        }
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
            wire, dataName.toUri());
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
          const auto accepted = m_groupCoordinator->acceptSegment(
            decoded.manifest, segment, dataName.toUri());
          (void)accepted; // identical duplicates remain readable/idempotent
          plaintextBySegment.emplace(
            segment.descriptor.segmentNo,
            m_groupCoordinator->openSegment(
              decoded.manifest, segment, dataName.toUri()));
        }
        if (plaintextBySegment.size() != manifest.segmentCount) {
          throw std::runtime_error(
            "NDNSF_DATA_V1 exact tensor object is incomplete");
        }
        TensorBundle bundle;
        bundle.name = edge.tensors.size() == 1 ?
          edge.tensors.front() : edge.plannedDataName;
        for (const auto& item : plaintextBySegment) {
          bundle.payload.insert(bundle.payload.end(),
                                item.second.begin(), item.second.end());
        }
        if (bundle.payload.size() != manifest.totalBytes ||
            sha256TensorBytes(bundle.payload) != manifest.contentDigest) {
          throw std::runtime_error(
            "TensorObjectManifestV1 reconstructed content mismatch");
        }
        bundle.expectedSegments = manifest.segmentCount;
        bundle.expectedBytes = manifest.totalBytes;
        validateCanonicalYoloMergeInput(edge, bundle);
        logDependencyObject(sessionId, edge, "fetch-exact-ndn",
                            bundle.payload.size(), "ok");
        return bundle;
      }
      const bool localProducer = producerMember != nullptr &&
        producerMember->provider == m_ctx.localProvider().toUri();
      if (localProducer) {
        const auto key = localDataV1Key(
          sessionId, edge.collectiveOperationIndex, producerRank, tensorDigest);
        std::unique_lock<std::mutex> lock(m_localDataV1Mutex);
        if (m_localDataV1Cv.wait_for(
              lock,
              std::chrono::milliseconds(m_fetchTimeoutMs),
              [&] { return m_localDataV1Segments.count(key) != 0; })) {
          encodedSegments = m_localDataV1Segments.at(key);
        }
      }
      else {
        if (producerMember == nullptr) {
          throw std::runtime_error(
            "NDNSF_DATA_V1 producer identity is not present in the capability");
        }
        CollectiveOperationManifestV1 nameBinding;
        nameBinding.requestId = capability.requestId;
        nameBinding.attemptId = capability.attemptId;
        nameBinding.planDigest = capability.planDigest;
        nameBinding.groupId = capability.groupId;
        nameBinding.epoch = capability.epoch;
        nameBinding.operationIndex = edge.collectiveOperationIndex;
        nameBinding.producerRank = producerRank;
        nameBinding.tensorDigest = tensorDigest;
        const auto deadline = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(m_fetchTimeoutMs);
        auto fetchExactSegment = [this, &edge, &capability, &nameBinding,
                                  &deadline, producerMember](std::size_t index) {
          if (m_groupCoordinator->terminal()) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 group is terminal before exact segment fetch");
          }
          const auto remaining = remainingDeadlineMs(deadline);
          if (remaining == 0) {
            throw std::runtime_error(
              "NDNSF_DATA_V1 exact segment fetch deadline expired");
          }
          const auto name = ndn::Name(ProviderGroupCoordinator::makeDataName(
            capability, nameBinding, index));
          auto content = m_ctx.fetchSignedExactData(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            name,
            ndn::Name(producerMember->provider),
            static_cast<int>(std::min<std::uint64_t>(
              remaining, static_cast<std::uint64_t>(m_fetchTimeoutMs))),
            [coordinator = m_groupCoordinator] {
              return coordinator->terminal();
            });
          if (!content) {
            throw std::runtime_error(
              "failed to fetch signed exact NDNSF_DATA_V1 segment: " +
              name.toUri());
          }
          return *content;
        };

        std::vector<ndn::Buffer> fetched;
        std::size_t segmentCount = expectedSegments;
        if (segmentCount == 0) {
          fetched.push_back(fetchExactSegment(0));
          const auto first = ProviderGroupCoordinator::decodeSegment(
            std::vector<std::uint8_t>(fetched.front().begin(),
                                      fetched.front().end()));
          segmentCount = static_cast<std::size_t>(first.manifest.segmentCount);
        }
        if (segmentCount == 0 || segmentCount > operation.maxSegments) {
          throw std::runtime_error(
            "NDNSF_DATA_V1 exact segment count exceeds the capability");
        }
        fetched.reserve(segmentCount);
        for (std::size_t index = fetched.size(); index < segmentCount; ++index) {
          fetched.push_back(fetchExactSegment(index));
        }
        encodedSegments = std::move(fetched);
      }
      if (!encodedSegments) {
        throw std::runtime_error(
          "failed to fetch NDNSF_DATA_V1 segments for: " + edge.plannedDataName);
      }
      if (expectedSegments == 0) {
        expectedSegments = encodedSegments->size();
      }
      std::map<std::uint64_t, std::vector<std::uint8_t>> plaintextBySegment;
      std::uint64_t manifestSegmentCount = 0;
      std::uint64_t manifestTotalBytes = 0;
      for (const auto& encoded : *encodedSegments) {
        const auto decoded = ProviderGroupCoordinator::decodeSegment(
          std::vector<std::uint8_t>(encoded.begin(), encoded.end()));
        if (decoded.segments.empty()) {
          throw std::runtime_error("NDNSF_DATA_V1 SVS publication has no segment");
        }
        const auto& manifest = decoded.manifest;
        const auto& segment = decoded.segments.front();
        if (manifestSegmentCount == 0) {
          manifestSegmentCount = manifest.segmentCount;
          manifestTotalBytes = manifest.totalBytes;
        }
        else if (manifest.segmentCount != manifestSegmentCount ||
                 manifest.totalBytes != manifestTotalBytes) {
          throw std::runtime_error("NDNSF_DATA_V1 manifest bounds differ across segments");
        }
        const auto result = m_groupCoordinator->acceptSegment(
          manifest, segment, segment.dataName);
        if (result == DataSegmentReplayWindow::Result::Accepted) {
          plaintextBySegment.emplace(
            segment.descriptor.segmentNo,
            m_groupCoordinator->openSegment(manifest, segment, segment.dataName));
        }
      }
      if (plaintextBySegment.empty() ||
          plaintextBySegment.size() != expectedSegments) {
        throw std::runtime_error("NDNSF_DATA_V1 operation is incomplete");
      }
      TensorBundle bundle;
      bundle.name = edge.tensors.size() == 1 ? edge.tensors.front() : edge.plannedDataName;
      for (const auto& item : plaintextBySegment) {
        bundle.payload.insert(bundle.payload.end(), item.second.begin(), item.second.end());
      }
      bundle.expectedSegments = static_cast<std::size_t>(manifestSegmentCount);
      bundle.expectedBytes = static_cast<std::size_t>(manifestTotalBytes);
      if (edge.expectedBytes != 0 && bundle.payload.size() != edge.expectedBytes) {
        throw std::runtime_error("NDNSF_DATA_V1 dependency byte count mismatch");
      }
      if (!m_groupCoordinator->recordProgress(nowEpochMs())) {
        throw std::runtime_error("NDNSF_DATA_V1 group deadline or cancellation");
      }
      validateCanonicalYoloMergeInput(edge, bundle);
      logDependencyObject(sessionId, edge, "fetch-ndnsf-data-v1",
                          bundle.payload.size(), "ok");
      return bundle;
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
      edge.expectedSegments);
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
    validateCanonicalYoloMergeInput(edge, bundle);
    logDependencyObject(sessionId, edge, "fetch", bundle.payload.size(), "ok");
    return bundle;
  });
}

void
NdnsfCollaborationDependencyIo::publishOutput(const std::string& sessionId,
                                              const DependencyEdge& edge,
                                              const TensorBundle& bundle)
{
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
    const auto producerRank = producerRankForEdge(capability, edge);
    const auto chunkSize = std::max<std::size_t>(1, m_maxSegmentSize);
    std::vector<std::vector<std::uint8_t>> chunks;
    for (std::size_t offset = 0; offset < bundle.payload.size(); offset += chunkSize) {
      const auto count = std::min(chunkSize, bundle.payload.size() - offset);
      chunks.emplace_back(bundle.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                          bundle.payload.begin() + static_cast<std::ptrdiff_t>(offset + count));
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
      const auto* producerMember = memberForRank(capability, producerRank);
      if (producerMember == nullptr ||
          producerMember->provider != m_ctx.localProvider().toUri() ||
          !ndn::Name(producerMember->endpointPrefix)
             .isPrefixOf(ndn::Name(edge.manifestDataName))) {
        throw std::runtime_error(
          "V3 mayPublish endpoint is not owned by the local Provider");
      }
      std::vector<std::string> exactNames;
      exactNames.reserve(chunks.size());
      for (std::size_t index = 0; index < chunks.size(); ++index) {
        exactNames.push_back(exactTensorSegmentName(edge, index).toUri());
      }
      const auto createdAtMs = nowEpochMs();
      const auto sealed = m_groupCoordinator->sealOperation(
        operation,
        producerRank.empty() ? "0" : producerRank,
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
      manifest.requester = m_ctx.requesterName().toUri();
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
      manifest.contentDigest = sha256TensorBytes(bundle.payload);
      manifest.totalBytes = bundle.payload.size();
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
      if (m_outputPublicationGate && !m_outputPublicationGate(
            sessionId, edge, manifest.contentDigest, bundle.payload.size())) {
        return;
      }
      if (!m_ctx.publishSignedExactData(
            edge.transportScope.empty() ? edge.scope : edge.transportScope,
            publications, m_freshnessMs)) {
        throw std::runtime_error(
          "failed to publish exact signed NDNSF_DATA_V1 tensor object");
      }
      if (!m_groupCoordinator->recordProgress(nowEpochMs())) {
        throw std::runtime_error(
          "NDNSF_DATA_V1 group deadline or cancellation after publication");
      }
      logDependencyObject(sessionId, edge, "publish-exact-ndn",
                          bundle.payload.size(), "ok");
      return;
    }
    const auto sealed = m_groupCoordinator->sealOperation(
      operation,
      producerRank.empty() ? "0" : producerRank,
      edge.collectiveSourceLayoutDigest.empty() ? "layout/unknown" : edge.collectiveSourceLayoutDigest,
      edge.collectiveTargetLayoutDigest.empty() ? "layout/unknown" : edge.collectiveTargetLayoutDigest,
      edge.collectiveTensorDigest.empty() ? edge.scope : edge.collectiveTensorDigest,
      chunks,
      nowEpochMs());
    std::vector<std::pair<ndn::Name, ndn::Buffer>> publications;
    publications.reserve(sealed.segments.size());
    for (const auto& segment : sealed.segments) {
      const auto wire = ProviderGroupCoordinator::encodeSegment(
        sealed.manifest, segment);
      publications.emplace_back(
        ndn::Name(segment.dataName),
        ndn::Buffer(wire.begin(), wire.end()));
    }
    if (!m_ctx.publishSignedExactData(
          edge.transportScope.empty() ? edge.scope : edge.transportScope,
          publications, m_freshnessMs)) {
      throw std::runtime_error(
        "failed to publish signed exact NDNSF_DATA_V1 segments");
    }
    const auto tensorDigest = edge.collectiveTensorDigest.empty() ?
      edge.scope : edge.collectiveTensorDigest;
    {
      std::lock_guard<std::mutex> lock(m_localDataV1Mutex);
      m_localDataV1Segments[localDataV1Key(
        sessionId, edge.collectiveOperationIndex, producerRank, tensorDigest)] =
          [&publications] {
            std::vector<ndn::Buffer> segments;
            segments.reserve(publications.size());
            for (const auto& publication : publications) {
              segments.push_back(publication.second);
            }
            return segments;
          }();
    }
    m_localDataV1Cv.notify_all();
    m_groupCoordinator->recordProgress(nowEpochMs());
    logDependencyObject(sessionId, edge, "publish-ndnsf-data-v1",
                        bundle.payload.size(), "ok");
    return;
  }
  const ndn::Buffer payload(bundle.payload.data(), bundle.payload.size());
  logDependencyObject(sessionId, edge, "publish", bundle.payload.size(), "ok");
  if (edge.plannedDataName.empty()) {
    m_ctx.publishLarge(
      edge.transportScope.empty() ? edge.scope : edge.transportScope,
      edge.topicPrefix.empty() ? ndn::Name("/output") : ndn::Name(edge.topicPrefix),
      payload,
      m_maxSegmentSize,
      m_freshnessMs);
    return;
  }
  m_ctx.publishLargeNamed(
    edge.transportScope.empty() ? edge.scope : edge.transportScope,
    ndn::Name(edge.plannedDataName),
    payload,
    m_maxSegmentSize,
    m_freshnessMs);
}

} // namespace ndnsf::di
