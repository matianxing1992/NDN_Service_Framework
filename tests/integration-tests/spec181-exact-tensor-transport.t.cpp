#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>

#include <algorithm>
#include <chrono>
#include <map>

namespace ndnsf::di::tests {
using namespace ndn_service_framework;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(Spec181ExactTensorTransport)
void
runTransfer(const std::string& mode)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 2;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();

  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  std::size_t mutations = 0;
  std::size_t segmentInterests = 0;
  auto forwardInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName().toUri().find("/SEG/") != std::string::npos) {
          ++segmentInterests;
        }
        producerFace.receive(interest);
      });
  auto forwardData = producerFace.onSendData.connect([&] (const ndn::Data& data) {
    const bool root = data.getName().get(-1).toUri() == "MANIFEST";
    const bool segment = data.getName().toUri().find("/SEG/") != std::string::npos;
    if (mutations == 0 && ((root && (mode == "signature" || mode == "bounds")) ||
                          (segment && mode == "ciphertext"))) {
      ++mutations;
      auto modified = data;
      ndn::Buffer content(data.getContent().value(),
                          data.getContent().value() + data.getContent().value_size());
      if (mode == "bounds") {
        // The contract places three little-endian u64 bounds after the marker
        // and content digest. Claim a consistent size just above maxBytes.
        const std::size_t offset =
          4 + std::string("TensorObjectManifestV1ContextCompact").size() + 32;
        const std::uint64_t total = (2U << 20) + 1;
        const std::uint64_t values[] = {total, 7000, 1 + (total - 1) / 7000};
        BOOST_REQUIRE_GE(content.size(), offset + sizeof(values));
        for (std::size_t field = 0; field < 3; ++field) {
          for (std::size_t byte = 0; byte < 8; ++byte) {
            content[offset + 8 * field + byte] = (values[field] >> (8 * byte)) & 0xff;
          }
        }
      }
      else {
        content.back() ^= 1;
      }
      modified.setContent(content);
      // Preserve the expected outer producer identity, so rejection must come
      // from the real tensor verifier, not the NDN signer-name check.
      environment.keyChain().sign(modified, ndn::security::signingByCertificate(
        producer.getSigningCertificateName()));
      consumerFace.receive(modified);
    }
    else {
      consumerFace.receive(data);
    }
  });

  ProviderGroupCoordinatorOptions options;
  options.wrapEpochKey = [&] (const std::string& provider, const ProviderGroupBytes& key) {
    const auto keyName = provider == producer.getName().toUri()
      ? producer.getSigningKeyName() : consumer.getSigningKeyName();
    const auto publicKey = environment.keyChain().getPib()
      .getIdentity(keyName.getPrefix(-2)).getKey(keyName).getPublicKey();
    const auto wrapped = wrapSelectionGatedInputKey(
      ndn::Buffer(key.begin(), key.end()), publicKey);
    return ProviderGroupBytes(wrapped.begin(), wrapped.end());
  };
  std::size_t packets = 0;
  std::map<ndn::Name, ndn::Buffer> uniquePackets;
  std::size_t largestPacket = 0;
  auto observe = producerFace.onSendData.connect([&] (const ndn::Data& data) {
    if (data.getName().toUri().find("/NDNSF-DI/TENSOR/") != std::string::npos) {
      ++packets;
      const auto& wire = data.wireEncode();
      const ndn::Buffer bytes(wire.begin(), wire.end());
      const auto inserted = uniquePackets.emplace(data.getName(), bytes);
      BOOST_CHECK(inserted.second || inserted.first->second == bytes);
      largestPacket = std::max(largestPacket, data.wireEncode().size());
      BOOST_CHECK_LE(data.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
    }
  });

  const auto sha = [] (char value) {
    return std::string("sha256:") + std::string(64, value);
  };
  const ndn::Name requestId("/request/v3-exact-dependency");
  const std::string planDigest = sha('1');
  const std::string groupId = "group-v3-exact";
  const std::string sourceLayout = sha('2');
  const std::string targetLayout = sha('3');
  const std::string tensorDigest = sha('4');

  GroupOperationV1 operation;
  operation.operationIndex = 7;
  operation.kind = "PIPELINE_TRANSFER";
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = sourceLayout;
  operation.maxBytes = 2U << 20;
  operation.maxSegments = 300;

  auto producerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(options);
  const auto capability = producerCoordinator->createCapability(
      requestId.toUri(), "attempt-1", planDigest, groupId, 3,
      {{producer.getName().toUri(), 0, "offer-producer",
        producer.getName().toUri()},
       {consumer.getName().toUri(), 1, "offer-consumer",
        consumer.getName().toUri()}},
      {operation}, 2U << 20, 5000, 30000);
  auto consumerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(options);
  consumerCoordinator->installCapability(
      capability,
      producerCoordinator->epochKeyForProvider(producer.getName().toUri()),
      true);

  NativeTensorEndpointV3 endpoint;
  endpoint.producerNamespace = producer.getName().toUri();
  endpoint.requester = profile.userIdentity.toUri();
  endpoint.requestId = requestId.toUri();
  endpoint.attempt = 1;
  endpoint.planDigest = planDigest;
  endpoint.groupId = groupId;
  endpoint.groupEpoch = "3";
  endpoint.operation = operation.kind;
  endpoint.round = operation.operationIndex;
  endpoint.sourceKind = "ROLE";
  endpoint.producerRole = "S0R0";
  endpoint.producerRank = 0;
  endpoint.consumerRole = "S1R0";
  endpoint.consumerRoles = {"S1R0"};
  endpoint.tensorId = std::string(160, 't');
  endpoint.tensorDigest = tensorDigest;
  endpoint.layoutDigest = sourceLayout;
  endpoint.targetLayoutDigest = targetLayout;
  endpoint.microbatch = 0;
  endpoint.segmentCount = operation.maxSegments;
  endpoint.manifestDigest = sha('5');
  endpoint.securityProfile = "NDNSF_DATA_V1";
  endpoint.noProgressDeadlineMs = capability.noProgressMs;
  endpoint.hardDeadlineMs = capability.hardDeadlineMs;
  endpoint.endpointDigest = sha('6');

  DependencyEdge edge;
  edge.scope = groupId;
  edge.producerRole = endpoint.producerRole;
  edge.consumerRole = endpoint.consumerRole;
  edge.consumerRoles = endpoint.consumerRoles;
  edge.plannedDataName = tensorObjectNamePrefix(endpoint);
  edge.tensors = {endpoint.tensorId};
  edge.requestId = endpoint.requestId;
  edge.attemptEpoch = endpoint.attempt;
  edge.useNdnsfDataV1 = true;
  edge.collectiveOperationIndex = operation.operationIndex;
  edge.collectiveProducerRank = "0";
  edge.collectiveSourceLayoutDigest = sourceLayout;
  edge.collectiveTargetLayoutDigest = targetLayout;
  edge.collectiveTensorDigest = tensorDigest;
  edge.transportScope = groupId;
  edge.producerProvider = producer.getName().toUri();
  edge.declaredByV3 = true;
  edge.manifestDataName = tensorObjectManifestName(endpoint);
  edge.maxSegments = endpoint.segmentCount;
  edge.endpointDigest = endpoint.endpointDigest;
  edge.planDigest = planDigest;
  edge.manifestContractDigest = endpoint.manifestDigest;
  edge.tensorDigest = tensorDigest;
  edge.layoutDigest = sourceLayout;
  edge.securityProfile = endpoint.securityProfile;
  edge.operationKind = operation.kind;
  edge.round = operation.operationIndex;
  edge.microbatch = 0;
  edge.noProgressDeadlineMs = capability.noProgressMs;
  edge.hardDeadlineMs = capability.hardDeadlineMs;

  ServiceProvider::CollaborationAssignment producerAssignment;
  producerAssignment.role = endpoint.producerRole;
  producerAssignment.service = profile.serviceName;
  ServiceProvider::CollaborationAssignment consumerAssignment;
  consumerAssignment.role = endpoint.consumerRole;
  consumerAssignment.service = profile.serviceName;
  ServiceProvider::CollaborationContext producerContext(
      producer, profile.userIdentity, requestId, RequestMessage(),
      producerAssignment);
  ServiceProvider::CollaborationContext consumerContext(
      consumer, profile.userIdentity, requestId, RequestMessage(),
      consumerAssignment);

  const bool legacy = mode == "legacy" || mode == "legacy-binding";
  const bool manual = legacy || mode == "hmac" || mode == "index";
  const std::size_t chunkSize = legacy ? 7 : 7000;
  TensorBundle original;
  original.name = endpoint.tensorId;
  original.payload.resize(mode.empty() ? 200 * 7000 + 17 : legacy ? 19 : 7017);
  for (std::size_t index = 0; index < original.payload.size(); ++index) {
    original.payload[index] = static_cast<std::uint8_t>(index * 31 + 7);
  }
  std::size_t publicationGateCalls = 0;
  NdnsfCollaborationDependencyIo::OutputPublicationGate publicationGate;
  if (mode == "withhold" || mode == "gate-permit" || mode == "invalid-before-gate") {
    publicationGate = [&](const std::string& session, const DependencyEdge& observed,
                          const std::string& contentDigest, std::size_t bytes) {
      ++publicationGateCalls;
      BOOST_CHECK_EQUAL(session, "session-v3");
      BOOST_CHECK_EQUAL(observed.requestId, edge.requestId);
      BOOST_CHECK_EQUAL(observed.attemptEpoch, 1U);
      BOOST_CHECK_EQUAL(observed.manifestDataName, edge.manifestDataName);
      BOOST_CHECK_EQUAL(contentDigest, sha256TensorBytes(original.payload));
      BOOST_CHECK_EQUAL(bytes, original.payload.size());
      return mode == "gate-permit";
    };
  }
  NdnsfCollaborationDependencyIo producerIo(
      producerContext, 5000, chunkSize, 60000, producerCoordinator, nullptr, publicationGate);
  NdnsfCollaborationDependencyIo consumerIo(
      consumerContext, 5000, chunkSize, 60000, consumerCoordinator);
  const auto expectedSegments = 1 + (original.payload.size() - 1) / chunkSize;
  if (manual) {
    // Produce the old representation with the maintained legacy encoders.
    // Consumer verification and restoration still use the real DependencyIo.
    std::vector<ProviderGroupBytes> chunks;
    std::vector<std::string> names;
    for (std::size_t offset = 0; offset < original.payload.size(); offset += chunkSize) {
      const auto end = std::min(original.payload.size(), offset + chunkSize);
      chunks.emplace_back(original.payload.begin() + offset, original.payload.begin() + end);
      auto name = ndn::Name(edge.manifestDataName).getPrefix(-1);
      names.push_back(name.append("SEG").appendSegment(names.size()).toUri());
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    const auto sealed = producerCoordinator->sealOperation(
      operation, "0", sourceLayout, targetLayout, tensorDigest, chunks, nowMs, names);
    TensorObjectManifestV1 manifest;
    manifest.capabilityDigest = "sha256:" + capability.capabilityDigest;
    manifest.epochKeyId = capability.epochKeyId;
    manifest.requester = profile.userIdentity.toUri();
    manifest.requestId = requestId.toUri();
    manifest.attemptId = "1";
    manifest.planDigest = planDigest;
    manifest.groupId = groupId;
    manifest.epoch = "3";
    manifest.operationIndex = operation.operationIndex;
    manifest.round = edge.round;
    manifest.operationKind = operation.kind;
    manifest.producerRole = edge.producerRole;
    manifest.producerRank = 0;
    manifest.consumerRoles = edge.consumerRoles;
    manifest.sourceLayoutDigest = sourceLayout;
    manifest.targetLayoutDigest = targetLayout;
    manifest.tensorId = endpoint.tensorId;
    manifest.tensorDigest = tensorDigest;
    manifest.contentDigest = sha256TensorBytes(original.payload);
    manifest.totalBytes = original.payload.size();
    manifest.segmentSize = chunkSize;
    manifest.segmentCount = chunks.size();
    manifest.createdAtMs = nowMs;
    manifest.noProgressMs = capability.noProgressMs;
    manifest.hardDeadlineMs = capability.hardDeadlineMs;
    manifest.endpointDigest = edge.endpointDigest;
    manifest.manifestContractDigest = edge.manifestContractDigest;
    std::vector<std::pair<ndn::Name, ndn::Buffer>> publications;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
      auto innerManifest = sealed.manifest;
      auto segment = sealed.segments[index];
      if (index == 0 && mode == "hmac") {
        segment.hmac.back() ^= 1;
      }
      if (index == 0 && mode == "index") {
        segment.descriptor.segmentNo = 1;
      }
      if (index == 0 && mode == "legacy-binding") {
        innerManifest.requestId = "/different-request";
        segment.descriptor.requestId = innerManifest.requestId;
        segment.descriptor.manifestDigest = innerManifest.digest();
      }
      const auto wire = legacy
        ? ProviderGroupCoordinator::encodeSegment(innerManifest, segment)
        : ProviderGroupCoordinator::encodeSegmentCompact(innerManifest, segment);
      manifest.orderedSegmentDigests.push_back(sha256TensorBytes(wire));
      publications.emplace_back(ndn::Name(names[index]), ndn::Buffer(wire.begin(), wire.end()));
    }
    manifest.producerSignature = producerCoordinator->signTensorObjectManifest(
      manifest.signingBytes());
    manifest.objectManifestDigest = manifest.digest();
    const auto wire = encodeTensorObjectManifest(manifest);
    publications.emplace_back(ndn::Name(edge.manifestDataName),
                              ndn::Buffer(wire.begin(), wire.end()));
    BOOST_REQUIRE(producerContext.publishSignedExactData(groupId, publications, 60000));
  }
  else {
    if (mode == "invalid-before-gate") {
      edge.endpointDigest.clear();
      BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", edge, original), std::invalid_argument);
      BOOST_CHECK_EQUAL(publicationGateCalls, 0U);
      BOOST_CHECK_EQUAL(packets, 0U);
      return;
    }
    BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", edge, original));
  }
  if (mode == "withhold" || mode == "gate-permit") BOOST_CHECK_EQUAL(publicationGateCalls, 1U);
  if (mode == "context") edge.endpointDigest = sha('9');

  auto fetched = consumerIo.prefetchInput("session-v3", edge);
  environment.pumpUntil([&] {
    return fetched.wait_for(0ms) == std::future_status::ready;
  });
  if (!mode.empty() && mode != "legacy" && mode != "gate-permit") {
    const auto expected = mode == "ciphertext" ? "ciphertext commitment mismatch" :
                          mode == "bounds" ? "does not match mustFetch authority" :
                          mode == "hmac" ? "HMAC verification failed" :
                          mode == "index" ? "index/rank/size mismatch" :
                          mode == "legacy-binding" ? "inner manifest mismatch" :
                          mode == "withhold" ? "failed to fetch signed exact Data" :
                                             "producer signature mismatch";
    BOOST_CHECK_EXCEPTION(fetched.get(), std::runtime_error,
      [&] (const std::runtime_error& error) {
        BOOST_TEST_MESSAGE("Actual rejection: " << error.what());
        return std::string(error.what()).find(expected) != std::string::npos;
      });
    if (mode != "context" && mode != "withhold" && !manual) BOOST_CHECK_EQUAL(mutations, 1U);
    if (mode == "withhold") {
      BOOST_CHECK_EQUAL(packets, 0U);
      BOOST_CHECK_EQUAL(segmentInterests, 0U);
    }
    if (mode == "bounds") BOOST_CHECK_EQUAL(segmentInterests, 0U);
    return;
  }
  std::optional<TensorBundle> reconstructed;
  BOOST_REQUIRE_NO_THROW(reconstructed = fetched.get());
  BOOST_REQUIRE(reconstructed);
  BOOST_CHECK_EQUAL_COLLECTIONS(
      reconstructed->payload.begin(), reconstructed->payload.end(),
      original.payload.begin(), original.payload.end());
  BOOST_CHECK_EQUAL(reconstructed->expectedSegments, expectedSegments);
  BOOST_CHECK_EQUAL(reconstructed->expectedBytes, original.payload.size());
  BOOST_CHECK_EQUAL(uniquePackets.size(), expectedSegments + 1);
  BOOST_CHECK_GE(packets, uniquePackets.size());
  BOOST_CHECK_LE(largestPacket, ndn::MAX_NDN_PACKET_SIZE);
  BOOST_TEST_MESSAGE("Actual signed packets: " << packets << ", largest bytes: " << largestPacket);
}

