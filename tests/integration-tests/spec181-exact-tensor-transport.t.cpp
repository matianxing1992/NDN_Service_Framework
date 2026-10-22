#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <ndn-cxx/util/signal/scoped-connection.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>

namespace ndn_service_framework::test {
struct TensorOriginalOracle
{
  static std::size_t fillUnrelated(ServiceProvider& provider, ndn::KeyChain& keys,
                                   std::size_t count)
  {
    std::lock_guard<std::mutex> lock(provider._cache_mutex);
    if (provider.m_IMS.getLimit() < count + 64) {
      throw std::logic_error("publication probe exceeds the existing IMS limit");
    }
    while (provider.m_IMS.size() < count) {
      auto name = ndn::Name(provider.getName()).append("SPEC216").append("UNRELATED")
                    .appendNumber(provider.m_IMS.size());
      auto data = std::make_shared<ndn::Data>(name);
      data->setContent(ndn::Buffer{0x42});
      keys.sign(*data, ndn::security::signingWithSha256());
      provider.m_IMS.insert(*data);
    }
    return provider.m_IMS.size();
  }

  static bool publish(ServiceProvider& provider, const ndn::Name& name,
                      const ndn::Buffer& payload,
                      std::vector<std::shared_ptr<const ndn::Data>>& originals)
  {
    return provider.publishCollaborationOwnedExactData(
      ndn::Name("/spec216/publication-probe"), "publication-probe",
      {{name, payload}}, 60000, nullptr, &originals, 64U * 1024U * 1024U);
  }

  static void poison(ServiceProvider& provider, const ndn::Data& packet)
  {
    std::lock_guard<std::mutex> lock(provider._cache_mutex);
    provider.m_IMS.insert(packet);
  }

  static void poisonStale(ServiceProvider& provider, const ndn::Data& packet)
  {
    std::lock_guard<std::mutex> lock(provider._cache_mutex);
    provider.m_IMS.insert(packet, ndn::time::milliseconds(0));
  }

  static bool contains(ServiceProvider& provider, const ndn::Name& name, bool fresh = false)
  {
    std::lock_guard<std::mutex> lock(provider._cache_mutex);
    ndn::Interest interest(name);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(fresh);
    return static_cast<bool>(provider.m_IMS.find(interest));
  }

  static bool publishBatch(ServiceProvider& provider,
                           const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
                           std::vector<std::shared_ptr<const ndn::Data>>& originals)
  {
    return provider.publishCollaborationOwnedExactData(
      ndn::Name("/spec216/publication-probe"), "publication-probe",
      objects, 60000, nullptr, &originals, 64U * 1024U * 1024U);
  }
};
}

namespace ndnsf::di::tests {
using namespace ndn_service_framework;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(Spec181ExactTensorTransport)

BOOST_AUTO_TEST_CASE(OwnedExactPublicationCacheOccupancyProbe)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 1;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  auto& provider = environment.provider(0);
  const ndn::Buffer payload(5000, 0x52);
  std::size_t sequence = 0;
  for (const std::size_t target : {0U, 256U, 4096U, 16384U}) {
    const auto before = ndn_service_framework::test::TensorOriginalOracle::fillUnrelated(
      provider, environment.keyChain(), target);
    for (std::size_t sample = 0; sample < 16; ++sample) {
      auto name = ndn::Name(provider.getName()).append("SPEC216").append("TENSOR")
                    .appendNumber(sequence++);
      std::vector<std::shared_ptr<const ndn::Data>> originals;
      const auto started = std::chrono::steady_clock::now();
      const bool published = ndn_service_framework::test::TensorOriginalOracle::publish(
        provider, name, payload, originals);
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
      BOOST_REQUIRE(published);
      BOOST_REQUIRE_EQUAL(originals.size(), 1U);
      BOOST_CHECK(originals.front()->getName() == name);
      const auto& content = originals.front()->getContent();
      BOOST_REQUIRE_EQUAL(content.value_size(), payload.size());
      BOOST_CHECK(std::equal(content.value_begin(), content.value_end(), payload.begin()));
      BOOST_TEST_MESSAGE("SPEC216_OWNED_PUBLICATION_CACHE_SAMPLE entriesBefore="
                         << before + sample << " sample=" << sample << " duration_ns=" << ns);
    }
  }
}

BOOST_AUTO_TEST_CASE(OwnedExactPublicationPreservesStaleAndMixedBatchConflicts)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 1;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  auto& provider = environment.provider(0);
  using Oracle = ndn_service_framework::test::TensorOriginalOracle;
  const auto prefix = ndn::Name(provider.getName()).append("SPEC216");
  const auto existing = ndn::Name(prefix).append("EXISTING");
  const auto fresh = ndn::Name(prefix).append("FRESH");
  const auto staleName = ndn::Name(prefix).append("STALE");
  const ndn::Buffer payload{0x51, 0x52};
  std::vector<std::shared_ptr<const ndn::Data>> originals, retry;
  BOOST_REQUIRE(Oracle::publish(provider, existing, payload, originals));
  BOOST_REQUIRE_EQUAL(originals.size(), 1U);
  BOOST_REQUIRE(Oracle::publish(provider, existing, payload, retry));
  BOOST_REQUIRE_EQUAL(retry.size(), 1U);
  BOOST_CHECK(retry.front()->wireEncode() == originals.front()->wireEncode());

  auto conflicting = std::make_shared<ndn::Data>(*originals.front());
  conflicting->setContent(ndn::Buffer{0x42});
  environment.keyChain().sign(*conflicting, ndn::security::signingByCertificate(
    provider.getSigningCertificateName()));
  Oracle::poison(provider, *conflicting);
  retry.clear();
  BOOST_CHECK_THROW(Oracle::publishBatch(provider, {{fresh, payload}, {existing, payload}},
                                        retry), std::invalid_argument);
  BOOST_CHECK(retry.empty());
  BOOST_CHECK(!Oracle::contains(provider, fresh));

  auto stale = std::make_shared<ndn::Data>(staleName);
  stale->setFreshnessPeriod(ndn::time::milliseconds(0));
  stale->setContent(payload);
  environment.keyChain().sign(*stale, ndn::security::signingByCertificate(
    provider.getSigningCertificateName()));
  Oracle::poisonStale(provider, *stale);
  environment.pumpUntil([&] { return !Oracle::contains(provider, staleName, true); });
  BOOST_REQUIRE(Oracle::contains(provider, staleName));
  BOOST_REQUIRE(!Oracle::contains(provider, staleName, true));
  BOOST_CHECK_THROW(Oracle::publish(provider, staleName, payload, retry), std::invalid_argument);
  BOOST_CHECK(!Oracle::contains(provider, staleName, true));
}

void
probeCancelledSourcePending(ndn::DummyClientFace& face, ndn::KeyChain& keyChain,
                            const ndn::Name& producer,
                            const std::map<ndn::Name, ndn::Interest>& issued)
{
  // A matching synchronous receive detects any surviving source Interest.
  // No Face is pumped and these packets never enter IMS or a transport.
  // Unlike a global pending count, this excludes unrelated protocol reads.
  for (const auto& [name, interest] : issued) {
    ndn::Data probe(name);
    probe.setFreshnessPeriod(ndn::time::seconds(1));
    keyChain.sign(probe, ndn::security::signingByIdentity(producer));
    const bool observable = interest.matchesData(probe);
    BOOST_CHECK_MESSAGE(observable, "pending source probe cannot match " << name);
    if (!observable) continue;
    const auto before = face.getNPendingInterests();
    face.receive(probe);
    const auto after = face.getNPendingInterests();
    BOOST_TEST_MESSAGE("SOURCE_PENDING_PROBE name=" << name
      << " canBePrefix=" << interest.getCanBePrefix()
      << " mustBeFresh=" << interest.getMustBeFresh()
      << " before=" << before << " after=" << after);
    BOOST_CHECK_MESSAGE(before == after, "cancelled source still pending: " << name);
  }
}

