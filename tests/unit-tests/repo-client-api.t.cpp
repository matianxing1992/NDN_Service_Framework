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
  // A well-formed delete is needed here: malformed shapes are rejected as caller
  // errors before the mode check (see RemoveRequiresATypedTargetAndCleanupId).
  RepoDeleteRequest deleteRequest;
  deleteRequest.targets = {RepoTarget{"/repo/node", "/provider", "/SVC/repo"}};
  deleteRequest.generation = "cleanup-1";
  FileReference deleteReference;
  deleteReference.logicalName = "/example/spec240/owner/object";
  deleteRequest.reference = deleteReference;
  BOOST_CHECK_THROW(client.remove(deleteRequest, makeOptions()), RepoClientError);
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
  expectUnsupported([&] { client.remove(deleteRequest, makeOptions()); });
  expectUnsupported([&] { client.authorizeWrite(RepoWriteAuthorization{}, makeOptions()); });
  BOOST_CHECK_EQUAL(unsupported, 5);
}

BOOST_AUTO_TEST_CASE(FilePublicationReturnsAFileReferenceNotAPacketSet) {
  // CA09: the two result shapes must stay distinguishable, so a packet set can never
  // be mistaken for a file object.
  TemporaryDirectory temporary("result-shape");
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_TEXT);
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = fixtureReference();
  const auto published = client.publishFile(publication, makeOptions());
  BOOST_REQUIRE(published.reference.has_value());
  BOOST_CHECK(!published.packetSet.has_value());
  BOOST_CHECK(std::holds_alternative<FileReference>(*published.reference));
}

BOOST_AUTO_TEST_CASE(CloseRefusesNewWorkAndReportsWhetherItDrained) {
  // CA08: close must not claim a drain it did not observe, and an operation started
  // after close must be refused instead of running on released state.
  TemporaryDirectory temporary("close");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  BOOST_CHECK(client.close(std::chrono::steady_clock::now() + std::chrono::seconds(1)));

  RepoInspectionRequest inspection;
  inspection.kind = RepoInspectionRequest::Kind::NODE;
  BOOST_CHECK_THROW(client.inspect(inspection, makeOptions()), RepoClientError);

  RepoFilePublication publication;
  publication.sourcePath = (temporary.path() / "missing.bin").string();
  publication.reference = fixtureReference();
  BOOST_CHECK_THROW(client.publishFile(publication, makeOptions()), RepoClientError);
}