BOOST_AUTO_TEST_CASE(LargeTensorUsesBoundedSignedPackets) { runTransfer(""); }
BOOST_AUTO_TEST_CASE(LegacyRepresentationStillReconstructs) { runTransfer("legacy"); }
BOOST_AUTO_TEST_CASE(RejectsChangedProducerSignature) { runTransfer("signature"); }
BOOST_AUTO_TEST_CASE(RejectsChangedCiphertextCommitment) { runTransfer("ciphertext"); }
BOOST_AUTO_TEST_CASE(RejectsChangedAuthenticatedContext) { runTransfer("context"); }
BOOST_AUTO_TEST_CASE(RejectsBoundsBeforeFetchingSegments) { runTransfer("bounds"); }

BOOST_AUTO_TEST_CASE(RejectsInnerHmacWithValidOuterManifest) { runTransfer("hmac"); }
BOOST_AUTO_TEST_CASE(RejectsSegmentIndexWithValidOuterManifest) { runTransfer("index"); }
BOOST_AUTO_TEST_CASE(LegacyFieldsAreCheckedBeforeRestoration) { runTransfer("legacy-binding"); }
BOOST_AUTO_TEST_CASE(WithheldValidatedOutputPublishesNoManifestOrSegments) { runTransfer("withhold"); }
BOOST_AUTO_TEST_CASE(PublicationGatePermitPreservesTensorBytes) { runTransfer("gate-permit"); }
BOOST_AUTO_TEST_CASE(MalformedEndpointRejectsBeforePublicationGate) { runTransfer("invalid-before-gate"); }

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di::tests
