#include "NDNSF-UAV-APP/shared/UavTrackingEvidenceStore.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/lp/fields.hpp>
#include <ndn-cxx/lp/packet.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace ndnsf::examples::uav::tests {
namespace {

// Exact JPEG lengths of the largest-total source in the frozen 100-window fixture.
// Numerical transport fixture only: no detector, image decoder or tracking oracle.
constexpr std::array<size_t, 100> FRAME_BYTES{{
  1259436, 1251949, 1247733, 1246788, 1247606, 1246431, 1244272, 1246334,
  1251526, 1251630, 1253772, 1246969, 1247188, 1246813, 1252601, 1253871,
  1247103, 1251626, 1248675, 1249746, 1249393, 1240868, 1244880, 1246064,
  1253765, 1285048, 1260336, 1255926, 1257968, 1254938, 1256649, 1248075,
  1250468, 1252309, 1249608, 1251230, 1248784, 1249834, 1250458, 1251794,
  1255503, 1244272, 1249909, 1251224, 1246298, 1248543, 1243073, 1248221,
  1248248, 1249093, 1285968, 1265669, 1261730, 1258237, 1256154, 1252910,
  1245629, 1249539, 1250375, 1254240, 1256883, 1248143, 1251348, 1249551,
  1250252, 1248212, 1240901, 1244802, 1244017, 1247504, 1250137, 1247125,
  1251442, 1246535, 1249337, 1287693, 1261822, 1264316, 1262380, 1260644,
  1260485, 1254361, 1256531, 1257611, 1257127, 1259363, 1250407, 1253730,
  1250870, 1254593, 1251548, 1244864, 1250952, 1252945, 1250673, 1254132,
  1244722, 1250236, 1253518, 1249711,
}};

std::string
payloadDigest(const std::vector<uint8_t>& bytes)
{
  ndn::util::Sha256 hash;
  hash << std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return canonicalSha256Digest(hash.toString());
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavVehiclePacketCapacity)

BOOST_AUTO_TEST_CASE(HundredVehicleWindowsFitExistingLargeSegmentsWithOriginalWire)
{
  using Context = ndn_service_framework::ServiceProvider::CollaborationContext;
  using Assignment = ndn_service_framework::ServiceProvider::CollaborationAssignment;
  constexpr size_t SEGMENT_BYTES = 7000;
  constexpr size_t EXACT_CAPACITY = 128 * 1024 * 1024;
  size_t oldMinimum = 0;
  for (auto bytes : FRAME_BYTES) oldMinimum += bytes + ((bytes + 1399) / 1400) * 256;
  BOOST_REQUIRE_GT(oldMinimum, EXACT_CAPACITY);

  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/UAV/Tracking/Source");
  profile.providerIdentity = ndn::Name("/example/uav/drone/UAV3");
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE_MESSAGE(environment.status() ==
    ndn_service_framework::test::EnvironmentStatus::Ready, environment.failureReason());
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  BOOST_REQUIRE(producer.getName() == profile.providerIdentity);
  const auto prefix = ndn::Name(producer.getName()).append("UAV").append("OBS")
    .append("UAV3").append("6104616274784016968");
  auto firstName = prefix;
  firstName.append("1").append("FRAME").appendVersion(1);
  std::map<ndn::Name, ndn::Block> firstWires;
  std::set<ndn::Name> seen, completedNames;
  size_t totalWire = 0, maximumWire = 0, maximumLp = 0, replays = 0;
  bool changedWire = false;
  ndn::signal::ScopedConnection observe = environment.providerFace(0).onSendData.connect(
    [&](const ndn::Data& packet) {
      if (!prefix.isPrefixOf(packet.getName())) return;
      const auto wire = packet.wireEncode();
      BOOST_CHECK_EQUAL(packet.getSignatureValue().value_size(), 256U);
      if (seen.insert(packet.getName()).second) totalWire += wire.size();
      const auto finalBlock = packet.getFinalBlock();
      if (finalBlock && packet.getName().get(-1) == *finalBlock) {
        completedNames.insert(packet.getName().getPrefix(-1));
      }
      if (wire.size() > maximumWire) {
        maximumWire = wire.size();
        ndn::lp::Packet lp(wire);
        lp.add<ndn::lp::CongestionMarkField>(std::numeric_limits<uint64_t>::max());
        lp.add<ndn::lp::IncomingFaceIdField>(std::numeric_limits<uint64_t>::max());
        maximumLp = lp.wireEncode().size();
      }
      if (firstName.isPrefixOf(packet.getName())) {
        const auto found = firstWires.find(packet.getName());
        if (found == firstWires.end()) firstWires.emplace(packet.getName(), wire);
        else {
          ++replays;
          changedWire = changedWire || found->second != wire;
        }
      }
    });
  const auto current = std::make_shared<std::atomic<bool>>(true);
  Assignment assignment;
  assignment.role = "Camera-UAV3";
  assignment.service = profile.serviceName;
  assignment.workCurrent = [current] { return current->load(); };
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
  assignment.scopeKeys.emplace("uav-tracking-inputs", ndn::Buffer(32, 0x41));
  assignment.scopeKeys.emplace("uav-tracking-observation", ndn::Buffer(32, 0x42));
  auto execute = [&](auto operation) {
    auto pending = std::async(std::launch::async, std::move(operation));
    environment.pumpUntil([&] {
      return pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    BOOST_REQUIRE(pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    return pending.get();
  };
  std::unique_ptr<Context> firstContext;
  std::optional<PublishedFrame> firstFrame;
  for (size_t index = 0; index < FRAME_BYTES.size(); ++index) {
    const auto suffix = std::to_string(index + 1);
    const ndn::Name request("/20261104T231017.123456-" + std::string(16 - suffix.size(), '0') + suffix);
    auto context = std::make_unique<Context>(producer, profile.userIdentity, request,
      ndn_service_framework::RequestMessage(), assignment);
    FrameEnvelope frame;
    frame.cameraId = "UAV3";
    frame.sourceEpoch = 6104616274784016968ULL;
    frame.sequence = index + 1;
    frame.ptsUs = index * 200000;
    frame.width = 3840;
    frame.height = 2160;
    frame.encoding = "image/jpeg";
    frame.bytes.assign(FRAME_BYTES[index], static_cast<uint8_t>(index));
    // Conservative frozen-profile transport bounds, not observation-schema fixtures.
    frame.observationMetadata.assign(4096, 'o');
    ObservationDataObject pose;
    pose.category = "POSE";
    pose.exactName = makeUavObservationDataName(producer.getName(), "UAV3",
      frame.sourceEpoch, frame.sequence, "POSE", frame.sequence).toUri();
    pose.producerIdentity = producer.getName().toUri();
    pose.payload.assign(2048, 'p');
    pose.contentSha256 = payloadDigest(pose.payload);
    frame.observationObjects.push_back(std::move(pose));
    UavTrackingEvidenceStore store(SEGMENT_BYTES);
    std::string reason;
    auto staged = store.stage(producer.getName(), "tracking-capacity", "window-" +
                               std::to_string(index), frame, &reason);
    BOOST_REQUIRE_MESSAGE(staged, reason);
    const auto published = execute([&] {
      return store.publish(*context, "uav-tracking-inputs", *staged, 60000, false);
    });
    BOOST_REQUIRE_MESSAGE(published == staged->reference.exactDataName,
                          "capacity rejected window " << index << " retained observed wire=" << totalWire);
    environment.pumpUntil([&] { return completedNames.count(published) != 0; });
    BOOST_REQUIRE(completedNames.count(published) != 0);
    BOOST_REQUIRE_LE(maximumLp, ndn::MAX_NDN_PACKET_SIZE);
    if (index == 0) {
      firstContext = std::move(context);
      firstFrame = std::move(staged);
    }
    if (index % 10 == 9) std::cout << "SPEC228_CAPACITY windows=" << index + 1
      << " wire=" << totalWire << " packets=" << seen.size() << std::endl;
  }
  UavTrackingEvidenceStore store(SEGMENT_BYTES);
  const auto before = replays;
  BOOST_REQUIRE(execute([&] {
    return store.publish(*firstContext, "uav-tracking-inputs", *firstFrame, 60000, false);
  }) == firstName);
  environment.pumpUntil([&] { return replays >= before + firstWires.size(); });
  BOOST_CHECK_GE(replays, before + firstWires.size());
  BOOST_CHECK(!changedWire);
  auto altered = firstFrame->payload;
  altered.front() ^= 1;
  BOOST_CHECK_THROW(execute([&] { return firstContext->publishLargeNamed(
    "uav-tracking-inputs", firstName, altered, SEGMENT_BYTES, 60000); }), std::invalid_argument);
  BOOST_CHECK(execute([&] { return firstContext->publishLargeNamed(
    "uav-tracking-inputs", ndn::Name("/wrong/FRAME").appendVersion(1),
    firstFrame->payload, SEGMENT_BYTES, 60000); }).empty());
  BOOST_CHECK(execute([&] { return firstContext->publishLargeNamed(
    "missing-scope", ndn::Name(prefix).append("missing").appendVersion(1),
    firstFrame->payload, SEGMENT_BYTES, 60000); }).empty());
  current->store(false);
  BOOST_CHECK(execute([&] { return firstContext->publishLargeNamed(
    "uav-tracking-inputs", firstName, firstFrame->payload, SEGMENT_BYTES, 60000); }).empty());
  BOOST_CHECK_LT(totalWire, EXACT_CAPACITY);
  std::cout << "SPEC228_CAPACITY_COMPLETE segmentBytes=" << SEGMENT_BYTES
            << " old1400Minimum=" << oldMinimum << " retainedWire=" << totalWire
            << " packets=" << seen.size() << " maxWire=" << maximumWire
            << " maxLocalLp=" << maximumLp << std::endl;
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::examples::uav::tests
