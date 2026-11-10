#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-window.hpp"

#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingResultVerifier.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-window-admission.hpp"
#include "NDNSF-UAV-APP/shared/uav-tracking-worker-process.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using namespace person_fixture;
using namespace std::chrono_literals;

const std::string FIRST_INPUT = "sha256:" + std::string(64, 'c');
const std::string SECOND_INPUT = "sha256:" + std::string(64, 'd');

struct TemporaryDirectory
{
  std::filesystem::path path;

  TemporaryDirectory()
  {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "ndnsf-person-flow-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("cannot create native person flow directory");
    path = created;
  }

  ~TemporaryDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

ndn::Buffer
bufferOf(const std::string& text)
{
  return ndn::Buffer(text.begin(), text.end());
}

std::string
textOf(const ndn::Buffer& buffer)
{
  return std::string(buffer.begin(), buffer.end());
}

std::string
readBounded(const std::filesystem::path& path, size_t bound)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read native flow artifact");
  std::string text(bound + 1, '\0');
  stream.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(stream.gcount()));
  if (text.size() > bound || (!stream.eof() && stream.fail())) {
    throw std::runtime_error("native flow artifact exceeds bound or read failed");
  }
  return text;
}

void
writeText(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  stream.flush();
  if (!stream) throw std::runtime_error("cannot write native flow artifact");
}

Json
backendFor(const UavPersonWindowContext& context,
           const std::string& profileDigest = std::string(PERSON_PROFILE_DIGEST))
{
  // Literal synthetic feature/device declarations, never a model qualification claim.
  auto wire = backendFixture();
  wire["runId"] = context.runId;
  wire["missionId"] = context.missionId;
  wire["profileDigest"] = profileDigest;
  wire["windowId"] = context.windowId;
  wire["sequence"] = context.sequence;
  for (size_t i = 0; i < context.frames.size(); ++i) {
    const auto& frame = context.frames[i];
    wire["frames"][i]["sequence"] = frame.sequence;
    wire["frames"][i]["commonPtsUs"] = frame.commonPtsUs;
    auto& tracker = wire["backendState"]["cameras"][frame.cameraId]["tracker"];
    tracker["frameId"] = context.sequence;
    tracker["tracked"][0]["frameId"] = context.sequence;
  }
  return wire;
}

struct Candidate
{
  UavPersonWindowResult decision;
  std::string result;
};

struct FlowFixture
{
  TemporaryDirectory temporary;
  UavPersonTrackingProfile profile;
  UavTrackingWorkerTransaction transactions;
  UavPersonTrackingEngine engine;
  UavPersonWindowContext first = atWindow(1, 1000000);
  UavPersonWindowContext second = atWindow(2, 1500000);
  std::filesystem::path input = temporary.path / "literal-backend.json";
  std::filesystem::path output = temporary.path / "child-output.json";
  std::string firstResult;
  std::string firstState;
  std::atomic<unsigned> childLaunchAttempts{0};

  explicit FlowFixture(uint64_t retainedLimit = 120)
    : profile([retainedLimit] {
        auto wire = Json::parse(std::string(PERSON_PROFILE_GOLDEN));
        wire["limits"]["maxRetainedTransactionWindows"] = retainedLimit;
        return parseUavPersonTrackingProfile(wire.dump());
      }())
    , transactions(temporary.path / "transactions" / "mission", retainedLimit)
    , engine(profile)
  {
  }

  std::string
  copyFromChild(const Json& wire)
  {
    writeText(input, wire.dump());
    std::filesystem::remove(output);
    ++childLaunchAttempts;
    const auto status = runUavTrackingWorkerProcess(
      {"/bin/cp", "--", input.string(), output.string()}, 2s, 4096, [] { return true; });
    BOOST_REQUIRE(status.outcome == UavTrackingWorkerOutcome::SUCCESS);
    BOOST_REQUIRE_EQUAL(status.exitCode, 0);
    return readBounded(output, profile.limits.at("maxResultBytes"));
  }

