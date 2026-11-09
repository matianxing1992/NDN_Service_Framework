#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/api.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/RuntimeTestAccess.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-qwen-config.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoSourceProvider.hpp"

#include <boost/test/unit_test.hpp>
#include <boost/asio/io_context.hpp>

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/io.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ndnsf::di::DiError;
using ndnsf::di::ModelRegistration;
using ndnsf::di::RepositorySourceError;
using ndnsf::di::Runtime;
using ndnsf::di::RuntimeConfig;

std::shared_ptr<EVP_PKEY> testEd25519Key(char value)
{
  const std::string seed(32, value);
  return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(
    EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(seed.data()), seed.size()), EVP_PKEY_free);
}

void writeEd25519KeyPair(const std::filesystem::path& privatePath,
                         const std::filesystem::path& publicPath)
{
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
    EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
  BOOST_REQUIRE(context);
  BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen_init(context.get()), 1);
  EVP_PKEY* raw = nullptr;
  BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen(context.get(), &raw), 1);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);

  std::unique_ptr<BIO, decltype(&BIO_free)> privateBio(
    BIO_new_file(privatePath.c_str(), "wb"), BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> publicBio(
    BIO_new_file(publicPath.c_str(), "wb"), BIO_free);
  BOOST_REQUIRE(privateBio);
  BOOST_REQUIRE(publicBio);
  BOOST_REQUIRE_EQUAL(PEM_write_bio_PrivateKey(privateBio.get(), key.get(), nullptr,
                                                nullptr, 0, nullptr, nullptr), 1);
  BOOST_REQUIRE_EQUAL(PEM_write_bio_PUBKEY(publicBio.get(), key.get()), 1);
  std::filesystem::permissions(privatePath,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace);
}

class TempRuntimeConfig
{
public:
  std::string canonicalGraphDigest;

  TempRuntimeConfig()
    : m_root(std::filesystem::temp_directory_path() /
             ("spec185-runtime-" + std::to_string(::getpid()) + "-" +
              std::to_string(++s_sequence)))
  {
    std::filesystem::create_directories(m_root);
    write("trust.conf",
          "; Local test trust schema\n"
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
          "  type file\n"
          "  file-name \"" + (m_root / "controller.cert").string() + "\"\n"
          "}\n");
    writeEd25519KeyPair(m_root / "requester.pem", m_root / "requester-public.pem");
    writeEd25519KeyPair(m_root / "authority.pem", m_root / "authority-public.pem");
    writeEd25519KeyPair(m_root / "offer.pem", m_root / "offer-public.pem");
    ndn::KeyChain controllerKeys("pib-memory:", "tpm-memory:");
    const auto controllerCertificate = controllerKeys.createIdentity(ndn::Name("/aa"))
                                         .getDefaultKey().getDefaultCertificate();
    const auto controllerPath = (m_root / "controller.cert").string();
    ndn::io::save(controllerCertificate, controllerPath);
    m_controllerEnvironment =
      std::make_unique<ndn_service_framework::test::ScopedEnvironmentValue>(
        "NDNSF_CONTROLLER_CERT_FILE", controllerPath.c_str());
  }

  ~TempRuntimeConfig()
  {
    m_controllerEnvironment.reset();
    std::error_code error;
    std::filesystem::remove_all(m_root, error);
  }

  std::filesystem::path write(const std::string& name, const std::string& content)
  {
    const auto path = m_root / name;
    std::ofstream output(path);
    output << content;
    output.close();
    return path;
  }

  std::filesystem::path makeConfig(const std::string& name,
                                   const std::string& group = "/group",
                                   const std::string& authority = "/aa")
  {
    const auto digest = std::string("sha256:") + std::string(64, 'a');
    const auto offerKeyId = publicKeyDigest(m_root / "offer-public.pem");
    const auto json = std::string("{") +
      "\"schema\":\"ndnsf-di-native-requester-v1\"," +
      "\"core\":{" +
        "\"requester_identity\":\"/user\",\"authority_identity\":\"" + authority +
        "\",\"group\":\"" + group + "\",\"trust_schema_file\":\"trust.conf\"}," +
      "\"catalog\":{\"source\":{\"file\":\"missing-model.onnx\"}}," +
      "\"offer_admission\":{\"policy\":{\"schema\":\"spec180-provider-offer-trust-v1\"," +
        "\"candidateId\":\"candidate\",\"candidateDigest\":\"" + digest +
        "\",\"trustSchema\":\"/group/trust\",\"entries\":[{\"provider\":\"/provider\",\"service\":\"/Inference\"," +
        "\"keyLocatorPrefix\":\"/provider/KEY/offer\",\"signerKeyId\":\"" + offerKeyId +
        "\",\"certificateName\":\"/provider/KEY/offer/ID-CERT\"}]}," +
        "\"public_key_files\":{\"" + offerKeyId + "\":\"offer-public.pem\"}," +
        "\"candidate_digest\":\"" + digest + "\"}," +
      "\"grant\":{" +
        "\"authority_identity\":\"" + authority + "\",\"authority_service\":\"/grant\"," +
        "\"authority_public_key_file\":\"authority-public.pem\"," +
        "\"requester_private_key_file\":\"requester.pem\",\"protection_epoch\":\"epoch-1\"}," +
      "\"limits\":{\"bootstrap_ms\":1000,\"max_source_bytes\":4096,\"max_assembled_bytes\":8192}," +
      "\"request\":{" +
        "\"service\":\"/Inference\",\"task\":\"task\"," +
        "\"adapter_composition_digest\":\"" + digest + "\"," +
        "\"task_descriptor_digest\":\"" + digest + "\",\"input_layout_digest\":\"" + digest + "\",\"security_policy_digest\":\"" + digest + "\"," +
        "\"max_candidates\":1,\"max_policy_ms\":100,\"generation_mode\":\"TOKEN_DIAGNOSTIC\"," +
        "\"timeout_ms\":30000,\"ack_timeout_ms\":5000}" +
      "}";
    return write(name, json);
  }

  std::filesystem::path makePublishStoredConfig(const std::string& name)
  {
    const auto source = makeConfig(name);
    std::ifstream input(source);
    ndnsf::di::NativeJson root;
    input >> root;
    root.erase("grant");
    root.erase("offer_admission");
    root["model_input"] = {{"mode", "publish-stored"}};
    auto& request = root.at("request");
    for (const auto* field : {"max_candidates", "max_policy_ms", "generation_mode",
                               "timeout_ms", "ack_timeout_ms"})
      request.erase(field);
    return write(name, ndnsf::di::nativeCanonicalJson(root));
  }

