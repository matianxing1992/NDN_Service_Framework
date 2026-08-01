#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp"
#include "tests/boost-test.hpp"

#include <cstdio>
#include <map>
#include <unistd.h>

namespace ndnsf_distributed_repo::test {

namespace {

class TestPayloadStore final : public PayloadStore
{
public:
  void begin(const ArtifactReference&, uint64_t) override {}
  void writeRange(const ArtifactReference&, uint64_t, ArtifactByteRange,
                  const std::vector<uint8_t>&) override {}
  std::vector<uint8_t> readRange(const ArtifactReference&, uint64_t,
                                 ArtifactByteRange) const override
  {
    return {};
  }
  void markVerified(const ArtifactReference&, uint64_t, ArtifactByteRange) override {}
  std::vector<ArtifactByteRange>
  verifiedRanges(const ArtifactReference&, uint64_t) const override
  {
    return {};
  }
  void flush(const ArtifactReference&, uint64_t) override {}
  void finalize(const ArtifactReference&, uint64_t) override {}
  bool isCommitted(const ArtifactReference&, uint64_t) const override
  {
    return false;
  }
  void abort(const ArtifactReference&, uint64_t) override {}
};

class TestMetadataStore final : public MetadataStore
{
public:
  uint64_t schemaGeneration() const override
  {
    return 9;
  }

  void appendLifecycleEvent(const ArtifactLifecycleEvent& event) override
  {
    events.push_back(event);
    if (event.accepted) {
      states[event.operationId] = event.toState;
    }
  }

  std::vector<ArtifactLifecycleEvent>
  lifecycleEvents(const std::string& operationId) const override
  {
    std::vector<ArtifactLifecycleEvent> selected;
    for (const auto& event : events) {
      if (event.operationId == operationId) {
        selected.push_back(event);
      }
    }
    return selected;
  }

  ArtifactLifecycleState
  currentLifecycleState(const std::string& operationId) const override
  {
    const auto found = states.find(operationId);
    return found == states.end() ? ArtifactLifecycleState::Absent : found->second;
  }

  std::vector<ArtifactLifecycleEvent> events;
  std::map<std::string, ArtifactLifecycleState> states;
};

std::string
temporaryBackendPath(const std::string& suffix)
{
  return "/tmp/ndnsf-repo-store-backend-" +
         std::to_string(static_cast<unsigned long>(::getpid())) + "-" + suffix;
}

void
removeBackendFiles(const std::string& path)
{
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
  std::remove((path + ".authority.lock").c_str());
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoStoreBackend)

BOOST_AUTO_TEST_CASE(OwnershipIsExclusiveAcrossNativeBackendEntryPoints)
{
  const auto path = temporaryBackendPath("exclusive.sqlite3");
  removeBackendFiles(path);
  {
    BackendOwnershipLease pythonOwner(path, "python-test-owner");
    BOOST_CHECK(pythonOwner.ownsBackend());
    BOOST_CHECK_EXCEPTION(
      makeSqliteRepoStore(path),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find("repo-persistence-owned:") == 0;
      });
  }
  {
    auto nativeStore = makeSqliteRepoStore(path);
    BOOST_REQUIRE(nativeStore != nullptr);
  }
  removeBackendFiles(path);
}

BOOST_AUTO_TEST_CASE(FacadeEnforcesAndRecordsLifecycleTransitions)
{
  const auto path = temporaryBackendPath("facade.sqlite3");
  removeBackendFiles(path);
  auto payload = std::make_shared<TestPayloadStore>();
  auto metadata = std::make_shared<TestMetadataStore>();
  {
    RepositoryStoreFacade facade(path, "native-test-owner", payload, metadata);
    ArtifactLifecycleEvent reserved{
      "event-1", "operation-1", std::string(64, 'a'), 1,
      ArtifactLifecycleState::Absent, ArtifactLifecycleState::Reserved,
      1000, true, "reserved",
    };
    const auto accepted = facade.transition(reserved);
    BOOST_CHECK(accepted.accepted);
    const auto replay = facade.transition(reserved);
    BOOST_CHECK_EQUAL(replay.eventId, reserved.eventId);
    BOOST_REQUIRE_EQUAL(metadata->events.size(), 1);
    BOOST_CHECK_EQUAL(
      toString(metadata->currentLifecycleState("operation-1")), "RESERVED");

    ArtifactLifecycleEvent changedIdentity{
      "event-2", "operation-1", std::string(64, 'b'), 2,
      ArtifactLifecycleState::Reserved, ArtifactLifecycleState::Receiving,
      1050, true, "changed identity",
    };
    BOOST_CHECK_EXCEPTION(
      facade.transition(changedIdentity),
      std::invalid_argument,
      [] (const std::invalid_argument& error) {
        return std::string(error.what()).find(
          "repo-lifecycle-identity-conflict:") == 0;
      });

    ArtifactLifecycleEvent illegal{
      "event-3", "operation-1", std::string(64, 'a'), 1,
      ArtifactLifecycleState::Reserved, ArtifactLifecycleState::Committed,
      1100, true, "skip verification",
    };
    BOOST_CHECK_EXCEPTION(
      facade.transition(illegal),
      std::invalid_argument,
      [] (const std::invalid_argument& error) {
        return std::string(error.what()).find(
          "repo-lifecycle-illegal-transition:") == 0;
      });
    const auto events = metadata->lifecycleEvents("operation-1");
    BOOST_REQUIRE_EQUAL(events.size(), 3);
    BOOST_CHECK(events[0].accepted);
    BOOST_CHECK(!events[1].accepted);
    BOOST_CHECK(!events[2].accepted);
    BOOST_CHECK_EQUAL(
      toString(metadata->currentLifecycleState("operation-1")), "RESERVED");
  }
  removeBackendFiles(path);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
