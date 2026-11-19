#include "tests/boost-test.hpp"

#include "ndn-service-framework/AbeHistoricalKeyRequest.hpp"
#include "ndn-service-framework/ControllerGenerationStore.hpp"
#include "ndn-service-framework/AbeHistoricalKey.hpp"
#include "ndn-service-framework/AbePublicParametersArchive.hpp"
#include "ndn-service-framework/FileKeyRing.hpp"
#include "ndn-service-framework/PolicyRefreshCoordinator.hpp"
#include "ndn-service-framework/RevocationState.hpp"
#include "ndn-service-framework/utils.hpp"

#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <stdexcept>

#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <nac-abe/algo/abe-support.hpp>

namespace ndn_service_framework::test {
namespace {

struct DefaultGenerationFiles
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec217-default-generation-" + std::to_string(ndn::random::generateWord64()));
  std::filesystem::path legacy = root / "legacy.state";

  DefaultGenerationFiles()
  {
    if (!std::filesystem::create_directory(root))
      throw std::runtime_error("default generation fixture already exists");
    std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  }

  ~DefaultGenerationFiles()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};

std::string
readGenerationFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

PolicyStatusData
makeRefreshStatus(const ControllerVersion& version,
                  uint64_t validFrom = 100,
                  uint64_t validUntil = 5000)
{
  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(version);
  status.setValidity(validFrom, validUntil);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("signature"), 9));
  return status;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ControllerRevocationState)

