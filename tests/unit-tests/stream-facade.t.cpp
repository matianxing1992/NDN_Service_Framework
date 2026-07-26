/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "ndn-service-framework/StreamFacade.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/boost-test.hpp"

#include <ndn-cxx/util/dummy-client-face.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(StreamFacade)

BOOST_AUTO_TEST_CASE(ConfigDefaults)
{
  StreamConfig config;
  config.streamId = "telemetry";
  config.dataPrefix = ndn::Name("/provider/streams/telemetry");
  config.samplePeriodMs = 50.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("single", 1, 1));
  config.sessionEpoch = 77;

  BOOST_CHECK_EQUAL(config.advanced.mappingBlockCapacity, 16);
  BOOST_CHECK_EQUAL(config.advanced.startupTimeoutMs, 1000);
  BOOST_CHECK_EQUAL(*config.sessionEpoch, 77);
}

BOOST_AUTO_TEST_CASE(SubscriptionDefaultsFollowDescriptor)
{
  LiveStreamDescriptor descriptor;
  descriptor.definition.contractVersion =
    STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  descriptor.definition.streamId = "audio";
  descriptor.definition.provider = ndn::Name("/provider");
  descriptor.definition.semanticDataPrefix =
    ndn::Name("/provider/audio").appendVersion(7);
  descriptor.definition.sessionEpoch = 7;
  descriptor.definition.mappingVersion = 1;
  descriptor.definition.samplePeriodMs = 20.0;
  descriptor.definition.sampleClasses.push_back(
    SampleClassProfile::bounded("block", 2, 4));
  descriptor.definition.fec =
    LiveStreamFecOptions::gf256TwoRepair(4, 1024);
  descriptor.measuredSamplePeriodMs = 20.0;
  descriptor.safeJoinCursor = 0;
  descriptor.checkpoint.blockNumber = 0;
  descriptor.checkpoint.frontiers.oldestRetained = 0;
  descriptor.checkpoint.frontiers.latestJoin = 0;
  descriptor.checkpoint.frontiers.latestProduced = 0;
  descriptor.checkpoint.frontiers.mappingCommittedThrough = 15;
  descriptor.checkpoint.frontiers.nextReserved = 16;

  StreamSubscriptionOptions options;
  options.onItem = [] (const VerifiedLiveStreamItem&) {
    return LiveStreamItemAdmission::acceptItem();
  };
  const auto lowLevel =
    detail::makeLiveStreamOpenOptions(descriptor, std::move(options));

  BOOST_CHECK(lowLevel.prefetchPolicy ==
              LiveStreamPrefetchPolicy::AdaptiveSampleAtomic);
  BOOST_CHECK(lowLevel.enableFecRecovery);
  BOOST_CHECK_EQUAL(lowLevel.aggregateInterestLimit, 64);
  BOOST_CHECK_EQUAL(lowLevel.interestLifetimeMs, 500);
}

BOOST_AUTO_TEST_CASE(ProviderFacadeBootstrapsAndReleasesSessionClaim)
{
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/test/spec147/provider");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec147/authority"));

  boost::asio::io_context io;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace face(io, keyChain, faceOptions);
  LocalServiceProvider provider(
    face, ndn::Name("/test/spec147/group"), providerCert, aaCert,
    "examples/trust-any.conf");

  std::atomic_bool running{true};
  std::thread faceThread([&] {
    while (running.load()) {
      face.processEvents(ndn::time::milliseconds(1));
      io.restart();
    }
  });
  struct FaceThreadGuard
  {
    std::atomic_bool& running;
    std::thread& thread;
    ~FaceThreadGuard()
    {
      running = false;
      if (thread.joinable()) {
        thread.join();
      }
    }
  } faceThreadGuard{running, faceThread};

  StreamConfig config;
  config.streamId = "telemetry";
  config.dataPrefix = ndn::Name(providerName).append("telemetry");
  config.samplePeriodMs = 50.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("single", 1, 1));
  config.sessionEpoch = 147;

  auto publisher = provider.createStream(config);
  const auto descriptor = publisher->start();
  BOOST_CHECK_EQUAL(descriptor.definition.contractVersion,
                    STREAM_NAME_MAP_CONTRACT_VERSION_V2);
  BOOST_CHECK_EQUAL(descriptor.definition.mappingVersion, 147);
  BOOST_CHECK_EQUAL(descriptor.definition.sessionEpoch, 147);
  BOOST_CHECK_EQUAL(
    descriptor.definition.semanticDataPrefix,
    ndn::Name(config.dataPrefix).appendVersion(147));
  BOOST_CHECK(publisher->status().state == LiveStreamLifecycleState::Active);

  BOOST_CHECK_THROW(provider.createStream(config), std::invalid_argument);
  publisher->stop();

  auto replacement = provider.createStream(config);
  replacement->stop();

  auto invalidEpoch = config;
  invalidEpoch.sessionEpoch = 0;
  BOOST_CHECK_THROW(provider.createStream(invalidEpoch), std::invalid_argument);

  running = false;
  faceThread.join();
}

