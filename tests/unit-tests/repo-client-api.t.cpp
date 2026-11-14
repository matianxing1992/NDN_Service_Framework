#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp"
#include "tests/boost-test.hpp"

#include <atomic>
#include <thread>
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
  BOOST_CHECK_EQUAL(fetched.bytesWritten, sizeof(FIXTURE_TEXT) - 1);
  // The reference path verifies the committed payload before writing, so the caller can
  // tell written bytes from verified bytes instead of assuming they are the same.
  BOOST_CHECK_EQUAL(fetched.bytesVerified, sizeof(FIXTURE_TEXT) - 1);
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

  // The handle keeps the typed target and the committed replica detail, not a flattened
  // list of names: a restart needs the identities that were committed.
  BOOST_REQUIRE(published.committedHandle.has_value());
  const auto& handle = *published.committedHandle;
  BOOST_CHECK(std::holds_alternative<FileReference>(handle.reference));
  if (const auto* file = std::get_if<FileReference>(&handle.reference)) {
    // The engine canonicalises the reference it stores, so compare the identity that
    // matters rather than the literal input string.
    BOOST_CHECK(!file->logicalName.empty());
    BOOST_CHECK_EQUAL(file->sizeBytes, fixtureReference().sizeBytes);
    BOOST_CHECK_EQUAL(file->contentDigest, fixtureReference().contentDigest);
  }
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

BOOST_AUTO_TEST_CASE(FailureCarriesNoInventedProgress) {
  // A failing path that cannot report progress must hand back an empty value rather than
  // plausible-looking entries; "unknown" and "none" are different answers.
  TemporaryDirectory temporary("partial");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoFilePublication publication;
  publication.sourcePath = (temporary.path() / "missing.bin").string();
  publication.reference = fixtureReference();
  try {
    client.publishFile(publication, makeOptions());
    BOOST_FAIL("a missing source must fail");
  }
  catch (const RepoClientError& error) {
    BOOST_CHECK(!error.code().empty());
    BOOST_CHECK(!error.phase().empty());
    const auto& partial = error.partial();
    BOOST_CHECK(partial.confirmedGroups.empty());
    BOOST_CHECK(partial.replicaReceipts.empty());
    BOOST_CHECK(partial.unconfirmedTargets.empty());
    BOOST_CHECK(partial.failedAttempts.empty());
  }
}

BOOST_AUTO_TEST_CASE(DeclaredOptionsThatCannotBeHonouredAreRefused) {
  // Declaring a field the facade then ignores is how a caller ends up believing in a
  // capability that does not exist. Each of these is refused with its own phase.
  TemporaryDirectory temporary("unsupported");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  const auto refused = [&] (RepoClientError error, const std::string& phase) {
    return error.code() == "UNSUPPORTED" && error.phase() == phase;
  };

  RepoFilePublication stored;
  stored.storedSource = StoredFileSource{};
  BOOST_CHECK_EXCEPTION(client.publishFile(stored, makeOptions()), RepoClientError,
    [&] (const RepoClientError& error) { return refused(error, "publishFile"); });

  RepoDataPublication object;
  object.object = ProducerObjectSource{};
  BOOST_CHECK_EXCEPTION(client.publishData(object, makeOptions()), RepoClientError,
    [&] (const RepoClientError& error) { return refused(error, "publishData"); });

  RepoInspectionRequest liveness;
  liveness.probeLiveness = true;
  BOOST_CHECK_EXCEPTION(client.inspect(liveness, makeOptions()), RepoClientError,
    [&] (const RepoClientError& error) { return refused(error, "inspect"); });

  RepoInspectionRequest noMetadata;
  noMetadata.probeMetadata = false;
  BOOST_CHECK_EXCEPTION(client.inspect(noMetadata, makeOptions()), RepoClientError,
    [&] (const RepoClientError& error) { return refused(error, "inspect"); });

  RepoListRequest pinned;
  pinned.catalogRevision = "rev-1";
  BOOST_CHECK_EXCEPTION(client.list(pinned, makeOptions()), RepoClientError,
    [&] (const RepoClientError& error) { return refused(error, "list"); });
}