  std::filesystem::path makeValidPreparationConfig(const std::string& name)
  {
    const auto configPath = makeConfig(name);
    std::ifstream oracleFile("tests/fixtures/spec182/yolo-semantic-oracle.json");
    if (!oracleFile.good())
      throw std::runtime_error("YOLO semantic oracle is unavailable");
    ndnsf::di::NativeJson oracle;
    oracleFile >> oracle;
    ndnsf::di::NativeCanonicalSource source;
    const auto hex = oracle.at("model_hex").get<std::string>();
    for (std::size_t i = 0; i < hex.size(); i += 2)
      source.modelBytes.push_back(static_cast<std::uint8_t>(
        std::stoul(hex.substr(i, 2), nullptr, 16)));
    {
      std::ofstream model(m_root / "model.onnx", std::ios::binary);
      model.write(reinterpret_cast<const char*>(source.modelBytes.data()),
                  static_cast<std::streamsize>(source.modelBytes.size()));
    }
    std::ifstream graphOracleFile("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
    if (!graphOracleFile.good())
      throw std::runtime_error("ONNX planning graph oracle is unavailable");
    ndnsf::di::NativeJson graphOracles;
    graphOracleFile >> graphOracles;
    ndnsf::di::NativeJson graphOracle;
    for (const auto& item : graphOracles) {
      if (item.at("model_hex") == hex) {
        graphOracle = item;
        break;
      }
    }
    if (graphOracle.is_null())
      throw std::runtime_error("matching ONNX planning graph oracle is unavailable");
    canonicalGraphDigest = graphOracle.at("canonical_graph_digest").get<std::string>();
    const auto control = ndnsf::di::NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
      1 << 20, 1 << 20};
    auto descriptor = ndnsf::di::fixture::completeModel({
      "yolo26n", ndnsf::di::nativePlanningDigest("YOLOFixture-content"),
      ndnsf::di::nativePlanningDigest("YOLOFixture-semantics"),
      oracle.at("graph_digest"), "onnx", "float32", "YOLOFixture", "1"});
    const auto identity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
    const auto planning = ndnsf::di::inspectNativeOnnxSourceGraph(source, descriptor, control);
    descriptor.graphDigest = planning.graph.graphDigest;
    if (identity.graphDigest != canonicalGraphDigest)
      throw std::runtime_error("ONNX canonical graph oracle differs from native identity");
    ndnsf::di::NativeJson catalog = ndnsf::di::NativeJson::object();
    catalog["schema"] = "ndnsf-di-native-request-catalog-v1";
    catalog["model"] = ndnsf::di::nativeParseJson(descriptor.canonicalJson());
    catalog["source"] = { {"file", "model.onnx"}, {"data_name", "/fixture/source"},
      {"digest", ndnsf::di::nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", ndnsf::di::nativePlanningDigest("fixture-manifest")},
      {"canonical_graph_digest", canonicalGraphDigest} };
    catalog["recipe"] = {
      {"artifact_profile_digest", ndnsf::di::nativePlanningDigest("fixture-profile")},
      {"assembler_descriptor_digest", ndnsf::di::nativePlanningDigest("fixture-assembler-v1")},
      {"backend_abi", "fixture-abi"}, {"precision", descriptor.precision},
      {"quantization", "none"}, {"layout", "NCHW"}, {"padding", "none"},
      {"protection_epoch", "fixture-epoch"}, {"max_source_bytes", 1 << 20},
      {"max_assembled_bytes", 1 << 20}, {"max_nodes", 100} };
    catalog["publication"] = {{"artifact_root", "/fixture/artifacts"}};
    catalog["input_format"] = "JSON";
    catalog["max_payload_bytes"] = 32;
    const ndnsf::di::NativeJson component = {
      {"candidate_id", "semantic-v1"}, {"priority", 1},
      {"roles", {"Front", "Branch", "Merge"}},
      {"node_names_by_role", ndnsf::di::NativeJson::object()},
      {"input_ingress_role", "Front"}, {"result_egress_role", "Merge"},
      {"merge_kind", "NATIVE_POSTPROCESS"},
      {"candidate_digest", oracle.at("registered_digest")},
      {"semantic_partition", oracle.at("partition")} };
    catalog["splitter"] = {{"kind", "YOLO"}, {"components", {component}}};

    std::ifstream input(configPath);
    ndnsf::di::NativeJson runtime;
    input >> runtime;
    runtime["catalog"] = catalog;
    return write(name, ndnsf::di::nativeCanonicalJson(runtime));
  }

  std::filesystem::path makeValidQwenPreparationConfig(const std::string& name)
  {
    const auto configPath = makeConfig(name);
    onnx::ModelProto model;
    if (!model.ParseFromString(ndnsf::di::fixture::makeQwenNativeConfigOnnxSource()))
      throw std::runtime_error("Qwen source fixture is invalid");
    auto* weight = model.mutable_graph()->add_initializer();
    weight->set_name("fixture_weight");
    weight->set_data_type(onnx::TensorProto::FLOAT);
    weight->add_float_data(1.0F);
    auto* layer = model.mutable_graph()->mutable_node(1);
    layer->set_op_type("Add");
    layer->add_input("fixture_weight");
    std::string sourceBytes;
    if (!model.SerializeToString(&sourceBytes))
      throw std::runtime_error("failed to serialize Qwen source fixture");
    {
      std::ofstream output(m_root / "model.onnx", std::ios::binary);
      output.write(sourceBytes.data(), static_cast<std::streamsize>(sourceBytes.size()));
    }

    const auto control = ndnsf::di::NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 20, 1 << 20};
    ndnsf::di::NativeCanonicalSource source;
    source.modelBytes.assign(sourceBytes.begin(), sourceBytes.end());
    const auto sourceIdentity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
    const std::string role = "/LLM/Pipeline/Stage/0";
    auto descriptor = ndnsf::di::fixture::completeModel({
      "QwenFixture", ndnsf::di::nativePlanningDigest("runtime-qwen-content"),
      ndnsf::di::nativePlanningDigest("runtime-qwen-semantics"),
      "sha256:8f901f484f07440aef844373c95d64b6462bcbc90c0070087b4c85c78a0de2b2",
      "onnx", "float32", "qwen", "1"});
    descriptor.sourceRevision = "pinned-r1";
    const auto sourceDigest = ndnsf::di::nativePlanningDigest(
      source.modelBytes.data(), source.modelBytes.size());
    const ndnsf::di::NativeJson catalog{
      {"schema", "ndnsf-di-native-request-catalog-v1"},
      {"model", ndnsf::di::nativeParseJson(descriptor.canonicalJson())},
      {"source", {{"file", "model.onnx"}, {"data_name", "/fixture/qwen/source"},
        {"digest", sourceDigest},
        {"model_manifest_digest", ndnsf::di::nativePlanningDigest("runtime-qwen-manifest")},
        {"canonical_graph_digest", sourceIdentity.graphDigest}}},
      {"recipe", {{"artifact_profile_digest",
          ndnsf::di::nativePlanningDigest("fixture-profile")},
        {"assembler_descriptor_digest", ndnsf::di::nativePlanningDigest("runtime-qwen-assembler")},
        {"backend_abi", "fixture-abi"}, {"precision", "float32"},
        {"quantization", "none"}, {"layout", "native"}, {"padding", "none"},
        {"protection_epoch", "epoch-1"}, {"max_source_bytes", 1 << 20},
        {"max_assembled_bytes", 1 << 20}, {"max_nodes", 64}}},
      {"publication", {{"artifact_root", "/fixture/qwen/artifacts"}}},
      {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
      {"conversation_input", {{"kind", "OPAQUE_BYTE_TOKEN_IDS"}}},
      {"splitter", {{"kind", "QWEN"}, {"layer_ranges", {{0, 2}}},
        {"artifact_digests_by_role", {{role,
          ndnsf::di::nativePlanningDigest("runtime-qwen-artifact")}}},
        {"weight_bytes_by_role", {{role, 1}}}, {"roles", {role}},
        {"resources_by_role", {{role, ndnsf::di::nativeParseJson(
          ndnsf::di::NativeRoleResourceRequirement{
            {"onnxruntime"}, 1U << 20, 0, 0, 0, 0, 1.0}.canonicalJson())}}},
        {"tensor_degrees", {1}}, {"input_ingress_role", role},
        {"result_egress_role", role}}},
      {"node_mapping", {{"embedding", {0}}, {"layer-00", {1}}, {"layer-01", {2}},
        {"final-norm-head", {3, 4, 5, 6, 7, 8, 9}}}},
      {"state_inputs", {{role, {{"attention_kv_in", {"attention_kv_in"}},
        {"recurrent_state_in", {"recurrent_state_in"}},
        {"convolution_state_in", {"convolution_state_in"}}}}}},
      {"state_outputs", {{role, {{"attention_kv_out", {"attention_kv_out"}},
        {"recurrent_state_out", {"recurrent_state_out"}},
        {"convolution_state_out", {"convolution_state_out"}}}}}}};
    std::ifstream input(configPath);
    ndnsf::di::NativeJson runtime;
    input >> runtime;
    runtime["catalog"] = catalog;
    return write(name, ndnsf::di::nativeCanonicalJson(runtime));
  }

  std::filesystem::path rewrite(const std::string& sourceName,
                                const std::string& outputName,
                                const std::string& needle,
                                const std::string& replacement)
  {
    std::ifstream input(m_root / sourceName);
    std::string json((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const auto position = json.find(needle);
    if (position == std::string::npos)
      throw std::runtime_error("test config rewrite marker is absent");
    json.replace(position, needle.size(), replacement);
    return write(outputName, json);
  }

  const std::filesystem::path& root() const { return m_root; }

private:
  static std::string publicKeyDigest(const std::filesystem::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    const std::string pem((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr,
      EVP_PKEY_free);
    if (!key)
      throw std::runtime_error("failed to parse generated offer public key");
    std::string raw(32, '\0');
    std::size_t size = raw.size();
    if (EVP_PKEY_get_raw_public_key(
          key.get(), reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 ||
        size != raw.size())
      throw std::runtime_error("failed to extract generated offer public key");
    return ndnsf::di::nativePlanningDigest(raw);
  }

  inline static std::atomic<unsigned> s_sequence{0};
  std::filesystem::path m_root;
  std::unique_ptr<ndn_service_framework::test::ScopedEnvironmentValue> m_controllerEnvironment;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec185Runtime)

BOOST_AUTO_TEST_CASE(MissingControllerCertificateDoesNotMintForeignIdentity)
{
  TempRuntimeConfig files;
  const auto configPath = files.makeConfig("requester.json");
  const auto directory = configPath.parent_path() / "identity";
  std::filesystem::create_directory(directory);
  const auto pib = "pib-sqlite3:" + directory.string();
  const auto tpm = "tpm-file:" + directory.string();
  ndn_service_framework::test::ScopedEnvironmentValue pibEnvironment("NDN_CLIENT_PIB", pib.c_str());
  ndn_service_framework::test::ScopedEnvironmentValue tpmEnvironment("NDN_CLIENT_TPM", tpm.c_str());
  const auto missing = (configPath.parent_path() / "missing-controller.cert").string();
  ndn_service_framework::test::ScopedEnvironmentValue certificateEnvironment(
    "NDNSF_CONTROLLER_CERT_FILE", missing.c_str());
  RuntimeConfig config;
  config.nativeConfigPath = configPath.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(config), DiError, [] (const DiError& error) {
    return error.code() == "INVALID_RUNTIME_CONFIGURATION" && error.boundary() == "identity";
  });
  ndn::KeyChain audit(pib, tpm, false);
  BOOST_CHECK_NO_THROW(audit.getPib().getIdentity(ndn::Name("/user")));
  BOOST_CHECK_THROW(audit.getPib().getIdentity(ndn::Name("/aa")), ndn::security::pib::Pib::Error);
  BOOST_CHECK_EQUAL(audit.getPib().getIdentities().size(), 1U);
}

BOOST_AUTO_TEST_CASE(ValidPinnedConfigurationOpensAndDefersModelSource)
{
  TempRuntimeConfig files;
  const auto configPath = files.makeConfig("requester.json");

  RuntimeConfig config;
  config.nativeConfigPath = configPath.string();
  config.maxPreparedBytes = 1024;
  config.maxPreparedEntries = 2;
  config.preparationJobTimeout = std::chrono::seconds(3);

  auto runtime = Runtime::open(config);
  BOOST_REQUIRE(runtime);
  BOOST_CHECK_NO_THROW(runtime->user());
  BOOST_CHECK_NO_THROW(runtime->user({"default"}));

  // The source named by catalog.source is deliberately absent.  T001 freezes
  // operator metadata only; source acquisition belongs to T003/T004 prepare.
  std::filesystem::remove(configPath);
  BOOST_CHECK_NO_THROW(runtime->user());
}

BOOST_AUTO_TEST_CASE(RequestTimeStoredPreparationRejectsInvalidInputsWithoutSideEffects)
{
  TempRuntimeConfig files;
  RuntimeConfig config;
  config.nativeConfigPath = files.makeConfig("source.json").string();
  auto runtime = Runtime::open(config);
  const auto user = runtime->user();
  const ndnsf_distributed_repo::RepoArtifactTarget target{"/repo", "/repo/provider", "/repo/service"};
  const auto journal = files.root() / "preparation.json";
  const auto handle = files.root() / "private-handle";
  BOOST_CHECK_EXCEPTION(user.prepareStoredModel("default", target, journal, handle,
    std::chrono::milliseconds(0)), DiError, [] (const DiError& error) {
      return error.code() == "INVALID_MODEL_PREPARATION";
    });
  for (const auto& alias : {journal, std::filesystem::path(journal.string() + ".lock"),
                            std::filesystem::path(journal.string() + ".object")}) {
    BOOST_CHECK_EXCEPTION(user.prepareStoredModel("default", target, journal, alias), DiError,
      [] (const DiError& error) { return error.code() == "INVALID_MODEL_PUBLICATION"; });
  }
  BOOST_CHECK(!std::filesystem::exists(journal));
  BOOST_CHECK(!std::filesystem::exists(handle));
  BOOST_CHECK(!std::filesystem::exists(journal.string() + ".lock"));
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PublishStoredModeDoesNotRequireInferenceKeysOrGenerationBudget)
{
  TempRuntimeConfig files;
  RuntimeConfig config;
  config.nativeConfigPath = files.makePublishStoredConfig("publish-only.json").string();

  auto runtime = Runtime::open(config);
  BOOST_REQUIRE(runtime);
  auto user = runtime->user();
  BOOST_CHECK_EXCEPTION(user.prepare(), DiError, [] (const DiError& error) {
    return error.code() == "MODEL_INPUT_MODE_MISMATCH";
  });
  BOOST_CHECK_EXCEPTION(user.prepareAsync(), DiError, [] (const DiError& error) {
    return error.code() == "MODEL_INPUT_MODE_MISMATCH";
  });

  const ndnsf_distributed_repo::RepoArtifactTarget invalidTarget{
    "invalid", "/repo/provider", "/repo/service"};
  const auto statePath = files.root() / "publication-state.json";
  const auto handlePath = files.root() / "private-handle";
  BOOST_CHECK_EXCEPTION(user.prepareStoredModel("default", invalidTarget, statePath, handlePath),
    DiError, [] (const DiError& error) { return error.code() == "MODEL_INPUT_MODE_MISMATCH"; });
  auto lockHandlePath = handlePath;
  lockHandlePath += ".lock";
  BOOST_CHECK_EXCEPTION(user.publishStoredModel("default", invalidTarget,
    statePath, statePath.string() + ".lock"), DiError, [] (const DiError& error) {
      return error.code() == "INVALID_MODEL_PUBLICATION";
    });
  BOOST_CHECK_EXCEPTION(user.publishStoredModel("default", invalidTarget,
    statePath, statePath.string() + ".object"), DiError, [] (const DiError& error) {
      return error.code() == "INVALID_MODEL_PUBLICATION";
    });
  BOOST_CHECK_THROW(user.publishStoredModel("default", invalidTarget, statePath, handlePath),
                    std::invalid_argument);
  BOOST_CHECK(!std::filesystem::exists(statePath));
  BOOST_CHECK(!std::filesystem::exists(handlePath));
  BOOST_CHECK(!std::filesystem::exists(lockHandlePath));
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(StoredModelOpenKeepsCoreOwnerUntilQuietReadCallbacksDrain)
{
  TempRuntimeConfig files;
  const auto sourcePath = files.makeValidQwenPreparationConfig("stored-source.json");
  std::ifstream sourceInput(sourcePath);
  ndnsf::di::NativeJson runtimeJson;
  sourceInput >> runtimeJson;
  auto storedCatalog = runtimeJson.at("catalog");
  storedCatalog.at("source").erase("file");
  const auto catalogJson = ndnsf::di::nativeCanonicalJson(storedCatalog);
  std::ifstream modelInput(files.root() / "model.onnx", std::ios::binary);
  ndnsf::di::NativeCanonicalSource canonicalSource;
  canonicalSource.modelBytes.assign(std::istreambuf_iterator<char>(modelInput),
                                   std::istreambuf_iterator<char>());
  const ndnsf::di::NativeAssemblyControl preparationControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 20, 1 << 20};
  const auto catalog = ndnsf::di::NativeRequestCatalog::load(
    catalogJson, canonicalSource, preparationControl);
  std::map<ndn::Name, std::vector<std::uint8_t>> originalPackets;
  ndn::KeyChain publisherKeys{"pib-memory:", "tpm-memory:"};
  ndnsf::di::NativeStoredModelPacketIo publisherIo;
  publisherIo.requireCurrent = [] {};
  publisherIo.commit = [&] (auto wires, const std::string&) {
    for (auto& wire : wires) {
      const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(wire))};
      originalPackets.emplace(packet.getName(), std::move(wire));
    }
  };
  publisherIo.fetch = [&] (const std::vector<ndn::Name>& names) {
    ndnsf::di::NativeStoredModelPacketIo::PacketWires wires;
    for (const auto& name : names)
      wires.push_back(originalPackets.at(name));
    return wires;
  };
  const auto publicationPrefix = ndn::Name("/model-preparer/models").appendVersion(1);
  const auto publication = ndnsf::di::nativePublishStoredPreparedModel(
    publisherKeys, ndn::Name("/model-preparer"), ndn::Name("/repo"), publicationPrefix,
    catalog, catalogJson, publisherIo, {});
  const auto handle = publication.handle;
  const auto packetStore = std::make_shared<decltype(originalPackets)>(
    std::move(originalPackets));
  runtimeJson["catalog"] = std::move(storedCatalog);
  runtimeJson["model_input"] = {{"mode", "stored"}};
  const auto configPath = files.write("stored.json",
    ndnsf::di::nativeCanonicalJson(runtimeJson));
  std::filesystem::remove(files.root() / "model.onnx");
  BOOST_CHECK(!std::filesystem::exists(files.root() / "model.onnx"));

  ndn::security::KeyChain fixtureKeyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace fixtureFace{fixtureKeyChain};
  const auto userCertificate = ndn_service_framework::test::makeRsaIdentity(
    fixtureKeyChain, ndn::Name("/user"));
  const auto authorityCertificate = ndn_service_framework::test::makeRsaIdentity(
    fixtureKeyChain, ndn::Name("/aa"));
  auto fixtureUser = std::make_shared<ndn_service_framework::test::LocalServiceUser>(
    fixtureFace, ndn::Name("/group"), userCertificate, authorityCertificate,
    (files.root() / "trust.conf").string());

  struct IoContextRunner
  {
    ndn::Face& face;
    std::atomic<bool> running{true};
    std::unique_ptr<boost::asio::io_context::work> work;
    std::thread thread;

    explicit IoContextRunner(ndn::Face& value)
      : face(value)
      , work(std::make_unique<boost::asio::io_context::work>(face.getIoContext()))
      , thread([this] {
          while (running.load(std::memory_order_acquire)) {
            face.getIoContext().poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        })
    {
    }

    void stop()
    {
      if (!running.exchange(false, std::memory_order_acq_rel))
        return;
      work.reset();
      if (thread.joinable())
        thread.join();
    }

    ~IoContextRunner()
    {
      stop();
    }
  } fixtureIo(fixtureFace);

  const auto requesterKey = testEd25519Key('r');
  auto grants = std::make_shared<ndnsf::di::NativeAuthenticatedGrantClient>(
    "/user", requesterKey, "/aa", std::string(32, 'a'), "epoch-1",
    [] (const ndnsf::di::NativeSignedGrantRequest&, const std::string&, std::uint64_t,
        const ndnsf::di::NativeGrantControl&) { return ndnsf::di::NativeKeyGrant{}; },
    [] (const std::string&, const std::string&, const ndnsf::di::NativeGrantControl&) {
      return std::string("/fixture/grant");
    });
  const auto& publicKeyFiles = runtimeJson.at("offer_admission").at("public_key_files");
  const auto keyId = publicKeyFiles.begin().key();
  std::ifstream keyInput(files.root() / publicKeyFiles.begin().value().get<std::string>());
  const std::string keyPem((std::istreambuf_iterator<char>(keyInput)),
                           std::istreambuf_iterator<char>());
  auto admission = std::make_shared<ndnsf::di::NativeOfferAdmission>(
    runtimeJson.at("offer_admission").at("policy").dump(),
    std::map<std::string, std::string>{{keyId, keyPem}},
    runtimeJson.at("offer_admission").at("candidate_digest").get<std::string>());

  RuntimeConfig config;
  config.nativeConfigPath = configPath.string();
  const auto repositoryConfig = ndnsf_distributed_repo::RepoArtifactTarget{
    "/repo", "/repo/provider", "/repo/service"};
  const auto handlePath = files.root() / "private-handle";
  ndnsf::di::nativeExportStoredModelHandle(handle, handlePath);
  struct RepoReadObservations
  {
    std::mutex mutex;
    std::set<ndn::Name> fetchedNames;
    std::atomic<bool> pauseRoot{false};
    std::atomic<std::size_t> unexpectedInterests{0};
  };
  auto observations = std::make_shared<RepoReadObservations>();

  struct QuietReadGate
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    bool handlerDone = false;
  };
  auto gate = std::make_shared<QuietReadGate>();
  std::vector<std::shared_ptr<Runtime>> runtimes;
  std::future<bool> openResult;
  const auto rootName = handle.rootName;
  auto interestGate = fixtureFace.onSendInterest.connect(
    [&fixtureFace, packetStore, observations, gate,
     rootName] (const ndn::Interest& interest) {
    if (interest.getName() == rootName &&
        observations->pauseRoot.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(gate->mutex);
      gate->entered = true;
      gate->condition.notify_all();
      gate->condition.wait(lock, [&] { return gate->released; });
      gate->handlerDone = true;
      lock.unlock();
      gate->condition.notify_all();
      return;
    }
    const auto found = packetStore->find(interest.getName());
    if (found == packetStore->end()) {
      observations->unexpectedInterests.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(observations->mutex);
      observations->fetchedNames.insert(interest.getName());
    }
    const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(found->second))};
    fixtureFace.receive(packet);
  });
  BOOST_CHECK(interestGate.isConnected());

