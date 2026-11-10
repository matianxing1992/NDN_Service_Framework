#include <NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int
main(int argc, char** argv)
{
  try {
    const bool identityOnly = argc == 6 && std::string(argv[3]) == "--identity";
    if (argc != 3 && !identityOnly)
      throw std::invalid_argument("usage: installed-material-plan-probe MODEL EXPECTED_DIGEST"
                                 " [--identity INITIALIZER EXPECTED_INITIALIZER_DIGEST]");
    const std::filesystem::path path(argv[1]);
    const auto bytes = std::filesystem::file_size(path);
    if (!path.is_absolute() || path != std::filesystem::canonical(path) ||
        !std::filesystem::is_regular_file(path) || bytes == 0 || bytes > (1ULL << 30))
      throw std::invalid_argument("bounded canonical model required");
    using Clock = std::chrono::steady_clock;
    const auto readStart = Clock::now();
    const auto readCpu = std::clock();
    ndnsf::di::NativeCanonicalSource source;
    source.modelBytes.resize(bytes);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(source.modelBytes.data()), bytes) ||
        input.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("source size changed or unreadable");
    const auto digest = ndnsf::di::nativePlanningDigest(source.modelBytes.data(), bytes);
    if (digest != argv[2])
      throw std::runtime_error("pinned model digest mismatch");
    std::cout << "{\"phase\":\"model-read-and-hash\",\"wallSeconds\":"
              << std::chrono::duration<double>(Clock::now() - readStart).count()
              << ",\"cpuSeconds\":"
              << static_cast<double>(std::clock() - readCpu) / CLOCKS_PER_SEC << "}" << std::endl;
    const auto start = Clock::now();
    const auto cpu = std::clock();
    ndnsf::di::NativeAssemblyControl control;
    control.deadline = start + std::chrono::seconds(identityOnly ? 600 : 60);
    control.maxSourceBytes = 1ULL << (identityOnly ? 36 : 30);
    control.maxAssembledBytes = control.maxSourceBytes;
    control.requireActive = [deadline = control.deadline] {
      if (Clock::now() >= deadline)
        throw std::runtime_error("native material plan probe deadline");
    };
    if (identityOnly) {
      const std::filesystem::path initializer(argv[4]);
      const auto initializerBytes = std::filesystem::file_size(initializer);
      if (!initializer.is_absolute() || initializer != std::filesystem::canonical(initializer) ||
          !std::filesystem::is_regular_file(initializer) || initializerBytes == 0 ||
          initializerBytes > control.maxSourceBytes - bytes)
        throw std::invalid_argument("bounded canonical initializer required");
      source.initializerRangeSource = ndnsf::di::openNativeCanonicalInitializerFile(
        initializer.string(), initializerBytes, argv[5], control);
      const auto identity = ndnsf::di::canonicalOnnxSourceIdentity(source, control);
      std::cout << "{\"phase\":\"native-canonical-identity\",\"graphDigest\":\""
                << identity.graphDigest << "\",\"initializerDigest\":\""
                << identity.initializerDigest << "\",\"initializerBytes\":" << initializerBytes
                << ",\"wallSeconds\":"
                << std::chrono::duration<double>(Clock::now() - start).count()
                << ",\"cpuSeconds\":"
                << static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC
                << ",\"scope\":\"native identity only; no publication or inference\"}"
                << std::endl;
      return 0;
    }
    const auto manifest = ndnsf::di::deriveNativeCanonicalMaterialManifest(source, control);
    uint64_t materialBytes = 0;
    uint64_t largest = 0;
    for (const auto& payload : manifest->payloads) {
      materialBytes += payload.byteSize();
      largest = std::max(largest, payload.byteSize());
    }
    const auto bundleBound = ndnsf::di::NativeCanonicalMaterialBundleMaxBytes;
    std::cout << "{\"phase\":\"native-material-plan\",\"wallSeconds\":"
              << std::chrono::duration<double>(Clock::now() - start).count()
              << ",\"cpuSeconds\":" << static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC
              << ",\"sourceDigest\":\"" << digest << "\",\"sourceBytes\":" << bytes
              << ",\"payloads\":" << manifest->payloads.size()
              << ",\"materialBytes\":" << materialBytes << ",\"largestPayloadBytes\":" << largest
              << ",\"bundleByteLimit\":" << bundleBound
              << ",\"minimumBundlesByBytes\":" << (materialBytes + bundleBound - 1) / bundleBound
              << ",\"scope\":\"material derivation only; no protection, publication or inference\"}"
              << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "MATERIAL_PLAN_PROBE_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