BOOST_AUTO_TEST_CASE(ConsumerFacadeOpensAndStartsExistingHandle)
{
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/test/spec147/e2e/provider");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto userCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec147/e2e/user"));
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec147/e2e/authority"));

  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  auto forwardInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      boost::asio::post(
        providerIo,
        [&, interest] { providerFace.receive(interest); });
    });
  auto forwardData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { consumerFace.receive(data); });

  LocalServiceProvider provider(
    providerFace, ndn::Name("/test/spec147/e2e/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  LocalServiceUser user(
    consumerFace, ndn::Name("/test/spec147/e2e/group"), userCert, aaCert,
    "examples/trust-any.conf");

  std::atomic_bool providerRunning{true};
  std::thread providerThread([&] {
    while (providerRunning.load()) {
      providerFace.processEvents(ndn::time::milliseconds(1));
      providerIo.restart();
    }
  });
  struct ProviderThreadGuard
  {
    std::atomic_bool& running;
    std::thread& thread;
    ~ProviderThreadGuard()
    {
      running = false;
      if (thread.joinable()) {
        thread.join();
      }
    }
  } providerThreadGuard{providerRunning, providerThread};

  StreamConfig config;
  config.streamId = "sensor";
  config.dataPrefix = ndn::Name(providerName).append("sensor");
  config.samplePeriodMs = 50.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("single", 1, 1));
  config.sessionEpoch = 148;
  auto publisher = provider.createStream(config);
  const auto descriptor = publisher->start();

  providerRunning = false;
  providerThread.join();

  StreamSubscriptionOptions options;
  options.start = LiveStreamStart::Beginning;
  options.onItem = [] (const VerifiedLiveStreamItem&) {
    return LiveStreamItemAdmission::acceptItem();
  };
  auto handle = user.subscribeStream(descriptor, std::move(options));

  BOOST_CHECK(handle->status().state == LiveStreamLifecycleState::Active);

  handle->stop();
  publisher->stop();
}

BOOST_AUTO_TEST_CASE(PredictiveFacadeDescriptorUsesConfiguredSession)
{
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/test/spec147/parity/provider");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec147/parity/authority"));

  boost::asio::io_context facadeIo;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace facadeFace(facadeIo, keyChain, faceOptions);
  LocalServiceProvider provider(
    facadeFace, ndn::Name("/test/spec147/parity/group"), providerCert, aaCert,
    "examples/trust-any.conf");

  StreamConfig config;
  config.streamId = "generic";
  config.dataPrefix = ndn::Name(providerName).append("generic");
  config.samplePeriodMs = 25.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("block", 2, 4));
  config.sessionEpoch = 151;

  auto facade = provider.createStream(config);

  std::atomic_bool running{true};
  std::thread facadeThread([&] {
    while (running.load()) {
      facadeFace.processEvents(ndn::time::milliseconds(1));
      facadeIo.restart();
    }
  });
  const auto descriptor = facade->start();

  running = false;
  facadeThread.join();

  BOOST_CHECK_EQUAL(descriptor.definition.streamId, config.streamId);
  BOOST_CHECK_EQUAL(descriptor.definition.sessionEpoch, 151);
  BOOST_CHECK_EQUAL(descriptor.definition.mappingVersion, 151);
  BOOST_CHECK_EQUAL(descriptor.measuredSamplePeriodMs, 25.0);
  BOOST_CHECK_EQUAL(
    descriptor.frontierName,
    makePredictiveFrontierName(descriptor.definition.mappingRoot()));
  BOOST_CHECK(!descriptor.validate());

  facade->stop();
}

