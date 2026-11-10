#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/uav-person-published-frame.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::test {
namespace {

using namespace examples::uav;

const ndn::Name PRODUCER("/example/uav/drone/UAV1");
const ndn::Name REQUESTER("/example/uav/gs");

ndn::Buffer
frameBytes(const std::string& text = "source-frame-bytes")
{
  // Opaque byte-cache fixture, not model or JPEG decoding evidence.
  return ndn::Buffer(text.begin(), text.end());
}

UavPersonFrameReadBinding
readBinding()
{
  UavPersonFrameReadBinding binding;
  binding.runId = "run";
  binding.missionId = "mission";
  binding.windowId = "window-0";
  binding.cameraId = "UAV1";
  binding.sourceId = "source1";
  binding.sourceEpoch = 7;
  binding.sequence = 1;
  binding.frameName = ndn::Name("/example/uav/drone/UAV1/UAV/OBS/UAV1/7/1/FRAME")
                        .appendVersion(1).toUri();
  binding.inputDigest = "sha256:778c4838d5c372eaef191b97a8dc9eddbd92998944a935bbbf4cb35d3f761f5e";
  return binding;
}

void
checkBytes(const ndn::Buffer& actual, const ndn::Buffer& expected)
{
  BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonPublishedFrame)

BOOST_AUTO_TEST_CASE(PublicationAndReadSnapshotsOwnTheirBytes)
{
  UavPersonPublishedFrameStore store(PRODUCER, "UAV1");
  const auto binding = readBinding();
  const auto fields = makeUavPersonFrameReadFields(binding);
  BOOST_REQUIRE_EQUAL(fields.size(), 10);
  BOOST_CHECK_EQUAL(fields.at("schema"), "uav-person-source-frame-read-v1");
  BOOST_CHECK_EQUAL(fields.at("source_epoch"), "7");
  BOOST_CHECK_EQUAL(fields.at("sequence"), "1");
  auto bytes = frameBytes();
  store.retainPublished(binding, bytes, REQUESTER);
  bytes[0] ^= 0xff;
  auto snapshot = store.read(fields, REQUESTER);
  checkBytes(snapshot, frameBytes());
  snapshot[0] ^= 0xff;
  checkBytes(store.read(fields, REQUESTER), frameBytes());
}

BOOST_AUTO_TEST_CASE(EveryRequestFieldAndAuthenticatedRequesterMustMatch)
{
  UavPersonPublishedFrameStore store(PRODUCER, "UAV1");
  const auto binding = readBinding();
  const auto fields = makeUavPersonFrameReadFields(binding);
  BOOST_CHECK_THROW(store.read(fields, REQUESTER), std::invalid_argument);
  store.retainPublished(binding, frameBytes(), REQUESTER);
  for (const auto& [key, value] : fields) {
    auto missing = fields;
    missing.erase(key);
    BOOST_CHECK_THROW(store.read(missing, REQUESTER), std::invalid_argument);
    auto changed = fields;
    changed.at(key) += "x";
    BOOST_CHECK_THROW(store.read(changed, REQUESTER), std::invalid_argument);
  }
  auto extra = fields;
  extra.emplace("requester", REQUESTER.toUri());
  BOOST_CHECK_THROW(store.read(extra, REQUESTER), std::invalid_argument);
  auto noncanonical = fields;
  noncanonical.at("sequence") = "01";
  BOOST_CHECK_THROW(store.read(noncanonical, REQUESTER), std::invalid_argument);
  BOOST_CHECK_THROW(store.read(fields, ndn::Name("/other/user")), std::invalid_argument);
  BOOST_CHECK_THROW(store.read(fields, ndn::Name()), std::invalid_argument);
  checkBytes(store.read(fields, REQUESTER), frameBytes());
}

BOOST_AUTO_TEST_CASE(RejectedRetentionCannotReplaceThePublishedSlot)
{
  UavPersonPublishedFrameStore store(PRODUCER, "UAV1");
  const auto binding = readBinding();
  const auto fields = makeUavPersonFrameReadFields(binding);
  store.retainPublished(binding, frameBytes(), REQUESTER);
  std::vector<UavPersonFrameReadBinding> invalid;
  for (const auto field : {&UavPersonFrameReadBinding::runId,
                           &UavPersonFrameReadBinding::missionId,
                           &UavPersonFrameReadBinding::windowId,
                           &UavPersonFrameReadBinding::sourceId}) {
    auto changed = binding;
    changed.*field = "";
    invalid.push_back(changed);
    changed.*field = std::string(129, 'x');
    invalid.push_back(changed);
  }
  auto changed = binding;
  changed.cameraId = "UAV2";
  invalid.push_back(changed);
  changed = binding;
  changed.sourceEpoch = 0;
  invalid.push_back(changed);
  changed = binding;
  changed.sequence = 0;
  invalid.push_back(changed);
  changed = binding;
  changed.frameName = "/other/FRAME";
  invalid.push_back(changed);
  changed = binding;
  changed.inputDigest = "sha256:" + std::string(64, 'a');
  invalid.push_back(changed);
  for (const auto& bad : invalid) {
    BOOST_CHECK_THROW(store.retainPublished(bad, frameBytes(), REQUESTER), std::invalid_argument);
    checkBytes(store.read(fields, REQUESTER), frameBytes());
  }
  BOOST_CHECK_THROW(store.retainPublished(binding, {}, REQUESTER), std::invalid_argument);
  BOOST_CHECK_THROW(store.retainPublished(binding,
    ndn::Buffer(MAX_PERSON_PUBLISHED_FRAME_BYTES + 1), REQUESTER), std::invalid_argument);
  BOOST_CHECK_THROW(store.retainPublished(binding, frameBytes(), ndn::Name()), std::invalid_argument);
  checkBytes(store.read(fields, REQUESTER), frameBytes());
  BOOST_CHECK_THROW((UavPersonPublishedFrameStore(ndn::Name(), "UAV1")), std::invalid_argument);
  BOOST_CHECK_THROW((UavPersonPublishedFrameStore(PRODUCER, "UAV4")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OnlyTheLatestPublicationCanBeRead)
{
  UavPersonPublishedFrameStore store(PRODUCER, "UAV1");
  const auto before = readBinding();
  store.retainPublished(before, frameBytes(), REQUESTER);
  auto after = before;
  after.windowId = "window-1";
  after.sequence = 2;
  after.frameName = ndn::Name("/example/uav/drone/UAV1/UAV/OBS/UAV1/7/2/FRAME")
                      .appendVersion(1).toUri();
  after.inputDigest = "sha256:69d56ad730f5f15e2ce80ddc856aeacf4258240842b413f32d378cb279bcd945";
  store.retainPublished(after, frameBytes("next-frame-bytes"), REQUESTER);
  BOOST_CHECK_THROW(store.read(makeUavPersonFrameReadFields(before), REQUESTER),
                    std::invalid_argument);
  checkBytes(store.read(makeUavPersonFrameReadFields(after), REQUESTER), frameBytes("next-frame-bytes"));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
