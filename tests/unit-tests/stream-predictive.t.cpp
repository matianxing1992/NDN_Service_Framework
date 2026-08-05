#include "ndn-service-framework/StreamFacade.hpp"
#include "ndn-service-framework/Stream.hpp"

#include <boost/test/unit_test.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/name.hpp>

#include <type_traits>

namespace nsf = ndn_service_framework;

namespace {

template<typename T, typename = void>
struct HasOldStreamStart : std::false_type
{
};

template<typename T>
struct HasOldStreamStart<T, std::void_t<decltype(
  static_cast<nsf::LiveStreamDescriptor (T::*)(
    uint64_t, const std::string&,
    const std::vector<std::vector<uint8_t>>&)>(&T::start))>>
  : std::true_type
{
};

template<typename T, typename = void>
struct HasOldStreamAnnounce : std::false_type
{
};

template<typename T>
struct HasOldStreamAnnounce<T, std::void_t<decltype(
  static_cast<void (T::*)(uint64_t, const std::string&)>(&T::announce))>>
  : std::true_type
{
};

template<typename T, typename = void>
struct HasOldStreamPublish : std::false_type
{
};

template<typename T>
struct HasOldStreamPublish<T, std::void_t<decltype(
  static_cast<void (T::*)(
    uint64_t, const std::vector<std::vector<uint8_t>>&)>(&T::publish))>>
  : std::true_type
{
};

using PredictiveStart =
  nsf::PredictiveStreamDescriptor (nsf::StreamPublisher::*)();

static_assert(!HasOldStreamStart<nsf::StreamPublisher>::value,
              "Spec 148 removes start(initialSampleId, sampleClass, items)");
static_assert(!HasOldStreamAnnounce<nsf::StreamPublisher>::value,
              "Spec 148 removes StreamPublisher::announce");
static_assert(!HasOldStreamPublish<nsf::StreamPublisher>::value,
              "Spec 148 removes StreamPublisher::publish");
static_assert(std::is_same_v<
                decltype(static_cast<PredictiveStart>(
                  &nsf::StreamPublisher::start)),
                PredictiveStart>,
              "Spec 148 requires PredictiveStreamDescriptor start()");

} // namespace

BOOST_AUTO_TEST_SUITE(StreamPredictive)

BOOST_AUTO_TEST_CASE(PredictiveCheckpointValidation)
{
  nsf::PredictiveStreamCheckpoint c;
  BOOST_CHECK(c.validate().has_value() == false); // all-zero is valid

  c.latestProducedSampleId = 10;
  c.nextExpectedSampleId = 10;
  BOOST_CHECK(!c.validate().has_value()); // equal frontiers are valid
  
  c.oldestRetainedSampleId = 20;
  c.latestProducedSampleId = 10;
  BOOST_CHECK(c.validate().has_value()); // oldestRetained > latestProduced = invalid

  c = {};
  c.oldestRetainedSampleId = 5;
  c.latestProducedSampleId = 10;
  c.nextExpectedSampleId = 15;
  BOOST_CHECK(c.validate().has_value() == false); // monotonically increasing = valid
}

BOOST_AUTO_TEST_CASE(PredictiveDescriptorFrontierName)
{
  nsf::LiveStreamDefinition definition;
  definition.streamId = "sensor";
  definition.provider = ndn::Name("/provider");
  definition.semanticDataPrefix =
    ndn::Name("/provider/stream/sensor").appendVersion(7);
  definition.sessionEpoch = 7;
  definition.mappingVersion = 7;
  const auto mappingRoot = definition.mappingRoot();

  auto frontierName = nsf::makePredictiveFrontierName(mappingRoot);
  BOOST_CHECK_EQUAL(frontierName, ndn::Name(mappingRoot).append("frontier"));
  BOOST_CHECK_EQUAL(
    nsf::makePredictiveDataName(definition, 9),
    ndn::Name(mappingRoot).append("v").appendNumber(7).appendSequenceNumber(9));
  BOOST_CHECK_EQUAL(
    nsf::makePredictiveGroupName(definition, 3),
    ndn::Name(mappingRoot).append("v").appendNumber(7)
                          .append("group").appendNumber(3));
  BOOST_CHECK_EQUAL(
    nsf::makePredictiveRepairName(definition, 3, 1),
    ndn::Name(mappingRoot).append("v").appendNumber(7)
                          .append("group").appendNumber(3)
                          .append("repair").appendNumber(1));
}

BOOST_AUTO_TEST_CASE(PredictiveDescriptorValidation)
{
  nsf::PredictiveStreamDescriptor d;
  d.frontierName = ndn::Name("/example/stream/frontier");
  // definition is default-constructed, will fail validation
  BOOST_CHECK(d.validate().has_value()); // invalid definition
  BOOST_CHECK(d.isPredictive());
}

BOOST_AUTO_TEST_CASE(PredictiveDescriptorRejectsNonCanonicalFrontier)
{
  nsf::LiveStreamDefinition definition;
  definition.contractVersion = nsf::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "descriptor-check";
  definition.provider = ndn::Name("/provider");
  definition.semanticDataPrefix =
    ndn::Name("/provider/stream/descriptor-check").appendVersion(3);
  definition.sessionEpoch = 3;
  definition.mappingVersion = 3;
  definition.samplePeriodMs = 20.0;
  definition.sampleClasses.push_back(
    nsf::SampleClassProfile::bounded("default", 1, 4));

  nsf::PredictiveStreamDescriptor descriptor;
  descriptor.definition = definition;
  descriptor.frontierName =
    nsf::makePredictiveFrontierName(definition.mappingRoot());
  BOOST_CHECK(!descriptor.validate().has_value());

  descriptor.frontierName.append("/attacker");
  BOOST_CHECK(descriptor.validate().has_value());
}