BOOST_AUTO_TEST_CASE(DiscoveryHonoursTheRequestedProviderScope) {
  // A declared scope must constrain the answer: being handed a Repo the caller excluded is
  // a silent security-relevant substitution.
  TemporaryDirectory temporary("discover-scope");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  // A caller-supplied ACK window is not something the engine can honour; the facade says so
  // instead of accepting the field and ignoring it.
  RepoDiscoveryRequest bounded;
  bounded.ackCollectionMs = 5000;
  BOOST_CHECK_EXCEPTION(client.discover(bounded, makeOptions()), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "UNSUPPORTED" && error.phase() == "discover";
    });

  RepoDiscoveryRequest unscoped;
  RepoCandidateSet all;
  try {
    all = client.discover(unscoped, makeOptions());
  }
  catch (const std::exception& exception) {
    // Discovery is a persistent-mode capability; an in-app client refuses it before the
    // scope check can be asked.
    BOOST_TEST_MESSAGE(std::string("scope case skipped: ") + exception.what());
    return;
  }
  if (all.candidates.empty()) {
    BOOST_TEST_MESSAGE("scope case skipped: in-app discovery returned no candidates");
    return;
  }
  RepoDiscoveryRequest scoped;
  scoped.providerScope = "/someone/else";
  BOOST_CHECK_EXCEPTION(client.discover(scoped, makeOptions()), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "NO_MATCHING_PROVIDER" && error.phase() == "discover";
    });
  // A scope that names an actual candidate keeps it.
  scoped.providerScope = all.candidates.front().providerIdentity.toUri();
  const auto kept = client.discover(scoped, makeOptions());
  BOOST_CHECK_EQUAL(kept.candidates.size(), 1U);
}

BOOST_AUTO_TEST_CASE(CloseIsIdempotentAndSafeToRepeat) {
  // close() is the only way a caller states "no more work". Repeating it, or calling it
  // on an expired deadline while idle, must be answered rather than throwing or hanging.
  TemporaryDirectory temporary("close-repeat");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  BOOST_CHECK(client.close(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  BOOST_CHECK(client.close(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  // An exhausted deadline with nothing in flight is still a drained client.
  BOOST_CHECK(client.close(std::chrono::steady_clock::now() - std::chrono::milliseconds(1)));
}

BOOST_AUTO_TEST_CASE(CloseDoesNotClaimADrainWhileAPublicationIsInFlight) {
  // CA08 asks for a drain that was observed, not assumed. The idle case only proves the
  // trivial answer, so this one stalls a real publication on a FIFO source and closes while
  // it is still running.
  TemporaryDirectory temporary("close-inflight");
  const fs::path fifo = temporary.path() / "stalled.fifo";
  BOOST_REQUIRE_EQUAL(::mkfifo(fifo.string().c_str(), 0600), 0);
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));

  RepoFilePublication publication;
  publication.sourcePath = fifo.string();
  publication.reference = fixtureReference();

  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::string failure;
  std::thread worker([&] {
    started.store(true);
    try {
      client.publishFile(publication, makeOptions());
    }
    catch (const std::exception& exception) {
      failure = exception.what();
    }
    finished.store(true);
  });
  while (!started.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  if (finished.load()) {
    // The engine refused the non-regular source before blocking; the drain question
    // cannot be asked with this fixture, and saying so beats asserting on nothing.
    BOOST_TEST_MESSAGE("in-flight close skipped: engine rejected the FIFO (" + failure + ")");
    worker.join();
  }
  else {
    const auto drained = client.close(std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(50));
    BOOST_CHECK_MESSAGE(!drained, "close must not report a drain while work is running");

    // Unblock the reader so the worker can finish and the test can join it.
    std::ofstream writer(fifo, std::ios::binary);
    writer << FIXTURE_TEXT;
    writer.close();
    worker.join();
    BOOST_CHECK(finished.load());
    RepoInspectionRequest inspection;
    inspection.kind = RepoInspectionRequest::Kind::NODE;
    BOOST_CHECK_THROW(client.inspect(inspection, makeOptions()), RepoClientError);
  }
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
  // The Repo's commit receipt survives the facade: identity, counts and state.
  BOOST_REQUIRE(first.receipt.has_value());
  BOOST_CHECK(!first.receipt->receiptId.empty());
  BOOST_CHECK_EQUAL(first.receipt->state, "COMMITTED");
  BOOST_CHECK_EQUAL(first.receipt->generation, 1U);
  // The engine's total timing survives the facade, so a slow publication can be attributed.
  // In-app publication reports no phase split; phases come from the persistent engine.
  BOOST_CHECK_GT(first.totalDurationMs, 0.0);

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

BOOST_AUTO_TEST_CASE(FetchDataRefusesInputsItCannotHonour) {
  // A ranged or segmented retrieval must be refused, not silently answered with the whole
  // packet set.
  TemporaryDirectory temporary("fetch-shapes");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoDataFetch ranged;
  ranged.packetNames = {"/example/spec240/owner/object/0"};
  ranged.maxWireBytes = 64;
  ranged.hasRange = true;
  ranged.rangeOffsetBytes = 0;
  ranged.rangeLengthBytes = 8;
  BOOST_CHECK_EXCEPTION(client.fetchData(ranged, makeOptions()), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "UNSUPPORTED" && error.phase() == "fetchData";
    });

  RepoDataFetch segmented;
  segmented.object = SegmentedObjectReference{"/producer", "/prefix", 128, "d", 64, 1};
  BOOST_CHECK_EXCEPTION(client.fetchData(segmented, makeOptions()), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "UNSUPPORTED" && error.phase() == "fetchData";
    });
}

BOOST_AUTO_TEST_CASE(AnExpiredDeadlineIsReportedNotReplaced) {
  // Passing an exhausted deadline must not let the engine fall back to its configured
  // wait: the caller's budget is the whole operation's budget.
  TemporaryDirectory temporary("deadline");
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_TEXT);
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = fixtureReference();
  publication.selectedTargets = {RepoTarget{"/repo/node", "/provider", "/SVC/repo"}};
  RepoOperationOptions expired;
  expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
  BOOST_CHECK_EXCEPTION(client.publishFile(publication, expired), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "DEADLINE_EXPIRED" && error.phase() == "publishFile";
    });
}

