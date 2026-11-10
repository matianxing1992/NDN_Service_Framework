#include "tests/boost-test.hpp"
#include "tests/unit-tests/noncanonical-tlv-fixture.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/collaboration-input.hpp"
#include "ndn-service-framework/dependency-transfer.hpp"
#include "ndn-service-framework/xor-repair.hpp"
#include "ndn-service-framework/detail/dependency-repair-packet.hpp"
#include "ndn-service-framework/detail/finite-packet-layout.hpp"
#include "ndn-service-framework/detail/finite-packet-recovery.hpp"
#include "ndn-service-framework/detail/exact-data-window.hpp"
#include "ndn-service-framework/detail/collaboration-input-budget.hpp"
#include "ndn-service-framework/exact-data-congestion-window.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>

namespace ndn_service_framework::test {

struct Spec226RecoveryOracle
{
  static bool publish(ServiceProvider::CollaborationContext& context,
                      const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
                      std::vector<std::shared_ptr<const ndn::Data>>& originals,
                      CollaborationTransferMetrics& metrics)
  {
    return context.publishOwnedExactData("input", objects, 60000, &metrics, originals, 65536);
  }
  static bool replay(ServiceProvider::CollaborationContext& context,
                     const std::vector<std::shared_ptr<const ndn::Data>>& originals,
                     CollaborationTransferMetrics& metrics)
  {
    return context.replayOwnedExactData("input", originals, &metrics);
  }
  template<typename Recovery, typename Ready>
  static auto withRepairs(ServiceProvider::CollaborationContext& context,
                          const std::vector<ndn::Name>& names, const ndn::Name& producer,
                          std::vector<std::size_t> optionalSlots, std::size_t capacity,
                          Recovery recovery, Ready ready)
  {
    return context.startVerifiedSignedExactDataBatchImpl(
      "input", {}, names, producer, 2000, {}, {},
      ServiceProvider::ExactDataFetchPhase::Published, {}, capacity, 64 * 1024,
      {}, std::move(recovery), false, std::move(optionalSlots), std::move(ready));
  }
  static auto bootstrap(ServiceProvider::CollaborationContext& context,
                        const ndn::Name& locator, const ndn::Name& producer)
  {
    return context.startVerifiedSignedExactDataBatchImpl(
      "input", {}, {locator}, producer, 2000, {}, {},
      ServiceProvider::ExactDataFetchPhase::Published, {}, 1,
      128 * ndn::MAX_NDN_PACKET_SIZE, {}, {}, true);
  }
  template<typename Recovery>
  static auto start(ServiceProvider::CollaborationContext& context,
                    std::string keyScope,
                    const std::vector<ndn::Name>& names,
                    const ndn::Name& producer,
                    int timeoutMs,
                    ServiceProvider::ExactDataFetchPhase phase,
                    std::function<bool()> current,
                    size_t maxOutstanding,
                    size_t maxWireBytes,
                    Recovery recovery)
  {
    return context.startVerifiedSignedExactDataBatchImpl(
      std::move(keyScope), {}, names, producer, timeoutMs, {}, {}, phase,
      std::move(current), maxOutstanding, maxWireBytes, {},
      std::move(recovery));
  }
};

namespace {
struct FiniteRepairFixture
{
  ndn::Name producer{"/spec226/producer/site/node"};
  ndn::Name rootName;
  std::string rootWireSha256;
  std::vector<detail::DependencyRepairGroup> groups;
  std::vector<ndn::Buffer> originals;
  std::vector<ndn::Buffer> parity;
};

FiniteRepairFixture makeFiniteRepairFixture(size_t count = 9, bool maximumFirst = false)
{
  FiniteRepairFixture fixture;
  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  fixture.rootName = ndn::Name(fixture.producer).append("NDNSF").append("OBJECT").appendVersion(1);
  ndn::Data root(fixture.rootName);
  root.setContent(ndn::Buffer{1});
  keyChain.sign(root, ndn::security::signingWithSha256());
  const auto& rootWire = root.wireEncode();
  fixture.rootWireSha256 = detail::dependencyWireDigest(ndn::span<const uint8_t>(rootWire.data(), rootWire.size()));
  for (size_t i = 0; i < count; ++i) {
    ndn::Data packet(ndn::Name(fixture.rootName).appendSegment(i));
    ndn::Buffer payload(7 + i * 31);
    std::fill(payload.begin(), payload.end(), static_cast<uint8_t>(i + 1));
    packet.setContent(payload);
    keyChain.sign(packet, ndn::security::signingWithSha256());
    if (maximumFirst && i == 0) {
      payload.resize(ndn::MAX_NDN_PACKET_SIZE);
      packet.setContent(payload);
      keyChain.sign(packet, ndn::security::signingWithSha256());
      const auto excess = packet.wireEncode().size() - ndn::MAX_NDN_PACKET_SIZE;
      payload.resize(payload.size() - excess);
      packet.setContent(payload);
      keyChain.sign(packet, ndn::security::signingWithSha256());
    }
    const auto& wire = packet.wireEncode();
    fixture.originals.emplace_back(wire.begin(), wire.end());
    if (i % 8 == 0) {
      detail::DependencyRepairGroup group;
      group.index = fixture.groups.size();
      group.repairRoot = ndn::Name(fixture.producer).append("NDNSF").append("DEPENDENCY-REPAIR")
        .append(fixture.rootWireSha256.substr(7)).append("fixture-policy").append("group").appendNumber(group.index);
      fixture.groups.push_back(std::move(group));
    }
    fixture.groups.back().members.push_back({packet.getName(), wire.size(),
      detail::dependencyWireDigest(fixture.originals.back())});
  }
  for (auto& group : fixture.groups) {
    size_t width = 0;
    for (const auto& member : group.members) width = std::max<uint64_t>(width, member.wireLength);
    ndn::Buffer parity(width);
    for (size_t i = 0; i < group.members.size(); ++i) {
      const auto& wire = fixture.originals.at(group.index * 8 + i);
      detail::xorInto(ndn::span<uint8_t>(parity.data(), parity.size()),
        ndn::span<const uint8_t>(wire.data(), wire.size()));
    }
    group.plaintextLength = width;
    group.plaintextSha256 = detail::dependencyWireDigest(parity);
    fixture.parity.push_back(std::move(parity));
  }
  return fixture;
}

detail::DependencyRepairPacketV1
makeRepairPacket(const FiniteRepairFixture& fixture)
{
  detail::DependencyRepairPacketV1 packet;
  packet.originalRootName = fixture.rootName;
  packet.rootWireSha256 = fixture.rootWireSha256;
  packet.group = fixture.groups.front();
  packet.codedBytes = fixture.parity.front();
  return packet;
}

enum class InBandOwnerCase {
  RECOVER, NO_REPAIR, MISSING_REPAIR, BAD_SIGNATURE, BAD_BODY, BAD_ORIGINAL_SIGNATURE,
  GROUP_SNAPSHOT, CANCEL_WORK, EXPIRE_AUTHORITY
};

void
runInBandOwnerCase(InBandOwnerCase mode, size_t capacity)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();
  const auto producer = environment.provider(0).getName();
  const auto root = ndn::Name(producer).append("NDNSF").append("IN-BAND").appendVersion(1);
  std::vector<ndn::Name> names;
  std::vector<std::shared_ptr<ndn::Data>> sources;
  std::vector<ndn::Buffer> codedWires;
  detail::DependencyRepairPacketV1 repair;
  repair.group.repairRoot = ndn::Name(root).append("FEC").appendNumber(0);
  const size_t memberCount = mode == InBandOwnerCase::GROUP_SNAPSHOT ? 8 : 4;
  const size_t sourceCount = mode == InBandOwnerCase::GROUP_SNAPSHOT ? 12 : 4;
  size_t width = 0;
  for (size_t i = 0; i < sourceCount; ++i) {
    names.push_back(ndn::Name(root).appendSegment(i));
    auto packet = std::make_shared<ndn::Data>(names.back());
    ndn::Buffer payload(i < memberCount ? 20 + i : 7000);
    std::fill(payload.begin(), payload.end(), static_cast<uint8_t>(i + 1));
    packet->setContent(payload);
    packet->setFinalBlock(ndn::name::Component::fromSegment(sourceCount - 1));
    environment.keyChain().sign(*packet, ndn::security::signingByIdentity(producer));
    ndn::Data fecSource(*packet);
    if (mode == InBandOwnerCase::BAD_ORIGINAL_SIGNATURE && i == 2) {
      payload[0] ^= 1;
      fecSource.setContent(payload);
    }
    const auto& wire = fecSource.wireEncode();
    if (i < memberCount) {
      width = std::max(width, wire.size());
      repair.group.members.push_back({packet->getName(), wire.size(),
        detail::dependencyWireDigest(ndn::span<const uint8_t>(wire.data(), wire.size()))});
      codedWires.emplace_back(wire.begin(), wire.end());
    }
    sources.push_back(std::move(packet));
  }
  repair.originalRootName = names.front();
  repair.rootWireSha256 = repair.group.members.front().wireSha256;
  repair.codedBytes.assign(width, 0);
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& wire = codedWires[i];
    detail::xorInto({repair.codedBytes.data(), repair.codedBytes.size()}, {wire.data(), wire.size()});
  }
  repair.group.plaintextLength = width;
  repair.group.plaintextSha256 = detail::dependencyWireDigest(repair.codedBytes);
  const auto body = repair.wireEncode(producer);
  ndn::Buffer bodyBytes(body.begin(), body.end());
  if (mode == InBandOwnerCase::BAD_BODY) bodyBytes.back() ^= 1;
  names.push_back(ndn::Name(repair.group.repairRoot).appendSegment(0));
  auto repairData = std::make_shared<ndn::Data>(names.back());
  repairData->setFinalBlock(ndn::name::Component::fromSegment(0));
  repairData->setContent(bodyBytes);
  environment.keyChain().sign(*repairData, ndn::security::signingByIdentity(producer));
  if (mode == InBandOwnerCase::BAD_SIGNATURE) {
    bodyBytes.back() ^= 1;
    repairData->setContent(bodyBytes);
  }
  ServiceProvider::CollaborationAssignment assignment;
  const auto current = std::make_shared<std::atomic<bool>>(true);
  const auto entered = std::make_shared<std::atomic<bool>>(false);
  const auto released = std::make_shared<std::atomic<bool>>(false);
  const auto exited = std::make_shared<std::atomic<bool>>(false);
  const bool controlled = mode == InBandOwnerCase::CANCEL_WORK ||
                          mode == InBandOwnerCase::EXPIRE_AUTHORITY;
  assignment.service = profile.serviceName;
  assignment.role = "/consumer";
  assignment.selectionDigest = "in-band-owner";
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.workCurrent = [current] { return current->load(); };
  assignment.workDeadline = std::chrono::steady_clock::now() + 3s;
  ServiceProvider::CollaborationContext context(environment.provider(1), profile.userIdentity,
    ndn::Name("/spec226/in-band-owner"), RequestMessage{}, assignment);
  auto& face = environment.providerFace(1);
  ndn::Scheduler scheduler(face.getIoContext());
  std::map<ndn::Name, size_t> sent;
  ndn::signal::ScopedConnection relay = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    const auto found = std::find(names.begin(), names.end(), interest.getName());
    if (found == names.end()) return;
    BOOST_CHECK(!interest.getCanBePrefix());
    const auto index = static_cast<size_t>(std::distance(names.begin(), found));
    const auto count = ++sent[*found];
    if (index == sourceCount) {
      if (mode == InBandOwnerCase::NO_REPAIR || mode == InBandOwnerCase::MISSING_REPAIR) return;
      if (mode == InBandOwnerCase::GROUP_SNAPSHOT)
        scheduler.schedule(ndn::time::milliseconds(10), [&] { face.receive(*repairData); });
      else face.receive(*repairData);
      if (mode == InBandOwnerCase::BAD_SIGNATURE)
        scheduler.schedule(ndn::time::milliseconds(10), [&] { face.receive(*sources.at(3)); });
      return;
    }
    if (index == 2 && (controlled || mode == InBandOwnerCase::RECOVER || mode == InBandOwnerCase::GROUP_SNAPSHOT ||
        ((mode == InBandOwnerCase::BAD_BODY || mode == InBandOwnerCase::BAD_ORIGINAL_SIGNATURE ||
          mode == InBandOwnerCase::MISSING_REPAIR) && count == 1))) return;
    if (index == 3 && mode == InBandOwnerCase::BAD_SIGNATURE) return;
    face.receive(*sources.at(index));
  });
  auto ready = [memberCount, sourceCount](size_t, const std::vector<ndn::Buffer>& wires,
                                        std::vector<size_t>& snapshot)
    -> std::optional<std::pair<size_t, size_t>> {
    if (wires.size() != sourceCount + 1 || wires.at(sourceCount).empty()) return std::nullopt;
    std::optional<size_t> missing;
    for (size_t i = 0; i < memberCount; ++i) {
      if (!wires[i].empty()) continue;
      if (missing) return std::nullopt;
      missing = i;
    }
    if (!missing) return std::nullopt;
    for (size_t i = 0; i < memberCount; ++i) snapshot.push_back(i);
    snapshot.push_back(sourceCount);
    return std::pair<size_t, size_t>{*missing, sourceCount};
  };
  auto recover = [producer, names, memberCount, sourceCount, controlled, entered, released, exited,
                  expectedRoot = repair.rootWireSha256](
      const ndn::Name&, const std::vector<ndn::Name>&, const std::vector<ndn::Buffer>& wires)
      -> std::optional<ndn::Buffer> {
    if (wires.size() != sourceCount + 1 || wires.at(sourceCount).empty()) return std::nullopt;
    const auto& repairWire = wires[sourceCount];
    const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(repairWire.data(), repairWire.size())));
    const auto decoded = detail::DependencyRepairPacketV1::wireDecode(
      packet.getContent().blockFromValue(), producer);
    if (decoded.originalRootName != names.front() || decoded.rootWireSha256 != expectedRoot ||
        decoded.group.members.size() != memberCount ||
        decoded.group.repairRoot != names.back().getPrefix(-1)) return std::nullopt;
    std::vector<std::optional<ndn::Buffer>> originals;
    for (size_t i = memberCount; i < sourceCount; ++i)
      if (!wires[i].empty()) throw std::runtime_error("recovery copied an unrelated source group");
    for (size_t i = 0; i < memberCount; ++i) {
      if (decoded.group.members[i].name != names[i]) return std::nullopt;
      if (wires[i].empty()) originals.emplace_back();
      else originals.emplace_back(wires[i]);
    }
    auto value = detail::recoverDependencyWire(decoded.group, originals, decoded.codedBytes);
    if (controlled) {
      entered->store(true);
      const auto limit = std::chrono::steady_clock::now() + 500ms;
      while (!released->load() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(1ms);
      exited->store(true);
    }
    return value;
  };
  auto operation = Spec226RecoveryOracle::withRepairs(context, names, producer,
    {sourceCount}, capacity, std::move(recover), std::move(ready));
  const auto end = std::chrono::steady_clock::now() + 3s;
  bool steered = false;
  while (operation.wait_for(0ms) != std::future_status::ready &&
         std::chrono::steady_clock::now() < end) {
    face.processEvents(ndn::time::milliseconds(5));
    face.getIoContext().restart();
    if (controlled && entered->load() && !steered) {
      if (mode == InBandOwnerCase::CANCEL_WORK) operation.cancel();
      else current->store(false);
      released->store(true);
      steered = true;
    }
  }
  BOOST_REQUIRE(operation.wait_for(0ms) == std::future_status::ready);
  const auto result = operation.get();
  if (controlled) {
    BOOST_REQUIRE(steered);
    BOOST_CHECK(!result);
    while (!exited->load() && std::chrono::steady_clock::now() < end) {
      face.processEvents(ndn::time::milliseconds(5));
      face.getIoContext().restart();
    }
    BOOST_REQUIRE(exited->load());
    face.processEvents(ndn::time::milliseconds(20));
    BOOST_CHECK_EQUAL(operation.metricsSnapshot().fecRecoveredPackets, 0);
    BOOST_CHECK_EQUAL(operation.metricsSnapshot().deadlineExpired, 0);
    return;
  }
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->object.packets().size(), sourceCount);
  for (size_t i = 0; i < sourceCount; ++i)
    BOOST_CHECK(result->object.packets()[i]->wireEncode() == sources[i]->wireEncode());
  size_t sourceInterests = 0;
  for (size_t i = 0; i < sourceCount; ++i) sourceInterests += sent[names[i]];
  BOOST_CHECK_EQUAL(result->metrics.originalInterests, sourceInterests);
  BOOST_CHECK_EQUAL(result->metrics.repairInterests, sent[names.back()]);
  const bool recovered = mode == InBandOwnerCase::RECOVER || mode == InBandOwnerCase::GROUP_SNAPSHOT;
  BOOST_CHECK_EQUAL(result->metrics.fecRecoveredPackets, recovered ? 1 : 0);
  const bool retried = mode == InBandOwnerCase::BAD_BODY ||
    mode == InBandOwnerCase::BAD_ORIGINAL_SIGNATURE || mode == InBandOwnerCase::MISSING_REPAIR;
  BOOST_CHECK_EQUAL(result->metrics.originalRetries, retried ? 1 : 0);
  if (mode == InBandOwnerCase::NO_REPAIR || mode == InBandOwnerCase::MISSING_REPAIR) {
    BOOST_CHECK_EQUAL(result->metrics.fecRecoveryAttempts, 0);
    BOOST_CHECK_EQUAL(result->metrics.repairWireBytes, 0);
  }
  else BOOST_CHECK_GT(result->metrics.repairWireBytes, 0);
  if (mode == InBandOwnerCase::BAD_SIGNATURE || mode == InBandOwnerCase::BAD_BODY ||
      mode == InBandOwnerCase::BAD_ORIGINAL_SIGNATURE)
    BOOST_CHECK_GT(result->metrics.fecRejectedRepairs, 0);
  if (mode == InBandOwnerCase::GROUP_SNAPSHOT)
    BOOST_CHECK_LE(result->metrics.peakRetainedBytes, 64 * 1024);
}
}
BOOST_AUTO_TEST_SUITE(Spec226DependencyRecovery)

