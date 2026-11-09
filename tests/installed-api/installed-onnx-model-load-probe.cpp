#include <onnxruntime_cxx_api.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int
main(int argc, char** argv)
{
  if (argc != 2) return 2;
  const auto start = std::chrono::steady_clock::now();
  const auto cpu = std::clock();
  try {
    Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "ndnsf-cpu-model-load");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // No CUDA provider is appended: ORT's default session uses CPU only.
    // Session construction checks kernel availability; it does not run inputs.
    {
      Ort::Session session(environment, argv[1], options);
      std::cout << "ONNX_CPU_MODEL_LOAD_PASS {\"runtimeVersion\":"
                << std::quoted(Ort::GetVersionString()) << ",\"model\":"
                << std::quoted(argv[1]) << ",\"inputs\":" << session.GetInputCount()
                << ",\"outputs\":" << session.GetOutputCount() << "}" << std::endl;
    }
    std::cout << "ONNX_CPU_MODEL_LOAD_RESOURCES wallSeconds="
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " cpuSeconds=" << static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC
              << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "ONNX_CPU_MODEL_LOAD_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