  Candidate
  admit(const UavPersonWindowContext& context, const std::string& bytes,
        UavPersonTrackingEngine& owner)
  {
    const auto backend = parseUavPersonBackendWindow(bytes, context, profile);
    auto decision = owner.processWindow(context, backend);
    std::array<std::string, 3> observations;
    std::array<std::string, 3> digests;
    UavTrackingResultExpectation expected;
    expected.runId = context.runId;
    expected.missionId = context.missionId;
    expected.windowId = context.windowId;
    expected.sequence = context.sequence;
    expected.sourceEpoch = context.frames[0].sourceEpoch;
    expected.computeProvider = ndn::Name("/example/uav/compute");
    expected.personTracking = UavPersonTrackingResultExpectation{profile, context.computationDigest};
    for (size_t i = 0; i < context.frames.size(); ++i) {
      const auto& frame = context.frames[i];
      observations[i] = makeUavObservationDataName(droneIdentity(frame.cameraId),
        frame.cameraId, frame.sourceEpoch, frame.sequence, "OBSERVATION", 1).toUri();
      digests[i] = "sha256:" + std::string(64, static_cast<char>('c' + i));
      expected.sourceIds[i] = frame.sourceId;
      expected.inputDigests[i] = frame.inputDigest;
    }
    auto result = makeUavPersonTrackingResultJson(decision, backend, profile, observations, digests);
    const auto name = makeUavMultiViewResultName(expected.computeProvider, context.missionId,
      context.windowId, context.sequence, 1);
    validateUavTrackingResultContext(name.toUri(), expected.computeProvider.toUri(), result, expected);
    return {std::move(decision), std::move(result)};
  }

  Candidate
  commitFirst()
  {
    auto candidate = admit(first, copyFromChild(backendFor(first, profile.profileDigest)), engine);
    BOOST_REQUIRE(transactions.commit(first.windowId, FIRST_INPUT, bufferOf(candidate.result),
      bufferOf(candidate.decision.candidateStateJson), [] { return true; }));
    firstResult = candidate.result;
    firstState = candidate.decision.candidateStateJson;
    return candidate;
  }

  void
  checkPredecessorUnchanged() const
  {
    const auto cached = transactions.loadCommittedResult(first.windowId, FIRST_INPUT);
    BOOST_REQUIRE(cached.has_value());
    BOOST_CHECK_EQUAL(textOf(*cached), firstResult);
    const auto state = transactions.committedStatePath(first.windowId);
    BOOST_REQUIRE(state.has_value());
    BOOST_CHECK_EQUAL(readBounded(*state, profile.limits.at("maxSnapshotBytes")), firstState);
  }
};

void
checkIds(const Candidate& candidate)
{
  const std::array<uint64_t, 3> ids{1, 1, 2};
  for (size_t i = 0; i < ids.size(); ++i) {
    BOOST_REQUIRE_EQUAL(candidate.decision.tracks[i].size(), 1);
    BOOST_REQUIRE(candidate.decision.tracks[i][0].globalId.has_value());
    BOOST_CHECK_EQUAL(*candidate.decision.tracks[i][0].globalId, ids[i]);
  }
  BOOST_CHECK(candidate.result.find("embedding") == std::string::npos);
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonTrackingFlow)

BOOST_AUTO_TEST_CASE(ChildCandidateCommitsRestoresAndReplaysImmutableNativeDecisions)
{
  FlowFixture fixture;
  checkIds(fixture.commitFirst());
  fixture.checkPredecessorUnchanged();
  UavPersonTrackingEngine restored(fixture.profile);
  restored.restoreState(fixture.firstState, fixture.second);
  BOOST_CHECK_EQUAL(restored.backendStateJson(), fixture.engine.backendStateJson());
  const auto beforeReplay = restored.serializeState();
  const auto cached = fixture.transactions.loadCommittedResult(fixture.first.windowId, FIRST_INPUT);
  BOOST_REQUIRE(cached.has_value());
  BOOST_CHECK_EQUAL(textOf(*cached), fixture.firstResult);
  BOOST_CHECK_EQUAL(restored.serializeState(), beforeReplay);
  BOOST_CHECK_THROW(fixture.transactions.loadCommittedResult(fixture.first.windowId, SECOND_INPUT),
                    std::runtime_error);
  const auto next = fixture.admit(fixture.second,
    fixture.copyFromChild(backendFor(fixture.second)), restored);
  checkIds(next);
  BOOST_REQUIRE(fixture.transactions.commit(fixture.second.windowId, SECOND_INPUT,
    bufferOf(next.result), bufferOf(next.decision.candidateStateJson), [] { return true; }));
  fixture.checkPredecessorUnchanged();
}