BOOST_AUTO_TEST_CASE(ProductionExactPublicationAndAdmissionShareOneInBandPacketSet)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();
  auto& producer = environment.provider(0);
  auto& receiver = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& receiverFace = environment.providerFace(1);
  const auto producerName = producer.getName();
  const auto rootName = ndn::Name(producerName).append("NDNSF").append("IN-BAND").appendVersion(1);
  const ndn::Name request("/spec226/in-band/native");
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "/source";
  assignment.service = profile.serviceName;
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.selectionDigest = "in-band-native";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.dependencyTransport.redundancy = DependencyRedundancy::Xor;
  assignment.workDeadline = std::chrono::steady_clock::now() + 10s;
  assignment.workCurrent = [] { return true; };
  ServiceProvider::CollaborationContext publisher(producer, profile.userIdentity,
    request, RequestMessage{}, assignment);
  std::vector<std::pair<ndn::Name, ndn::Buffer>> objects;
  for (size_t i = 0; i < 4; ++i)
    objects.emplace_back(i == 0 ? rootName : ndn::Name(rootName).appendNumber(i),
                         ndn::Buffer{static_cast<uint8_t>(i), 9});
  std::vector<std::shared_ptr<const ndn::Data>> originals;
  CollaborationTransferMetrics published;
  BOOST_REQUIRE(Spec226RecoveryOracle::publish(publisher, objects, originals, published));
  BOOST_REQUIRE_EQUAL(originals.size(), 5);
  BOOST_CHECK_EQUAL(published.recoveryMode, "xor-packet-set-v1");
  BOOST_CHECK_GT(published.repairPublishedWireBytes, 0);
  BOOST_REQUIRE(published.wireBytes && published.transportPayloadBytes && published.metadataBytes);
  BOOST_CHECK_EQUAL(*published.wireBytes, *published.transportPayloadBytes +
    *published.metadataBytes + published.repairPublishedWireBytes);
  CollaborationTransferMetrics replayed;
  BOOST_REQUIRE(Spec226RecoveryOracle::replay(publisher, originals, replayed));
  BOOST_CHECK_EQUAL(replayed.recoveryMode, published.recoveryMode);
  BOOST_CHECK(replayed.wireBytes == published.wireBytes);
  BOOST_CHECK(replayed.transportPayloadBytes == published.transportPayloadBytes);
  BOOST_CHECK(replayed.metadataBytes == published.metadataBytes);
  BOOST_CHECK_EQUAL(replayed.repairPublishedWireBytes, published.repairPublishedWireBytes);
  DependencyPacketPlan plan;
  plan.rootName = rootName;
  plan.producer = producerName;
  plan.maxWireBytes = 8192;
  plan.maxPackets = 4;
  for (size_t i = 0; i < 4; ++i) plan.packets.push_back({originals[i]->getName(), {}, {}});
  for (bool useXor : {true, false}) {
    auto consumerAssignment = assignment;
    consumerAssignment.role = "/consumer";
    consumerAssignment.selectedRoleProviders["/source"] = {producerName};
    consumerAssignment.dependencyTransport.redundancy = useXor ?
      DependencyRedundancy::Xor : DependencyRedundancy::None;
    consumerAssignment.admissionLimits.emplace();
    consumerAssignment.admissionLimits->maxRetainedBytes = 512 * 1024;
    ServiceProvider::CollaborationContext consumer(receiver, profile.userIdentity,
      request, RequestMessage{}, consumerAssignment);
    std::map<ndn::Name, size_t> interests;
    ndn::signal::ScopedConnection interestRelay = receiverFace.onSendInterest.connect(
      [&](const ndn::Interest& interest) {
        if (!rootName.isPrefixOf(interest.getName())) return;
        ++interests[interest.getName()];
        producerFace.receive(interest);
      });
    ndn::signal::ScopedConnection dataRelay = producerFace.onSendData.connect(
      [&](const ndn::Data& data) {
        if (!rootName.isPrefixOf(data.getName())) return;
        if (useXor && data.getName() == objects[2].first) return;
        receiverFace.receive(data);
      });
    CollaborationInputSpec input;
    input.slotId = "finite-object";
    input.producerRole = "/source";
    input.object.name = rootName;
    input.object.producerName = producerName;
    input.object.serviceName = profile.serviceName;
    input.keyScope = "input";
    input.encoding = InputObjectEncoding::SignedExactPackets;
    input.digestKind = InputDigestKind::OrderedWireSha256;
    input.maxBytes = 8192;
    input.maxPackets = 4;
    auto operation = consumer.startInputAdmission({input}, [&](const auto& admitted) {
      if (admitted.size() != 1 || admitted.front().packets().size() != 4)
        throw std::runtime_error("FEC leaked into business packet set");
      for (size_t i = 0; i < 4; ++i)
        if (admitted.front().packets()[i]->wireEncode() != originals[i]->wireEncode())
          throw std::runtime_error("original source wire changed during recovery");
    }, [plan](const auto&, const auto&) { return plan; });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (operation.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      producerFace.processEvents(ndn::time::milliseconds(5));
      receiverFace.processEvents(ndn::time::milliseconds(5));
      producerFace.getIoContext().restart();
      receiverFace.getIoContext().restart();
    }
    BOOST_REQUIRE(operation.wait_for(0ms) == std::future_status::ready);
    const auto result = operation.get();
    BOOST_REQUIRE_MESSAGE(result.status == InputAdmissionStatus::Ready, result.reasonCode);
    BOOST_CHECK_EQUAL(result.metrics.fecRecoveredPackets, useXor ? 1 : 0);
    BOOST_CHECK_EQUAL(result.metrics.originalRetries, 0);
    BOOST_CHECK_EQUAL(result.metrics.repairInterests, useXor ? 1 : 0);
    BOOST_CHECK_EQUAL(interests[objects[2].first], 1);
    BOOST_CHECK_EQUAL(interests[originals.back()->getName()], useXor ? 1 : 0);
  }
}

