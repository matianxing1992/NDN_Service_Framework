#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/model-repository-publication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/controller-certificate.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <ndn-cxx/encoding/tlv.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <boost/asio/signal_set.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
namespace di = ndnsf::di;
namespace repo = ndnsf_distributed_repo;
using Clock = std::chrono::steady_clock;
using Json = di::NativeJson;
constexpr uint64_t FILE_BYTES = 500000000;
constexpr size_t CONTENT_BYTES = 7000;

void
require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

std::string
digest(ndn::util::Sha256& hash)
{
  auto value = hash.toString();
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

struct Measurement
{
  Clock::time_point wall = Clock::now();
  std::clock_t cpu = std::clock();
  Json result() const
  {
    return Json{{"wallSeconds", std::chrono::duration<double>(Clock::now() - wall).count()},
                {"startMonotonicNs", std::chrono::duration_cast<std::chrono::nanoseconds>(
                  wall.time_since_epoch()).count()},
                {"endMonotonicNs", std::chrono::duration_cast<std::chrono::nanoseconds>(
                  Clock::now().time_since_epoch()).count()},
                {"processCpuSeconds", double(std::clock() - cpu) / CLOCKS_PER_SEC}};
  }
};

struct Group
{
  ndn::Name prefix;
  size_t packetCount = 0;
  std::string wireDigest;
  uint64_t payloadBytes = 0;
  uint64_t wireBytes = 0;
  uint64_t minimumWireBytes = ndn::MAX_NDN_PACKET_SIZE;
  uint64_t maximumWireBytes = 0;
};

int
run(int argc, char** argv)
{
  const std::set<std::string> required{"--group-prefix", "--user-identity",
    "--controller-identity", "--controller-certificate", "--trust-schema", "--repo-node",
    "--provider-identity", "--service-prefix", "--source", "--expected-sha256", "--report"};
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "ndnsf-repo-file-benchmark: native 500000000-byte original-packet file I/O\n";
    for (const auto& option : required) std::cout << option << " VALUE ";
    std::cout << "\nUses existing HOME/PIB/TPM credentials; bounded 4096-packet commits and "
                 "whole-group reads and at most "
              << di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS
              << " concurrent native commits. Deadline 1800 seconds. Report must not exist.\n";
    return 0;
  }
  std::map<std::string, std::string> options;
  for (int i = 1; i < argc; i += 2) {
    require(i + 1 < argc && required.count(argv[i]) &&
            options.emplace(argv[i], argv[i + 1]).second, "invalid benchmark option");
  }
  require(options.size() == required.size(), "missing benchmark option");
  const auto& source = options.at("--source");
  require(std::filesystem::is_regular_file(source) &&
          !std::filesystem::is_symlink(source) && std::filesystem::file_size(source) == FILE_BYTES,
          "benchmark input must be a regular 500000000-byte file");
  const auto expected = options.at("--expected-sha256");
  require(expected.size() == 64 && std::all_of(expected.begin(), expected.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }), "benchmark digest must be lowercase SHA-256");
  const int report = ::open(options.at("--report").c_str(),
    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  require(report >= 0, "cannot create exclusive benchmark report");
  const auto closeReport = ndn::make_scope_exit([report] { ::close(report); });
  std::mutex reportMutex;
  const auto record = [report, &reportMutex](Json value) {
    std::lock_guard<std::mutex> lock(reportMutex);
    const auto line = di::nativeCanonicalJson(value) + "\n";
    size_t written = 0;
    while (written < line.size()) {
      const auto count = ::write(report, line.data() + written, line.size() - written);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "benchmark report write failed");
      written += static_cast<size_t>(count);
    }
    require(::fsync(report) == 0, "benchmark report sync failed");
  };
  const Measurement total;
  const auto deadline = Clock::now() + std::chrono::seconds(1800);
  std::atomic<bool> cancelled{false};
  const auto current = [&] {
    require(!cancelled.load() && Clock::now() < deadline, "benchmark cancelled or expired");
  };
  ndn::Face face;
  ndn::KeyChain keys;
  const ndn::Name identity(options.at("--user-identity"));
  const auto producer = keys.getPib().getIdentity(identity);
  keys.setDefaultIdentity(producer);
  const auto certificate = producer.getDefaultKey().getDefaultCertificate();
  const ndn::Name controller(options.at("--controller-identity"));
  const auto controllerCertificate = ndn_service_framework::loadControllerCertificate(
    keys, controller, options.at("--controller-certificate").c_str());
  ndn_service_framework::CertificatePublisher certificates(face, keys, certificate.getName());
  auto user = std::make_shared<ndn_service_framework::ServiceUser>(face,
    ndn::Name(options.at("--group-prefix")), certificate, controllerCertificate,
    options.at("--trust-schema"));
  boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code& error, int) {
    if (!error) cancelled.store(true);
  });
  user->init();
  user->fetchPermissionsFromController(controller);
  const repo::RepoArtifactTarget target{options.at("--repo-node"),
    options.at("--provider-identity"), options.at("--service-prefix")};
  const auto service = repo::makeRepoServiceName(ndn::Name(target.servicePrefix),
                                                "USER_STORAGE_GRANT");
  while (!user->isRequestBootstrapReady(service, {ndn::Name(target.providerIdentity)})) {
    current();
    face.processEvents(ndn::time::milliseconds(10));
  }
  auto bootstrap = total.result();
  bootstrap["phase"] = "bootstrap";
  bootstrap["contentSegmentBytes"] = CONTENT_BYTES;
  bootstrap["publicationIncludesReadback"] = false;
  bootstrap["publicationMode"] = "bounded-pipeline";
  bootstrap["publicationMaxActiveCommits"] = di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS;
  record(std::move(bootstrap));
  auto work = std::async(std::launch::async, [&] {
    std::string phase = "publication";
    uint64_t payloadBytes = 0, wireBytes = 0;
    size_t groupIndex = 0;
    try {
      auto io = di::nativeStoredModelRepositoryIo(user, target, deadline, current, true);
      require(bool(io.commitConcurrent), "benchmark native concurrent commit unavailable");
      ndn::KeyChain packetKeys("pib-memory:repo-file-benchmark", "tpm-memory:repo-file-benchmark");
      ndn::Name publication(identity);
      publication.append("repo-file-benchmark").appendVersion(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      std::ifstream input(source, std::ios::binary);
      require(bool(input), "benchmark source open failed");
      ndn::util::Sha256 sourceHash;
      std::vector<Group> groups;
      const auto publicationRow = [&](const Measurement& measurement, const Group& group,
                                      size_t index, const char* rowPhase) {
        auto row = measurement.result();
        row.update(Json{{"phase", rowPhase}, {"group", index},
          {"payloadBytes", group.payloadBytes}, {"wireBytes", group.wireBytes},
          {"packets", group.packetCount}, {"originalWireSha256", group.wireDigest},
          {"objectName", ndn::Name(group.prefix).appendSegment(0).toUri(
            ndn::name::UriFormat::CANONICAL)},
          {"minimumWireBytes", group.minimumWireBytes},
          {"maximumWireBytes", group.maximumWireBytes},
          {"publicationMode", "bounded-pipeline"},
          {"publicationMaxActiveCommits", di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS},
          {"cpuScope", "process CPU during overlapping group interval"}});
        return row;
      };
      const Measurement publicationTime;
      di::NativeModelCommitPipeline pipeline(current);
      while (payloadBytes < FILE_BYTES) {
        const Measurement groupTime;
        Group group;
        group.prefix = ndn::Name(publication).append(std::to_string(groupIndex));
        di::NativeStoredModelPacketIo::PacketWires wires;
        ndn::util::Sha256 wireHash;
        const auto groupPackets = std::min<uint64_t>(di::NATIVE_STORED_MODEL_UPLOAD_PACKETS,
          (FILE_BYTES - payloadBytes + CONTENT_BYTES - 1) / CONTENT_BYTES);
        for (size_t segment = 0; segment < di::NATIVE_STORED_MODEL_UPLOAD_PACKETS &&
             payloadBytes < FILE_BYTES; ++segment) {
          current();
          std::array<uint8_t, CONTENT_BYTES> buffer;
          const auto count = static_cast<size_t>(std::min<uint64_t>(
            buffer.size(), FILE_BYTES - payloadBytes));
          input.read(reinterpret_cast<char*>(buffer.data()), count);
          require(static_cast<size_t>(input.gcount()) == count, "benchmark source truncated");
          const ndn::span<const uint8_t> content(buffer.data(), count);
          sourceHash.update(content);
          ndn::Data packet(ndn::Name(group.prefix).appendSegment(segment));
          packet.setContent(content);
          packet.setFinalBlock(ndn::name::Component::fromSegment(groupPackets - 1));
          packetKeys.sign(packet, ndn::security::signingWithSha256());
          const auto& wire = packet.wireEncode();
          wires.emplace_back(wire.begin(), wire.end());
          wireHash.update(ndn::span<const uint8_t>(wire.data(), wire.size()));
          ++group.packetCount;
          group.payloadBytes += count;
          group.wireBytes += wire.size();
          group.minimumWireBytes = std::min<uint64_t>(group.minimumWireBytes, wire.size());
          group.maximumWireBytes = std::max<uint64_t>(group.maximumWireBytes, wire.size());
          payloadBytes += count;
        }
        group.wireDigest = digest(wireHash);
        const auto preparationEnd = std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch()).count();
        auto prepared = std::make_shared<Group>(std::move(group));
        const auto index = groupIndex;
        pipeline.submit(prepared->wireBytes,
          [&, commit = io.commitConcurrent, wires = std::move(wires), prepared, index,
           preparationEnd, groupTime](const di::NativeModelCommitPipeline::CurrentCheck& active)
            mutable {
            const Measurement commitTime;
            try {
              commit(std::move(wires), "sha256:" + prepared->wireDigest, active);
              auto row = publicationRow(commitTime, *prepared, index, "publication-commit");
              row["status"] = "COMMITTED_MANIFEST_VERIFIED";
              row["preparationStartMonotonicNs"] = std::chrono::duration_cast<
                std::chrono::nanoseconds>(groupTime.wall.time_since_epoch()).count();
              row["preparationEndMonotonicNs"] = preparationEnd;
              record(std::move(row));
            }
            catch (const std::exception& error) {
              try {
                auto row = publicationRow(commitTime, *prepared, index, "publication-commit");
                row["status"] = "FAILED_OR_AMBIGUOUS";
                row["error"] = error.what();
                row["preparationEndMonotonicNs"] = preparationEnd;
                record(std::move(row));
              }
              catch (...) {}
              throw;
            }
          }, [&, prepared, groupTime, preparationEnd, index] {
            wireBytes += prepared->wireBytes;
            auto row = publicationRow(groupTime, *prepared, index, "publication");
            row["status"] = "COMMITTED_MANIFEST_VERIFIED";
            row["preparationEndMonotonicNs"] = preparationEnd;
            row["timingBoundary"] = "preparation start to owner-observed durable confirmation";
            record(std::move(row));
            groups.push_back(std::move(*prepared));
          });
        ++groupIndex;
      }
      require(input.peek() == std::char_traits<char>::eof() && digest(sourceHash) == expected,
              "benchmark source SHA-256/size mismatch");
      pipeline.finish();
      auto published = publicationTime.result();
      published.update(Json{{"phase", "publication-total"}, {"payloadBytes", payloadBytes},
        {"wireBytes", wireBytes}, {"sha256", expected}, {"groups", groups.size()},
        {"publicationIncludesReadback", false}, {"publicationMode", "bounded-pipeline"},
        {"publicationMaxActiveCommits", di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS}});
      record(std::move(published));
      phase = "retrieval";
      const Measurement retrievalTime;
      ndn::util::Sha256 restored;
      uint64_t restoredBytes = 0;
      for (groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Measurement groupTime;
        const auto& group = groups[groupIndex];
        std::vector<ndn::Name> names;
        names.reserve(group.packetCount);
        for (size_t segment = 0; segment < group.packetCount; ++segment)
          names.emplace_back(ndn::Name(group.prefix).appendSegment(segment));
        ndn::util::Sha256 wireHash;
        current();
        const auto packets = io.fetch(names);
        require(packets.size() == group.packetCount, "benchmark packet count changed");
        for (size_t index = 0; index < packets.size(); ++index) {
          const auto& wire = packets[index];
          wireHash.update(ndn::span<const uint8_t>(wire.data(), wire.size()));
          const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
          require(packet.getName() == names[index], "benchmark packet name changed");
          const auto& content = packet.getContent();
          restored.update(ndn::span<const uint8_t>(content.value(), content.value_size()));
          restoredBytes += content.value_size();
        }
        require(digest(wireHash) == group.wireDigest, "benchmark original wire changed");
        auto row = groupTime.result();
        row.update(Json{{"phase", phase}, {"group", groupIndex},
          {"payloadBytes", group.payloadBytes}, {"wireBytes", group.wireBytes},
          {"status", "VERIFIED"}});
        record(std::move(row));
      }
      require(restoredBytes == FILE_BYTES && digest(restored) == expected,
              "benchmark reconstructed file SHA-256/size mismatch");
      auto retrieved = retrievalTime.result();
      retrieved.update(Json{{"phase", "retrieval-total"}, {"payloadBytes", restoredBytes},
        {"wireBytes", wireBytes}, {"sha256", expected}});
      record(std::move(retrieved));
      auto result = total.result();
      result.update(Json{{"phase", "complete"}, {"pass", true}, {"fileBytes", FILE_BYTES},
        {"sha256", expected}, {"wireBytes", wireBytes}, {"groups", groups.size()},
        {"contentSegmentBytes", CONTENT_BYTES},
        {"publicationIncludesReadback", false},
        {"publicationMode", "bounded-pipeline"},
        {"publicationMaxActiveCommits", di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS},
        {"retainedGroupMetadata", "prefix/count/digest only; names regenerated per read group"},
        {"scope", "Native original-packet file I/O; publication confirms durable manifest; "
                   "retrieval may hit Repo memory/OS caches; no inference"}});
      record(result);
      return result;
    }
    catch (const std::exception& error) {
      auto failure = total.result();
      failure.update(Json{{"phase", phase}, {"group", groupIndex}, {"pass", false},
        {"generatedPayloadBytes", payloadBytes}, {"committedWireBytes", wireBytes},
        {"error", error.what()}});
      record(std::move(failure));
      throw;
    }
  });
  while (work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    try { face.processEvents(ndn::time::milliseconds(10)); }
    catch (...) { cancelled.store(true); }
  }
  const auto result = work.get();
  user.reset();
  face.processEvents(ndn::time::milliseconds(10));
  std::cout << di::nativeCanonicalJson(result) << std::endl;
  return 0;
}
} // namespace

int
main(int argc, char** argv)
{
  try { return run(argc, argv); }
  catch (const std::exception& error) {
    std::cerr << "REPO_FILE_BENCHMARK_FAILED: " << error.what() << '\n';
    return 1;
  }
}
