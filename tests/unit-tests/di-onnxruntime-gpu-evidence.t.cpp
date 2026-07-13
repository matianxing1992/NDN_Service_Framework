#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>

namespace ndnsf::di::tests {

BOOST_AUTO_TEST_SUITE(DiOnnxRuntimeGpuEvidence)

BOOST_AUTO_TEST_CASE(CudaSelectionRejectsFallbackAndCpuOnlyAllocation)
{
  NativeModelRunnerSpec spec;
  spec.metadata["executionProvider"] = "cuda";
  spec.metadata["deviceId"] = "1";
  spec.metadata["allowCpuFallback"] = "false";
  BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, {"CPUExecutionProvider"}),
                    std::runtime_error);
  const auto selected = resolveOnnxRuntimeProviderSelection(
    spec, {"CUDAExecutionProvider", "CPUExecutionProvider"});
  BOOST_CHECK_EQUAL(selected.selectedProvider, "cuda");
  BOOST_CHECK_EQUAL(selected.deviceId, "1");
  BOOST_CHECK(!selected.usedCpuFallback);
}

BOOST_AUTO_TEST_CASE(OrtProfileRecordsEveryModelNodeProviderAndGpuUuid)
{
  const auto path = std::filesystem::temp_directory_path() /
                    "ndnsf-spec109-ort-profile.json";
  std::ofstream(path) << R"json([
    {"cat":"Session","name":"model_loading_uri"},
    {"cat":"Node","name":"MatMul_0_kernel_time","args":{"provider":"CUDAExecutionProvider"}},
    {"cat":"Node","name":"Add_1_kernel_time","args":{"provider":"CUDAExecutionProvider"}}
  ])json";
  ExecutionEvidence evidence;
  applyOnnxRuntimeProviderProfile(evidence, path.string(), "/LLM/Stage/0", false,
                                  "GPU-test-uuid");
  BOOST_REQUIRE_EQUAL(evidence.nodeProviderAssignments.size(), 2);
  BOOST_CHECK_EQUAL(evidence.nodeProviderAssignments.front().provider,
                    "CUDAExecutionProvider");
  BOOST_CHECK_EQUAL(evidence.gpuUuid, "GPU-test-uuid");
  BOOST_CHECK(!evidence.cpuFallbackUsed);
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