BOOST_AUTO_TEST_CASE(PreparedPacketSetContainsFourSourcesAndOneInBandRepair)
{
  const ndn::Name producer("/site/producer");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("object").appendVersion(1);
  layout.sourceCount = 4;
  std::vector<ndn::Data> sources;
  for (size_t i = 0; i < 4; ++i) {
    sources.emplace_back(ndn::Name(layout.objectName).appendSegment(i));
    sources.back().setContent(ndn::Buffer{static_cast<uint8_t>(i), 2, 3});
  }
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto sign = [&keys](ndn::Data& data) {
    keys.sign(data, ndn::security::signingWithSha256());
  };
  const auto packets = detail::prepareFinitePacketSet(sources, layout, producer, sign, 65536, 7000);
  BOOST_REQUIRE_EQUAL(packets.size(), 5);
  const auto actual = detail::FinitePacketLayout::readFrom(*packets.front(), layout.objectName, producer);
  BOOST_REQUIRE(actual);
  BOOST_CHECK_EQUAL(actual->totalPacketCount(), 5);
  BOOST_CHECK(packets.back()->getName() == ndn::Name(layout.objectName).appendSegment(4));
  for (size_t i = 0; i < sources.size(); ++i) {
    BOOST_CHECK(packets[i]->getContent() == sources[i].getContent());
    BOOST_CHECK(packets[i]->getFinalBlock() == ndn::name::Component::fromSegment(4));
  }
  const auto& content = packets.back()->getContent();
  const auto repair = detail::DependencyRepairPacketV1::wireDecode(
    ndn::Block(ndn::span<const uint8_t>(content.value(), content.value_size())), producer);
  BOOST_CHECK(repair.group.repairRoot == packets.back()->getName());
  std::vector<std::optional<ndn::Buffer>> originals;
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto& wire = packets[i]->wireEncode();
    originals.push_back(i == 2 ? std::nullopt :
      std::optional<ndn::Buffer>(ndn::Buffer(wire.begin(), wire.end())));
  }
  const auto recovered = detail::recoverDependencyWire(repair.group, originals, repair.codedBytes);
  BOOST_REQUIRE(recovered);
  const auto& expected = packets[2]->wireEncode();
  BOOST_CHECK_EQUAL_COLLECTIONS(recovered->begin(), recovered->end(), expected.begin(), expected.end());
  BOOST_CHECK(!sources.front().getFinalBlock());
}

