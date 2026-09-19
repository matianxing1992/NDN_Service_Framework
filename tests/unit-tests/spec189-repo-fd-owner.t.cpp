#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace ndnsf_distributed_repo;

std::atomic<int> g_fsyncCalls{0};
std::atomic<int> g_closeCalls{0};
std::atomic<int> g_failFsyncAt{0};
std::atomic<int> g_failCloseAt{0};
std::atomic<int> g_duplicateCloseCalls{0};
std::mutex g_fdMutex;
std::set<int> g_fsyncObservedFds;

int
scriptedFsync(int fd)
{
  {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    g_fsyncObservedFds.insert(fd);
  }
  const int call = ++g_fsyncCalls;
  if (g_failFsyncAt.load() == call) {
    errno = EIO;
    return -1;
  }
  return ::fsync(fd);
}

int
scriptedClose(int fd)
{
  const int call = ++g_closeCalls;
  {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    if (g_fsyncObservedFds.erase(fd) == 0)
      ++g_duplicateCloseCalls;
  }
  const int result = ::close(fd);
  if (g_failCloseAt.load() == call) {
    errno = EIO;
    return -1;
  }
  return result;
}

void
resetHooks()
{
  g_fsyncCalls = 0;
  g_closeCalls = 0;
  g_failFsyncAt = 0;
  g_failCloseAt = 0;
  g_duplicateCloseCalls = 0;
  std::lock_guard<std::mutex> lock(g_fdMutex);
  g_fsyncObservedFds.clear();
}

bool
allObservedFdsClosed()
{
  std::lock_guard<std::mutex> lock(g_fdMutex);
  return g_fsyncObservedFds.empty();
}

struct HookScope
{
  detail::FilesystemRepoStoreIoHooks previous;

  HookScope()
    : previous(detail::installFilesystemRepoStoreIoHooks({scriptedFsync, scriptedClose}))
  {
    resetHooks();
  }

  ~HookScope()
  {
    detail::installFilesystemRepoStoreIoHooks(previous);
  }
};

struct Fixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec189-fd-owner-" + std::to_string(::getpid()));

  Fixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
      throw std::runtime_error("unable to make fd fixture: " + error.message());
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error)
      throw std::runtime_error("unable to protect fd fixture: " + error.message());
  }

  ~Fixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove(root.string() + ".authority.lock", error);
  }
};

RepoObjectManifest
manifest(const std::string& objectName, const std::vector<std::uint8_t>& payload)
{
  RepoObjectManifest result;
  result.objectName = objectName;
  result.objectType = "spec189-fd-owner";
  result.size = payload.size();
  result.sha256 = sha256Hex(payload);
  result.generation = 0;
  return result;
}

std::vector<std::filesystem::path>
temporaryMetadata(const std::filesystem::path& root)
{
  std::vector<std::filesystem::path> result;
  const auto manifests = root / "manifests";
  if (!std::filesystem::exists(manifests))
    return result;
  for (const auto& item : std::filesystem::directory_iterator(manifests)) {
    if (item.path().filename().string().find(".tmp.") != std::string::npos)
      result.push_back(item.path());
  }
  return result;
}

std::shared_ptr<FilesystemRepoStoreBackend>
makeBackend(const Fixture& fixture)
{
  return std::make_shared<FilesystemRepoStoreBackend>(
    fixture.root.string(), 4U * 1024U * 1024U, 1U * 1024U * 1024U,
    "spec189-fd-owner");
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec189FilesystemRepoFdOwner)

BOOST_AUTO_TEST_CASE(FsyncFailureClosesOnceAndPreservesOldManifest)
{
  Fixture fixture;
  const std::vector<std::uint8_t> oldPayload{0x01, 0x02};
  const std::vector<std::uint8_t> newPayload{0x03, 0x04, 0x05};
  auto backend = makeBackend(fixture);
  backend->put(manifest("/fd/object", oldPayload), oldPayload);

  HookScope hooks;
  g_failFsyncAt = 1;
  BOOST_CHECK_THROW(backend->put(manifest("/fd/object", newPayload), newPayload),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(g_closeCalls.load(), 2);
  BOOST_CHECK_EQUAL(g_duplicateCloseCalls.load(), 0);
  BOOST_CHECK(allObservedFdsClosed());
  const auto restored = backend->get("/fd/object").payload;
  BOOST_CHECK_EQUAL_COLLECTIONS(restored.begin(), restored.end(),
                                oldPayload.begin(), oldPayload.end());
  BOOST_CHECK(temporaryMetadata(fixture.root).empty());
}

BOOST_AUTO_TEST_CASE(CloseFailureTransfersOwnershipAndDoesNotRetry)
{
  Fixture fixture;
  const std::vector<std::uint8_t> oldPayload{0x11, 0x12};
  const std::vector<std::uint8_t> newPayload{0x13, 0x14, 0x15};
  auto backend = makeBackend(fixture);
  backend->put(manifest("/fd/object", oldPayload), oldPayload);

  HookScope hooks;
  g_failCloseAt = 1;
  BOOST_CHECK_THROW(backend->put(manifest("/fd/object", newPayload), newPayload),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(g_closeCalls.load(), 2);
  BOOST_CHECK_EQUAL(g_duplicateCloseCalls.load(), 0);
  BOOST_CHECK(allObservedFdsClosed());
  const auto restored = backend->get("/fd/object").payload;
  BOOST_CHECK_EQUAL_COLLECTIONS(restored.begin(), restored.end(),
                                oldPayload.begin(), oldPayload.end());
  BOOST_CHECK(temporaryMetadata(fixture.root).empty());
}

BOOST_AUTO_TEST_CASE(DirectoryFsyncFailureIsAmbiguousAfterNewManifestIsVisible)
{
  Fixture fixture;
  const std::vector<std::uint8_t> oldPayload{0x21, 0x22};
  const std::vector<std::uint8_t> newPayload{0x23, 0x24, 0x25};
  auto backend = makeBackend(fixture);
  backend->put(manifest("/fd/object", oldPayload), oldPayload);

  HookScope hooks;
  g_failFsyncAt = 2; // metadata fsync succeeds; directory fsync fails.
  try {
    backend->put(manifest("/fd/object", newPayload), newPayload);
    BOOST_FAIL("directory fsync failure must be reported");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_NE(std::string(error.what()).find("ambiguous-commit"), std::string::npos);
  }
  const auto restored = backend->get("/fd/object").payload;
  BOOST_CHECK_EQUAL_COLLECTIONS(restored.begin(), restored.end(),
                                newPayload.begin(), newPayload.end());
  BOOST_CHECK_EQUAL(g_closeCalls.load(), 2);
  BOOST_CHECK_EQUAL(g_duplicateCloseCalls.load(), 0);
  BOOST_CHECK(allObservedFdsClosed());
  BOOST_CHECK(temporaryMetadata(fixture.root).empty());
}

BOOST_AUTO_TEST_SUITE_END()