void
runTransfer(const std::string& mode, bool enableSpeculation = true)
{
  const bool inlineDelivery = mode.rfind("inline", 0) == 0;
  const bool windowCancellation = mode == "inline-window-cancel";
  const bool windowRefill = mode == "inline-window-refill";
  const bool boundedWindow = mode == "inline-window" || windowCancellation || windowRefill;
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
  ndn::Scheduler deliveryScheduler(consumerFace.getIoContext());
  std::set<ndn::Name> unansweredSegments;
  std::size_t maximumUnansweredSegments = 0;
  std::function<void()> cancelWindowFetch;
  bool windowCancellationRequested = false;
  std::optional<ndn::Data> heldWindowSegment;
  bool heldWindowSegmentReleased = false;
  bool refilledBeforeHeldReply = false;
  const auto releaseWindowSegment = [&] {
    heldWindowSegmentReleased = true;
    if (heldWindowSegment) {
      unansweredSegments.erase(heldWindowSegment->getName());
      consumerFace.receive(*heldWindowSegment);
      heldWindowSegment.reset();
    }
  };
  std::size_t mutations = 0;
  std::size_t segmentInterests = 0;
  std::set<ndn::Name> requestedSegments;
  std::map<ndn::Name, ndn::Interest> earlyInterests;
  std::atomic<std::size_t> manifestInterests{0};
  std::size_t segmentNamesBeforeManifest = 0;
  std::optional<ndn::Data> heldManifest;
  std::mutex manifestNameMutex;
  std::string expectedManifestUri;
  std::map<ndn::Name, ndn::Interest> issuedSourceInterests;
  auto forwardInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        bool isExpectedManifest = false;
        {
          std::lock_guard<std::mutex> lock(manifestNameMutex);
          isExpectedManifest = !expectedManifestUri.empty() &&
            interest.getName() == ndn::Name(expectedManifestUri);
          if (!expectedManifestUri.empty() &&
              ndn::Name(expectedManifestUri).getPrefix(-1).isPrefixOf(interest.getName()))
            issuedSourceInterests.insert_or_assign(interest.getName(), interest);
        }
        if (isExpectedManifest) {
          ++manifestInterests;
        }
        if (mode == "prefetch-renewals" &&
            (isExpectedManifest || (interest.getName().size() >= 2 && interest.getName().get(-2) == ndn::name::Component("DATA") && interest.getName().get(-1).isSegment()))) {
          earlyInterests.emplace(interest.getName(), interest);
        }
        if ((interest.getName().size() >= 2 && interest.getName().get(-2) == ndn::name::Component("DATA") && interest.getName().get(-1).isSegment())) {
          ++segmentInterests;
          requestedSegments.insert(interest.getName());
          if (boundedWindow) {
            unansweredSegments.insert(interest.getName());
            maximumUnansweredSegments = std::max(
              maximumUnansweredSegments, unansweredSegments.size());
            if (windowRefill && interest.getName().get(-1).toSegment() == 9) {
              refilledBeforeHeldReply = !heldWindowSegmentReleased &&
                heldWindowSegment.has_value();
              releaseWindowSegment();
            }
          }
          if ((mode == "prefetch" || mode == "prefetch-bytes" ||
               mode == "prefetch-chunk") && requestedSegments.size() == 13 && heldManifest) {
            segmentNamesBeforeManifest = requestedSegments.size();
            consumerFace.receive(*heldManifest);
            heldManifest.reset();
          }
          if (mode == "inline-prefetch" && requestedSegments.size() == 12 && heldManifest) {
            segmentNamesBeforeManifest = requestedSegments.size();
            consumerFace.receive(*heldManifest);
            heldManifest.reset();
          }
        }
        producerFace.receive(interest);
      });
  auto forwardData = producerFace.onSendData.connect([&] (const ndn::Data& data) {
    const bool root = data.getName().get(-1) == ndn::name::Component("MANIFEST");
    if ((mode == "prefetch" || mode == "prefetch-bytes" ||
         mode == "prefetch-chunk") && root && requestedSegments.size() < 13) {
      // Deterministic barrier: a manifest-dependent implementation cannot
      // satisfy this oracle by issuing the remaining names after its reply.
      heldManifest = data;
      return;
    }
    if (mode == "inline-prefetch" && root && requestedSegments.size() < 12) {
      heldManifest = data;
      return;
    }
    if (root && segmentNamesBeforeManifest == 0)
      segmentNamesBeforeManifest = requestedSegments.size();
    const bool segment = (data.getName().size() >= 2 && data.getName().get(-2) == ndn::name::Component("DATA") && data.getName().get(-1).isSegment());
    if (mutations == 0 && ((root && (mode == "signature" || mode == "bounds" ||
                                   mode == "inline-corrupt" ||
                                   mode == "inline-compressed-binding")) ||
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
      else if (mode == "inline-compressed-binding") {
        const std::vector<std::uint8_t> wire(content.begin(), content.end());
        auto framed = decodeTensorManifestWithFirstSegment(
          wire, ndn::MAX_NDN_PACKET_SIZE);
        BOOST_REQUIRE_GE(framed.first.size(), 8U);
        // Change only the authenticated expansion size; keep ciphertext and
        // the original inner Producer signature, then re-sign outer Data.
        framed.first[framed.first.size() - 8] ^= 1;
        const auto changed = encodeTensorManifestWithFirstSegment(
          framed.first, framed.second, ndn::MAX_NDN_PACKET_SIZE);
        content.assign(changed.begin(), changed.end());
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
      if (boundedWindow && segment) {
        if (windowRefill && data.getName().get(-1).toSegment() == 1 &&
            !heldWindowSegmentReleased) {
          if (!heldWindowSegment) {
            heldWindowSegment = data;
            // Let the fixed-batch implementation finish, but fail the refill
            // property independently of a transport timeout or payload check.
            deliveryScheduler.schedule(ndn::time::milliseconds(1500), releaseWindowSegment);
          }
          return;
        }
        auto packet = std::make_shared<ndn::Data>(data);
        deliveryScheduler.schedule(ndn::time::milliseconds(2), [&, packet] {
          unansweredSegments.erase(packet->getName());
          if (windowCancellation && !windowCancellationRequested) {
            windowCancellationRequested = true;
            cancelWindowFetch();
          }
          consumerFace.receive(*packet);
        });
      }
      else {
        consumerFace.receive(data);
      }
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
    bool tensorPacket = false;
    {
      std::lock_guard<std::mutex> lock(manifestNameMutex);
      tensorPacket = !expectedManifestUri.empty() &&
        ndn::Name(expectedManifestUri).getPrefix(-1).isPrefixOf(data.getName());
    }
    if (tensorPacket) {
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
  const bool feedback = mode == "feedback" || mode == "feedback-next";
  operation.operationIndex = feedback ? (mode == "feedback-next" ? 5 : 2) : 7;
  operation.kind = feedback ? "TOKEN_FEEDBACK" : "PIPELINE_TRANSFER";
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = sourceLayout;
  operation.maxBytes = 2U << 20;
  operation.maxSegments = 300;

  auto producerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(options);
  const auto inflightBudget =
    (mode == "packed-operation-bounds" || mode == "inline-packed-publish-bounds") ?
      4U << 20 : mode == "prefetch-budget" ? 100000 :
      enableSpeculation ? 2U << 20 : ndn::MAX_NDN_PACKET_SIZE;
  const auto capability = producerCoordinator->createCapability(
      requestId.toUri(), "attempt-1", planDigest, groupId, 3,
      {{producer.getName().toUri(), 0, "offer-producer",
        producer.getName().toUri()},
       {consumer.getName().toUri(), 1, "offer-consumer",
        consumer.getName().toUri()}},
      {operation}, inflightBudget,
      5000, 30000);
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
  endpoint.namingOperationStride = feedback ? 3 : 0;
  endpoint.sourceKind = "ROLE";
  endpoint.producerRole = feedback ? "S1R0" : "S0R0";
  endpoint.producerRank = 0;
  endpoint.consumerRole = feedback ? "S0R0" : "S1R0";
  endpoint.consumerRoles = mode == "reuse" ?
    std::vector<std::string>{"S1R0", "S1R1"} : std::vector<std::string>{endpoint.consumerRole};
  endpoint.tensorId = feedback ? "input_ids" : std::string(160, 't');
  endpoint.tensorDigest = tensorDigest;
  endpoint.layoutDigest = sourceLayout;
  endpoint.targetLayoutDigest = targetLayout;
  endpoint.microbatch = 0;
  endpoint.segmentCount = operation.maxSegments;
  endpoint.manifestDigest = sha('5');
  endpoint.securityProfile = "NDNSF_DATA_V1";
  endpoint.noProgressDeadlineMs = capability.noProgressMs;
  endpoint.hardDeadlineMs = capability.hardDeadlineMs;
  {
    auto identity = nativeEndpointJson(endpoint);
    identity.erase("endpoint_digest"); identity.erase("consumer_role");
    endpoint.endpointDigest = nativePlanningDigest(nativeCanonicalJson(identity));
  }

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
  edge.executionObjectDigest = computeTensorExecutionObjectDigest(endpoint);
  edge.namingOperationStride = endpoint.namingOperationStride;
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
  edge.inlineFirstSegment = inlineDelivery;
  {
    std::lock_guard<std::mutex> lock(manifestNameMutex);
    expectedManifestUri = edge.manifestDataName;
  }

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
  const bool prefetch = mode.rfind("prefetch", 0) == 0 || mode == "inline-prefetch" || mode == "inline-cancel";
  const bool cancellation = mode == "cancel" || mode == "prefetch-cancel" || mode == "inline-cancel";
  const bool manual = legacy || mode == "hmac" || mode == "index" || mode == "packed-bounds" ||
    mode == "packed-operation-bounds" || mode == "packed-inflight-bounds";
  const bool compressed = mode.rfind("inline-compressed", 0) == 0;
  // Both URI profiles measured 7750 as 8577 standalone/8766 inline wire bytes.
  // Adding 100 leaves 123 bytes standalone headroom and crosses the signed
  // inline limit by 66; the actual wire assertions below verify the boundary.
  const std::size_t chunkSize = legacy ? 7 : mode == "inline-oversize" ? 7850 : 7000;
  TensorBundle original;
  original.name = endpoint.tensorId;
  original.payload.resize(mode == "inline-oversize" ? chunkSize + 17 :
    mode == "inline-single" ? 5994 : (prefetch || boundedWindow) ? 12 * 7000 + 17 :
    mode.empty() ? 200 * 7000 + 17 : legacy ? 19 : 7017);
  for (std::size_t index = 0; index < original.payload.size(); ++index) {
    original.payload[index] = static_cast<std::uint8_t>(index * 31 + 7);
  }
  if (feedback) {
    NamedTensor token;
    token.name = "input_ids";
    token.elementType = TensorElementType::Int64;
    token.shape = {1, 1};
    const std::int64_t value = 7;
    token.payload.resize(sizeof(value));
    std::memcpy(token.payload.data(), &value, sizeof(value));
    original = makeEncodedTensorBundle(endpoint.tensorId, {std::move(token)});
  }
  if (compressed) {
    original = makeEncodedTensorBundle(endpoint.tensorId, {
      NamedTensor{"hidden", TensorElementType::Float32, {1, 4096},
                  std::vector<std::uint8_t>(4096 * sizeof(float), 0)}});
    BOOST_REQUIRE_GT(original.payload.size(), 2 * chunkSize);
  }
  if (mode == "packed-bounds" || mode == "inline-packed-opaque" ||
      mode == "packed-operation-bounds" || mode == "packed-inflight-bounds") {
    // The same bytes exercise explicit packed/opaque manifest semantics.
    // The packed header claims 64 MiB + 1, beyond its signed 8192-byte bound.
    original.payload = {'N', 'D', 'I', 'T', 'B', 'Z', '0', '1',
                        1, 0, 0, 4, 0, 0, 0, 0, 0x78};
  }
  NdnsfCollaborationDependencyIo producerIo(
      producerContext, 5000, chunkSize, 60000, producerCoordinator);
  NdnsfCollaborationDependencyIo consumerIo(
      consumerContext, 5000, chunkSize, 60000, consumerCoordinator);
  cancelWindowFetch = [&consumerIo] { consumerIo.cancelPendingPrefetches(); };
  if (mode == "inline-packed-publish-bounds" || mode == "inline-packed-publish-inflight") {
    const auto rawBudget = mode == "inline-packed-publish-bounds" ?
      operation.maxBytes : capability.maxInflightBytes;
    const auto oversized = makeEncodedTensorBundle("raw-budget", {
      NamedTensor{"mask", TensorElementType::UInt8, {static_cast<std::int64_t>(rawBudget)},
                  std::vector<std::uint8_t>(rawBudget, 0)}});
    BOOST_REQUIRE_GT(oversized.payload.size(), rawBudget);
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", edge, oversized), std::length_error);
    BOOST_CHECK_EQUAL(packets, 0U);
    BOOST_CHECK(uniquePackets.empty());
    // The ordinary publication below must still reuse this endpoint safely.
  }
  if (mode == "admission") {
    std::vector<DependencyEdge> invalid;
    auto bad = edge; bad.declaredByV3 = false; invalid.push_back(bad);
    bad = edge; bad.producerProvider += "/transport"; invalid.push_back(bad);
    bad = edge; bad.operationKind = "TOKEN_FEEDBACK"; invalid.push_back(bad);
    bad = edge; bad.producerRole.clear(); invalid.push_back(bad);
    bad = edge; bad.namingOperationStride = 3; invalid.push_back(bad);
    bad = edge;
    auto foreignCategory = producer.getName(); foreignCategory.append("NDNSF-DI").append("blob");
    bad.plannedDataName = foreignCategory.toUri(ndn::name::UriFormat::CANONICAL);
    bad.manifestDataName = foreignCategory.append("MANIFEST").toUri(ndn::name::UriFormat::CANONICAL);
    invalid.push_back(bad);
    bad = edge;
    auto foreignKey = ndn::Name(edge.plannedDataName).getPrefix(-1);
    foreignKey.append(makeNamingDigestComponent(std::string(64, '9')));
    bad.plannedDataName = foreignKey.toUri(ndn::name::UriFormat::CANONICAL);
    bad.manifestDataName = foreignKey.append("MANIFEST").toUri(ndn::name::UriFormat::CANONICAL);
    invalid.push_back(bad);
    for (const auto& rejected : invalid) {
      BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", rejected, original), std::exception);
      BOOST_CHECK_THROW(consumerIo.prefetchInput("session-v3", rejected).get(), std::exception);
      BOOST_CHECK_EQUAL(manifestInterests.load(), 0U);
      BOOST_CHECK_EQUAL(segmentInterests, 0U);
      BOOST_CHECK_EQUAL(packets, 0U);
      BOOST_CHECK(uniquePackets.empty());
    }
  }
  if (mode == "quota") {
    TensorBundle oversized;
    oversized.name = original.name;
    oversized.payload.resize(64U * 1024U * 1024U + 1);
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", edge, oversized), std::length_error);
    BOOST_CHECK_EQUAL(packets, 0U);
    BOOST_CHECK(uniquePackets.empty());
    // The failed reservation must leave this same endpoint publishable.
  }
  const auto expectedSegments = compressed ? 1 :
    1 + (original.payload.size() - 1) / chunkSize;
  if (prefetch) {
    edge.prefetchBundleBytes = original.payload.size();
    edge.prefetchSegmentBytes = chunkSize;
    if (mode == "prefetch-count") edge.prefetchBundleBytes += chunkSize;
    if (mode == "prefetch-bytes") --edge.prefetchBundleBytes;
    if (mode == "prefetch-chunk") --edge.prefetchSegmentBytes;
  }
  if (cancellation) {
    // Leave the authorized exact manifest absent so cancellation must stop a
    // real production adapter fetch rather than a completed/local fast path.
  }
  else if (manual) {
    // Produce the old representation with the maintained legacy encoders.
    // Consumer verification and restoration still use the real DependencyIo.
    std::vector<ProviderGroupBytes> chunks;
    std::vector<std::string> names;
    for (std::size_t offset = 0; offset < original.payload.size(); offset += chunkSize) {
      const auto end = std::min(original.payload.size(), offset + chunkSize);
      chunks.emplace_back(original.payload.begin() + offset, original.payload.begin() + end);
      auto name = ndn::Name(edge.manifestDataName).getPrefix(-1);
      names.push_back(name.append("DATA").appendSegment(names.size()).toUri(ndn::name::UriFormat::CANONICAL));
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
    manifest.segmentSize = std::min(chunkSize, original.payload.size());
    manifest.segmentCount = chunks.size();
    if (mode == "packed-bounds") manifest.unpackedBytes = 8192;
    if (mode == "packed-operation-bounds") manifest.unpackedBytes = operation.maxBytes + 1;
    if (mode == "packed-inflight-bounds") manifest.unpackedBytes = capability.maxInflightBytes + 1;
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
  else if (mode != "prefetch-renewals") {
    auto publishEdge = edge;
    if (mode == "inline-fallback") publishEdge.inlineFirstSegment = false;
    BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", publishEdge, original));
  }
  if (mode == "context") edge.endpointDigest = sha('9');

  if (cancellation) {
    const auto roleName = endpoint.consumerRole;
    NativeProviderRuntime runtime(1);
    auto runner = makeNativeModelRunner(
      [] (const RoleExecutionContext&) {
        NamedTensor logits;
        logits.name = "logits";
        logits.elementType = TensorElementType::Float32;
        logits.shape = {1, 2};
        const float values[] = {0.0F, 5.0F};
        logits.payload.resize(sizeof(values));
        std::memcpy(logits.payload.data(), values, sizeof(values));
        return std::map<std::string, TensorBundle>{
          {"onnx-output-bundle",
           makeEncodedTensorBundle("onnx-output-bundle", {std::move(logits)})}};
      });
    runtime.registerRunner(roleName, runner);

    NativeExecutionPlan plan;
    plan.roles = {roleName};
    NativeProviderAssignment assignment;
    assignment.providerByRole[roleName] = consumerContext.localProvider().toUri();
    auto coordinatorIo = std::make_shared<NdnsfCollaborationDependencyIo>(
      consumerContext, 5000, chunkSize, 60000, consumerCoordinator);
    NativeEpochCoordinatorConfig config{
      runtime, plan, assignment, coordinatorIo};
    config.sessionId = "session-v3-coordinator";
    config.requestId = requestId.toUri();
    config.lineagePlanDigest = planDigest;
    config.localProvider = consumerContext.localProvider().toUri();
    config.role = roleName;
    DecodeStateIdentityV1 stateIdentity;
    stateIdentity.modelDigest = sha('1');
    stateIdentity.graphSemanticDigest = sha('2');
    stateIdentity.artifactDigest = sha('3');
    stateIdentity.adapterDigest = sha('4');
    stateIdentity.tokenizerDigest = sha('5');
    stateIdentity.runnerDigest = sha('6');
    stateIdentity.roleName = roleName;
    stateIdentity.roleSplitDigest = sha('7');
    stateIdentity.layerBegin = 0;
    stateIdentity.layerEnd = 1;
    stateIdentity.prefixDigest = sha('8');
    stateIdentity.prefixTokenCount = 1;
    stateIdentity.positionDigest = sha('9');
    stateIdentity.precision = "fp32";
    stateIdentity.layoutDigest = sha('a');
    stateIdentity.stateSchemaDigest = sha('b');
    stateIdentity.stateComponentDigests = {sha('c')};
    stateIdentity.runtimeAbiDigest = sha('d');
    stateIdentity.securityDomainDigest = sha('e');
    stateIdentity.providerIdentity = consumerContext.localProvider().toUri();
    stateIdentity.providerBootId = "fixture-boot";
    stateIdentity.requestId = requestId.toUri();
    stateIdentity.generationId = "fixture-generation";
    stateIdentity.validate();
    config.positionPolicyDigest = stateIdentity.positionDigest;
    config.stateIdentityTemplate = std::move(stateIdentity);
    config.roleSpecFactory = [edge, roleName] (std::size_t epoch) {
      RoleSpec role;
      role.role = roleName;
      if (epoch > 0) {
        role.inputs = {edge};
      }
      DependencyEdge feedback;
      feedback.scope = "token-feedback";
      feedback.producerRole = roleName;
      feedback.consumerRole = roleName;
      feedback.operationKind = "TOKEN_FEEDBACK";
      feedback.tensors = {"token-feedback"};
      role.outputs = {feedback};
      return role;
    };
    config.terminalRole = true;
    config.maxEpochs = 2;
    NamedTensor tokenIds;
    tokenIds.name = "input_ids";
    tokenIds.elementType = TensorElementType::Int64;
    tokenIds.shape = {1, 1};
    const std::int64_t initialToken = 1;
    tokenIds.payload.resize(sizeof(initialToken));
    std::memcpy(tokenIds.payload.data(), &initialToken, sizeof(initialToken));
    config.initialInputs.emplace(
      "input_ids", makeEncodedTensorBundle("prompt", {std::move(tokenIds)}));
    std::atomic<std::size_t> interestCountAtEvent{0};
    config.eventSink = [&] (const std::vector<std::uint8_t>&) {
      interestCountAtEvent.store(
        manifestInterests.load(std::memory_order_acquire),
        std::memory_order_release);
      return false;
    };
    const auto pendingInterestsBefore = consumerFace.getNPendingInterests();
    auto pending = std::async(std::launch::async, [&config] {
      return runNativeEpochCoordinator(std::move(config));
    });
    environment.pumpUntil([&] {
      return pending.wait_for(0ms) == std::future_status::ready;
    });
    std::string error;
    try {
      (void)pending.get();
    }
    catch (const std::exception& caught) {
      error = caught.what();
    }
    BOOST_CHECK_EQUAL(error,
                      "native epoch token event admission was rejected");
    BOOST_CHECK_GE(manifestInterests.load(std::memory_order_acquire), 1U);
    BOOST_CHECK_GE(interestCountAtEvent.load(std::memory_order_acquire), 1U);
    const auto cancelDeadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(100);
    // Drain all fixture Faces: unrelated bootstrap/control replies also need
    // their peers pumped. Keep the original deadline and exact leak assertion.
    environment.pumpUntil([&] {
      return consumerFace.getNPendingInterests() == pendingInterestsBefore ||
        std::chrono::steady_clock::now() >= cancelDeadline;
    });
    const auto observedPending = consumerFace.getNPendingInterests();
    probeCancelledSourcePending(consumerFace, environment.keyChain(),
                                profile.providerIdentity, issuedSourceInterests);
    BOOST_CHECK_EQUAL(observedPending, pendingInterestsBefore);
    return;
  }

  auto fetched = consumerIo.prefetchInput("session-v3", edge);
  if (mode == "prefetch-renewals") {
    environment.pumpUntil([&] { return earlyInterests.size() == expectedSegments + 1; });
    BOOST_REQUIRE_EQUAL(earlyInterests.size(), expectedSegments + 1);
    // Model repeated readiness renewals without a wall-clock sleep. Nonce
    // changes do not alter the Data name or its content authority.
    for (const auto& entry : earlyInterests) {
      for (unsigned renewal = 0; renewal < 32; ++renewal) {
        auto interest = entry.second;
        interest.refreshNonce();
        producerFace.receive(interest);
      }
    }
    bool drained = false;
    boost::asio::post(producerFace.getIoContext(), [&] { drained = true; });
    environment.pumpUntil([&] { return drained; });
    BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", edge, original));
  }
  environment.pumpUntil([&] {
    return fetched.wait_for(0ms) == std::future_status::ready;
  });
  if (windowCancellation) {
    BOOST_CHECK_THROW(fetched.get(), std::runtime_error);
    deliveryScheduler.cancelAllEvents();
    bool drained = false;
    boost::asio::post(consumerFace.getIoContext(), [&] { drained = true; });
    environment.pumpUntil([&] { return drained; });
    BOOST_CHECK(windowCancellationRequested);
    BOOST_CHECK_EQUAL(segmentInterests, 8U);
    BOOST_CHECK_LE(maximumUnansweredSegments, 8U);
    probeCancelledSourcePending(consumerFace, environment.keyChain(),
                                producer.getName(), issuedSourceInterests);
    return;
  }
  if (mode == "packed-operation-bounds" || mode == "packed-inflight-bounds") {
    BOOST_CHECK_EXCEPTION(fetched.get(), std::runtime_error, [] (const std::runtime_error& error) {
      return std::string(error.what()).find("does not match mustFetch authority") != std::string::npos;
    });
    if (mode == "packed-inflight-bounds") {
      BOOST_CHECK_EQUAL(segmentInterests, 0U);
    }
    else {
      // A capability budget large enough to isolate operation.maxBytes also
      // permits the existing bounded speculative first read. It must not
      // expand to another name after the invalid manifest arrives.
      BOOST_CHECK_LE(requestedSegments.size(), 1U);
      for (const auto& name : requestedSegments)
        BOOST_CHECK(name == ndn::Name(tensorObjectSegmentName(endpoint, 0)));
    }
    return;
  }
  if (!mode.empty() && !feedback && mode != "admission" && mode != "reuse" &&
      mode != "quota" && mode != "legacy" && mode != "packed-bounds" &&
      !prefetch && (!inlineDelivery || mode == "inline-corrupt")) {
    const auto expected = (mode == "ciphertext" || mode == "inline-corrupt") ? "ciphertext commitment mismatch" :
                          mode == "bounds" ? "does not match mustFetch authority" :
                          mode == "hmac" ? "HMAC verification failed" :
                          mode == "index" ? "index/rank/size mismatch" :
                          mode == "legacy-binding" ? "inner manifest mismatch" :
                                             "producer signature mismatch";
    if (mode == "context") {
      BOOST_CHECK_EXCEPTION(fetched.get(), std::invalid_argument,
        [](const std::invalid_argument& error) {
          return std::string(error.what()) ==
            "C04 tensor readable fields or commitment mismatch";
        });
      BOOST_CHECK_EQUAL(manifestInterests.load(), 0U);
      BOOST_CHECK_EQUAL(segmentInterests, 0U);
    }
    else {
    BOOST_CHECK_EXCEPTION(fetched.get(), std::runtime_error,
      [&] (const std::runtime_error& error) {
        BOOST_TEST_MESSAGE("Actual rejection: " << error.what());
        return std::string(error.what()).find(expected) != std::string::npos;
      });
    }
    if (mode != "context" && !manual) BOOST_CHECK_EQUAL(mutations, 1U);
    if (mode == "bounds") {
      if (!enableSpeculation) {
        BOOST_CHECK_EQUAL(segmentInterests, 0U);
      }
      else {
        // The sealed deterministic first packet may overlap manifest fetch.
        // Invalid manifest bounds must prevent expansion to any other name;
        // the future still rejects instead of returning reconstructed bytes.
        BOOST_CHECK_LE(requestedSegments.size(), 1U);
        for (const auto& name : requestedSegments)
          BOOST_CHECK(name == ndn::Name(tensorObjectSegmentName(endpoint, 0)));
        BOOST_CHECK_LE(largestPacket, ndn::MAX_NDN_PACKET_SIZE);
      }
    }
    return;
  }
  if (mode == "inline-compressed-binding") {
    BOOST_CHECK_EXCEPTION(fetched.get(), std::runtime_error, [] (const std::runtime_error& error) {
      return std::string(error.what()).find("producer signature mismatch") != std::string::npos;
    });
    BOOST_CHECK_EQUAL(mutations, 1U);
    BOOST_CHECK_EQUAL(segmentInterests, 0U);
    return;
  }
  if (mode == "packed-bounds") {
    try {
      fetched.get();
      BOOST_FAIL("authenticated packed expansion was not rejected");
    }
    catch (const std::invalid_argument& error) {
      BOOST_CHECK_EQUAL(error.what(), "packed tensor bundle size exceeds bounds");
    }
    BOOST_CHECK_EQUAL(uniquePackets.size(), 2U);
    BOOST_CHECK_EQUAL(segmentInterests, 1U);
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
  if (boundedWindow) {
    BOOST_CHECK_EQUAL(expectedSegments, 13U);
    BOOST_CHECK_EQUAL(requestedSegments.size(), 12U);
    if (windowRefill) {
      BOOST_CHECK_MESSAGE(refilledBeforeHeldReply,
                          "segment9 waited for the slow first window to finish");
      deliveryScheduler.cancelAllEvents();
    }
    else {
      BOOST_CHECK_EQUAL(segmentInterests, 12U);
    }
    BOOST_CHECK_GT(maximumUnansweredSegments, 0U);
    BOOST_TEST_MESSAGE("SPEC216_TENSOR_WINDOW max_unanswered=" << maximumUnansweredSegments);
    BOOST_CHECK_LE(maximumUnansweredSegments, 8U);
    BOOST_CHECK(unansweredSegments.empty());
  }
  if (mode == "inline-compressed") {
    const auto firstWires = uniquePackets;
    BOOST_CHECK_EQUAL(firstWires.size(), 1U);
    BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", edge, original));
    auto changed = original;
    changed.payload.back() ^= 1;
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", edge, changed), std::invalid_argument);
    BOOST_CHECK(uniquePackets == firstWires);
  }
  if (mode == "reuse") {
    const auto firstWires = uniquePackets;
    BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", edge, original));
    auto anotherProjection = edge;
    anotherProjection.consumerRole = "S1R1";
    BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", anotherProjection, original));
    auto changed = original;
    changed.payload.back() ^= 1;
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", edge, changed), std::invalid_argument);
    auto changedProtection = edge;
    changedProtection.transportScope += "-changed";
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", changedProtection, original), std::invalid_argument);
    auto changedMetadata = edge;
    changedMetadata.consumerRoles.push_back("foreign-consumer");
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", changedMetadata, original), std::invalid_argument);
    auto reads = std::async(std::launch::async, [&] {
      for (const auto& item : firstWires) {
        const auto content = consumerContext.fetchSignedExactData(
          groupId, item.first, producer.getName(), 5000);
        if (!content) throw std::runtime_error("exact tensor retry original is absent");
      }
    });
    environment.pumpUntil([&] { return reads.wait_for(0ms) == std::future_status::ready; });
    BOOST_REQUIRE_NO_THROW(reads.get());
    BOOST_CHECK(uniquePackets == firstWires);
    BOOST_CHECK_GT(packets, firstWires.size());
    const auto& first = firstWires.rbegin()->second;
    auto conflicting = std::make_shared<ndn::Data>(ndn::Block(
      ndn::span<const std::uint8_t>(first.data(), first.size())));
    conflicting->setContent(ndn::Buffer{0x42});
    environment.keyChain().sign(*conflicting, ndn::security::signingByCertificate(
      producer.getSigningCertificateName()));
    ndn::nacabe::SPtrVector<ndn::Data> injected{conflicting}, noKeys;
    // Explicit local fault injection keeps both same-Name/different-digest
    // IMS entries. Arbitrary find(Name) cannot qualify immutable admission.
    BOOST_CHECK_THROW(producer.serveDataWithIMS(injected, noKeys), std::invalid_argument);
    ndn_service_framework::test::TensorOriginalOracle::poison(producer, *conflicting);
    const auto beforeRejectedReplay = packets;
    BOOST_CHECK_THROW(producerIo.publishOutput("session-v3", edge, original), std::invalid_argument);
    BOOST_CHECK_EQUAL(packets, beforeRejectedReplay);
    std::vector<std::pair<ndn::Name, ndn::Buffer>> originalObjects;
    for (const auto& item : firstWires) {
      const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(item.second.data(), item.second.size())));
      originalObjects.emplace_back(packet.getName(), ndn::Buffer(
        packet.getContent().value_begin(), packet.getContent().value_end()));
    }
    BOOST_CHECK_THROW(producerContext.publishSignedExactData(groupId, originalObjects, 60000), std::invalid_argument);
    BOOST_CHECK_EQUAL(packets, beforeRejectedReplay);
  }
  const bool expectInline = inlineDelivery && mode != "inline-fallback" && mode != "inline-oversize";
  BOOST_CHECK_EQUAL(uniquePackets.size(), expectedSegments + (expectInline ? 0U : 1U));
  if (mode == "inline-oversize") {
    BOOST_CHECK_EQUAL(requestedSegments.count(ndn::Name(tensorObjectSegmentName(endpoint, 0))), 1U);
    const ndn::Data rawManifest(ndn::Block(std::make_shared<const ndn::Buffer>(
      uniquePackets.at(ndn::Name(edge.manifestDataName)))));
    const ndn::Data rawFirst(ndn::Block(std::make_shared<const ndn::Buffer>(
      uniquePackets.at(ndn::Name(tensorObjectSegmentName(endpoint, 0))))));
    BOOST_CHECK_LE(rawFirst.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
    const auto& header = rawManifest.getContent();
    const auto& first = rawFirst.getContent();
    const std::vector<std::uint8_t> headerWire{header.value_begin(), header.value_end()};
    const auto plainManifest = decodeTensorManifestWithFirstSegment(headerWire, ndn::MAX_NDN_PACKET_SIZE);
    BOOST_CHECK(plainManifest.second.empty());
    BOOST_CHECK(plainManifest.first == headerWire);
    const auto candidate = encodeTensorManifestWithFirstSegment(
      {header.value(), header.value() + header.value_size()},
      {first.value(), first.value() + first.value_size()}, ndn::MAX_NDN_PACKET_SIZE);
    BOOST_REQUIRE_LE(candidate.size(), ndn::MAX_NDN_PACKET_SIZE);
    ndn::Data oversized(ndn::Name(edge.manifestDataName));
    oversized.setFreshnessPeriod(ndn::time::milliseconds(60000));
    oversized.setContent(ndn::Buffer(candidate.begin(), candidate.end()));
    environment.keyChain().sign(oversized, ndn::security::signingByCertificate(
      producer.getSigningCertificateName()));
    BOOST_TEST_MESSAGE("Actual inline boundary chunk=" << chunkSize
      << " wrapper=" << candidate.size()
      << " standalone_wire=" << rawFirst.wireEncode().size()
      << " candidate_wire=" << oversized.wireEncode().size());
    BOOST_CHECK_GT(oversized.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
  }
  if (expectInline) {
    BOOST_CHECK_EQUAL(requestedSegments.count(
      ndn::Name(tensorObjectSegmentName(endpoint, 0))), 0U);
    if (mode == "inline-single" || mode == "inline-compressed")
      BOOST_CHECK_EQUAL(segmentInterests, 0U);
    if (mode == "inline-prefetch") {
      BOOST_CHECK_EQUAL(expectedSegments, 13U);
      BOOST_CHECK_EQUAL(segmentNamesBeforeManifest, 12U);
      BOOST_CHECK_EQUAL(segmentInterests, 12U);
    }
  }
  if (feedback) {
    BOOST_CHECK_GT(manifestInterests.load(), 0U);
    BOOST_CHECK_GT(segmentInterests, 0U);
    BOOST_CHECK_EQUAL(requestedSegments.count(ndn::Name(tensorObjectSegmentName(endpoint, 0))), 1U);
    const auto binding = parseProducerDataName(ndn::Name(edge.plannedDataName), producer.getName());
    BOOST_REQUIRE(binding);
    BOOST_CHECK(binding->suffix.get(5) == makeNamingDecimalComponent(mode == "feedback-next" ? 1 : 0));
    BOOST_CHECK(binding->suffix.get(11) == makeNamingDecimalComponent(2));
    BOOST_CHECK(binding->suffix.get(7) == makeNamingLabelComponent("S1R0"));
  }
  BOOST_CHECK_GE(packets, uniquePackets.size());
  if (mode == "prefetch-renewals") BOOST_CHECK_EQUAL(packets, expectedSegments + 1);
  BOOST_CHECK_LE(largestPacket, ndn::MAX_NDN_PACKET_SIZE);
  if (mode == "prefetch" || mode == "prefetch-bytes" || mode == "prefetch-chunk") {
    BOOST_CHECK_EQUAL(expectedSegments, 13U);
    BOOST_CHECK_EQUAL(segmentNamesBeforeManifest, expectedSegments);
    BOOST_CHECK_EQUAL(segmentInterests, expectedSegments);
  }
  if (mode == "prefetch-budget")
    BOOST_CHECK_LE(segmentNamesBeforeManifest, 1U);
  BOOST_TEST_MESSAGE("Actual signed packets: " << packets << ", largest bytes: " << largestPacket);
}

BOOST_AUTO_TEST_CASE(LargeTensorUsesBoundedSignedPackets) { runTransfer(""); }
BOOST_AUTO_TEST_CASE(ExactTensorRetryReusesOriginalSignedPacketsAndRejectsConflicts) { runTransfer("reuse"); }
BOOST_AUTO_TEST_CASE(TensorOriginalQuotaRejectsBeforePacketsAndLeavesEndpointReusable) { runTransfer("quota"); }
BOOST_AUTO_TEST_CASE(InlineManifestDeliversSingleSegmentWithoutSegmentInterest) { runTransfer("inline-single"); }
BOOST_AUTO_TEST_CASE(LosslessPackingReducesC04PacketsAndPreservesOriginalTensorWire)
{
  runTransfer("inline-compressed");
}

BOOST_AUTO_TEST_CASE(AuthenticatedPackedPayloadRejectsOversizedExpansion)
{
  runTransfer("packed-bounds");
}

BOOST_AUTO_TEST_CASE(PackedExpansionSizeIsBoundByProducerSignature)
{
  runTransfer("inline-compressed-binding");
}

BOOST_AUTO_TEST_CASE(PackedExpansionCannotExceedSignedOperationBudget)
{
  runTransfer("packed-operation-bounds");
}

BOOST_AUTO_TEST_CASE(PackedExpansionCannotExceedSignedInflightBudget)
{
  runTransfer("packed-inflight-bounds", false);
}

BOOST_AUTO_TEST_CASE(PackingCannotBypassProducerRawOperationBudgetAndEndpointRecovers)
{
  runTransfer("inline-packed-publish-bounds");
}

BOOST_AUTO_TEST_CASE(PackingCannotBypassProducerRawInflightBudgetAndEndpointRecovers)
{
  runTransfer("inline-packed-publish-inflight", false);
}

BOOST_AUTO_TEST_CASE(OpaquePayloadWithPackedMagicRetainsRawManifestSemantics)
{
  runTransfer("inline-packed-opaque");
}

BOOST_AUTO_TEST_CASE(PackedTensorCodecPreservesBitsAndRawCompatibility)
{
  std::vector<std::uint8_t> floatBits{
    0, 0, 0, 0, 0, 0, 0, 0x80, 1, 0, 0xc0, 0x7f, 0, 0, 0x80, 0x7f};
  const std::vector<NamedTensor> tensors{
    {"mask", TensorElementType::Bool, {1, 8192}, std::vector<std::uint8_t>(8192, 0)},
    {"hidden", TensorElementType::Float32, {1, 4}, floatBits}};
  const auto raw = encodeTensorBundle(tensors);
  const auto alternative = tryPackTensorBundleForTransport(raw);
  BOOST_REQUIRE(alternative.has_value());
  const auto& packed = *alternative;
  BOOST_REQUIRE_LE(packed.size() + 256, raw.size());
  BOOST_CHECK(!isEncodedTensorBundle(packed));
  BOOST_CHECK(unpackTensorBundleFromTransport(packed) == raw);
  BOOST_CHECK(!tryPackTensorBundleForTransport(packed).has_value());
  const auto decoded = decodeTensorBundle(unpackTensorBundleFromTransport(packed));
  BOOST_REQUIRE_EQUAL(decoded.size(), tensors.size());
  for (std::size_t index = 0; index < tensors.size(); ++index) {
    BOOST_CHECK_EQUAL(decoded[index].name, tensors[index].name);
    BOOST_CHECK(decoded[index].elementType == tensors[index].elementType);
    BOOST_CHECK(decoded[index].shape == tensors[index].shape);
    BOOST_CHECK(decoded[index].payload == tensors[index].payload);
  }
  const auto small = encodeTensorBundle({
    {"input_ids", TensorElementType::Int64, {1, 1}, std::vector<std::uint8_t>(8, 0)}});
  BOOST_CHECK(!tryPackTensorBundleForTransport(small).has_value());
  const auto smallCompressible = encodeTensorBundle({
    {"feedback", TensorElementType::UInt8, {512}, std::vector<std::uint8_t>(512, 0)}});
  BOOST_REQUIRE_LT(smallCompressible.size(), 1024U);
  const auto smallPacked = tryPackTensorBundleForTransport(smallCompressible);
  BOOST_REQUIRE(smallPacked.has_value());
  BOOST_CHECK(unpackTensorBundleFromTransport(*smallPacked) == smallCompressible);
  std::mt19937 random(216);
  std::vector<std::uint8_t> randomBytes(8192);
  for (auto& byte : randomBytes) byte = static_cast<std::uint8_t>(random());
  const auto incompressible = encodeTensorBundle({
    {"hidden", TensorElementType::UInt8, {8192}, randomBytes}});
  BOOST_CHECK(!tryPackTensorBundleForTransport(incompressible).has_value());
  BOOST_CHECK(unpackTensorBundleFromTransport(incompressible) == incompressible);
  const std::vector<std::uint8_t> opaque(2048, 0x42);
  BOOST_CHECK(!tryPackTensorBundleForTransport(opaque).has_value());
  BOOST_CHECK(unpackTensorBundleFromTransport(opaque) == opaque);

  GenerationEpochLineageV1 lineage;
  lineage.requestId = "request-packed";
  lineage.attemptEpoch = 1;
  lineage.planDigest = "sha256:" + std::string(64, '1');
  lineage.generationId = "generation-packed";
  lineage.streamEpoch = 1;
  lineage.inferenceEpoch = 2;
  lineage.logicalPrefixDigest = "sha256:" + std::string(64, '2');
  lineage.logicalPrefixTokenCount = 5;
  lineage.positionDigest = "sha256:" + std::string(64, '3');
  lineage.producerRole = "/Stage/0";
  lineage.consumerRole = "/Stage/1";
  lineage.operationIndex = 9;
  const auto original = makeEncodedTensorBundle("activation", tensors);
  auto withLineage = attachGenerationEpochLineage(original, lineage);
  const auto originalWire = withLineage.payload;
  const auto packedLineage = tryPackTensorBundleForTransport(originalWire);
  BOOST_REQUIRE(packedLineage.has_value());
  withLineage.payload = unpackTensorBundleFromTransport(*packedLineage);
  BOOST_CHECK(withLineage.payload == originalWire);
  const auto restoredLineage = extractGenerationEpochLineage(withLineage);
  BOOST_REQUIRE(restoredLineage.has_value());
  BOOST_CHECK(*restoredLineage == lineage);
}

BOOST_AUTO_TEST_CASE(PackedTensorCodecRejectsCorruptionExpansionAndNestedFrames)
{
  const auto raw = encodeTensorBundle({
    {"mask", TensorElementType::Bool, {8192}, std::vector<std::uint8_t>(8192, 0)}});
  const auto alternative = tryPackTensorBundleForTransport(raw);
  BOOST_REQUIRE(alternative.has_value());
  const auto& packed = *alternative;
  BOOST_REQUIRE_LT(packed.size(), raw.size());
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(packed, raw.size() - 1),
                    std::invalid_argument);
  for (const auto size : {std::size_t(8), std::size_t(15), packed.size() - 1}) {
    const std::vector<std::uint8_t> truncated(packed.begin(), packed.begin() + size);
    BOOST_CHECK_THROW(unpackTensorBundleFromTransport(truncated), std::invalid_argument);
  }
  auto damaged = packed;
  damaged.back() ^= 1;
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(damaged), std::invalid_argument);
  auto trailing = packed;
  trailing.push_back(0);
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(trailing), std::invalid_argument);
  auto wrongSize = packed;
  wrongSize[8] ^= 1;
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(wrongSize), std::invalid_argument);
  auto oversized = packed;
  oversized[15] = 0x80;
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(oversized), std::invalid_argument);
  auto empty = packed;
  std::fill(empty.begin() + 8, empty.begin() + 16, 0);
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(empty), std::invalid_argument);
  // Independent zlib stream expands to NDITBZ01 and four zero bytes. It is
  // valid deflate but cannot be accepted as the required original NDITB001.
  const std::vector<std::uint8_t> nested{
    'N', 'D', 'I', 'T', 'B', 'Z', '0', '1', 12, 0, 0, 0, 0, 0, 0, 0,
    120, 1, 243, 115, 241, 12, 113, 138, 50, 48, 100, 0, 2, 0, 19, 9, 2, 45};
  BOOST_CHECK_THROW(unpackTensorBundleFromTransport(nested), std::invalid_argument);
}
BOOST_AUTO_TEST_CASE(InlineManifestRetainsRemainingSegmentPrefetch) { runTransfer("inline-prefetch"); }
BOOST_AUTO_TEST_CASE(PublishedSegmentBatchesBoundUnansweredInterests)
{
  runTransfer("inline-window");
}
BOOST_AUTO_TEST_CASE(PublishedSegmentWindowCancellationStopsLaterBatches)
{
  runTransfer("inline-window-cancel");
}
BOOST_AUTO_TEST_CASE(PublishedSegmentWindowRefillsBeforeSlowReply)
{
  runTransfer("inline-window-refill");
}
BOOST_AUTO_TEST_CASE(InlineManifestAcceptsPlainManifestFallback) { runTransfer("inline-fallback"); }
BOOST_AUTO_TEST_CASE(InlineManifestFallsBackOnActualSignedPacketBound) { runTransfer("inline-oversize"); }
BOOST_AUTO_TEST_CASE(InlineManifestCancellationStopsRemainingPrefetch) { runTransfer("inline-cancel"); }
BOOST_AUTO_TEST_CASE(InlineManifestRejectsResignedEmbeddedCiphertextChange) { runTransfer("inline-corrupt"); }
BOOST_AUTO_TEST_CASE(InlineManifestFramingRejectsTruncationAndBounds)
{
  const std::vector<std::uint8_t> manifest{1, 2, 3}, segment{4, 5, 6};
  auto wire = encodeTensorManifestWithFirstSegment(manifest, segment, 64);
  auto decoded = decodeTensorManifestWithFirstSegment(wire, 64);
  BOOST_CHECK(decoded.first == manifest);
  BOOST_CHECK(decoded.second == segment);
  BOOST_CHECK_THROW(encodeTensorManifestWithFirstSegment(manifest, segment, 21), std::invalid_argument);
  for (std::size_t count = 8; count < wire.size(); ++count) {
    const std::vector<std::uint8_t> truncated(wire.begin(), wire.begin() + count);
    BOOST_CHECK_THROW(decodeTensorManifestWithFirstSegment(truncated, 64), std::invalid_argument);
  }
  wire.push_back(0);
  BOOST_CHECK_THROW(decodeTensorManifestWithFirstSegment(wire, 64), std::invalid_argument);
  BOOST_CHECK_THROW(decodeTensorManifestWithFirstSegment(wire, 8), std::invalid_argument);
}
BOOST_AUTO_TEST_CASE(AllThirteenSegmentsAreRequestedBeforeManifest) { runTransfer("prefetch"); }
BOOST_AUTO_TEST_CASE(ReadinessRenewalsSendOneDataPerObject) { runTransfer("prefetch-renewals"); }
BOOST_AUTO_TEST_CASE(WrongPrefetchCountFallsBackToAuthenticatedManifest) { runTransfer("prefetch-count"); }
BOOST_AUTO_TEST_CASE(AdvisoryByteVariationReusesAuthenticatedPrefetch) { runTransfer("prefetch-bytes"); }
BOOST_AUTO_TEST_CASE(AdvisoryChunkVariationReusesAuthenticatedPrefetch) { runTransfer("prefetch-chunk"); }
BOOST_AUTO_TEST_CASE(PrefetchRespectsActualInflightBudget) { runTransfer("prefetch-budget"); }
BOOST_AUTO_TEST_CASE(CancellationStopsWholePrefetch) { runTransfer("prefetch-cancel"); }
BOOST_AUTO_TEST_CASE(LegacyRepresentationStillReconstructs) { runTransfer("legacy"); }
BOOST_AUTO_TEST_CASE(RejectsChangedProducerSignature) { runTransfer("signature"); }
BOOST_AUTO_TEST_CASE(RejectsChangedCiphertextCommitment) { runTransfer("ciphertext"); }
BOOST_AUTO_TEST_CASE(RejectsChangedAuthenticatedContext) { runTransfer("context"); }
BOOST_AUTO_TEST_CASE(RejectsBoundsBeforeFetchingSegments) { runTransfer("bounds", false); }
BOOST_AUTO_TEST_CASE(RejectsBoundsWithoutExpandingSpeculativeFetch) { runTransfer("bounds"); }

BOOST_AUTO_TEST_CASE(RejectsInnerHmacWithValidOuterManifest) { runTransfer("hmac"); }
BOOST_AUTO_TEST_CASE(RejectsSegmentIndexWithValidOuterManifest) { runTransfer("index"); }
BOOST_AUTO_TEST_CASE(LegacyFieldsAreCheckedBeforeRestoration) { runTransfer("legacy-binding"); }
BOOST_AUTO_TEST_CASE(PromptCancellationStopsExactManifestFetch) { runTransfer("cancel"); }

BOOST_AUTO_TEST_CASE(CrossRoleFeedbackUsesActualC04ManifestAndSegmentInterests) { runTransfer("feedback"); }
BOOST_AUTO_TEST_CASE(NextEpochFeedbackUsesSignedStrideAndActualC04Interests) { runTransfer("feedback-next"); }
BOOST_AUTO_TEST_CASE(CollectiveReferencesRejectInvalidOwnerKindKeyAndStrideBeforeIo) { runTransfer("admission"); }

BOOST_AUTO_TEST_CASE(CurrentLargeFetchRejectsMissingExpiredOrCancelledFenceBeforeIo)
{
  ndn_service_framework::test::BootstrapProfile profile;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == ndn_service_framework::test::EnvironmentStatus::Ready);
  auto& provider = environment.provider(0);
  PublishedContentName sourceIdentity;
  sourceIdentity.label = "fixed-assignment";
  const auto source = sourceIdentity.contentPrefix(profile.userIdentity, std::string(64, '1'))
    .append("PROTECTED").append(makeNamingDigestComponent(std::string(64, '2'))).append("DATA");
  BOOST_REQUIRE(ProtectedBlobManifest::inferDataProducer(source) == profile.userIdentity);
  std::size_t interests = 0;
  std::map<ndn::Name, ndn::Interest> issuedSourceInterests;
  ndn::signal::ScopedConnection connection = environment.providerFace(0).onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      if (source.isPrefixOf(interest.getName())) {
        ++interests;
        issuedSourceInterests.insert_or_assign(interest.getName(), interest);
      }
    });
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "fixed-assignment";
  assignment.service = profile.serviceName;
  const ndn::Name requestId("/request/fixed-assignment-fence");
  const auto future = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  ServiceProvider::CollaborationContext missing(
    provider, profile.userIdentity, requestId, RequestMessage(), assignment);
  BOOST_CHECK(!missing.fetchEncryptedLargeDataCurrent(source, future));
  assignment.workDeadline = future;
  assignment.workCurrent = [] { return false; };
  ServiceProvider::CollaborationContext cancelled(
    provider, profile.userIdentity, requestId, RequestMessage(), assignment);
  BOOST_CHECK(!cancelled.fetchEncryptedLargeDataCurrent(source, future));
  assignment.workCurrent = [] { return true; };
  assignment.workDeadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
  ServiceProvider::CollaborationContext expired(
    provider, profile.userIdentity, requestId, RequestMessage(), assignment);
  BOOST_CHECK(!expired.fetchEncryptedLargeDataCurrent(source, future));
  assignment.workDeadline = future;
  ServiceProvider::CollaborationContext callerExpired(
    provider, profile.userIdentity, requestId, RequestMessage(), assignment);
  BOOST_CHECK(!callerExpired.fetchEncryptedLargeDataCurrent(
    source, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)));
  bool drained = false;
  boost::asio::post(environment.providerFace(0).getIoContext(), [&] { drained = true; });
  environment.pumpUntil([&] { return drained; });
  BOOST_REQUIRE(drained);
  BOOST_CHECK_EQUAL(interests, 0U);

  // A non-publisher collaboration role receives the same streamed Request,
  // but has no User-facing stream lifecycle. Its current dispatch still may
  // fetch a fixed source; the missing source below bounds the network probe.
  StreamRequestOptions options;
  options.generationId.fill(1);
  options.streamEpoch = 1;
  options.eventKeyCommitment.fill(2);
  options.deadlineEpochMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
  RequestMessage streamedRequest;
  streamedRequest.setStreamRequestOptions(options);
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  ServiceProvider::CollaborationContext nonPublisher(
    provider, profile.userIdentity, requestId, streamedRequest, assignment);
  auto fetch = std::async(std::launch::async, [&] {
    return nonPublisher.fetchEncryptedLargeDataCurrent(source, assignment.workDeadline);
  });
  environment.pumpUntil([&] {
    return fetch.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(!fetch.get());
  BOOST_CHECK_GT(interests, 0U);

  struct LateAdmission {
    std::atomic<unsigned> checks{0};
    std::mutex mutex;
    std::condition_variable ready;
    bool posted = false;
  };
  const auto late = std::make_shared<LateAdmission>();
  assignment.workCurrent = [late] {
    // Context admission is the first call. Without pumping the Face, the
    // second call is the worker wait after it has posted the start closure.
    if (++late->checks == 2) {
      {
        std::lock_guard<std::mutex> lock(late->mutex);
        late->posted = true;
      }
      late->ready.notify_one();
      return false;
    }
    return true;
  };
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  ServiceProvider::CollaborationContext lateStart(
    provider, profile.userIdentity, requestId, streamedRequest, assignment);
  const auto beforeLate = interests;
  auto lateFetch = std::async(std::launch::async, [&] {
    return lateStart.fetchEncryptedLargeDataCurrent(source, assignment.workDeadline);
  });
  bool observedPosted = false;
  {
    std::unique_lock<std::mutex> lock(late->mutex);
    observedPosted = late->ready.wait_for(lock, std::chrono::seconds(1), [late] { return late->posted; });
  }
  environment.pumpUntil([&] {
    return lateFetch.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(!lateFetch.get());
  BOOST_REQUIRE(observedPosted);
  BOOST_CHECK_EQUAL(late->checks.load(), 2U);
  BOOST_CHECK_EQUAL(interests, beforeLate);
  probeCancelledSourcePending(environment.providerFace(0), environment.keyChain(),
                              profile.userIdentity, issuedSourceInterests);

  // Shared Faces also carry SVS/bootstrap reads. Keep an independent read
  // pending to prove source cancellation neither counts nor cancels it.
  const auto unrelatedName = ndn::Name(requestId).append("unrelated-background");
  bool unrelatedIssued = false;
  bool unrelatedReceived = false;
  ndn::signal::ScopedConnection observeUnrelated =
    environment.providerFace(0).onSendInterest.connect([&](const ndn::Interest& interest) {
      if (interest.getName() == unrelatedName) unrelatedIssued = true;
    });
  ndn::Interest unrelatedInterest(unrelatedName);
  unrelatedInterest.setInterestLifetime(ndn::time::seconds(5));
  auto unrelated = environment.providerFace(0).expressInterest(
    unrelatedInterest,
    [&](const ndn::Interest&, const ndn::Data&) { unrelatedReceived = true; },
    [](const ndn::Interest&, const ndn::lp::Nack&) {},
    [](const ndn::Interest&) {});
  environment.pumpUntil([&] { return unrelatedIssued; });
  BOOST_REQUIRE(unrelatedIssued);

  const auto current = std::make_shared<std::atomic<bool>>(true);
  assignment.workCurrent = [current] { return current->load(); };
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  ServiceProvider::CollaborationContext startedCancel(
    provider, profile.userIdentity, requestId, streamedRequest, assignment);
  ndn::signal::ScopedConnection cancelAfterStart = environment.providerFace(0).onSendInterest.connect(
    [current, source](const ndn::Interest& interest) {
      if (source.isPrefixOf(interest.getName())) current->store(false);
    });
  const auto beforeCancel = interests;
  auto cancelledFetch = std::async(std::launch::async, [&] {
    return startedCancel.fetchEncryptedLargeDataCurrent(source, assignment.workDeadline);
  });
  environment.pumpUntil([&] {
    return cancelledFetch.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(!cancelledFetch.get());
  BOOST_CHECK_GT(interests, beforeCancel);
  BOOST_CHECK(!current->load());
  probeCancelledSourcePending(environment.providerFace(0), environment.keyChain(),
                              profile.userIdentity, issuedSourceInterests);
  ndn::Data unrelatedData(unrelatedName);
  environment.keyChain().sign(unrelatedData,
    ndn::security::signingByIdentity(profile.userIdentity));
  environment.providerFace(0).receive(unrelatedData);
  BOOST_CHECK(unrelatedReceived);
  unrelated.cancel();
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di::tests