BOOST_AUTO_TEST_CASE(FiniteRecoveryInterleavesGroupsAndCopiesOnlyTheAffectedGroup)
{
  const ndn::Name producer("/site/producer");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("object").appendVersion(1);
  layout.sourceCount = 12;
  std::vector<ndn::Data> sources;
  std::vector<ndn::Name> sourceNames;
  for (size_t i = 0; i < 12; ++i) {
    sourceNames.push_back(ndn::Name(layout.objectName).appendSegment(i));
    sources.emplace_back(sourceNames.back());
    sources.back().setContent(ndn::Buffer{static_cast<uint8_t>(i), 4, 5});
  }
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto packets = detail::prepareFinitePacketSet(sources, layout, producer,
    [&keys](ndn::Data& data) { keys.sign(data, ndn::security::signingWithSha256()); }, 65536, 7000);
  const auto actual = detail::FinitePacketLayout::readFrom(*packets.front(), layout.objectName, producer);
  BOOST_REQUIRE(actual);
  detail::FinitePacketRecovery recovery(*actual, sourceNames,
    {{sourceNames.front(), packets.front()}}, producer);
  const auto& names = recovery.requestedNames();
  BOOST_REQUIRE_EQUAL(names.size(), 13);
  BOOST_REQUIRE_EQUAL(recovery.optionalSlots().size(), 2);
  BOOST_CHECK_EQUAL(recovery.optionalSlots().front(), 7);
  BOOST_CHECK(names[7] == actual->repairName(0, 0));
  BOOST_CHECK(names[8] == sourceNames[8]);
  std::vector<ndn::Buffer> wires(names.size());
  for (size_t i = 0; i < 8; ++i) {
    if (names[i] == sourceNames[2]) continue;
    const auto packet = std::find_if(packets.begin(), packets.end(),
      [&](const auto& candidate) { return candidate->getName() == names[i]; });
    BOOST_REQUIRE(packet != packets.end());
    const auto& wire = (*packet)->wireEncode();
    wires[i] = ndn::Buffer(wire.begin(), wire.end());
  }
  std::vector<size_t> snapshot;
  const auto ready = recovery.ready(7, wires, snapshot);
  BOOST_REQUIRE(ready);
  BOOST_CHECK_EQUAL(ready->first, 1);
  BOOST_CHECK_EQUAL(ready->second, 7);
  BOOST_REQUIRE_EQUAL(snapshot.size(), 8);
  BOOST_CHECK_EQUAL(snapshot.back(), 7);
  const auto restored = recovery.recover(sourceNames[2], names, wires);
  BOOST_REQUIRE(restored);
  const auto& expected = packets[2]->wireEncode();
  BOOST_CHECK_EQUAL_COLLECTIONS(restored->begin(), restored->end(), expected.begin(), expected.end());
  wires[2].clear();
  BOOST_CHECK(!recovery.ready(7, wires, snapshot));
  BOOST_CHECK(!recovery.recover(sourceNames[2], names, wires));
}