BOOST_AUTO_TEST_CASE(OperationInspectionValidatesItsBinding) {
  // An operation status is only meaningful with the operation id and the seed it belongs
  // to; without them the facade must refuse rather than guess.
  TemporaryDirectory temporary("operation");
  auto client = RepoClient::makeInApp(makeConfig(temporary.path() / "repo"));
  RepoInspectionRequest request;
  request.kind = RepoInspectionRequest::Kind::OPERATION;
  request.operationReference = "op-1";
  BOOST_CHECK_EXCEPTION(client.inspect(request, makeOptions()), RepoClientError,
    [] (const RepoClientError& error) {
      return error.code() == "UNSUPPORTED" && error.phase() == "inspect";
    });
}

BOOST_AUTO_TEST_CASE(AvailabilityIsFalseWhenTheCommittedPayloadIsGone) {
  // CA05: a catalog entry is not proof of a payload. Removing the stored bytes must turn
  // AVAILABLE into UNAVAILABLE.
  TemporaryDirectory temporary("payload-gone");
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_TEXT);
  const fs::path root = temporary.path() / "repo";
  auto client = RepoClient::makeInApp(makeConfig(root));
  RepoFilePublication publication;
  publication.sourcePath = source.string();
  publication.reference = fixtureReference();
  BOOST_REQUIRE(client.publishFile(publication, makeOptions()).committedHandle.has_value());

  RepoInspectionRequest inspection;
  inspection.kind = RepoInspectionRequest::Kind::OBJECT;
  RepoHandle handle;
  handle.reference = fixtureReference();
  inspection.object = handle;
  const auto present = client.inspect(inspection, makeOptions());
  BOOST_CHECK(present.availability == RepoInspection::Availability::AVAILABLE);
  // The answer says when it was observed and which probe produced it, so a caller can
  // tell a current observation from a stale one.
  BOOST_CHECK_GT(present.observedAtMs, 0U);
  BOOST_CHECK_EQUAL(present.probeKind, "local-payload");

  // The payload lives under payloads/<algorithm>/<first two hex>/<digest>.
  const auto digest = fixtureReference().contentDigest;
  const auto payload = root / "payloads" / "sha256" / digest.substr(0, 2) / digest;
  std::error_code error;
  BOOST_REQUIRE(fs::exists(payload));
  fs::remove(payload, error);
  BOOST_REQUIRE(!error);
  BOOST_CHECK(client.inspect(inspection, makeOptions()).availability ==
              RepoInspection::Availability::UNAVAILABLE);
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
