#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <atomic>
#include <iostream>
#include <thread>

namespace nsf = ndn_service_framework;

static ndn::security::Certificate
identity(ndn::KeyChain& keyChain, const ndn::Name& name)
{
  try {
    return keyChain.getPib().getIdentity(name)
      .getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(name, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

int
main()
{
  ndn::Face face;
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/example/live/provider");
  const auto providerCert = identity(keyChain, providerName);
  nsf::ServiceProvider provider(
    face, "/example/live/group", providerCert,
    identity(keyChain, "/example/live/controller"),
    "examples/trust-schema.conf");

  std::atomic_bool running{true};
  std::thread faceThread([&] {
    while (running.load()) {
      face.processEvents(ndn::time::milliseconds(10));
    }
  });

  nsf::StreamConfig config;
  config.streamId = "binary-demo";
  config.dataPrefix = ndn::Name(providerName).append("samples");
  config.samplePeriodMs = 33.0;
  config.sampleClasses = {
    nsf::SampleClassProfile::bounded("demo", 2, 4, 8, 1),
  };

  auto stream = provider.createStream(config);
  const auto descriptor = stream->start();
  std::cout << "stream prefix: "
            << descriptor.definition.semanticDataPrefix << std::endl;

  for (uint64_t sequence = 0; sequence < 8; ++sequence) {
    const auto name =
      nsf::makePredictiveDataName(descriptor.definition, sequence);
    auto data = std::make_shared<ndn::Data>(name);
    const uint8_t content[] = {
      static_cast<uint8_t>(sequence), 0x00,
    };
    data->setFreshnessPeriod(ndn::time::seconds(1));
    data->setContent(ndn::span<const uint8_t>(content));
    keyChain.sign(*data, ndn::security::signingByCertificate(providerCert));
    stream->push(std::move(data));
    stream->flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  stream->stop();
  running = false;
  faceThread.join();
}