BOOST_AUTO_TEST_CASE(VariableSignerEnvelopeRestagesBeforeReturningThePacketSet)
{
  const ndn::Name producer("/site/producer");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("object").appendVersion(1);
  layout.sourceCount = 4;
  std::vector<ndn::Data> sources;
  for (size_t i = 0; i < 4; ++i) {
    sources.emplace_back(ndn::Name(layout.objectName).appendSegment(i));
    ndn::Buffer content(8000);
    std::fill(content.begin(), content.end(), static_cast<uint8_t>(i));
    sources.back().setContent(content);
  }
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto certificate = keys.createIdentity(producer).getDefaultKey().getDefaultCertificate();
  bool oversizedStagingPacket = false;
  const auto packets = detail::prepareFinitePacketSet(sources, layout, producer,
    [&](ndn::Data& packet) {
      if (packet.getName().at(-1).toSegment() == 65535)
        keys.sign(packet, ndn::security::signingWithSha256());
      else {
        keys.sign(packet, ndn::security::signingByIdentity(producer));
        oversizedStagingPacket |= packet.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE;
      }
    }, 65536, ndn::MAX_NDN_PACKET_SIZE);
  BOOST_CHECK(oversizedStagingPacket);
  BOOST_REQUIRE_GT(packets.size(), 5);
  for (const auto& packet : packets) {
    BOOST_CHECK_LE(packet->wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
    BOOST_CHECK(ndn::security::verifySignature(*packet, certificate));
  }
}

BOOST_AUTO_TEST_CASE(PreparedPacketSetFragmentsRepairAndRejectsCapacityBeforePublication)
{
  const ndn::Name producer("/site/producer");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("object").appendVersion(1);
  layout.sourceCount = 4;
  std::vector<ndn::Data> sources;
  for (size_t i = 0; i < 4; ++i) {
    sources.emplace_back(ndn::Name(layout.objectName).appendSegment(i));
    ndn::Buffer body(2000);
    std::fill(body.begin(), body.end(), static_cast<uint8_t>(i));
    sources.back().setContent(body);
  }
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto sign = [&keys](ndn::Data& data) {
    keys.sign(data, ndn::security::signingWithSha256());
  };
  const auto packets = detail::prepareFinitePacketSet(sources, layout, producer, sign, 65536, 1000);
  const auto actual = detail::FinitePacketLayout::readFrom(*packets.front(), layout.objectName, producer);
  BOOST_REQUIRE(actual);
  BOOST_REQUIRE_GT(actual->repairFragmentsPerGroup, 1);
  BOOST_CHECK_EQUAL(packets.size(), actual->totalPacketCount());
  ndn::Buffer encoded;
  for (size_t i = sources.size(); i < packets.size(); ++i) {
    BOOST_CHECK_LE(packets[i]->wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
    const auto& content = packets[i]->getContent();
    encoded.insert(encoded.end(), content.value_begin(), content.value_end());
  }
  const auto repair = detail::DependencyRepairPacketV1::wireDecode(
    ndn::Block(ndn::span<const uint8_t>(encoded.data(), encoded.size())), producer);
  BOOST_CHECK_EQUAL(repair.group.members.size(), 4);
  BOOST_CHECK_THROW(detail::prepareFinitePacketSet(sources, layout, producer, sign, 100, 1000),
                    std::invalid_argument);
  sources[1].setName(sources.front().getName());
  BOOST_CHECK_THROW(detail::prepareFinitePacketSet(sources, layout, producer, sign, 65536, 1000),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SegmentedLayoutNamesTheFifthPacketWithoutChangingBusinessContent)
{
  const ndn::Name producer("/producer/site");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("NDNSF").append("OBJECT").appendVersion(1);
  layout.sourceCount = 4;
  layout.repairFragmentsPerGroup = 1;
  BOOST_CHECK_EQUAL(layout.totalPacketCount(), 5);
  BOOST_CHECK(layout.repairName(0, 0) == ndn::Name(layout.objectName).appendSegment(4));
  ndn::Data root(ndn::Name(layout.objectName).appendSegment(0));
  root.setContent(ndn::Buffer{2, 3, 5});
  root.setFinalBlock(ndn::name::Component::fromSegment(3));
  const auto content = root.getContent();
  ndn::Block other(240);
  other.push_back(ndn::makeStringBlock(100, "application-owned"));
  other.encode();
  auto meta = root.getMetaInfo();
  meta.addAppMetaInfo(other);
  root.setMetaInfo(meta);
  layout.applyTo(root, producer);
  layout.applyTo(root, producer);
  BOOST_CHECK(root.getContent() == content);
  BOOST_CHECK(root.getFinalBlock() == ndn::name::Component::fromSegment(4));
  BOOST_REQUIRE_EQUAL(root.getMetaInfo().getAppMetaInfo().size(), 2);
  BOOST_CHECK(root.getMetaInfo().getAppMetaInfo().front() == other);
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  keys.sign(root, ndn::security::signingWithSha256());
  const ndn::Data received(root.wireEncode());
  const auto decoded = detail::FinitePacketLayout::readFrom(received, layout.objectName, producer);
  BOOST_REQUIRE(decoded);
  BOOST_CHECK_EQUAL(decoded->sourceCount, 4);
  BOOST_CHECK(decoded->repairName(0, 0) == layout.repairName(0, 0));
  BOOST_CHECK(received.getContent() == content);
}

BOOST_AUTO_TEST_CASE(NamedLayoutKeepsTheOriginalRootNameAndFinalBlock)
{
  const ndn::Name producer("/producer/site");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("NDNSF").append("TENSOR").append("manifest");
  layout.sourceCount = 9;
  layout.repairFragmentsPerGroup = 2;
  layout.framing = detail::FinitePacketLayout::Framing::NAMED_PACKETS;
  BOOST_CHECK_EQUAL(layout.totalPacketCount(), 13);
  BOOST_CHECK(layout.repairName(1, 1) ==
    ndn::Name(layout.objectName).append("FEC").appendNumber(1).appendSegment(1));
  ndn::Data root(layout.objectName);
  root.setContent(ndn::Buffer{1});
  const auto name = root.getName();
  layout.applyTo(root, producer);
  BOOST_CHECK(root.getName() == name);
  BOOST_CHECK(!root.getFinalBlock());
  BOOST_REQUIRE(detail::FinitePacketLayout::readFrom(root, layout.objectName, producer));
}

BOOST_AUTO_TEST_CASE(LayoutRejectsConflictingCountsDuplicatesForeignNamesAndWrongRoot)
{
  const ndn::Name producer("/producer/site");
  detail::FinitePacketLayout layout;
  layout.objectName = ndn::Name(producer).append("NDNSF").append("OBJECT").appendVersion(1);
  layout.sourceCount = 4;
  layout.repairFragmentsPerGroup = 1;
  BOOST_CHECK_THROW(layout.repairName(1, 0), std::invalid_argument);
  BOOST_CHECK_THROW(layout.repairName(0, 1), std::invalid_argument);
  BOOST_CHECK_THROW(layout.wireEncode(ndn::Name("/foreign")), std::invalid_argument);
  auto bad = layout;
  bad.sourceCount = 65536;
  BOOST_CHECK_THROW(bad.totalPacketCount(), std::invalid_argument);
  bad = layout;
  bad.repairFragmentsPerGroup = std::numeric_limits<uint64_t>::max();
  BOOST_CHECK_THROW(bad.totalPacketCount(), std::invalid_argument);
  ndn::Data root(ndn::Name(layout.objectName).appendSegment(0));
  root.setContent(ndn::Buffer{1});
  layout.applyTo(root, producer);
  BOOST_CHECK_THROW(detail::FinitePacketLayout::readFrom(root, ndn::Name("/other"), producer),
                    std::invalid_argument);
  root.setFinalBlock(ndn::name::Component::fromSegment(3));
  BOOST_CHECK_THROW(detail::FinitePacketLayout::readFrom(root, layout.objectName, producer),
                    std::invalid_argument);
  layout.applyTo(root, producer);
  auto meta = root.getMetaInfo();
  meta.addAppMetaInfo(layout.wireEncode(producer));
  root.setMetaInfo(meta);
  BOOST_CHECK_THROW(detail::FinitePacketLayout::readFrom(root, layout.objectName, producer),
                    std::invalid_argument);
  ndn::Data wrong(layout.objectName);
  wrong.setContent(ndn::Buffer{1});
  BOOST_CHECK_THROW(layout.applyTo(wrong, producer), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(InBandOwnerRecoversBeforeTheFirstSourceRetry)
{ runInBandOwnerCase(InBandOwnerCase::RECOVER, 8); }
BOOST_AUTO_TEST_CASE(InBandOwnerPreservesTheSmallWindowDuringRecovery)
{ runInBandOwnerCase(InBandOwnerCase::RECOVER, 2); }
BOOST_AUTO_TEST_CASE(InBandOwnerNeverWaitsForRepairWhenAllSourcesArrive)
{ runInBandOwnerCase(InBandOwnerCase::NO_REPAIR, 1); }
BOOST_AUTO_TEST_CASE(InBandOwnerDoesNotPoisonValidSourcesWithBadRepairSignature)
{ runInBandOwnerCase(InBandOwnerCase::BAD_SIGNATURE, 8); }
BOOST_AUTO_TEST_CASE(InBandOwnerMissingRepairStillUsesBoundedOriginalArq)
{ runInBandOwnerCase(InBandOwnerCase::MISSING_REPAIR, 8); }
BOOST_AUTO_TEST_CASE(InBandOwnerRejectsAnInvalidReconstructedOriginalSignature)
{ runInBandOwnerCase(InBandOwnerCase::BAD_ORIGINAL_SIGNATURE, 8); }
BOOST_AUTO_TEST_CASE(InBandOwnerResumesTheSameSourceSlotAfterBadRepair)
{ runInBandOwnerCase(InBandOwnerCase::BAD_BODY, 8); }
BOOST_AUTO_TEST_CASE(InBandOwnerSnapshotsOnlyTheRecoveringGroup)
{ runInBandOwnerCase(InBandOwnerCase::GROUP_SNAPSHOT, 32); }
BOOST_AUTO_TEST_CASE(InBandOwnerCancellationRejectsTheProcessingResult)
{ runInBandOwnerCase(InBandOwnerCase::CANCEL_WORK, 8); }
BOOST_AUTO_TEST_CASE(InBandOwnerCurrentFenceRejectsTheProcessingResult)
{ runInBandOwnerCase(InBandOwnerCase::EXPIRE_AUTHORITY, 8); }

BOOST_AUTO_TEST_CASE(InBandRepairCarriesFourSourceWiresInOneRepairPacket)
{
  auto fixture = makeFiniteRepairFixture(4);
  detail::DependencyRepairPacketV1 repair;
  repair.originalRootName = fixture.rootName;
  repair.rootWireSha256 = fixture.rootWireSha256;
  repair.group = fixture.groups.front();
  repair.group.repairRoot = ndn::Name(repair.originalRootName).append("FEC").appendNumber(0);
  repair.codedBytes = fixture.parity.front();
  ndn::Data packet(ndn::Name(repair.group.repairRoot).appendSegment(0));
  packet.setFinalBlock(ndn::name::Component::fromSegment(0));
  packet.setContent(repair.wireEncode(fixture.producer));
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  keys.sign(packet, ndn::security::signingWithSha256());
  BOOST_CHECK_LE(packet.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
  BOOST_CHECK(repair.originalRootName.isPrefixOf(packet.getName()));
  const auto decoded = detail::DependencyRepairPacketV1::wireDecode(
    packet.getContent().blockFromValue(), fixture.producer);
  BOOST_REQUIRE_EQUAL(decoded.group.members.size(), 4);
  std::vector<std::optional<ndn::Buffer>> sources;
  for (const auto& original : fixture.originals) sources.emplace_back(original);
  sources.at(2).reset();
  const auto recovered = detail::recoverDependencyWire(decoded.group, sources, decoded.codedBytes);
  BOOST_REQUIRE(recovered);
  BOOST_CHECK(*recovered == fixture.originals.at(2));
  const ndn::Data original(ndn::Block(ndn::span<const uint8_t>(
    recovered->data(), recovered->size())));
  BOOST_CHECK(original.getName() == decoded.group.members.at(2).name);
  sources.at(1).reset();
  BOOST_CHECK(!detail::recoverDependencyWire(decoded.group, sources, decoded.codedBytes));
}

BOOST_AUTO_TEST_CASE(InBandRepairRejectsMalformedMembersAndPayloadBeforeRecovery)
{
  auto fixture = makeFiniteRepairFixture(4);
  detail::DependencyRepairPacketV1 repair;
  repair.originalRootName = fixture.rootName;
  repair.rootWireSha256 = fixture.rootWireSha256;
  repair.group = fixture.groups.front();
  repair.group.repairRoot = ndn::Name(repair.originalRootName).append("FEC").appendNumber(0);
  repair.codedBytes = fixture.parity.front();
  auto bad = repair;
  bad.codedBytes.front() ^= 1;
  BOOST_CHECK_THROW(bad.wireEncode(fixture.producer), std::invalid_argument);
  bad = repair;
  bad.group.members.at(1).name = bad.group.members.front().name;
  BOOST_CHECK_THROW(bad.wireEncode(fixture.producer), std::invalid_argument);
  bad = repair;
  bad.group.members.front().name = ndn::Name("/foreign/producer/segment");
  BOOST_CHECK_THROW(bad.wireEncode(fixture.producer), std::invalid_argument);
  bad = repair;
  bad.group.members.front().name = bad.originalRootName;
  bad.group.members.front().wireSha256 = "sha256:" + std::string(64, '0');
  BOOST_CHECK_THROW(bad.wireEncode(fixture.producer), std::invalid_argument);
  auto extra = repair.wireEncode(fixture.producer);
  extra.push_back(ndn::makeNonNegativeIntegerBlock(0xF444, 1));
  extra.encode();
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(extra, fixture.producer),
                    std::invalid_argument);
  const auto encoded = repair.wireEncode(fixture.producer);
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(encoded, ndn::Name("/other")),
                    std::invalid_argument);
  detail::DependencyRepairLimits limit;
  limit.maxRepairBytes = encoded.size() - 1;
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(encoded, fixture.producer, limit),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(HybridBootstrapRetriesOnlyTheNamedProducer)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();
  const auto producer = environment.provider(0).getName();
  const auto attribute = makeAbeServiceAttribute(profile.serviceName, "SERVICE");
  const auto locator = makeHybridMessageKeyDataName(profile.serviceName, producer, attribute, "test");
  const auto payload = ndn::makeStringBlock(ndn::tlv::Content, std::string(12000, 'a'));
  const auto originals = prepareHybridMessageKeyPackets(profile.serviceName, producer, attribute,
    "test", {payload.data(), payload.size()}, environment.keyChain(),
    ndn::security::signingByIdentity(producer), ndn::time::seconds(60), ndn::tlv::ContentType_Blob);
  BOOST_REQUIRE_GT(originals.size(), 1);
  ServiceProvider::CollaborationAssignment assignment;
  assignment.service = profile.serviceName;
  assignment.role = "/consumer";
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.workCurrent = [] { return true; };
  assignment.workDeadline = std::chrono::steady_clock::now() + 3s;
  ServiceProvider::CollaborationContext context(environment.provider(1), profile.userIdentity,
    ndn::Name("/spec226/bootstrap"), RequestMessage{}, assignment);
  auto& face = environment.providerFace(1);
  size_t interests = 0;
  std::vector<ndn::Name> usedHints;
  ndn::signal::ScopedConnection relay = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() != locator) return;
    BOOST_CHECK(interest.getCanBePrefix());
    ++interests;
    if (!interest.getForwardingHint().empty()) usedHints.push_back(interest.getForwardingHint().front());
    if (interests == 2) face.receive(*originals.at(1));
  });
  auto operation = Spec226RecoveryOracle::bootstrap(context, locator, producer);
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (operation.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < end) {
    face.processEvents(ndn::time::milliseconds(5));
    face.getIoContext().restart();
  }
  BOOST_REQUIRE(operation.wait_for(0ms) == std::future_status::ready);
  const auto result = operation.get();
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->object.packets().size(), 1);
  BOOST_CHECK(result->object.packets().front()->wireEncode() == originals.at(1)->wireEncode());
  BOOST_CHECK_EQUAL(result->metrics.actualDataName, originals.at(1)->getName().toUri());
  BOOST_CHECK_EQUAL(interests, 2);
  BOOST_REQUIRE_EQUAL(usedHints.size(), 2);
  BOOST_CHECK(usedHints[0] == producer);
  BOOST_CHECK(usedHints[1] == producer);
  BOOST_CHECK_EQUAL(result->metrics.originalRetries, 1);
  BOOST_CHECK_EQUAL(result->metrics.fecRecoveryAttempts, 0);
}