BOOST_AUTO_TEST_CASE(PredictiveProviderExactWireValidationAndAtomicFlush)
{
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/test/spec148/provider");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto attackerCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec148/attacker"));
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec148/authority"));

  boost::asio::io_context io;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace face(io, keyChain, faceOptions);
  LocalServiceProvider provider(
    face, ndn::Name("/test/spec148/group"), providerCert, aaCert,
    "examples/trust-any.conf");

  std::mutex observedMutex;
  std::condition_variable observedCondition;
  std::map<ndn::Name, ndn::Block> observedWires;
  auto observeData = face.onSendData.connect([&] (const ndn::Data& data) {
    {
      std::lock_guard<std::mutex> guard(observedMutex);
      observedWires[data.getName()] = data.wireEncode();
    }
    observedCondition.notify_all();
  });

  std::atomic_bool running{true};
  std::thread faceThread([&] {
    while (running.load()) {
      face.processEvents(ndn::time::milliseconds(1));
      io.restart();
    }
  });
  struct FaceThreadGuard
  {
    std::atomic_bool& running;
    std::thread& thread;
    ~FaceThreadGuard()
    {
      running = false;
      if (thread.joinable()) {
        thread.join();
      }
    }
  } faceThreadGuard{running, faceThread};

  StreamConfig config;
  config.streamId = "video";
  config.dataPrefix = ndn::Name(providerName).append("video");
  config.samplePeriodMs = 33.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("frame", 1, 4));
  config.fec = LiveStreamFecOptions::xorOneRepair(4, 4096, 500);
  config.sessionEpoch = 148;
  config.advanced.signedWireCap = 4096;

  auto publisher = provider.createStream(config);
  const auto descriptor = publisher->start();
  const auto makeSource = [&] (uint64_t sequence,
                               const std::vector<uint8_t>& content,
                               const ndn::security::Certificate& signer) {
    auto data = std::make_shared<ndn::Data>(
      makePredictiveDataName(descriptor.definition, sequence));
    data->setFreshnessPeriod(ndn::time::seconds(1));
    data->setContent(ndn::span<const uint8_t>(content.data(), content.size()));
    keyChain.sign(*data, ndn::security::signingByCertificate(signer));
    return data;
  };

  auto unsignedSource = std::make_shared<ndn::Data>(
    makePredictiveDataName(descriptor.definition, 0));
  const std::vector<uint8_t> unsignedBytes{0x00};
  unsignedSource->setContent(ndn::span<const uint8_t>(
    unsignedBytes.data(), unsignedBytes.size()));
  BOOST_CHECK_THROW(publisher->push(unsignedSource), std::invalid_argument);
  BOOST_CHECK(publisher->status().state == LiveStreamLifecycleState::Active);

  BOOST_CHECK_THROW(
    publisher->push(makeSource(0, {0x01}, attackerCert)),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    publisher->push(makeSource(1, {0x01}, providerCert)),
    std::invalid_argument);

  const auto source0 = makeSource(0, {0x10, 0x11, 0x12}, providerCert);
  const auto source1 = makeSource(
    1, {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26}, providerCert);
  const auto source0Wire = source0->wireEncode();
  const auto source1Wire = source1->wireEncode();
  publisher->push(source0);
  publisher->push(source1);
  BOOST_CHECK_NO_THROW(publisher->push(source1)); // exact duplicate is a no-op
  BOOST_CHECK_NO_THROW(publisher->flush());
  BOOST_CHECK_NO_THROW(publisher->flush()); // empty flush is a no-op

  const auto waitForData = [&] (const ndn::Name& name) {
    face.receive(ndn::Interest(name));
    std::unique_lock<std::mutex> lock(observedMutex);
    return observedCondition.wait_for(
      lock, std::chrono::seconds(1),
      [&] { return observedWires.count(name) != 0; });
  };

  BOOST_REQUIRE(waitForData(source0->getName()));
  BOOST_REQUIRE(waitForData(source1->getName()));
  {
    std::lock_guard<std::mutex> guard(observedMutex);
    BOOST_CHECK(observedWires.at(source0->getName()) == source0Wire);
    BOOST_CHECK(observedWires.at(source1->getName()) == source1Wire);
  }

  const auto groupName =
    makePredictiveGroupName(descriptor.definition, 0);
  const auto repairName =
    makePredictiveRepairName(descriptor.definition, 0, 0);
  BOOST_REQUIRE(waitForData(groupName));
  BOOST_REQUIRE(waitForData(repairName));
  BOOST_REQUIRE(waitForData(descriptor.frontierName));

  ndn::Data groupData;
  ndn::Data repairData;
  ndn::Data frontierData;
  {
    std::lock_guard<std::mutex> guard(observedMutex);
    groupData.wireDecode(observedWires.at(groupName));
    repairData.wireDecode(observedWires.at(repairName));
    frontierData.wireDecode(observedWires.at(descriptor.frontierName));
  }
  groupData.getContent().parse();
  repairData.getContent().parse();
  frontierData.getContent().parse();

  PredictiveStreamGroupCommit group;
  BOOST_REQUIRE(group.wireDecode(groupData.getContent().elements().at(0)));
  BOOST_REQUIRE(!group.validate(descriptor.definition));
  BOOST_CHECK_EQUAL(group.sourceWireLengths.at(0), source0Wire.size());
  BOOST_CHECK_EQUAL(group.sourceWireLengths.at(1), source1Wire.size());

  LiveStreamFecRepair repair;
  BOOST_REQUIRE(repair.wireDecode(repairData.getContent().elements().at(0)));
  std::vector<std::optional<std::vector<uint8_t>>> sources(2);
  sources[1] = std::vector<uint8_t>(source1Wire.begin(), source1Wire.end());
  const auto recovered = recoverLiveStreamXorSource(
    descriptor.definition, repair, sources, 0, streamNowMs());
  BOOST_REQUIRE(recovered);
  BOOST_CHECK_EQUAL_COLLECTIONS(
    recovered->begin(), recovered->end(), source0Wire.begin(), source0Wire.end());

  PredictiveStreamFrontier frontier;
  BOOST_REQUIRE(frontier.wireDecode(
    frontierData.getContent().elements().at(0)));
  BOOST_REQUIRE(!frontier.validate(descriptor.definition));
  BOOST_CHECK_EQUAL(frontier.checkpoint.latestProducedSampleId, 1);
  BOOST_CHECK_EQUAL(frontier.checkpoint.nextExpectedSampleId, 2);
  BOOST_REQUIRE(frontier.latestCommittedGroupId);
  BOOST_CHECK_EQUAL(*frontier.latestCommittedGroupId, 0);

  auto wrongPrefix = std::make_shared<ndn::Data>(
    ndn::Name("/outside/predictive/authority").appendSequenceNumber(2));
  const std::vector<uint8_t> wrongPrefixContent{0x01};
  wrongPrefix->setContent(ndn::span<const uint8_t>(
    wrongPrefixContent.data(), wrongPrefixContent.size()));
  keyChain.sign(*wrongPrefix,
                ndn::security::signingByCertificate(providerCert));
  BOOST_CHECK_THROW(publisher->push(wrongPrefix), std::invalid_argument);

  std::vector<uint8_t> oversizedContent(5000, 0x5a);
  BOOST_CHECK_THROW(
    publisher->push(makeSource(2, oversizedContent, providerCert)),
    std::length_error);

  const auto source2 = makeSource(2, {0x30, 0x31}, providerCert);
  std::exception_ptr pushFailure;
  std::exception_ptr flushFailure;
  std::thread pushThread([&] {
    try {
      publisher->push(source2);
    }
    catch (...) {
      pushFailure = std::current_exception();
    }
  });
  std::thread flushThread([&] {
    try {
      publisher->flush();
    }
    catch (...) {
      flushFailure = std::current_exception();
    }
  });
  pushThread.join();
  flushThread.join();
  BOOST_CHECK(!pushFailure);
  BOOST_CHECK(!flushFailure);
  BOOST_CHECK_NO_THROW(publisher->flush());

  const auto group1Name =
    makePredictiveGroupName(descriptor.definition, 1);
  BOOST_REQUIRE(waitForData(group1Name));
  ndn::Data group1Data;
  {
    std::lock_guard<std::mutex> guard(observedMutex);
    group1Data.wireDecode(observedWires.at(group1Name));
  }
  group1Data.getContent().parse();
  PredictiveStreamGroupCommit group1;
  BOOST_REQUIRE(group1.wireDecode(group1Data.getContent().elements().at(0)));
  BOOST_CHECK_EQUAL(group1.sourceNames.size(), 1);
  BOOST_CHECK_EQUAL(group1.sourceNames.front(), source2->getName());

  // A long stream must prune old recovery references before the signed
  // frontier exceeds the one-Data wire budget.
  for (uint64_t sequence = 3; sequence < 120; ++sequence) {
    publisher->push(makeSource(
      sequence, {static_cast<uint8_t>(sequence)}, providerCert));
    publisher->flush();
  }
  BOOST_CHECK(
    publisher->status().state == LiveStreamLifecycleState::Active);

  const auto equivocation =
    makeSource(0, {0x99, 0x98}, providerCert);
  BOOST_CHECK_THROW(publisher->push(equivocation), std::logic_error);
  BOOST_CHECK(publisher->status().state == LiveStreamLifecycleState::Failed);
  publisher->stop();
  BOOST_CHECK_THROW(publisher->push(source0), std::logic_error);

  running = false;
  faceThread.join();
}

