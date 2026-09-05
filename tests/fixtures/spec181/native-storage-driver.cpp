#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

// Cross-language test adapter. All cryptography stays in production functions.
int main(int argc, char** argv)
{
  try {
    if (argc == 3 && std::string(argv[1]) == "assembly-digest") {
      std::cout << ndnsf::di::nativeAssemblyDigestFromCanonicalProjection(argv[2]);
      return 0;
    }
    if (argc != 9) return 2;
    std::vector<std::uint8_t> key(32);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = i;
    ndnsf::di::NativeAssembledEntryContext context{argv[3], argv[4], argv[5], argv[2]};
    std::ifstream input(argv[6], std::ios::binary);
    if (!input) return 2;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    auto output = std::string(argv[1]) == "seal"
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