  struct StoredOpenCleanup
  {
    std::vector<std::shared_ptr<Runtime>>& runtimes;
    std::future<bool>& openResult;
    ndn::signal::Connection& interestGate;
    IoContextRunner& io;
    std::shared_ptr<QuietReadGate> gate;
    std::shared_ptr<RepoReadObservations> observations;

    ~StoredOpenCleanup()
    {
      for (const auto& runtime : runtimes)
        runtime->close();
      observations->pauseRoot.store(false, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
      }
      gate->condition.notify_all();
      if (openResult.valid())
        openResult.wait_for(std::chrono::seconds(12));
      bool drained = true;
      for (const auto& runtime : runtimes) {
        const auto current = runtime->drain(std::chrono::seconds(12));
        drained = current && drained;
      }
      if (drained) {
        io.stop();
        interestGate.disconnect();
      }
    }
  } cleanup{runtimes, openResult, interestGate, fixtureIo, gate, observations};

  auto successfulRuntime = Runtime::open(config);
  runtimes.push_back(successfulRuntime);
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    successfulRuntime, fixtureUser, grants, admission);
  {
    const auto prepared = successfulRuntime->user().openStoredModel(
      "default", handlePath, repositoryConfig, std::chrono::seconds(10));
    BOOST_CHECK_EQUAL(prepared.manifest().modelName, "QwenFixture");
    const auto originalKey = prepared.receipt().preparationKeyDigest;
    const auto reused = successfulRuntime->user().openStoredModel(
      "default", handlePath, repositoryConfig, std::chrono::seconds(10));
    BOOST_CHECK_EQUAL(reused.receipt().preparationKeyDigest, originalKey);
    BOOST_CHECK(reused.receipt().origin == ndnsf::di::PreparationReceipt::Origin::CacheHit);
    auto canonicalTarget = repositoryConfig;
    canonicalTarget.repoNode = ndn::Name(repositoryConfig.repoNode).toUri(
      ndn::name::UriFormat::CANONICAL);
    canonicalTarget.providerIdentity = ndn::Name(repositoryConfig.providerIdentity).toUri(
      ndn::name::UriFormat::CANONICAL);
    canonicalTarget.servicePrefix = ndn::Name(repositoryConfig.servicePrefix).toUri(
      ndn::name::UriFormat::CANONICAL);
    const auto equivalent = successfulRuntime->user().openStoredModel(
      "default", handlePath, canonicalTarget, std::chrono::seconds(10));
    BOOST_CHECK_EQUAL(equivalent.receipt().preparationKeyDigest, originalKey);
    BOOST_CHECK(equivalent.receipt().origin == ndnsf::di::PreparationReceipt::Origin::CacheHit);

    auto wrongHandle = handle;
    wrongHandle.masterKey.front() ^= 1;
    const auto wrongHandlePath = files.root() / "wrong-private-handle";
    ndnsf::di::nativeExportStoredModelHandle(wrongHandle, wrongHandlePath);
    BOOST_CHECK_THROW(successfulRuntime->user().openStoredModel(
      "default", wrongHandlePath, repositoryConfig, std::chrono::seconds(10)), std::exception);

    auto alternateRepository = repositoryConfig;
    alternateRepository.providerIdentity += "/alternate";
    const auto rebound = successfulRuntime->user().openStoredModel(
      "default", handlePath, alternateRepository, std::chrono::seconds(10));
    BOOST_CHECK_NE(rebound.receipt().preparationKeyDigest, originalKey);
  }
  successfulRuntime->close();
  BOOST_CHECK(successfulRuntime->drain(std::chrono::seconds(2)));
  {
    auto sourceJson = runtimeJson;
    sourceJson["model_input"]["mode"] = "source";
    sourceJson["catalog"]["source"]["file"] = (files.root() / "model.onnx").string();
    auto sourceConfig = config;
    sourceConfig.nativeConfigPath = files.write("source-existing-handle.json",
      ndnsf::di::nativeCanonicalJson(sourceJson)).string();
    auto sourceRuntime = Runtime::open(sourceConfig);
    runtimes.push_back(sourceRuntime);
    ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
      sourceRuntime, fixtureUser, grants, admission);
    const auto journal = files.root() / "absent-preparation-state";
    const auto prepared = sourceRuntime->user().prepareStoredModel(
      "default", repositoryConfig, journal, handlePath, std::chrono::seconds(10));
    BOOST_CHECK_EQUAL(prepared.manifest().modelName, "QwenFixture");
    const auto reused = sourceRuntime->user().prepareStoredModel(
      "default", repositoryConfig, journal, handlePath, std::chrono::seconds(10));
    BOOST_CHECK(reused.receipt().origin == ndnsf::di::PreparationReceipt::Origin::CacheHit);
    BOOST_CHECK_THROW(sourceRuntime->user().prepareStoredModel(
      "default", repositoryConfig, journal, files.root() / "wrong-private-handle",
      std::chrono::seconds(10)), std::exception);
    const auto symlinkHandle = files.root() / "symlink-private-handle";
    std::filesystem::create_symlink(handlePath, symlinkHandle);
    BOOST_CHECK_THROW(sourceRuntime->user().prepareStoredModel(
      "default", repositoryConfig, journal, symlinkHandle, std::chrono::seconds(10)),
      std::exception);
    BOOST_CHECK(!std::filesystem::exists(journal));
    BOOST_CHECK(!std::filesystem::exists(journal.string() + ".lock"));
    BOOST_CHECK(!std::filesystem::exists(journal.string() + ".object"));
    sourceRuntime->close();
    BOOST_CHECK(sourceRuntime->drain(std::chrono::seconds(2)));
  }
  BOOST_CHECK_EQUAL(observations->unexpectedInterests.load(), 0U);
  {
    std::lock_guard<std::mutex> lock(observations->mutex);
    BOOST_REQUIRE(!observations->fetchedNames.empty());
    for (const auto& name : observations->fetchedNames)
      BOOST_CHECK(name.toUri().find("material-bundle-") == std::string::npos);
    observations->fetchedNames.clear();
  }

  for (const auto mode : {"stored-close", "source-close", "source-deadline"}) {
    const bool sourceMode = std::string(mode) != "stored-close";
    const bool expire = std::string(mode) == "source-deadline";
    auto quietConfig = config;
    if (sourceMode)
      quietConfig.nativeConfigPath = (files.root() / "source-existing-handle.json").string();
    auto runtime = Runtime::open(quietConfig);
    runtimes.push_back(runtime);
    ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
      runtime, fixtureUser, grants, admission);
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->entered = false;
      gate->released = false;
      gate->handlerDone = false;
    }
    observations->pauseRoot.store(true, std::memory_order_release);
    const auto journal = files.root() / "quiet-absent-preparation-state";
    auto failure = std::make_shared<std::pair<std::string, std::chrono::milliseconds>>();
    openResult = std::async(std::launch::async,
      [runtime, handlePath, repositoryConfig, sourceMode, expire, journal, failure] {
      const auto started = std::chrono::steady_clock::now();
      try {
        if (sourceMode)
          (void)runtime->user().prepareStoredModel("default", repositoryConfig, journal,
            handlePath, expire ? std::chrono::milliseconds(300) : std::chrono::milliseconds(10000));
        else
          (void)runtime->user().openStoredModel("default", handlePath, repositoryConfig,
                                                std::chrono::seconds(10));
        return false;
      }
      catch (const DiError& error) {
        failure->first = error.code();
        failure->second = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started);
        return true;
      }
      catch (...) {
        return !expire;
      }
    });
    bool reachedQuietRead = false;
    {
      std::unique_lock<std::mutex> lock(gate->mutex);
      reachedQuietRead = gate->condition.wait_for(lock, std::chrono::seconds(2),
                                                   [&] { return gate->entered; });
    }
    if (!expire)
      runtime->close();
    BOOST_REQUIRE(openResult.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    BOOST_CHECK(reachedQuietRead);
    BOOST_CHECK(openResult.get());
    if (expire) {
      BOOST_CHECK_EQUAL(failure->first, "PREPARATION_TIMEOUT");
      // A 300 ms deadline may incur scheduling delay, but not a later transport timeout.
      BOOST_CHECK_GE(failure->second.count(), 200);
      BOOST_CHECK_LE(failure->second.count(), 1000);
    }
    runtime->close();
    BOOST_CHECK(!runtime->drain(std::chrono::milliseconds(20)));
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->released = true;
    }
    observations->pauseRoot.store(false, std::memory_order_release);
    gate->condition.notify_all();
    {
      std::unique_lock<std::mutex> lock(gate->mutex);
      BOOST_REQUIRE(gate->condition.wait_for(lock, std::chrono::seconds(2),
                                              [&] { return gate->handlerDone; }));
    }
    BOOST_REQUIRE(runtime->drain(std::chrono::seconds(2)));
    BOOST_CHECK(!std::filesystem::exists(journal));
  }
  fixtureIo.stop();
  interestGate.disconnect();
}