BOOST_AUTO_TEST_CASE(AbeMaterialPersistsAndMarksRotationBeforeRestart)
{
  DefaultGenerationFiles files;
  const auto owner = ndn::Name("/controller/a").toUri(ndn::name::UriFormat::CANONICAL);
  ControllerGenerationStore::AbeMaterial material;
  material.owner = owner;
  material.signingKeyDigest = "sha256:" + std::string(64, 'a');
  material.policyDigest = "sha256:" + std::string(64, 'b');
  material.publicParameters = "public";
  material.masterSecret = "private";
  {
    ControllerGenerationStore store(files.legacy);
    BOOST_REQUIRE(store.acquireWriter(owner));
    store.startGeneration(1000);
    store.storeAbeMaterial(material);
    store.advanceEpoch({}, true);
  }
  {
    ControllerGenerationStore store(files.legacy);
    BOOST_REQUIRE(store.acquireWriter(owner));
    auto restored = store.loadAbeMaterial();
    BOOST_REQUIRE(restored);
    BOOST_CHECK(restored->rotationRequired);
    BOOST_CHECK(restored->masterSecret == material.masterSecret);
    restored->rotationRequired = false;
    store.storeAbeMaterial(*restored);
    BOOST_CHECK(!store.loadAbeMaterial()->rotationRequired);
  }
  std::filesystem::permissions(files.legacy, std::filesystem::perms::group_read,
                               std::filesystem::perm_options::add);
  ControllerGenerationStore unsafe(files.legacy);
  BOOST_REQUIRE(unsafe.acquireWriter(owner));
  BOOST_CHECK_THROW(unsafe.loadAbeMaterial(), std::runtime_error);
  std::filesystem::permissions(files.legacy, std::filesystem::perms::group_read,
                               std::filesystem::perm_options::remove);
  {
    std::fstream state(files.legacy, std::ios::in | std::ios::out | std::ios::binary);
    state.seekg(-1, std::ios::end);
    char last = 0;
    state.get(last);
    state.seekp(-1, std::ios::end);
    state.put(last == '0' ? '1' : '0');
  }
  BOOST_CHECK_THROW(unsafe.loadAbeMaterial(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(LegacyArchiveEntriesMigrateToTheBoundLayout)
{
  // CS5 recorded archive entries before the layout gained the published name and the wire digest.
  // A Controller upgrading from it derives both from what the file holds instead of refusing it.
  DefaultGenerationFiles files;
  const auto statePath = files.root / "migrated.state";
  const auto lockPath = std::filesystem::path(statePath.string() + ".lock");
  auto putU64 = [] (std::ostream& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
      out.put(static_cast<char>((value >> shift) & 0xff));
  };
  auto putString = [&putU64] (std::ostream& out, const std::string& value) {
    putU64(out, value.size());
    out.write(value.data(), value.size());
  };
  {
    std::ofstream state(statePath, std::ios::binary | std::ios::trunc);
    const std::array<char, 8> magic{{'N','D','N','S','F','C','S','5'}};
    state.write(magic.data(), magic.size());
    putU64(state, 1002);
    putU64(state, 3);
    putU64(state, 0);                 // no revocations
    putU64(state, 1);                 // one archived generation, pre-CS6 layout
    putU64(state, 1002);
    putU64(state, 3);
    putString(state, std::string("legacy-params", 13));
    putString(state, std::string("legacy-secret", 13));
    putString(state, "sha256:" + std::string(64, 'a'));
    putString(state, "rotation");
  }
  {
    std::ofstream lock(lockPath);
    lock << "lock";
  }
  std::filesystem::permissions(statePath, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
  ControllerGenerationStore store(statePath);
  // The store needs its owner identity to derive the name the generation published under.
  const auto history = store.loadAbeHistory();
  BOOST_REQUIRE_EQUAL(history.size(), 1u);
  BOOST_CHECK_EQUAL(history.front().publicParameters, std::string("legacy-params", 13));
  BOOST_CHECK_EQUAL(history.front().masterSecret, std::string("legacy-secret", 13));
  BOOST_CHECK_EQUAL(history.front().publicParametersWireDigest.size(), 71u);
}

BOOST_AUTO_TEST_CASE(LegacyVersionOnlyStateRemainsReadable)
{
  DefaultGenerationFiles files;
  {
    std::ofstream state(files.legacy, std::ios::binary);
    const std::array<char, 16> version{{0, 0, 0, 0, 0, 0, 3, static_cast<char>(0xe8),
                                        0, 0, 0, 0, 0, 0, 0, 1}};
    state.write(version.data(), version.size());
  }
  std::filesystem::permissions(files.legacy, std::filesystem::perms::owner_read |
                                            std::filesystem::perms::owner_write);
  ControllerGenerationStore store(files.legacy);
  const auto version = store.load();
  BOOST_REQUIRE(version);
  BOOST_CHECK_EQUAL(version->controllerGenerationTimestamp, 1000);
  BOOST_CHECK_EQUAL(version->controllerEpoch, 1);
}

BOOST_AUTO_TEST_CASE(DefaultGenerationSeparatesIdentitiesAndPreservesRevocations)
{
  DefaultGenerationFiles files;
  const auto first = ControllerGenerationStore::openDefault("/controller/a", files.legacy);
  const auto second = ControllerGenerationStore::openDefault("/controller/b", files.legacy);
  BOOST_CHECK(first->ownsWriter() && second->ownsWriter());
  BOOST_CHECK(first->statePath() != second->statePath());
  RevocationTarget revoked;
  revoked.kind = RevocationKind::IDENTITY;
  revoked.targetIdentity = ndn::Name("/revoked/user");
  const auto version = first->startGeneration(1000, {revoked});
  BOOST_CHECK(std::filesystem::status(first->statePath()).permissions() ==
    (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
  second->startGeneration(1000);
  const auto path = first->statePath();
  first->releaseWriter();
  const auto restarted = ControllerGenerationStore::openDefault(
    "/8=controller/8=a", files.root / "another-policy.state");
  BOOST_CHECK_EQUAL(restarted->statePath(), path);
  const auto revocations = restarted->loadRevocations();
  BOOST_REQUIRE_EQUAL(revocations.size(), 1U);
  BOOST_CHECK(revocations[0].wireEncode() == revoked.wireEncode());
  BOOST_CHECK(restarted->startGeneration(900, revocations).controllerGenerationTimestamp >
              version.controllerGenerationTimestamp);
}

BOOST_AUTO_TEST_CASE(DefaultGenerationPreservesVerifiedLegacyInPlace)
{
  DefaultGenerationFiles files;
  const auto owner = ndn::Name("/controller/a").toUri(ndn::name::UriFormat::CANONICAL);
  ControllerVersion previous;
  RevocationTarget revoked;
  revoked.kind = RevocationKind::IDENTITY;
  revoked.targetIdentity = ndn::Name("/revoked/legacy");
  {
    ControllerGenerationStore legacy(files.legacy);
    BOOST_REQUIRE(legacy.acquireWriter(owner));
    previous = legacy.startGeneration(1000, {revoked});
  }
  std::filesystem::permissions(files.legacy,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
    std::filesystem::perms::group_read | std::filesystem::perms::others_read);
  const auto current = ControllerGenerationStore::openDefault("/controller/a", files.legacy);
  BOOST_CHECK_EQUAL(current->statePath(), files.legacy);
  const auto revocations = current->loadRevocations();
  BOOST_REQUIRE_EQUAL(revocations.size(), 1U);
  BOOST_CHECK(revocations[0].wireEncode() == revoked.wireEncode());
  BOOST_CHECK(current->startGeneration(900, revocations).controllerGenerationTimestamp >
              previous.controllerGenerationTimestamp);
}

BOOST_AUTO_TEST_CASE(DefaultGenerationRejectsAmbiguousLegacyWithoutChangingItsBytes)
{
  for (const auto* mode : {"foreign", "corrupt-state", "corrupt-lock", "missing-state",
                           "missing-lock", "active", "ambiguous", "symlink", "writable"}) {
    DefaultGenerationFiles files;
    std::unique_ptr<ControllerGenerationStore> current;
    if (std::string(mode) == "ambiguous") {
      current = ControllerGenerationStore::openDefault("/controller/a", files.legacy);
      current->startGeneration(1000);
      current.reset();
    }
    auto legacy = std::make_unique<ControllerGenerationStore>(files.legacy);
    const auto owner = ndn::Name(std::string(mode) == "foreign" ?
      "/controller/foreign" : "/controller/a").toUri(ndn::name::UriFormat::CANONICAL);
    BOOST_REQUIRE(legacy->acquireWriter(owner));
    legacy->startGeneration(1000);
    if (std::string(mode) != "active")
      legacy.reset();
    const auto lock = std::filesystem::path(files.legacy.string() + ".lock");
    if (std::string(mode) == "corrupt-state")
      std::ofstream(files.legacy, std::ios::trunc) << "bad";
    else if (std::string(mode) == "corrupt-lock")
      std::ofstream(lock, std::ios::trunc) << "bad";
    else if (std::string(mode) == "missing-state")
      std::filesystem::remove(files.legacy);
    else if (std::string(mode) == "missing-lock")
      std::filesystem::remove(lock);
    else if (std::string(mode) == "writable")
      std::filesystem::permissions(files.legacy, std::filesystem::perms::group_write,
                                   std::filesystem::perm_options::add);
    else if (std::string(mode) == "symlink") {
      std::filesystem::rename(files.legacy, files.root / "target");
      std::filesystem::create_symlink(files.root / "target", files.legacy);
    }
    const auto stateBytes = readGenerationFile(files.legacy);
    const auto lockBytes = readGenerationFile(lock);
    BOOST_CHECK_THROW(ControllerGenerationStore::openDefault("/controller/a", files.legacy),
                      std::runtime_error);
    BOOST_CHECK_EQUAL(readGenerationFile(files.legacy), stateBytes);
    BOOST_CHECK_EQUAL(readGenerationFile(lock), lockBytes);
  }
}

BOOST_AUTO_TEST_CASE(GenerationTemporarySymlinkCannotOverwriteAnotherFile)
{
  DefaultGenerationFiles files;
  const auto store = ControllerGenerationStore::openDefault("/controller/a", files.legacy);
  const auto target = files.root / "unrelated";
  std::ofstream(target) << "preserved";
  const auto temporary = store->statePath().string() + ".tmp." +
    std::to_string(store->fencingValue());
  std::filesystem::create_symlink(target, temporary);
  BOOST_CHECK_THROW(store->startGeneration(1000), std::runtime_error);
  BOOST_CHECK_EQUAL(readGenerationFile(target), "preserved");
  BOOST_CHECK(!std::filesystem::exists(store->statePath()));
  BOOST_CHECK(std::filesystem::is_symlink(temporary));
  std::filesystem::remove(temporary);
  BOOST_CHECK(store->startGeneration(1000).isValid());
}

BOOST_AUTO_TEST_CASE(PersistedUppercaseCertificateTargetMatchesCanonicalSubject)
{
  auto status = makeRefreshStatus(ControllerVersion{1000, 1});
  RevocationTarget target;
  target.kind = RevocationKind::CERTIFICATE;
  target.targetIdentity = ndn::Name("/provider");
  target.certificateDigest = "sha256:" + std::string(64, 'A');
  status.addRevocation(target);
  const auto originalWire = status.wireEncode();
  PolicyStatusData restored;
  BOOST_REQUIRE(restored.wireDecode(originalWire));
  BOOST_CHECK(restored.wireEncode() == originalWire);
  BOOST_CHECK_EQUAL(restored.getRevocations().front().certificateDigest,
                    target.certificateDigest);
  RevocationState state(status.getServiceName());
  BOOST_REQUIRE(state.acceptStatus(restored, 200, true));
  AuthorizationSubject subject{target.targetIdentity,
      "sha256:" + std::string(64, 'a'), status.getServiceName(),
      ndn::Name("/SERVICE/ObjectDetection/YOLOv8")};
  BOOST_CHECK(!state.authorize(subject, ProtectedTransition::PROVIDER_EXECUTION, 200).allowed);
  subject.certificateDigest.back() = 'b';
  BOOST_CHECK(state.authorize(subject, ProtectedTransition::PROVIDER_EXECUTION, 200).allowed);
  subject.certificateDigest = "sha256:" + std::string(64, 'a');
  subject.identity = ndn::Name("/another-provider");
  BOOST_CHECK(state.authorize(subject, ProtectedTransition::PROVIDER_EXECUTION, 200).allowed);
  BOOST_CHECK(!sameCertificateDigest("sha256:AB", "sha256:ab"));
  BOOST_CHECK(!sameCertificateDigest("opaque-A", "opaque-a"));
}

BOOST_AUTO_TEST_CASE(UnauthenticatedHintsNeverChangeRefreshState)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_CHECK(coordinator.observeHint(ControllerVersion{1000, 2}, false, 200).outcome ==
              PolicyRefreshCoordinator::Outcome::REJECTED_UNAUTHENTICATED_HINT);
  BOOST_CHECK(!coordinator.inFlight());
  BOOST_CHECK(!coordinator.hasCurrentStatus());
}

BOOST_AUTO_TEST_CASE(AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch)
{
  // A protected message may be the first evidence that this runtime is behind
  // the Controller.  The hint starts a bounded exact-name fetch, but it is not
  // an authority update and therefore cannot authorize a transition by itself.
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 2, 10);
  const ControllerVersion hintedVersion{1100, 1};
  const auto started = coordinator.observeHint(hintedVersion, true, 100);
  BOOST_REQUIRE(started.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(started.request);
  BOOST_CHECK(started.request->version == hintedVersion);
  BOOST_CHECK(!coordinator.hasCurrentStatus());
  BOOST_CHECK(!coordinator.currentVersion().isValid());

  // A forged/invalid exact result leaves the runtime without authority and
  // schedules only the bounded retry; a valid signed result is the sole path
  // that installs the version and permits later authorization.
  const auto rejected = coordinator.completeFetch(
      makeRefreshStatus(hintedVersion), 200, false);
  BOOST_CHECK(rejected.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK(!coordinator.hasCurrentStatus());
  BOOST_REQUIRE(rejected.request);
  BOOST_CHECK_EQUAL(rejected.request->attempt, 2);
  BOOST_CHECK(coordinator.failFetch(210).outcome ==
              PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_CHECK(!coordinator.hasCurrentStatus());

  const auto retry = coordinator.observeHint(hintedVersion, true, 300);
  BOOST_REQUIRE(retry.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(retry.request);
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(hintedVersion), 400).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_CHECK(coordinator.hasCurrentStatus());
  BOOST_CHECK(coordinator.currentVersion() == hintedVersion);
}

BOOST_AUTO_TEST_CASE(PolicyStatusNameIsExactAndVersionAddressable)
{
  const ndn::Name controller("/controller/spec179");
  const ndn::Name service("/ObjectDetection/YOLOv8");
  const ControllerVersion version{1788285600123ULL, 7};
  const auto exact = makePolicyStatusName(controller, service, version);

  ndn::Name expected(controller);
  expected.append("NDNSF").append("POLICY-STATUS").append(service)
      .append("version")
      .appendNumber(version.controllerGenerationTimestamp)
      .appendNumber(version.controllerEpoch);
  BOOST_CHECK(exact == expected);
  const auto parsed = parsePolicyStatusName(controller, exact);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK(parsed->serviceName == service);
  BOOST_REQUIRE(parsed->version);
  BOOST_CHECK(*parsed->version == version);

  // A ParametersSha256Digest belongs to the Interest name and must not
  // change the (service, generation, epoch) identity used for retrieval.
  auto withDigest = exact;
  std::array<uint8_t, 32> digest{};
  withDigest.appendParametersSha256Digest(
      ndn::span<const uint8_t>(digest.data(), digest.size()));
  const auto parsedWithDigest = parsePolicyStatusName(controller, withDigest);
  BOOST_REQUIRE(parsedWithDigest);
  BOOST_CHECK(parsedWithDigest->serviceName == service);
  BOOST_REQUIRE(parsedWithDigest->version);
  BOOST_CHECK(*parsedWithDigest->version == version);

  // Bare service names remain the only bootstrap form; they carry no
  // authority version and therefore cannot be mistaken for an exact result.
  const auto bootstrap = parsePolicyStatusName(
      controller,
      ndn::Name("/controller/spec179/NDNSF/POLICY-STATUS/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(bootstrap);
  BOOST_CHECK(bootstrap->serviceName == service);
  BOOST_CHECK(!bootstrap->version);
  BOOST_CHECK(!parsePolicyStatusName(
      ndn::Name("/controller/other"), exact));
  BOOST_CHECK_THROW(makePolicyStatusName(controller, service, ControllerVersion{}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(HigherHintsCoalesceToOneHighestCandidate)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{1000, 1}), 200));

  const auto first = coordinator.observeHint(ControllerVersion{1000, 2}, true, 300);
  BOOST_REQUIRE(first.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(first.request);
  BOOST_CHECK((first.request->version == ControllerVersion{1000, 2}));
  BOOST_CHECK_EQUAL(first.request->attempt, 1);

  const auto duplicate = coordinator.observeHint(ControllerVersion{1000, 2}, true, 301);
  BOOST_CHECK(duplicate.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  const auto higher = coordinator.observeHint(ControllerVersion{1001, 1}, true, 302);
  BOOST_CHECK(higher.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  BOOST_REQUIRE(coordinator.highestCandidate());
  BOOST_CHECK((*coordinator.highestCandidate() == ControllerVersion{1001, 1}));

  const auto accepted = coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{1001, 1}), 400);
  BOOST_CHECK(accepted.outcome == PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_CHECK(!coordinator.inFlight());
  BOOST_CHECK((coordinator.currentVersion() == ControllerVersion{1001, 1}));
}

BOOST_AUTO_TEST_CASE(TransportOwnedFetchSurvivesUnrelatedStatusInstall)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 3, 10);
  const ControllerVersion first{1000, 1};
  const ControllerVersion second{1000, 2};
  const ControllerVersion highest{1000, 3};
  BOOST_REQUIRE(coordinator.installCurrentStatus(makeRefreshStatus(first), 200));
  BOOST_REQUIRE(coordinator.observeHint(second, true, 300).request);
  BOOST_CHECK(coordinator.observeHint(highest, true, 301).outcome ==
              PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);

  // A separately validated status must not finish another transport's work.
  BOOST_REQUIRE(coordinator.installCurrentStatus(makeRefreshStatus(second), 400, true, true));
  BOOST_CHECK(coordinator.inFlight());
  BOOST_CHECK_EQUAL(coordinator.attempt(), 1);
  BOOST_CHECK(coordinator.currentVersion() == second);
  BOOST_REQUIRE(coordinator.highestCandidate());
  BOOST_CHECK(*coordinator.highestCandidate() == highest);
  BOOST_CHECK(!coordinator.installCurrentStatus(makeRefreshStatus(highest), 401, false, true));
  BOOST_CHECK(coordinator.currentVersion() == second);
  BOOST_CHECK(coordinator.inFlight());

  coordinator.finishTransportFetch();
  BOOST_CHECK(!coordinator.inFlight());
  BOOST_CHECK_EQUAL(coordinator.attempt(), 0);
  BOOST_CHECK(!coordinator.retryIfDue(500));
  BOOST_REQUIRE(coordinator.highestCandidate());
  BOOST_CHECK(*coordinator.highestCandidate() == highest);

  // A scheduled refresh of the current version cannot forget the higher hint.
  const auto restarted = coordinator.startScheduledRefresh(4999, 10);
  BOOST_REQUIRE(restarted.request);
  BOOST_CHECK(restarted.request->version == highest);
  BOOST_REQUIRE(coordinator.installCurrentStatus(makeRefreshStatus(highest), 4999, true, true));
  BOOST_CHECK(coordinator.inFlight());
  BOOST_CHECK(!coordinator.highestCandidate());
  const auto staleCompletion = coordinator.completeFetch(makeRefreshStatus(second), 4999, true);
  BOOST_CHECK(staleCompletion.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK(coordinator.currentVersion() == highest);
  BOOST_CHECK(coordinator.inFlight());
  coordinator.finishTransportFetch();
  BOOST_CHECK(!coordinator.inFlight());
  BOOST_CHECK(coordinator.currentVersion() == highest);

  // Existing callers still finish a fetch on the ordinary three-argument install.
  BOOST_REQUIRE(coordinator.startScheduledRefresh(4999, 10).request);
  BOOST_REQUIRE(coordinator.installCurrentStatus(makeRefreshStatus(highest), 4999, true));
  BOOST_CHECK(!coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(HigherHintNeverBecomesAuthorityBeforeExactStatusFetch)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto current = makeRefreshStatus(ControllerVersion{1250, 1});
  BOOST_REQUIRE(coordinator.installCurrentStatus(current, 200));

  const auto hint = coordinator.observeHint(ControllerVersion{1250, 2}, true, 300);
  BOOST_REQUIRE(hint.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(hint.request);
  BOOST_CHECK(coordinator.currentVersion() == current.getControllerVersion());
  BOOST_CHECK(coordinator.hasCurrentStatus());

  // An invalid exact-name fetch must leave the last accepted authority in
  // place; only a validated status may advance it.
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{1250, 2}), 400, false).request);
  BOOST_CHECK(coordinator.currentVersion() == current.getControllerVersion());
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{1250, 2}), 500).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  const ControllerVersion advanced{1250, 2};
  BOOST_CHECK(coordinator.currentVersion() == advanced);
}

BOOST_AUTO_TEST_CASE(ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{2000, 1}, 100, 1000), 200));
  BOOST_CHECK(coordinator.startScheduledRefresh(700, 100).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);
  const auto started = coordinator.startScheduledRefresh(950, 100);
  BOOST_CHECK(started.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  const auto coalesced = coordinator.startScheduledRefresh(951, 100);
  BOOST_CHECK(coalesced.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  BOOST_CHECK(coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(InvalidStatusRetriesWithBoundedAttempts)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 3, 10);
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{3000, 1}), 200));
  BOOST_REQUIRE(coordinator.observeHint(ControllerVersion{3000, 2}, true, 300).request);

  const auto invalid = coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{3000, 2}), 400, false);
  BOOST_CHECK(invalid.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_REQUIRE(invalid.request);
  BOOST_CHECK_EQUAL(invalid.request->attempt, 2);
  BOOST_CHECK(!coordinator.retryIfDue(419));
  BOOST_CHECK(coordinator.retryIfDue(420));

  const auto timeout = coordinator.failFetch(420);
  BOOST_CHECK(timeout.outcome == PolicyRefreshCoordinator::Outcome::RETRY_SCHEDULED);
  BOOST_REQUIRE(timeout.request);
  BOOST_CHECK_EQUAL(timeout.request->attempt, 3);
  const auto exhausted = coordinator.failFetch(500);
  BOOST_CHECK(exhausted.outcome == PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_CHECK(!coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(OlderAndConflictingEqualVersionStatusFailClosed)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{4000, 1}), 200));
  BOOST_REQUIRE(coordinator.observeHint(ControllerVersion{4000, 2}, true, 300).request);

  const auto stale = coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{4000, 1}), 400);
  BOOST_CHECK(stale.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK((coordinator.currentVersion() == ControllerVersion{4000, 1}));

  // Exhaust the bounded retry state before starting a fresh candidate.
  BOOST_CHECK(coordinator.failFetch(400).outcome ==
              PolicyRefreshCoordinator::Outcome::RETRY_SCHEDULED);
  BOOST_CHECK(coordinator.failFetch(420).outcome ==
              PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_REQUIRE(coordinator.observeHint(ControllerVersion{4000, 2}, true, 500).request);
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{4000, 2}), 600).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);

  // A scheduled refresh can fetch the current version again.  A different
  // payload at that same version must not replace the accepted authority.
  BOOST_REQUIRE(coordinator.startScheduledRefresh(4999, 10).request);
  auto conflicting = makeRefreshStatus(ControllerVersion{4000, 2});
  conflicting.addRevocation(RevocationTarget{
      RevocationKind::IDENTITY, ndn::Name("/user/alice"), {}, {}, {}});
  const auto result = coordinator.completeFetch(conflicting, 800);
  BOOST_CHECK(result.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK((coordinator.currentVersion() == ControllerVersion{4000, 2}));
}

