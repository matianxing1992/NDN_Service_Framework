#include "tests/fixtures/owned-test-process.hpp"
#include "tests/fixtures/test-resource-paths.hpp"
#include "tests/fixtures/wan-video/wan-video-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ndnsf::di::tests {
namespace {

namespace fs = std::filesystem;

struct CheckpointOracleFixture
{
  fs::path temporaryRoot;
  fs::path root;
  NativeJson config;
  unsigned calls = 0;

  CheckpointOracleFixture()
  {
    auto pattern = (fs::temp_directory_path() / "wan-checkpoint-cli-XXXXXX").string();
    const auto* made = ::mkdtemp(pattern.data());
    if (!made) throw std::runtime_error("cannot create checkpoint CLI fixture");
    temporaryRoot = made;
    root = temporaryRoot / "fixture";
    try {
      config = wan::fixture::writeWanCheckpointOracleFixture(root);
    }
    catch (...) {
      std::error_code error;
      fs::remove_all(temporaryRoot, error);
      throw;
    }
  }

  ~CheckpointOracleFixture()
  {
    std::error_code error;
    fs::remove_all(temporaryRoot, error);
  }

  static std::string
  raw(const std::vector<float>& values)
  {
    return {reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float)};
  }

  static std::string
  digest(const std::string& bytes)
  {
    return nativePlanningDigest(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  }

  void
  write(const std::string& name, const std::string& bytes) const
  {
    std::ofstream stream(root / name, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(bytes.data(), bytes.size());
  }

  std::string
  read(const std::string& name) const
  {
    std::ifstream stream(root / name, std::ios::binary);
    if (!stream) throw std::runtime_error("checkpoint fixture output missing: " + name);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  }

  std::pair<int, std::string>
  run(bool cuda, const std::string& outputName = "cuda")
  {
    write("oracle.json", nativeCanonicalJson(config));
    const auto* configured = std::getenv("NDNSF_WAN_CHECKPOINT_ORACLE");
    const auto binary = configured ? fs::path(configured) :
      ndnsf::test::testBinaryPath("di-wan-checkpoint-oracle");
    if (!fs::is_regular_file(binary) || ::access(binary.c_str(), X_OK) != 0)
      throw std::runtime_error("checkpoint oracle executable is missing");
    std::vector<std::string> arguments{binary.string()};
    if (cuda) arguments.push_back("--cuda");
    arguments.push_back((root / "oracle.json").string());
    if (cuda) arguments.push_back((root / outputName).string());
    const auto log = "child-" + std::to_string(calls++) + ".log";
    const auto status = ndn_service_framework::test::runOwnedTestProcess(
      binary, std::move(arguments), {}, std::chrono::seconds(30), root / log);
    return {status, read(log)};
  }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(WanCheckpointOracleCli, CheckpointOracleFixture)

BOOST_AUTO_TEST_CASE(CpuDefaultExecutesIndependentExactFixture)
{
  const auto [status, log] = run(false);
  BOOST_CHECK_MESSAGE(status == 0, log);
  BOOST_CHECK(log.find("elements=3 max_abs=0 failures=0") != std::string::npos);
  BOOST_CHECK(!fs::exists(root / "cuda"));
}

BOOST_AUTO_TEST_CASE(CudaRejectsWrongInputReferenceAndGraphBeforeDeviceOrOutput)
{
  const auto original = config;
  for (const auto& boundary : {"input", "reference", "model graph"}) {
    config = original;
    const auto wrong = "sha256:" + std::string(64, '0');
    if (std::string(boundary) == "input") config["inputs"][0]["digest"] = wrong;
    else if (std::string(boundary) == "reference") config["reference"]["digest"] = wrong;
    else config["model_graph_digest"] = wrong;
    const auto [status, log] = run(true);
    BOOST_CHECK_MESSAGE(status == 1, log);
    BOOST_CHECK_MESSAGE(log.find(std::string("checkpoint oracle ") + boundary +
                                " identity differs") != std::string::npos, log);
    BOOST_CHECK(!fs::exists(root / "cuda"));
  }
}

BOOST_AUTO_TEST_CASE(CudaDoesNotReuseOutputDirectory)
{
  fs::create_directory(root / "cuda");
  write("cuda/canary", "retained");
  const auto [status, log] = run(true);
  BOOST_CHECK_MESSAGE(status == 1, log);
  BOOST_CHECK(log.find("CUDA output directory already exists") != std::string::npos);
  BOOST_CHECK_EQUAL(read("cuda/canary"), "retained");
  BOOST_CHECK_EQUAL(std::distance(fs::directory_iterator(root / "cuda"),
                                fs::directory_iterator()), 1);
}

BOOST_AUTO_TEST_CASE(CpuNumericalFailureIsPreserved)
{
  const auto bytes = raw({-1.0F, 1.0F, 5.5F});
  write("reference.raw", bytes);
  config["reference"]["digest"] = digest(bytes);
  const auto [status, log] = run(false);
  BOOST_CHECK_MESSAGE(status == 1, log);
  BOOST_CHECK(log.find("failures=1") != std::string::npos);
  BOOST_CHECK(log.find("checkpoint oracle numerical mismatch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CudaUnavailableBackendDoesNotFallback,
                     *boost::unit_test::disabled())
{
  const auto [status, log] = run(true);
  BOOST_REQUIRE_MESSAGE(status == 1, log);
  BOOST_CHECK_MESSAGE(log.find("required ONNX Runtime CUDAExecutionProvider is unavailable; "
                              "CPU fallback is disabled") != std::string::npos, log);
  BOOST_CHECK(!fs::exists(root / "cuda/evidence.json"));
  BOOST_CHECK(!fs::exists(root / "cuda/actual.f32le"));
}

BOOST_AUTO_TEST_CASE(CudaExecutesWithCurrentProfile, *boost::unit_test::disabled())
{
  const auto [status, log] = run(true);
  BOOST_REQUIRE_MESSAGE(status == 0, log);
  BOOST_CHECK(log.find("elements=3 max_abs=0 failures=0") != std::string::npos);
  BOOST_CHECK_EQUAL(read("cuda/actual.f32le"), read("reference.raw"));
  const auto receipt = nativeParseJson(read("cuda/evidence.json"));
  BOOST_CHECK_EQUAL(receipt.at("model_graph_digest").get<std::string>(),
                    config.at("model_graph_digest").get<std::string>());
  const auto evidence = executionEvidenceFromJson(
    nativeCanonicalJson(receipt.at("execution_evidence")));
  BOOST_CHECK_EQUAL(toString(evidence.runnerKind), "onnxruntime-cuda");
  BOOST_CHECK(evidence.executionCompleted);
  BOOST_CHECK(!evidence.cpuFallbackUsed);
  BOOST_CHECK(!evidence.gpuUuid.empty());
  BOOST_CHECK_EQUAL(evidence.profileRequestId, evidence.requestId);
  const auto actual = read("cuda/actual.f32le");
  const auto wrongReference = raw({-1.0F, 1.0F, 5.5F});
  write("reference.raw", wrongReference);
  config["reference"]["digest"] = digest(wrongReference);
  const auto [failure, failureLog] = run(true, "cuda-mismatch");
  BOOST_CHECK_MESSAGE(failure == 1, failureLog);
  BOOST_CHECK(failureLog.find("checkpoint oracle numerical mismatch") != std::string::npos);
  BOOST_CHECK_EQUAL(read("cuda-mismatch/actual.f32le"), actual);
  BOOST_CHECK(fs::is_regular_file(root / "cuda-mismatch/evidence.json"));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
