#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

// The storage-only driver does not exercise runtime evidence; satisfy the
// ProtectedRuntime translation unit without pulling the logging stack into
// this focused production-entry test.
namespace ndnsf::di {
void logRuntimeEvidence(const std::string&) {}
}

// Cross-language test adapter. All cryptography stays in production functions.
int main(int argc, char** argv)
{
  try {
    if (argc == 3 && std::string(argv[1]) == "assembly-digest") {
      std::cout << ndnsf::di::nativeAssemblyDigestFromCanonicalProjection(argv[2]);
      return 0;
    }
    if (argc < 9 || argc > 10) return 2;
    std::vector<std::uint8_t> key(32);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = i;
    ndnsf::di::NativeAssembledEntryContext context{argv[3], argv[4], argv[5], argv[2]};
    const auto mode = std::string(argv[1]);
    std::vector<std::uint8_t> bytes;
    if (mode != "open-file") {
      std::ifstream input(argv[6], std::ios::binary);
      if (!input) return 2;
      bytes.assign(std::istreambuf_iterator<char>(input), {});
    }
    if (mode == "seal-file") {
      const auto digest = ndnsf::di::sealNativeAssembledEntryToFile(
        key, bytes, argv[7], context);
      std::cout << digest;
      return 0;
    }
    if (mode == "open-file") {
      const auto digest = ndnsf::di::openNativeAssembledEntryToFile(
        key, argv[6], argv[7], context, std::stoull(argv[8]),
        argc == 10 ? argv[9] : "");
      std::cout << digest;
      return 0;
    }
    auto output = mode == "seal"
      ? ndnsf::di::sealNativeAssembledEntry(key, bytes, context)
      : ndnsf::di::openNativeAssembledEntry(key, bytes, context, std::stoull(argv[8]));
    std::ofstream destination(argv[7], std::ios::binary);
    destination.write(reinterpret_cast<const char*>(output.data()), output.size());
    return destination ? 0 : 2;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
