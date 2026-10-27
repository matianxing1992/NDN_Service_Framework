#include <NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp>
#include <ndn-service-framework/ServiceUser.hpp>
#include <ndn-service-framework/controller-certificate.hpp>

// This diagnostic consumes the installed implementation through its existing private declaration.
#include "../../NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeRequestOptionsTransport.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

using Json = ndnsf::di::NativeJson;

std::vector<std::uint8_t>
readBytes(const std::filesystem::path& path)
{
  if (!path.is_absolute() || path != std::filesystem::canonical(path) ||
      !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 1024 * 1024)
    throw std::runtime_error("probe input must be a bounded canonical regular file");
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("probe input is unavailable");
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

Json
readJson(const std::filesystem::path& path)
{
  const auto bytes = readBytes(path);
  return Json::parse(bytes.begin(), bytes.end());
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc != 2)
      throw std::invalid_argument("usage: installed-ack-probe ORIGINAL_DEPLOYMENT_JSON");
    const auto config = readJson(argv[1]);
    const auto& native = config.at("requester");
    const auto& request = native.at("request");
    const auto& network = config.at("network");
    std::string requester;
    std::string controller;
    for (const auto& actor : config.at("actors")) {
      if (actor.at("kind") == "requester")
        requester = actor.at("identity").get<std::string>();
      if (actor.at("kind") == "controller")
        controller = actor.at("identity").get<std::string>();
    }
    if (requester.empty() || controller.empty() || request.at("ack_timeout_ms") != 1000)
      throw std::runtime_error("missing identity or changed ACK condition");
    const auto model = ndnsf::di::NativeModelDescriptor::fromCanonicalJson(
      native.at("catalog").at("model").dump());
    ndnsf::di::NativeApplicationInput input;
    input.taskName = request.at("task").get<std::string>();
    input.inputSchemaDigest = model.adapter.inputSchemaDigest;
    input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
    input.payload = readBytes(native.at("turns").at(0).at("input_file").get<std::string>());
    input.options = readBytes(native.at("turns").at(0).at("options_file").get<std::string>());
    ndnsf::di::NativeRequestContract contract;
    contract.serviceName = network.at("service").get<std::string>();
    contract.taskName = input.taskName;
    contract.adapterName = model.adapterId;
    contract.adapterDescriptorDigest = model.adapter.descriptorDigest();
    contract.adapterCompositionDigest = request.at("adapter_composition_digest");
    contract.taskDescriptorDigest = request.at("task_descriptor_digest");
    contract.generationMode = request.at("generation_mode");
    contract.tokenizerDigest = request.at("tokenizer_digest");
    std::vector<std::string> providers = request.at("provider_names");
    std::vector<ndn::Name> providerNames;
    for (const auto& provider : providers)
      providerNames.emplace_back(provider);

    ndn::Face face;
    const auto* pib = std::getenv("NDN_CLIENT_PIB");
    const auto* tpm = std::getenv("NDN_CLIENT_TPM");
    if (pib == nullptr || tpm == nullptr || *pib == '\0' || *tpm == '\0')
      throw std::runtime_error("explicit original role PIB/TPM locators required");
    ndn::KeyChain keyChain{std::string(pib), std::string(tpm)};
    const auto certificate = keyChain.getPib().getIdentity(ndn::Name(requester))
      .getDefaultKey().getDefaultCertificate();
    const auto authority = ndn_service_framework::loadControllerCertificate(
      keyChain, ndn::Name(controller),
      config.at("security").at("controller_certificate").get<std::string>().c_str());
    ndn_service_framework::ServiceUser user(face,
      ndn::Name(network.at("group").get<std::string>()), certificate, certificate,
      authority, config.at("security").at("trust_schema").get<std::string>(), keyChain);
    user.init();
    user.fetchPermissionsFromController(ndn::Name(controller));
    const auto started = std::chrono::steady_clock::now();
    while (!user.isRequestBootstrapReady(ndn::Name(contract.serviceName), providerNames)) {
      if (std::chrono::steady_clock::now() - started > std::chrono::seconds(10))
        throw std::runtime_error("ACK_PROBE_BOOTSTRAP_TIMEOUT");
      face.processEvents(ndn::time::milliseconds(50), true);
    }
    const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    ndn::Name requestId(requester);
    requestId.append("ACK-PROBE").appendNumber(static_cast<std::uint64_t>(epochMs));
    const auto encoded = ndnsf::di::detail::encodeNativeRequestEnvelopeForRuntime(
      model, input, contract, requestId.toUri(), 1,
      epochMs + request.at("timeout_ms").get<std::uint64_t>(), std::nullopt,
      providers, std::nullopt, std::nullopt, requester, "SELECTION_GENERATION_V1");
    bool closed = false;
    Json result;
    const ndn::Buffer requestWire(encoded.wire.begin(), encoded.wire.end());
    const auto published = std::chrono::steady_clock::now();
    user.BeginCollaborationWithProviders(ndn::Name(contract.serviceName), requestWire,
      1000, request.at("timeout_ms").get<int>(),
      [&] (const ndn_service_framework::CollaborationAckClosure& closure) {
        result = {{"phase", "ACK_CLOSED"}, {"requestId", closure.requestId.toUri()},
          {"digest", closure.digest}, {"closedAtUs", closure.closedAtUs},
          {"requestDeadlineUs", closure.requestDeadlineUs}, {"candidates", Json::array()}};
        for (const auto& candidate : closure.candidates) {
          const auto& evidence = candidate.authenticationEvidence;
          result["candidates"].push_back({{"provider", candidate.providerName.toUri()},
            {"service", candidate.serviceName.toUri()},
            {"requestId", candidate.requestId.toUri()}, {"status", candidate.ack.getStatus()},
            {"message", candidate.ack.getMessage()}, {"ackObservedAtMs", candidate.ackObservedAtMs},
            {"ackRttMs", candidate.ackRttMs ? Json(*candidate.ackRttMs) : Json(nullptr)},
            {"signerIdentity", evidence.signerIdentity},
            {"signerKeyLocator", evidence.signerKeyLocator},
            {"wireDigest", evidence.wireDigest},
            {"trustSchemaValidated", evidence.trustSchemaValidated}});
        }
        closed = true;
      }, [] (const auto&) {}, [] (const auto&) {}, requestId, {},
      ndn_service_framework::RequestCapabilities(), std::nullopt, {}, {}, {}, providerNames);
    while (!closed && std::chrono::steady_clock::now() - published < std::chrono::seconds(3))
      face.processEvents(ndn::time::milliseconds(50), true);
    const bool cancelled = user.CancelCollaboration(requestId);
    if (!closed || !cancelled)
      throw std::runtime_error("ACK_PROBE_CLOSURE_OR_CANCEL_MISSING");
    result["wallSeconds"] = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - published).count();
    result["scope"] = "real V3 ACK only; no prepared model reference or V4 inventory";
    result["requestContractDigest"] = encoded.requestContractDigest;
    result["cancelled"] = cancelled;
    std::cout << result.dump() << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "ACK_PROBE_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
