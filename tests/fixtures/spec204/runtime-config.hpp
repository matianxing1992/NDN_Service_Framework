#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>

// Shared checked native Runtime configuration; no transport, key or crypto override.
namespace {
using namespace ndnsf::di;
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


class RuntimeFixture
{
public:
  explicit RuntimeFixture(bool streaming = false, bool qwenStreaming = false,
                          std::size_t maxPayloadBytes = 32)
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
      // Use the maintained small causal ONNX graph. The old Qwen config path
      // was an unavailable external fixture; this file exercises the same
      // token and KV-family tensor names through the actual ORT graph.
      std::ifstream causalFile(
        "tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx",
        std::ios::binary);
      if (!causalFile.good())
        throw std::runtime_error("missing pinned Spec175 tiny causal ONNX fixture");
      source.modelBytes.assign(std::istreambuf_iterator<char>(causalFile), {});
      const auto sourceIdentity = canonicalOnnxSourceIdentity(source, control);
      const std::string role = "/LLM/Pipeline/Stage/0";
      const std::string manifestDigest = nativePlanningDigest("spec185-qwen-manifest");
      const std::string protectionEpoch = "epoch-1";
      // The provider fixture's grant publication source intentionally binds the
      // shared fixture profile; keep the Qwen source/model identity distinct
      // while using the same authorized artifact profile at this boundary.
      const std::string artifactProfileDigest = nativePlanningDigest("fixture-profile");
      const std::string sourceDigest = nativePlanningDigest(
        source.modelBytes.data(), source.modelBytes.size());
      const auto layerRanges = NativeJson::array({NativeJson::array({0, 2})});
      const auto graphNodes = NativeJson::array({
        "embedding", "layer-00", "layer-01", "final-norm-head"});
      const auto graphEdges = NativeJson::array({
        "hidden-embedding-to-layer-00", "hidden-layer-0-to-1",
        "hidden-layer-1-to-final"});
      // The source's canonical ONNX identity and the Qwen planner's abstract
      // layer graph are separate identities. Qwen planning validates against
      // the latter; preparation still pins the actual ONNX bytes above.
      const auto qwenGraphDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
        {"model", "Spec175TinyCausal"}, {"revision", "pinned-r1"},
        {"precision", "float32"}, {"decode_mode", "single-token-autoregressive"},
        {"modality", "text-only"}, {"mtp_enabled", false},
        {"thinking_mode", "disabled"}, {"layer_ranges", layerRanges},
        {"nodes", graphNodes}, {"edges", graphEdges}, {"legal_cuts", graphEdges}}));
      descriptor = fixture::completeModel({
        "Spec175TinyCausal", sourceDigest,
        nativePlanningDigest("spec175-tiny-causal-semantics"), qwenGraphDigest,
        "onnx", "float32", "qwen", "1"});
      descriptor.sourceRevision = "pinned-r1";
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
          {"layer_ranges", layerRanges},
          {"artifact_digests_by_role", {{role, nativePlanningDigest("spec185-qwen-artifact")}}},
          {"weight_bytes_by_role", {{role, 1}}}, {"roles", {role}},
          {"tensor_degrees", {1}}, {"input_ingress_role", role},
          {"result_egress_role", role}}},
        {"node_mapping", {{"embedding", {0, 1, 2, 3, 4}},
          {"layer-00", {5, 6, 7, 8, 9, 10, 11, 12}},
          {"layer-01", {13, 14, 15, 16, 17, 18, 19}},
          {"final-norm-head", {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30}}}},
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
      catalog["max_payload_bytes"] = maxPayloadBytes;
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

