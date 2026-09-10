#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <openssl/pem.h>
#include <csignal>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace ndnsf::di;
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }
std::vector<std::uint8_t> read(const std::filesystem::path& path, std::uint64_t limit)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("requester input file is unavailable");
  const auto size = in.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > limit)
    throw std::runtime_error("requester input file exceeds limit");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.seekg(0);
  if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
    throw std::runtime_error("requester input file read failed");
  return bytes;
}
std::string text(const std::filesystem::path& path, std::uint64_t limit = 1024 * 1024)
{
  const auto bytes = read(path, limit);
  return {bytes.begin(), bytes.end()};
}
std::shared_ptr<EVP_PKEY> key(const std::filesystem::path& path, bool privateKey)
{
  auto bytes = read(path, 65536);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(bytes.data(), bytes.size()), BIO_free);
  EVP_PKEY* value = bio ? (privateKey ? PEM_read_bio_PrivateKey(bio.get(), nullptr,
      [](char*, int, int, void*) { return 0; }, nullptr)
                                    : PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr)) : nullptr;
  if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
  if (!value) throw std::runtime_error("requester key could not be loaded");
  return {value, EVP_PKEY_free};
}
std::string publicBytes(EVP_PKEY& key)
{
  std::string bytes(32, '\0'); std::size_t size = bytes.size();
  if (EVP_PKEY_get_raw_public_key(&key, reinterpret_cast<unsigned char*>(bytes.data()), &size) != 1 || size != 32)
    throw std::runtime_error("requester requires an Ed25519 authority key");
  return bytes;
}

NativeJson runtimeConfiguration(const NativeJson& config,
                                const NativeRequestCatalog& catalog,
                                const std::string& requester,
                                const std::string& protectionEpoch)
{
  const auto& request = config.at("request");
  const auto& model = catalog.model.descriptor;
  // The CLI owns only composition.  Runtime policy is parsed and bound by
  // the same native boundary used by the public binding, so this adapter
  // cannot silently construct a partially pinned runtime.
  return NativeJson{
    {"schema", "ndnsf-di-native-request-runtime-v1"},
    {"contract", {
      {"service_name", request.at("service")},
      {"task_name", request.at("task")},
      {"adapter_name", model.adapterId},
      {"adapter_descriptor_digest", model.adapter.descriptorDigest()},
      {"adapter_composition_digest", request.at("adapter_composition_digest")},
      {"task_descriptor_digest", request.at("task_descriptor_digest")},
      {"generation_mode", request.value("generation_mode", "TOKEN_DIAGNOSTIC")},
      {"tokenizer_digest", request.value("tokenizer_digest", std::string{})},
    }},
    {"requester_identity", requester},
    {"protection_epoch", protectionEpoch},
    {"input_layout_digest", request.at("input_layout_digest")},
    {"security", {
      {"policy_digest", request.at("security_policy_digest")},
      {"require_protected_artifacts", true},
    }},
    {"budget", {
      {"max_candidates", request.at("max_candidates")},
      {"max_policy_ms", request.at("max_policy_ms")},
      {"max_reentries", request.value("max_reentries", 1)},
    }},
    {"state_mapping", {
      {"inputs", catalog.stateMapping.inputs},
      {"outputs", catalog.stateMapping.outputs},
    }},
    {"no_progress_ms", request.value("no_progress_ms", 5000)},
    {"max_segments", request.value("max_segments", 4096)},
  };
}
}

