#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/RuntimeTestAccess.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace ndnsf::di;

// Sanitizer instrumentation stretches the borrowed-Face cleanup path while
// preserving the same drain contract. Keep the normal test budget tight but
// give the instrumented selector enough wall-clock time to reach its real
// terminal barrier instead of turning tool overhead into a leaked fixture.
std::chrono::seconds testDrainTimeout()
{
#if defined(__SANITIZE_ADDRESS__)
  return std::chrono::seconds(5);
#elif defined(__clang__)
#  if __has_feature(address_sanitizer)
  return std::chrono::seconds(5);
#  endif
#endif
  return std::chrono::seconds(2);
}

template<typename Future>
void pumpUntilFutureReady(ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
                          Future& future,
                          std::chrono::seconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    // pumpUntil() is intentionally chunked to keep existing fixture callers
    // bounded.  Repeat complete chunks while the result future is pending,
    // and let the fixture stop at the first completed result so it does not
    // process unrelated queued events after the request's terminal edge.
    environment.pumpUntil([&future] {
      return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
  }
  if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    throw std::runtime_error("Spec185 conversation result pump deadline expired");
}

// Keep the synchronous busy-turn exception boundary out of the large
// integration test frame.  The production Conversation guard remains the
// subject under test; this helper only keeps the expected exception object
// and its unwinding in a small, independently protected C++ frame.
bool conversationRejectsBusyTurn(Conversation& conversation,
                                 const RequestOptions& options)
{
  try {
    (void)conversation.request(Input::inlineBytes({0x09}), options);
  }
  catch (const DiError& error) {
    return error.code() == "CONVERSATION_TURN_IN_PROGRESS";
  }
  return false;
}

void writeEd25519KeyPair(const std::filesystem::path& privatePath,
                         const std::filesystem::path& publicPath)
{
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
    EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1)
    throw std::runtime_error("cannot initialize fixture key generator");
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw) != 1)
    throw std::runtime_error("cannot generate fixture key");
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> privateBio(
    BIO_new_file(privatePath.c_str(), "wb"), BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> publicBio(
    BIO_new_file(publicPath.c_str(), "wb"), BIO_free);
  if (!privateBio || !publicBio ||
      PEM_write_bio_PrivateKey(privateBio.get(), key.get(), nullptr, nullptr, 0,
                               nullptr, nullptr) != 1 ||
      PEM_write_bio_PUBKEY(publicBio.get(), key.get()) != 1)
    throw std::runtime_error("cannot write fixture key");
  std::filesystem::permissions(privatePath,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace);
}

std::string publicKeyDigest(const std::filesystem::path& path)
{
  std::ifstream input(path);
  const std::string pem((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
    PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  if (!key) throw std::runtime_error("cannot read fixture public key");
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  if (EVP_PKEY_get_raw_public_key(key.get(),
        reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != raw.size())
    throw std::runtime_error("fixture public key is not Ed25519");
  return nativePlanningDigest(raw);
}

std::shared_ptr<EVP_PKEY> deterministicEd25519Key(unsigned char seed)
{
  std::array<unsigned char, 32> bytes{};
  bytes.fill(seed);
  auto* raw = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                            bytes.data(), bytes.size());
  if (!raw) throw std::runtime_error("cannot create fixture Ed25519 key");
  return {raw, EVP_PKEY_free};
}

std::string rawPublicKey(const std::shared_ptr<EVP_PKEY>& key)
{
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  if (!key || EVP_PKEY_get_raw_public_key(key.get(),
        reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != raw.size())
    throw std::runtime_error("cannot extract fixture Ed25519 public key");
  return raw;
}

std::string publicKeyPem(const std::shared_ptr<EVP_PKEY>& key)
{
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), key.get()) != 1)
    throw std::runtime_error("cannot encode fixture Ed25519 public key");
  char* data = nullptr;
  const auto size = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, size > 0 ? static_cast<std::size_t>(size) : 0);
}

std::string signDigest(const std::shared_ptr<EVP_PKEY>& key, const std::string& value)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
    throw std::runtime_error("cannot initialize fixture offer signer");
  std::array<unsigned char, 64> signature{};
  std::size_t size = signature.size();
  if (EVP_DigestSign(context.get(), signature.data(), &size,
                     reinterpret_cast<const unsigned char*>(value.data()), value.size()) != 1 ||
      size != signature.size())
    throw std::runtime_error("cannot sign fixture provider offer");
  std::array<unsigned char, 89> encoded{};
  if (EVP_EncodeBlock(encoded.data(), signature.data(), static_cast<int>(size)) != 88)
    throw std::runtime_error("cannot encode fixture provider offer signature");
  return std::string(reinterpret_cast<const char*>(encoded.data()), 88);
}