BOOST_AUTO_TEST_CASE(ReadCapacityIsBoundedAcrossServicesAndReleasedByItsLastOwner)
{
  auto budget = std::make_shared<detail::CollaborationInputBudget>();
  CollaborationAdmissionLimits limits;
  limits.validate();
  auto first = budget->reserve("first", {0, 0, 0, 128}, limits);
  auto second = budget->reserve("second", {0, 0, 0, 128}, limits);
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK(!budget->reserve("third", {0, 0, 0, 1}, limits));
  auto held = first;
  first.reset();
  BOOST_CHECK(!budget->reserve("third", {0, 0, 0, 1}, limits));
  held.reset();
  auto third = budget->reserve("third", {0, 0, 0, 128}, limits);
  BOOST_REQUIRE(third);
  BOOST_CHECK(!budget->reserve("third", {0, 0, 0, 1}, limits));
  limits.maxInFlightReads = 257;
  BOOST_CHECK_THROW(limits.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ExactWindowStartsWideGrowsAndSuppressesRepeatedLossReduction)
{
  for (size_t count : {size_t{1}, size_t{16}, size_t{32}}) {
    detail::ExactDataWindow window(count);
    BOOST_CHECK_EQUAL(window.size(), count);
  }
  detail::ExactDataWindow window(128);
  BOOST_CHECK_EQUAL(window.size(), 32);
  for (size_t sequence = 1; sequence <= 32; ++sequence) window.acknowledge(sequence);
  BOOST_CHECK_EQUAL(window.size(), 64);
  window.congest(96);
  BOOST_CHECK_EQUAL(window.size(), 32);
  for (size_t i = 0; i < 32; ++i) window.congest(96);
  BOOST_CHECK_EQUAL(window.size(), 32);
  window.acknowledge(97);
  window.congest(128);
  BOOST_CHECK_EQUAL(window.size(), 16);
  for (size_t sequence = 98; sequence < 10000; ++sequence) window.acknowledge(sequence);
  BOOST_CHECK_EQUAL(window.size(), 128);
  detail::ExactDataWindow noData(128);
  noData.congest(32);
  noData.congest(32);
  BOOST_CHECK_EQUAL(noData.size(), 16);
  detail::ExactDataWindow one(1);
  one.congest(1);
  BOOST_CHECK_EQUAL(one.size(), 1);
}

BOOST_AUTO_TEST_CASE(FiniteRepairPacketIsCanonicalAndProducerBound)
{
  const auto fixture = makeFiniteRepairFixture(8);
  const auto packet = makeRepairPacket(fixture);
  const auto wire = packet.wireEncode(fixture.producer);
  const auto decoded = detail::DependencyRepairPacketV1::wireDecode(wire, fixture.producer);
  BOOST_CHECK(decoded.wireEncode(fixture.producer) == wire);
  BOOST_REQUIRE_EQUAL(decoded.group.members.size(), 8);
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(
    wire, ndn::Name("/other/producer")), std::invalid_argument);
  auto exactLimit = detail::DependencyRepairLimits{};
  exactLimit.maxRepairBytes = wire.size();
  BOOST_CHECK_NO_THROW(packet.wireEncode(fixture.producer, exactLimit));
  --exactLimit.maxRepairBytes;
  BOOST_CHECK_THROW(packet.wireEncode(fixture.producer, exactLimit), std::invalid_argument);
  auto duplicate = packet;
  duplicate.group.members[1] = duplicate.group.members.front();
  BOOST_CHECK_THROW(duplicate.wireEncode(fixture.producer), std::invalid_argument);
  auto sourceLimit = detail::DependencyRepairLimits{};
  sourceLimit.maxSourcePackets = 7;
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(
    wire, fixture.producer, sourceLimit), std::invalid_argument);
  auto wrongIndex = packet;
  wrongIndex.group.index = 8192;
  BOOST_CHECK_THROW(wrongIndex.wireEncode(fixture.producer), std::invalid_argument);
  auto outside = packet;
  outside.group.repairRoot = ndn::Name("/other/producer/repair");
  BOOST_CHECK_THROW(outside.wireEncode(fixture.producer), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(MalformedMemberFieldsAreRejectedBeforeNameMaterialization)
{
  const auto fixture = makeFiniteRepairFixture(8);
  const auto wire = makeRepairPacket(fixture).wireEncode(fixture.producer);
  wire.parse();
  auto fields = wire.elements();
  fields.at(3).parse();
  auto groupFields = fields.at(3).elements();
  groupFields.at(9).parse();
  auto memberFields = groupFields.at(9).elements();
  memberFields.at(1) = ndn::makeBinaryBlock(0x2034, ndn::span<const uint8_t>(
    memberFields.at(1).value_begin(), memberFields.at(1).value_end()));
  groupFields.at(9) = withExactTlvChildren(groupFields.at(9).type(), memberFields);
  fields.at(3) = withExactTlvChildren(fields.at(3).type(), groupFields);
  const auto malformed = withExactTlvChildren(wire.type(), fields);
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(
    malformed, fixture.producer), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FiniteRepairPacketRejectsOverlongOuterAndNestedNameHeaders)
{
  const auto fixture = makeFiniteRepairFixture(8);
  const auto wire = makeRepairPacket(fixture).wireEncode(fixture.producer);
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(
    withOverlongTlvHeader(wire, true), fixture.producer), std::invalid_argument);
  BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(
    withOverlongTlvHeader(wire, false), fixture.producer), std::invalid_argument);
  wire.parse();
  for (bool component : {false, true}) {
    auto fields = wire.elements();
    fields.at(1) = withExactTlvChildren(fields.at(1).type(), {
      component ? nameWithOverlongComponent(fixture.rootName) :
        withOverlongTlvHeader(fixture.rootName.wireEncode(), true)});
    const auto malformed = withExactTlvChildren(wire.type(), fields);
    BOOST_REQUIRE_GT(malformed.size(), wire.size());
    BOOST_CHECK_THROW(detail::DependencyRepairPacketV1::wireDecode(
      malformed, fixture.producer), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(MaximumOriginalWireRecoveryDoesNotAssumeParityFitsOnePacket)
{
  const auto fixture = makeFiniteRepairFixture(2, true);
  BOOST_REQUIRE_EQUAL(fixture.originals.front().size(), ndn::MAX_NDN_PACKET_SIZE);
  BOOST_CHECK_EQUAL(fixture.parity.front().size(), ndn::MAX_NDN_PACKET_SIZE);
  const auto& group = fixture.groups.front();
  const auto recovered = detail::recoverDependencyWire(group, {std::nullopt, fixture.originals[1]}, fixture.parity.front());
  BOOST_REQUIRE(recovered);
  BOOST_CHECK(*recovered == fixture.originals.front());
  auto tighter = detail::DependencyRepairLimits{};
  --tighter.maxPacketWireBytes;
  BOOST_CHECK_THROW(makeRepairPacket(fixture).wireEncode(fixture.producer, tighter), std::invalid_argument);
  BOOST_CHECK(!detail::recoverDependencyWire(group, {std::nullopt, fixture.originals[1]}, fixture.parity.front(), tighter));
}

BOOST_AUTO_TEST_CASE(FiniteWireRecoveryRequiresExactlyOneGapAndMatchingEveryDigest)
{
  const auto fixture = makeFiniteRepairFixture();
  const auto& group = fixture.groups.front();
  std::vector<std::optional<ndn::Buffer>> originals;
  for (size_t i = 0; i < group.members.size(); ++i) originals.push_back(fixture.originals.at(i));
  BOOST_CHECK(!detail::recoverDependencyWire(group, originals, fixture.parity.front()));
  for (size_t missing = 0; missing < group.members.size(); ++missing) {
    auto oneGap = originals;
    oneGap[missing].reset();
    const auto recovered = detail::recoverDependencyWire(group, oneGap, fixture.parity.front());
    BOOST_REQUIRE(recovered);
    BOOST_CHECK(*recovered == fixture.originals.at(missing));
    ndn::Data parsed(ndn::Block(ndn::span<const uint8_t>(recovered->data(), recovered->size())));
    BOOST_CHECK(parsed.getName() == group.members.at(missing).name);
  }
  originals[0].reset();
  originals[1].reset();
  BOOST_CHECK(!detail::recoverDependencyWire(group, originals, fixture.parity.front()));
  originals[1] = fixture.originals[1];
  originals[1]->at(0) ^= 1;
  BOOST_CHECK(!detail::recoverDependencyWire(group, originals, fixture.parity.front()));
  originals[1] = fixture.originals[1];
  auto damagedParity = fixture.parity.front();
  damagedParity[0] ^= 1;
  BOOST_CHECK(!detail::recoverDependencyWire(group, originals, damagedParity));
  auto wrongLength = group;
  --wrongLength.members.front().wireLength;
  BOOST_CHECK(!detail::recoverDependencyWire(wrongLength, originals, fixture.parity.front()));
  auto wrongDigest = group;
  wrongDigest.members.front().wireSha256 = "sha256:" + std::string(64, '0');
  BOOST_CHECK(!detail::recoverDependencyWire(wrongDigest, originals, fixture.parity.front()));
  const auto& tail = fixture.groups.back();
  const auto recoveredTail = detail::recoverDependencyWire(tail, {std::nullopt}, fixture.parity.back());
  BOOST_REQUIRE(recoveredTail);
  BOOST_CHECK(*recoveredTail == fixture.originals.back());
}

BOOST_AUTO_TEST_CASE(SharedXorKernelPreservesPaddingAndRejectsOversizeBeforeMutation)
{
  const std::vector<uint8_t> first{1, 2, 3, 4, 5};
  const std::vector<uint8_t> second{8, 7, 6};
  std::vector<uint8_t> parity(first.size(), 0);
  detail::xorInto(ndn::span<uint8_t>(parity.data(), parity.size()), ndn::span<const uint8_t>(first.data(), first.size()));
  detail::xorInto(ndn::span<uint8_t>(parity.data(), parity.size()), ndn::span<const uint8_t>(second.data(), second.size()));
  auto recovered = parity;
  detail::xorInto(ndn::span<uint8_t>(recovered.data(), recovered.size()), ndn::span<const uint8_t>(first.data(), first.size()));
  recovered.resize(second.size());
  BOOST_CHECK(recovered == second);
  const auto unchanged = parity;
  const std::vector<uint8_t> tooWide(6, 0x55);
  BOOST_CHECK_THROW(detail::xorInto(ndn::span<uint8_t>(parity.data(), parity.size()),
    ndn::span<const uint8_t>(tooWide.data(), tooWide.size())), std::invalid_argument);
  BOOST_CHECK(parity == unchanged);
}


BOOST_AUTO_TEST_CASE(NoneStillHasSixBoundedOriginalAttempts)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.disconnectProviderPeerTransportForTest();
  auto& consumer = environment.provider(1);
  auto& face = environment.providerFace(1);
  const auto producer = environment.provider(0).getName();
  const auto name = ndn::Name(producer).append("NDNSF").append("UNAVAILABLE").appendVersion(1);
  size_t attempts = 0;
  ndn::signal::ScopedConnection observed = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == name) ++attempts; // The link drops every original Interest.
  });
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "/consumer";
  assignment.service = profile.serviceName;
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.selectionDigest = "spec226-bounded-original-arq";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.workDeadline = std::chrono::steady_clock::now() + 8s;
  assignment.workCurrent = [] { return true; };
  ServiceProvider::CollaborationContext context(consumer, profile.userIdentity,
    ndn::Name("/spec226/recovery/none"), RequestMessage{}, assignment);
  auto read = context.startVerifiedSignedExactDataBatch("input", {name}, producer, 5500,
    {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 1, 4096);
  const auto limit = std::chrono::steady_clock::now() + 6s;
  while (read.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < limit) {
    face.processEvents(ndn::time::milliseconds(5));
    face.getIoContext().restart();
  }
  BOOST_REQUIRE(read.wait_for(0ms) == std::future_status::ready);
  BOOST_CHECK(!read.get());
  BOOST_CHECK_EQUAL(attempts, 6);
  BOOST_CHECK(assignment.dependencyTransport.redundancy == DependencyRedundancy::None);
  const auto metrics = read.metricsSnapshot();
  BOOST_CHECK_EQUAL(metrics.originalInterests, 6);
  BOOST_CHECK_EQUAL(metrics.originalRetries, 5);
  BOOST_CHECK_EQUAL(metrics.fecRecoveryAttempts, 0);
  BOOST_CHECK_EQUAL(metrics.arqFallbacks, 0);
  BOOST_CHECK_EQUAL(metrics.recoveryMode, "none");
}