int main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: DI_NativeRequester --config FILE --input FILE --output FILE\n"
                 "Config schema: ndnsf-di-native-requester-v1; input/output paths are relative to the current working directory, other config paths to the config file.\n"
                 "Uses existing PIB identities and the native request pipeline.\n";
    return 0;
  }
  if (argc != 7 || std::string(argv[1]) != "--config" || std::string(argv[3]) != "--input" ||
      std::string(argv[5]) != "--output") {
    std::cerr << "Usage: DI_NativeRequester --config FILE --input FILE --output FILE\n";
    return 2;
  }
  try {
    using namespace ndnsf::di;
    const auto base = std::filesystem::absolute(argv[2]).parent_path();
    const auto config = nativeParseJson(text(argv[2]));
    if (config.at("schema") != "ndnsf-di-native-requester-v1")
      throw std::invalid_argument("unsupported requester configuration");
    const auto& catalogConfig = config.at("catalog");
    const auto& limits = config.at("limits");
    const auto bootstrapMs = limits.at("bootstrap_ms").get<std::uint64_t>();
    if (!bootstrapMs || bootstrapMs > 3600000) throw std::invalid_argument("invalid bootstrap time budget");
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bootstrapMs);
    NativeAssemblyControl control{bootstrapDeadline, [bootstrapDeadline] {
      if (std::chrono::steady_clock::now() >= bootstrapDeadline)
        throw std::runtime_error("requester bootstrap deadline expired");
    }, limits.at("max_source_bytes"), limits.at("max_assembled_bytes")};
    NativeCanonicalSource source;
    source.modelBytes = read(base / catalogConfig.at("source").at("file").get<std::string>(), control.maxSourceBytes);
    if (catalogConfig.at("source").contains("initializer_file"))
      source.initializerBytes = read(base / catalogConfig.at("source").at("initializer_file").get<std::string>(), control.maxSourceBytes);
    const auto catalog = NativeRequestCatalog::load(nativeCanonicalJson(catalogConfig), std::move(source), control);
    const auto& grant = config.at("grant");
    if (grant.contains("authority_private_key_file") || grant.contains("content_key_file") ||
        grant.contains("content_key_id")) {
      throw std::invalid_argument(
        "requester configuration must not contain authority signing or model content keys");
    }
    if (!grant.contains("authority_service") || !grant.contains("authority_public_key_file")) {
      throw std::invalid_argument(
        "requester configuration requires authority_service and authority_public_key_file");
    }
    const auto epoch = grant.at("protection_epoch").get<std::string>();
    const auto requesterKey = key(base / grant.at("requester_private_key_file").get<std::string>(), true);
    // The requester owns only its signing key and the authority verification
    // key.  Authority signing/content keys are deliberately absent from this
    // configuration and process; grants arrive through the independent native
    // authority service below.
    const auto authorityPublicKey = key(
      base / grant.at("authority_public_key_file").get<std::string>(), false);
    const auto& offer = config.at("offer_admission");
    std::map<std::string, std::string> offerKeys;
    for (const auto& entry : offer.at("public_key_files").items())
      offerKeys.emplace(entry.key(), text(base / entry.value().get<std::string>(), 65536));
    auto admission = std::make_shared<NativeOfferAdmission>(nativeCanonicalJson(offer.at("policy")), offerKeys,
      offer.at("candidate_digest").get<std::string>());
    auto face = std::make_shared<ndn::Face>();
    ndn::security::KeyChain keyChain;
    const auto& core = config.at("core");
    const auto requester = core.at("requester_identity").get<std::string>();
    auto user = std::shared_ptr<ndn_service_framework::ServiceUser>(
      new ndn_service_framework::ServiceUser(*face, ndn::Name(core.at("group").get<std::string>()),
      keyChain.getPib().getIdentity(ndn::Name(requester)).getDefaultKey().getDefaultCertificate(),
      keyChain.getPib().getIdentity(ndn::Name(core.at("authority_identity").get<std::string>())).getDefaultKey().getDefaultCertificate(),
      (base / core.at("trust_schema_file").get<std::string>()).string()),
      [face](auto* owner) { delete owner; });
    const auto& request = config.at("request");
    const auto& model = catalog.model.descriptor;
    std::shared_ptr<NativeConversationCoordinator> conversations;
    if (config.contains("conversation")) {
      conversations = nativeConversationCoordinatorFromConfig(
        nativeCanonicalJson(config.at("conversation")), base,
        requester);
    }
    auto grants = std::make_shared<NativeAuthenticatedGrantClient>(requester, requesterKey,
      grant.at("authority_identity"), publicBytes(*authorityPublicKey),
      epoch,
      NativeAuthenticatedGrantClient::issueThroughCore(
        user, grant.at("authority_identity").get<std::string>(),
        grant.at("authority_service").get<std::string>()),
      NativeAuthenticatedGrantClient::publishThroughCore(user));
    const auto runtime = nativeRequestRuntimeFromJson(
      nativeCanonicalJson(runtimeConfiguration(config, catalog, requester, epoch)),
      catalog, grants);
    NativeInferenceClient client(user, catalog.preparation->adapters(), runtime,
      std::move(conversations),
      catalog.preparation->makePreparation(user, runtime.contract.serviceName), admission);
    NativeModelRef modelRef;
    static_cast<NativeModelDescriptor&>(modelRef) = model;
    NativeApplicationInput input;
    input.taskName = runtime.contract.taskName; input.inputSchemaDigest = model.adapter.inputSchemaDigest;
    input.optionsSchemaDigest = model.adapter.optionsSchemaDigest; input.payload = read(argv[4], 4 * 1024 * 1024);
    if (request.contains("options_file")) input.options = read(base / request.at("options_file").get<std::string>(), 4 * 1024 * 1024);
    NativeRequestOptions options;
    options.timeoutMs = request.at("timeout_ms"); options.ackTimeoutMs = request.at("ack_timeout_ms");
    if (request.contains("application_request_id"))
      options.applicationRequestId = request.at("application_request_id").get<std::string>();
    if (runtime.contract.generationMode == "TOKEN_STREAMING") {
      if (input.options.empty())
        throw std::invalid_argument("TOKEN_STREAMING requester requires options_file");
      const auto applicationOptions = nativeParseJson(
        std::string(input.options.begin(), input.options.end()));
      const auto generationId = applicationOptions.value(
        "generationId", applicationOptions.value("generation_id", std::string{}));
      if (generationId.size() != 32 ||
          generationId.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("TOKEN_STREAMING options require lowercase 16-byte generationId");
      options.outputMode = "TOKEN_STREAMING";
      options.generation = nativeGenerationFromOptions(input.options, generationId);
      options.stream = ndn_service_framework::StreamRequestOptions{};
      for (std::size_t i = 0; i < options.stream->generationId.size(); ++i)
        options.stream->generationId[i] = static_cast<std::uint8_t>(
          std::stoul(generationId.substr(i * 2, 2), nullptr, 16));
      const auto maxTokens = options.generation->maxGeneratedTokens;
      options.stream->maxEvents = static_cast<std::uint32_t>(maxTokens + 1);
      options.stream->interestWindow = static_cast<std::uint16_t>(
        std::min<std::size_t>(64, std::max<std::size_t>(1, maxTokens + 1)));
      options.stream->callbackQueueCapacity = static_cast<std::uint16_t>(
        std::max<std::size_t>(16, maxTokens + 1));
      options.stream->reorderCapacity = options.stream->interestWindow;
    }
    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
    user->init();
    auto handle = client.request(modelRef, input, catalog.splitter, std::make_shared<NativePreSplitFirstPlacement>(), options);
    while (handle.status() == NativeRequestStatus::Pending) {
      if (interrupted) handle.cancel();
      face->processEvents(ndn::time::milliseconds(20));
    }
    client.close();
    face->processEvents(ndn::time::milliseconds(1));
    const auto result = handle.result(std::chrono::milliseconds(0));
    std::ofstream output(argv[6], std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(result.payload.data()), result.payload.size()))
      throw std::runtime_error("requester output could not be written");
    std::cout << "NATIVE_REQUEST_SUCCEEDED request=" << handle.requestId() << " plan=" << result.planDigest << '\n';
    return 0;
  }
  catch (const ndnsf::di::NativeDiError& error) {
    std::cerr << error.code() << " boundary=" << error.boundary() << '\n';
    return interrupted ? 130 : 1;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_REQUESTER_FAILED: " << error.what() << '\n';
    return 1;
  }
}