BOOST_AUTO_TEST_CASE(LongPreparationBootstrapBudgetRemainsBounded)
{
  TempRuntimeConfig files;
  files.makeConfig("requester.json");
  for (const auto budget : {3600000ULL, 10800000ULL, 86400000ULL}) {
    const auto path = files.rewrite("requester.json", std::to_string(budget) + ".json",
      "\"bootstrap_ms\":1000", "\"bootstrap_ms\":" + std::to_string(budget));
    RuntimeConfig config;
    config.nativeConfigPath = path.string();
    auto runtime = Runtime::open(config);
    BOOST_REQUIRE(runtime);
    BOOST_CHECK_NO_THROW(runtime->user());
  }
  const auto oversized = files.rewrite("requester.json", "oversized.json",
    "\"bootstrap_ms\":1000", "\"bootstrap_ms\":86400001");
  RuntimeConfig config;
  config.nativeConfigPath = oversized.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(config), DiError, [] (const DiError& error) {
    return error.code() == "INVALID_RUNTIME_CONFIGURATION" && error.boundary() == "limits";
  });
}

BOOST_AUTO_TEST_CASE(PrepareReportsSourceFailureAtPreparationBoundary)
{
  TempRuntimeConfig files;
  const auto configPath = files.makeConfig("requester.json");
  RuntimeConfig config;
  config.nativeConfigPath = configPath.string();
  config.maxPreparedBytes = 4 * 1024 * 1024;
  config.preparationJobTimeout = std::chrono::seconds(3);

  auto runtime = Runtime::open(config);
  auto user = runtime->user();
  BOOST_CHECK_EXCEPTION(user.prepare(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "PREPARATION_SOURCE_UNAVAILABLE" &&
                                 error.boundary() == "preparation";
                        });
  runtime->close();
  BOOST_CHECK_EXCEPTION(user.prepare(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "RUNTIME_CLOSED";
                        });
  BOOST_CHECK(runtime->drain(std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(RepositoryLoaderPreservesDeadlineAndCancellation)
{
  TempRuntimeConfig files;
  const auto deadlinePath = files.makeConfig("deadline-requester.json");
  RuntimeConfig deadlineConfig;
  deadlineConfig.nativeConfigPath = deadlinePath.string();
  deadlineConfig.maxPreparedBytes = 4 * 1024 * 1024;
  deadlineConfig.preparationJobTimeout = std::chrono::seconds(3);
  deadlineConfig.repositorySourceLoader = [] (
    const std::string&, const std::string&, std::uint64_t,
    std::chrono::steady_clock::time_point) {
    throw RepositorySourceError(RepositorySourceError::Kind::Timeout,
                                "repository source deadline expired");
    return ndnsf::di::NativeCanonicalSource{};
  };
  auto deadlineRuntime = Runtime::open(deadlineConfig);
  BOOST_CHECK_EXCEPTION(deadlineRuntime->user().prepare(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "PREPARATION_TIMEOUT" &&
                                 error.boundary() == "preparation";
                        });
  deadlineRuntime->close();
  BOOST_CHECK(deadlineRuntime->drain(std::chrono::seconds(2)));

  const auto cancelPath = files.makeConfig("cancel-requester.json");
  RuntimeConfig cancelConfig;
  cancelConfig.nativeConfigPath = cancelPath.string();
  cancelConfig.maxPreparedBytes = 4 * 1024 * 1024;
  cancelConfig.preparationJobTimeout = std::chrono::seconds(3);
  cancelConfig.repositorySourceLoader = [] (
    const std::string&, const std::string&, std::uint64_t,
    std::chrono::steady_clock::time_point) {
    throw RepositorySourceError(RepositorySourceError::Kind::Cancelled,
                                "repository source cancelled");
    return ndnsf::di::NativeCanonicalSource{};
  };
  auto cancelRuntime = Runtime::open(cancelConfig);
  BOOST_CHECK_EXCEPTION(cancelRuntime->user().prepare(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "PREPARATION_CANCELLED" &&
                                 error.boundary() == "preparation";
                        });
  cancelRuntime->close();
  BOOST_CHECK(cancelRuntime->drain(std::chrono::seconds(2)));

  const auto keywordPath = files.makeConfig("keyword-requester.json");
  RuntimeConfig keywordConfig;
  keywordConfig.nativeConfigPath = keywordPath.string();
  keywordConfig.maxPreparedBytes = 4 * 1024 * 1024;
  keywordConfig.preparationJobTimeout = std::chrono::seconds(3);
  keywordConfig.repositorySourceLoader = [] (
    const std::string&, const std::string&, std::uint64_t,
    std::chrono::steady_clock::time_point) {
    throw std::runtime_error("repository object /cancelled not found; connection closed");
    return ndnsf::di::NativeCanonicalSource{};
  };
  auto keywordRuntime = Runtime::open(keywordConfig);
  BOOST_CHECK_EXCEPTION(keywordRuntime->user().prepare(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "PREPARATION_SOURCE_UNAVAILABLE" &&
                                 error.boundary() == "preparation";
                        });
  keywordRuntime->close();
  BOOST_CHECK(keywordRuntime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PrepareSuccessUsesTheProductionRuntimeEntry)
{
  TempRuntimeConfig files;
  const auto configPath = files.makeValidPreparationConfig("prepared-requester.json");
  RuntimeConfig config;
  config.nativeConfigPath = configPath.string();
  config.maxPreparedBytes = 4 * 1024 * 1024;
  config.preparationJobTimeout = std::chrono::seconds(5);

  // Exercise the actual RepoCore authority path through the public Runtime
  // source-owner hook.  The local model file is removed before prepare, so a
  // successful preparation proves lookup/miss-ingest/read from the Repo and
  // cannot silently fall back to the legacy file loader.
  std::ifstream modelInput(files.root() / "model.onnx", std::ios::binary);
  const std::vector<std::uint8_t> repoPayload{
    std::istreambuf_iterator<char>(modelInput), std::istreambuf_iterator<char>()};
  BOOST_REQUIRE(!repoPayload.empty());
  auto repo = std::make_shared<ndnsf_distributed_repo::RepoCore>(
    ndnsf_distributed_repo::StorageCapability{
      "/spec188/runtime-repo", 64 * 1024 * 1024, 0, 0.0, 1.0,
      "local", {"filesystem"}, "persistent", true},
    ndnsf_distributed_repo::makeFilesystemRepoStore(
      (files.root() / "repo").string(), 64 * 1024, 1 << 20));
  auto sourceOwner = std::make_shared<ndnsf_distributed_repo::RepoSourceProvider>(
    repo, [repoPayload] (const ndnsf::di::RepositorySourceRequest& request) {
      if (repoPayload.size() > request.maxSourceBytes)
        throw ndnsf::di::RepositorySourceError(
          ndnsf::di::RepositorySourceError::Kind::Unavailable,
          "fallback source exceeds preparation limit");
      return ndnsf::di::NativeCanonicalSource{repoPayload, std::nullopt};
  });
  config.repositorySourceProvider = sourceOwner;
  config.repositoryArtifactPublisher = sourceOwner;
  // A repository-backed configuration deliberately has no local source
  // locator.  Runtime::open must accept the pinned identity and defer all
  // bytes to the configured owner.
  {
    std::ifstream input(configPath);
    ndnsf::di::NativeJson runtimeJson;
    input >> runtimeJson;
    runtimeJson.at("catalog").at("source").erase("file");
    files.write("prepared-requester.json", ndnsf::di::nativeCanonicalJson(runtimeJson));
  }
  std::error_code removed;
  std::filesystem::remove(files.root() / "model.onnx", removed);
  BOOST_REQUIRE(!removed);

  // Runtime::prepare now owns the canonical publication boundary.  Bind the
  // same LocalMock Core user used by the native publisher selector so this
  // isolated regression exercises the production Runtime entry without
  // depending on a live Controller/NFD bootstrap.  The wrapped REQUEST-LARGE
  // key is deliberately provisioned before preparation; production deployments
  // obtain the equivalent state from Core/NAC during startup.
  ndn::security::KeyChain fixtureKeyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace fixtureFace{fixtureKeyChain};
  auto fixtureCert = ndn_service_framework::test::makeRsaIdentity(
    fixtureKeyChain, ndn::Name("/user"));
  auto fixtureAa = ndn_service_framework::test::makeRsaIdentity(
    fixtureKeyChain, ndn::Name("/aa"));
  auto fixtureUser = std::make_shared<ndn_service_framework::test::LocalServiceUser>(
    fixtureFace, ndn::Name("/group"), fixtureCert, fixtureAa,
    (files.root() / "trust.conf").string());
  fixtureUser->useSigningKeyChainForTest(fixtureKeyChain);
  fixtureUser->prepareHybridSendKeyForTest(ndn::Name("/Inference"), "REQUEST-LARGE");

  // LocalMock ServiceUser queues publication work on its borrowed Face. Keep
  // that io_context alive and actively driven for the entire Runtime lifetime.
  struct IoContextRunner {
    ndn::Face& face;
    std::atomic<bool> running{true};
    std::thread thread;

    explicit IoContextRunner(ndn::Face& value)
      : face(value), thread([this] {
          while (running.load(std::memory_order_acquire)) {
            face.getIoContext().poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          face.getIoContext().poll();
        })
    {
    }

    ~IoContextRunner()
    {
      running.store(false, std::memory_order_release);
      face.getIoContext().stop();
      if (thread.joinable())
        thread.join();
    }
  } fixtureIo(fixtureFace);

  const auto makeGrants = [] {
    const auto requesterKey = testEd25519Key('r');
    return std::make_shared<ndnsf::di::NativeAuthenticatedGrantClient>(
      "/user", requesterKey, "/aa", std::string(32, 'a'), "epoch-1",
      [] (const ndnsf::di::NativeSignedGrantRequest&, const std::string&, std::uint64_t,
          const ndnsf::di::NativeGrantControl&) { return ndnsf::di::NativeKeyGrant{}; },
      [] (const std::string&, const std::string&, const ndnsf::di::NativeGrantControl&) {
        return std::string("/fixture/grant");
      });
  };
  auto grants = makeGrants();
  std::ifstream configInput(configPath);
  ndnsf::di::NativeJson runtimeJson;
  configInput >> runtimeJson;
  const auto& publicKeyFiles = runtimeJson.at("offer_admission").at("public_key_files");
  BOOST_REQUIRE_EQUAL(publicKeyFiles.size(), 1U);
  const auto keyId = publicKeyFiles.begin().key();
  std::ifstream keyInput(files.root() / publicKeyFiles.begin().value().get<std::string>());
  const std::string keyPem((std::istreambuf_iterator<char>(keyInput)),
                           std::istreambuf_iterator<char>());
  auto admission = std::make_shared<ndnsf::di::NativeOfferAdmission>(
    runtimeJson.at("offer_admission").at("policy").dump(),
    std::map<std::string, std::string>{{keyId, keyPem}},
    runtimeJson.at("offer_admission").at("candidate_digest").get<std::string>());
  // Declare Runtime after all borrowed fixture dependencies so normal scope
  // destruction releases Runtime/ServiceUser before Face and KeyChain.
  auto runtime = Runtime::open(config);
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    runtime, fixtureUser, std::move(grants), admission);
  auto prepared = runtime->user().prepare();
  const auto ownerStats = sourceOwner->stats();
  BOOST_CHECK_EQUAL(ownerStats.lookups, 1U);
  BOOST_CHECK_EQUAL(ownerStats.missIngests, 1U);
  std::ifstream oracleFile("tests/fixtures/spec182/yolo-semantic-oracle.json");
  ndnsf::di::NativeJson oracle;
  oracleFile >> oracle;
  BOOST_CHECK_EQUAL(prepared.manifest().modelName, "yolo26n");
  BOOST_CHECK_EQUAL(prepared.manifest().taskName, "task");
  BOOST_CHECK_EQUAL(prepared.manifest().canonicalGraphDigest, files.canonicalGraphDigest);
  BOOST_CHECK(prepared.receipt().origin == ndnsf::di::PreparationReceipt::Origin::Fetched);
  BOOST_CHECK_EQUAL(sourceOwner->stats().publicationCalls, 1U);

  ndnsf::di::PrepareOptions invalid;
  invalid.timeout = std::chrono::milliseconds(-1);
  BOOST_CHECK_EXCEPTION(runtime->user().prepare("default", invalid), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_ARGUMENT" &&
                                 error.boundary() == "preparation";
                        });

  const auto unsupportedPath = files.makeValidPreparationConfig("unsupported-task.json");
  std::ifstream unsupportedInput(unsupportedPath);
  ndnsf::di::NativeJson unsupportedRuntimeConfig;
  unsupportedInput >> unsupportedRuntimeConfig;
  // Keep the descriptor and its planning graph identity valid; exercise the
  // production capability gate by requesting a task outside the adapter's
  // declared task set.
  unsupportedRuntimeConfig["request"]["task"] = "other-task";
  files.write("unsupported-task.json", ndnsf::di::nativeCanonicalJson(unsupportedRuntimeConfig));
  RuntimeConfig unsupportedConfig;
  unsupportedConfig.nativeConfigPath = unsupportedPath.string();
  unsupportedConfig.maxPreparedBytes = 4 * 1024 * 1024;
  auto unsupportedRuntime = Runtime::open(unsupportedConfig);
  BOOST_CHECK_EXCEPTION(unsupportedRuntime->user().prepare(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "preparation";
                        });
  unsupportedRuntime->close();
  BOOST_CHECK(unsupportedRuntime->drain(std::chrono::seconds(2)));

  runtime->close();
  BOOST_REQUIRE(runtime->drain(std::chrono::seconds(2)));

  // Freeze the committed publication only after the original Runtime has
  // drained. Remove exactly one child, then verify that a fresh production
  // prepare observes a partial receipt before the existing source/publication
  // owners repair it. The root is never removed or rewritten; this is the
  // T005 handoff that the direct Repo unit test cannot observe through
  // ModelPreparationCache.
  const auto beforeRepair = repo->list();
  BOOST_REQUIRE(!beforeRepair.empty());
  const auto findObjectWithSuffix = [&] (const char* suffix) {
    const std::string marker(suffix);
    for (const auto& manifest : beforeRepair) {
      const ndn::Name name(manifest.objectName);
      const ndn::Name suffixName(marker);
      if (name.size() >= suffixName.size() &&
          name.getSubName(name.size() - suffixName.size()) == suffixName)
        return manifest.objectName;
    }
    return std::string{};
  };
  const auto sourceName = findObjectWithSuffix("/source");
  const auto rootName = findObjectWithSuffix("/manifest");
  BOOST_REQUIRE(!sourceName.empty());
  BOOST_REQUIRE(!rootName.empty());
  const auto rootBeforeRepair = repo->getManifest(rootName);
  BOOST_REQUIRE(repo->remove(sourceName));
  BOOST_CHECK(!repo->has(sourceName));

  const auto catalogJson = ndnsf::di::nativeCanonicalJson(runtimeJson.at("catalog"));
  auto partial = sourceOwner->lookupPrepared({
    "default", "/Inference", catalogJson, config.maxPreparedBytes,
    std::chrono::steady_clock::now() + std::chrono::seconds(5)});
  BOOST_REQUIRE(partial);
  BOOST_CHECK_EQUAL(partial->rootDataName, rootName);
  BOOST_REQUIRE(partial->materialManifest);
  BOOST_CHECK(!partial->materialManifest->payloadsComplete);
  BOOST_REQUIRE_EQUAL(partial->missingDataNames.size(), 1U);
  BOOST_CHECK_EQUAL(partial->missingDataNames.front(), sourceName);

  // A fresh Runtime has no in-memory preparation entry. Its async public
  // entry must therefore use the same partial-publication lookup, restore the
  // missing child, and retain the unchanged root and existing objects.
  auto asyncRuntime = Runtime::open(config);
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    asyncRuntime, fixtureUser, makeGrants(), admission);
  auto asyncHandle = asyncRuntime->user().prepareAsync();
  auto asyncPrepared = asyncHandle.result(std::chrono::seconds(5));
  BOOST_CHECK(asyncHandle.status() == ndnsf::di::PreparationStatus::Ready);
  BOOST_CHECK_EQUAL(asyncPrepared.manifest().modelName, "yolo26n");
  const auto afterRepair = repo->list();
  BOOST_REQUIRE_EQUAL(afterRepair.size(), beforeRepair.size());
  BOOST_CHECK(repo->has(sourceName));
  BOOST_CHECK_EQUAL(repo->getManifest(rootName).sha256, rootBeforeRepair.sha256);
  for (const auto& before : beforeRepair) {
    const auto found = std::find_if(afterRepair.begin(), afterRepair.end(),
      [&before] (const auto& current) {
        return current.objectName == before.objectName;
      });
    BOOST_REQUIRE(found != afterRepair.end());
    BOOST_CHECK_EQUAL(found->objectType, before.objectType);
    BOOST_CHECK_EQUAL(found->sha256, before.sha256);
    BOOST_CHECK_EQUAL(found->size, before.size);
    BOOST_CHECK_EQUAL(found->segmentCount, before.segmentCount);
    if (before.objectName != sourceName)
      BOOST_CHECK_EQUAL(found->operationId, before.operationId);
  }
  const auto finalOwnerStats = sourceOwner->stats();
  // The partial lookup causes one bounded source reconstruction and one
  // publication repair check. No unrelated Repo object is recreated or
  // rewritten.
  BOOST_CHECK_EQUAL(finalOwnerStats.lookups, 2U);
  // The canonical /fixture/source object remains present; only its prepared
  // publication child was removed, so source loading does not perform a
  // second fallback ingest.
  BOOST_CHECK_EQUAL(finalOwnerStats.missIngests, 1U);
  BOOST_CHECK_EQUAL(finalOwnerStats.publicationCalls, 2U);
  BOOST_CHECK_EQUAL(finalOwnerStats.publicationHits, 0U);
  asyncRuntime->close();
  BOOST_CHECK(asyncRuntime->drain(std::chrono::seconds(2)));

  // Repeat the same production handoff with one material payload absent.  The
  // source and root remain available, so this must be a partial material
  // repair rather than a source miss or a fresh publication.
  const auto beforeMaterialRepair = repo->list();
  const auto materialName = [&] {
    for (const auto& manifest : beforeMaterialRepair) {
      if (ndn::Name(manifest.objectName).toUri(ndn::name::UriFormat::ALTERNATE)
            .find("/materials/") != std::string::npos)
        return manifest.objectName;
    }
    return std::string{};
  }();
  BOOST_REQUIRE(!materialName.empty());
  const auto materialBefore = repo->getManifest(materialName);
  const auto rootBeforeMaterialRepair = repo->getManifest(rootName);
  BOOST_REQUIRE(repo->remove(materialName));
  BOOST_CHECK(!repo->has(materialName));

  auto materialPartial = sourceOwner->lookupPrepared({
    "default", "/Inference", catalogJson, config.maxPreparedBytes,
    std::chrono::steady_clock::now() + std::chrono::seconds(5)});
  BOOST_REQUIRE(materialPartial);
  BOOST_CHECK_EQUAL(materialPartial->rootDataName, rootName);
  BOOST_REQUIRE(materialPartial->materialManifest);
  BOOST_CHECK(!materialPartial->materialManifest->payloadsComplete);
  BOOST_REQUIRE_EQUAL(materialPartial->missingDataNames.size(), 1U);
  BOOST_CHECK_EQUAL(materialPartial->missingDataNames.front(), materialName);

  auto materialRuntime = Runtime::open(config);
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    materialRuntime, fixtureUser, makeGrants(), admission);
  auto materialHandle = materialRuntime->user().prepareAsync();
  auto materialPrepared = materialHandle.result(std::chrono::seconds(5));
  BOOST_CHECK(materialHandle.status() == ndnsf::di::PreparationStatus::Ready);
  BOOST_CHECK_EQUAL(materialPrepared.manifest().modelName, "yolo26n");
  const auto afterMaterialRepair = repo->list();
  BOOST_REQUIRE_EQUAL(afterMaterialRepair.size(), beforeMaterialRepair.size());
  BOOST_CHECK(repo->has(materialName));
  const auto materialAfter = repo->getManifest(materialName);
  BOOST_CHECK_EQUAL(materialAfter.sha256, materialBefore.sha256);
  BOOST_CHECK_EQUAL(materialAfter.size, materialBefore.size);
  BOOST_CHECK_EQUAL(repo->getManifest(rootName).sha256, rootBeforeMaterialRepair.sha256);
  for (const auto& before : beforeMaterialRepair) {
    const auto found = std::find_if(afterMaterialRepair.begin(), afterMaterialRepair.end(),
      [&before] (const auto& current) {
        return current.objectName == before.objectName;
      });
    BOOST_REQUIRE(found != afterMaterialRepair.end());
    BOOST_CHECK_EQUAL(found->objectType, before.objectType);
    BOOST_CHECK_EQUAL(found->sha256, before.sha256);
    BOOST_CHECK_EQUAL(found->size, before.size);
    BOOST_CHECK_EQUAL(found->segmentCount, before.segmentCount);
    if (before.objectName != materialName)
      BOOST_CHECK_EQUAL(found->operationId, before.operationId);
  }
  const auto materialOwnerStats = sourceOwner->stats();
  BOOST_CHECK_EQUAL(materialOwnerStats.missIngests, finalOwnerStats.missIngests);
  BOOST_CHECK_EQUAL(materialOwnerStats.publicationCalls,
                    finalOwnerStats.publicationCalls + 1U);
  BOOST_CHECK_EQUAL(materialOwnerStats.publicationHits, finalOwnerStats.publicationHits);
  materialRuntime->close();
  BOOST_CHECK(materialRuntime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(InvalidRuntimeLimitsAndProfileFailClosed)
{
  TempRuntimeConfig files;
  const auto path = files.makeConfig("requester.json");

  RuntimeConfig missing;
  BOOST_CHECK_EXCEPTION(Runtime::open(missing), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_RUNTIME_CONFIGURATION";
                        });

  RuntimeConfig zeroBytes;
  zeroBytes.nativeConfigPath = path.string();
  zeroBytes.maxPreparedBytes = 0;
  BOOST_CHECK_EXCEPTION(Runtime::open(zeroBytes), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CACHE_BUDGET_EXCEEDED";
                        });

  RuntimeConfig valid;
  valid.nativeConfigPath = path.string();
  auto runtime = Runtime::open(valid);
  BOOST_CHECK_EXCEPTION(runtime->user({"operator"}), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "profile";
                        });
}

