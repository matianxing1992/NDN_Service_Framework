#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/util/scope.hpp>
#include <boost/test/unit_test.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

struct OwnerCliFixture
{
  fs::path root;
  fs::path binary;

  OwnerCliFixture()
  {
    const auto* configured = std::getenv("NDNSF_REPO_OWNER_DELETE_BIN");
    BOOST_REQUIRE_MESSAGE(configured != nullptr, "provide the exact candidate executable");
    binary = configured;
    BOOST_REQUIRE(binary.is_absolute() && fs::is_regular_file(binary));
    auto pattern = (fs::temp_directory_path() / "spec222-owner-cli-XXXXXX").string();
    const auto* directory = ::mkdtemp(pattern.data());
    BOOST_REQUIRE(directory != nullptr);
    root = directory;
  }

  ~OwnerCliFixture()
  {
    std::error_code error;
    fs::remove_all(root, error);
  }

  fs::path
  write(const std::string& name, const std::string& bytes)
  {
    const auto path = root / name;
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    output.close();
    return path;
  }

  void
  rejects(const fs::path& input, const std::string& expected,
          const std::string& owner = "/owner")
  {
    std::vector<std::string> arguments{binary.string(), "--group-prefix", "/group",
      "--owner-identity", owner, "--controller-identity", "/controller",
      "--controller-certificate", (root / "no-certificate").string(),
      "--trust-schema", (root / "no-schema").string(), "--repo-node", "/logical/repo",
      "--provider-identity", "/producer/repo", "--service-prefix", "/repo-service",
      "--request-file", input.string(), "--timeout-ms", "1000"};
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto home = (root / "credentials-not-created").string();
    const auto pib = "pib-sqlite3:" + home + "/.ndn";
    const auto transport = "unix://" + (root / "absent.sock").string();
    const auto log = (root / "child.log").string();
    const auto pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
      ::setenv("HOME", home.c_str(), 1);
      ::setenv("NDN_CLIENT_PIB", pib.c_str(), 1);
      ::setenv("NDN_CLIENT_TPM", "tpm-file:", 1);
      ::setenv("NDN_CLIENT_TRANSPORT", transport.c_str(), 1);
      ::setenv("NDN_LOG", "*=NONE", 1);
      const int descriptor = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (descriptor < 0) ::_exit(126);
      ::dup2(descriptor, STDOUT_FILENO);
      ::dup2(descriptor, STDERR_FILENO);
      ::close(descriptor);
      ::execv(argv.front(), argv.data());
      ::_exit(127);
    }
    bool reaped = false;
    const auto cleanup = ndn::make_scope_exit([&] {
      if (!reaped) {
        ::kill(pid, SIGKILL);
        int ignored = 0;
        (void)::waitpid(pid, &ignored, 0);
      }
    });
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        reaped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_REQUIRE_MESSAGE(reaped, "invalid input blocked instead of terminating");
    BOOST_REQUIRE(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), 1);
    std::ifstream messages(log);
    const std::string text((std::istreambuf_iterator<char>(messages)),
                            std::istreambuf_iterator<char>());
    BOOST_CHECK_MESSAGE(text.find(expected) != std::string::npos, text);
    // Loaded SDKs can create an empty PIB even for --help, before main().
    // Check actual credentials rather than mistaking an empty directory for a key.
    const auto database = fs::path(home) / ".ndn/pib.db";
    if (fs::exists(database)) {
      sqlite3* raw = nullptr;
      const auto opened = ::sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
      std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
      BOOST_REQUIRE_EQUAL(opened, SQLITE_OK);
      for (const auto* table : {"identities", "keys", "certificates"}) {
        sqlite3_stmt* statement = nullptr;
        const auto sql = std::string("SELECT COUNT(*) FROM ") + table;
        const auto prepared = ::sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &statement, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
        BOOST_REQUIRE_EQUAL(prepared, SQLITE_OK);
        BOOST_REQUIRE_EQUAL(::sqlite3_step(query.get()), SQLITE_ROW);
        BOOST_CHECK_EQUAL(::sqlite3_column_int64(query.get(), 0), 0);
      }
    }
    const auto privateKeys = fs::path(home) / ".ndn/ndnsec-key-file";
    if (fs::exists(privateKeys)) {
      BOOST_CHECK(!fs::is_symlink(privateKeys));
      BOOST_CHECK(fs::is_empty(privateKeys));
    }
  }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(RepoOwnerDeleteCli, OwnerCliFixture)

BOOST_AUTO_TEST_CASE(RejectsInvalidFilesAndRequestsWithoutCreatingIdentities)
{
  const auto valid = ndnsf_distributed_repo::RepoUserStorageDeleteRequest{
    1, "cleanup", "/owner/object", 1}.toJson();
  std::string invalidFields;
  try {
    (void)ndnsf_distributed_repo::RepoUserStorageDeleteRequest::fromJson("{}");
  }
  catch (const std::exception& error) {
    invalidFields = error.what();
  }
  BOOST_REQUIRE(!invalidFields.empty());
  rejects(root / "missing", "request file is unavailable");
  rejects(write("invalid.json", "{}"), invalidFields);
  rejects(write("oversize.json", std::string(16385, 'x')), "bounded regular file");
  const auto regular = write("valid.json", valid);
  rejects(regular, "actor and group Names must be non-root", "/");
  const auto link = root / "link";
  fs::create_symlink(regular, link);
  rejects(link, "request file is unavailable");
  const auto fifo = root / "fifo";
  BOOST_REQUIRE_EQUAL(::mkfifo(fifo.c_str(), 0600), 0);
  rejects(fifo, "bounded regular file");
  rejects(root, "bounded regular file");
}

BOOST_AUTO_TEST_SUITE_END()