BOOST_AUTO_TEST_CASE(InvalidChildBindingsAndFinalRevocationPreserveCommittedState)
{
  FlowFixture fixture;
  fixture.commitFirst();
  const auto previousEngine = fixture.engine.serializeState();
  auto invalid = backendFor(fixture.second);
  invalid["missionId"] = "wrong-mission";
  BOOST_CHECK_THROW(fixture.admit(fixture.second, fixture.copyFromChild(invalid), fixture.engine),
                    std::invalid_argument);
  invalid = backendFor(fixture.second);
  invalid["frames"][0]["detections"][0]["embedding"] = {1.0};
  BOOST_CHECK_THROW(fixture.admit(fixture.second, fixture.copyFromChild(invalid), fixture.engine),
                    std::invalid_argument);
  BOOST_CHECK_EQUAL(fixture.engine.serializeState(), previousEngine);
  const auto next = fixture.admit(fixture.second,
    fixture.copyFromChild(backendFor(fixture.second)), fixture.engine);
  BOOST_CHECK(!fixture.transactions.commit(fixture.second.windowId, SECOND_INPUT,
    bufferOf(next.result), bufferOf(next.decision.candidateStateJson), [] { return false; }));
  BOOST_CHECK(!fixture.transactions.loadCommittedResult(fixture.second.windowId, SECOND_INPUT));
  BOOST_CHECK(!fixture.transactions.committedStatePath(fixture.second.windowId));
  fixture.checkPredecessorUnchanged();
}

BOOST_AUTO_TEST_CASE(CrashAndActiveCancellationReapChildBeforeFreshRecovery)
{
  FlowFixture fixture;
  fixture.commitFirst();
  writeText(fixture.input, backendFor(fixture.second).dump());
  std::filesystem::remove(fixture.output);
  const auto crashed = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "cp -- \"$1\" \"$2\"; exit 7", "synthetic-fixture",
     fixture.input.string(), fixture.output.string()}, 2s, 4096, [] { return true; });
  BOOST_CHECK(crashed.outcome == UavTrackingWorkerOutcome::CHILD_FAILURE);
  BOOST_CHECK_EQUAL(crashed.exitCode, 7);
  BOOST_REQUIRE(std::filesystem::is_regular_file(fixture.output));
  const auto marker = fixture.temporary.path / "cancelled.pid";
  pid_t leader = -1;
  const auto cancelled = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "printf '%s\\n' $$ > \"$1\"; exec sleep 30", "synthetic-fixture",
     marker.string()}, 2s, 4096, [&] {
      std::ifstream stream(marker);
      return !(stream >> leader) || leader <= 0;
    });
  BOOST_CHECK(cancelled.outcome == UavTrackingWorkerOutcome::CANCELLED);
  BOOST_REQUIRE_GT(leader, 0);
  BOOST_CHECK(!std::filesystem::exists("/proc/" + std::to_string(leader)));
  BOOST_CHECK(!fixture.transactions.committedStatePath(fixture.second.windowId));
  fixture.checkPredecessorUnchanged();
  UavPersonTrackingEngine recovered(fixture.profile);
  recovered.restoreState(fixture.firstState, fixture.second);
  const auto next = fixture.admit(fixture.second,
    fixture.copyFromChild(backendFor(fixture.second)), recovered);
  checkIds(next);
  BOOST_REQUIRE(fixture.transactions.commit(fixture.second.windowId, SECOND_INPUT,
    bufferOf(next.result), bufferOf(next.decision.candidateStateJson), [] { return true; }));
  fixture.checkPredecessorUnchanged();
}