BOOST_AUTO_TEST_CASE(KnownExactNamesStartWithTheInitialCongestionWindowAndRespectCapacity)
{
  using namespace std::chrono_literals;
  constexpr auto initialWindow = detail::ExactDataCongestionWindow::INITIAL_WINDOW;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.disconnectProviderPeerTransportForTest();
  auto& consumer = environment.provider(1);
  auto& face = environment.providerFace(1);
  const auto producer = environment.provider(0).getName();
  for (const auto [count, capacity] :
       {std::pair<size_t, size_t>{16, 128}, {32, 128}, {64, 128},
        {96, 128}, {32, 8}, {32, 256}}) {
    const auto prefix = ndn::Name(producer).append("NDNSF").append("WINDOW")
      .appendNumber(count).appendNumber(capacity);
    std::vector<ndn::Name> names;
    for (size_t i = 0; i < count; ++i) names.push_back(ndn::Name(prefix).appendSegment(i));
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "/consumer";
    assignment.service = profile.serviceName;
    assignment.inputAdmissionProfile = "data-admission-v1";
    assignment.selectionDigest = "spec226-window";
    assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
    assignment.workDeadline = std::chrono::steady_clock::now() + 3s;
    assignment.workCurrent = [] { return true; };
    std::set<ndn::Name> issued;
    size_t interestCount = 0;
    size_t probes = 0;
    ndn::signal::ScopedConnection observed = face.onSendInterest.connect(
      [&](const ndn::Interest& interest) {
        if (prefix.isPrefixOf(interest.getName())) {
          issued.insert(interest.getName());
          ++interestCount;
          if (interest.getCanBePrefix()) ++probes;
        }
      });
    ServiceProvider::CollaborationContext context(consumer, profile.userIdentity,
      ndn::Name("/spec226/recovery/window"), RequestMessage{}, assignment);
    BOOST_CHECK_THROW(context.startVerifiedSignedExactDataBatch("input", names, producer,
      2000, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 257,
      count * ndn::MAX_NDN_PACKET_SIZE), std::invalid_argument);
    auto operation = context.startVerifiedSignedExactDataBatch("input", names, producer, 2000,
      [] { return false; }, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, capacity,
      count * ndn::MAX_NDN_PACKET_SIZE);
    face.processEvents(ndn::time::milliseconds(40));
    face.getIoContext().restart();
    BOOST_CHECK_EQUAL(issued.size(), std::min({count, capacity, initialWindow}));
    BOOST_CHECK_EQUAL(probes, 0);
    if (count == 96) {
      ndn::Data first(names.front());
      first.setContent(ndn::Buffer{1});
      environment.keyChain().sign(first, ndn::security::signingByIdentity(producer));
      face.receive(first);
      face.processEvents(ndn::time::milliseconds(20));
      face.getIoContext().restart();
      // One ACK grows 16 -> 17. Its released slot and the AIMD increase
      // permit two additional names in the next flight.
      BOOST_CHECK_EQUAL(issued.size(), initialWindow + 2);
      ndn::Data second(names.at(1));
      second.setContent(ndn::Buffer{2});
      environment.keyChain().sign(second, ndn::security::signingByIdentity(producer));
      second.setCongestionMark(1);
      face.receive(second);
      face.processEvents(ndn::time::milliseconds(20));
      face.getIoContext().restart();
      // Outstanding packets drain before new credits are issued after decrease.
      BOOST_CHECK_EQUAL(issued.size(), initialWindow + 2);
    }
    operation.cancel();
    face.processEvents(ndn::time::milliseconds(5));
    face.getIoContext().restart();
    BOOST_REQUIRE(operation.wait_for(0ms) == std::future_status::ready);
    BOOST_CHECK(!operation.get());
  }
}

BOOST_AUTO_TEST_CASE(LargeExactBatchDispatchesItsFirstWindowWithinDeadline)
{
  using namespace std::chrono_literals;
  constexpr std::size_t nameCount = 876;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.disconnectProviderPeerTransportForTest();
  auto& consumer = environment.provider(1);
  auto& face = environment.providerFace(1);
  const auto producer = environment.provider(0).getName();
  std::vector<ndn::Name> names;
  names.reserve(nameCount);
  for (std::size_t i = 0; i < nameCount; ++i) {
    names.emplace_back(ndn::Name(producer).append("NDNSF").append("BATCH")
      .appendSegment(i));
  }
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "/consumer";
  assignment.service = profile.serviceName;
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.selectionDigest = "spec226-large-exact-batch-first-window";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.workDeadline = std::chrono::steady_clock::now() + 3s;
  assignment.workCurrent = [] { return true; };
  ServiceProvider::CollaborationContext context(consumer, profile.userIdentity,
    ndn::Name("/spec226/recovery/large-batch"), RequestMessage{}, assignment);

  std::size_t firstWindowInterests = 0;
  ndn::signal::ScopedConnection observed = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == names.front()) ++firstWindowInterests;
  });
  auto operation = context.startSignedExactDataBatch("input", names, producer, 2500,
    [] { return false; }, {}, ServiceProvider::ExactDataFetchPhase::Published,
    {}, 1);

  const auto dispatchDeadline = std::chrono::steady_clock::now() + 1s;
  while (firstWindowInterests == 0 && operation.wait_for(0ms) != std::future_status::ready &&
         std::chrono::steady_clock::now() < dispatchDeadline) {
    face.processEvents(ndn::time::milliseconds(5));
    face.getIoContext().restart();
  }
  operation.cancel();
  const auto cleanupDeadline = std::chrono::steady_clock::now() + 1s;
  while (operation.wait_for(0ms) != std::future_status::ready &&
         std::chrono::steady_clock::now() < cleanupDeadline) {
    face.processEvents(ndn::time::milliseconds(5));
    face.getIoContext().restart();
  }
  BOOST_CHECK(operation.wait_for(0ms) == std::future_status::ready);
  BOOST_CHECK_EQUAL(firstWindowInterests, 1);
  (void)operation.get();
}