BOOST_AUTO_TEST_CASE(FECOptionsRejectUnknownScheme)
{
  nsf::LiveStreamFecOptions options;
  options.scheme = static_cast<nsf::LiveStreamFecScheme>(99);
  BOOST_CHECK(options.validate().has_value());
}

BOOST_AUTO_TEST_CASE(FECOptionsDefault)
{
  auto fec = nsf::LiveStreamFecOptions::none();
  BOOST_CHECK(!fec.enabled());
  BOOST_CHECK_EQUAL(fec.repairItemCount(), 0u);

  auto fecOn = nsf::LiveStreamFecOptions::xorOneRepair(10, 8000, 500);
  BOOST_CHECK(fecOn.enabled());
  BOOST_CHECK_EQUAL(fecOn.maxSourceItems, 10u);
  BOOST_CHECK_EQUAL(fecOn.repairItemCount(), 1u);
}

BOOST_AUTO_TEST_CASE(PredictiveFutureHorizonUsesGenericHalfWindow)
{
  using ndn_service_framework::detail::computePredictiveFutureCursorHorizon;

  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(128, 128), 64);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(192, 128), 64);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(64, 128), 32);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(4, 9), 2);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(3, 3), 1);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(1, 1), 1);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(0, 8), 0);
  BOOST_CHECK_EQUAL(computePredictiveFutureCursorHorizon(8, 0), 0);
}

BOOST_AUTO_TEST_CASE(PredictiveGroupCommitCanonicalRoundTrip)
{
  nsf::LiveStreamDefinition definition;
  definition.contractVersion = nsf::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "generic";
  definition.provider = ndn::Name("/provider");
  definition.semanticDataPrefix =
    ndn::Name("/provider/stream/generic").appendVersion(17);
  definition.sessionEpoch = 17;
  definition.mappingVersion = 17;
  definition.samplePeriodMs = 20.0;
  definition.sampleClasses.push_back(
    nsf::SampleClassProfile::bounded("block", 1, 4));
  definition.fec = nsf::LiveStreamFecOptions::xorOneRepair(4, 4096, 500);

  nsf::PredictiveStreamGroupCommit group;
  group.streamId = definition.streamId;
  group.sessionEpoch = definition.sessionEpoch;
  group.mappingVersion = definition.mappingVersion;
  group.groupId = 4;
  group.createdMs = 100;
  group.expiresMs = 600;
  group.sourceNames = {
    nsf::makePredictiveDataName(definition, 8),
    nsf::makePredictiveDataName(definition, 9),
  };
  group.sourceWireLengths = {11, 23};
  group.sourceWireDigests.resize(2);
  group.sourceWireDigests[0][0] = 0x11;
  group.sourceWireDigests[1][31] = 0x22;
  group.repairNames = {
    nsf::makePredictiveRepairName(definition, 4, 0),
  };
  group.recoveryCapacity = 1;

  BOOST_REQUIRE(!group.validate(definition));
  const auto wire = group.wireEncode();
  nsf::PredictiveStreamGroupCommit decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(!decoded.validate(definition));
  BOOST_CHECK(decoded.wireEncode() == wire);
  BOOST_CHECK_EQUAL(decoded.sourceWireLengths.at(0), 11);
  BOOST_CHECK_EQUAL(decoded.sourceWireLengths.at(1), 23);

  decoded.sourceNames[1] = nsf::makePredictiveDataName(definition, 11);
  BOOST_CHECK(decoded.validate(definition));
}

BOOST_AUTO_TEST_CASE(PredictiveFrontierCanonicalRoundTrip)
{
  nsf::LiveStreamDefinition definition;
  definition.contractVersion = nsf::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "generic";
  definition.provider = ndn::Name("/provider");
  definition.semanticDataPrefix =
    ndn::Name("/provider/stream/generic").appendVersion(17);
  definition.sessionEpoch = 17;
  definition.mappingVersion = 17;
  definition.samplePeriodMs = 20.0;
  definition.sampleClasses.push_back(
    nsf::SampleClassProfile::bounded("block", 1, 4));

  nsf::PredictiveStreamFrontier frontier;
  frontier.streamId = definition.streamId;
  frontier.sessionEpoch = definition.sessionEpoch;
  frontier.mappingVersion = definition.mappingVersion;
  frontier.checkpoint.initialSampleId = 0;
  frontier.checkpoint.oldestRetainedSampleId = 4;
  frontier.checkpoint.latestProducedSampleId = 9;
  frontier.checkpoint.nextExpectedSampleId = 10;
  frontier.latestCommittedGroupId = 4;
  frontier.retainedGroupCommitNames = {
    nsf::makePredictiveGroupName(definition, 3),
    nsf::makePredictiveGroupName(definition, 4),
  };
  frontier.retainedGroupFirstCursors = {4, 7};
  frontier.retainedGroupLastCursors = {6, 9};

  BOOST_REQUIRE(!frontier.validate(definition));
  const auto wire = frontier.wireEncode();
  nsf::PredictiveStreamFrontier decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(!decoded.validate(definition));
  BOOST_CHECK(decoded.wireEncode() == wire);

  decoded.checkpoint.nextExpectedSampleId = 8;
  BOOST_CHECK(decoded.validate(definition));
}

BOOST_AUTO_TEST_SUITE_END()
