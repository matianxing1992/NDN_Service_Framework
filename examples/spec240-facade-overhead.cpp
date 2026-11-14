// Measures the unified client facade against the engine it delegates to, in one
// process and one window, so the facade's own overhead is visible.
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"

#include <openssl/evp.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace ndnsf_distributed_repo;

namespace {

double
seconds(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string
sha256Of(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  EVP_DigestInit_ex(context, EVP_sha256(), nullptr);
  EVP_DigestUpdate(context, bytes.data(), bytes.size());
  EVP_DigestFinal_ex(context, digest, &length);
  EVP_MD_CTX_free(context);
  std::string hex;
  hex.reserve(length * 2);
  static const char digits[] = "0123456789abcdef";
  for (unsigned int index = 0; index < length; ++index) {
    hex.push_back(digits[digest[index] >> 4]);
    hex.push_back(digits[digest[index] & 0x0f]);
  }
  return hex;
}

} // namespace

int
main(int argc, char** argv)
{
  const std::size_t megabytes = argc > 1 ? std::stoul(argv[1]) : 64;
  const fs::path root = fs::temp_directory_path() / "spec240-overhead";
  std::error_code error;
  fs::remove_all(root, error);
  fs::create_directories(root);
  const fs::path source = root / "blob.bin";
  {
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    std::mt19937 generator(7);
    std::vector<char> block(1 << 20);
    for (std::size_t index = 0; index < megabytes; ++index) {
      for (auto& byte : block)
        byte = static_cast<char>(generator());
      output.write(block.data(), block.size());
    }
  }
  const auto digest = sha256Of(source);
  const auto size = static_cast<std::uint64_t>(fs::file_size(source));

  FileReference reference;
  reference.logicalName = "/example/spec240/overhead/blob";
  reference.contentDigest = digest;
  reference.sizeBytes = size;
  reference.formatVersion = "raw-file-v1";
  reference.publisherIdentity = "/example/spec240/overhead";
  reference.policyEpoch = "1";

  RepoOperationOptions options;
  options.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

  // Facade path.
  auto facade = RepoClient::makeInApp({root / "facade-repo", "/example/spec240/overhead"});
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = reference;
  auto start = std::chrono::steady_clock::now();
  const auto published = facade.publishFile(publication, options);
  const auto facadePublish = seconds(start);
  RepoFileFetch fetch;
  fetch.reference = reference;
  fetch.targetPath = (root / "facade-out.bin").string();
  start = std::chrono::steady_clock::now();
  facade.fetchFile(fetch, options);
  const auto facadeFetch = seconds(start);

  // Engine path, same operations.
  auto engine = ArtifactRepositoryApi::makeInApp({root / "engine-repo", "/example/spec240/overhead"});
  ArtifactReference engineReference;
  engineReference.logicalName = reference.logicalName;
  engineReference.contentDigest = reference.contentDigest;
  engineReference.sizeBytes = reference.sizeBytes;
  engineReference.formatVersion = reference.formatVersion;
  engineReference.publisherIdentity = reference.publisherIdentity;
  engineReference.policyEpoch = reference.policyEpoch;
  start = std::chrono::steady_clock::now();
  engine.publishFileUntil(source.string(), engineReference, options.deadline);
  const auto enginePublish = seconds(start);
  start = std::chrono::steady_clock::now();
  engine.fetchFileUntil(engineReference, (root / "engine-out.bin").string(), options.deadline);
  const auto engineFetch = seconds(start);

  std::printf("size=%llu facade_publish_s=%.3f engine_publish_s=%.3f "
              "facade_fetch_s=%.3f engine_fetch_s=%.3f committed=%d\n",
              static_cast<unsigned long long>(size), facadePublish, enginePublish,
              facadeFetch, engineFetch,
              published.state == RepoPublishState::COMMITTED ? 1 : 0);
  fs::remove_all(root, error);
  return 0;
}