BOOST_AUTO_TEST_CASE(ProfileAndRunCapacityPreserveCommittedPairsAndCachedReplay)
{
  FlowFixture fixture(2);
  checkIds(fixture.commitFirst());
  fixture.transactions.requireCapacity(fixture.second.windowId);
  const auto second = fixture.admit(fixture.second,
    fixture.copyFromChild(backendFor(fixture.second, fixture.profile.profileDigest)), fixture.engine);
  BOOST_REQUIRE(fixture.transactions.commit(fixture.second.windowId, SECOND_INPUT,
    bufferOf(second.result), bufferOf(second.decision.candidateStateJson), [] { return true; }));
  const auto beforeEngine = fixture.engine.serializeState();
  const auto missionRoot = fixture.temporary.path / "transactions" / "mission";
  std::vector<std::pair<std::filesystem::path, std::string>> originalFiles;
  for (const auto& window : {fixture.first.windowId, fixture.second.windowId}) {
    for (const auto* filename : {"manifest.json", "result.json", "state.json"}) {
      const auto path = missionRoot / window / filename;
      originalFiles.emplace_back(path, readBounded(path, 8 * 1024 * 1024));
    }
  }
  const auto overLimit = atWindow(3, 2000000);
  const auto wire = backendFor(overLimit, fixture.profile.profileDigest);
  BOOST_CHECK_EXCEPTION(parseUavPersonBackendWindow(wire.dump(), overLimit, fixture.profile),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "PERSON_WINDOW_LIMIT";
    });
  const auto previousBackend = parseUavPersonBackendWindow(
    backendFor(fixture.second, fixture.profile.profileDigest).dump(), fixture.second, fixture.profile);
  BOOST_CHECK_EXCEPTION(fixture.engine.processWindow(overLimit, previousBackend),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "PERSON_WINDOW_LIMIT";
    });
  const auto full = [] (const auto& error) {
    return std::string(error.what()) == "tracking transaction retained window limit exceeded";
  };
  BOOST_CHECK_EXCEPTION(fixture.transactions.requireCapacity(overLimit.windowId), std::runtime_error, full);
  UavTrackingWorkerTransaction anotherMission(missionRoot.parent_path() / "another-mission", 2);
  BOOST_CHECK_EXCEPTION(anotherMission.requireCapacity("window-0"), std::runtime_error, full);
  BOOST_CHECK_EXCEPTION(anotherMission.commit("window-0", FIRST_INPUT,
    bufferOf(second.result), bufferOf(second.decision.candidateStateJson), [] { return true; }),
    std::runtime_error, full);
  BOOST_CHECK(!std::filesystem::exists(missionRoot.parent_path() / "another-mission"));
  const auto cached = fixture.transactions.loadCommittedResult(fixture.second.windowId, SECOND_INPUT);
  BOOST_REQUIRE(cached.has_value());
  BOOST_CHECK_EQUAL(textOf(*cached), second.result);
  BOOST_CHECK_EQUAL(fixture.engine.serializeState(), beforeEngine);
  for (const auto& [path, bytes] : originalFiles) {
    BOOST_CHECK_EQUAL(readBounded(path, 8 * 1024 * 1024), bytes);
  }
  fixture.checkPredecessorUnchanged();
  FlowFixture freshRun(2);
  freshRun.first.runId = "fresh-retention-run";
  freshRun.first.missionId = "fresh-retention-mission";
  checkIds(freshRun.commitFirst());
}

