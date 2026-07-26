#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <atomic>
#include <fstream>
#include <iostream>

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

static nsf::StreamContentDigest
unhex(const std::string& text)
{
  nsf::StreamContentDigest digest{};
  if (text.size() != digest.size() * 2) throw std::invalid_argument("bad digest");
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(std::stoul(text.substr(i * 2, 2), nullptr, 16));
  }
  return digest;
}

int
main(int argc, char** argv)
{
  const std::string descriptorPath = argc > 1 ? argv[1] : "/tmp/live-stream.json";
  boost::property_tree::ptree input;
  boost::property_tree::read_json(descriptorPath, input);

  nsf::LiveStreamDescriptor descriptor;
  auto& definition = descriptor.definition;
  definition.contractVersion = input.get<uint64_t>("contractVersion");
  definition.streamId = input.get<std::string>("streamId");
  definition.provider = input.get<std::string>("provider");
  definition.semanticDataPrefix = input.get<std::string>("semanticDataPrefix");
  definition.sessionEpoch = input.get<uint64_t>("sessionEpoch");
  definition.mappingVersion = input.get<uint64_t>("mappingVersion");
  definition.mappingBlockCapacity = input.get<size_t>("mappingBlockCapacity");
  definition.mappingAheadBlocks = input.get<size_t>("mappingAheadBlocks");
  definition.retainedItems = input.get<size_t>("retainedItems");
  definition.maxNameReservations = input.get<size_t>("maxNameReservations");
  definition.maxPendingInterests = input.get<size_t>("maxPendingInterests");
  definition.signedWireCap = input.get<size_t>("signedWireCap");
  definition.samplePeriodMs = input.get<double>("samplePeriodMs");
  definition.sampleClasses = {
    nsf::SampleClassProfile::bounded("demo", 2, 4, 8, 1),
  };
  descriptor.measuredSamplePeriodMs = input.get<double>("measuredSamplePeriodMs");
  descriptor.safeJoinCursor = input.get<uint64_t>("safeJoinCursor");
  descriptor.checkpoint.blockNumber = input.get<uint64_t>("checkpoint.blockNumber");
  descriptor.checkpoint.contentDigest = unhex(input.get<std::string>("checkpoint.digest"));
  auto& frontiers = descriptor.checkpoint.frontiers;
  frontiers.oldestRetained = input.get<uint64_t>("checkpoint.oldestRetained");
  frontiers.latestJoin = input.get<uint64_t>("checkpoint.latestJoin");
  frontiers.latestProduced = input.get<uint64_t>("checkpoint.latestProduced");
  frontiers.mappingCommittedThrough =
    input.get<uint64_t>("checkpoint.mappingCommittedThrough");
  frontiers.nextReserved = input.get<uint64_t>("checkpoint.nextReserved");

  ndn::Face face;
  ndn::KeyChain keyChain;
  const auto userCert = identity(keyChain, "/example/live/user");
  const auto controllerCert = identity(keyChain, "/example/live/controller");
  nsf::ServiceUser user(face, "/example/live/group", userCert,
                        controllerCert, "examples/trust-schema.conf");
  std::atomic<size_t> accepted{0};
  nsf::LiveStreamOpenOptions options;
  options.start = nsf::LiveStreamStart::Latest;
  options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  options.aggregateInterestLimit = 16;
  options.onItem = [&accepted] (const nsf::VerifiedLiveStreamItem& item) {
    ++accepted;
    std::cout << item.cursor << " " << item.originalName << " "
              << item.content.size() << std::endl;
    return nsf::LiveStreamItemAdmission::acceptItem();
  };
  auto stream = user.openLiveStream(descriptor, std::move(options));
  stream->start();
  face.processEvents(ndn::time::seconds(5));
  stream->stop();
  return accepted.load() == 0 ? 2 : 0;
}