class RuntimeFixture
{
public:
  explicit RuntimeFixture(bool streaming = false, bool qwenStreaming = false)
    : root(std::filesystem::temp_directory_path() /
           ("spec185-prepared-request-" + std::to_string(::getpid()) + "-" +
            std::to_string(++sequence)))
  {
    std::filesystem::create_directories(root);
    write("trust.conf",
      "; Spec185 prepared request fixture\n"
      "rule\n"
      "{\n"
      "  id \"Testbed Validation Rule\"\n"
      "  for data\n"
      "  checker\n"
      "  {\n"
      "    type hierarchical\n"
      "    sig-type rsa-sha256\n"
      "  }\n"
      "}\n"
      "trust-anchor\n"
      "{\n"
      "  type any\n"
      "}\n");
    writeEd25519KeyPair(root / "requester.pem", root / "requester-public.pem");
    writeEd25519KeyPair(root / "authority.pem", root / "authority-public.pem");
    writeEd25519KeyPair(root / "offer.pem", root / "offer-public.pem");
    {
      std::ofstream key(root / "conversation.key", std::ios::binary);
      const std::string bytes(32, 'k');
      key.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      key.close();
      std::filesystem::permissions(root / "conversation.key",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    }

    NativeCanonicalSource source;
    NativeModelDescriptor descriptor;
    NativeJson catalog = NativeJson::object();
    const auto control = NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
      1 << 20, 1 << 20};
    if (qwenStreaming) {
      std::ifstream qwenFile("tests/fixtures/spec182/qwen-native-config.onnx", std::ios::binary);
      if (!qwenFile.good()) throw std::runtime_error("missing Qwen native config fixture");
      const std::string qwenBytes((std::istreambuf_iterator<char>(qwenFile)),
                                  std::istreambuf_iterator<char>());
      source.modelBytes.assign(qwenBytes.begin(), qwenBytes.end());
      const auto sourceIdentity = canonicalOnnxSourceIdentity(source, control);
      const std::string role = "/LLM/Pipeline/Stage/0";
      const std::string manifestDigest = nativePlanningDigest("spec185-qwen-manifest");
      const std::string protectionEpoch = "epoch-1";
      // The provider fixture's grant publication source intentionally binds the
      // shared fixture profile; keep the Qwen source/model identity distinct
      // while using the same authorized artifact profile at this boundary.
      const std::string artifactProfileDigest = nativePlanningDigest("fixture-profile");
      const std::string qwenGraphDigest =
        "sha256:8f901f484f07440aef844373c95d64b6462bcbc90c0070087b4c85c78a0de2b2";
      descriptor = fixture::completeModel({
        "QwenFixture", nativePlanningDigest("spec185-qwen-content"),
        nativePlanningDigest("spec185-qwen-semantics"), qwenGraphDigest,
        "onnx", "float32", "qwen", "1"});
      descriptor.sourceRevision = "pinned-r1";
      const std::string sourceDigest = nativePlanningDigest(
        source.modelBytes.data(), source.modelBytes.size());
      const NativeJson stateInputs = {{role, {{"attention_kv_in", {"attention_kv_in"}},
        {"recurrent_state_in", {"recurrent_state_in"}},
        {"convolution_state_in", {"convolution_state_in"}}}}};
      const NativeJson stateOutputs = {{role, {{"attention_kv_out", {"attention_kv_out"}},
        {"recurrent_state_out", {"recurrent_state_out"}},
        {"convolution_state_out", {"convolution_state_out"}}}}};
      catalog = {
        {"schema", "ndnsf-di-native-request-catalog-v1"},
        {"model", nativeParseJson(descriptor.canonicalJson())},
        {"source", {{"file", "model.onnx"}, {"data_name", "/fixture/qwen/source"},
          {"digest", sourceDigest}, {"model_manifest_digest", manifestDigest},
          {"canonical_graph_digest", sourceIdentity.graphDigest}}},
        {"recipe", {{"artifact_profile_digest", artifactProfileDigest},
          {"assembler_descriptor_digest", nativePlanningDigest("spec185-qwen-assembler")},
          {"backend_abi", "fixture-abi"}, {"precision", "float32"},
          {"quantization", "none"}, {"layout", "native"}, {"padding", "none"},
          {"protection_epoch", protectionEpoch}, {"max_source_bytes", 1 << 20},
          {"max_assembled_bytes", 1 << 20}, {"max_nodes", 64}}},
        {"publication", {{"artifact_root", "/fixture/qwen/artifacts"}}},
        // The Qwen generation envelope carries tokenizer/state metadata in
        // addition to the inline input; use the maintained native-config
        // bound rather than the compact YOLO fixture's 32-byte probe limit.
        {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
        {"conversation_input", {{"kind", "OPAQUE_BYTE_TOKEN_IDS"}}},
        {"splitter", {{"kind", "QWEN"},
          {"layer_ranges", NativeJson::array({NativeJson::array({0, 2})})},
          {"artifact_digests_by_role", {{role, nativePlanningDigest("spec185-qwen-artifact")}}},
          {"weight_bytes_by_role", {{role, 1}}}, {"roles", {role}},
          {"tensor_degrees", {1}}, {"input_ingress_role", role},
          {"result_egress_role", role}}},
        {"node_mapping", {{"embedding", {0}}, {"layer-00", {1}},
          {"layer-01", {2}}, {"final-norm-head", {3, 4, 5, 6}}}},
        {"state_inputs", stateInputs}, {"state_outputs", stateOutputs}};
    }
    else {
      std::ifstream oracleFile("tests/fixtures/spec182/yolo-semantic-oracle.json");
      if (!oracleFile.good()) throw std::runtime_error("missing YOLO semantic oracle");
      NativeJson oracle;
      oracleFile >> oracle;
      const auto hex = oracle.at("model_hex").get<std::string>();
      for (std::size_t i = 0; i < hex.size(); i += 2)
        source.modelBytes.push_back(static_cast<std::uint8_t>(
          std::stoul(hex.substr(i, 2), nullptr, 16)));
      std::ifstream graphFile("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
      if (!graphFile.good()) throw std::runtime_error("missing ONNX graph oracle");
      NativeJson graphOracles;
      graphFile >> graphOracles;
      NativeJson graphOracle;
      for (const auto& item : graphOracles) {
        if (item.at("model_hex") == hex) {
          graphOracle = item;
          break;
        }
      }
      if (graphOracle.is_null()) throw std::runtime_error("missing matching graph oracle");
      descriptor = fixture::completeModel({
        "yolo26n", nativePlanningDigest("YOLOFixture-content"),
        nativePlanningDigest("YOLOFixture-semantics"), oracle.at("graph_digest"),
        "onnx", "float32", "YOLOFixture", "1"});
      const auto sourceIdentity = canonicalOnnxSourceIdentity(source, control);
      const auto planning = inspectNativeOnnxSourceGraph(source, descriptor, control);
      descriptor.graphDigest = planning.graph.graphDigest;
      if (sourceIdentity.graphDigest != graphOracle.at("canonical_graph_digest"))
        throw std::runtime_error("fixture graph identity differs from oracle");
      catalog["schema"] = "ndnsf-di-native-request-catalog-v1";
      catalog["model"] = nativeParseJson(descriptor.canonicalJson());
      catalog["source"] = {
        {"file", "model.onnx"}, {"data_name", "/fixture/source"},
        {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
        {"model_manifest_digest", nativePlanningDigest("fixture-manifest")},
        {"canonical_graph_digest", sourceIdentity.graphDigest} };
      catalog["recipe"] = NativeJson::object();
      catalog["recipe"]["artifact_profile_digest"] = nativePlanningDigest("fixture-profile");
      catalog["recipe"]["assembler_descriptor_digest"] = nativePlanningDigest("fixture-assembler-v1");
      catalog["recipe"]["backend_abi"] = "fixture-abi";
      catalog["recipe"]["precision"] = descriptor.precision;
      catalog["recipe"]["quantization"] = "none";
      catalog["recipe"]["layout"] = "NCHW";
      catalog["recipe"]["padding"] = "none";
      catalog["recipe"]["protection_epoch"] = "epoch-1";
      catalog["recipe"]["max_source_bytes"] = 1 << 20;
      catalog["recipe"]["max_assembled_bytes"] = 1 << 20;
      catalog["recipe"]["max_nodes"] = 100;
      catalog["publication"] = {{"artifact_root", "/fixture/artifacts"}};
      catalog["input_format"] = "OPAQUE";
      catalog["max_payload_bytes"] = 32;
      const NativeJson component = {
        {"candidate_id", "semantic-v1"}, {"priority", 1},
        {"roles", {"Front", "Branch", "Merge"}},
        {"node_names_by_role", NativeJson::object()},
        {"input_ingress_role", "Front"}, {"result_egress_role", "Merge"},
        {"merge_kind", ""}, {"candidate_digest", oracle.at("registered_digest")},
        {"semantic_partition", oracle.at("partition")} };
      catalog["splitter"] = { {"kind", "YOLO"}, {"components", {component}} };
    }

    std::ofstream model(root / "model.onnx", std::ios::binary);
    model.write(reinterpret_cast<const char*>(source.modelBytes.data()),
                static_cast<std::streamsize>(source.modelBytes.size()));
    const auto digest = std::string("sha256:") + std::string(64, 'a');
    const auto offerKeyId = publicKeyDigest(root / "offer-public.pem");
    NativeJson offerAdmission = NativeJson::object();
    offerAdmission["policy"] = NativeJson::object();
    offerAdmission["policy"]["schema"] = "spec180-provider-offer-trust-v1";
    offerAdmission["policy"]["candidateId"] = "candidate";
    offerAdmission["policy"]["candidateDigest"] = digest;
    offerAdmission["policy"]["trustSchema"] = "/group/trust";
    offerAdmission["policy"]["entries"] = NativeJson::array({NativeJson{
      {"provider", "/provider"}, {"service", "/Inference"},
      {"keyLocatorPrefix", "/provider/KEY/offer"}, {"signerKeyId", offerKeyId},
      {"certificateName", "/provider/KEY/offer/ID-CERT"}}});
    offerAdmission["public_key_files"] = NativeJson::object();
    offerAdmission["public_key_files"][offerKeyId] = "offer-public.pem";
    offerAdmission["candidate_digest"] = digest;

    // Sanitizer instrumentation makes the cooperative extension boundary
    // substantially slower on this host.  Keep the normal fixture's strict
    // 100 ms policy budget, but give the instrumented C++ selector enough
    // wall-clock budget to exercise the same authenticated path without
    // turning tool overhead into a deadline failure.
#if defined(__SANITIZE_ADDRESS__)
    constexpr std::uint64_t policyBudgetMs = 5000;
#elif defined(__clang__)
#  if __has_feature(address_sanitizer)
    constexpr std::uint64_t policyBudgetMs = 5000;
#  else
    constexpr std::uint64_t policyBudgetMs = 100;
#  endif
#else
    constexpr std::uint64_t policyBudgetMs = 100;
#endif
    NativeJson request = {
      {"service", "/Inference"}, {"task", "task"},
      {"adapter_composition_digest", digest}, {"task_descriptor_digest", digest},
      {"input_layout_digest", digest}, {"security_policy_digest", digest},
      {"max_candidates", 1}, {"max_policy_ms", policyBudgetMs},
      {"generation_mode", streaming ? "TOKEN_STREAMING" : "TOKEN_DIAGNOSTIC"},
      {"timeout_ms", 30000}, {"ack_timeout_ms", 5000}};
    if (streaming) {
      const auto tokenizerDigest = nativePlanningDigest("spec185-fixture-tokenizer");
      request["tokenizer_digest"] = tokenizerDigest;
      request["generation_defaults"] = {
        {"useCache", true}, {"outputMode", "TOKEN_STREAMING"},
        {"maxNewTokens", 32}, {"tokenizerDigest", tokenizerDigest},
        {"eosTokenIds", {2}}, {"samplingMode", "Greedy"},
        {"stateInputNames", {"attention_kv_in", "recurrent_state_in", "convolution_state_in"}},
        {"stateOutputNames", {"attention_kv_out", "recurrent_state_out", "convolution_state_out"}}};
    }
    auto config = NativeJson{
      {"schema", "ndnsf-di-native-requester-v1"},
      {"core", {{"requester_identity", "/user"}, {"authority_identity", "/aa"},
        {"group", "/group"}, {"trust_schema_file", "trust.conf"}}},
      {"catalog", catalog}, {"offer_admission", offerAdmission},
      {"grant", {{"authority_identity", "/aa"}, {"authority_service", "/grant"},
        {"authority_public_key_file", "authority-public.pem"},
        {"requester_private_key_file", "requester.pem"},
        {"protection_epoch", "epoch-1"}}},
      {"limits", {{"bootstrap_ms", 1000}, {"max_source_bytes", 1 << 20},
        {"max_assembled_bytes", 1 << 20}}},
      {"request", request}};
    if (streaming) {
      config["conversation"] = {
        {"schema", "ndnsf-di-native-conversation-v1"},
        {"journal", {{"state_root", "conversation-state"}, {"identity", "runtime"},
          {"keys", NativeJson::array({NativeJson{{"id", "k1"}, {"file", "conversation.key"}}})},
          {"quota_bytes", 1 << 20}, {"test_only_allow_ephemeral_state_root", true}}},
        {"owner", {{"requester_identity", "/user"}, {"service_name", "/Inference"},
          {"security_domain_digest", digest}}}};
    }
    configPath = root / "requester.json";
    std::ofstream output(configPath);
    output << nativeCanonicalJson(config);
  }

  ~RuntimeFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path write(const std::string& name, const std::string& content)
  {
    const auto path = root / name;
    std::ofstream output(path);
    output << content;
    return path;
  }

  std::filesystem::path root;
  std::filesystem::path configPath;
  inline static std::atomic<unsigned> sequence{0};
};

RuntimeConfig runtimeConfig(const RuntimeFixture& fixture)
{
  RuntimeConfig config;
  config.nativeConfigPath = fixture.configPath.string();
  config.maxPreparedBytes = 4 * 1024 * 1024;
  config.maxPreparedEntries = 2;
  config.preparationJobTimeout = std::chrono::seconds(5);
  return config;
}

} // namespace

namespace ndnsf::di {

struct Spec185PreparedModelTestAccess
{
  static std::shared_ptr<const PreparedModelPackage>
  package(const PreparedModel& model)
  {
    return model.m_package;
  }

  static PreparedModel
  bind(const PreparedModel& model,
       std::function<std::shared_ptr<NativeInferenceClient>(
         const std::shared_ptr<const PreparedModelPackage>&)> factory)
  {
    return PreparedModel(model.m_package, model.m_receipt, model.m_lease,
                         std::move(factory));
  }
};

} // namespace ndnsf::di

BOOST_AUTO_TEST_SUITE(Spec185PreparedRequest)

BOOST_AUTO_TEST_CASE(PreparedRequestsSharePackageButAllocateIndependentIds)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  // This request goes through Runtime::prepare's production client factory;
  // the immediate cancellation only avoids requiring a Provider for this
  // ownership/identity probe.
  const auto bytes = Input::inlineBytes({0x01, 0x02, 0x03});
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto first = prepared.request(bytes, options);
  auto second = prepared.request(bytes, options);
  BOOST_REQUIRE(!first.id().empty());
  BOOST_REQUIRE(!second.id().empty());
  BOOST_CHECK(first.id() != second.id());
  BOOST_CHECK(first.status() == RequestStatus::Pending ||
              first.status() == RequestStatus::Failed);
  BOOST_CHECK(second.status() == RequestStatus::Pending ||
              second.status() == RequestStatus::Failed);
  RequestOptions unsupportedMode;
  unsupportedMode.timeout = std::chrono::milliseconds(500);
  unsupportedMode.ackTimeout = std::chrono::milliseconds(50);
  unsupportedMode.outputMode = "TOKEN_STREAMING";
  BOOST_CHECK_EXCEPTION(prepared.request(bytes, unsupportedMode), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY";
                        });
  // No Provider is attached to this local Runtime.  Cancel immediately after
  // the identity assertions so this allocation-only case cannot turn the
  // unprovisioned Runtime's asynchronous NAC public-parameter retry into an
  // unrelated test-process abort.  Provider-backed execution and grant
  // identity are covered by the next case.
  first.cancel();
  second.cancel();
  BOOST_CHECK(first.status() == RequestStatus::Cancelled ||
              first.status() == RequestStatus::Failed);
  BOOST_CHECK(second.status() == RequestStatus::Cancelled ||
              second.status() == RequestStatus::Failed);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(StreamingRequestReaderPreservesTerminalEventAcrossCancel)
{
  RuntimeFixture fixture(true);
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  options.stream = StreamOptions{true};
  auto handle = prepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  auto reader = handle.events();
  BOOST_CHECK_EXCEPTION(reader.next(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "WAIT_TIMEOUT" &&
                                 error.boundary() == "wait";
                        });
  auto cancelledCallbacks = std::make_shared<std::atomic<unsigned>>(0);
  auto pendingRead = reader.nextAsync(
    std::chrono::seconds(2),
    [cancelledCallbacks] (std::exception_ptr, std::optional<Event>) {
      cancelledCallbacks->fetch_add(1, std::memory_order_relaxed);
    });
  BOOST_CHECK_EXCEPTION(
    reader.nextAsync(std::chrono::seconds(2),
      [] (std::exception_ptr, std::optional<Event>) {}),
    DiError,
    [] (const DiError& error) {
      return error.code() == "READ_IN_PROGRESS" && error.boundary() == "events";
    });
  pendingRead.unsubscribe();
  auto movedReader = std::move(reader);
  BOOST_CHECK_EXCEPTION(reader.next(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "READER_CLOSED" &&
                                 error.boundary() == "events";
                        });
  handle.cancel();
  const auto terminal = movedReader.next(std::chrono::seconds(2));
  BOOST_REQUIRE(terminal);
  BOOST_CHECK(terminal->terminal);
  BOOST_CHECK_EQUAL(terminal->requestId, handle.id());
  BOOST_CHECK_EQUAL(terminal->sequence, 1U);
  BOOST_CHECK_EQUAL(cancelledCallbacks->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EXCEPTION(movedReader.next(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CANCELLED" &&
                                 error.boundary() == "request";
                        });
  movedReader.close();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(VerifiedStreamingDefaultsMaterializeWithoutExplicitOptions)
{
  RuntimeFixture fixture(true);
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  BOOST_REQUIRE(prepared.capabilities().streaming);
  BOOST_CHECK(std::find(prepared.capabilities().outputModes.begin(),
                        prepared.capabilities().outputModes.end(),
                        "TOKEN_STREAMING") != prepared.capabilities().outputModes.end());
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  // The verified task contract supplies stream and generation defaults; an
  // ordinary caller need not duplicate those operator-owned fields.
  auto handle = prepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  BOOST_REQUIRE(!handle.id().empty());
  handle.cancel();
  BOOST_CHECK(handle.status() == RequestStatus::Cancelled ||
              handle.status() == RequestStatus::Failed);
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestCompletesThroughProvider)
{
  RuntimeFixture fixture;
  ndn_service_framework::test::BootstrapProfile profile;
  // Match the operator-pinned Runtime identity/group so the production
  // factory's frozen request contract and the fixture ServiceUser are the
  // same principal after the internal transport override.
  profile.groupPrefix = ndn::Name("/group");
  profile.syncPrefix = ndn::Name("/ndnsf/spec185/t005/sync");
  profile.userNode = ndn::Name("/ndnsf/spec185/t005/user");
  profile.providerNode = ndn::Name("/ndnsf/spec185/t005/provider");
  profile.userIdentity = ndn::Name("/user");
  profile.providerIdentity = ndn::Name("/provider");
  profile.attributeAuthority = ndn::Name("/aa");
  profile.serviceName = ndn::Name("/Inference");
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  // Construct the fixture before Runtime so the borrowed test transport
  // remains alive until the Runtime owner has completed its drain fence.
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  const auto package = ndnsf::di::Spec185PreparedModelTestAccess::package(prepared);

  const auto serviceName = environment.profile().serviceName.toUri();
  const auto requesterName = environment.user().getName().toUri();
  const auto providerName = environment.provider().getName().toUri();
  const auto model = package->catalog.model.descriptor;
  const auto candidates = package->catalog.splitter->enumerate(
    model, package->catalog.model.graph, NativeCandidateBudget{1, 1000, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto roles = candidates.front().executionPlan.roles;
  BOOST_REQUIRE(!roles.empty());

  const auto offerKey = deterministicEd25519Key(0x31);
  const auto offerKeyId = nativePlanningDigest(rawPublicKey(offerKey));
  NativeProviderOfferV3Config offerConfig;
  offerConfig.provider = providerName;
  offerConfig.service = serviceName;
  offerConfig.bootEpoch = providerName + ":" + environment.provider().getProviderBootEpoch();
  offerConfig.signerKeyId = offerKeyId;
  offerConfig.acceptedRoles = roles;
  offerConfig.backends = {"onnxruntime-cpu"};
  offerConfig.hasModel = true;
  offerConfig.signDigest = [offerKey] (const std::string& value) {
    return signDigest(offerKey, value);
  };
  const auto candidatePolicyDigest = nativePlanningDigest("spec185-t005-provider-policy");
  const auto policy = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"},
    {"candidateId", "spec185-t005"}, {"candidateDigest", candidatePolicyDigest},
    {"trustSchema", "/spec185/t005/trust"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", providerName}, {"service", serviceName},
      {"keyLocatorPrefix", environment.provider().getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment.provider().getSigningCertificateName().toUri()}}})}});
  auto admission = std::make_shared<NativeOfferAdmission>(
    policy, std::map<std::string, std::string>{{offerKeyId, publicKeyPem(offerKey)}},
    candidatePolicyDigest);