BOOST_AUTO_TEST_CASE(ServiceScopeAndMissingStatusFailClosed)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_CHECK(coordinator.startScheduledRefresh(100, 10).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);
  BOOST_CHECK(!coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{5000, 1}), 200, false));
  BOOST_CHECK(!coordinator.installCurrentStatus(
      [] {
        auto status = makeRefreshStatus(ControllerVersion{5000, 1});
        status.setServiceName(ndn::Name("/OtherService"));
        return status;
      }(), 200));
  BOOST_CHECK(!coordinator.hasCurrentStatus());
}

BOOST_AUTO_TEST_CASE(RefreshCoordinatorHandlesTerminalAndExpiryBoundaries)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 2, 25);

  // A scheduled refresh cannot invent authority, and completion/failure
  // callbacks without an outstanding fetch terminate deterministically.
  BOOST_CHECK(coordinator.startScheduledRefresh(100, 10).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{6000, 1}), 100).outcome ==
              PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK(coordinator.failFetch(100).outcome ==
              PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_CHECK(!coordinator.retryIfDue(100));

  const auto current = makeRefreshStatus(ControllerVersion{6000, 1}, 100, 1000);
  BOOST_REQUIRE(coordinator.installCurrentStatus(current, 200));
  BOOST_CHECK(!coordinator.hasExpiredStatus(999));
  BOOST_CHECK(coordinator.hasExpiredStatus(1000));
  BOOST_CHECK(coordinator.observeHint(current.getControllerVersion(), true, 300).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);

  const auto scheduled = coordinator.startScheduledRefresh(950, 100);
  BOOST_REQUIRE(scheduled.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(scheduled.request);
  BOOST_CHECK(!coordinator.retryIfDue(949));
  BOOST_CHECK(coordinator.retryIfDue(950));
  BOOST_CHECK(coordinator.completeFetch(current, 950).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_CHECK(!coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(RevocationStateInvalidatesOnlyAcceptedServiceFamilies)
{
  const ndn::Name service("/ObjectDetection/YOLOv8");
  const ndn::Name otherService("/OtherService");
  const auto current = makeRefreshStatus(ControllerVersion{7000, 1});

  RevocationState state(service);
  BOOST_REQUIRE(state.acceptStatus(current, 1000));
  state.clearInvalidationEvidence();

  auto revoked = makeRefreshStatus(ControllerVersion{7000, 2});
  revoked.addRevocation(RevocationTarget{
      RevocationKind::SERVICE_AUTHORIZATION,
      ndn::Name("/provider/p1"), service, {},
      ndn::Name("/SERVICE/ObjectDetection/YOLOv8")});
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
  for (const auto family : {"abe", "message-key", "targeted-token",
                            "selection-binding", "nonce", "replay"})
    BOOST_CHECK(state.cacheInvalidated(family));

  // Re-reading the identical immutable status is idempotent.  It must not
  // evict request keys or replay state a second time merely because refresh
  // was scheduled or a duplicate status Data arrived.
  state.clearInvalidationEvidence();
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 0);

  // A status for another service cannot become authority here or invalidate
  // this service's authorization material.
  RevocationState otherState(otherService);
  auto otherCurrent = makeRefreshStatus(ControllerVersion{7000, 1});
  otherCurrent.setServiceName(otherService);
  BOOST_REQUIRE(otherState.acceptStatus(otherCurrent, 1000));
  otherState.clearInvalidationEvidence();
  BOOST_CHECK(!otherState.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(otherState.invalidatedCacheCount(), 0);
}

BOOST_AUTO_TEST_CASE(ArchivedGenerationsAreRetainedAndBounded)
{
  DefaultGenerationFiles files;
  const auto owner = ndn::Name("/controller/a").toUri(ndn::name::UriFormat::CANONICAL);
  ControllerGenerationStore::AbeMaterial material;
  material.owner = owner;
  material.signingKeyDigest = "sha256:" + std::string(64, 'a');
  material.policyDigest = "sha256:" + std::string(64, 'b');
  material.publicParameters = "public";
  material.masterSecret = "private";
  {
    ControllerGenerationStore store(files.legacy);
    BOOST_REQUIRE(store.acquireWriter(owner));
    store.startGeneration(1000);
    store.storeAbeMaterial(material);
    BOOST_CHECK(store.loadAbeHistory().empty());
    for (uint64_t index = 1; index <= 3; ++index) {
      ControllerGenerationStore::AbeHistoryEntry entry;
      entry.generationTimestamp = 1000 + index;
      entry.epoch = index;
      entry.publicParameters = "params-" + std::to_string(index);
      entry.masterSecret = "secret-" + std::to_string(index);
      entry.publicParametersName = "/controller/a/PUBPARAMS/kp-abe/v=" +
        std::to_string(1000 + index);
      entry.publicParametersWireDigest = "sha256:" + std::string(64, 'c');
      entry.reason = "revocation-rotation";
      store.recordAbeHistory(entry, 2);
    }
    ControllerGenerationStore::AbeHistoryEntry invalid;
    invalid.generationTimestamp = 0;
    BOOST_CHECK_THROW(store.recordAbeHistory(invalid), std::invalid_argument);
    // Retention is a bound, not a suggestion: the oldest archive is gone.
    const auto bounded = store.loadAbeHistory();
    BOOST_REQUIRE_EQUAL(bounded.size(), 2u);
    BOOST_CHECK_EQUAL(bounded.front().generationTimestamp, 1002u);
    // A retried rotation re-archives the same material; it must not evict a real one.
    ControllerGenerationStore::AbeHistoryEntry retry;
    retry.generationTimestamp = 1003;
    retry.epoch = 3;
    retry.publicParameters = "params-3";
    retry.masterSecret = "secret-3";
    retry.publicParametersName = "/controller/a/PUBPARAMS/kp-abe/v=1003";
    retry.publicParametersWireDigest = "sha256:" + std::string(64, 'c');
    retry.reason = "revocation-rotation";
    store.recordAbeHistory(retry, 2);
    const auto afterRetry = store.loadAbeHistory();
    BOOST_REQUIRE_EQUAL(afterRetry.size(), 2u);
    BOOST_CHECK_EQUAL(afterRetry.front().generationTimestamp, 1002u);
    BOOST_CHECK_EQUAL(afterRetry.back().generationTimestamp, 1003u);
  }
  {
    // History is readable without the writer lease; the secrets never leave the file.
    ControllerGenerationStore store(files.legacy);
    const auto history = store.loadAbeHistory();
    BOOST_REQUIRE_EQUAL(history.size(), 2u);
    BOOST_CHECK_EQUAL(history.back().generationTimestamp, 1003u);
    BOOST_CHECK_EQUAL(history.back().epoch, 3u);
    BOOST_CHECK_EQUAL(history.back().masterSecret, std::string("secret-3"));
    BOOST_CHECK_EQUAL(history.back().digest.size(), 71u);
    BOOST_REQUIRE(store.acquireWriter(owner));
    const auto current = store.loadAbeMaterial();
    BOOST_REQUIRE(current);
    // Archiving must not disturb the live generation.
    BOOST_CHECK_EQUAL(current->masterSecret, material.masterSecret);
  }
  {
    // A rewritten archive secret is detected instead of being served.
    std::fstream state(files.legacy, std::ios::in | std::ios::out | std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(state)),
                      std::istreambuf_iterator<char>());
    const auto at = bytes.find("secret-3");
    BOOST_REQUIRE(at != std::string::npos);
    bytes.replace(at, std::string("secret-3").size(), "secret-9");
    state.seekp(0);
    state.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    state.flush();
  }
  ControllerGenerationStore tampered(files.legacy);
  BOOST_CHECK_THROW(tampered.loadAbeHistory(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ArchivedGenerationDecryptsItsOwnCiphertext)
{
  // Spec 243 T002 premise: the archived material must be enough to decrypt Data
  // protected under that generation, using only the NAC-ABE primitives.
  DefaultGenerationFiles files;
  const auto owner = ndn::Name("/controller/a").toUri(ndn::name::UriFormat::CANONICAL);
  auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  abe.kpInit(params, master);
  const auto paramsWire = params.toBuffer();
  const std::string publicParameters(paramsWire.begin(), paramsWire.end());
  const std::string masterSecret = master.m_msk;
  BOOST_REQUIRE(!publicParameters.empty());
  BOOST_REQUIRE(!masterSecret.empty());

  const ndn::Buffer plaintext{9, 8, 7, 6, 5};
  const auto cipherText = abe.kpEncrypt(params, {"attr-a"}, plaintext);

  {
    ControllerGenerationStore store(files.legacy);
    BOOST_REQUIRE(store.acquireWriter(owner));
    store.startGeneration(1000);
    ControllerGenerationStore::AbeHistoryEntry archived;
    archived.generationTimestamp = 1000;
    archived.epoch = 1;
    archived.publicParameters = publicParameters;
    archived.masterSecret = masterSecret;
    archived.publicParametersName = "/controller/a/PUBPARAMS/kp-abe/v=1000";
    archived.publicParametersWireDigest = "sha256:" + std::string(64, 'c');
    archived.reason = "revocation-rotation";
    store.recordAbeHistory(archived);
  }

  ControllerGenerationStore store(files.legacy);
  const auto history = store.loadAbeHistory();
  BOOST_REQUIRE_EQUAL(history.size(), 1u);

  // Rebuild the generation from the archive alone, then decrypt with it.
  ndn::nacabe::algo::PublicParams archivedParams;
  archivedParams.fromBuffer(ndn::Buffer(history.front().publicParameters.begin(),
                                        history.front().publicParameters.end()));
  ndn::nacabe::algo::MasterKey archivedMaster;
  archivedMaster.m_msk = history.front().masterSecret;
  auto key = abe.kpPrvKeyGen(archivedParams, archivedMaster, "attr-a");
  const auto decrypted = abe.kpDecrypt(archivedParams, key, cipherText);
  BOOST_CHECK(decrypted == plaintext);

  // A policy the generation did not authorize must not produce that plaintext.
  bool rejected = false;
  try {
    auto wrong = abe.kpPrvKeyGen(archivedParams, archivedMaster, "attr-z");
    const auto wrongPlain = abe.kpDecrypt(archivedParams, wrong, cipherText);
    rejected = wrongPlain != plaintext;
  }
  catch (const std::exception&) {
    rejected = true;
  }
  BOOST_CHECK(rejected);
}

BOOST_AUTO_TEST_CASE(FileKeyRingResolvesByConfiguredId)
{
  DefaultGenerationFiles files;
  const auto ring = files.root / "keyring";
  ::unsetenv("NDNSF_FILE_KEYRING");
  BOOST_CHECK(!fileKeyRingPath());
  BOOST_CHECK(!loadFileKeyRingEntry("file-key-1"));

  {
    std::ofstream out(ring);
    out << "# deployment file keys\n"
        << "file-key-1 " << std::string(64, 'a') << "\n"
        << "file-key-2 " << std::string(64, 'b') << "\n";
  }
  std::filesystem::permissions(ring, std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write);
  ::setenv("NDNSF_FILE_KEYRING", ring.c_str(), 1);
  const auto found = loadFileKeyRingEntry("file-key-2");
  BOOST_REQUIRE(found);
  BOOST_CHECK_EQUAL(found->keyId, std::string("file-key-2"));
  BOOST_REQUIRE_EQUAL(found->key.size(), 32u);
  BOOST_CHECK_EQUAL(found->key.front(), 0xbb);
  BOOST_CHECK(!loadFileKeyRingEntry("file-key-9"));

  // A readable-by-group ring is not a private key store.
  std::filesystem::permissions(ring, std::filesystem::perms::group_read,
                               std::filesystem::perm_options::add);
  BOOST_CHECK_THROW(loadFileKeyRingEntry("file-key-1"), std::runtime_error);
  std::filesystem::permissions(ring, std::filesystem::perms::group_read,
                               std::filesystem::perm_options::remove);

  // A duplicated ID is ambiguous, not "first wins".
  {
    std::ofstream out(ring, std::ios::trunc);
    out << "file-key-1 " << std::string(64, 'a') << "\n"
        << "file-key-1 " << std::string(64, 'b') << "\n";
  }
  BOOST_CHECK_THROW(loadFileKeyRingEntry("file-key-1"), std::runtime_error);

  // A malformed key rejects the ring even for a different ID.
  {
    std::ofstream out(ring, std::ios::trunc);
    out << "file-key-1 nothex\n";
  }
  BOOST_CHECK_THROW(loadFileKeyRingEntry("file-key-2"), std::runtime_error);
  ::unsetenv("NDNSF_FILE_KEYRING");
}

namespace {
std::string
entryName(const ndn::Name& identity, uint64_t generation, uint64_t epoch,
          const std::string& digestHex)
{
  return historicalPublicParametersName(identity, generation, epoch, digestHex)
    .toUri(ndn::name::UriFormat::CANONICAL);
}
std::string
digestHexOf(const std::string& bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty())
    digest << bytes;
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return hex;
}
} // namespace

BOOST_AUTO_TEST_CASE(HistoricalPublicParametersBindTheOriginalObject)
{
  const ndn::Name identity("/controller/a");
  const std::string params("params-wire\0", 12);
  const auto hex = digestHexOf(params);
  const auto expected = historicalPublicParametersName(identity, 1002, 3, hex);
  BOOST_CHECK_EQUAL(expected.size(), 8u);
  BOOST_CHECK_EQUAL(expected.get(4).toUri(), std::string("PUBPARAMS"));
  BOOST_CHECK_EQUAL(expected.get(5).toUri(), std::string("1002"));
  BOOST_CHECK_EQUAL(expected.get(6).toUri(), std::string("3"));
  BOOST_CHECK_EQUAL(expected.get(7).toUri(), hex);
  BOOST_CHECK_THROW(historicalPublicParametersName(identity, 0, 3, hex), std::invalid_argument);
  BOOST_CHECK_EQUAL(expected.toUri(ndn::name::UriFormat::CANONICAL),
                    entryName(identity, 1002, 3, hex));

  ControllerGenerationStore::AbeHistoryEntry entry;
  entry.generationTimestamp = 1002;
  entry.epoch = 3;
  entry.publicParameters = params;
  entry.publicParametersName = "/controller/a/PUBPARAMS/KP-ABE/v=1002003";
  entry.masterSecret = "secret";
  entry.publicParametersWireDigest = "sha256:" + hex;

  bool signedOnce = false;
  const auto sign = [&signedOnce] (ndn::Data& data) {
    data.setSignatureInfo(ndn::SignatureInfo());
    data.setSignatureValue(ndn::Buffer{1, 2, 3});
    signedOnce = true;
  };
  ndn::Data answer;
  BOOST_REQUIRE(buildHistoricalPublicParametersData(entry, identity, sign, answer));
  BOOST_CHECK(signedOnce);
  BOOST_CHECK(answer.getName() == expected);
  // The envelope carries the original name and the digest before the parameters themselves.
  const std::string content(reinterpret_cast<const char*>(answer.getContent().value()),
                            answer.getContent().value_size());
  BOOST_CHECK(content.rfind("NDNSF-HIST-PUBPARAMS-v1\noriginal /controller/a/PUBPARAMS", 0) == 0);
  BOOST_CHECK(content.find("digest sha256:" + hex) != std::string::npos);
  BOOST_CHECK(content.find("length 12\n" + params) != std::string::npos);

  // A digest that does not match the archived bytes is refused rather than published unbound.
  auto damaged = entry;
  damaged.publicParametersWireDigest = "sha256:" + std::string(64, 'd');
  ndn::Data refused;
  BOOST_CHECK(!buildHistoricalPublicParametersData(damaged, identity, sign, refused));
  auto unnamed = entry;
  unnamed.publicParametersName.clear();
  BOOST_CHECK(!buildHistoricalPublicParametersData(unnamed, identity, sign, refused));
  auto empty = entry;
  empty.publicParameters.clear();
  BOOST_CHECK(!buildHistoricalPublicParametersData(empty, identity, sign, refused));
}

BOOST_AUTO_TEST_CASE(BrokerAnswersOnceForTheNamedCertificateAndTheKeyWorks)
{
  DefaultGenerationFiles files;
  const auto requesterDir = files.root / "requester";
  const auto controllerDir = files.root / "controller";
  std::filesystem::create_directory(requesterDir);
  std::filesystem::create_directory(controllerDir);
  ndn::KeyChain requester("pib-sqlite3:" + requesterDir.string(),
                          "tpm-file:" + requesterDir.string(), false);
  ndn::KeyChain controller("pib-sqlite3:" + controllerDir.string(),
                           "tpm-file:" + controllerDir.string(), false);
  const ndn::Name requesterIdentity("/user/history");
  const ndn::Name controllerIdentity("/controller/a");
  requester.createIdentity(requesterIdentity);
  controller.createIdentity(controllerIdentity);
  const auto userCert = requester.getPib().getIdentity(requesterIdentity).getDefaultKey()
    .getDefaultCertificate();
  const auto controllerCert = controller.getPib().getIdentity(controllerIdentity).getDefaultKey()
    .getDefaultCertificate();

  // A real archived generation: the key minted for it must decrypt what it protected.
  auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  abe.kpInit(params, master);
  const auto wire = params.toBuffer();
  const auto cipher = abe.kpEncrypt(params, {"attr-a"}, ndn::Buffer{9, 8, 7});

  HistoricalKeyBroker::Material material;
  material.publicParameters.assign(wire.begin(), wire.end());
  material.masterSecret = master.m_msk;
  const auto signerLookup = [userCert] (const ndn::Name& name, ndn::security::Certificate& out) {
    if (name != userCert.getName())
      return false;
    out = userCert;
    return true;
  };
  const auto materialLookup = [&material] (uint64_t generation, uint64_t epoch,
                                           HistoricalKeyBroker::Material& out) {
    if (generation != 1002 || epoch != 3)
      return false;
    out = material;
    return true;
  };
  const auto replySigner = [&controller, controllerCert] (ndn::Data& data) {
    controller.sign(data, ndn::security::signingByCertificate(controllerCert));
  };
  HistoricalKeyBroker broker(signerLookup, materialLookup, replySigner);

  HistoricalKeyRequest request;
  request.controllerPrefix = controllerIdentity;
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "attr-a";
  request.recipientCertificate = userCert.getName();
  request.mintedAtMs = 1'000'000;
  request.expiresAtMs = 1'060'000;
  ndn::Interest interest;
  signHistoricalKeyRequest(interest, request, requester,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      userCert.getName()));

  const auto reply = broker.answer(interest, controllerIdentity, 1'000'000);
  BOOST_REQUIRE(reply);
  HistoricalKeyReply accepted;
  BOOST_REQUIRE(verifyHistoricalKeyReply(*reply, request, 1'000'000, accepted));
  // The minted key really decrypts the archived generation's ciphertext.
  ndn::nacabe::algo::PrivateKey key;
  key.fromBuffer(ndn::Buffer(reinterpret_cast<const uint8_t*>(accepted.keyBytes.data()),
                             accepted.keyBytes.size()));
  BOOST_CHECK(abe.kpDecrypt(params, key, cipher) == ndn::Buffer({9, 8, 7}));
  // The same request must not mint twice.
  BOOST_CHECK(!broker.answer(interest, controllerIdentity, 1'000'000));
  // A request naming someone else's certificate gets nothing.
  auto stranger = request;
  stranger.recipientCertificate = controllerCert.getName();
  ndn::Interest strangerInterest;
  signHistoricalKeyRequest(strangerInterest, stranger, requester,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      userCert.getName()));
  BOOST_CHECK(!broker.answer(strangerInterest, controllerIdentity, 1'000'000));
}

BOOST_AUTO_TEST_CASE(SignedHistoricalKeyRequestCoversNameAndBody)
{
  // A consumer signs the request itself: the signature must cover the name and the body, so neither
  // can be swapped for another requester's or another scope's.
  DefaultGenerationFiles files;
  const auto identityDir = files.root / "requester";
  std::filesystem::create_directory(identityDir);
  ndn::KeyChain keyChain("pib-sqlite3:" + identityDir.string(),
                         "tpm-file:" + identityDir.string(), false);
  const ndn::Name identity("/user/history");
  keyChain.createIdentity(identity);
  const auto certificate = keyChain.getPib().getIdentity(identity).getDefaultKey()
    .getDefaultCertificate();

  HistoricalKeyRequest request;
  request.controllerPrefix = ndn::Name("/controller/a");
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "/SERVICE/ObjectDetection";
  request.recipientCertificate = certificate.getName();
  request.mintedAtMs = 1'000'000;
  request.expiresAtMs = 1'060'000;

  ndn::Interest interest;
  signHistoricalKeyRequest(interest, request, keyChain,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      certificate.getName()));
  const auto requestName = makeHistoricalKeyRequestName(request.controllerPrefix, request);
  BOOST_CHECK(interest.getName().getSubName(0, requestName.size()) == requestName);
  HistoricalKeyRequest carried;
  const auto parameters = interest.getApplicationParameters();
  BOOST_REQUIRE(decodeHistoricalKeyRequest(std::string(
    reinterpret_cast<const char*>(parameters.value()), parameters.value_size()), carried));
  BOOST_CHECK(carried == request);

  const auto& publicKey = certificate.getPublicKey();
  BOOST_CHECK(ndn::security::verifySignature(interest, publicKey));
  // Changing the body without re-signing must invalidate that signature.
  ndn::Interest tampered = interest;
  HistoricalKeyRequest widened = request;
  widened.policy = "/SERVICE/Everything";
  const auto otherBody = encodeHistoricalKeyRequest(widened);
  tampered.setApplicationParameters(ndn::Buffer(
    reinterpret_cast<const uint8_t*>(otherBody.data()), otherBody.size()));
  BOOST_CHECK(!ndn::security::verifySignature(tampered, publicKey));
}

BOOST_AUTO_TEST_CASE(HistoricalKeyReplyBindsRequestRecipientGenerationScopeAndExpiry)
{
  HistoricalKeyRequest request;
  request.controllerPrefix = ndn::Name("/controller/a");
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "/SERVICE/ObjectDetection";
  request.recipientCertificate = ndn::Name("/user/KEY/1/self/v=1");
  request.mintedAtMs = 1'000'000;
  request.expiresAtMs = 1'060'000;

  // The canonical body survives a round trip and its digest names the request.
  const auto body = encodeHistoricalKeyRequest(request);
  HistoricalKeyRequest parsed;
  BOOST_REQUIRE(decodeHistoricalKeyRequest(body, parsed));
  BOOST_CHECK(parsed == request);
  BOOST_CHECK_EQUAL(historicalKeyRequestDigest(request).size(), 71u);

  const auto name = makeHistoricalKeyRequestName(request.controllerPrefix, request);
  BOOST_CHECK_EQUAL(name.size(), 8u);
  BOOST_CHECK_EQUAL(name.get(2).toUri(), std::string("NDNSF"));
  BOOST_CHECK_EQUAL(name.get(3).toUri(), std::string("HISTORY"));
  BOOST_CHECK_EQUAL(name.get(4).toUri(), std::string("DKEY"));
  BOOST_CHECK_EQUAL(name.get(5).toUri(), std::string("1002"));
  BOOST_CHECK_EQUAL(name.get(6).toUri(), std::string("3"));
  BOOST_CHECK_EQUAL(name.get(7).toUri(), historicalKeyRequestDigest(request).substr(7));

  const std::string keyBytes("dkey-bytes", 10);
  const auto sign = [] (ndn::Data& data) {
    data.setSignatureInfo(ndn::SignatureInfo());
    data.setSignatureValue(ndn::Buffer{7});
  };
  ndn::Data reply;
  BOOST_REQUIRE(buildHistoricalKeyReply(request, keyBytes, 1'030'000, sign, reply));
  BOOST_CHECK(reply.getName() == name);

  HistoricalKeyReply accepted;
  BOOST_CHECK(verifyHistoricalKeyReply(reply, request, 1'010'000, accepted));
  BOOST_CHECK_EQUAL(accepted.keyBytes, keyBytes);
  BOOST_CHECK_EQUAL(accepted.generation, request.generation);
  BOOST_CHECK_EQUAL(accepted.expiresAtMs, 1'030'000u);   // capped by the reply budget
  // An expired reply, a different recipient and a different generation must all be refused.
  BOOST_CHECK(!verifyHistoricalKeyReply(reply, request, 1'030'000, accepted));
  auto other = request;
  other.recipientCertificate = ndn::Name("/user/KEY/2/self/v=1");
  BOOST_CHECK(!verifyHistoricalKeyReply(reply, other, 1'010'000, accepted));
  auto wrongGeneration = request;
  wrongGeneration.generation = 1003;
  BOOST_CHECK(!verifyHistoricalKeyReply(reply, wrongGeneration, 1'010'000, accepted));
  // The request name is checked against the body, so a reply cannot land under another name.
  BOOST_CHECK(matchHistoricalKeyRequestName(name, request.controllerPrefix, request));
  auto otherController = request;
  otherController.controllerPrefix = ndn::Name("/controller/b");
  BOOST_CHECK(!matchHistoricalKeyRequestName(name, request.controllerPrefix, otherController));
  auto otherScope = request;
  otherScope.policy = "/SERVICE/Other";
  BOOST_CHECK(!matchHistoricalKeyRequestName(name, request.controllerPrefix, otherScope));
  ndn::Name renamed(name);
  renamed.set(renamed.size() - 1, ndn::name::Component(std::string(64, 'b')));
  BOOST_CHECK(!matchHistoricalKeyRequestName(renamed, request.controllerPrefix, request));
  // No key material means no reply worth accepting.
  ndn::Data empty;
  BOOST_CHECK(!buildHistoricalKeyReply(request, std::string(), 1'030'000, sign, empty));
}

BOOST_AUTO_TEST_CASE(HistoricalDecryptionKeyIsMintedInTheLibraryFormat)
{
  // A superseded generation must still be able to issue a key for a policy it authorized.
  auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  abe.kpInit(params, master);
  const auto wire = params.toBuffer();

  HistoricalAbeMaterial material;
  material.publicParameters.assign(wire.begin(), wire.end());
  material.masterSecret = master.m_msk;

  const ndn::Buffer plaintext{4, 5, 6, 7};
  const auto cipherText = abe.kpEncrypt(params, {"attr-a"}, plaintext);

  const auto dkeyWire = issueHistoricalDecryptionKey(material, "attr-a");
  BOOST_REQUIRE(!dkeyWire.empty());
  ndn::nacabe::algo::PrivateKey recovered;
  recovered.fromBuffer(ndn::Buffer(dkeyWire.data(), dkeyWire.size()));
  const auto decrypted = abe.kpDecrypt(params, recovered, cipherText);
  BOOST_CHECK(decrypted == plaintext);

  // An unauthorized policy must not yield that plaintext.
  const auto wrongWire = issueHistoricalDecryptionKey(material, "attr-z");
  ndn::nacabe::algo::PrivateKey wrong;
  wrong.fromBuffer(ndn::Buffer(wrongWire.data(), wrongWire.size()));
  bool rejected = false;
  try {
    rejected = abe.kpDecrypt(params, wrong, cipherText) != plaintext;
  }
  catch (const std::exception&) {
    rejected = true;
  }
  BOOST_CHECK(rejected);

  // Malformed material is refused rather than silently minted.
  auto truncated = material;
  truncated.publicParameters.pop_back();
  BOOST_CHECK_THROW(issueHistoricalDecryptionKey(truncated, "attr-a"), std::runtime_error);
  BOOST_CHECK_THROW(issueHistoricalDecryptionKey(material, ""), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(KeyRingKeepsOlderKeysReadableWhileANewOneWrites)
{
  DefaultGenerationFiles files;
  const auto ring = files.root / "rotation-ring";
  {
    std::ofstream out(ring);
    out << "old-key " << std::string(64, 'a') << "\n"
        << "new-key " << std::string(64, 'b') << "\n";
  }
  std::filesystem::permissions(ring, std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write);
  ::setenv("NDNSF_FILE_KEYRING", ring.c_str(), 1);
  const auto entries = loadFileKeyRingEntries();
  BOOST_REQUIRE_EQUAL(entries.size(), 2u);
  BOOST_CHECK_EQUAL(entries.front().keyId, std::string("old-key"));
  BOOST_CHECK_EQUAL(entries.back().keyId, std::string("new-key"));
  // Both generations stay resolvable by ID, which is what a rotation needs.
  BOOST_CHECK(loadFileKeyRingEntry("old-key"));
  BOOST_CHECK(loadFileKeyRingEntry("new-key"));
  ::unsetenv("NDNSF_FILE_KEYRING");
}

BOOST_AUTO_TEST_CASE(CollectHistoricalAnswersSkipsUnboundGenerations)
{
  const ndn::Name identity("/controller/a");
  const std::string params("wire\0", 5);
  const auto hex = digestHexOf(params);
  const auto makeEntry = [&hex] (uint64_t generation, uint64_t epoch) {
    ControllerGenerationStore::AbeHistoryEntry entry;
    entry.generationTimestamp = generation;
    entry.epoch = epoch;
    entry.publicParameters = std::string("wire\0", 5);
    entry.masterSecret = "secret";
    entry.publicParametersWireDigest = "sha256:" + hex;
    entry.publicParametersName = "/controller/a/PUBPARAMS/KP-ABE/v=" +
      std::to_string(generation * 1000 + epoch);
    return entry;
  };

  std::vector<ControllerGenerationStore::AbeHistoryEntry> entries;
  entries.push_back(makeEntry(1002, 1));
  entries.push_back(makeEntry(1003, 2));
  // A record whose digest does not cover its bytes cannot be bound, so it is skipped.
  auto damaged = makeEntry(1004, 3);
  damaged.publicParametersWireDigest = "sha256:" + std::string(64, 'd');
  entries.push_back(damaged);
  auto empty = makeEntry(1005, 4);
  empty.publicParameters.clear();
  entries.push_back(empty);

  int signedCount = 0;
  const auto sign = [&signedCount] (ndn::Data& data) {
    data.setSignatureInfo(ndn::SignatureInfo());
    data.setSignatureValue(ndn::Buffer{9});
    ++signedCount;
  };
  const auto answers = collectHistoricalPublicParameterAnswers(entries, identity, sign);
  BOOST_REQUIRE_EQUAL(answers.size(), 2u);
  BOOST_CHECK_EQUAL(signedCount, 2);
  BOOST_CHECK(answers.front()->getName() == historicalPublicParametersName(identity, 1002, 1, hex));
  BOOST_CHECK(answers.back()->getName() == historicalPublicParametersName(identity, 1003, 2, hex));
  // Historical names live in their own namespace, so they can never collide with the live one.
  BOOST_CHECK(!answers.front()->getName().equals(
    historicalPublicParametersName(identity, 1003, 2, hex)));
  BOOST_CHECK(collectHistoricalPublicParameterAnswers({}, identity, sign).empty());
}

BOOST_AUTO_TEST_CASE(BrokerRetriesAfterAFailedMintInsteadOfBurningTheRequest)
{
  // A request that could not be answered must stay answerable: burning the digest would make the
  // consumer's only retry impossible.
  DefaultGenerationFiles files;
  const auto requesterDir = files.root / "requester";
  std::filesystem::create_directory(requesterDir);
  ndn::KeyChain requester("pib-sqlite3:" + requesterDir.string(),
                          "tpm-file:" + requesterDir.string(), false);
  const ndn::Name identity("/user/history");
  requester.createIdentity(identity);
  const auto userCert = requester.getPib().getIdentity(identity).getDefaultKey()
    .getDefaultCertificate();

  auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  abe.kpInit(params, master);
  const auto wire = params.toBuffer();

  HistoricalKeyBroker::Material material;
  material.publicParameters.assign(wire.begin(), wire.end());
  material.masterSecret = master.m_msk;
  bool materialUsable = false;
  const auto signerLookup = [userCert] (const ndn::Name& name, ndn::security::Certificate& out) {
    if (name != userCert.getName())
      return false;
    out = userCert;
    return true;
  };
  const auto materialLookup = [&material, &materialUsable] (uint64_t generation, uint64_t epoch,
                                                            HistoricalKeyBroker::Material& out) {
    if (generation != 1002 || epoch != 3 || !materialUsable)
      return false;
    out = material;
    return true;
  };
  const auto replySigner = [] (ndn::Data& data) {
    data.setSignatureInfo(ndn::SignatureInfo());
    data.setSignatureValue(ndn::Buffer{1});
  };
  HistoricalKeyBroker broker(signerLookup, materialLookup, replySigner);

  HistoricalKeyRequest request;
  request.controllerPrefix = ndn::Name("/controller/a");
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "attr-a";
  request.recipientCertificate = userCert.getName();
  request.mintedAtMs = 1'000'000;
  request.expiresAtMs = 1'060'000;
  ndn::Interest interest;
  signHistoricalKeyRequest(interest, request, requester,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      userCert.getName()));

  // Nothing is archived yet, so the first attempt cannot answer.
  BOOST_CHECK(!broker.answer(interest, request.controllerPrefix, 1'000'000));
  materialUsable = true;
  const auto reply = broker.answer(interest, request.controllerPrefix, 1'000'000);
  BOOST_REQUIRE(reply);
  HistoricalKeyReply accepted;
  BOOST_REQUIRE(verifyHistoricalKeyReply(*reply, request, 1'000'000, accepted));
  // Now it is answered, so a replay must not mint again.
  BOOST_CHECK(!broker.answer(interest, request.controllerPrefix, 1'000'000));
}

BOOST_AUTO_TEST_CASE(HistoricalKeyReplyRefusesAnOversizedKey)
{
  // The length field is attacker-controlled; a consumer must not allocate or accept whatever it says.
  HistoricalKeyRequest request;
  request.controllerPrefix = ndn::Name("/controller/a");
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "attr-a";
  request.recipientCertificate = ndn::Name("/user/KEY/1/self/v=1");
  request.mintedAtMs = 1'000'000;
  request.expiresAtMs = 1'060'000;

  const auto sign = [] (ndn::Data& data) {
    data.setSignatureInfo(ndn::SignatureInfo());
    data.setSignatureValue(ndn::Buffer{1});
  };
  ndn::Data reply;
  BOOST_REQUIRE(buildHistoricalKeyReply(request, std::string(10, 'k'), 1'030'000, sign, reply));

  // Rewrite the length field to a large value without touching name or binding fields.
  std::string content(reinterpret_cast<const char*>(reply.getContent().value()),
                      reply.getContent().value_size());
  const auto marker = content.find("\nlength ");
  BOOST_REQUIRE(marker != std::string::npos);
  const auto end = content.find('\n', marker + 1);
  const auto declared = content.substr(marker + 1, end - marker - 1);
  BOOST_REQUIRE_EQUAL(declared, std::string("length 10"));
  content.replace(marker + 1, end - marker - 1, "length 9000");
  ndn::Data forged(reply);
  forged.setContent(ndn::Buffer(reinterpret_cast<const uint8_t*>(content.data()), content.size()));

  HistoricalKeyReply accepted;
  BOOST_CHECK(!verifyHistoricalKeyReply(forged, request, 1'000'000, accepted));
}

BOOST_AUTO_TEST_CASE(BrokerAnswersARealSignedInterestOnce)
{
  std::unique_ptr<ndn::Face> face;
  try {
    face = std::make_unique<ndn::Face>();
    face->processEvents(ndn::time::milliseconds(200));
  }
  catch (const std::exception& error) {
    BOOST_TEST_MESSAGE("no forwarder available, exchange not attempted: " << error.what());
    return;
  }

  DefaultGenerationFiles files;
  const auto requesterDir = files.root / "requester";
  const auto controllerDir = files.root / "controller";
  std::filesystem::create_directory(requesterDir);
  std::filesystem::create_directory(controllerDir);
  ndn::KeyChain requester("pib-sqlite3:" + requesterDir.string(),
                          "tpm-file:" + requesterDir.string(), false);
  ndn::KeyChain controller("pib-sqlite3:" + controllerDir.string(),
                           "tpm-file:" + controllerDir.string(), false);
  const ndn::Name requesterIdentity("/user/history");
  const ndn::Name controllerIdentity("/controller/a");
  requester.createIdentity(requesterIdentity);
  controller.createIdentity(controllerIdentity);
  const auto userCert = requester.getPib().getIdentity(requesterIdentity).getDefaultKey()
    .getDefaultCertificate();
  const auto controllerCert = controller.getPib().getIdentity(controllerIdentity).getDefaultKey()
    .getDefaultCertificate();

  auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  abe.kpInit(params, master);
  const auto wire = params.toBuffer();
  HistoricalKeyBroker::Material material;
  material.publicParameters.assign(wire.begin(), wire.end());
  material.masterSecret = master.m_msk;

  const auto signerLookup = [userCert] (const ndn::Name& name, ndn::security::Certificate& out) {
    if (name != userCert.getName())
      return false;
    out = userCert;
    return true;
  };
  const auto materialLookup = [&material] (uint64_t generation, uint64_t epoch,
                                           HistoricalKeyBroker::Material& out) {
    if (generation != 1002 || epoch != 3)
      return false;
    out = material;
    return true;
  };
  const auto replySigner = [&controller, controllerCert] (ndn::Data& data) {
    controller.sign(data, ndn::security::signingByCertificate(controllerCert));
  };
  HistoricalKeyBroker broker(signerLookup, materialLookup, replySigner);

  const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count());
  HistoricalKeyRequest request;
  request.controllerPrefix = controllerIdentity;
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "attr-a";
  request.recipientCertificate = userCert.getName();
  request.mintedAtMs = now;
  request.expiresAtMs = now + 60000;

  ndn::Name dkeyPrefix(controllerIdentity);
  dkeyPrefix.append("NDNSF").append("HISTORY").append("DKEY");
  auto handle = face->setInterestFilter(dkeyPrefix,
    [face = face.get(), &broker, controllerIdentity, now]
    (const ndn::InterestFilter&, const ndn::Interest& interest) {
      const auto reply = broker.answer(interest, controllerIdentity, now);
      if (reply)
        face->put(*reply);
    });
  face->processEvents(ndn::time::milliseconds(500));

  ndn::Interest interest;
  signHistoricalKeyRequest(interest, request, requester,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      userCert.getName()));
  // Minting a key takes longer than a default Interest lifetime.
  interest.setInterestLifetime(ndn::time::milliseconds(10000));
  BOOST_CHECK(ndn::security::verifySignature(interest, userCert.getPublicKey()));

  ndn::Data received;
  bool arrived = false;
  face->expressInterest(interest,
    [&received, &arrived] (const ndn::Interest&, const ndn::Data& data) {
      received = data;
      arrived = true;
    },
    [] (const ndn::Interest&, const ndn::lp::Nack&) {},
    [] (const ndn::Interest&) {});
  const auto deadline = ndn::time::steady_clock::now() + ndn::time::seconds(15);
  while (!arrived && ndn::time::steady_clock::now() < deadline)
    face->processEvents(ndn::time::milliseconds(100));

  BOOST_REQUIRE(arrived);
  // The consumer needs the binding and a signature by the Controller's own key.
  BOOST_CHECK(verifyHistoricalKeyReplySignature(received, controllerCert));
  HistoricalKeyReply accepted;
  BOOST_REQUIRE(verifyHistoricalKeyReply(received, request, now, accepted));
}

