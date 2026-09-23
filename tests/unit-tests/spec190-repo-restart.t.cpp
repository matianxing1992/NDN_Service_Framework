#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"

#include <boost/test/unit_test.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace ndnsf_distributed_repo;
namespace fs = std::filesystem;

std::vector<uint8_t>
makeBytes(size_t size, uint8_t seed)
{
  std::vector<uint8_t> result(size);
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<uint8_t>(seed + (i * 29U) % 251U);
  return result;
}

RepoObjectManifest
makeManifest(const std::string& objectName,
             const std::vector<uint8_t>& payload,
             uint64_t generation = 1)
{
  RepoObjectManifest manifest;
  manifest.objectName = objectName;
  manifest.objectType = "spec190-repo-restart";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.generation = generation;
  manifest.policyEpoch = "spec190-node-epoch";
  manifest.operationId = "spec190-repo-restart-" + objectName;
  return manifest;
}

void
removeTree(const fs::path& root)
{
  std::error_code error;
  fs::remove_all(root, error);
  if (error)
    throw std::runtime_error("repo restart fixture cleanup failed: " + error.message());
  fs::remove(root.string() + ".authority.lock", error);
}

int
runChild(const std::string& label, const std::function<void()>& body)
{
  const pid_t child = ::fork();
  if (child < 0)
    throw std::runtime_error("fork failed for " + label + ": " + std::strerror(errno));
  if (child == 0) {
    try {
      body();
      ::_exit(0);
    }
    catch (const std::exception& error) {
      std::cerr << "Spec190RepoRestart child " << label << " failed: "
                << error.what() << std::endl;
      ::_exit(111);
    }
    catch (...) {
      std::cerr << "Spec190RepoRestart child " << label
                << " failed with an unknown exception" << std::endl;
      ::_exit(112);
    }
  }

  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("waitpid failed for " + label + ": " +
                               std::strerror(errno));
  }
  if (!WIFEXITED(status))
    throw std::runtime_error(label + " did not exit normally");
  return WEXITSTATUS(status);
}

struct Fixture
{
  fs::path root = fs::temp_directory_path() /
    ("spec190-repo-restart-" + std::to_string(::getpid()));

  Fixture()
  {
    removeTree(root);
    fs::create_directories(root);
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace);
  }

  ~Fixture()
  {
    removeTree(root);
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190RepoRestart)

BOOST_AUTO_TEST_CASE(StableNodeRootSurvivesThreeProcessRestarts)
{
  Fixture fixture;
  const auto graph = makeBytes(96, 0x11);
  const auto layer = makeBytes(256, 0x23);
  const auto later = makeBytes(80, 0x37);
  const auto half = makeBytes(192, 0x49);
  const auto graphManifest = makeManifest("/spec190/node/graph", graph);
  const auto layerManifest = makeManifest("/spec190/node/layer/0", layer);
  const auto laterManifest = makeManifest("/spec190/node/later", later);
  const auto halfManifest = makeManifest("/spec190/node/half", half);
  const std::string owner = "deployment-spec190/node-ucla";

  const auto firstStatus = runChild("restart-1", [&] {
    FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, owner);
    backend.put(graphManifest, graph);
    for (size_t offset = 0; offset < layer.size(); offset += 128) {
      backend.putRange(layerManifest, {offset, 128},
                       std::vector<uint8_t>(layer.begin() + offset,
                                            layer.begin() + offset + 128));
    }
    backend.commitRanges(layerManifest);

    // Simulate a process crash after a verified range write but before the
    // manifest commit.  _exit intentionally skips C++ destructors.
    backend.putRange(halfManifest, {0, 64},
                     std::vector<uint8_t>(half.begin(), half.begin() + 64));
    if (!backend.has(graphManifest.objectName) ||
        !backend.has(layerManifest.objectName) ||
        backend.has(halfManifest.objectName))
      throw std::runtime_error("first process observed an invalid visibility boundary");
  });
  BOOST_REQUIRE_EQUAL(firstStatus, 0);

  const auto secondStatus = runChild("restart-2", [&] {
    FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, owner);
    if (backend.get(graphManifest.objectName).payload != graph ||
        backend.getRange(layerManifest.objectName, {64, 64}) !=
          std::vector<uint8_t>(layer.begin() + 64, layer.begin() + 128) ||
        backend.has(halfManifest.objectName))
      throw std::runtime_error("restart recovery changed committed or partial visibility");
    backend.put(laterManifest, later);
  });
  BOOST_REQUIRE_EQUAL(secondStatus, 0);

  const auto thirdStatus = runChild("restart-3", [&] {
    FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, owner);
    if (backend.get(graphManifest.objectName).payload != graph ||
        backend.get(laterManifest.objectName).payload != later ||
        backend.getRange(layerManifest.objectName, {192, 64}) !=
          std::vector<uint8_t>(layer.begin() + 192, layer.end()) ||
        backend.has(halfManifest.objectName) || backend.listManifests().size() != 3)
      throw std::runtime_error("third process could not read the complete committed catalog");
  });
  BOOST_REQUIRE_EQUAL(thirdStatus, 0);

  {
    FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, owner);
    BOOST_REQUIRE_EQUAL(backend.usedBytes(),
                        static_cast<uint64_t>(graph.size() + layer.size() + later.size()));
    BOOST_REQUIRE(backend.get(graphManifest.objectName).payload == graph);
    BOOST_REQUIRE(backend.get(laterManifest.objectName).payload == later);
    BOOST_REQUIRE(!backend.has(halfManifest.objectName));
  }
}