BOOST_AUTO_TEST_CASE(PredictiveConsumerValidatesAndDeliversReorderedData)
{
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/test/spec148/e2e/provider");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto userCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec148/e2e/user"));
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/spec148/e2e/authority"));

  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  auto forwardInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      boost::asio::post(
        providerIo, [&, interest] { providerFace.receive(interest); });
    });

  std::mutex networkMutex;
  std::vector<ndn::Data> heldSources;
  bool droppedSourceZero = false;
  bool droppedSourceOne = false;
  bool droppedSourceEight = false;
  size_t sourceZeroSends = 0;
  size_t sourceOneSends = 0;
  size_t sourceEightSends = 0;
  auto reorderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      const auto& name = data.getName();
      const bool isSource =
        name.size() >= 3 && name[name.size() - 1].isSequenceNumber();
      if (!isSource) {
        boost::asio::post(
          consumerIo, [&, data] { consumerFace.receive(data); });
        return;
      }
      std::vector<ndn::Data> release;
      {
        std::lock_guard<std::mutex> guard(networkMutex);
        const auto sequence =
          name[name.size() - 1].toSequenceNumber();
        if (sequence == 0) {
          ++sourceZeroSends;
        }
        if (sequence == 1) {
          ++sourceOneSends;
        }
        if (sequence == 8) {
          ++sourceEightSends;
        }
        if (sequence == 0 && !droppedSourceZero) {
          droppedSourceZero = true;
          return;
        }
        if (sequence == 1 && !droppedSourceOne) {
          droppedSourceOne = true;
          return;
        }
        if (sequence == 8 && !droppedSourceEight) {
          droppedSourceEight = true;
          return;
        }
        heldSources.push_back(data);
        if (heldSources.size() == 2) {
          release.assign(heldSources.rbegin(), heldSources.rend());
          heldSources.clear();
        }
      }
      for (const auto& packet : release) {
        boost::asio::post(
          consumerIo, [&, packet] { consumerFace.receive(packet); });
      }
    });

  LocalServiceProvider provider(
    providerFace, ndn::Name("/test/spec148/e2e/group"),
    providerCert, aaCert, "examples/trust-any.conf");
  LocalServiceUser user(
    consumerFace, ndn::Name("/test/spec148/e2e/group"),
    userCert, aaCert, "examples/trust-any.conf");

  std::atomic_bool running{true};
  std::thread providerThread([&] {
    while (running.load()) {
      providerFace.processEvents(ndn::time::milliseconds(1));
      providerIo.restart();
    }
  });
  std::thread consumerThread([&] {
    while (running.load()) {
      consumerFace.processEvents(ndn::time::milliseconds(1));
      consumerIo.restart();
    }
  });
  struct ThreadsGuard
  {
    std::atomic_bool& running;
    std::thread& provider;
    std::thread& consumer;
    ~ThreadsGuard()
    {
      running = false;
      if (provider.joinable()) provider.join();
      if (consumer.joinable()) consumer.join();
    }
  } threadsGuard{running, providerThread, consumerThread};

  StreamConfig config;
  config.streamId = "generic";
  config.dataPrefix = ndn::Name(providerName).append("generic");
  config.samplePeriodMs = 20.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("block", 1, 4));
  config.fec = LiveStreamFecOptions::xorOneRepair(4, 4096, 2000);
  config.sessionEpoch = 149;
  auto publisher = provider.createStream(config);
  const auto descriptor = publisher->start();

  std::mutex deliveredMutex;
  std::condition_variable deliveredCondition;
  std::vector<uint64_t> delivered;
  std::vector<LiveStreamItemProvenance> provenances;
  StreamSubscriptionOptions options;
  options.start = LiveStreamStart::Beginning;
  options.aggregateInterestLimit = 8;
  options.interestLifetimeMs = 500;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    {
      std::lock_guard<std::mutex> guard(deliveredMutex);
      delivered.push_back(item.cursor);
      provenances.push_back(item.provenance);
    }
    deliveredCondition.notify_all();
    return LiveStreamItemAdmission::acceptItem();
  };
  auto subscriber = user.subscribeStream(descriptor, std::move(options));

  const auto makeSource = [&] (uint64_t sequence) {
    auto data = std::make_shared<ndn::Data>(
      makePredictiveDataName(descriptor.definition, sequence));
    const std::vector<uint8_t> content{
      static_cast<uint8_t>(sequence), 0xaa,
    };
    data->setContent(ndn::span<const uint8_t>(
      content.data(), content.size()));
    keyChain.sign(*data,
                  ndn::security::signingByCertificate(providerCert));
    return data;
  };
  for (uint64_t sequence = 0; sequence < 4; ++sequence) {
    publisher->push(makeSource(sequence));
  }
  publisher->flush();
  for (uint64_t sequence = 4; sequence < 8; ++sequence) {
    publisher->push(makeSource(sequence));
  }
  publisher->flush();
  for (uint64_t sequence = 8; sequence < 12; ++sequence) {
    publisher->push(makeSource(sequence));
  }
  publisher->flush();

  {
    std::unique_lock<std::mutex> lock(deliveredMutex);
    BOOST_REQUIRE(deliveredCondition.wait_for(
      lock, std::chrono::seconds(3),
      [&] { return delivered.size() == 12; }));
    const std::vector<uint64_t> expected{
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(
      delivered.begin(), delivered.end(), expected.begin(), expected.end());
    BOOST_REQUIRE_EQUAL(provenances.size(), 12);
    // Cursors 0 and 1 are missing concurrently in one XOR-one group. That
    // exceeds its recovery capacity, so both must leave recovery and succeed
    // through their bounded source retry instead of one attempt being stranded
    // by group-shared repair bookkeeping.
    BOOST_CHECK(
      provenances.at(8) == LiveStreamItemProvenance::FecRecovered ||
      provenances.at(8) == LiveStreamItemProvenance::SignedData);
  }
  LiveStreamStatus status;
  const auto statusDeadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  do {
    status = subscriber->status();
    if (status.delivered == 12) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < statusDeadline);
  BOOST_CHECK_EQUAL(status.delivered, 12);
  BOOST_CHECK_EQUAL(status.payloadSourceDataAdmissions, 12);
  BOOST_CHECK_EQUAL(status.rejected, 0);
  BOOST_CHECK_LE(status.recovered, status.recoveryAttempts);
  // The three deliberately dropped published cursors must enter recovery.
  // With predictive names, one not-yet-published cursor can also time out and
  // enter recovery before this status snapshot.  Its presence is scheduling
  // dependent, but the aggregate limit keeps the total bounded.
  BOOST_CHECK_GE(status.recoveryAttempts, 3);
  BOOST_CHECK_LE(
    status.recoveryAttempts,
    uint64_t{3} + uint64_t{8});
  BOOST_CHECK_EQUAL(
    status.recoveryExhaustions + status.recovered,
    status.recoveryAttempts);
  BOOST_CHECK_EQUAL(status.mappingInterests, 0);
  BOOST_CHECK_EQUAL(status.recoveryFrontierInterests, 1);
  BOOST_CHECK_EQUAL(status.recoveryGroupInterests, 2);
  BOOST_CHECK_EQUAL(status.recoveryControlInterests, 3);
  // Both losses share one validated frontier snapshot. Depending on callback
  // ordering, the second cursor either joins the in-flight frontier Interest
  // or uses the freshly cached snapshot, so recoveryCoalescedWaiters is not a
  // deterministic requirement here.
  BOOST_CHECK_EQUAL(status.nextDeliverCursor, 12);
  BOOST_CHECK_EQUAL(status.readyQueueDepth, 0);
  BOOST_CHECK_EQUAL(status.terminalGapQueueDepth, 0);
  BOOST_CHECK_GE(status.drainWakeCount, 1);
  BOOST_REQUIRE(status.fetchDecision);
  BOOST_CHECK_EQUAL(
    status.futureCursorHorizon,
    std::min<uint64_t>(status.fetchDecision->lookahead, uint64_t{8}));
  {
    std::lock_guard<std::mutex> guard(networkMutex);
    BOOST_CHECK_GE(sourceZeroSends, 1);
    BOOST_CHECK_LE(sourceZeroSends, 3);
    BOOST_CHECK_GE(sourceOneSends, 1);
    BOOST_CHECK_LE(sourceOneSends, 3);
    BOOST_CHECK_GE(sourceEightSends, 1);
    BOOST_CHECK_LE(sourceEightSends, 2);
  }

  // With no source after sequence 11, the predictive subscriber may keep only
  // a bounded set of future Interests pending/retried.  The adaptive
  // concurrency window must not be mistaken for an unbounded cursor horizon.
  std::this_thread::sleep_for(std::chrono::milliseconds(1600));
  status = subscriber->status();
  BOOST_CHECK_LE(status.payloadInterests, 72);
  BOOST_CHECK_LE(status.futurePayloadInterests, 64);

  subscriber->stop();
  publisher->push(makeSource(8));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::lock_guard<std::mutex> guard(deliveredMutex);
    BOOST_CHECK_EQUAL(delivered.size(), 12);
  }
  publisher->stop();
  running = false;
  providerThread.join();
  consumerThread.join();
}