  const auto requesterKey = deterministicEd25519Key(0x41);
  const auto authorityKey = deterministicEd25519Key(0x51);
  const auto recipientKey = deterministicEd25519Key(0x61);
  const auto registration = nativeParseJson(package->registration->configurationJson);
  const auto protectionEpoch = registration.at("grant").at(
    "protection_epoch").get<std::string>();
  const auto authorityName = environment.profile().attributeAuthority.toUri();
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = authorityName;
  issuerConfig.requesterIdentity = requesterName;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-t005-grant-key";
  issuerConfig.authorityPrivateKey = authorityKey;
  issuerConfig.requesterPublicKey = requesterKey;
  issuerConfig.allowedModelManifests = {package->catalog.model.modelManifestDigest};
  // The requester grants against the request-scoped published root digest;
  // authorize that root to resolve back to the package's immutable source.
  issuerConfig.publicationSources.emplace(
    package->catalog.model.modelManifestDigest,
    NativeGrantPublicationSource{
      package->catalog.model.descriptor.modelName,
      package->catalog.model.descriptor.contentDigest,
      package->catalog.model.canonicalSourceDigest,
      package->catalog.model.canonicalInitializerObjectDigest,
      nativePlanningDigest("fixture-profile")});
  issuerConfig.recipientPublicKeys = {{providerName, recipientKey}};
  issuerConfig.contentKey = [] (const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 0x77);
  };
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuerConfig));
  NativeAuthenticatedGrantClient::Issue issue = [grantIssuer] (
    const NativeSignedGrantRequest& request, const std::string& publishedManifest,
    std::uint64_t expiresAtMs, const NativeGrantControl& control) {
    control.check();
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return grantIssuer->issue(request, nowMs, expiresAtMs, publishedManifest);
  };
  NativeAuthenticatedGrantClient::Publish publish = [] (
    const std::string& name, const std::string&, const NativeGrantControl& control) {
    control.check();
    return name;
  };
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    requesterName, requesterKey, authorityName, rawPublicKey(authorityKey),
    protectionEpoch, std::move(issue), std::move(publish));

  auto ackCount = std::make_shared<std::atomic<unsigned>>(0);
  auto responseCount = std::make_shared<std::atomic<unsigned>>(0);
  auto wrongDigestRejected = std::make_shared<std::atomic<bool>>(false);
  auto observationMutex = std::make_shared<std::mutex>();
  auto ackRequestIds = std::make_shared<std::set<std::string>>();
  auto selectionByRequest = std::make_shared<std::map<std::string, std::string>>();
  auto grantRequestIds = std::make_shared<std::map<std::string, std::set<std::string>>>();
  const auto acceptedRepositoryDigest = nativePlanningDigest("spec185-t005-repository-ciphertext");
  environment.provider().addCollaborationHandler(
    ndn::Name(serviceName),
    [offerConfig, acceptedRepositoryDigest, wrongDigestRejected, ackCount,
     observationMutex, ackRequestIds] (
      const ndn_service_framework::RequestMessage& request) {
      ackCount->fetch_add(1, std::memory_order_relaxed);
      const auto payload = request.getPayload();
      try {
        const auto root = nativeParseJson(std::string(payload.begin(), payload.end()));
        const auto requestId = root.value("request_id", std::string{});
        if (root.value("input_transport", std::string{}) == "REPO_REF") {
          const auto reference = root.value("input_reference", NativeJson::object());
          if (reference.value("ciphertextDigest", std::string{}) != acceptedRepositoryDigest) {
            wrongDigestRejected->store(true, std::memory_order_release);
            ndn_service_framework::ServiceProvider::AckDecision decision;
            decision.message = "DI_INPUT_DIGEST_REJECTED";
            return decision;
          }
        }
        if (!requestId.empty()) {
          std::lock_guard<std::mutex> lock(*observationMutex);
          ackRequestIds->insert(requestId);
        }
      }
      catch (...) {
        // The production offer parser below remains authoritative.
      }
      const auto issued = issueNativeProviderOfferV3(
        std::vector<std::uint8_t>(payload.begin(), payload.end()), offerConfig,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      ndn_service_framework::ServiceProvider::AckDecision decision;
      if (!issued || !issued->status) {
        decision.message = issued ? issued->message : "DI_NATIVE_REQUEST_NOT_V3";
        return decision;
      }
      decision.status = true;
      decision.message = issued->message;
      decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
      decision.pendingStateTtlMs = issued->pendingStateTtlMs;
      return decision;
    },
    [responseCount, observationMutex, selectionByRequest, grantRequestIds]
      (ndn_service_framework::ServiceProvider::CollaborationContext& context,
       const ndn_service_framework::RequestMessage&) {
      const auto assignment = context.assignment().assignmentPayload;
      const auto root = nativeParseJson(std::string(assignment.begin(), assignment.end()));
      const auto requestId = root.value("request_id", std::string{});
      const auto selectionDigest = context.assignment().selectionDigest;
      const auto grant = root.value("grant_binding", NativeJson::object());
      const auto grantRequestId = grant.value("request_id", std::string{});
      bool firstResponseForRequest = false;
      {
        std::lock_guard<std::mutex> lock(*observationMutex);
        firstResponseForRequest = selectionByRequest->emplace(requestId, selectionDigest).second;
        (*grantRequestIds)[requestId].insert(grantRequestId);
      }
      const std::string responseText = "spec185-provider-response";
      const ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(responseText.data()),
                                responseText.size());
      context.publishFinalResponse(payload);
      if (firstResponseForRequest)
        responseCount->fetch_add(1, std::memory_order_relaxed);
    });

  environment.enableProductionIngressForTest();
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "ACK");
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.user().cacheHybridReceiveKeyForTest(responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  // Keep the Provider fixture's transport and grant owner, but invoke the
  // production Runtime::makeRuntimeClient factory through the prepared
  // package's frozen clientFactory.  This test-only override is internal and
  // never changes the public Runtime/PreparedModel API or its owner registry.
  auto environmentUser = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment.user(), [] (ndn_service_framework::ServiceUser*) {});
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    runtime, environmentUser, grants, admission);
  auto providerPrepared = prepared;

  RequestOptions options;
  options.timeout = std::chrono::seconds(5);
  options.ackTimeout = std::chrono::seconds(2);
  auto first = providerPrepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  auto second = providerPrepared.request(Input::inlineBytes({0x04, 0x05, 0x06}), options);
  BOOST_REQUIRE_NE(first.id(), second.id());
  const auto firstId = first.id();
  BOOST_REQUIRE(first.status() == RequestStatus::Pending);
  BOOST_CHECK_EXCEPTION(first.events(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "events";
                        });
  BOOST_CHECK_EXCEPTION(first.result(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "LOCAL_WAIT_TIMEOUT" &&
                                 error.boundary() == "wait";
                        });

  auto observedTerminals = std::make_shared<std::atomic<unsigned>>(0);
  auto cancelledObservations = std::make_shared<std::atomic<unsigned>>(0);
  auto observation = first.observe(
    [observedTerminals, firstId] (const Event& event) {
      if (event.terminal && event.requestId == firstId)
        observedTerminals->fetch_add(1, std::memory_order_relaxed);
    });
  auto cancelledObservation = first.observe(
    [cancelledObservations] (const Event&) {
      cancelledObservations->fetch_add(1, std::memory_order_relaxed);
    });
  cancelledObservation.unsubscribe();
  auto completionPromise = std::make_shared<std::promise<bool>>();
  auto completionSet = std::make_shared<std::atomic<bool>>(false);
  auto completionSubscription = first.onCompletion(
    [completionPromise, completionSet, firstId]
    (std::exception_ptr error, std::optional<Result> result) {
      if (!completionSet->exchange(true, std::memory_order_acq_rel))
        completionPromise->set_value(!error && result && result->requestId == firstId);
    });
  auto resultAsyncPromise = std::make_shared<std::promise<bool>>();
  auto resultAsyncSet = std::make_shared<std::atomic<bool>>(false);
  auto resultAsyncSubscription = first.resultAsync(
    std::chrono::seconds(5),
    [resultAsyncPromise, resultAsyncSet, firstId]
    (std::exception_ptr error, std::optional<Result> result) {
      if (!resultAsyncSet->exchange(true, std::memory_order_acq_rel))
        resultAsyncPromise->set_value(!error && result && result->requestId == firstId);
    });
  auto timedWaitPromise = std::make_shared<std::promise<bool>>();
  auto timedWaitSet = std::make_shared<std::atomic<bool>>(false);
  auto timedWaitSubscription = second.resultAsync(
    std::chrono::milliseconds(1),
    [timedWaitPromise, timedWaitSet]
    (std::exception_ptr error, std::optional<Result> result) {
      if (timedWaitSet->exchange(true, std::memory_order_acq_rel))
        return;
      bool timedOut = !result;
      try {
        if (error) std::rethrow_exception(error);
      }
      catch (const DiError& value) {
        timedOut = timedOut && value.code() == "WAIT_TIMEOUT" &&
          value.boundary() == "wait";
      }
      catch (...) {
        timedOut = false;
      }
      timedWaitPromise->set_value(timedOut);
    });
  // Completion and timed-result waiters share one bounded native slot pool.
  // Release one slot and prove that a new subscription can reuse it.
  std::vector<Subscription> extraSubscriptions;
  extraSubscriptions.reserve(61);
  for (unsigned index = 0; index < 61; ++index) {
    extraSubscriptions.push_back(first.onCompletion(
      [] (std::exception_ptr, std::optional<Result>) {}));
  }
  BOOST_CHECK_EXCEPTION(
    first.onCompletion([] (std::exception_ptr, std::optional<Result>) {}), DiError,
    [] (const DiError& error) {
      return error.code() == "SUBSCRIPTION_LIMIT" && error.boundary() == "completion";
    });
  extraSubscriptions.back().unsubscribe();
  auto recycledSubscription = first.onCompletion(
    [] (std::exception_ptr, std::optional<Result>) {});

  auto firstFuture = std::async(std::launch::async, [first] { return first.result(std::chrono::seconds(5)); });
  auto secondFuture = std::async(std::launch::async, [second] { return second.result(std::chrono::seconds(5)); });
  environment.pumpUntil([&] {
    return firstFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
           secondFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  const auto firstResult = firstFuture.get();
  const auto secondResult = secondFuture.get();
  auto completionReady = completionPromise->get_future();
  auto resultAsyncReady = resultAsyncPromise->get_future();
  auto timedWaitReady = timedWaitPromise->get_future();
  BOOST_REQUIRE(completionReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_REQUIRE(resultAsyncReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_REQUIRE(timedWaitReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK(completionReady.get());
  BOOST_CHECK(resultAsyncReady.get());
  BOOST_CHECK(timedWaitReady.get());
  BOOST_CHECK_EQUAL(observedTerminals->load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(cancelledObservations->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(std::string(firstResult.payload.begin(), firstResult.payload.end()),
                    "spec185-provider-response");
  BOOST_CHECK_EQUAL(firstResult.requestId, firstId);
  BOOST_CHECK_EQUAL(std::string(secondResult.payload.begin(), secondResult.payload.end()),
                    "spec185-provider-response");
  BOOST_CHECK_EQUAL(ackCount->load(std::memory_order_relaxed), 2U);
  BOOST_CHECK_EQUAL(responseCount->load(std::memory_order_relaxed), 2U);
  {
    std::lock_guard<std::mutex> lock(*observationMutex);
    BOOST_CHECK_EQUAL(ackRequestIds->size(), 2U);
    BOOST_CHECK_EQUAL(selectionByRequest->size(), 2U);
    BOOST_CHECK_EQUAL(grantRequestIds->size(), 2U);
    for (const auto& item : *grantRequestIds) {
      BOOST_CHECK_EQUAL(item.second.size(), 1U);
      BOOST_CHECK_EQUAL(*item.second.begin(), item.first);
      BOOST_CHECK(!selectionByRequest->at(item.first).empty());
    }
  }

  // A repository reference keeps its complete canonical metadata on the
  // request wire.  The Provider fixture applies the same authenticated
  // content-digest gate used by its data owner and must reject a hot request
  // whose ciphertext digest does not match the object bound to this service.
  const NativeJson wrongReference = {
    {"authorizationScope", "/group/di"},
    {"ciphertextDigest", "sha256:" + std::string(64, 'b')},
    {"dataName", "/group/data/spec185-t005"}, {"encrypted", true},
    {"manifestDigest", "sha256:" + std::string(64, 'c')},
    {"plaintextSize", 3}, {"protectionEpoch", "fixture-epoch"} };
  auto wrongDigestRequest = providerPrepared.request(
    Input::repository(DataRef::fromPublishedMetadata(nativeCanonicalJson(wrongReference))), options);
  auto wrongDigestFuture = std::async(std::launch::async, [wrongDigestRequest] () mutable {
    try {
      (void)wrongDigestRequest.result(std::chrono::seconds(5));
      return false;
    }
    catch (const std::exception&) {
      return true;
    }
  });
  environment.pumpUntil([&] {
    return wrongDigestFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(wrongDigestFuture.get());
  BOOST_CHECK(wrongDigestRejected->load(std::memory_order_acquire));

  // Advance the installed Controller status after the grant/response keys
  // are already hot.  A subsequent request must fail closed instead of
  // reusing the cached authorization material for a revoked identity.
  const auto now = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto currentVersion = environment.user().getControllerVersion();
  const auto nextEpoch = currentVersion ? currentVersion->controllerEpoch + 1 : 2;
  ndn_service_framework::PolicyStatusData revokedStatus;
  revokedStatus.setServiceName(ndn::Name(serviceName));
  revokedStatus.setControllerVersion(ndn_service_framework::ControllerVersion{now, nextEpoch});
  revokedStatus.setValidity(now - 1000, now + 120000);
  revokedStatus.setPolicyDigest("sha256:" + std::string(64, '0'));
  revokedStatus.setControllerCertificate(ndn::Name("/spec185/t005/controller"));
  ndn_service_framework::RevocationTarget revokedUser;
  revokedUser.kind = ndn_service_framework::RevocationKind::IDENTITY;
  revokedUser.targetIdentity = ndn::Name(requesterName);
  revokedStatus.addRevocation(revokedUser);
  BOOST_REQUIRE(environment.user().installControllerStatus(revokedStatus));

  auto revokedRequest = providerPrepared.request(Input::inlineBytes({0x06, 0x07, 0x08}), options);
  auto revokedFuture = std::async(std::launch::async, [revokedRequest] () mutable {
    try {
      (void)revokedRequest.result(std::chrono::seconds(5));
      return false;
    }
    catch (const std::exception&) {
      return true;
    }
  });
  environment.pumpUntil([&] {
    return revokedFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(revokedFuture.get());
  BOOST_CHECK(revokedRequest.status() == RequestStatus::Failed ||
              revokedRequest.status() == RequestStatus::Cancelled);

  // The status advance intentionally re-arms the fixture User's NAC DKEY
  // fetch.  Finish that real AA/User exchange before destroying the borrowed
  // Face; otherwise SegmentFetcher retains pending interests into the
  // sanitizer exit path even though the request itself is terminal.
  bool userReadyAfterRevocation = false;
  environment.pumpUntilWithAttributeAuthority([&] {
    userReadyAfterRevocation = environment.user().isNacConsumerReadyForTest();
    return userReadyAfterRevocation;
  });
  BOOST_CHECK(userReadyAfterRevocation);

  runtime->close();
  // This test-only binding borrows the integration environment's Face.  Run
  // the Runtime drain concurrently while pumping that borrowed transport so
  // terminal collaboration cleanup reaches the same external I/O owner.
  auto drainFuture = std::async(std::launch::async, [runtime] {
    return runtime->drain(std::chrono::seconds(2));
  });
  environment.pumpUntil([&] {
    return drainFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(drainFuture.get());
}

BOOST_AUTO_TEST_CASE(PreparedConversationCommitsTwoNativeTurns)
{
  // Conversation exercises the native Qwen generation contract, including
  // sealed state tensors.  Keep the ordinary streaming probes on the compact
  // YOLO fixture, but use the source-bound Qwen fixture for real turns.
  RuntimeFixture fixture(true, true);
  ndn_service_framework::test::BootstrapProfile profile;
  profile.groupPrefix = ndn::Name("/group");
  profile.syncPrefix = ndn::Name("/ndnsf/spec185/t007/sync");
  profile.userNode = ndn::Name("/ndnsf/spec185/t007/user");
  profile.providerNode = ndn::Name("/ndnsf/spec185/t007/provider");
  profile.userIdentity = ndn::Name("/user");
  profile.providerIdentity = ndn::Name("/provider");
  profile.attributeAuthority = ndn::Name("/aa");
  profile.serviceName = ndn::Name("/Inference");
  profile.deferBridgeDelivery = true;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  const auto package = ndnsf::di::Spec185PreparedModelTestAccess::package(prepared);
  const auto serviceName = environment.profile().serviceName.toUri();
  const auto requesterName = environment.user().getName().toUri();
  const auto providerName = environment.provider().getName().toUri();
  const auto model = package->catalog.model.descriptor;
  const auto candidates = package->catalog.splitter->enumerate(
    model, package->catalog.model.graph, NativeCandidateBudget{1, 1000, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto roles = candidates.front().executionPlan.roles;
  BOOST_REQUIRE(!roles.empty());
  const auto terminalRole = candidates.front().resultEgressRole;
  BOOST_REQUIRE(!terminalRole.empty());

  const auto offerKey = deterministicEd25519Key(0x71);
  const auto offerKeyId = nativePlanningDigest(rawPublicKey(offerKey));
  NativeProviderOfferV3Config offerConfig;
  offerConfig.provider = providerName;
  offerConfig.service = serviceName;
  offerConfig.bootEpoch = providerName + ":" + environment.provider().getProviderBootEpoch();
  offerConfig.signerKeyId = offerKeyId;
  offerConfig.acceptedRoles = roles;
  offerConfig.backends = {"onnxruntime-cpu"};
  offerConfig.hasModel = true;
  offerConfig.signDigest = [offerKey] (const std::string& value) {
    return signDigest(offerKey, value);
  };
  const auto candidatePolicyDigest = nativePlanningDigest("spec185-t007-provider-policy");
  const auto policy = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"},
    {"candidateId", "spec185-t007"}, {"candidateDigest", candidatePolicyDigest},
    {"trustSchema", "/spec185/t007/trust"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", providerName}, {"service", serviceName},
      {"keyLocatorPrefix", environment.provider().getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment.provider().getSigningCertificateName().toUri()}}})}});
  auto admission = std::make_shared<NativeOfferAdmission>(
    policy, std::map<std::string, std::string>{{offerKeyId, publicKeyPem(offerKey)}},
    candidatePolicyDigest);

  const auto requesterKey = deterministicEd25519Key(0x81);
  const auto authorityKey = deterministicEd25519Key(0x91);
  const auto recipientKey = deterministicEd25519Key(0xa1);
  const auto registration = nativeParseJson(package->registration->configurationJson);
  const auto protectionEpoch = registration.at("grant").at("protection_epoch").get<std::string>();
  const auto policyDigest = registration.at("request").at("security_policy_digest").get<std::string>();
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/aa";
  issuerConfig.requesterIdentity = requesterName;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-t007-grant-key";
  issuerConfig.authorityPrivateKey = authorityKey;
  issuerConfig.requesterPublicKey = requesterKey;
  issuerConfig.allowedModelManifests = {package->catalog.model.modelManifestDigest};
  issuerConfig.publicationSources.emplace(package->catalog.model.modelManifestDigest,
    NativeGrantPublicationSource{
      package->catalog.model.descriptor.modelName,
      package->catalog.model.descriptor.contentDigest,
      package->catalog.model.canonicalSourceDigest,
      package->catalog.model.canonicalInitializerObjectDigest,
      nativePlanningDigest("fixture-profile")});
  issuerConfig.recipientPublicKeys = {{providerName, recipientKey}};
  issuerConfig.contentKey = [] (const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 0x77);
  };
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuerConfig));
  NativeAuthenticatedGrantClient::Issue issue = [grantIssuer] (
    const NativeSignedGrantRequest& request, const std::string& publishedManifest,
    std::uint64_t expiresAtMs, const NativeGrantControl& control) {
    control.check();
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return grantIssuer->issue(request, nowMs, expiresAtMs, publishedManifest);
  };
  NativeAuthenticatedGrantClient::Publish publish = [] (
    const std::string& name, const std::string&, const NativeGrantControl& control) {
    control.check();
    return name;
  };
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    requesterName, requesterKey, "/aa", rawPublicKey(authorityKey),
    protectionEpoch, std::move(issue), std::move(publish));

  auto ackCount = std::make_shared<std::atomic<unsigned>>(0);
  auto completedTurns = std::make_shared<std::atomic<unsigned>>(0);
  environment.provider().addCollaborationHandler(
    ndn::Name(serviceName),
    [offerConfig, ackCount] (const ndn_service_framework::RequestMessage& request) {
      ackCount->fetch_add(1, std::memory_order_relaxed);
      const auto payload = request.getPayload();
      const auto issued = issueNativeProviderOfferV3(
        std::vector<std::uint8_t>(payload.begin(), payload.end()), offerConfig,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      ndn_service_framework::ServiceProvider::AckDecision decision;
      if (!issued || !issued->status) {
        decision.message = issued ? issued->message : "DI_NATIVE_REQUEST_NOT_V3";
        return decision;
      }
      decision.status = true;
      decision.message = issued->message;
      decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
      decision.pendingStateTtlMs = issued->pendingStateTtlMs;
      return decision;
    },
    [model, serviceName, providerName, policyDigest, terminalRole, completedTurns] (
      ndn_service_framework::ServiceProvider::CollaborationContext& context,
      const ndn_service_framework::RequestMessage&) {
      try {
        const auto assignment = context.assignment().assignmentPayload;
        std::istringstream input(std::string(assignment.begin(), assignment.end()));
        const auto projection = nativeSelectionProjectionV3FromJson(input, context.role());
        if (!context.isStreamed() || !projection.conversationTurnBinding)
          throw std::runtime_error("Spec185 T007 conversation stream projection missing");
        const auto binding = *projection.conversationTurnBinding;
        const bool terminal = context.role() == terminalRole;
        const auto publishToken = [&context, &projection] (std::int64_t token,
                                                            std::uint64_t epoch,
                                                            const std::string& prefix,
                                                            const std::string& delta,
                                                            const std::string& hint) {
          const auto wire = nativeCanonicalJson(NativeJson{
            {"schema", "GenerationTokenEventV1"}, {"tokenId", token},
            {"tokenEpoch", epoch}, {"acceptedPrefixDigest", nativePlanningDigest(prefix)},
            {"textDelta", delta}, {"finishHint", hint},
            {"generationId", projection.generationContract.generationId},
            {"samplingDigest", projection.generationContract.samplingDigest}});
          if (!context.publishStreamEvent(ndn::Buffer(wire.begin(), wire.end())))
            throw std::runtime_error("Spec185 T007 token publication failed");
        };
        const bool append = binding.parentContextEpoch != 0;
        const auto nextToken = static_cast<std::int64_t>(binding.parentContextEpoch + 2);
        if (terminal) {
          if (append) {
            const auto token = std::to_string(nextToken);
            publishToken(nextToken, 1, token, "c", "EOS");
          }
          else {
            publishToken(1, 1, "1", "a", "NONE");
            publishToken(2, 2, "1,2", "b", "EOS");
          }
          const auto final = nativeCanonicalJson(NativeJson{
            {"schema", "NDNSF-DI-FINAL-V1"},
            {"tokenIds", append ? NativeJson::array({nextToken}) : NativeJson::array({1, 2})},
            {"text", append ? "c" : "ab"}, {"finishHint", "EOS"},
            {"finishReason", "eos"},
            {"generationId", projection.generationContract.generationId}});
          if (!context.finishStream(ndn::Buffer(final.begin(), final.end()),
                                    ndn_service_framework::StreamFinishReason::ApplicationComplete))
            throw std::runtime_error("Spec185 T007 final publication failed");
        }
        ProviderConversationStateReceiptV1 receipt;
        receipt.conversationId = binding.conversationId;
        receipt.parentContextEpoch = binding.parentContextEpoch;
        receipt.successorContextEpoch = binding.successorContextEpoch;
        receipt.originRequestId = context.sessionId();
        receipt.originGenerationId = projection.generationContract.generationId;
        receipt.serviceName = binding.serviceName;
        receipt.requesterIdentity = context.requesterName().toUri();
        receipt.securityDomainDigest = policyDigest;
        receipt.modelDigest = model.intentDigest();
        receipt.graphSemanticDigest = model.semanticsDigest;
        receipt.adapterDigest = model.adapter.descriptorDigest();
        receipt.roleName = context.role();
        receipt.roleSplitDigest = projection.selectedRole.recipeDigest;
        receipt.layoutDigest = projection.selectedRole.artifactProfileDigest;
        receipt.planRoleMapDigest = binding.planRoleMapDigest;
        receipt.providerIdentity = context.localProvider().toUri();
        receipt.providerBootId = providerName + "-boot";
        receipt.cacheEpoch = binding.successorContextEpoch;
        // The fixture adapter derives one canonical token per inline input
        // byte. Include those adapter-owned prompt tokens before the
        // deterministic streamed output in the committed successor prefix.
        std::vector<std::int64_t> fullTokens;
        if (!append)
          fullTokens = {1, 2, 3, 1, 2};
        else if (binding.parentContextEpoch == 1)
          fullTokens = {1, 2, 3, 1, 2, 4, 5, 6, 3};
        else
          fullTokens = {1, 2, 3, 1, 2, 4, 5, 6, 3, 7, 8, 9, 4};
        receipt.prefixDigest = nativeConversationPrefixDigest(fullTokens);
        receipt.prefixTokenCount = static_cast<std::uint32_t>(fullTokens.size());
        receipt.positionDigest = nativePlanningDigest("spec185-t007-position");
        receipt.stateSchemaDigest = nativePlanningDigest("spec185-t007-state-schema");
        receipt.stateComponentDigests = {nativePlanningDigest("spec185-t007-state")};
        receipt.expiresAtMs = binding.retentionDeadlineMs;
        receipt.validate();
        const auto receiptJson = receipt.toJson();
        context.publish("ndnsf-di-conversation-state-v1",
                        ndn::Name("/ndnsf-di/conversation/receipt").append(context.role()),
                        ndn::Buffer(receiptJson.begin(), receiptJson.end()));
        const auto controlTopic = ndn::Name("/ndnsf-di/conversation/control");
        const auto commitTopic = ndn::Name("/ndnsf-di/conversation/commit");
        for (int rounds = 0; rounds < 30; ++rounds) {
          const auto controls = context.waitFor("ndnsf-di-conversation-state-v1",
                                                controlTopic, 1, 1000);
          for (const auto& controlData : controls) {
            if (controlData.producer != context.requesterName() ||
                controlData.producerRole != "user-control-v1") continue;
            const auto control = nativeParseJson(std::string(
              controlData.payload.begin(), controlData.payload.end()));
            if (control.value("conversationId", std::string{}) != binding.conversationId ||
                control.value("successorContextEpoch", std::uint64_t{0}) != binding.successorContextEpoch ||
                control.value("roleName", std::string{}) != context.role()) continue;
            const auto action = control.value("action", std::string{});
            if (action == "FINALIZE" || action == "ROLLBACK") return;
            if (action != "COMMIT") continue;
            const auto ack = nativeCanonicalJson(NativeJson{
              {"schema", "ndnsf-di-provider-conversation-commit-ack-v1"},
              {"requestId", context.sessionId()}, {"attemptEpoch", projection.attempt},
              {"generationId", projection.generationContract.generationId},
              {"planDigest", projection.planDigest}, {"conversationId", binding.conversationId},
              {"parentContextEpoch", binding.parentContextEpoch},
              {"successorContextEpoch", binding.successorContextEpoch},
              {"serviceName", binding.serviceName}, {"planRoleMapDigest", binding.planRoleMapDigest},
              {"roleName", context.role()}, {"receiptDigest", receipt.computedDigest()},
              {"checkpointDigest", control.value("checkpointDigest", std::string{})},
              {"providerIdentity", context.localProvider().toUri()},
              {"providerBootId", providerName + "-boot"}, {"cacheEpoch", receipt.cacheEpoch},
              {"committed", true}});
            context.publish("ndnsf-di-conversation-state-v1", commitTopic,
                            ndn::Buffer(ack.begin(), ack.end()));
            if (terminal) completedTurns->fetch_add(1, std::memory_order_relaxed);
            else if (!context.completeRole())
              throw std::runtime_error("Spec185 T007 non-terminal role completion failed");
            return;
          }
        }
        throw std::runtime_error("Spec185 T007 conversation control timeout");
      }
      catch (const std::exception& error) {
        context.fail(std::string("Spec185 T007 handler failure: ") + error.what());
      }
    });
  environment.enableProductionIngressForTest();
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "ACK");
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.user().cacheHybridReceiveKeyForTest(responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);
  auto environmentUser = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment.user(), [] (ndn_service_framework::ServiceUser*) {});
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(runtime, environmentUser, grants, admission);

  auto conversation = prepared.openConversation();
  RequestOptions options;
  // The three-turn recovery path includes a fresh authenticated preparation
  // and state promotion.  Keep a bounded margin between the request deadline
  // and each blocking observation so normal VM scheduling cannot turn a valid
  // turn into an observation-timeout failure.
  // The authenticated three-turn path is sensitive to VM scheduling in both
  // normal and instrumented builds.  Keep one explicit bounded contract so a
  // valid turn is not made dependent on compiler-specific sanitizer macros.
  constexpr auto conversationTimeout = std::chrono::seconds(180);
  constexpr auto conversationResultWait = std::chrono::seconds(120);
  options.timeout = conversationTimeout;
  options.ackTimeout = std::chrono::seconds(3);
  options.generation = GenerationOptions{8};
  BOOST_CHECK_EXCEPTION(conversation.checkpoint(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CHECKPOINT_NOT_READY";
                        });
  auto first = conversation.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  // The first request cannot complete before the test pumps the borrowed Face.
  // Keep the active-turn negative case in a small helper frame so the
  // sanitizer observes the production exception boundary without coupling it
  // to this integration test's large stack frame.
  BOOST_CHECK(conversationRejectsBusyTurn(conversation, options));
  auto firstResultFuture = std::async(std::launch::async, [first, conversationResultWait] {
    return first.result(conversationResultWait);
  });
  pumpUntilFutureReady(environment, firstResultFuture, conversationTimeout);
  const auto firstResult = firstResultFuture.get();
  BOOST_CHECK(!firstResult.payload.empty());
  auto checkpoint = conversation.checkpoint();
  const auto checkpointBytes = checkpoint.bytes();
  BOOST_REQUIRE(!checkpointBytes.empty());
  BOOST_CHECK_EQUAL(completedTurns->load(std::memory_order_relaxed), 1U);
  const auto exportPath = fixture.root / "checkpoint.json";
  conversation.exportCheckpoint(exportPath);
  {
    std::ifstream exported(exportPath, std::ios::binary);
    const std::string exportedWire((std::istreambuf_iterator<char>(exported)),
                                   std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(exportedWire,
                      std::string(checkpointBytes.begin(), checkpointBytes.end()));
  }
  const auto exportedCheckpoint = [&] {
    std::ifstream input(exportPath, std::ios::binary);
    const std::string wire((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    return ConversationCheckpoint::fromBytes(
      std::vector<std::uint8_t>(wire.begin(), wire.end()));
  }();
  BOOST_CHECK(exportedCheckpoint.bytes() == checkpointBytes);

  auto second = conversation.request(Input::inlineBytes({0x04, 0x05, 0x06}), options);
  auto secondResultFuture = std::async(std::launch::async, [second, conversationResultWait] {
    return second.result(conversationResultWait);
  });
  pumpUntilFutureReady(environment, secondResultFuture, conversationTimeout);
  const auto secondResult = secondResultFuture.get();
  BOOST_CHECK(!secondResult.payload.empty());
  BOOST_CHECK_EQUAL(completedTurns->load(std::memory_order_relaxed), 2U);
  BOOST_CHECK(ackCount->load(std::memory_order_relaxed) >= 2U);
  const auto secondCheckpoint = conversation.checkpoint();
  const auto secondCheckpointBytes = secondCheckpoint.bytes();
  const auto recoveryExportPath = fixture.root / "recovery-checkpoint.json";
  conversation.exportCheckpoint(recoveryExportPath);
  const auto failedExportPath = fixture.root / "failed-checkpoint.json";
  {
    std::ofstream old(failedExportPath, std::ios::binary);
    old << "old-checkpoint";
  }
  const auto symlinkPath = fixture.root / "failed-checkpoint-link.json";
  std::error_code symlinkError;
  std::filesystem::create_symlink(failedExportPath, symlinkPath, symlinkError);
  BOOST_REQUIRE(!symlinkError);
  BOOST_CHECK_EXCEPTION(conversation.exportCheckpoint(symlinkPath), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CHECKPOINT_EXPORT_FAILED" &&
                                 error.boundary() == "export";
                        });
  {
    std::ifstream old(failedExportPath, std::ios::binary);
    const std::string oldWire((std::istreambuf_iterator<char>(old)),
                              std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(oldWire, "old-checkpoint");
  }

  ConversationOptions restoredOptions;
  std::ifstream recoveryInput(recoveryExportPath, std::ios::binary);
  const std::string recoveryWire((std::istreambuf_iterator<char>(recoveryInput)),
                                 std::istreambuf_iterator<char>());
  const auto recoveredCheckpoint = ConversationCheckpoint::fromBytes(
    std::vector<std::uint8_t>(recoveryWire.begin(), recoveryWire.end()));
  BOOST_CHECK(recoveredCheckpoint.bytes() == secondCheckpointBytes);
  restoredOptions.conversationId = nativeParseJson(recoveryWire).at("conversationId").get<std::string>();
  restoredOptions.checkpoint = recoveredCheckpoint;
  auto restored = prepared.openConversation(restoredOptions);
  auto third = restored.request(Input::inlineBytes({0x07, 0x08, 0x09}), options);
  auto thirdResultFuture = std::async(std::launch::async, [third, conversationResultWait] {
    return third.result(conversationResultWait);
  });
  pumpUntilFutureReady(environment, thirdResultFuture, conversationTimeout);
  BOOST_CHECK(!thirdResultFuture.get().payload.empty());
  BOOST_CHECK_EQUAL(completedTurns->load(std::memory_order_relaxed), 3U);
  restored.close();
  conversation.close();
  runtime->close();
  auto drainFuture = std::async(std::launch::async, [runtime] {
    return runtime->drain(std::chrono::seconds(3));
  });
  environment.pumpUntil([&] {
    return drainFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK(drainFuture.get());
}

BOOST_AUTO_TEST_CASE(PlacementHandlesAreRuntimeBound)
{
  RuntimeFixture fixture;
  auto firstRuntime = Runtime::open(runtimeConfig(fixture));
  auto secondRuntime = Runtime::open(runtimeConfig(fixture));
  auto prepared = firstRuntime->user().prepare();
  auto placement = secondRuntime->placementStrategy("native-pre-split-first");
  auto materializations = std::make_shared<std::atomic<unsigned>>(0);
  auto instrumented = ndnsf::di::Spec185PreparedModelTestAccess::bind(
    prepared, [materializations] (const std::shared_ptr<const PreparedModelPackage>&) {
      materializations->fetch_add(1, std::memory_order_relaxed);
      return std::shared_ptr<NativeInferenceClient>{};
    });
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  options.placement = placement;
  BOOST_CHECK_EXCEPTION(instrumented.request(Input::inlineBytes({0x01}), options), DiError,
                        [] (const DiError& error) {
                          return error.code() == "STRATEGY_NOT_FOUND" &&
                                 error.boundary() == "request";
                        });
  // Placement rejection is before client materialization, so no request or
  // Selection can be published through a factory that would otherwise run.
  BOOST_CHECK_EQUAL(materializations->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EXCEPTION(firstRuntime->placementStrategy("missing"), DiError,
                        [] (const DiError& error) {
                          return error.code() == "STRATEGY_NOT_FOUND" &&
                                 error.boundary() == "placement";
                        });
  firstRuntime->close();
  secondRuntime->close();
  BOOST_CHECK(firstRuntime->drain(std::chrono::seconds(2)));
  BOOST_CHECK(secondRuntime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestFactoryErrorsMapAtPublicBoundary)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  auto throwing = ndnsf::di::Spec185PreparedModelTestAccess::bind(
    prepared, [] (const std::shared_ptr<const PreparedModelPackage>&) ->
      std::shared_ptr<NativeInferenceClient> {
      throw std::runtime_error("injected client factory failure");
    });
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  BOOST_CHECK_EXCEPTION(throwing.request(Input::inlineBytes({0x01}), options), DiError,
                        [] (const DiError& error) {
                          return error.code() == "REQUEST_FAILED" &&
                                 error.domain() == "runtime" &&
                                 error.boundary() == "request";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncIncludesNativeClientWork)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto handle = prepared.request(Input::inlineBytes({0x01}), options);
  runtime->close();
  auto completion = std::make_shared<std::promise<bool>>();
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto future = completion->get_future();
  const auto drainTimeout = testDrainTimeout();
  auto subscription = runtime->drainAsync(
    drainTimeout,
    [completion, completed](std::exception_ptr error, bool drained) {
      if (!completed->exchange(true, std::memory_order_acq_rel))
        completion->set_value(!error && drained);
    });
  BOOST_REQUIRE(future.wait_for(drainTimeout) == std::future_status::ready);
  BOOST_CHECK(future.get());
  BOOST_CHECK(handle.status() == RequestStatus::Failed ||
              handle.status() == RequestStatus::Cancelled);
  subscription.cancel();
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncTracksMultiplePreparedClients)
{
  RuntimeFixture fixture;
  auto config = runtimeConfig(fixture);
  config.models.push_back({"secondary", fixture.configPath.string()});
  auto runtime = Runtime::open(config);
  auto primary = runtime->user().prepare("default");
  auto secondary = runtime->user().prepare("secondary");
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto primaryHandle = primary.request(Input::inlineBytes({0x01}), options);
  auto secondaryHandle = secondary.request(Input::inlineBytes({0x02}), options);
  BOOST_REQUIRE(!primaryHandle.id().empty());
  BOOST_REQUIRE(!secondaryHandle.id().empty());

  // Two distinct prepared registrations materialize two Runtime clients. The
  // non-closing Core drain must inspect both through the immutable DI snapshot
  // without taking the DI state mutex under the Core worker lock.
  auto completion = std::make_shared<std::promise<bool>>();
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto future = completion->get_future();
  auto subscription = runtime->drainAsync(
    std::chrono::seconds(2),
    [completion, completed](std::exception_ptr error, bool drained) {
      if (!completed->exchange(true, std::memory_order_acq_rel))
        completion->set_value(!error && drained);
    });
  // Register while both clients still own active request work, then drive the
  // terminal transitions. This exercises the notifier wakeup path as well as
  // the multi-client snapshot.
  primaryHandle.cancel();
  secondaryHandle.cancel();
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK(future.get());
  subscription.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncWakesAfterLastClientTimerRetires)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(300);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto handle = prepared.request(Input::inlineBytes({0x01}), options);

  // Keep Runtime open: this exercises the non-closing drainAsync path and
  // requires the client-owned request deadline timer to release its final
  // ticket before the outer Core waiter is notified.
  auto completion = std::make_shared<std::promise<bool>>();
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto future = completion->get_future();
  auto subscription = runtime->drainAsync(
    std::chrono::seconds(2),
    [completion, completed](std::exception_ptr error, bool drained) {
      if (!completed->exchange(true, std::memory_order_acq_rel))
        completion->set_value(!error && drained);
    });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK(future.get());
  BOOST_CHECK(handle.status() == RequestStatus::Failed ||
              handle.status() == RequestStatus::Cancelled);
  // The open-runtime notification is not a shutdown operation.
  BOOST_CHECK_NO_THROW(runtime->user());
  subscription.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestRejectsUnsupportedTextBeforeNativeSubmission)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  BOOST_CHECK_EXCEPTION(prepared.request(Input::text("hello")), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "input";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestRejectsUnsupportedGenerationContract)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.stream = StreamOptions{};
  options.generation = GenerationOptions{4};
  BOOST_CHECK_EXCEPTION(prepared.request(Input::inlineBytes({0x01}), options), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "request";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestValidatesProtectedReferenceMetadata)
{
  const NativeJson reference = {
    {"authorizationScope", "/group/di"}, {"ciphertextDigest", "sha256:" + std::string(64, 'b')},
    {"dataName", "/group/data/model"}, {"encrypted", true},
    {"manifestDigest", "sha256:" + std::string(64, 'c')},
    {"plaintextSize", 3}, {"protectionEpoch", "epoch-1"} };
  auto data = DataRef::fromPublishedMetadata(nativeCanonicalJson(reference));
  BOOST_CHECK_EQUAL(data.canonicalMetadata(), nativeCanonicalJson(reference));
  auto malformed = reference;
  malformed["plaintextSize"] = 0;
  BOOST_CHECK_EXCEPTION(DataRef::fromPublishedMetadata(nativeCanonicalJson(malformed)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_DATA_REFERENCE" &&
                                 error.boundary() == "input";
                        });

  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  auto oversized = reference;
  oversized["plaintextSize"] = 33; // fixture adapter max_payload_bytes is 32
  const auto oversizedRef = DataRef::fromPublishedMetadata(nativeCanonicalJson(oversized));
  BOOST_CHECK_EXCEPTION(prepared.request(Input::repository(oversizedRef)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_DATA_REFERENCE" &&
                                 error.boundary() == "input";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_SUITE_END()
