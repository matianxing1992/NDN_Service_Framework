#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/model-repository-publication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "ndnsf-distributed-repo/repo-commit-pipeline.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/controller-certificate.hpp"
#include "ndn-service-framework/segment-manifest.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <ndn-cxx/encoding/tlv.hpp>
#include <csignal>
#include <ucontext.h>
#include <sys/ucontext.h>
#include <cstdio>
#include <execinfo.h>
#include <unistd.h>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
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

constexpr std::uint64_t NDNSF_DI_WIRE_LIMIT = 256ULL * 1024 * 1024;

namespace {
namespace di = ndnsf::di;
namespace repo = ndnsf_distributed_repo;
using Clock = std::chrono::steady_clock;
using Json = di::NativeJson;
constexpr uint64_t FILE_BYTES = 500000000;
constexpr size_t CONTENT_BYTES = 7000;
constexpr uint32_t CERTIFICATE_GROUP_SEGMENTS = 2048;

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

ndn_service_framework::SegmentManifestScope
manifestScope(const ndn::Name& producer, const ndn::Name& prefix)
{
  ndn_service_framework::SegmentManifestScope scope{producer, prefix};
  const auto& name = prefix.wireEncode();
  const auto hash = ndn::util::Sha256::computeDigest(
    ndn::span<const uint8_t>(name.data(), name.size()));
  std::copy(hash->begin(), hash->end(), scope.bindingDigest.begin());
  return scope;
}

repo::RepoClient
makePacketClient(std::shared_ptr<ndn_service_framework::ServiceUser> user,
                 const repo::RepoArtifactTarget& target)
{
  repo::PersistentRepoClientConfig config;
  config.servicePrefix = ndn::Name(target.servicePrefix);
  config.providerScope = {ndn::Name(target.providerIdentity)};
  config.dataOnly = true;
  return repo::RepoClient::makePersistent(std::move(user), std::move(config));
}

int
run(int argc, char** argv)
{
  const std::set<std::string> required{"--group-prefix", "--user-identity",
    "--controller-identity", "--controller-certificate", "--trust-schema", "--repo-node",
    "--provider-identity", "--service-prefix", "--source", "--expected-sha256", "--report"};
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "ndnsf-repo-file-benchmark: native 500000000-byte original-packet file I/O\n";
    for (const auto& option : required) std::cout << option << " VALUE ";
    std::cout << "[--phase write|read|write-read] [--reference PATH] "
                 "[--authentication digest|certificate|manifest] [--signing-certificate NAME] ";
    std::cout << "\nUses existing HOME/PIB/TPM credentials; bounded 4096-packet commits and "
                 "whole-group reads and at most "
              << di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS
              << " concurrent native commits. Deadline 1800 seconds. Report must not exist.\n";
    return 0;
  }
  std::map<std::string, std::string> options;
  for (int i = 1; i < argc; i += 2) {
    const bool known = required.count(argv[i]) || std::string(argv[i]) == "--phase" ||
      std::string(argv[i]) == "--reference" || std::string(argv[i]) == "--authentication" ||
      std::string(argv[i]) == "--signing-certificate";
    require(i + 1 < argc && known && options.emplace(argv[i], argv[i + 1]).second,
            "invalid benchmark option");
  }
  const std::string phaseOption = options.count("--phase") ? options.at("--phase") : "write-read";
  const std::string authentication = options.count("--authentication")
    ? options.at("--authentication") : "digest";
  require(authentication == "digest" || authentication == "certificate" ||
          authentication == "manifest", "unsupported benchmark authentication");
  const bool useCertificate = authentication != "digest";
  const bool useManifest = authentication == "manifest";
  const auto selectedCertificate = options.find("--signing-certificate");
  require(useCertificate || selectedCertificate == options.end(),
          "a signing certificate requires authenticated benchmark mode");
  const size_t maxSourcePackets = useCertificate ? CERTIFICATE_GROUP_SEGMENTS :
    di::NATIVE_STORED_MODEL_UPLOAD_PACKETS;
  require(phaseOption == "write" || phaseOption == "read" || phaseOption == "write-read",
          "benchmark phase must be write, read or write-read");
  for (const auto& option : required) {
    if (option == "--source") continue;
    require(options.count(option) == 1, "missing benchmark option");
  }
  require(phaseOption == "read" || options.count("--source") == 1,
          "write phase requires --source");
  require(phaseOption != "write-read" || options.count("--reference") == 0,
          "the combined phase does not use a reference file");
  if (phaseOption != "write-read")
    require(options.count("--reference") == 1,
            "staged phases require --reference");
  const auto& source = phaseOption == "read" ? std::string{} : options.at("--source");
  if (phaseOption != "read")
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
  const auto serviceKey = producer.getDefaultKey();
  const auto serviceCertificate = serviceKey.getDefaultCertificate();
  const auto explicitCertificate = selectedCertificate == options.end() ? ndn::Name{} :
    ndn::Name(selectedCertificate->second);
  require(selectedCertificate == options.end() ||
          ndn::security::Certificate::isValidName(explicitCertificate),
          "benchmark signing certificate name is invalid");
  const auto dataKey = explicitCertificate.empty() ? serviceKey :
    producer.getKey(ndn::security::extractKeyNameFromCertName(explicitCertificate));
  const auto certificate = explicitCertificate.empty() ? serviceCertificate :
    dataKey.getCertificate(explicitCertificate);
  const auto keyType = dataKey.getKeyType();
  require(!useCertificate || keyType == ndn::KeyType::RSA || keyType == ndn::KeyType::EC,
          "benchmark certificate mode requires an RSA or ECDSA signing identity");
  const ndn::Name controller(options.at("--controller-identity"));
  const auto controllerCertificate = ndn_service_framework::loadControllerCertificate(
    keys, controller, options.at("--controller-certificate").c_str());
  ndn_service_framework::CertificatePublisher certificates(face, keys, serviceCertificate.getName());
  auto user = std::make_shared<ndn_service_framework::ServiceUser>(face,
    ndn::Name(options.at("--group-prefix")), serviceCertificate, controllerCertificate,
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
  bootstrap["authentication"] = authentication;
  if (useCertificate) {
    bootstrap["keyAlgorithm"] = keyType == ndn::KeyType::EC ? "ECDSA" : "RSA";
    bootstrap["signingCertificate"] = certificate.getName().toUri();
  }
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
      const repo::RepoTarget packetTarget{target.repoNode, target.providerIdentity,
                                          target.servicePrefix};
      const auto verifyWire = std::make_shared<const std::function<bool(const std::vector<uint8_t>&)>>(
        [certificate](const std::vector<uint8_t>& wire) {
          const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
          return ndn::security::verifySignature(packet, certificate);
        });
      const auto rootName = [&](const Group& group) {
        return useManifest ? manifestScope(identity, group.prefix).rootName()
                           : ndn::Name(group.prefix).appendSegment(0);
      };
      const auto commitSecured = [&](di::NativeStoredModelPacketIo::PacketWires wires,
                                     const Group& group, const std::function<void()>& active) {
        repo::RepoClient client = makePacketClient(user, target);
        const repo::RepoOperationOptions operation{deadline, active, {}};
        repo::RepoDataPublication publication;
        publication.target = packetTarget;
        publication.producerIdentity = identity.toUri(ndn::name::UriFormat::ALTERNATE);
        publication.generation = 1;
        const auto object = rootName(group).toUri(ndn::name::UriFormat::ALTERNATE);
        const auto expiry = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch() + (deadline - Clock::now()));
        const auto grant = client.authorizeWrite({packetTarget, publication.producerIdentity, object,
          group.wireDigest, static_cast<uint64_t>(expiry.count())}, operation);
        publication.ownerIdentity = grant.ownerIdentity;
        publication.grantId = grant.grantId;
        publication.ownerEpoch = grant.ownerEpoch;
        publication.packets.emplace();
        auto& source = *publication.packets;
        source.expectedDigest = "sha256:" + group.wireDigest;
        source.expectedWireBytes = group.wireBytes;
        source.packetOffsets = {0};
        for (const auto& wire : wires) {
          const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
          source.packetNames.push_back(packet.getName().toUri(ndn::name::UriFormat::CANONICAL));
          source.packetOffsets.push_back(source.packetOffsets.back() + wire.size());
        }
        source.next = [&wires, index = size_t{0}](std::vector<uint8_t>& wire) mutable {
          if (index == wires.size()) return false;
          wire = std::move(wires[index++]);
          return true;
        };
        if (useManifest) {
          publication.segmentManifest =
            std::make_shared<ndn_service_framework::SegmentManifestScope>(
              manifestScope(identity, group.prefix));
        }
        else {
          publication.verifyWire = verifyWire;
        }
        const auto committed = client.publishData(publication, operation);
        require(committed.packetSet && committed.packetSet->wireDigest == source.expectedDigest &&
                ndn::Name(committed.packetSet->objectName) == ndn::Name(object),
                "benchmark signed publication did not commit the original packet set");
      };
      std::vector<Group> groups;
      if (phaseOption == "read") {
        // A staged reader trusts only the frozen expectation and the recorded
        // group reference; every object is still fetched and verified through
        // the production API below.
        std::ifstream referenceInput(options.at("--reference"));
        require(bool(referenceInput), "benchmark reference open failed");
        const auto reference = Json::parse(referenceInput, nullptr, false);
        require(!reference.is_discarded() &&
                reference.at("schema") == "ndnsf-repo-file-benchmark-reference-v1" &&
                reference.at("fileBytes") == FILE_BYTES &&
                reference.at("sha256") == expected &&
                reference.value("authentication", "digest") == authentication &&
                (!useCertificate || reference.at("signingCertificate") ==
                                      certificate.getName().toUri()) &&
                reference.at("contentSegmentBytes") == CONTENT_BYTES,
                "benchmark reference identity mismatch");
        for (const auto& item : reference.at("groups")) {
          Group group;
          group.prefix = ndn::Name(item.at("prefix").get<std::string>());
          group.packetCount = item.at("packetCount").get<size_t>();
          group.wireDigest = item.at("wireSha256").get<std::string>();
          group.payloadBytes = item.at("payloadBytes").get<uint64_t>();
          group.wireBytes = item.at("wireBytes").get<uint64_t>();
          require(group.packetCount > 0 &&
                  group.packetCount <= maxSourcePackets &&
                  group.payloadBytes > 0 && group.payloadBytes <= group.packetCount * CONTENT_BYTES &&
                  group.wireBytes >= group.payloadBytes,
                  "benchmark reference group invalid");
          payloadBytes += group.payloadBytes;
          wireBytes += group.wireBytes;
          groups.push_back(std::move(group));
        }
        require(payloadBytes == FILE_BYTES, "benchmark reference payload size mismatch");
      }
      auto packetKeys = useCertificate ? std::make_unique<ndn::KeyChain>() :
        std::make_unique<ndn::KeyChain>("pib-memory:repo-file-benchmark",
                                        "tpm-memory:repo-file-benchmark");
      if (phaseOption != "read") {
      ndn::Name publication(identity);
      publication.append("repo-file-benchmark").appendVersion(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      std::ifstream input(source, std::ios::binary);
      require(bool(input), "benchmark source open failed");
      ndn::util::Sha256 sourceHash;
      const auto publicationRow = [&](const Measurement& measurement, const Group& group,
                                      size_t index, const char* rowPhase) {
        auto row = measurement.result();
        row.update(Json{{"phase", rowPhase}, {"group", index},
          {"payloadBytes", group.payloadBytes}, {"wireBytes", group.wireBytes},
          {"packets", group.packetCount}, {"originalWireSha256", group.wireDigest},
          {"objectName", rootName(group).toUri(ndn::name::UriFormat::CANONICAL)},
          {"authentication", authentication},
          {"certificateSignedPackets", useManifest ? 1 : (useCertificate ? group.packetCount : 0)},
          {"minimumWireBytes", group.minimumWireBytes},
          {"maximumWireBytes", group.maximumWireBytes},
          {"publicationMode", "bounded-pipeline"},
          {"publicationMaxActiveCommits", di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS},
          {"cpuScope", "process CPU during overlapping group interval"}});
        return row;
      };
      const Measurement publicationTime;
      di::NativeModelCommitPipeline pipeline(current, NDNSF_DI_WIRE_LIMIT);
      while (payloadBytes < FILE_BYTES) {
        const Measurement groupTime;
        Group group;
        group.prefix = ndn::Name(publication).append(std::to_string(groupIndex));
        di::NativeStoredModelPacketIo::PacketWires wires;
        ndn::util::Sha256 wireHash;
        const auto groupPackets = std::min<uint64_t>(maxSourcePackets,
          (FILE_BYTES - payloadBytes + CONTENT_BYTES - 1) / CONTENT_BYTES);
        const auto appendOriginal = [&](const ndn::Data& packet) {
          const auto& wire = packet.wireEncode();
          wires.emplace_back(wire.begin(), wire.end());
          wireHash.update(ndn::span<const uint8_t>(wire.data(), wire.size()));
          group.wireBytes += wire.size();
          group.minimumWireBytes = std::min<uint64_t>(group.minimumWireBytes, wire.size());
          group.maximumWireBytes = std::max<uint64_t>(group.maximumWireBytes, wire.size());
        };
        std::vector<ndn::ConstBufferPtr> manifestContent;
        if (useManifest) {
          manifestContent.reserve(groupPackets);
        }
        for (size_t segment = 0; segment < groupPackets; ++segment) {
          current();
          std::array<uint8_t, CONTENT_BYTES> buffer;
          const auto count = static_cast<size_t>(std::min<uint64_t>(
            buffer.size(), FILE_BYTES - payloadBytes));
          input.read(reinterpret_cast<char*>(buffer.data()), count);
          require(static_cast<size_t>(input.gcount()) == count, "benchmark source truncated");
          const ndn::span<const uint8_t> content(buffer.data(), count);
          sourceHash.update(content);
          if (useManifest) {
            manifestContent.push_back(std::make_shared<ndn::Buffer>(content.begin(), content.end()));
          }
          else {
            ndn::Data packet(ndn::Name(group.prefix).appendSegment(segment));
            packet.setContent(content);
            packet.setFinalBlock(ndn::name::Component::fromSegment(groupPackets - 1));
            packetKeys->sign(packet, useCertificate ?
              ndn::security::signingByCertificate(certificate) : ndn::security::signingWithSha256());
            appendOriginal(packet);
          }
          ++group.packetCount;
          group.payloadBytes += count;
          payloadBytes += count;
        }
        if (useManifest) {
          const auto manifest = ndn_service_framework::ManifestPublication::prepare(*packetKeys,
            ndn::security::signingByCertificate(certificate), manifestScope(identity, group.prefix),
            {CERTIFICATE_GROUP_SEGMENTS, repo::REPO_USER_STORAGE_OBJECT_MAX_BYTES,
             static_cast<uint32_t>(groupPackets)}, manifestContent, {}, ndn::time::seconds(60));
          for (const auto& packet : manifest.packets()) {
            appendOriginal(packet);
          }
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
              if (useCertificate) {
                commitSecured(std::move(wires), *prepared, active);
              }
              else {
                commit(std::move(wires), "sha256:" + prepared->wireDigest, active);
              }
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
      if (phaseOption == "write") {
        // The reference is an input record for a later independent reader, not
        // a second Repo or publication authority. Write it atomically and sync
        // before claiming the staged write complete.
        ndn::util::Sha256 orderedWire;
        Json referenceGroups = Json::array();
        for (const auto& group : groups) {
          referenceGroups.push_back(Json{{"prefix", group.prefix.toUri(ndn::name::UriFormat::CANONICAL)},
            {"packetCount", group.packetCount}, {"payloadBytes", group.payloadBytes},
            {"wireBytes", group.wireBytes}, {"wireSha256", group.wireDigest}});
          orderedWire.update(ndn::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(group.wireDigest.data()), group.wireDigest.size()));
        }
        const auto referencePath = options.at("--reference");
        const auto temporary = referencePath + ".tmp";
        Json reference{{"schema", "ndnsf-repo-file-benchmark-reference-v1"},
          {"fileBytes", FILE_BYTES}, {"sha256", expected}, {"contentSegmentBytes", CONTENT_BYTES},
          {"orderedWireSha256", digest(orderedWire)}, {"groups", referenceGroups}};
        if (useCertificate) {
          reference["authentication"] = authentication;
          reference["signingCertificate"] = certificate.getName().toUri();
        }
        {
          std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
          require(bool(output), "benchmark reference open failed");
          output << di::nativeCanonicalJson(reference) << std::endl;
          require(output.good(), "benchmark reference write failed");
        }
        const int descriptor = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        require(descriptor >= 0 && ::fsync(descriptor) == 0, "benchmark reference sync failed");
        ::close(descriptor);
        require(::rename(temporary.c_str(), referencePath.c_str()) == 0,
                "benchmark reference publish failed");
        auto referenceRow = total.result();
        referenceRow.update(Json{{"phase", "reference"}, {"groups", groups.size()},
          {"orderedWireSha256", digest(orderedWire)},
          {"status", "REFERENCE_WRITTEN"}});
        record(std::move(referenceRow));
        auto written = total.result();
        written.update(Json{{"phase", "complete"}, {"pass", true}, {"fileBytes", FILE_BYTES},
          {"sha256", expected}, {"wireBytes", wireBytes}, {"groups", groups.size()},
          {"contentSegmentBytes", CONTENT_BYTES}, {"stagedPhase", "write"},
          {"publicationMode", "bounded-pipeline"},
          {"publicationMaxActiveCommits", di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS}});
        record(written);
        return written;
      }
      }
      phase = "retrieval";
      const Measurement retrievalTime;
      ndn::util::Sha256 restored;
      uint64_t restoredBytes = 0;
      for (groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Measurement groupTime;
        const auto& group = groups[groupIndex];
        std::vector<ndn::Name> names;
        const auto scope = manifestScope(identity, group.prefix);
        if (useManifest) {
          names.push_back(scope.rootName());
          const auto pages = ndn_service_framework::SegmentManifestVerifier::pageCountFor(
            static_cast<uint32_t>(group.packetCount));
          for (uint32_t page = 0; page < pages; ++page) {
            names.push_back(scope.pageName(page));
          }
        }
        const auto firstSource = names.size();
        for (size_t segment = 0; segment < group.packetCount; ++segment) {
          names.push_back(useManifest ? scope.sourceName(static_cast<uint32_t>(segment)) :
            ndn::Name(group.prefix).appendSegment(segment));
        }
        ndn::util::Sha256 wireHash;
        current();
        const auto packets = [&] {
          if (!useCertificate) {
            return io.fetch(names);
          }
          auto client = makePacketClient(user, target);
          repo::RepoDataFetch retrieval;
          retrieval.ownerClaim.emplace();
          retrieval.ownerClaim->target = packetTarget;
          retrieval.ownerClaim->objectName = names.front().toUri(ndn::name::UriFormat::ALTERNATE);
          if (useManifest) {
            retrieval.ownerClaim->segmentManifest =
              std::make_shared<ndn_service_framework::SegmentManifestScope>(scope);
          }
          else {
            retrieval.verifyWire = verifyWire;
          }
          auto result = client.fetchData(retrieval, {deadline, current, {}});
          require(result.ownerPacketSet && result.digestVerified &&
                  result.wireDigest == "sha256:" + group.wireDigest,
                  "benchmark signed retrieval did not verify the current Owner claim");
          return std::move(result.packets);
        }();
        require(packets.size() == names.size(), "benchmark packet count changed");
        for (size_t index = 0; index < packets.size(); ++index) {
          const auto& wire = packets[index];
          wireHash.update(ndn::span<const uint8_t>(wire.data(), wire.size()));
          const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
          require(packet.getName() == names[index], "benchmark packet name changed");
          if (index < firstSource) {
            continue;
          }
          const auto& content = packet.getContent();
          restored.update(ndn::span<const uint8_t>(content.value(), content.value_size()));
          restoredBytes += content.value_size();
        }
        require(digest(wireHash) == group.wireDigest, "benchmark original wire changed");
        auto row = groupTime.result();
        row.update(Json{{"phase", phase}, {"group", groupIndex},
          {"payloadBytes", group.payloadBytes}, {"wireBytes", group.wireBytes},
          {"authentication", authentication}, {"manifestControlPackets", firstSource},
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
        {"authentication", authentication},
        {"stagedPhase", phaseOption == "read" ? "read" : "write-read"},
        {"retainedGroupMetadata", "prefix/count/digest only; names regenerated per read group"},
        {"scope", "Native original-packet file I/O; publication confirms durable manifest; "
                   "retrieval may hit Repo memory/OS caches; no inference"}});
      if (phaseOption != "read") {
        result["publicationIncludesReadback"] = false;
        result["publicationMode"] = "bounded-pipeline";
        result["publicationMaxActiveCommits"] = di::NativeModelCommitPipeline::MAX_ACTIVE_COMMITS;
      }
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
  // The user owns the face callbacks and the SVS timers. Drain queued events
  // while it is still alive, then release it; pumping the face after the user
  // is destroyed would dispatch callbacks against freed framework state.
  face.processEvents(ndn::time::milliseconds(10));
  user.reset();
  std::cout << di::nativeCanonicalJson(result) << std::endl;
  return 0;
}
} // namespace

namespace {

/** Diagnostic only: capture the faulting thread's stack for the T012 crash. */
void
onFatalSignal(int signalNumber)
{
  void* frames[64];
  const int count = ::backtrace(frames, 64);
  char header[64];
  const int length = std::snprintf(header, sizeof(header), "BENCHMARK_FATAL_SIGNAL %d\n",
                                   signalNumber);
  if (length > 0) {
    const auto written = ::write(STDERR_FILENO, header, static_cast<std::size_t>(length));
    (void)written;
  }
  ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
  ::_exit(128 + signalNumber);
}

} // namespace

int
main(int argc, char** argv)
{
  // Diagnostic only: capture the faulting PC and stack for the T012 crash.
  struct sigaction action {};
  action.sa_sigaction = [] (int signalNumber, siginfo_t*, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    const auto pc = static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RIP]);
    char header[96];
    const int length = std::snprintf(header, sizeof(header),
                                     "BENCHMARK_FATAL_SIGNAL %d pc=0x%llx\n",
                                     signalNumber, pc);
    if (length > 0) {
      const auto written = ::write(STDERR_FILENO, header, static_cast<std::size_t>(length));
      (void)written;
    }
    onFatalSignal(signalNumber);
  };
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  ::sigaction(SIGSEGV, &action, nullptr);
  ::sigaction(SIGABRT, &action, nullptr);
  try { return run(argc, argv); }
  catch (const std::exception& error) {
    std::cerr << "REPO_FILE_BENCHMARK_FAILED: " << error.what() << '\n';
    return 1;
  }
}