BOOST_AUTO_TEST_CASE(SchemaTrustAndKeyFailuresAreSynchronous)
{
  TempRuntimeConfig files;
  const auto path = files.makeConfig("requester.json");

  auto invalidSchema = files.write("invalid-schema.json", "{\"schema\":\"wrong\"}");
  RuntimeConfig schemaConfig;
  schemaConfig.nativeConfigPath = invalidSchema.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(schemaConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "schema";
                        });

  auto missingTrust = files.makeConfig("missing-trust.json");
  {
    std::ifstream input(missingTrust);
    std::string json((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    input.close();
    const auto marker = std::string("\"trust.conf\"");
    json.replace(json.find(marker), marker.size(), "\"no.conf\"");
    files.write("missing-trust.json", json);
  }
  RuntimeConfig trustConfig;
  trustConfig.nativeConfigPath = missingTrust.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(trustConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "trust";
                        });

  auto config = files.makeConfig("bad-key.json");
  std::ifstream input(config);
  std::string json((std::istreambuf_iterator<char>(input)), {});
  input.close();
  const auto marker = std::string("\"requester.pem\"");
  json.replace(json.find(marker), marker.size(), "\"missing.pem\"");
  files.write("bad-key.json", json);
  RuntimeConfig keyConfig;
  keyConfig.nativeConfigPath = config.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(keyConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "trust";
                        });
  (void)path;
}