BOOST_AUTO_TEST_CASE(RemoveRequiresATypedTargetAndCleanupId) {
  // CA07: a delete without a target, a cleanup id or any object binding must be a
  // caller error, not a silent default to epoch zero or a constant cleanup id.
  TemporaryDirectory temporary("remove");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoDeleteRequest noTarget;
  BOOST_CHECK_THROW(client.remove(noTarget, makeOptions()), std::invalid_argument);

  RepoDeleteRequest noCleanup;
  noCleanup.targets = {RepoTarget{"/repo/node", "/provider", "/SVC/repo"}};
  BOOST_CHECK_THROW(client.remove(noCleanup, makeOptions()), std::invalid_argument);

  RepoDeleteRequest noObject;
  noObject.targets = {RepoTarget{"/repo/node", "/provider", "/SVC/repo"}};
  noObject.generation = "cleanup-1";
  BOOST_CHECK_THROW(client.remove(noObject, makeOptions()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ReceiptInspectionRequiresTheObjectBinding) {
  // CA05: a receipt id alone is history, not proof of current availability.
  TemporaryDirectory temporary("receipt");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoInspectionRequest request;
  request.kind = RepoInspectionRequest::Kind::RECEIPT;
  request.receiptId = "receipt-1";
  BOOST_CHECK_EXCEPTION(client.inspect(request, makeOptions()), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "UNSUPPORTED" && error.phase() == "inspect";
    });
}

BOOST_AUTO_TEST_CASE(DiscoveryRejectsUnknownCapabilitiesBeforeAnyRequest) {
  TemporaryDirectory temporary("discovery");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoDiscoveryRequest request;
  request.requiredCapabilities = {"not-a-real-capability"};
  request.ackCollectionMs = 10;
  // The mapping must reject an unknown capability instead of writing it into a
  // typed requirement field and issuing a request.
  BOOST_CHECK_THROW(client.discover(request, makeOptions()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RangedFetchReturnsTheExactStoredBytes) {
  // A partial read must return the stored bytes for that window and must not claim a
  // whole-file digest verification.
  TemporaryDirectory temporary("range");
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_TEXT);
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = fixtureReference();
  BOOST_REQUIRE(client.publishFile(publication, makeOptions()).committedHandle.has_value());

  RepoFileFetch fetch;
  fetch.reference = fixtureReference();
  fetch.targetPath = (temporary.path() / "window.bin").string();
  fetch.hasRange = true;
  fetch.rangeOffsetBytes = 7;
  fetch.rangeLengthBytes = 9;
  const auto fetched = client.fetchFile(fetch, makeOptions());
  BOOST_CHECK_EQUAL(fetched.bytesWritten, 9U);
  BOOST_CHECK(!fetched.digestVerified);
  const auto window = readFile(fetch.targetPath);
  BOOST_CHECK_EQUAL(window.size(), 9U);
  const std::string expected(FIXTURE_TEXT + 7, 9);
  BOOST_CHECK(std::string(window.begin(), window.end()) == expected);
}

BOOST_AUTO_TEST_CASE(RepublishingTheSameObjectReportsDeduplication) {
  // The engine already decides this; the facade must surface it rather than hide it,
  // because a caller that cannot tell a replay from a fresh commit cannot recover.
  TemporaryDirectory temporary("replay");
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_TEXT);
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = fixtureReference();

  const auto first = client.publishFile(publication, makeOptions());
  BOOST_CHECK(first.state == RepoPublishState::COMMITTED);
  BOOST_CHECK(!first.deduplicated);

  const auto replay = client.publishFile(publication, makeOptions());
  BOOST_CHECK(replay.state == RepoPublishState::COMMITTED);
  BOOST_CHECK(replay.deduplicated);
}

BOOST_AUTO_TEST_CASE(PublishFileValidatesSelectionShapes) {
  // A selection is honoured now, so the caller errors are shape errors: an
  // incomplete target and an ACCEPTED request without a selection.
  TemporaryDirectory temporary("publish-shapes");
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_TEXT);
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));

  RepoFilePublication incomplete;
  incomplete.sourcePath = source.string();
  incomplete.reference = fixtureReference();
  incomplete.selectedTargets = {RepoTarget{"/repo/node", "", "/SVC/repo"}};
  BOOST_CHECK_THROW(client.publishFile(incomplete, makeOptions()), std::invalid_argument);

  RepoFilePublication accepted;
  accepted.sourcePath = source.string();
  accepted.reference = fixtureReference();
  accepted.completion = RepoCompletion::ACCEPTED;
  BOOST_CHECK_THROW(client.publishFile(accepted, makeOptions()), std::invalid_argument);

  // An ACCEPTED request without an idempotency key could never be recovered on retry.
  RepoFilePublication unrecoverable = accepted;
  unrecoverable.selectedTargets = {RepoTarget{"/repo/node", "/provider", "/SVC/repo"}};
  BOOST_CHECK_THROW(client.publishFile(unrecoverable, makeOptions()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DigestCommitmentsCompareCaseInsensitivelyOnlyForSha256) {
  // CA03: hex case is a rendering detail. A correct set committed in lowercase must
  // verify, and anything that is not a full sha256 commitment stays untouched.
  const std::string lower =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const std::string upper =
    "sha256:0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
  BOOST_CHECK_EQUAL(canonicalSha256Digest(upper), lower);
  BOOST_CHECK_EQUAL(canonicalSha256Digest(lower), lower);
  BOOST_CHECK_EQUAL(canonicalSha256Digest("sha256:ABC"), "sha256:ABC");
  BOOST_CHECK_EQUAL(canonicalSha256Digest("sha512:" + std::string(64, 'A')),
                    "sha512:" + std::string(64, 'A'));
}

BOOST_AUTO_TEST_CASE(DataOperationsNeverPullTheSourceBeforeRejecting) {
  TemporaryDirectory temporary("no-side-effects");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));

  // CA01: a missing verifier must never be an implicit approval, and the
  // rejection has to happen before the packet source is touched. The in-app
  // client answers UNSUPPORTED first; either way the source must stay untouched.
  int pulled = 0;
  RepoDataPublication publication;
  PacketSource source;
  source.packetNames = {"/example/spec240/owner/object/0"};
  source.packetOffsets = {0, 8};
  source.expectedWireBytes = 8;
  source.expectedDigest = "sha256:" + std::string(64, '0');
  source.next = [&pulled](std::vector<uint8_t>& wire) {
    ++pulled;
    wire.assign(8, 0);
    return true;
  };
  publication.packets = std::move(source);
  publication.target = {"/repo/node", "/provider", "/SVC/repo"};
  BOOST_CHECK_THROW(client.publishData(publication, makeOptions()), RepoClientError);
  BOOST_CHECK_EQUAL(pulled, 0);

  RepoDataFetch fetch;
  fetch.packetNames = {"/example/spec240/owner/object/0"};
  fetch.maxWireBytes = 8;
  fetch.target = {"/repo/node", "/provider", "/SVC/repo"};
  BOOST_CHECK_THROW(client.fetchData(fetch, makeOptions()), RepoClientError);
  BOOST_CHECK_EQUAL(pulled, 0);
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