BOOST_AUTO_TEST_CASE(SecondOwnerIsBusyAndCannotMutateStableRoot)
{
  Fixture fixture;
  const auto payload = makeBytes(48, 0x61);
  const auto manifest = makeManifest("/spec190/node/owner-check", payload);
  FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, "owner-a");
  backend.put(manifest, payload);

  const auto status = runChild("second-owner", [&] {
    bool busy = false;
    try {
      FilesystemRepoStoreBackend other(fixture.root.string(), 128, 128, "owner-b");
    }
    catch (const std::runtime_error& error) {
      busy = std::string(error.what()).find("repo-persistence-owned") !=
             std::string::npos;
    }
    if (!busy)
      throw std::runtime_error("second owner acquired or obscured the active lock");
  });
  BOOST_REQUIRE_EQUAL(status, 0);
  BOOST_REQUIRE(backend.get(manifest.objectName).payload == payload);
}

BOOST_AUTO_TEST_CASE(CorruptCatalogFailsClosedAndRootSymlinkIsRejected)
{
  Fixture fixture;
  const auto payload = makeBytes(32, 0x73);
  const auto manifest = makeManifest("/spec190/node/corrupt-check", payload);
  {
    FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, "owner");
    backend.put(manifest, payload);
  }

  fs::create_directories(fixture.root / "payloads" / "staging");
  std::ofstream(fixture.root / "payloads" / "staging" / "foreign.partial") << "uncommitted";
  std::ofstream(fixture.root / "manifests" / "corrupt.json") << "{not-json";
  {
    FilesystemRepoStoreBackend backend(fixture.root.string(), 128, 128, "owner");
    BOOST_REQUIRE(backend.get(manifest.objectName).payload == payload);
    BOOST_REQUIRE(fs::exists(fixture.root / "payloads" / "staging" / "foreign.partial"));
    BOOST_REQUIRE(fs::exists(fixture.root / "manifests" / "corrupt.json"));
  }

  const auto alias = fixture.root.parent_path() /
    (fixture.root.filename().string() + "-alias");
  const auto outside = fixture.root.parent_path() /
    (fixture.root.filename().string() + "-outside");
  removeTree(alias);
  removeTree(outside);
  fs::create_directories(outside);
  fs::create_directory_symlink(outside, alias);
  bool rejected = false;
  try {
    FilesystemRepoStoreBackend escaped(alias.string(), 128, 128, "escape");
  }
  catch (const std::runtime_error& error) {
    rejected = std::string(error.what()).find("repo-file-root-invalid") !=
               std::string::npos;
  }
  BOOST_REQUIRE(rejected);
  removeTree(alias);
  removeTree(outside);
}

BOOST_AUTO_TEST_CASE(LogicalQuotaRejectsBeforePayloadMutation)
{
  Fixture fixture;
  StorageCapability capability;
  capability.repoNode = "/spec190/node/quota";
  capability.freeBytes = 16;
  auto store = makeFilesystemRepoStore(fixture.root.string(), 128, 128, "quota-owner");
  RepoCore repo(capability, std::move(store));
  const auto payload = makeBytes(32, 0x85);
  bool rejected = false;
  try {
    (void)repo.put("/spec190/node/quota/object", payload, "spec190-repo-restart");
  }
  catch (const std::runtime_error& error) {
    rejected = std::string(error.what()).find("insufficient free space") !=
               std::string::npos;
  }
  BOOST_REQUIRE(rejected);
  BOOST_CHECK(!repo.has("/spec190/node/quota/object"));
}

BOOST_AUTO_TEST_SUITE_END()