BOOST_AUTO_TEST_CASE(PredictiveTerminalGapAdvancesOrderedDrain)
{
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/test/spec150/provider");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto userCert =
    makeRsaIdentity(keyChain, ndn::Name("/test/spec150/user"));
  const auto aaCert =
    makeRsaIdentity(keyChain, ndn::Name("/test/spec150/authority"));

  boost::asio::io_context providerIo;
  boost::asio::io_context consumerIo;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(providerIo, keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, faceOptions);
  auto forwardInterest = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      boost::asio::post(
        providerIo, [&, interest] { providerFace.receive(interest); });
    });
  auto forwardData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) {
      const auto& name = data.getName();
      const bool isSource =
        name.size() >= 3 && name[name.size() - 1].isSequenceNumber();
      if (isSource &&
          name[name.size() - 1].toSequenceNumber() == 0) {
        return;
      }
      boost::asio::post(
        consumerIo, [&, data] { consumerFace.receive(data); });
    });

  LocalServiceProvider provider(
    providerFace, ndn::Name("/test/spec150/group"),
    providerCert, aaCert, "examples/trust-any.conf");
  LocalServiceUser user(
    consumerFace, ndn::Name("/test/spec150/group"),
    userCert, aaCert, "examples/trust-any.conf");

  std::atomic_bool running{true};
  std::thread providerThread([&] {
    while (running.load()) {
      providerFace.processEvents(ndn::time::milliseconds(1));
      providerIo.restart();
    }
  });
  std::thread consumerThread([&] {
    while (running.load()) {
      consumerFace.processEvents(ndn::time::milliseconds(1));
      consumerIo.restart();
    }
  });
  struct ThreadsGuard
  {
    std::atomic_bool& running;
    std::thread& provider;
    std::thread& consumer;
    ~ThreadsGuard()
    {
      running = false;
      if (provider.joinable()) provider.join();
      if (consumer.joinable()) consumer.join();
    }
  } threadsGuard{running, providerThread, consumerThread};

  StreamConfig config;
  config.streamId = "ordered";
  config.dataPrefix = ndn::Name(providerName).append("ordered");
  config.samplePeriodMs = 20.0;
  config.sampleClasses.push_back(
    SampleClassProfile::bounded("single", 1, 1));
  config.fec = LiveStreamFecOptions::none();
  config.sessionEpoch = 150;
  auto publisher = provider.createStream(config);
  const auto descriptor = publisher->start();

  std::mutex deliveredMutex;
  std::condition_variable deliveredCondition;
  std::vector<uint64_t> delivered;
  StreamSubscriptionOptions options;
  options.start = LiveStreamStart::Beginning;
  options.aggregateInterestLimit = 8;
  options.interestLifetimeMs = 50;
  options.enableFecRecovery = false;
  options.onItem = [&] (const VerifiedLiveStreamItem& item) {
    {
      std::lock_guard<std::mutex> guard(deliveredMutex);
      delivered.push_back(item.cursor);
    }
    deliveredCondition.notify_all();
    return LiveStreamItemAdmission::acceptItem();
  };
  auto subscriber = user.subscribeStream(descriptor, std::move(options));

  for (uint64_t sequence = 0; sequence < 8; ++sequence) {
    auto data = std::make_shared<ndn::Data>(
      makePredictiveDataName(descriptor.definition, sequence));
    const std::vector<uint8_t> content{static_cast<uint8_t>(sequence)};
    data->setContent(ndn::span<const uint8_t>(
      content.data(), content.size()));
    keyChain.sign(*data,
                  ndn::security::signingByCertificate(providerCert));
    publisher->push(data);
    publisher->flush();
  }

  {
    std::unique_lock<std::mutex> lock(deliveredMutex);
    BOOST_REQUIRE(deliveredCondition.wait_for(
      lock, std::chrono::seconds(3),
      [&] { return delivered.size() == 7; }));
    const std::vector<uint64_t> expected{1, 2, 3, 4, 5, 6, 7};
    BOOST_CHECK_EQUAL_COLLECTIONS(
      delivered.begin(), delivered.end(), expected.begin(), expected.end());
  }
  const auto status = subscriber->status();
  // The subscriber may already have declared and drained the next unproduced
  // future cursor before this asynchronous snapshot is taken.
  BOOST_CHECK_GE(status.nextDeliverCursor, 8);
  BOOST_CHECK_EQUAL(status.readyQueueDepth, 0);
  BOOST_CHECK_EQUAL(status.terminalGapQueueDepth, 0);
  BOOST_CHECK_GE(status.terminalMissingSources, 1);
  BOOST_CHECK_GE(status.drainWakeCount, 1);

  subscriber->stop();
  publisher->stop();
  running = false;
  providerThread.join();
  consumerThread.join();
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