BOOST_AUTO_TEST_CASE(RecoveredOriginalWireReusesSignatureAndCurrentPermissionChecks)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();
  auto& producer = environment.provider(0);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  const auto producerName = producer.getName();
  const ndn::Name request("/spec226/recovery/revalidate");
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "/consumer";
  assignment.service = profile.serviceName;
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.selectionDigest = "spec226-recovered-original-revalidation";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.dependencyTransport.redundancy = DependencyRedundancy::Xor;
  assignment.workDeadline = std::chrono::steady_clock::now() + 40s;
  assignment.workCurrent = [] { return true; };
  ServiceProvider::CollaborationContext publisher(
    producer, profile.userIdentity, request, RequestMessage{}, assignment);
  ServiceProvider::CollaborationContext receiver(
    environment.provider(1), profile.userIdentity, request, RequestMessage{}, assignment);
  std::mutex sourceMutex;
  std::map<ndn::Name, ndn::Buffer> sourceWires;
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (producerName.isPrefixOf(interest.getName()) &&
        interest.getName().size() > producerName.size() + 1 &&
        interest.getName().at(producerName.size()).toUri() == "NDNSF" &&
        interest.getName().at(producerName.size() + 1).toUri() == "RECOVERED") {
      // The sender is allowed to answer, but this test deliberately drops its
      // Data so the exact-name slot reaches the finite ARQ recovery boundary.
      producerFace.receive(interest);
    }
  });
  ndn::signal::ScopedConnection packets = producerFace.onSendData.connect([&](const ndn::Data& data) {
    if (!producerName.isPrefixOf(data.getName()) ||
        data.getName().size() <= producerName.size() + 1 ||
        data.getName().at(producerName.size()).toUri() != "NDNSF" ||
        data.getName().at(producerName.size() + 1).toUri() != "RECOVERED") return;
    const auto& wire = data.wireEncode();
    std::lock_guard<std::mutex> lock(sourceMutex);
    sourceWires[data.getName()] = ndn::Buffer(wire.begin(), wire.end());
  });

  const auto runRecovery = [&](uint64_t version, bool corruptWire,
                               bool currentPermission, bool provideRepair = true,
                               bool throwPermission = false) {
    const auto name = ndn::Name(producerName).append("NDNSF")
      .append("RECOVERED").appendVersion(version);
    BOOST_REQUIRE(publisher.publishSignedExactData("input", {{name, ndn::Buffer{7, 8, 9}}}));
    size_t attempts = 0;
    ndn::signal::ScopedConnection countInterests = consumerFace.onSendInterest.connect(
      [&](const ndn::Interest& interest) {
        if (interest.getName() == name) {
          ++attempts;
        }
      });
    size_t recoveryCalls = 0;
    size_t permissionChecks = 0;
    auto read = Spec226RecoveryOracle::start(
      receiver, "input", {name}, producerName, 10000,
      ServiceProvider::ExactDataFetchPhase::Published,
      [&] {
        ++permissionChecks;
        if (throwPermission) throw std::runtime_error("private admission failure");
        return currentPermission;
      }, 1, 4096,
      [&](const ndn::Name& missing, const std::vector<ndn::Name>& names,
          const std::vector<ndn::Buffer>& originals) -> std::optional<ndn::Buffer> {
        ++recoveryCalls;
        if (missing != name || names != std::vector<ndn::Name>{name} ||
            originals.size() != 1 || !originals.front().empty()) return std::nullopt;
        if (!provideRepair) return std::nullopt;
        std::lock_guard<std::mutex> lock(sourceMutex);
        const auto found = sourceWires.find(name);
        if (found == sourceWires.end()) return std::nullopt;
        auto wire = found->second;
        if (corruptWire && !wire.empty()) wire.back() ^= 1;
        return wire;
      });
    const auto limit = std::chrono::steady_clock::now() + 12s;
    while (read.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < limit) {
      producerFace.processEvents(ndn::time::milliseconds(5));
      consumerFace.processEvents(ndn::time::milliseconds(5));
      producerFace.getIoContext().restart();
      consumerFace.getIoContext().restart();
    }
    BOOST_REQUIRE(read.wait_for(0ms) == std::future_status::ready);
    const auto metrics = read.metricsSnapshot();
    auto result = read.get();
    BOOST_CHECK_EQUAL(recoveryCalls, 1);
    if (provideRepair && !corruptWire && currentPermission && !throwPermission) {
      BOOST_REQUIRE(result);
      BOOST_CHECK_EQUAL(attempts, 1);
      BOOST_CHECK_EQUAL(permissionChecks, 1);
      BOOST_REQUIRE_EQUAL(result->object.packets().size(), 1);
      const auto& returned = result->object.packets().front()->wireEncode();
      BOOST_CHECK_EQUAL(returned.size(), sourceWires.at(name).size());
      BOOST_CHECK(std::equal(returned.begin(), returned.end(), sourceWires.at(name).begin()));
    }
    else {
      BOOST_CHECK(!result);
      if (corruptWire || !provideRepair) BOOST_CHECK_EQUAL(permissionChecks, 0);
      else BOOST_CHECK_EQUAL(permissionChecks, 1);
      if (provideRepair && !corruptWire && (!currentPermission || throwPermission)) {
        BOOST_CHECK_EQUAL(attempts, 1);
        BOOST_CHECK_EQUAL(metrics.arqFallbacks, 0);
        BOOST_CHECK_EQUAL(metrics.fecRecoveredPackets, 0);
      }
      else
        BOOST_CHECK_EQUAL(attempts, 6);
    }
    countInterests.disconnect();
  };

  runRecovery(1, false, true);
  runRecovery(2, true, true);
  runRecovery(3, false, false);
  runRecovery(4, false, true, false);
  runRecovery(5, false, true, true, true);
}




BOOST_AUTO_TEST_CASE(SixthFutureInterestRemainsAliveUntilTheOriginalDeadline)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  environment.disconnectProviderPeerTransportForTest();
  auto& producer = environment.provider(0);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  const auto name = ndn::Name(producer.getName()).append("NDNSF").append("FUTURE").appendVersion(1);
  size_t attempts = 0;
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == name) { ++attempts; producerFace.receive(interest); }
  });
  ndn::signal::ScopedConnection packets = producerFace.onSendData.connect([&](const ndn::Data& data) {
    if (data.getName() == name) consumerFace.receive(data);
  });
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "/consumer";
  assignment.service = profile.serviceName;
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.selectionDigest = "spec226-future-original-arq";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.workDeadline = std::chrono::steady_clock::now() + 8s;
  assignment.workCurrent = [] { return true; };
  const ndn::Name request("/spec226/recovery/future");
  ServiceProvider::CollaborationContext publisher(producer, profile.userIdentity, request, RequestMessage{}, assignment);
  ServiceProvider::CollaborationContext receiver(environment.provider(1), profile.userIdentity, request, RequestMessage{}, assignment);
  auto read = receiver.startVerifiedSignedExactDataBatch("input", {name}, producer.getName(), 5500,
    {}, {}, ServiceProvider::ExactDataFetchPhase::AwaitPublication, {}, 1, 4096);
  const auto begin = std::chrono::steady_clock::now();
  bool published = false;
  while (read.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() - begin < 6s) {
    if (!published && std::chrono::steady_clock::now() - begin >= 4800ms) {
      published = publisher.publishSignedExactData("input", {{name, ndn::Buffer{7}}});
      BOOST_REQUIRE(published);
    }
    producerFace.processEvents(ndn::time::milliseconds(5));
    consumerFace.processEvents(ndn::time::milliseconds(5));
    producerFace.getIoContext().restart();
    consumerFace.getIoContext().restart();
  }
  BOOST_REQUIRE(read.wait_for(0ms) == std::future_status::ready);
  auto result = read.get();
  BOOST_REQUIRE(result);
  BOOST_CHECK(published);
  BOOST_CHECK_EQUAL(attempts, 6);
  BOOST_REQUIRE(result->metrics.interestCount);
  BOOST_CHECK_EQUAL(*result->metrics.interestCount, 6);
  BOOST_REQUIRE_EQUAL(result->object.packets().size(), 1);
  BOOST_CHECK(result->object.packets().front()->getName() == name);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