BOOST_AUTO_TEST_CASE(RetentionCountsInterruptedAndEmptySlotsWithoutFollowingLinks)
{
  BOOST_CHECK_THROW(UavTrackingWorkerTransaction("mission", 2), std::invalid_argument);
  TemporaryDirectory temporary;
  const auto run = temporary.path / "run";
  const auto current = run / "mission-a";
  std::filesystem::create_directories(current / "window-0");
  writeText(current / "window-0" / "result.json", "interrupted private candidate");
  std::filesystem::create_directories(run / "mission-b");
  UavTrackingWorkerTransaction first(current, 2);
  UavTrackingWorkerTransaction second(run / "mission-b", 2);
  BOOST_CHECK_NO_THROW(first.requireCapacity("window-0"));
  BOOST_CHECK_NO_THROW(second.requireCapacity("window-0"));
  const auto result = bufferOf("literal-result");
  const auto state = bufferOf("literal-state");
  BOOST_REQUIRE(first.commit("window-0", FIRST_INPUT, result, state, [] { return true; }));
  BOOST_CHECK(!second.commit("window-0", SECOND_INPUT, result, state, [] { return false; }));
  BOOST_CHECK(!std::filesystem::exists(run / "mission-b"));
  BOOST_REQUIRE(second.commit("window-0", SECOND_INPUT, result, state, [] { return true; }));
  const auto full = [] (const auto& error) {
    return std::string(error.what()) == "tracking transaction retained window limit exceeded";
  };
  UavTrackingWorkerTransaction third(run / "mission-c", 2);
  BOOST_CHECK_EXCEPTION(third.requireCapacity("window-0"), std::runtime_error, full);
  BOOST_REQUIRE(first.loadCommittedResult("window-0", FIRST_INPUT).has_value());
  BOOST_CHECK_EQUAL(textOf(*first.loadCommittedResult("window-0", FIRST_INPUT)), "literal-result");
  const auto outside = temporary.path / "outside";
  std::filesystem::create_directories(outside);
  const auto linkedRun = temporary.path / "linked-run";
  std::filesystem::create_directory_symlink(outside, linkedRun);
  UavTrackingWorkerTransaction rootLink(linkedRun / "mission", 2);
  BOOST_CHECK_THROW(rootLink.requireCapacity("window-0"), std::runtime_error);
  const auto linkedMissionRun = temporary.path / "mission-link-run";
  std::filesystem::create_directories(linkedMissionRun);
  std::filesystem::create_directory_symlink(outside, linkedMissionRun / "mission");
  UavTrackingWorkerTransaction missionLink(linkedMissionRun / "mission", 2);
  BOOST_CHECK_THROW(missionLink.requireCapacity("window-0"), std::runtime_error);
  const auto linkedWindowMission = temporary.path / "window-link-run" / "mission";
  std::filesystem::create_directories(linkedWindowMission);
  std::filesystem::create_directory_symlink(outside, linkedWindowMission / "window-0");
  UavTrackingWorkerTransaction windowLink(linkedWindowMission, 2);
  BOOST_CHECK_THROW(windowLink.requireCapacity("window-0"), std::runtime_error);
  BOOST_CHECK(std::filesystem::is_empty(outside));
}