BOOST_AUTO_TEST_CASE(ForwarderDeliversNoKeyToAStrangerOrAnExpiredRequest)
{
  std::unique_ptr<ndn::Face> face;
  try {
    face = std::make_unique<ndn::Face>();
    face->processEvents(ndn::time::milliseconds(200));
  }
  catch (const std::exception& error) {
    BOOST_TEST_MESSAGE("no forwarder available, exchange not attempted: " << error.what());
    return;
  }

  DefaultGenerationFiles files;
  const auto requesterDir = files.root / "requester";
  const auto controllerDir = files.root / "controller";
  std::filesystem::create_directory(requesterDir);
  std::filesystem::create_directory(controllerDir);
  ndn::KeyChain requester("pib-sqlite3:" + requesterDir.string(),
                          "tpm-file:" + requesterDir.string(), false);
  ndn::KeyChain controller("pib-sqlite3:" + controllerDir.string(),
                           "tpm-file:" + controllerDir.string(), false);
  const ndn::Name requesterIdentity("/user/history");
  const ndn::Name controllerIdentity("/controller/a");
  requester.createIdentity(requesterIdentity);
  controller.createIdentity(controllerIdentity);
  const auto userCert = requester.getPib().getIdentity(requesterIdentity).getDefaultKey()
    .getDefaultCertificate();
  const auto controllerCert = controller.getPib().getIdentity(controllerIdentity).getDefaultKey()
    .getDefaultCertificate();

  auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  abe.kpInit(params, master);
  const auto wire = params.toBuffer();
  HistoricalKeyBroker::Material material;
  material.publicParameters.assign(wire.begin(), wire.end());
  material.masterSecret = master.m_msk;

  // The broker only knows the requester's certificate, so anyone else fails to resolve.
  const auto signerLookup = [userCert] (const ndn::Name& name, ndn::security::Certificate& out) {
    if (name != userCert.getName())
      return false;
    out = userCert;
    return true;
  };
  const auto materialLookup = [&material] (uint64_t generation, uint64_t epoch,
                                           HistoricalKeyBroker::Material& out) {
    if (generation != 1002 || epoch != 3)
      return false;
    out = material;
    return true;
  };
  const auto replySigner = [&controller, controllerCert] (ndn::Data& data) {
    controller.sign(data, ndn::security::signingByCertificate(controllerCert));
  };
  HistoricalKeyBroker broker(signerLookup, materialLookup, replySigner);

  const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count());
  ndn::Name dkeyPrefix(controllerIdentity);
  dkeyPrefix.append("NDNSF").append("HISTORY").append("DKEY");
  auto handle = face->setInterestFilter(dkeyPrefix,
    [face = face.get(), &broker, controllerIdentity, now]
    (const ndn::InterestFilter&, const ndn::Interest& interest) {
      const auto reply = broker.answer(interest, controllerIdentity, now);
      if (reply)
        face->put(*reply);
    });
  face->processEvents(ndn::time::milliseconds(500));

  const auto fetch = [&face] (ndn::Interest& interest) {
    bool arrived = false;
    face->expressInterest(interest,
      [&arrived] (const ndn::Interest&, const ndn::Data&) { arrived = true; },
      [] (const ndn::Interest&, const ndn::lp::Nack&) {},
      [] (const ndn::Interest&) {});
    const auto deadline = ndn::time::steady_clock::now() + ndn::time::seconds(4);
    while (!arrived && ndn::time::steady_clock::now() < deadline)
      face->processEvents(ndn::time::milliseconds(100));
    return arrived;
  };

  HistoricalKeyRequest request;
  request.controllerPrefix = controllerIdentity;
  request.generation = 1002;
  request.epoch = 3;
  request.policy = "attr-a";
  request.recipientCertificate = userCert.getName();

  // Signed by someone who is not the named recipient.
  auto stranger = request;
  stranger.mintedAtMs = now;
  stranger.expiresAtMs = now + 60000;
  ndn::Interest strangerInterest;
  signHistoricalKeyRequest(strangerInterest, stranger, controller,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      controllerCert.getName()));
  strangerInterest.setInterestLifetime(ndn::time::milliseconds(3000));
  BOOST_CHECK(!fetch(strangerInterest));

  // Signed by the recipient, but already expired.
  auto expired = request;
  expired.mintedAtMs = now - 120000;
  expired.expiresAtMs = now - 60000;
  ndn::Interest expiredInterest;
  signHistoricalKeyRequest(expiredInterest, expired, requester,
                           ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_CERT,
                                                      userCert.getName()));
  expiredInterest.setInterestLifetime(ndn::time::milliseconds(3000));
  BOOST_CHECK(!fetch(expiredInterest));
}

