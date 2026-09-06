#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
  using namespace ndnsf::di;
  try {
    if (argc == 2) {
      std::cout << queryCudaDeviceUuid(std::stoi(argv[1])) << std::endl;
      return 0;
    }
    NativeModelRunnerSpec spec;
    spec.role = "BackboneNeck";
    spec.metadata = {{"evidence.providerName", "/provider/A"},
      {"evidence.providerBootId", "boot-a"}, {"evidence.epoch", "1"},
      {"evidence.createdAtMs", "1"}, {"evidence.modelDigest", "sha256:model"},
      {"evidence.planDigest", "sha256:plan"}, {"evidence.artifactDigest", "sha256:artifact"},
      {"evidence.gpuUuid", "GPU-forged"}};
    auto e = executionEvidenceFromRunnerSpec(spec, RunnerKind::OnnxRuntimeCuda,
                                            "test-runtime", "cuda", "0");
    if (!e.gpuUuid.empty()) throw std::runtime_error("untrusted UUID accepted");
    applyOnnxRuntimeProviderProfile(e, argv[1], spec.role, false,
                                   "GPU-00010203-0405-0607-0809-0a0b0c0d0e0f");
    e.gpuIdentitySource = "cuda-runtime-pci+driver-uuid";
    e.profileRequestId = "/request/A";
    e.profileAttemptEpoch = 1;
    bindExecutionObservation(e, "/request/A", 1, std::string(argv[2]) == "cache");
    auto decoded = executionEvidenceFromJson(executionEvidenceToJson(e));
    std::cout << executionEvidenceToJson(decoded) << std::endl;
  }
  catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 7;
  }
}