BOOST_AUTO_TEST_CASE(BusyPersonWindowAdmissionPreservesActiveCandidateAndReleasesForRecovery)
{
  FlowFixture fixture;
  UavPersonWindowAdmission admission;
  fixture.commitFirst();
  std::vector<std::pair<std::filesystem::path, std::string>> committedFiles;
  const auto root = fixture.temporary.path / "transactions" / "mission";
  for (const auto* name : {"manifest.json", "result.json", "state.json"}) {
    const auto path = root / fixture.first.windowId / name;
    committedFiles.emplace_back(path, readBounded(path, 8 * 1024 * 1024));
  }
  auto lease = admission.tryAcquire();
  BOOST_REQUIRE(lease.owns_lock());
  const auto candidate = fixture.admit(fixture.second,
    fixture.copyFromChild(backendFor(fixture.second, fixture.profile.profileDigest)), fixture.engine);
  const auto beforeEngine = fixture.engine.serializeState();
  const auto beforeInput = readBounded(fixture.input, 8 * 1024 * 1024);
  const auto beforeOutput = readBounded(fixture.output, 8 * 1024 * 1024);
  const auto beforeAttempts = fixture.childLaunchAttempts.load();
  BOOST_REQUIRE_EQUAL(beforeAttempts, 2);
  const auto active = root / fixture.second.windowId;
  const auto third = atWindow(3, 2000000);
  bool checkedActiveCandidate = false;
  BOOST_REQUIRE(fixture.transactions.commit(fixture.second.windowId, SECOND_INPUT,
    bufferOf(candidate.result), bufferOf(candidate.decision.candidateStateJson), [&] {
      std::vector<std::pair<std::filesystem::path, std::string>> activeFiles;
      for (const auto* name : {"result.json", "state.json", "manifest.json.tmp"}) {
        const auto path = active / name;
        activeFiles.emplace_back(path, readBounded(path, 8 * 1024 * 1024));
      }
      const auto caller = std::this_thread::get_id();
      auto contender = std::async(std::launch::async, [&] {
        auto attempted = admission.tryAcquire();
        const bool admitted = attempted.owns_lock();
        if (admitted) {
          // A broken admission owner must expose a real, bounded child attempt.
          fixture.copyFromChild(backendFor(third, fixture.profile.profileDigest));
        }
        return std::make_pair(admitted, std::this_thread::get_id() != caller);
      });
      const bool ready = contender.wait_for(2s) == std::future_status::ready;
      // get() drains the owned contender before assertions or owner destruction.
      const auto observation = contender.get();
      BOOST_REQUIRE(ready);
      BOOST_CHECK(!observation.first);
      BOOST_CHECK(observation.second);
      BOOST_CHECK_EQUAL(fixture.childLaunchAttempts.load(), beforeAttempts);
      BOOST_CHECK_EQUAL(fixture.engine.serializeState(), beforeEngine);
      BOOST_CHECK_EQUAL(readBounded(fixture.input, 8 * 1024 * 1024), beforeInput);
      BOOST_CHECK_EQUAL(readBounded(fixture.output, 8 * 1024 * 1024), beforeOutput);
      for (const auto& [path, bytes] : activeFiles) {
        BOOST_CHECK_EQUAL(readBounded(path, 8 * 1024 * 1024), bytes);
      }
      BOOST_CHECK(!std::filesystem::exists(active / "manifest.json"));
      BOOST_CHECK(!std::filesystem::exists(root / third.windowId));
      fixture.checkPredecessorUnchanged();
      for (const auto& [path, bytes] : committedFiles) {
        BOOST_CHECK_EQUAL(readBounded(path, 8 * 1024 * 1024), bytes);
      }
      checkedActiveCandidate = true;
      return true;
    }));
  BOOST_CHECK(checkedActiveCandidate);
  fixture.checkPredecessorUnchanged();
  const auto committed = fixture.transactions.loadCommittedResult(fixture.second.windowId,
                                                                 SECOND_INPUT);
  BOOST_REQUIRE(committed.has_value());
  BOOST_CHECK_EQUAL(textOf(*committed), candidate.result);
  for (const auto* name : {"manifest.json", "result.json", "state.json"}) {
    const auto path = active / name;
    committedFiles.emplace_back(path, readBounded(path, 8 * 1024 * 1024));
  }
  lease.unlock();
  {
    auto recovered = admission.tryAcquire();
    BOOST_REQUIRE(recovered.owns_lock());
    const auto cancelled = fixture.admit(third,
      fixture.copyFromChild(backendFor(third, fixture.profile.profileDigest)), fixture.engine);
    BOOST_CHECK(!fixture.transactions.commit(third.windowId, FIRST_INPUT,
      bufferOf(cancelled.result), bufferOf(cancelled.decision.candidateStateJson),
      [] { return false; }));
    BOOST_CHECK(!std::filesystem::exists(root / third.windowId));
  }
  bool unwound = false;
  try {
    auto recovered = admission.tryAcquire();
    BOOST_REQUIRE(recovered.owns_lock());
    throw std::runtime_error("native window failure");
  }
  catch (const std::runtime_error&) {
    unwound = true;
  }
  auto recovered = admission.tryAcquire();
  BOOST_REQUIRE(recovered.owns_lock());
  BOOST_CHECK(unwound);
  fixture.checkPredecessorUnchanged();
  const auto unchanged = fixture.transactions.loadCommittedResult(fixture.second.windowId,
                                                                  SECOND_INPUT);
  BOOST_REQUIRE(unchanged.has_value());
  BOOST_CHECK_EQUAL(textOf(*unchanged), candidate.result);
  BOOST_CHECK_EQUAL(fixture.childLaunchAttempts.load(), 3);
  for (const auto& [path, bytes] : committedFiles) {
    BOOST_CHECK_EQUAL(readBounded(path, 8 * 1024 * 1024), bytes);
  }
  BOOST_CHECK(!std::filesystem::exists(root / third.windowId));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
