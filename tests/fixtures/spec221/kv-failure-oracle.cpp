#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <ndn-cxx/name.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using ndnsf::di::NativeJson;

void
require(bool condition, const char* reason)
{
  if (!condition)
    throw std::runtime_error(reason);
}

NativeJson
readJson(const fs::path& path)
{
  require(fs::is_regular_file(path) && fs::file_size(path) <= 4U * 1024U * 1024U,
          "oracle input is missing or exceeds its bound");
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "oracle input is unreadable");
  return ndnsf::di::nativeParseJson(
    std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
}

const NativeJson&
actor(const NativeJson& rows, const std::string& id)
{
  const NativeJson* result = nullptr;
  for (const auto& row : rows) {
    if (row.at("id") == id) {
      require(!result, "duplicate actor identity");
      result = &row;
    }
  }
  require(result != nullptr, "required actor is missing");
  return *result;
}

std::string
argument(const NativeJson& row, const std::string& flag)
{
  const auto& argv = row.at("argv");
  std::string result;
  for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == flag) {
      require(result.empty(), "duplicate actor argument");
      result = argv[i + 1].get<std::string>();
    }
  }
  require(!result.empty(), "required actor argument is missing");
  return result;
}

NativeJson
verify(const fs::path& configPath, const fs::path& statePath, const std::string& mode,
       const fs::path& receiptPath)
{
  require(mode == "precommit" || mode == "partial" || mode == "drain" || mode == "commit-drain",
          "invalid oracle mode");
  const auto config = readJson(configPath);
  const auto state = readJson(statePath);
  require(config.at("schema") == "ndnsf-di-deployment-v1" &&
          state.at("schema") == "ndnsf-di-host-operation-v1", "unexpected deployment schema");
  require(state.at("status") == "STOPPED" && state.at("execution") == "FAILED",
          "negative fixture requires a stopped failed execution");
  require(ndnsf::di::nativePlanningDigest(ndnsf::di::nativeCanonicalJson(config)) ==
          "sha256:" + state.at("configuration_sha256").get<std::string>(),
          "configuration identity mismatch");
  for (const auto& cleanup : state.at("cleanup"))
    require(cleanup.at("status") == "ZERO" && !cleanup.at("forced").get<bool>(),
            "forced or incomplete process cleanup");

  // Positive fault ordering supplements the journal/ledger assertions below.
  const auto receipt = readJson(receiptPath);
  std::string faultSource;
  bool armed = false;
  bool boundary = false;
  bool injected = false;
  bool remainedOpen = false;
  bool pausedBeforeReady = false;
  bool repoCrash = false;
  std::string failedSource;
  double previous = -1;
  for (const auto& event : receipt.at("events")) {
    const auto elapsed = event.at("elapsed_s").get<double>();
    require(elapsed >= previous, "fault receipt ordering is invalid");
    previous = elapsed;
    const auto name = event.at("event").get<std::string>();
    if (name == "SOURCE_PAUSED_BEFORE_READY")
      pausedBeforeReady = true;
    if (name == "REPO_CRASH_INJECTED") {
      require(mode == "partial" && injected, "Repo crash precedes its durable fault boundary");
      repoCrash = true;
    }
    if (name == "FAULT_ARMED") {
      require(!armed && event.at("mode") == mode &&
              event.at("boot_id") == state.at("boot_id") &&
              event.at("configuration_sha256") == state.at("configuration_sha256"),
              "fault is not bound to this deployment");
      require(mode == "partial" || mode == "commit-drain" || pausedBeforeReady,
              "pre-READY Source pause evidence is missing");
      faultSource = event.at("source").get<std::string>();
      require(faultSource == "provider-0" || faultSource == "provider-1",
              "fault Source is not an original role");
      armed = true;
    }
    if ((mode == "precommit" && name == "READY_SOURCE_REQUESTER_CANCEL") ||
        (mode == "partial" && name == "ONE_SOURCE_DURABLE") ||
        (mode == "drain" && name == "ACTIVE_SOURCE_CLOSE_REQUESTED") ||
        (mode == "commit-drain" && name == "ACTIVE_COMMITTED_SOURCE_CLOSE_REQUESTED")) {
      require(armed && event.at("source") == faultSource, "fault boundary precedes arming");
      boundary = true;
    }
    if ((mode == "precommit" && name == "NATIVE_CANCEL_ACCEPTED") ||
        (mode == "partial" && name == "REPO_PAUSED_AFTER_ONE_COMMIT") ||
        (name == "SOURCE_OPERATOR_STOPPED" && (mode == "drain" || mode == "commit-drain") &&
          event.at("actor") == faultSource)) {
      require(boundary, "fault was injected before its required native boundary");
      injected = true;
    }
    if (name == "FAILED_WITHDRAWAL_REMAINED_OPEN") {
      require(injected, "withdrawal evidence precedes the fault");
      remainedOpen = true;
    }
    if (name == "SOURCE_REPO_SAVE_FAILED") {
      require(mode == "partial" && injected, "Source failure precedes Repo outage");
      failedSource = event.at("source").get<std::string>();
      require((failedSource == "provider-0" || failedSource == "provider-1") &&
              failedSource != faultSource, "failure is not bound to the other Source");
    }
  }
  require(armed && boundary && injected &&
          (mode != "partial" || (remainedOpen && repoCrash && !failedSource.empty())),
          "required positive fault evidence is missing");
  const auto& sourceLaunch = actor(state.at("launches"), faultSource);
  const fs::path sourceLog(sourceLaunch.at("log").get<std::string>());
  require(fs::is_regular_file(sourceLog) && fs::file_size(sourceLog) <= 128U * 1024U * 1024U,
          "native Source phase log is missing or exceeds its bound");
  std::ifstream sourceInput(sourceLog);
  std::string line;
  bool phaseFound = false;
  const std::string phase = mode == "partial" ? "COMMIT_ACCEPTED" :
                            mode == "commit-drain" ? "SAVED" : "BOUNDARY_READY";
  bool shutdownSeen = false;
  while (std::getline(sourceInput, line)) {
    shutdownSeen = shutdownSeen ||
      line.find("NDNSF_DI_NATIVE_PROVIDER_SHUTDOWN_REQUESTED") != std::string::npos;
    if (line.find("NDNSF_DI_HANDOFF_PHASE phase=" + phase + " ") != std::string::npos) {
      require(mode != "commit-drain" || shutdownSeen,
              "Source had already saved before the tested shutdown");
      phaseFound = true;
    }
  }
  require(phaseFound, "required actual Source phase is absent");
  if (mode == "partial") {
    const fs::path failedLog(actor(state.at("launches"), failedSource).at("log")
      .get<std::string>());
    require(fs::is_regular_file(failedLog) && fs::file_size(failedLog) <= 128U * 1024U * 1024U,
            "failed Source log is missing or exceeds its bound");
    std::ifstream failedInput(failedLog);
    bool failureFound = false;
    while (std::getline(failedInput, line))
      failureFound = failureFound ||
        line.find("NDNSF_DI_HANDOFF_REPO_PUBLICATION_FAILED ") != std::string::npos ||
        line.find("NDNSF_DI_HANDOFF_SAVE_FAILED stage=PUBLICATION_COMPLETE ") != std::string::npos;
    require(failureFound, "actual other-Source publication failure is absent");
  }

  const auto& launch = actor(state.at("launches"), "requester");
  const fs::path requesterPath(argument(launch, "--config"));
  const auto requester = readJson(requesterPath);
  const auto ownerName = config.at("requester").at("core").at("requester_identity")
    .get<std::string>();
  const auto cold = readJson(requester.at("turns").at(0).at("checkpoint_output_file")
    .get<std::string>());
  const auto coldOutput = readJson(requester.at("turns").at(0).at("output_file")
    .get<std::string>());
  require(coldOutput.at("tokenIds").size() == 32, "initial inference did not complete 32 tokens");
  auto conversations = ndnsf::di::nativeConversationCoordinatorFromConfig(
    ndnsf::di::nativeCanonicalJson(requester.at("conversation")), requesterPath.parent_path(),
    ownerName);
  const auto head = conversations->find(cold.at("conversationId").get<std::string>());
  require(head && head->checkpoint.checkpointDigest == cold.at("checkpointDigest").get<std::string>(),
          "Owner checkpoint changed or is unavailable");
  require(!head->protectedKvReferences && head->nextTurnProvidersByRole.empty(),
          "failed save promoted protected state or placement");
  std::set<std::string> expectedSources;
  for (const auto& source : config.at("requester").at("request").at("provider_names"))
    expectedSources.insert(ndn::Name(source.get<std::string>())
      .toUri(ndn::name::UriFormat::ALTERNATE));
  require(expectedSources.size() == 2 && head->checkpoint.providersByRole.size() == 2,
          "fixture requires both actual Source roles");
  std::set<std::string> actualSources;
  for (const auto& role : head->checkpoint.providersByRole)
    actualSources.insert(ndn::Name(role.second).toUri(ndn::name::UriFormat::ALTERNATE));
  require(actualSources == expectedSources, "Source placement changed after failure");
  std::size_t checkedSources = 0;
  for (const auto& row : config.at("actors")) {
    if (row.at("kind") == "provider" &&
        expectedSources.count(ndn::Name(row.at("identity").get<std::string>())
          .toUri(ndn::name::UriFormat::ALTERNATE))) {
      const auto& cleanup = actor(state.at("cleanup"), row.at("id").get<std::string>());
      require(cleanup.at("exitCode") == 0, "Source native close/drain did not exit normally");
      ++checkedSources;
    }
  }
  require(checkedSources == 2, "both Source cleanup records must be checked");

  std::string repoId;
  for (const auto& row : config.at("actors")) {
    if (row.at("kind") == "repo") {
      require(repoId.empty(), "fixture requires one authoritative Repo");
      repoId = row.at("id").get<std::string>();
    }
  }
  require(!repoId.empty(), "Repo is missing");
  const auto& repo = actor(state.at("launches"), repoId);
  const auto database = argument(repo, "--database") + ".artifact-v2.sqlite";
  const auto repoNode = argument(repo, "--repo-node");
  sqlite3* raw = nullptr;
  const auto opened = sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
  require(opened == SQLITE_OK, "cannot read the actual Repo ledger");
  const char* sql =
    "SELECT (SELECT count(*) FROM repo_user_storage_writes "
    "WHERE owner_identity=?1 AND repo_node=?2),"
    "(SELECT count(*) FROM repo_user_storage_writes "
    "WHERE owner_identity=?1 AND repo_node=?2 AND state='Committed'),"
    "(SELECT count(*) FROM repo_user_storage_claims "
    "WHERE owner_identity=?1 AND repo_node=?2 AND state='Committed'),"
    "(SELECT coalesce(sum(payload_wire_bytes),0) FROM repo_user_storage_claims "
    "WHERE owner_identity=?1 AND repo_node=?2 AND state='Committed')";
  sqlite3_stmt* rawStatement = nullptr;
  const auto prepared = sqlite3_prepare_v2(raw, sql, -1, &rawStatement, nullptr);
  const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
    rawStatement, sqlite3_finalize);
  require(prepared == SQLITE_OK && sqlite3_bind_text(rawStatement, 1, ownerName.c_str(), -1,
    SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_bind_text(rawStatement, 2, repoNode.c_str(), -1,
    SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(rawStatement) == SQLITE_ROW,
    "cannot measure the actual Owner/Repo transaction");
  const auto writes = sqlite3_column_int64(rawStatement, 0);
  const auto committed = sqlite3_column_int64(rawStatement, 1);
  const auto claims = sqlite3_column_int64(rawStatement, 2);
  const auto bytes = sqlite3_column_int64(rawStatement, 3);
  if (mode == "commit-drain")
    require(writes >= 1 && writes <= 2 && committed >= 1 && committed <= 2 &&
            claims == committed && bytes > 0,
            "healthy committed publication did not finish durably during drain");
  else if (mode == "partial")
    require(writes >= 1 && writes <= 2 && committed == 1 && claims == 1 && bytes > 0,
            "partial-save fault did not retain exactly one durable Source claim");
  else
    require(writes == 0 && committed == 0 && claims == 0 && bytes == 0,
            "pre-COMMIT cancellation or close crossed the INSERT boundary");
  return NativeJson{{"result", "NATIVE_KV_FAILURE_ORACLE_PASS"}, {"mode", mode},
    {"bootId", state.at("boot_id")}, {"configurationSha256", state.at("configuration_sha256")},
    {"checkpointDigest", head->checkpoint.checkpointDigest}, {"writes", writes},
    {"committedWrites", committed}, {"committedClaims", claims}, {"committedWireBytes", bytes}};
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    require(argc == 5, "usage: spec221-handoff-failure-oracle CONFIG STATE MODE RECEIPT");
    std::cout << ndnsf::di::nativeCanonicalJson(verify(argv[1], argv[2], argv[3], argv[4])) << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_KV_FAILURE_ORACLE_REJECTED: " << error.what() << '\n';
    return 1;
  }
}