BOOST_AUTO_TEST_CASE(CompleteNativeContractRejectsMalformedFields)
{
  TempRuntimeConfig files;
  files.makeConfig("requester.json");

  const auto missingCoreAuthority = files.rewrite(
    "requester.json", "missing-core-authority.json",
    "\"authority_identity\":\"/aa\",\"group\"", "\"group\"");
  RuntimeConfig coreConfig;
  coreConfig.nativeConfigPath = missingCoreAuthority.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(coreConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "configuration";
                        });

  const auto badOffer = files.rewrite(
    "requester.json", "bad-offer.json", "\"candidateId\":\"candidate\"",
    "\"candidateId\":\"\"");
  RuntimeConfig offerConfig;
  offerConfig.nativeConfigPath = badOffer.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(offerConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "offer-admission";
                        });

  const auto badGeneration = files.rewrite(
    "requester.json", "bad-generation.json",
    "\"generation_mode\":\"TOKEN_DIAGNOSTIC\"",
    "\"generation_mode\":\"UNSUPPORTED\"");
  RuntimeConfig generationConfig;
  generationConfig.nativeConfigPath = badGeneration.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(generationConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "request";
                        });

  const auto streamingWithoutTokenizer = files.rewrite(
    "requester.json", "streaming-without-tokenizer.json",
    "\"generation_mode\":\"TOKEN_DIAGNOSTIC\"",
    "\"generation_mode\":\"TOKEN_STREAMING\"");
  RuntimeConfig streamingConfig;
  streamingConfig.nativeConfigPath = streamingWithoutTokenizer.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(streamingConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "request";
                        });

  const auto plaintextEpoch = files.rewrite(
    "requester.json", "plaintext-epoch.json",
    "\"protection_epoch\":\"epoch-1\"",
    "\"protection_epoch\":\"plaintext-v1\"");
  RuntimeConfig epochConfig;
  epochConfig.nativeConfigPath = plaintextEpoch.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(epochConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "trust";
                        });

  const auto optionalGeneration = files.rewrite(
    "requester.json", "optional-generation.json",
    "\"max_policy_ms\":100,\"generation_mode\":\"TOKEN_DIAGNOSTIC\",\"timeout_ms\"",
    "\"max_policy_ms\":100,\"timeout_ms\"");
  RuntimeConfig optionalGenerationConfig;
  optionalGenerationConfig.nativeConfigPath = optionalGeneration.string();
  BOOST_CHECK_NO_THROW(Runtime::open(optionalGenerationConfig));

  const auto missingCandidateBudget = files.rewrite(
    "requester.json", "missing-max-candidates.json", "\"max_candidates\":1,", "");
  RuntimeConfig missingCandidateConfig;
  missingCandidateConfig.nativeConfigPath = missingCandidateBudget.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(missingCandidateConfig), DiError,
                        [] (const DiError& error) { return error.boundary() == "request"; });

  const auto missingPolicyBudget = files.rewrite(
    "requester.json", "missing-max-policy-ms.json", "\"max_policy_ms\":100,", "");
  RuntimeConfig missingPolicyConfig;
  missingPolicyConfig.nativeConfigPath = missingPolicyBudget.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(missingPolicyConfig), DiError,
                        [] (const DiError& error) { return error.boundary() == "request"; });

  const auto excessiveCandidates = files.rewrite(
    "requester.json", "excessive-candidates.json", "\"max_candidates\":1",
    "\"max_candidates\":1025");
  RuntimeConfig candidateConfig;
  candidateConfig.nativeConfigPath = excessiveCandidates.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(candidateConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "request";
                        });

  const auto excessiveTimeout = files.rewrite(
    "requester.json", "excessive-timeout.json", "\"timeout_ms\":30000",
    "\"timeout_ms\":2147483648");
  RuntimeConfig timeoutConfig;
  timeoutConfig.nativeConfigPath = excessiveTimeout.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(timeoutConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "request";
                        });

  const auto excessiveApplicationId = files.rewrite(
    "requester.json", "excessive-application-id.json",
    "\"timeout_ms\":30000",
    "\"application_request_id\":\"" + std::string(257, 'x') +
      "\",\"timeout_ms\":30000");
  RuntimeConfig applicationIdConfig;
  applicationIdConfig.nativeConfigPath = excessiveApplicationId.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(applicationIdConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "request";
                        });

  const auto nulApplicationId = files.rewrite(
    "requester.json", "nul-application-id.json",
    "\"timeout_ms\":30000",
    "\"application_request_id\":\"bad\\u0000id\",\"timeout_ms\":30000");
  RuntimeConfig nulApplicationIdConfig;
  nulApplicationIdConfig.nativeConfigPath = nulApplicationId.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(nulApplicationIdConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "request";
                        });

  const auto nulIdentity = files.rewrite(
    "requester.json", "nul-identity.json", "\"requester_identity\":\"/user\"",
    "\"requester_identity\":\"/bad\\u0000identity\"");
  RuntimeConfig nulIdentityConfig;
  nulIdentityConfig.nativeConfigPath = nulIdentity.string();
  BOOST_CHECK_EXCEPTION(Runtime::open(nulIdentityConfig), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "identity";
                        });
}

