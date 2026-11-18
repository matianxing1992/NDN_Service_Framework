#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp"
#include "tests/boost-test.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo::test {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t DEFAULT_BYTES = 64 * 1024 * 1024;

std::size_t
payloadBytes()
{
  const char* text = std::getenv("REPO_IO_BENCHMARK_BYTES");
  if (text == nullptr || *text == '\0')
    return DEFAULT_BYTES;
  try {
    const long long value = std::stoll(text);
    if (value <= 0 || value > 512LL * 1024 * 1024)
      return DEFAULT_BYTES;
    return static_cast<std::size_t>(value);
  }
  catch (const std::exception&) {
    return DEFAULT_BYTES;
  }
}

std::vector<std::uint8_t>
fixtureBytes(std::size_t size)
{
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t index = 0; index < size; ++index)
    bytes[index] = static_cast<std::uint8_t>((index * 131 + 17) & 0xff);
  return bytes;
}

fs::path
makeWorkspace()
{
  auto pattern = (fs::temp_directory_path() / "ndnsf-repo-io-benchmark-XXXXXX").string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr)
    throw std::runtime_error("cannot create benchmark workspace");
  return fs::path(buffer.data());
}

InAppRepoConfig
makeConfig(const fs::path& root)
{
  InAppRepoConfig config;
  config.durableRoot = root;
  config.ownerId = "/example/spec233/benchmark";
  return config;
}

double
megabytesPerSecond(std::size_t bytes, double seconds)
{
  return seconds <= 0 ? 0 : static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

} // namespace

BOOST_AUTO_TEST_SUITE(RepoIoBenchmark)

BOOST_AUTO_TEST_CASE(PublishesAndFetchesALargeObject)
{
  const auto bytes = payloadBytes();
  const auto payload = fixtureBytes(bytes);
  const auto workspace = makeWorkspace();
  const auto source = workspace / "source.bin";
  {
    std::ofstream output(source, std::ios::binary);
    output.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
  }
  const auto digest = sha256Hex(payload);

  FileReference reference;
  reference.logicalName = "/example/spec233/benchmark/owner/object";
  reference.contentDigest = digest;
  reference.sizeBytes = bytes;
  reference.formatVersion = "raw-file-v1";
  reference.publisherIdentity = "/example/spec233/benchmark/owner";
  reference.policyEpoch = "1";

  RepoOperationOptions options;
  options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);

  auto client = RepoClient::makeInApp(makeConfig(workspace / "repo"));
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = reference;
  const auto writeBegin = std::chrono::steady_clock::now();
  const auto published = client.publishFile(publication, options);
  const auto writeEnd = std::chrono::steady_clock::now();
  BOOST_REQUIRE(published.committedHandle);
  BOOST_REQUIRE(published.state == RepoPublishState::COMMITTED);

  RepoFileFetch fetch;
  fetch.handle = published.committedHandle;
  fetch.targetPath = (workspace / "fetch.bin").string();
  const auto readBegin = std::chrono::steady_clock::now();
  const auto fetched = client.fetchFile(fetch, options);
  const auto readEnd = std::chrono::steady_clock::now();
  BOOST_REQUIRE_EQUAL(fetched.bytesWritten, bytes);
  BOOST_REQUIRE_EQUAL(fetched.bytesVerified, bytes);

  const auto writeSeconds = std::chrono::duration<double>(writeEnd - writeBegin).count();
  const auto readSeconds = std::chrono::duration<double>(readEnd - readBegin).count();
  std::printf("REPO_IO_BENCHMARK {\"bytes\":%zu,\"writeSeconds\":%.3f,"
              "\"writeMBps\":%.2f,\"readSeconds\":%.3f,\"readMBps\":%.2f}\n",
              bytes, writeSeconds, megabytesPerSecond(bytes, writeSeconds),
              readSeconds, megabytesPerSecond(bytes, readSeconds));
  std::remove(source.string().c_str());
  std::remove(fetch.targetPath.c_str());
  std::error_code ignored;
  fs::remove_all(workspace, ignored);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
