#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <openssl/pem.h>
#include <cmath>
#include <csignal>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

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

std::optional<NativeConversationContinuation> conversationContinuationFromConfig(
  const NativeJson& conversation, const std::filesystem::path& base)
{
  if (!conversation.contains("turn")) return std::nullopt;
  const auto& turn = conversation.at("turn");
  if (!turn.is_object())
    throw std::invalid_argument("native conversation turn must be an object");

  NativeConversationContinuation continuation;
  continuation.mode = turn.value("mode", "FULL_CONTEXT");
  continuation.generationId = turn.at("generation_id").get<std::string>();
  if (turn.contains("parent_state_file")) {
    const auto state = nativeParseJson(text(
      base / turn.at("parent_state_file").get<std::string>(), 4 * 1024 * 1024));
    if (state.value("schema", std::string{}) != "ndnsf-di-native-conversation-state-v1" ||
        !state.contains("checkpoint_wire") || !state.contains("transcript"))
      throw std::invalid_argument("native conversation parent state is malformed");
    continuation.parentCheckpointWire = state.at("checkpoint_wire").get<std::string>();
    const auto checkpoint = nativeParseJson(continuation.parentCheckpointWire);
    continuation.conversationId = checkpoint.at("conversationId").get<std::string>();
    continuation.parentContextEpoch = checkpoint.at("contextEpoch").get<std::uint64_t>();
    continuation.serviceName = checkpoint.at("serviceName").get<std::string>();
    continuation.planRoleMapDigest = checkpoint.at("planRoleMapDigest").get<std::string>();
    continuation.parentCheckpointDigest = checkpoint.at("checkpointDigest").get<std::string>();
    continuation.retentionDeadlineMs = checkpoint.at("expiresAtMs").get<std::uint64_t>();
    const auto& transcript = state.at("transcript");
    continuation.canonicalTokenIds = transcript.at("canonicalTokenIds").get<std::vector<std::int64_t>>();
    for (const auto& item : checkpoint.at("roleReceiptDigests").items())
      continuation.expectedRoles.push_back(item.key());
    if (turn.contains("delta_token_ids")) {
      const auto delta = turn.at("delta_token_ids").get<std::vector<std::int64_t>>();
      continuation.canonicalTokenIds.insert(
        continuation.canonicalTokenIds.end(), delta.begin(), delta.end());
    }
  }
  else {
    continuation.conversationId = turn.at("conversation_id").get<std::string>();
    continuation.parentContextEpoch = turn.value("parent_context_epoch", std::uint64_t{0});
    continuation.serviceName = turn.at("service_name").get<std::string>();
    continuation.planRoleMapDigest = turn.at("plan_role_map_digest").get<std::string>();
    continuation.parentCheckpointDigest = turn.value("parent_checkpoint_digest", std::string{});
    continuation.retentionDeadlineMs = turn.at("retention_deadline_ms").get<std::uint64_t>();
    if (turn.contains("parent_checkpoint_wire"))
      continuation.parentCheckpointWire = turn.at("parent_checkpoint_wire").get<std::string>();
    if (turn.contains("canonical_token_ids"))
      continuation.canonicalTokenIds = turn.at("canonical_token_ids").get<std::vector<std::int64_t>>();
    if (turn.contains("expected_roles"))
      continuation.expectedRoles = turn.at("expected_roles").get<std::vector<std::string>>();
  }
  if (turn.contains("request_contract_digest"))
    continuation.requestContractDigest = turn.at("request_contract_digest").get<std::string>();
  if (turn.contains("parent_context_epoch"))
    continuation.parentContextEpoch = turn.at("parent_context_epoch").get<std::uint64_t>();
  if (turn.contains("parent_checkpoint_digest"))
    continuation.parentCheckpointDigest = turn.at("parent_checkpoint_digest").get<std::string>();
  if (turn.contains("service_name"))
    continuation.serviceName = turn.at("service_name").get<std::string>();
  if (turn.contains("plan_role_map_digest"))
    continuation.planRoleMapDigest = turn.at("plan_role_map_digest").get<std::string>();
  if (turn.contains("retention_deadline_ms"))
    continuation.retentionDeadlineMs = turn.at("retention_deadline_ms").get<std::uint64_t>();
  if (turn.contains("canonical_token_ids"))
    continuation.canonicalTokenIds = turn.at("canonical_token_ids").get<std::vector<std::int64_t>>();
  if (turn.contains("expected_roles"))
    continuation.expectedRoles = turn.at("expected_roles").get<std::vector<std::string>>();
  return continuation;
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
    const auto conversationOwner = conversations;
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
    if (config.contains("conversation")) {
      const auto continuation = conversationContinuationFromConfig(config.at("conversation"), base);
      if (continuation) options.conversation = *continuation;
    }
    if (request.contains("application_request_id"))
      options.applicationRequestId = request.at("application_request_id").get<std::string>();
    if (request.contains("provider_names")) {
      if (!request.at("provider_names").is_array())
        throw std::invalid_argument("request.provider_names must be an array");
      for (const auto& value : request.at("provider_names")) {
        if (!value.is_string())
          throw std::invalid_argument("request.provider_names entries must be strings");
        const auto provider = value.get<std::string>();
        if (provider.empty() || provider.front() != '/')
          throw std::invalid_argument("request.provider_names entries must be absolute names");
        options.providerNames.emplace_back(provider);
      }
    }
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
      const auto allowReplacement = request.value("allow_replacement", false);
      const auto maxReplacements = request.value(
        "max_replacements", static_cast<unsigned>(allowReplacement ? 1 : 0));
      if (maxReplacements > 1 || (!allowReplacement && maxReplacements != 0) ||
          (allowReplacement && maxReplacements != 1))
        throw std::invalid_argument(
          "request replacement options require allow_replacement=true and max_replacements=1");
      options.stream->allowReplacement = allowReplacement;
      options.stream->maxReplacements = static_cast<std::uint8_t>(maxReplacements);
    }
    std::vector<std::int64_t> streamOracleExpected;
    std::vector<std::int64_t> streamOracleObserved;
    if (config.contains("stream_oracle")) {
      if (runtime.contract.generationMode != "TOKEN_STREAMING")
        throw std::invalid_argument("stream_oracle requires TOKEN_STREAMING");
      const auto& oracle = config.at("stream_oracle");
      if (!oracle.is_object() || !oracle.contains("token_ids") ||
          !oracle.at("token_ids").is_array())
        throw std::invalid_argument("stream_oracle.token_ids must be an array");
      streamOracleExpected = oracle.at("token_ids").get<std::vector<std::int64_t>>();
      if (streamOracleExpected.empty())
        throw std::invalid_argument("stream_oracle.token_ids must not be empty");
      options.onGenerationEvent = [&streamOracleExpected, &streamOracleObserved] (
                                    const std::vector<std::uint8_t>& bytes) {
        const auto event = nativeParseJson(std::string(bytes.begin(), bytes.end()));
        if (!event.is_object() || event.value("schema", std::string{}) != "GenerationTokenEventV1" ||
            !event.contains("tokenId") || !event.at("tokenId").is_number_integer() ||
            !event.contains("tokenEpoch") || !event.at("tokenEpoch").is_number_unsigned())
          throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: malformed event");
        const auto index = streamOracleObserved.size();
        if (index >= streamOracleExpected.size() ||
            event.at("tokenId").get<std::int64_t>() != streamOracleExpected[index] ||
            event.at("tokenEpoch").get<std::uint64_t>() != index + 1)
          throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: event sequence mismatch");
        streamOracleObserved.push_back(event.at("tokenId").get<std::int64_t>());
      };
    }
    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
    user->init();
    // User permissions are an explicit Controller-signed input to the Core
    // request path.  Bootstrap them before publishing the deferred
    // collaboration; otherwise the requester enters ACK collection without
    // an authorized service and closes an empty candidate set.
    user->fetchPermissionsFromController(
      ndn::Name(core.at("authority_identity").get<std::string>()));
    const auto requestService = ndn::Name(request.at("service").get<std::string>());
    bool permissionReady = false;
    while (!interrupted && std::chrono::steady_clock::now() < bootstrapDeadline) {
      const auto allowed = user->getAllowedServices();
      permissionReady = user->getCurrentPolicyEpoch(requestService) != 0 &&
        user->getControllerVersion(requestService).has_value() &&
        std::any_of(allowed.begin(), allowed.end(),
          [&requestService](const auto& entry) {
            return std::get<1>(entry) == requestService.toUri();
          });
      if (permissionReady) break;
      face->processEvents(ndn::time::milliseconds(20));
    }
    if (!permissionReady) {
      throw NativeDiError("NATIVE_REQUEST_PERMISSION_BOOTSTRAP_FAILED", "runtime",
        "permission-bootstrap", "requester did not receive current Controller permission",
        {}, 0);
    }
    auto handle = client.request(modelRef, input, catalog.splitter, std::make_shared<NativePreSplitFirstPlacement>(), options);
    while (handle.status() == NativeRequestStatus::Pending) {
      if (interrupted) handle.cancel();
      face->processEvents(ndn::time::milliseconds(20));
    }
    client.close();
    face->processEvents(ndn::time::milliseconds(1));
    const auto result = handle.result(std::chrono::milliseconds(0));
    if (conversationOwner && config.at("conversation").contains("checkpoint_output_file")) {
      if (!options.conversation)
        throw std::runtime_error("native conversation checkpoint output requires a turn");
      const auto conversationId = options.conversation->conversationId;
      const auto record = conversationOwner->find(conversationId);
      if (!record)
        throw std::runtime_error("native conversation checkpoint was not committed");
      const NativeJson state{
        {"schema", "ndnsf-di-native-conversation-state-v1"},
        {"checkpoint_wire", record->checkpoint.wire},
        {"transcript", record->checkpoint.transcript}};
      const auto statePath = base / config.at("conversation").at("checkpoint_output_file").get<std::string>();
      std::ofstream stateFile(statePath, std::ios::binary | std::ios::trunc);
      if (!stateFile || !(stateFile << nativeCanonicalJson(state)))
        throw std::runtime_error("native conversation checkpoint state could not be written");
      stateFile.close();
      (void)::chmod(statePath.c_str(), 0600);
      std::cout << "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN id=" << conversationId
                << " epoch=" << record->checkpoint.successorContextEpoch << '\n';
    }
    std::ofstream output(argv[6], std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(result.payload.data()), result.payload.size()))
      throw std::runtime_error("requester output could not be written");
    if (config.contains("oracle")) {
      const auto& oracle = config.at("oracle");
      const auto tensorName = oracle.at("tensor").get<std::string>();
      const auto expected = oracle.at("float32").get<std::vector<float>>();
      const auto tolerance = oracle.value("tolerance", 1e-5);
      if (tensorName.empty() || expected.empty() || !std::isfinite(tolerance) || tolerance < 0.0)
        throw std::invalid_argument("requester numerical oracle is invalid");
      const auto tensors = decodeTensorBundle(
        std::vector<std::uint8_t>(result.payload.begin(), result.payload.end()));
      const auto& tensor = findTensor(tensors, tensorName);
      if (tensor.elementType != TensorElementType::Float32 ||
          tensor.payload.size() != expected.size() * sizeof(float))
        throw std::runtime_error("NATIVE_NUMERICAL_ORACLE_FAILED: tensor type or size mismatch");
      for (std::size_t i = 0; i < expected.size(); ++i) {
        float actual = 0.0F;
        std::memcpy(&actual, tensor.payload.data() + i * sizeof(float), sizeof(float));
        if (!std::isfinite(actual) || std::fabs(static_cast<double>(actual) - expected[i]) > tolerance)
          throw std::runtime_error("NATIVE_NUMERICAL_ORACLE_FAILED: tensor value mismatch");
      }
      std::cout << "NATIVE_NUMERICAL_ORACLE_PASS tensor=" << tensorName
                << " values=";
      for (std::size_t i = 0; i < expected.size(); ++i)
        std::cout << (i == 0 ? "" : ",") << expected[i];
      std::cout << '\n';
    }
    if (config.contains("stream_oracle")) {
      const auto value = nativeParseJson(std::string(result.payload.begin(), result.payload.end()));
      const auto expected = config.at("stream_oracle").at("token_ids").get<std::vector<std::int64_t>>();
      if (!value.is_object() || value.value("schema", std::string{}) != "NDNSF-DI-FINAL-V1" ||
          !value.contains("tokenIds") || value.at("tokenIds").get<std::vector<std::int64_t>>() != expected ||
          streamOracleObserved != expected)
        throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: final sequence mismatch");
      std::cout << "NATIVE_STREAM_ORACLE_PASS tokens=";
      for (std::size_t i = 0; i < expected.size(); ++i)
        std::cout << (i == 0 ? "" : ",") << expected[i];
      std::cout << " events=" << streamOracleObserved.size() << '\n';
    }
    std::cout << "NATIVE_REQUEST_SUCCEEDED request=" << handle.requestId() << " plan=" << result.planDigest << '\n';
    return 0;
  }
  catch (const ndnsf::di::NativeDiError& error) {
    std::cerr << error.code() << " boundary=" << error.boundary()
              << " message=" << error.what() << '\n';
    return interrupted ? 130 : 1;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_REQUESTER_FAILED: " << error.what() << '\n';
    return 1;
  }
}
