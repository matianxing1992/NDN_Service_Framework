#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp"
#include "tests/boost-test.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace ndnsf_distributed_repo::test {

namespace {

namespace fs = std::filesystem;

constexpr char FIXTURE_TEXT[] = "spec240 unified client facade v1";
constexpr char FIXTURE_SHA256[] =
  "66c9264ef1f8025544a394c58a4e120e7bc6bcffe500e07d4db257d43a905d45";

class TemporaryDirectory
{
public:
  explicit TemporaryDirectory(const std::string& label)
  {
    static std::atomic<unsigned> sequence{0};
    m_path = fs::temp_directory_path() /
      ("ndnsf-spec240-client-" + std::to_string(static_cast<unsigned long>(::getpid())) +
       "-" + label + "-" + std::to_string(sequence.fetch_add(1)));
    std::error_code error;
    fs::remove_all(m_path, error);
    fs::create_directories(m_path);
  }

  ~TemporaryDirectory()
  {
    std::error_code error;
    fs::remove_all(m_path, error);
  }

  const fs::path&
  path() const noexcept
  {
    return m_path;
  }

private:
  fs::path m_path;
};

void
writeFile(const fs::path& path, const std::string& bytes)
{
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  BOOST_REQUIRE(output.good() || output.eof());
}

std::vector<uint8_t>
readFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

InAppRepoConfig
makeConfig(const fs::path& root)
{
  InAppRepoConfig config;
  config.durableRoot = root;
  config.ownerId = "/example/spec240/owner";
  return config;
}

FileReference
fixtureReference()
{
  FileReference reference;
  reference.logicalName = "/example/spec240/owner/object";
  reference.contentDigest = FIXTURE_SHA256;
  reference.sizeBytes = sizeof(FIXTURE_TEXT) - 1;
  reference.formatVersion = "raw-file-v1";
  reference.publisherIdentity = "/example/spec240/owner";
  reference.policyEpoch = "1";
  return reference;
}

RepoOperationOptions
makeOptions()
{
  RepoOperationOptions options;
  options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  return options;
}

/** The facade must not silently accept an operation it cannot honor. */
bool
isUnsupported(const RepoClientError& error)
{
  return error.code() == "UNSUPPORTED";
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec240RepoClientApi)

BOOST_AUTO_TEST_CASE(FacadePublishesFetchesInspectsAndReopens)
{
  TemporaryDirectory temporary("facade");
  const fs::path root = temporary.path() / "repo";
  const fs::path source = temporary.path() / "source.bin";
  const fs::path firstFetch = temporary.path() / "first.bin";
  const fs::path secondFetch = temporary.path() / "second.bin";
  writeFile(source, FIXTURE_TEXT);

  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = fixtureReference();

  RepoPublishResult published;
  {
    auto client = RepoClient::makeInApp(makeConfig(root));
    published = client.publishFile(publication, makeOptions());
    BOOST_REQUIRE(published.committedHandle);
    BOOST_CHECK(published.state == RepoPublishState::COMMITTED);

    RepoFileFetch fetch;
    fetch.handle = published.committedHandle;
    fetch.targetPath = firstFetch.string();
    const auto fetched = client.fetchFile(fetch, makeOptions());
    BOOST_CHECK_EQUAL(fetched.bytesWritten, published.committedHandle->reference.index() == 0
      ? std::get<FileReference>(published.committedHandle->reference).sizeBytes : 0);
    BOOST_CHECK(readFile(firstFetch) == readFile(source));

    RepoInspectionRequest inspection;
    inspection.kind = RepoInspectionRequest::Kind::OBJECT;
    inspection.object = published.committedHandle;
    const auto observed = client.inspect(inspection, makeOptions());
    BOOST_CHECK(observed.availability == RepoInspection::Availability::AVAILABLE);

    BOOST_CHECK(client.close(std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  }

  // A new client instance over the same durable root is the restart oracle.
  auto reopened = RepoClient::makeInApp(makeConfig(root));
  RepoFileFetch fetch;
  fetch.reference = fixtureReference();
  fetch.targetPath = secondFetch.string();
  const auto fetchedAgain = reopened.fetchFile(fetch, makeOptions());
  BOOST_CHECK_EQUAL(fetchedAgain.bytesWritten, sizeof(FIXTURE_TEXT) - 1);
  BOOST_CHECK(readFile(secondFetch) == readFile(source));
}

BOOST_AUTO_TEST_CASE(UnimplementedOperationsFailHonestly)
{
  TemporaryDirectory temporary("unsupported");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));

  BOOST_CHECK_THROW(client.publishData(RepoDataPublication{}, makeOptions()), RepoClientError);
  BOOST_CHECK_THROW(client.fetchData(RepoDataFetch{}, makeOptions()), RepoClientError);
  BOOST_CHECK_THROW(client.list(RepoListRequest{}, makeOptions()), RepoClientError);
  BOOST_CHECK_THROW(client.remove(RepoDeleteRequest{}, makeOptions()), RepoClientError);
  BOOST_CHECK_THROW(client.authorizeWrite(RepoWriteAuthorization{}, makeOptions()), RepoClientError);

  int unsupported = 0;
  const auto expectUnsupported = [&](const std::function<void()>& operation) {
    try {
      operation();
    }
    catch (const RepoClientError& error) {
      if (isUnsupported(error))
        ++unsupported;
    }
  };
  expectUnsupported([&] { client.publishData(RepoDataPublication{}, makeOptions()); });
  expectUnsupported([&] { client.fetchData(RepoDataFetch{}, makeOptions()); });
  expectUnsupported([&] { client.list(RepoListRequest{}, makeOptions()); });
  expectUnsupported([&] { client.remove(RepoDeleteRequest{}, makeOptions()); });
  expectUnsupported([&] { client.authorizeWrite(RepoWriteAuthorization{}, makeOptions()); });
  BOOST_CHECK_EQUAL(unsupported, 5);
}

BOOST_AUTO_TEST_CASE(InvalidInputFailsBeforeSideEffects)
{
  TemporaryDirectory temporary("invalid");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));

  RepoFilePublication publication;
  publication.sourcePath = (temporary.path() / "missing.bin").string();
  BOOST_CHECK_THROW(client.publishFile(publication, makeOptions()), std::invalid_argument);

  RepoFileFetch fetch;
  fetch.targetPath = (temporary.path() / "out.bin").string();
  BOOST_CHECK_THROW(client.fetchFile(fetch, makeOptions()), std::invalid_argument);

  RepoInspectionRequest inspection;
  inspection.kind = RepoInspectionRequest::Kind::NODE;
  BOOST_CHECK_THROW(client.inspect(inspection, makeOptions()), RepoClientError);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
