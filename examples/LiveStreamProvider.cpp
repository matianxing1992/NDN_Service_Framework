#include "ndn-service-framework/ServiceProvider.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace nsf = ndn_service_framework;

static ndn::security::Certificate
identity(ndn::KeyChain& keyChain, const ndn::Name& name)
{
  try {
    return keyChain.getPib().getIdentity(name).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(name).getDefaultKey().getDefaultCertificate();
  }
}

static std::string
hex(const nsf::StreamContentDigest& digest)
{
  static const char digits[] = "0123456789abcdef";
  std::string value;
  for (const auto byte : digest) {
    value.push_back(digits[byte >> 4]);
    value.push_back(digits[byte & 0x0f]);
  }
  return value;
}

int
main(int argc, char** argv)
{
  const std::string descriptorPath = argc > 1 ? argv[1] : "/tmp/live-stream.json";
  ndn::Face face;
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/example/live/provider");
  const ndn::Name controllerName("/example/live/controller");
  const auto providerCert = identity(keyChain, providerName);
  const auto controllerCert = identity(keyChain, controllerName);
  nsf::ServiceProvider provider(face, "/example/live/group", providerCert,
                                controllerCert, "examples/trust-schema.conf");

  nsf::LiveStreamDefinition definition;
  definition.contractVersion = nsf::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "binary-demo";
  definition.provider = providerName;
  definition.semanticDataPrefix = ndn::Name("/example/live/provider/samples").appendVersion(1);
  definition.sessionEpoch = 1;
  definition.mappingVersion = 1;
  definition.mappingBlockCapacity = 4;
  definition.mappingAheadBlocks = 2;
  definition.retainedItems = 32;
  definition.maxNameReservations = 256;
  definition.samplePeriodMs = 33.0;
  definition.sampleClasses = {
    nsf::SampleClassProfile::bounded("demo", 2, 4, 8, 1),
  };
  auto publisher = provider.createLiveStream(definition);

  face.processEvents(ndn::time::milliseconds(30));
  face.getIoContext().restart();
  const auto publishSample = [&] (uint64_t sampleId) {
    const auto reservation = publisher->announceSample(
      sampleId, "demo", [&] (size_t index, nsf::LiveStreamItemKind) {
        return ndn::Name(definition.semanticDataPrefix)
          .append("sample").appendSequenceNumber(sampleId).appendSegment(index);
      });
    const auto count = 1 + sampleId % 3;
    std::vector<std::vector<uint8_t>> content;
    for (size_t index = 0; index < count; ++index) {
      content.push_back({static_cast<uint8_t>(sampleId),
                         static_cast<uint8_t>(index), 0x00, 0xa5});
    }
    publisher->publishSample(reservation, content);
    return reservation;
  };
  const auto first = publishSample(0);
  const auto descriptor = publisher->activate(
    {33.0, first.group.sources.front().cursor});

  boost::property_tree::ptree out;
  out.put("streamId", definition.streamId);
  out.put("contractVersion", definition.contractVersion);
  out.put("provider", definition.provider.toUri());
  out.put("semanticDataPrefix", definition.semanticDataPrefix.toUri());
  out.put("sessionEpoch", definition.sessionEpoch);
  out.put("mappingVersion", definition.mappingVersion);
  out.put("mappingBlockCapacity", definition.mappingBlockCapacity);
  out.put("mappingAheadBlocks", definition.mappingAheadBlocks);
  out.put("retainedItems", definition.retainedItems);
  out.put("maxNameReservations", definition.maxNameReservations);
  out.put("maxPendingInterests", definition.maxPendingInterests);
  out.put("signedWireCap", definition.signedWireCap);
  out.put("samplePeriodMs", definition.samplePeriodMs);
  out.put("measuredSamplePeriodMs", descriptor.measuredSamplePeriodMs);
  out.put("safeJoinCursor", descriptor.safeJoinCursor);
  out.put("checkpoint.blockNumber", descriptor.checkpoint.blockNumber);
  out.put("checkpoint.digest", hex(descriptor.checkpoint.contentDigest));
  out.put("checkpoint.oldestRetained", descriptor.checkpoint.frontiers.oldestRetained);
  out.put("checkpoint.latestJoin", descriptor.checkpoint.frontiers.latestJoin);
  out.put("checkpoint.latestProduced", descriptor.checkpoint.frontiers.latestProduced);
  out.put("checkpoint.mappingCommittedThrough",
          descriptor.checkpoint.frontiers.mappingCommittedThrough);
  out.put("checkpoint.nextReserved", descriptor.checkpoint.frontiers.nextReserved);
  boost::property_tree::write_json(descriptorPath, out);

  for (uint64_t i = 1; i < 8; ++i) {
    publishSample(i);
    face.processEvents(ndn::time::milliseconds(33));
    face.getIoContext().restart();
  }
  face.processEvents(ndn::time::seconds(5));
  publisher->stop();
  std::cout << "published semantic-name opaque stream; descriptor="
            << descriptorPath << std::endl;
}