BOOST_AUTO_TEST_CASE(ModelKeysFreezeAndTrustDomainIsShared)
{
  TempRuntimeConfig files;
  const auto primary = files.makeConfig("requester.json");
  files.makeConfig("same-domain.json");
  files.makeConfig("other-domain.json", "/other-group");

  RuntimeConfig config;
  config.nativeConfigPath = primary.string();
  config.models = {{"same", "same-domain.json"}};
  BOOST_CHECK_NO_THROW(Runtime::open(config));

  RuntimeConfig duplicate = config;
  duplicate.models.push_back({"same", "same-domain.json"});
  BOOST_CHECK_EXCEPTION(Runtime::open(duplicate), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "model-registry";
                        });

  RuntimeConfig defaultKey = config;
  defaultKey.models = {{"default", "same-domain.json"}};
  BOOST_CHECK_EXCEPTION(Runtime::open(defaultKey), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "model-registry";
                        });

  RuntimeConfig crossDomain = config;
  crossDomain.models = {{"other", "other-domain.json"}};
  BOOST_CHECK_EXCEPTION(Runtime::open(crossDomain), DiError,
                        [] (const DiError& error) {
                          return error.boundary() == "identity";
                        });
}

BOOST_AUTO_TEST_CASE(RuntimeCloseDrainAndAsyncNotificationAreSafe)
{
  TempRuntimeConfig files;
  const auto path = files.makeConfig("requester.json");
  RuntimeConfig config;
  config.nativeConfigPath = path.string();

  auto runtime = Runtime::open(config);
  auto user = runtime->user();
  runtime->close();
  runtime->close();
  BOOST_CHECK_EXCEPTION(runtime->user(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "RUNTIME_CLOSED";
                        });

  BOOST_CHECK_EXCEPTION(runtime->drain(std::chrono::milliseconds(-1)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_ARGUMENT" &&
                                 error.boundary() == "lifecycle";
                        });

  std::mutex mutex;
  std::condition_variable condition;
  bool called = false;
  bool completed = false;
  std::exception_ptr callbackError;
  auto subscription = runtime->drainAsync(
    std::chrono::seconds(2),
    [&] (std::exception_ptr error, bool drained) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        callbackError = error;
        completed = drained;
        called = true;
      }
      // close() is explicitly permitted from a completion callback.  It must
      // not attempt to join the worker that is currently delivering it.
      runtime->close();
      condition.notify_one();
    });
  auto movedSubscription = std::move(subscription);
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(condition.wait_for(lock, std::chrono::seconds(3),
                                     [&] { return called; }));
  }
  BOOST_CHECK(!callbackError);
  BOOST_CHECK(completed);
  BOOST_CHECK(runtime->drain(std::chrono::milliseconds(0)));
  movedSubscription.cancel();

  // The User keeps the State alive after the Runtime shell is released.  The
  // shell destructor has already issued the same idempotent close barrier.
  runtime.reset();
  (void)user;
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncDoesNotImplicitlyClose)
{
  TempRuntimeConfig files;
  RuntimeConfig config;
  config.nativeConfigPath = files.makeConfig("requester.json").string();
  auto runtime = Runtime::open(config);

  std::mutex mutex;
  std::condition_variable condition;
  bool called = false;
  bool completed = false;
  std::exception_ptr callbackError;
  auto subscription = runtime->drainAsync(
    std::chrono::seconds(2),
    [&] (std::exception_ptr error, bool drained) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        callbackError = error;
        called = true;
        completed = drained;
      }
      condition.notify_one();
    });
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(condition.wait_for(lock, std::chrono::seconds(3),
                                     [&] { return called; }));
  }
  BOOST_CHECK(!callbackError);
  BOOST_CHECK(completed);
  BOOST_CHECK_NO_THROW(runtime->user());
  subscription.cancel();

  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_SUITE(Spec185RuntimeT004)

BOOST_AUTO_TEST_CASE(BlockingPrepareFromCoreWorkerReturnsWouldDeadlock)
{
  TempRuntimeConfig files;
  RuntimeConfig config;
  config.nativeConfigPath = files.makeConfig("requester.json").string();
  auto runtime = Runtime::open(config);
  auto user = runtime->user();
  std::promise<std::string> code;
  auto future = code.get_future();
  auto subscription = runtime->drainAsync(std::chrono::seconds(2),
    [&] (std::exception_ptr, bool) {
      try { user.prepare(); }
      catch (const DiError& error) { code.set_value(error.code()); }
      catch (...) { code.set_value("UNKNOWN"); }
    });
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(future.get(), "WOULD_DEADLOCK");
  subscription.cancel();
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
