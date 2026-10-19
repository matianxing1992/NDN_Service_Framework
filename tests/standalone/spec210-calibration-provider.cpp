#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace nsf = ndn_service_framework;

namespace {

std::atomic<bool> g_stop{false};

void
onSignal(int)
{
  g_stop.store(true);
}

ndn::security::Certificate
getIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
}

ndn::Buffer
readBytes(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open calibration fixture: " + path);
  }
  const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
  if (bytes.empty()) {
    throw std::runtime_error("calibration fixture is empty: " + path);
  }
  return ndn::Buffer(bytes.begin(), bytes.end());
}

void
writeAll(const std::string& path, const std::string& value)
{
  const auto temporary = path + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot create calibration reference receipt");
    }
    output << value;
    output.flush();
    if (!output) {
      throw std::runtime_error("cannot write calibration reference receipt");
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    throw std::runtime_error("cannot atomically publish calibration reference receipt");
  }
}

std::string
sha256File(const std::string& path)
{
  ndn::util::Sha256 digest;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read source-generation manifest: " + path);
  }
  const std::string bytes(std::istreambuf_iterator<char>(input), {});
  digest << bytes;
  return "sha256:" + digest.toString();
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc != 8) {
    std::cerr << "usage: spec210-calibration-provider TRUST_SCHEMA RECEIPT "
                 "GENERATION_MANIFEST PAYLOAD_UAV1 PAYLOAD_UAV2 PAYLOAD_UAV3 "
                 "CONTROLLER_PREFIX\n";
    return 2;
  }

  try {
    const ndn::Name identity("/example/uav/calibration-provider");
    const ndn::Name controllerName(argv[7]);
    const ndn::Name service("/UAV/Tracking/Window");
    const auto generationDigest = sha256File(argv[3]);

    ndn::Face face;
    ndn::KeyChain keyChain;
    const auto providerCert = getIdentity(keyChain, identity);
    const auto controllerCert = getIdentity(keyChain, controllerName);
    nsf::CertificatePublisher certificatePublisher(face, keyChain, providerCert.getName());
    nsf::ServiceProvider provider(face, ndn::Name("/example/uav"), providerCert,
                                  controllerCert, argv[1]);
    provider.setPerformanceMode(true);
    provider.setUseTokens(true);

    std::vector<ndn::Buffer> payloads;
    for (int camera = 1; camera <= 3; ++camera) {
      payloads.push_back(readBytes(argv[camera + 3]));
    }

    std::map<std::string, std::pair<ndn::Name, std::string>> firstPublication;
    std::mutex publicationMutex;
    unsigned invocationCount = 0;
    provider.addCollaborationHandler(
      service, std::vector<nsf::CollaborationRole>{"Calibration-Provider"},
      [] (const nsf::RequestMessage&) {
        nsf::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "external-calibration-ready";
        return decision;
      },
      [&] (nsf::ServiceProvider::CollaborationContext& context,
           const nsf::RequestMessage&) {
        try {
          if (context.role() != "Calibration-Provider" ||
              context.localProvider() != identity || !context.hasCurrentWorkAuthority()) {
            throw std::runtime_error("Core did not dispatch current Calibration-Provider work");
          }

          std::vector<std::string> referenceRows;
          referenceRows.reserve(3);
          for (size_t index = 0; index < 3; ++index) {
            const auto cameraId = std::string("UAV") + std::to_string(index + 1);
            const ndn::Name semanticPrefix = ndn::Name(identity).append("UAV")
              .append("CALIBRATION").append(cameraId).append("1");
            const auto result = context.publishImmutableProtectedData(
              semanticPrefix, payloads[index]);
            if (!result.success) {
              throw std::runtime_error("Core immutable calibration publication failed: " +
                                       result.errorMessage);
            }
            if (result.dataName.getPrefix(-1) != semanticPrefix ||
                !result.dataName.get(-1).isVersion() || result.dataName.get(-1).toVersion() == 0) {
              throw std::runtime_error("Core changed calibration producer-owned name shape");
            }

            bool reused = false;
            {
              std::lock_guard<std::mutex> lock(publicationMutex);
              const auto previous = firstPublication.find(cameraId);
              if (previous == firstPublication.end()) {
                firstPublication.emplace(cameraId,
                                         std::make_pair(result.dataName, result.contentSha256));
              }
              else {
                reused = previous->second.first == result.dataName &&
                         previous->second.second == result.contentSha256;
                if (!reused) {
                  throw std::runtime_error("cross-request calibration reference changed");
                }
              }
            }
            referenceRows.push_back(
              "    \"" + cameraId + "\": {\"exactName\": \"" + result.dataName.toUri() +
              "\", \"contentSha256\": \"" + result.contentSha256 +
              "\", \"producerIdentity\": \"" + identity.toUri() + "\"}");
            std::cout << "SPEC210_EXTERNAL_CALIBRATION_REFERENCE camera=" << cameraId
                      << " name=" << result.dataName.toUri()
                      << " producer=" << identity.toUri()
                      << " digest=" << result.contentSha256
                      << " reused=" << (reused ? "true" : "false") << std::endl;
          }

          std::string receipt =
            "{\n  \"schema\": \"spec210-external-calibration-provider/v1\",\n"
            "  \"providerIdentity\": \"" + identity.toUri() +
            "\",\n  \"role\": \"" + context.role() + "\",\n  \"references\": {\n";
          for (size_t index = 0; index < referenceRows.size(); ++index) {
            receipt += referenceRows[index] + (index + 1 == referenceRows.size() ? "\n" : ",\n");
          }
          receipt += "  }\n}\n";
          writeAll(argv[2], receipt);

          {
            std::lock_guard<std::mutex> lock(publicationMutex);
            ++invocationCount;
          }
          std::cout << "SPEC210_EXTERNAL_CALIBRATION_ROLE_COMPLETE invocation="
                    << invocationCount << std::endl;
          context.completeRole();
        }
        catch (const std::exception& error) {
          std::cerr << "SPEC210_EXTERNAL_CALIBRATION_ROLE_FAILED " << error.what() << std::endl;
          context.fail(error.what());
        }
      });

    provider.init();
    provider.fetchPermissionsFromController(controllerName);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!g_stop.load() && std::chrono::steady_clock::now() < readyDeadline &&
           (!provider.isRequestDecryptionReady() || !provider.isNacProducerReadyForTest())) {
      face.processEvents(ndn::time::milliseconds(10));
      face.getIoContext().restart();
    }
    if (!provider.isRequestDecryptionReady() || !provider.isNacProducerReadyForTest()) {
      throw std::runtime_error("controller did not install calibration-provider role permission");
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::cout << "UAV_OBSERVATION_SOURCE_ADAPTER_READY generation-manifest-sha256="
              << generationDigest << " calibration-provider=" << identity.toUri() << std::endl;
    while (!g_stop.load()) {
      face.processEvents(ndn::time::milliseconds(100));
      face.getIoContext().restart();
    }
    std::cout << "SPEC210_EXTERNAL_CALIBRATION_PROVIDER_EXIT invocations="
              << invocationCount << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC210_EXTERNAL_CALIBRATION_PROVIDER_START_FAILED " << error.what() << std::endl;
    return 1;
  }
}
