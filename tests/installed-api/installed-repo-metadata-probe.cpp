#include <ndnsf-distributed-repo/ArtifactRepositoryApi.hpp>
#include <ndnsf-distributed-repo/RepoTypes.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

int
main(int argc, char** argv)
{
  const auto started = std::chrono::steady_clock::now();
  const auto cpuStarted = std::clock();
  try {
    if (argc != 3)
      throw std::invalid_argument(
        "usage: installed-repo-metadata-probe ROOT publish|read|open|fetch-catalog");
    const std::filesystem::path root(argv[1]);
    const std::string action(argv[2]);
    if (!root.is_absolute() || root.lexically_normal() != root ||
        (action != "publish" && action != "read" && action != "open" &&
         action != "fetch-catalog"))
      throw std::invalid_argument("canonical absolute root and supported action required");
    namespace repo = ndnsf_distributed_repo;
    std::vector<repo::ArtifactReference> catalog;
    if (action == "fetch-catalog") {
      sqlite3* database = nullptr;
      const auto databasePath = (root / "metadata.sqlite3").string();
      if (sqlite3_open_v2(databasePath.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
          SQLITE_OK) {
        const std::string error = database ? sqlite3_errmsg(database) : "allocation failed";
        if (database)
          sqlite3_close(database);
        throw std::runtime_error("catalog open failed: " + error);
      }
      sqlite3_stmt* statement = nullptr;
      const char* query = "SELECT logical_name,digest_algorithm,content_digest,size_bytes,"
        "format_version,root_manifest_name,publisher_identity,policy_epoch "
        "FROM artifact_local_commits LIMIT 5";
      int status = sqlite3_prepare_v2(database, query, -1, &statement, nullptr);
      if (status == SQLITE_OK) {
        while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
          const auto text = [&] (int column) {
            const auto* value = sqlite3_column_text(statement, column);
            return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
          };
          catalog.push_back({text(0), text(1), text(2),
            static_cast<uint64_t>(sqlite3_column_int64(statement, 3)),
            text(4), text(5), text(6), text(7)});
        }
      }
      const std::string error = sqlite3_errmsg(database);
      sqlite3_finalize(statement);
      sqlite3_close(database);
      if (status != SQLITE_DONE || catalog.empty() || catalog.size() > 4)
        throw std::runtime_error("bounded catalog read failed: " + error);
    }
    const std::string producer = "/ndnsf/di/diagnostic/metadata";
    repo::InAppRepoConfig config;
    config.durableRoot = root;
    config.ownerId = producer;
    auto api = repo::ArtifactRepositoryApi::makeInApp(std::move(config));
    if (action == "fetch-catalog") {
      for (const auto& reference : catalog) {
        reference.validate();
        if (reference.sizeBytes > 3ULL * 1024 * 1024 * 1024)
          throw std::runtime_error("catalog object exceeds diagnostic budget");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        if (!api.lookupLocalCommitUntil(reference, deadline))
          throw std::runtime_error("catalog receipt unavailable through native API");
        const auto destination = std::filesystem::temp_directory_path() /
          ("ndnsf-probe-" + reference.contentDigest);
        if (std::filesystem::exists(destination))
          throw std::runtime_error("diagnostic output already exists");
        try {
          api.fetchFileUntil(reference, destination.string(), deadline);
          if (std::filesystem::file_size(destination) != reference.sizeBytes)
            throw std::runtime_error("native fetched size mismatch");
          std::cout << "FETCH_PASS bytes=" << reference.sizeBytes << std::endl;
        }
        catch (...) {
          std::filesystem::remove(destination);
          throw;
        }
        std::filesystem::remove(destination);
      }
    }
    else if (action != "open") {
      const std::string bytes = "NDNSF_NATIVE_SQLITE_PROBE\n";
      const std::vector<uint8_t> payload(bytes.begin(), bytes.end());
      repo::ArtifactReference reference;
      reference.logicalName = producer + "/artifact/v1";
      reference.publisherIdentity = producer;
      reference.contentDigest = repo::sha256Hex(payload);
      reference.sizeBytes = payload.size();
      reference.policyEpoch = "plaintext-v1";
      reference.formatVersion = "raw-file-v1";
      const auto source = root.parent_path() / "probe-source.bin";
      const auto destination = root.parent_path() / ("probe-result-" + action + ".bin");
      if (action == "publish") {
        {
          std::ofstream output(source, std::ios::binary);
          output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
          output.close();
          if (!output)
            throw std::runtime_error("probe source write failed");
        }
        const auto published = api.publishFile(source.string(), reference);
        if (!published.handle || !published.receipt)
          throw std::runtime_error("native publication lacks receipt or handle");
      }
      if (!api.lookupLocalCommit(reference))
        throw std::runtime_error("native local commit missing after publication/reopen");
      api.fetchFile(reference, destination.string());
      std::ifstream input(destination, std::ios::binary);
      const std::string fetched{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
      if (!input.eof() && input.fail())
        throw std::runtime_error("probe result read failed");
      if (fetched != bytes)
        throw std::runtime_error("native artifact payload mismatch");
    }
    std::cout << "{\"status\":\"PASS\",\"action\":\"" << action
              << "\",\"wallSeconds\":" << std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - started).count()
              << ",\"cpuSeconds\":"
              << static_cast<double>(std::clock() - cpuStarted) / CLOCKS_PER_SEC
              << "}" << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_REPO_METADATA_PROBE_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