BOOST_AUTO_TEST_CASE(ArchivedResponderAnswersARealInterest)
{
  // Behavioural check of the responder registration shape. It needs a running forwarder; without
  // one the case reports and returns instead of pretending to have validated the exchange.
  std::unique_ptr<ndn::Face> face;
  try {
    face = std::make_unique<ndn::Face>();
    face->processEvents(ndn::time::milliseconds(200));
  }
  catch (const std::exception& error) {
    BOOST_TEST_MESSAGE("no forwarder available, exchange not attempted: " << error.what());
    return;
  }

  const ndn::Name identity("/controller/a");
  ControllerGenerationStore::AbeHistoryEntry entry;
  entry.generationTimestamp = 1002;
  entry.epoch = 3;
  entry.publicParameters = std::string("wire\0", 5);
  entry.masterSecret = "secret";
  entry.publicParametersWireDigest = "sha256:" + digestHexOf(entry.publicParameters);
  entry.publicParametersName = "/controller/a/PUBPARAMS/KP-ABE/v=1002003";

  const auto sign = [] (ndn::Data& data) { data.setSignatureInfo(ndn::SignatureInfo()); };
  const auto answers = collectHistoricalPublicParameterAnswers({entry}, identity, sign);
  BOOST_REQUIRE_EQUAL(answers.size(), 1u);
  auto published = answers.front();

  auto handle = face->setInterestFilter(published->getName(),
    [face = face.get(), published] (const ndn::InterestFilter&, const ndn::Interest&) {
      face->put(*published);
    });
  face->processEvents(ndn::time::milliseconds(500));

  ndn::Data received;
  bool arrived = false;
  ndn::Interest interest(published->getName());
  interest.setInterestLifetime(ndn::time::milliseconds(1500));
  interest.setMustBeFresh(false);
  face->expressInterest(interest,
    [&received, &arrived] (const ndn::Interest&, const ndn::Data& data) {
      received = data;
      arrived = true;
    },
    [] (const ndn::Interest&, const ndn::lp::Nack&) {},
    [] (const ndn::Interest&) {});
  const auto deadline = ndn::time::steady_clock::now() + ndn::time::seconds(3);
  while (!arrived && ndn::time::steady_clock::now() < deadline)
    face->processEvents(ndn::time::milliseconds(100));

  BOOST_REQUIRE(arrived);
  BOOST_CHECK(received.getName() == published->getName());
  // The answer is a bound envelope, not the original packet: the original wire stays untouched.
  const std::string receivedContent(reinterpret_cast<const char*>(received.getContent().value()),
                                     received.getContent().value_size());
  BOOST_CHECK(receivedContent.rfind("NDNSF-HIST-PUBPARAMS-v1\n", 0) == 0);
  BOOST_CHECK(receivedContent.find("original /controller/a/PUBPARAMS/KP-ABE/v=1002003") !=
              std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
