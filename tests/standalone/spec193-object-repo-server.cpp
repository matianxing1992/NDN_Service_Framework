#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

int
main(int argc, char* argv[])
{
  try {
    std::string localCommand;
    std::string localSource;
    std::string localReplacement;
    std::string localObject;
    std::string localManifest;
    std::string localCertificate;
    std::string localStoreRoot;
    std::string responseFault;
    std::string faultResponseFile;
    size_t segmentBytes = 128;
    std::vector<char*> configArgs{argv[0]};
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--local-command" || option == "--local-source" ||
          option == "--local-replacement" ||
          option == "--local-object" || option == "--local-manifest" ||
          option == "--local-segment-bytes" || option == "--local-certificate" ||
          option == "--local-store-root" || option == "--response-fault" ||
          option == "--fault-response-file") {
        if (++i >= argc) throw std::invalid_argument("missing local option value: " + option);
        const std::string value = argv[i];
        if (option == "--local-command") localCommand = value;
        else if (option == "--local-source") localSource = value;
        else if (option == "--local-replacement") localReplacement = value;
        else if (option == "--local-object") localObject = value;
        else if (option == "--local-manifest") localManifest = value;
        else if (option == "--local-certificate") localCertificate = value;
        else if (option == "--local-store-root") localStoreRoot = value;
        else if (option == "--response-fault") responseFault = value;
        else if (option == "--fault-response-file") faultResponseFile = value;
        else segmentBytes = std::stoul(value);
      }
      else {
        configArgs.push_back(argv[i]);
      }
    }
    if (!responseFault.empty() && responseFault != "malformed" &&
        responseFault != "hold" && responseFault != "wrong-signer")
      throw std::invalid_argument("unsupported response fault");
    if ((responseFault == "wrong-signer") != !faultResponseFile.empty())
      throw std::invalid_argument("wrong-signer requires a fault response gate only");
    // The shared parser rejects bad arguments before Face, PIB or store work.
    const auto config =
      ndnsf_distributed_repo::PersistentRepoServerConfig::fromArguments(
        static_cast<int>(configArgs.size()), configArgs.data());

    std::filesystem::create_directories(config.databasePath.parent_path());
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = config.repoNode;
    capability.repoMode = "persistent";
    capability.freeBytes =
      std::filesystem::space(config.databasePath.parent_path()).available;
    if (localCommand == "range-rejections") {
      capability.freeBytes = 1400;
    }
    capability.failureDomain = config.failureDomain;

    const auto storeRoot = localStoreRoot.empty() ?
      config.databasePath.string() + ".store" : localStoreRoot;
    auto store = ndnsf_distributed_repo::makeTieredRepoStore(
      ndnsf_distributed_repo::makeFilesystemRepoStore(
        storeRoot, config.largeObjectThresholdBytes,
        config.largeObjectThresholdBytes,
        config.repoNode + "|" + config.providerIdentity.toUri()),
      config.memoryCacheBytes, "filesystem", config.largeObjectThresholdBytes);

    // Empty artifact metadata path selects RepoNode's original object protocol.
    auto repo = std::make_shared<ndnsf_distributed_repo::RepoNode>(
      config.servicePrefix, std::move(capability), std::move(store),
      config.repoNode, config.providerIdentity.toUri(), std::string{},
      ndnsf_distributed_repo::ArtifactLimits{}, config.availabilityFreshnessMs);
    if (!localCommand.empty()) {
      namespace repo_api = ndnsf_distributed_repo;
      if (localObject.empty() || localSource.empty() || segmentBytes == 0) {
        throw std::invalid_argument("local segmented command needs object, source, and segment bytes");
      }
      std::ifstream input(localSource, std::ios::binary);
      if (!input) throw std::runtime_error("cannot open local segmented source");
      const std::vector<uint8_t> payload(std::istreambuf_iterator<char>{input}, {});
      if (localCommand == "registry-insert-segmented") {
        if (localManifest.empty() || localStoreRoot.empty() || payload.size() <= segmentBytes * 3) {
          throw std::invalid_argument("L03/L04 requires source, report, isolated store and segments");
        }
        ndn_service_framework::LocalServiceRegistry registry;
        repo->registerLocalServices(registry);
        const auto& prefix = config.servicePrefix;
        const auto initial = repo_api::RepoClient::localCatalogStatus(registry, prefix);
        if (initial.objectCount != 0) throw std::runtime_error("L03 local store was not empty");
        ndn::security::KeyChain keyChain;
        const auto identity = keyChain.getPib().getIdentity(config.providerIdentity);
        const auto certificate = identity.getDefaultKey().getDefaultCertificate();
        const auto packetName = localObject + "/packet";
        ndn::Data data{ndn::Name(packetName)};
        data.setContent(ndn::span<const uint8_t>(payload.data(), payload.size()));
        keyChain.sign(data, ndn::security::signingByIdentity(identity.getName()));
        const auto& encoded = data.wireEncode();
        const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
        if (!ndn::security::verifySignature(data, certificate)) {
          throw std::runtime_error("L03 generated Data signature is invalid");
        }
        const auto artifactDir = std::filesystem::path(localManifest).parent_path();
        const auto save = [&] (const std::string& name, const std::vector<uint8_t>& bytes) {
          std::ofstream output(artifactDir / name, std::ios::binary | std::ios::trunc);
          output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          output.close();
          if (!output) throw std::runtime_error("cannot save local witness: " + name);
        };
        save("l03-original.wire", wire);
        const auto& certWire = certificate.wireEncode();
        save("l03-certificate.wire", {certWire.begin(), certWire.end()});
        repo_api::RepoDataReference reference;
        reference.objectName = localObject + "/insert";
        reference.dataPrefix = packetName;
        reference.objectType = "ndn-local-packet-set";
        reference.expectedSize = wire.size();
        reference.expectedSha256 = repo_api::RepoClient::makeManifest(
          packetName, "ndn-data-wire", wire, 1, {}, "").sha256;
        repo->setDataReferenceFetcher([wire, packetName] (const repo_api::RepoDataReference& requested) {
          if (requested.dataPrefix != packetName) throw std::runtime_error("local-fetch-denied");
          return std::vector<std::vector<uint8_t>>{wire};
        });
        const auto inserted = repo_api::RepoClient::localInsert(registry, prefix, reference);
        const auto completed = repo_api::RepoClient::localStatus(registry, prefix, inserted.operationId);
        if (inserted.state != "DONE" || completed.toJson() != inserted.toJson() ||
            inserted.completedSegments != 1 || inserted.totalSegments != 1) {
          throw std::runtime_error("L03 local INSERT/STATUS did not complete exact packet");
        }
        const auto parent = repo_api::RepoClient::localGetManifest(registry, prefix, reference.objectName);
        const auto packet = repo_api::RepoClient::localGetManifest(registry, prefix, packetName);
        const auto catalog = repo_api::RepoClient::localCatalogStatus(registry, prefix);
        const auto snapshot = repo_api::RepoClient::localCatalogSnapshot(registry, prefix);
        const auto delta = repo_api::RepoClient::localCatalogDelta(registry, prefix, initial.catalogEpoch);
        const auto lookup = repo_api::RepoClient::localCatalogLookup(registry, prefix, reference.objectName);
        auto catalogParent = parent;
        if (catalogParent.replicaNodes.empty()) catalogParent.replicaNodes.push_back(config.repoNode);
        if (catalog.objectCount != 2 || snapshot.entries.size() != 2 || delta.entries.size() != 2 ||
            lookup.manifest.toJson() != catalogParent.toJson() || lookup.state != "AVAILABLE" ||
            lookup.sourceRepo != config.repoNode ||
            parent.packetNames != std::vector<std::string>{packetName} ||
            parent.sha256 != reference.expectedSha256 || packet.sha256 != parent.sha256) {
          throw std::runtime_error("L03 local catalog disagrees with original packet");
        }
        const auto cacheBefore = repo_api::RepoClient::localCacheStatus(registry, prefix);
        if (repo_api::RepoClient::localGet(registry, prefix, packetName) != wire ||
            repo_api::RepoClient::localGet(registry, prefix, packetName) != wire) {
          throw std::runtime_error("L03 cached reads changed original Data wire");
        }
        const auto cacheAfter = repo_api::RepoClient::localCacheStatus(registry, prefix);
        if (cacheAfter.hits < cacheBefore.hits + 2) {
          throw std::runtime_error("L03 cache status did not observe repeated reads");
        }
        bool missingLookup = false;
        try { (void)repo_api::RepoClient::localCatalogLookup(registry, prefix, localObject + "/missing"); }
        catch (const std::exception& error) {
          missingLookup = std::string(error.what()).find("not-found") != std::string::npos;
        }
        auto denied = reference;
        denied.objectName = localObject + "/denied";
        denied.dataPrefix = localObject + "/unavailable";
        const auto failed = repo_api::RepoClient::localInsert(registry, prefix, denied);
        const auto failedQuery = repo_api::RepoClient::localStatus(registry, prefix, failed.operationId);
        if (!missingLookup || failed.state != "FAILED" || failed.completedSegments != 0 ||
            failed.message.find("local-fetch-denied") == std::string::npos ||
            failedQuery.toJson() != failed.toJson() ||
            repo_api::RepoClient::localCatalogStatus(registry, prefix).objectCount != 2) {
          throw std::runtime_error("L03 failed INSERT or missing lookup changed durable catalog");
        }
        const auto segmented = repo_api::RepoClient::localPutSegmented(
          registry, prefix, localObject + "/segmented", payload, {}, segmentBytes);
        const std::vector<uint8_t> small(payload.begin(), payload.begin() + 23);
        const auto plain = repo_api::RepoClient::localPutSegmented(
          registry, prefix, localObject + "/plain", small, {}, segmentBytes);
        if (segmented.segmentCount < 4 || plain.segmentCount != 1 ||
            repo_api::RepoClient::localGetSegmented(registry, prefix, segmented) != payload ||
            repo_api::RepoClient::localGetObject(registry, prefix, segmented) != payload ||
            repo_api::RepoClient::localGetSegmented(registry, prefix, plain) != small ||
            repo_api::RepoClient::localGetObject(registry, prefix, plain) != small) {
          throw std::runtime_error("L04 local segmented/plain reads disagree with source");
        }
        const auto segmentName = segmented.objectName + "/seg/1";
        const std::vector<uint8_t> segment(payload.begin() + segmentBytes,
                                          payload.begin() + segmentBytes * 2);
        const auto rejectReads = [&] (const std::string& reason) {
          for (const bool object : {false, true}) {
            bool rejected = false;
            try {
              if (object) (void)repo_api::RepoClient::localGetObject(registry, prefix, segmented);
              else (void)repo_api::RepoClient::localGetSegmented(registry, prefix, segmented);
            }
            catch (const std::exception& error) {
              rejected = std::string(error.what()).find(reason) != std::string::npos;
            }
            if (!rejected) throw std::runtime_error("L04 incomplete/corrupt object was returned");
          }
        };
        if (!repo_api::RepoClient::localRemove(registry, prefix, segmentName)) {
          throw std::runtime_error("L04 could not remove middle segment");
        }
        rejectReads("not-found");
        repo_api::StoreOptions segmentOptions;
        segmentOptions.objectType = segmented.objectType + ".segment";
        repo_api::RepoClient::localPut(registry, prefix, segmentName, segment, segmentOptions);
        auto corrupt = segment;
        corrupt.front() ^= 1;
        repo_api::RepoClient::localPut(registry, prefix, segmentName, corrupt, segmentOptions);
        rejectReads("sha256");
        repo_api::RepoClient::localPut(registry, prefix, segmentName, segment, segmentOptions);
        const auto rebuilt = repo_api::RepoClient::localGetObject(registry, prefix, segmented);
        if (rebuilt != payload) throw std::runtime_error("L04 restoration did not recover original bytes");
        save("l04-rebuilt.bin", rebuilt);
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"insertStatus\":" << inserted.toJson()
               << ",\"failedStatus\":" << failed.toJson()
               << ",\"parent\":" << parent.toJson() << ",\"packet\":" << packet.toJson()
               << ",\"catalog\":" << catalog.toJson() << ",\"snapshot\":" << snapshot.toJson()
               << ",\"delta\":" << delta.toJson() << ",\"lookup\":" << lookup.toJson()
               << ",\"cacheBefore\":" << cacheBefore.toJson() << ",\"cacheAfter\":" << cacheAfter.toJson()
               << ",\"segmented\":" << segmented.toJson() << ",\"plain\":" << plain.toJson()
               << ",\"missingLookupRejected\":true,\"missingSegmentRejected\":true"
               << ",\"corruptSegmentRejected\":true,\"restored\":true}";
        report.close();
        if (!report) throw std::runtime_error("cannot save L03/L04 report");
        std::cout << "SPEC193_L03_L04_LOCAL_PASS segments=" << segmented.segmentCount << std::endl;
        return 0;
      }
      if (localCommand == "registry-ranges") {
        if (localManifest.empty() || payload.size() < 400 || localStoreRoot.empty()) {
          throw std::invalid_argument("L02 requires source, report, and isolated store root");
        }
        ndn_service_framework::LocalServiceRegistry registry;
        repo->registerLocalServices(registry);
        const auto& prefix = config.servicePrefix;
        const auto make = [&] (const std::string& suffix,
                               const std::vector<uint8_t>& bytes) {
          auto manifest = repo_api::RepoClient::makeManifest(
            localObject + "/" + suffix, "application/octet-stream",
            bytes, 1, {}, "spec193-l02");
          manifest.operationId = "spec193-l02-" + suffix;
          return manifest;
        };
        const auto countStaging = [&] {
          size_t count = 0;
          for (const auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(localStoreRoot) / "staging")) {
            if (entry.is_regular_file()) ++count;
          }
          return count;
        };
        const auto absent = [&] (const std::string& name) {
          const auto listed = repo_api::RepoClient::localList(registry, prefix);
          return std::none_of(listed.begin(), listed.end(),
            [&] (const auto& item) { return item.objectName == name; });
        };
        const auto slice = [&] (size_t begin, size_t end) {
          return std::vector<uint8_t>(payload.begin() + begin, payload.begin() + end);
        };
        const auto committed = make("committed", payload);
        repo_api::RepoClient::localPutRange(
          registry, prefix, committed, {0, 137}, slice(0, 137));
        const auto stagedFiles = countStaging();
        if (stagedFiles == 0 || !absent(committed.objectName)) {
          throw std::runtime_error("L02 partial range lacks staging or became visible");
        }
        bool incompleteRejected = false;
        try {
          (void)repo_api::RepoClient::localCommitRanges(registry, prefix, committed);
        }
        catch (const std::exception& error) {
          incompleteRejected = std::string(error.what()).find(
            "repo-artifact-incomplete") != std::string::npos;
        }
        if (!incompleteRejected || !absent(committed.objectName) ||
            countStaging() != 0) {
          throw std::runtime_error("L02 incomplete commit left a half object");
        }
        repo_api::RepoClient::localPutRange(
          registry, prefix, committed, {137, payload.size() - 137},
          slice(137, payload.size()));
        repo_api::RepoClient::localPutRange(
          registry, prefix, committed, {0, 137}, slice(0, 137));
        const auto durable = repo_api::RepoClient::localCommitRanges(
          registry, prefix, committed);
        if (durable.sha256 != committed.sha256 ||
            repo_api::RepoClient::localGet(registry, prefix, committed.objectName) !=
              payload || countStaging() != 0) {
          throw std::runtime_error("L02 committed range bytes or cleanup differ");
        }
        auto abortPayload = payload;
        abortPayload.front() ^= 0x31;
        const auto aborted = make("aborted", abortPayload);
        repo_api::RepoClient::localPutRange(
          registry, prefix, aborted, {0, 137},
          std::vector<uint8_t>(abortPayload.begin(), abortPayload.begin() + 137));
        if (countStaging() == 0 || !absent(aborted.objectName)) {
          throw std::runtime_error("L02 abort candidate was not staged");
        }
        repo_api::RepoClient::localAbortRanges(registry, prefix, aborted.objectName);
        if (countStaging() != 0 || !absent(aborted.objectName)) {
          throw std::runtime_error("L02 abort left staged or visible bytes");
        }
        auto boundsPayload = payload;
        boundsPayload.front() ^= 0x52;
        const auto bounds = make("bounds", boundsPayload);
        bool boundsRejected = false;
        try {
          repo_api::RepoClient::localPutRange(
            registry, prefix, bounds, {payload.size() - 10, 30},
            std::vector<uint8_t>(30, 0x55));
        }
        catch (const std::exception& error) {
          boundsRejected = std::string(error.what()).find("out-of-bounds") !=
                           std::string::npos;
        }
        auto mismatchPayload = payload;
        mismatchPayload.front() ^= 0x73;
        const auto mismatch = make("mismatch", mismatchPayload);
        bool lengthRejected = false;
        try {
          repo_api::RepoClient::localPutRange(
            registry, prefix, mismatch, {0, 30}, std::vector<uint8_t>(29, 0x55));
        }
        catch (const std::exception& error) {
          lengthRejected = std::string(error.what()).find(
            "repo range write payload length mismatch") !=
                           std::string::npos;
        }
        const auto finalStaging = countStaging();
        const auto boundsAbsent = absent(bounds.objectName);
        const auto mismatchAbsent = absent(mismatch.objectName);
        const auto finalListSize = repo_api::RepoClient::localList(registry, prefix).size();
        if (!boundsRejected || !lengthRejected || finalStaging != 0 ||
            !boundsAbsent || !mismatchAbsent || finalListSize != 1) {
          throw std::runtime_error(
            "L02 invalid range state bounds=" + std::to_string(boundsRejected) +
            " length=" + std::to_string(lengthRejected) +
            " staging=" + std::to_string(finalStaging) +
            " boundsAbsent=" + std::to_string(boundsAbsent) +
            " mismatchAbsent=" + std::to_string(mismatchAbsent) +
            " list=" + std::to_string(finalListSize));
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"committed\":" << durable.toJson()
               << ",\"stagedFiles\":" << stagedFiles
               << ",\"incompleteRejected\":true,\"abortClean\":true"
               << ",\"boundsRejected\":true,\"lengthRejected\":true"
               << ",\"finalStagingFiles\":0}";
        report.close();
        if (!report) throw std::runtime_error("cannot save L02 report");
        std::cout << "SPEC193_L02_LOCAL_PASS size=" << payload.size() << std::endl;
        return 0;
      }
      if (localCommand == "registry-object") {
        if (localManifest.empty() || payload.empty() || localStoreRoot.empty()) {
          throw std::invalid_argument("L01 requires source, report, and isolated store root");
        }
        ndn_service_framework::LocalServiceRegistry registry;
        repo->registerLocalServices(registry);
        const auto& prefix = config.servicePrefix;
        const std::string witnessName = localObject + "/witness";
        if (!repo_api::RepoClient::localList(registry, prefix).empty() ||
            repo_api::RepoClient::localRemove(registry, prefix, localObject)) {
          throw std::runtime_error("L01 isolated local store was not empty");
        }
        repo_api::StoreOptions options;
        options.objectType = "application/octet-stream";
        const auto stored = repo_api::RepoClient::localPut(
          registry, prefix, localObject, payload, options);
        if (repo_api::RepoClient::localGet(registry, prefix, localObject) != payload ||
            repo_api::RepoClient::localGetManifest(registry, prefix, localObject).toJson() !=
              stored.toJson()) {
          throw std::runtime_error("L01 local read differs from stored object");
        }
        const auto afterPut = repo_api::RepoClient::localList(registry, prefix);
        if (afterPut.size() != 1 || afterPut.front().toJson() != stored.toJson()) {
          throw std::runtime_error("L01 local list did not expose stored object");
        }
        if (!repo_api::RepoClient::localRemove(registry, prefix, localObject) ||
            repo_api::RepoClient::localRemove(registry, prefix, localObject) ||
            !repo_api::RepoClient::localList(registry, prefix).empty()) {
          throw std::runtime_error("L01 local remove or final list was incorrect");
        }
        bool missingGet = false;
        bool missingManifest = false;
        try {
          (void)repo_api::RepoClient::localGet(registry, prefix, localObject);
        }
        catch (const std::exception& error) {
          missingGet = std::string(error.what()).find("repo-object-not-found") !=
                       std::string::npos;
        }
        try {
          (void)repo_api::RepoClient::localGetManifest(registry, prefix, localObject);
        }
        catch (const std::exception& error) {
          missingManifest = std::string(error.what()).find("repo-object-not-found") !=
                            std::string::npos;
        }
        if (!missingGet || !missingManifest) {
          throw std::runtime_error("L01 removed object remained readable");
        }
        const auto witness = repo_api::RepoClient::localPut(
          registry, prefix, witnessName, payload, options);
        const auto finalList = repo_api::RepoClient::localList(registry, prefix);
        if (finalList.size() != 1 || finalList.front().toJson() != witness.toJson()) {
          throw std::runtime_error("L01 durable local witness is absent");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"stored\":" << stored.toJson()
               << ",\"witness\":" << witness.toJson()
               << ",\"afterPutCount\":1,\"afterRemoveCount\":0"
               << ",\"missingGetRejected\":true"
               << ",\"missingManifestRejected\":true}";
        report.close();
        if (!report) throw std::runtime_error("cannot save L01 report");
        std::cout << "SPEC193_L01_LOCAL_PASS size=" << payload.size() << std::endl;
        return 0;
      }
      if (localCommand == "corruption-seed") {
        if (localManifest.empty() || payload.empty()) {
          throw std::invalid_argument("O05 requires nonempty source and report path");
        }
        for (const bool manifestLookup : {false, true}) {
          bool rejected = false;
          try {
            if (manifestLookup) {
              (void)repo_api::RepoClient::getManifest(*repo, localObject);
            }
            else {
              (void)repo_api::RepoClient::get(*repo, localObject);
            }
          }
          catch (const std::exception& error) {
            rejected = std::string(error.what()).find("repo-object-not-found") !=
                       std::string::npos;
          }
          if (!rejected) throw std::runtime_error("O05 missing object appeared readable");
        }
        repo_api::StoreOptions options;
        options.objectType = "application/octet-stream";
        const auto manifest = repo_api::RepoClient::put(*repo, localObject, payload, options);
        if (repo_api::RepoClient::get(*repo, localObject) != payload ||
            repo_api::RepoClient::getManifest(*repo, localObject).toJson() !=
              manifest.toJson()) {
          throw std::runtime_error("O05 seed did not round-trip original bytes");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"manifest\":" << manifest.toJson()
               << ",\"missingGetRejected\":true"
               << ",\"missingManifestRejected\":true}";
        report.close();
        if (!report) throw std::runtime_error("cannot save O05 report");
        std::cout << "SPEC193_O05_LOCAL_SEED_PASS size=" << payload.size() << std::endl;
        return 0;
      }
      if (localCommand == "object-remove") {
        if (localManifest.empty() || payload.empty()) {
          throw std::invalid_argument("O04 requires nonempty source and report path");
        }
        const auto initial = repo->core().catalogStatus();
        if (repo_api::RepoClient::remove(*repo, localObject) ||
            repo->core().catalogStatus().catalogEpoch != initial.catalogEpoch) {
          throw std::runtime_error("O04 missing-object removal changed catalog");
        }
        repo_api::StoreOptions options;
        options.objectType = "application/octet-stream";
        const auto stored = repo_api::RepoClient::put(
          *repo, localObject, payload, options);
        const auto afterPut = repo->core().catalogStatus();
        if (stored.generation != 1 ||
            afterPut.catalogEpoch != initial.catalogEpoch + 1 ||
            afterPut.objectCount != initial.objectCount + 1 ||
            repo_api::RepoClient::get(*repo, localObject) != payload) {
          throw std::runtime_error("O04 put baseline did not become visible");
        }
        if (!repo_api::RepoClient::remove(*repo, localObject)) {
          throw std::runtime_error("O04 existing-object removal returned false");
        }
        const auto afterDelete = repo->core().catalogStatus();
        const auto delta = repo->core().catalogDelta(afterPut.catalogEpoch);
        if (afterDelete.catalogEpoch != afterPut.catalogEpoch + 1 ||
            afterDelete.objectCount != initial.objectCount ||
            delta.entries.size() != 1 ||
            delta.entries.front().manifest.objectName != localObject ||
            delta.entries.front().manifest.sha256 != stored.sha256 ||
            delta.entries.front().state != "DELETED") {
          throw std::runtime_error("O04 delete did not publish one catalog tombstone");
        }
        if (repo_api::RepoClient::remove(*repo, localObject) ||
            repo->core().catalogStatus().catalogEpoch != afterDelete.catalogEpoch) {
          throw std::runtime_error("O04 repeated remove was not idempotent");
        }
        const auto inventory = repo_api::RepoClient::list(*repo);
        if (std::any_of(inventory.begin(), inventory.end(),
                        [&] (const auto& item) {
                          return item.objectName == localObject;
                        })) {
          throw std::runtime_error("O04 deleted object remained in list");
        }
        for (const bool manifestLookup : {false, true}) {
          bool rejected = false;
          try {
            if (manifestLookup) {
              (void)repo_api::RepoClient::getManifest(*repo, localObject);
            }
            else {
              (void)repo_api::RepoClient::get(*repo, localObject);
            }
          }
          catch (const std::exception& error) {
            rejected = std::string(error.what()).find("repo-object-not-found") !=
                       std::string::npos;
          }
          if (!rejected) throw std::runtime_error("O04 deleted object remained readable");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"stored\":" << stored.toJson()
               << ",\"before\":" << initial.toJson()
               << ",\"afterPut\":" << afterPut.toJson()
               << ",\"afterDelete\":" << afterDelete.toJson()
               << ",\"delta\":" << delta.toJson()
               << ",\"missingRemove\":false"
               << ",\"firstRemove\":true"
               << ",\"repeatRemove\":false}";
        report.close();
        if (!report) throw std::runtime_error("cannot save O04 report");
        std::cout << "SPEC193_O04_LOCAL_PASS epoch="
                  << afterDelete.catalogEpoch << std::endl;
        return 0;
      }
      if (localCommand == "range-rejections") {
        if (localManifest.empty() || payload.size() < 400) {
          throw std::invalid_argument("O03 requires report path and at least 400 source bytes");
        }
        const auto make = [&] (const std::string& suffix,
                               const std::vector<uint8_t>& bytes) {
          auto manifest = repo_api::RepoClient::makeManifest(
            localObject + "/" + suffix, "application/octet-stream",
            bytes, 1, {}, "spec193-o03");
          manifest.operationId = "spec193-o03-" + suffix;
          return manifest;
        };
        const auto slice = [&] (const std::vector<uint8_t>& bytes,
                                size_t start, size_t end) {
          return std::vector<uint8_t>(bytes.begin() + start,
                                      bytes.begin() + end);
        };
        const auto absent = [&] (const std::string& name) {
          if (repo->core().has(name)) return false;
          const auto inventory = repo_api::RepoClient::list(*repo);
          return std::none_of(inventory.begin(), inventory.end(),
            [&] (const auto& item) { return item.objectName == name; });
        };
        const auto owned = make("owned", payload);
        repo_api::RepoClient::putRange(*repo, owned, {0, 101}, slice(payload, 0, 101));
        auto foreign = owned;
        foreign.operationId = "spec193-o03-foreign";
        if (repo->core().abortRangesIfOwned(foreign)) {
          throw std::runtime_error("O03 wrong owner aborted staged ranges");
        }
        bool wrongCommitRejected = false;
        try {
          (void)repo->core().commitRangesIfOwned(foreign);
        }
        catch (const std::exception& error) {
          wrongCommitRejected = std::string(error.what()).find(
            "repo-publication-identity-mismatch") != std::string::npos;
        }
        if (!wrongCommitRejected || !absent(owned.objectName)) {
          throw std::runtime_error("O03 wrong owner altered reservation or visibility");
        }
        repo_api::RepoClient::putRange(*repo, owned, {101, payload.size() - 101},
                                      slice(payload, 101, payload.size()));
        const auto committed = repo->core().commitRangesIfOwned(owned);
        if (committed.sha256 != owned.sha256 ||
            repo_api::RepoClient::get(*repo, owned.objectName) != payload) {
          throw std::runtime_error("O03 rightful owner could not complete staged object");
        }

        auto incompletePayload = payload;
        incompletePayload.front() ^= 0x11;
        const auto incomplete = make("incomplete", incompletePayload);
        auto abortedPayload = payload;
        abortedPayload.front() ^= 0x22;
        const auto aborted = make("aborted", abortedPayload);
        repo_api::RepoClient::putRange(*repo, incomplete, {0, 101},
                                      slice(incompletePayload, 0, 101));
        bool quotaRejected = false;
        try {
          repo_api::RepoClient::putRange(*repo, aborted, {0, 101},
                                        slice(abortedPayload, 0, 101));
        }
        catch (const std::exception& error) {
          quotaRejected = std::string(error.what()).find(
            "insufficient free space") != std::string::npos;
        }
        if (!quotaRejected || !absent(aborted.objectName)) {
          throw std::runtime_error("O03 outstanding reservation did not enforce quota");
        }
        bool missingRejected = false;
        std::string missingReason;
        try {
          (void)repo_api::RepoClient::commitRanges(*repo, incomplete);
        }
        catch (const std::exception& error) {
          missingReason = error.what();
          missingRejected = std::string(error.what()).find(
            "repo-artifact-incomplete") != std::string::npos;
        }
        const bool incompleteAbsent = absent(incomplete.objectName);
        if (!missingRejected || !incompleteAbsent) {
          throw std::runtime_error("O03 missing range commit rejection=" +
            std::to_string(missingRejected) + " absent=" +
            std::to_string(incompleteAbsent) + " reason=" + missingReason);
        }

        repo_api::RepoClient::putRange(*repo, aborted, {0, 101},
                                      slice(abortedPayload, 0, 101));
        repo_api::RepoClient::abortRanges(*repo, aborted.objectName);
        if (!absent(aborted.objectName)) {
          throw std::runtime_error("O03 abort left a visible object");
        }
        // A different reservation must fit after abort releases the 509-byte charge.
        repo_api::RepoClient::putRange(*repo, incomplete, {0, 101},
                                      slice(incompletePayload, 0, 101));
        repo_api::RepoClient::abortRanges(*repo, incomplete.objectName);
        if (!absent(incomplete.objectName)) {
          throw std::runtime_error("O03 post-abort reservation remained visible");
        }

        auto boundsPayload = payload;
        boundsPayload.front() ^= 0x33;
        const auto bounds = make("bounds", boundsPayload);
        bool boundsRejected = false;
        try {
          repo_api::RepoClient::putRange(*repo, bounds,
            {payload.size() - 10, 30}, std::vector<uint8_t>(30, 0x55));
        }
        catch (const std::out_of_range& error) {
          boundsRejected = std::string(error.what()).find(
            "repo-core-range-write-out-of-bounds") != std::string::npos;
        }
        if (!boundsRejected || !absent(bounds.objectName)) {
          throw std::runtime_error("O03 out-of-bounds range became visible");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"committed\":" << committed.toJson()
               << ",\"wrongOwnerRejected\":true"
               << ",\"missingCommitRejected\":true"
               << ",\"quotaRejectedWhileReserved\":true"
               << ",\"abortReadmitted\":true"
               << ",\"boundsRejected\":true}";
        report.close();
        if (!report) throw std::runtime_error("cannot save O03 report");
        std::cout << "SPEC193_O03_LOCAL_PASS size=" << payload.size() << std::endl;
        return 0;
      }
      if (localCommand == "range-commit") {
        if (localManifest.empty() || payload.size() < 400) {
          throw std::invalid_argument("O02 requires report path and at least 400 source bytes");
        }
        const auto manifest = repo_api::RepoClient::makeManifest(
          localObject, "application/octet-stream", payload, 1, {}, "spec193-o02");
        constexpr size_t firstEnd = 101;
        constexpr size_t middleEnd = 318;
        const auto write = [&] (size_t start, size_t end) {
          const std::vector<uint8_t> bytes(payload.begin() + start,
                                           payload.begin() + end);
          repo_api::RepoClient::putRange(*repo, manifest,
                                        repo_api::RepoByteRange{start, bytes.size()}, bytes);
        };
        const auto invisible = [&] {
          const auto inventory = repo_api::RepoClient::list(*repo);
          if (std::any_of(inventory.begin(), inventory.end(),
                          [&] (const auto& item) { return item.objectName == localObject; })) {
            return false;
          }
          try {
            (void)repo_api::RepoClient::get(*repo, localObject);
            return false;
          }
          catch (const std::exception&) {
            return true;
          }
        };
        write(middleEnd, payload.size());
        write(0, firstEnd);
        if (!invisible()) throw std::runtime_error("O02 staged ranges became visible");
        write(firstEnd, middleEnd);
        const auto committed = repo_api::RepoClient::commitRanges(*repo, manifest);
        const auto subrange = repo_api::RepoClient::getRange(
          *repo, localObject, repo_api::RepoByteRange{73, 281});
        if (committed.objectName != localObject || committed.generation != 1 ||
            committed.sha256 != manifest.sha256 || committed.size != payload.size() ||
            repo_api::RepoClient::get(*repo, localObject) != payload ||
            repo_api::RepoClient::getManifest(*repo, localObject).toJson() !=
              committed.toJson() ||
            subrange != std::vector<uint8_t>(payload.begin() + 73,
                                             payload.begin() + 354)) {
          throw std::runtime_error("O02 committed range bytes or manifest differ");
        }
        const auto inventory = repo_api::RepoClient::list(*repo);
        if (std::count_if(inventory.begin(), inventory.end(),
                          [&] (const auto& item) {
                            return item.objectName == localObject;
                          }) != 1) {
          throw std::runtime_error("O02 committed inventory is not one object");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"manifest\":" << committed.toJson()
               << ",\"stagedInvisible\":true,\"writeOrder\":[["
               << middleEnd << "," << payload.size() - middleEnd << "],[0,"
               << firstEnd << "],[" << firstEnd << ","
               << middleEnd - firstEnd << "]]}";
        report.close();
        if (!report) throw std::runtime_error("cannot save O02 report");
        std::cout << "SPEC193_O02_LOCAL_PASS size=" << payload.size() << std::endl;
        return 0;
      }
      if (localCommand == "object-put-replace") {
        if (localReplacement.empty() || localManifest.empty()) {
          throw std::invalid_argument("O01 needs replacement source and report path");
        }
        std::ifstream replacementInput(localReplacement, std::ios::binary);
        if (!replacementInput) throw std::runtime_error("cannot open O01 replacement source");
        const std::vector<uint8_t> replacement(
          std::istreambuf_iterator<char>{replacementInput}, {});
        if (replacement == payload || replacement.empty() || payload.empty()) {
          throw std::invalid_argument("O01 needs two distinct nonempty payloads");
        }
        const auto count = [&] {
          const auto inventory = repo_api::RepoClient::list(*repo);
          return std::count_if(inventory.begin(), inventory.end(),
            [&] (const auto& item) { return item.objectName == localObject; });
        };
        if (count() != 0) throw std::runtime_error("O01 target already exists");
        repo_api::StoreOptions options;
        options.objectType = "application/octet-stream";
        const auto first = repo_api::RepoClient::put(*repo, localObject, payload, options);
        if (first.generation != 1 || count() != 1 ||
            repo_api::RepoClient::get(*repo, localObject) != payload ||
            repo_api::RepoClient::getManifest(*repo, localObject).toJson() != first.toJson()) {
          throw std::runtime_error("O01 first put/get/manifest/list disagree");
        }
        const auto duplicate = repo_api::RepoClient::put(*repo, localObject, payload, options);
        if (duplicate.generation != 2 || count() != 1 ||
            repo_api::RepoClient::get(*repo, localObject) != payload ||
            repo_api::RepoClient::getManifest(*repo, localObject).toJson() != duplicate.toJson()) {
          throw std::runtime_error("O01 identical put changed bytes or inventory");
        }
        const auto final = repo_api::RepoClient::put(*repo, localObject, replacement, options);
        if (final.generation != 3 || count() != 1 ||
            repo_api::RepoClient::get(*repo, localObject) != replacement ||
            repo_api::RepoClient::getManifest(*repo, localObject).toJson() != final.toJson() ||
            first.sha256 == final.sha256) {
          throw std::runtime_error("O01 replacement put/get/manifest/list disagree");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"first\":" << first.toJson()
               << ",\"duplicate\":" << duplicate.toJson()
               << ",\"final\":" << final.toJson()
               << ",\"listCount\":" << count() << "}";
        report.close();
        if (!report) throw std::runtime_error("cannot save O01 report");
        std::cout << "SPEC193_O01_LOCAL_PASS generation=" << final.generation << std::endl;
        return 0;
      }
      if (localCommand == "payload-insert") {
        if (localManifest.empty() || localCertificate.empty() ||
            payload.size() <= segmentBytes * 2) {
          throw std::invalid_argument("payload insert needs source, manifest and certificate outputs");
        }
        ndn::security::KeyChain keyChain;
        const auto identity = keyChain.getPib().getIdentity(config.providerIdentity);
        const auto certificate = identity.getDefaultKey().getDefaultCertificate();
        repo_api::StoreOptions options;
        options.objectType = "ndn-auto-segmented-payload";
        const auto status = repo_api::RepoClient::insertPayload(
          *repo, localObject, payload, keyChain,
          ndn::security::signingByIdentity(config.providerIdentity),
          options, segmentBytes);
        const auto queried = repo_api::RepoClient::status(*repo, status.operationId);
        if (status.state != "DONE" || queried.state != "DONE" ||
            status.completedSegments < 3 ||
            queried.completedSegments != status.completedSegments) {
          throw std::runtime_error("insertPayload did not complete all signed segments");
        }
        const auto manifest = repo_api::RepoClient::getManifest(*repo, localObject);
        const auto wires = repo_api::RepoClient::getDataPackets(*repo, manifest);
        if (manifest.objectType != options.objectType ||
            manifest.segmentCount != wires.size() ||
            wires.size() != status.completedSegments) {
          throw std::runtime_error("insertPayload manifest and packet set disagree");
        }
        for (size_t index = 0; index < wires.size(); ++index) {
          const auto expectedName = ndn::Name(localObject).appendSegment(index).toUri();
          ndn::Block block(ndn::span<const uint8_t>(wires[index].data(), wires[index].size()));
          block.parse();
          const ndn::Data data(block);
          if (manifest.packetNames[index] != expectedName ||
              data.getName().toUri() != expectedName) {
            throw std::runtime_error("insertPayload changed generated segment name");
          }
          const auto path = std::filesystem::path(localManifest).parent_path() /
                            ("o11-segment-" + std::to_string(index) + ".wire");
          std::ofstream file(path, std::ios::binary | std::ios::trunc);
          file.write(reinterpret_cast<const char*>(wires[index].data()), wires[index].size());
          file.close();
          if (!file) throw std::runtime_error("cannot save generated Data wire");
        }
        const auto& certBlock = certificate.wireEncode();
        const std::vector<uint8_t> certBytes(certBlock.begin(), certBlock.end());
        std::ofstream certFile(localCertificate, std::ios::binary | std::ios::trunc);
        certFile.write(reinterpret_cast<const char*>(certBytes.data()), certBytes.size());
        certFile.close();
        std::ofstream manifestFile(localManifest, std::ios::binary | std::ios::trunc);
        manifestFile << manifest.toJson();
        manifestFile.close();
        if (!certFile || !manifestFile) {
          throw std::runtime_error("cannot save payload manifest or certificate");
        }
        std::cout << "SPEC193_INSERT_PAYLOAD_DONE count=" << wires.size()
                  << " signer=" << certificate.getIdentity()
                  << " operation=" << status.operationId << std::endl;
        return 0;
      }
      if (localCommand == "payload-reject") {
        const auto manifest = repo_api::RepoClient::getManifest(*repo, localObject);
        const auto original = repo_api::RepoClient::getDataPackets(*repo, manifest);
        if (original.size() < 3 || manifest.packetNames.size() != original.size()) {
          throw std::runtime_error("O12 requires an existing segmented Data object");
        }
        const auto unchanged = [&] {
          return repo_api::RepoClient::getManifest(*repo, localObject).toJson() ==
                   manifest.toJson() &&
                 repo_api::RepoClient::getDataPackets(*repo, manifest) == original;
        };
        const auto expectRejected = [&] (const char* label,
                                         const repo_api::RepoOperationStatus& status,
                                         const std::string& error) {
          const auto queried = repo_api::RepoClient::status(*repo, status.operationId);
          if (status.state != "FAILED" || queried.state != "FAILED" ||
              status.completedSegments != 0 ||
              status.message.find(error) == std::string::npos || !unchanged()) {
            throw std::runtime_error(std::string("O12 rejection or original object changed: ") +
                                     label + " " + status.toJson());
          }
          std::cout << "SPEC193_O12_REJECT " << label << " " << status.toJson()
                    << std::endl;
        };

        auto conflictingPayload = payload;
        conflictingPayload.front() ^= 0x01;
        ndn::security::KeyChain keyChain;
        const auto identity = keyChain.getPib().getIdentity(config.providerIdentity);
        const auto certificate = identity.getDefaultKey().getDefaultCertificate();
        expectRejected("same-name-new-content",
          repo_api::RepoClient::insertPayload(
            *repo, localObject, conflictingPayload, keyChain,
            ndn::security::signingByIdentity(config.providerIdentity),
            {}, segmentBytes), "repo-data-wire-conflict");

        repo_api::RepoDataReference reference;
        reference.objectName = localObject + "/rejected-name";
        reference.dataPrefix = localObject + "/wrong-prefix";
        reference.storeWirePackets = true;
        reference.objectType = "ndn-segmented-data";
        expectRejected("declared-name-mismatch",
          repo->insertWirePackets(reference, {original.front()}),
          "Data name is outside declared dataPrefix");

        auto badSignature = original.front();
        badSignature.back() ^= 0x01;
        ndn::Block originalBlock(ndn::span<const uint8_t>(original.front().data(),
                                                           original.front().size()));
        ndn::Block changedBlock(ndn::span<const uint8_t>(badSignature.data(),
                                                          badSignature.size()));
        originalBlock.parse();
        changedBlock.parse();
        const ndn::Data signedData(originalBlock);
        const ndn::Data alteredData(changedBlock);
        if (signedData.getName() != alteredData.getName() ||
            !ndn::security::verifySignature(signedData, certificate) ||
            ndn::security::verifySignature(alteredData, certificate)) {
          throw std::runtime_error("O12 signature alteration was not isolated");
        }
        reference.objectName = localObject;
        reference.dataPrefix = localObject;
        expectRejected("same-name-bad-signature",
          repo->insertWirePackets(reference, {badSignature}),
          "repo-data-wire-conflict");
        std::cout << "SPEC193_O12_REJECTIONS_DONE" << std::endl;
        return 0;
      }
      if (localCommand == "cache-seed") {
        if (localManifest.empty() || payload.size() != 257) {
          throw std::invalid_argument("O13 cache seed needs a 257-byte source and report");
        }
        const auto before = repo_api::RepoClient::cacheStatus(*repo);
        repo_api::StoreOptions options;
        options.objectType = "o13-cache-object";
        const auto manifest = repo_api::RepoClient::put(*repo, localObject, payload, options);
        const auto afterPut = repo_api::RepoClient::cacheStatus(*repo);
        const auto first = repo_api::RepoClient::get(*repo, localObject);
        const auto second = repo_api::RepoClient::get(*repo, localObject);
        const auto afterReads = repo_api::RepoClient::cacheStatus(*repo);
        const auto charge = payload.size() + localObject.size() + manifest.toJson().size();
        if (manifest.size != payload.size() || first != payload || second != payload ||
            before.entryCount != 0 || before.usedBytes != 0 ||
            afterPut.entryCount != 1 || afterPut.usedBytes != charge ||
            afterPut.admissions != before.admissions + 1 ||
            afterPut.backingWrites != before.backingWrites + 1 ||
            afterReads.hits != afterPut.hits + 2 ||
            afterReads.entryCount != 1 || afterReads.usedBytes != charge ||
            afterReads.toJson() != repo->cacheStatus().toJson()) {
          throw std::runtime_error("O13 local cache status disagrees with write/read effects");
        }
        std::ofstream report(localManifest, std::ios::binary | std::ios::trunc);
        report << "{\"manifest\":" << manifest.toJson()
               << ",\"before\":" << before.toJson()
               << ",\"afterPut\":" << afterPut.toJson()
               << ",\"afterReads\":" << afterReads.toJson()
               << ",\"expectedChargeBytes\":" << charge << "}";
        report.close();
        if (!report) throw std::runtime_error("cannot save O13 cache report");
        std::cout << "SPEC193_O13_LOCAL_CACHE_PASS" << std::endl;
        return 0;
      }
      if (localCommand == "packet-set-seed" ||
          localCommand == "packet-set-missing" ||
          localCommand == "packet-set-restore") {
        if (localManifest.empty() || payload.size() != 513) {
          throw std::invalid_argument("packet set needs a manifest path and 513 source bytes");
        }
        const std::array<int, 3> order{{2, 0, 1}};
        const auto packetName = [&] (int index) {
          return localObject + "/pkt/" + std::to_string(index);
        };
        const auto wirePath = [&] (int index) {
          return std::filesystem::path(localManifest).parent_path() /
                 ("o09-packet-" + std::to_string(index) + ".wire");
        };
        const auto readWire = [&] (int index) {
          std::ifstream file(wirePath(index), std::ios::binary);
          if (!file) throw std::runtime_error("cannot open packet set wire file");
          return std::vector<uint8_t>(std::istreambuf_iterator<char>{file}, {});
        };
        if (localCommand == "packet-set-seed") {
          if (localCertificate.empty()) {
            throw std::invalid_argument("packet set seed needs certificate output");
          }
          ndn::security::KeyChain keyChain;
          const auto identity = keyChain.getPib().getIdentity(config.providerIdentity);
          const auto certificate = identity.getDefaultKey().getDefaultCertificate();
          for (int index = 0; index < 3; ++index) {
            ndn::Data data{ndn::Name(packetName(index))};
            const auto start = static_cast<size_t>(index) * 171;
            data.setContent(ndn::span<const uint8_t>(payload.data() + start, 171));
            keyChain.sign(data, ndn::security::signingByIdentity(config.providerIdentity));
            const auto& encoded = data.wireEncode();
            const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
            (void)repo_api::RepoClient::putDataPacket(*repo, wire);
            std::ofstream file(wirePath(index), std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(wire.data()), wire.size());
            file.close();
            if (!file) throw std::runtime_error("cannot write packet set wire file");
          }
          const auto& certBlock = certificate.wireEncode();
          const std::vector<uint8_t> certBytes(certBlock.begin(), certBlock.end());
          std::ofstream certFile(localCertificate, std::ios::binary | std::ios::trunc);
          certFile.write(reinterpret_cast<const char*>(certBytes.data()), certBytes.size());
          certFile.close();
          if (!certFile) throw std::runtime_error("cannot write packet set certificate");
          std::vector<uint8_t> concatenated;
          std::vector<std::string> packetNames;
          std::vector<std::vector<uint8_t>> orderedWires;
          for (int index : order) {
            auto wire = readWire(index);
            concatenated.insert(concatenated.end(), wire.begin(), wire.end());
            packetNames.push_back(packetName(index));
            orderedWires.push_back(std::move(wire));
          }
          auto manifest = repo_api::RepoClient::makeManifest(
            localObject, "ndn-data-packet-set", concatenated, 1, {}, "");
          manifest.segmentCount = 3;
          manifest.packetNames = packetNames;
          manifest = repo->putManifest(manifest);
          if (manifest.packetNames != packetNames ||
              repo_api::RepoClient::getDataPackets(*repo, manifest) != orderedWires) {
            throw std::runtime_error("packet set local read changed manifest order or wire");
          }
          std::ofstream manifestFile(localManifest, std::ios::binary | std::ios::trunc);
          manifestFile << manifest.toJson();
          manifestFile.close();
          if (!manifestFile) throw std::runtime_error("cannot write packet set manifest");
          std::cout << "SPEC193_PACKET_SET_SEEDED count=3 order=2,0,1" << std::endl;
          return 0;
        }
        const auto manifest = repo_api::RepoClient::getManifest(*repo, localObject);
        if (localCommand == "packet-set-missing") {
          if (!repo_api::RepoClient::remove(*repo, packetName(1))) {
            throw std::runtime_error("packet set missing fault removed no packet");
          }
          try {
            (void)repo_api::RepoClient::getDataPackets(*repo, manifest);
          }
          catch (const std::exception& error) {
            std::cout << "SPEC193_PACKET_SET_REJECTED missing=1 reason="
                      << error.what() << std::endl;
            return 0;
          }
          throw std::runtime_error("getDataPackets returned incomplete packet set");
        }
        (void)repo_api::RepoClient::putDataPacket(*repo, readWire(1));
        std::vector<std::vector<uint8_t>> orderedWires;
        for (int index : order) orderedWires.push_back(readWire(index));
        if (repo_api::RepoClient::getDataPackets(*repo, manifest) != orderedWires) {
          throw std::runtime_error("restored packet set changed order or wire");
        }
        std::cout << "SPEC193_PACKET_SET_RESTORED missing=1" << std::endl;
        return 0;
      }
      if (localCommand == "data-seed" || localCommand == "data-legacy-seed" ||
          localCommand == "data-cache-seed") {
        if (localManifest.empty() || localCertificate.empty()) {
          throw std::invalid_argument("Data seed needs wire and certificate output paths");
        }
        ndn::security::KeyChain keyChain;
        const auto providerIdentity = keyChain.getPib().getIdentity(config.providerIdentity);
        const auto certificate = providerIdentity.getDefaultKey().getDefaultCertificate();
        ndn::Data data{ndn::Name(localObject)};
        data.setContent(ndn::span<const uint8_t>(payload.data(), payload.size()));
        if (localCommand == "data-cache-seed") {
          data.setFreshnessPeriod(ndn::time::seconds(120));
        }
        keyChain.sign(data, ndn::security::signingByIdentity(config.providerIdentity));
        const auto& encoded = data.wireEncode();
        const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
        const auto manifest = localCommand == "data-legacy-seed" ?
          repo->core().putDataPacket(localObject, wire) :
          repo_api::RepoClient::putDataPacket(*repo, wire);
        if (manifest.objectName != localObject ||
            manifest.objectType != "ndn-data-wire" ||
            manifest.packetNames != std::vector<std::string>{localObject} ||
            manifest.size != wire.size() ||
            (localCommand == "data-legacy-seed" && !manifest.dataReceiptId.empty()) ||
            repo_api::RepoClient::getDataPacket(*repo, localObject) != wire) {
          throw std::runtime_error("signed Data wire did not round-trip through RepoClient");
        }
        std::ofstream wireOutput(localManifest, std::ios::binary | std::ios::trunc);
        wireOutput.write(reinterpret_cast<const char*>(wire.data()), wire.size());
        wireOutput.close();
        const auto& certWire = certificate.wireEncode();
        const std::vector<uint8_t> certBytes(certWire.begin(), certWire.end());
        std::ofstream certOutput(localCertificate, std::ios::binary | std::ios::trunc);
        certOutput.write(reinterpret_cast<const char*>(certBytes.data()), certBytes.size());
        certOutput.close();
        if (!wireOutput || !certOutput) {
          throw std::runtime_error("cannot write signed Data or signing certificate");
        }
        std::cout << "SPEC193_DATA_PACKET_SEEDED name=" << localObject
                  << " bytes=" << wire.size() << " locator="
                  << data.getSignatureInfo().getKeyLocator().getName() << std::endl;
        return 0;
      }
      if (localCommand == "data-legacy-import") {
        const auto before = repo->getManifest(localObject);
        if (before.objectType != "ndn-data-wire" || !before.dataReceiptId.empty() ||
            repo->getDataPacket(localObject) != payload) {
          throw std::runtime_error("legacy Data import precondition failed");
        }
        const auto receipt = repo->importLegacyDataPacketReceipt(localObject);
        const auto again = repo->importLegacyDataPacketReceipt(localObject);
        if (receipt.operationId.rfind("repo-import-data-", 0) != 0 ||
            again.operationId != receipt.operationId ||
            receipt.digest != before.sha256 ||
            repo->getDataPacket(localObject) != payload) {
          throw std::runtime_error("legacy Data import changed wire or receipt");
        }
        std::cout << "SPEC193_LEGACY_DATA_IMPORTED name=" << localObject
                  << " receipt=" << receipt.operationId << std::endl;
        return 0;
      }
      if (localCommand == "data-wire-seed") {
        const auto manifest = repo_api::RepoClient::putDataPacket(*repo, payload);
        if (manifest.objectName != localObject ||
            manifest.objectType != "ndn-data-wire" ||
            repo_api::RepoClient::getDataPacket(*repo, localObject) != payload) {
          throw std::runtime_error("original signed Data wire copy differs");
        }
        std::cout << "SPEC193_DATA_WIRE_REPLICA_SEEDED name=" << localObject
                  << " bytes=" << payload.size() << std::endl;
        return 0;
      }
      if (payload.size() <= segmentBytes * 2) {
        throw std::invalid_argument("local segmented source needs at least three segments");
      }
      const auto segmentName = localObject + "/seg/1";
      const auto start = segmentBytes;
      const auto end = std::min(payload.size(), start + segmentBytes);
      const std::vector<uint8_t> segment(payload.begin() + start, payload.begin() + end);
      if (localCommand == "seed") {
        auto manifest = repo_api::RepoClient::putSegmented(
          *repo, localObject, payload, {}, segmentBytes);
        if (manifest.segmentCount < 3 ||
            repo_api::RepoClient::getObject(*repo, manifest) != payload ||
            localManifest.empty()) {
          throw std::runtime_error("local segmented seed did not round-trip all bytes");
        }
        std::ofstream manifestOutput(localManifest, std::ios::binary | std::ios::trunc);
        manifestOutput << manifest.toJson();
        if (!manifestOutput) throw std::runtime_error("cannot write local segmented manifest");
        std::cout << "SPEC193_SEGMENTED_SEEDED count=" << manifest.segmentCount
                  << " bytes=" << payload.size() << std::endl;
      }
      else if (localCommand == "duplicate") {
        const auto manifest = repo_api::RepoClient::getManifest(*repo, localObject);
        const auto before = repo_api::RepoClient::list(*repo).size();
        (void)repo_api::RepoClient::put(*repo, segmentName, segment);
        if (repo_api::RepoClient::list(*repo).size() != before ||
            repo_api::RepoClient::getObject(*repo, manifest) != payload) {
          throw std::runtime_error("identical duplicate segment changed object inventory or bytes");
        }
        std::cout << "SPEC193_IDENTICAL_SEGMENT_DEDUPED index=1" << std::endl;
      }
      else if (localCommand == "restore") {
        const auto manifest = repo_api::RepoClient::getManifest(*repo, localObject);
        (void)repo_api::RepoClient::put(*repo, segmentName, segment);
        if (repo_api::RepoClient::getObject(*repo, manifest) != payload) {
          throw std::runtime_error("restored segment did not recover original object");
        }
        std::cout << "SPEC193_SEGMENTED_RESTORED index=1" << std::endl;
      }
      else if (localCommand == "missing" || localCommand == "corrupt") {
        const auto manifest = repo_api::RepoClient::getManifest(*repo, localObject);
        if (localCommand == "missing") {
          if (!repo_api::RepoClient::remove(*repo, segmentName)) {
            throw std::runtime_error("segment removal did not remove an existing segment");
          }
        }
        else {
          auto corrupt = segment;
          corrupt.front() ^= 0xff;
          (void)repo_api::RepoClient::put(*repo, segmentName, corrupt);
        }
        try {
          (void)repo_api::RepoClient::getObject(*repo, manifest);
        }
        catch (const std::exception& error) {
          std::cout << "SPEC193_SEGMENTED_REJECTED fault=" << localCommand
                    << " reason=" << error.what() << std::endl;
          return 0;
        }
        throw std::runtime_error("getObject returned bytes from an incomplete segmented object");
      }
      else {
        throw std::invalid_argument("unknown local segmented command: " + localCommand);
      }
      return 0;
    }

    ndn::Face face;
    ndn::security::KeyChain keyChain;
    auto providerIdentity = keyChain.getPib().getIdentity(config.providerIdentity);
    keyChain.setDefaultIdentity(providerIdentity);
    const auto providerCert = providerIdentity.getDefaultKey().getDefaultCertificate();
    const auto authorityCert = keyChain.getPib()
      .getIdentity(config.authorityIdentity).getDefaultKey().getDefaultCertificate();
    ndn_service_framework::CertificatePublisher certificatePublisher(
      face, keyChain, providerCert.getName());
    (void)certificatePublisher;
    ndn_service_framework::ServiceProvider provider(
      face, config.groupPrefix, providerCert, authorityCert,
      config.trustSchemaPath.string());
    repo->setDataReferenceFetcher([] (const ndnsf_distributed_repo::RepoDataReference& reference) {
      if (!reference.hasFinalSegment || reference.finalSegment < reference.firstSegment ||
          reference.finalSegment - reference.firstSegment >= 16) {
        throw std::invalid_argument("test Data fetcher needs 1-16 bounded segments");
      }
      ndn::Face fetchFace;
      std::vector<std::vector<uint8_t>> wires;
      for (uint64_t index = reference.firstSegment; index <= reference.finalSegment; ++index) {
        const auto expectedName = ndn::Name(reference.dataPrefix).appendSegment(index);
        ndn::Interest interest(expectedName);
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(3000));
        bool finished = false;
        std::string failure;
        std::vector<uint8_t> wire;
        auto pending = fetchFace.expressInterest(
          interest,
          [&] (const ndn::Interest&, const ndn::Data& data) {
            if (data.getName() != expectedName) {
              failure = "fetched Data name differs from exact reference";
            }
            else {
              const auto& block = data.wireEncode();
              wire.assign(block.begin(), block.end());
            }
            finished = true;
          },
          [&] (const ndn::Interest&, const ndn::lp::Nack&) {
            failure = "Data reference fetch received Nack";
            finished = true;
          },
          [&] (const ndn::Interest&) {
            failure = "Data reference fetch timed out";
            finished = true;
          });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
          fetchFace.processEvents(ndn::time::milliseconds(25));
          fetchFace.getIoContext().restart();
        }
        if (!finished || !failure.empty()) {
          throw std::runtime_error(failure.empty() ? "Data reference fetch deadline" : failure);
        }
        wires.push_back(std::move(wire));
      }
      return wires;
    });
    repo->registerServices(provider);
    if (responseFault == "malformed" || responseFault == "hold") {
      provider.setHandlerThreads(1);
      provider.addService(ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "STATUS"),
        [] (const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "spec193-fault-ready";
          decision.pendingStateTtlMs = 60000;
          return decision;
        },
        [&face, &provider, &responseFault] (const ndn::Name& requester, const ndn::Name& selectedProvider,
                           const ndn::Name& service, const ndn::Name& requestId,
                           const ndn_service_framework::RequestMessage&) {
          const auto name = ndn_service_framework::makeResponseNameV2(selectedProvider, requester, service, requestId);
          std::cout << "SPEC193_FAULT_SELECTED requestId=" << requestId
                    << " requester=" << requester << " response=" << name << std::endl;
          if (responseFault == "malformed") {
            face.getIoContext().post([&provider, name, requestId] {
              ndn::Buffer malformed{0xff};
              provider.publishHybridEncodedMessage(name, std::move(malformed));
              std::cout << "SPEC193_FAULT_PUBLISHED requestId=" << requestId << " plaintextHex=ff" << std::endl;
            });
          }
          std::this_thread::sleep_for(std::chrono::seconds(15));
          ndn_service_framework::ResponseMessage response;
          response.setStatus(false);
          response.setErrorInfo("fault observation window closed");
          return response;
        }, ndn_service_framework::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    }
    repo->registerOriginalDataServing(face);
    provider.setProviderRequestLifecycleCallback(
      [prefix = config.servicePrefix] (
        const ndn_service_framework::ServiceProvider::ProviderRequestLifecycleStatus& status) {
        if (status.requestId.toUri().find("/spec193-server-auth-") != 0 ||
            (status.serviceName != ndnsf_distributed_repo::makeRepoServiceName(prefix, "DATA_RECEIPT") &&
             status.serviceName != ndnsf_distributed_repo::makeRepoServiceName(prefix, "DATA_LOCATION"))) {
          return;
        }
        std::cout << "SPEC202_METADATA_AUTH_EVENT"
                  << " requestId=" << status.requestId
                  << " serviceName=" << status.serviceName
                  << " provider=" << status.providerName
                  << " state=" << ndn_service_framework::ServiceProvider::providerRequestLifecycleStateToString(status.state)
                  << std::endl;
      });
    provider.init();
    provider.fetchPermissionsFromController(config.authorityIdentity);

    ndn::Scheduler faultScheduler(face.getIoContext());
    std::function<void()> pollFault;
    unsigned faultPollCount = 0;
    if (responseFault == "wrong-signer") {
      pollFault = [&] {
        if (std::filesystem::exists(faultResponseFile)) {
          std::ifstream input(faultResponseFile);
          std::string encodedName;
          std::getline(input, encodedName);
          const ndn::Name name(encodedName);
          const auto parsed = ndn_service_framework::parseResponseNameV2(name);
          if (!parsed || parsed->providerName == config.providerIdentity)
            throw std::runtime_error("fault gate must claim a different Provider");
          ndn_service_framework::ResponseMessage response;
          response.setStatus(true);
          ndn::Buffer payload{0x42};
          response.setPayload(payload, payload.size());
          const auto block = response.WireEncode();
          provider.publishHybridEncodedMessage(name, ndn::Buffer(block.begin(), block.end()));
          std::cout << "SPEC193_WRONG_SIGNER_PUBLISHED requestId=" << parsed->requestId
                    << " signer=" << config.providerIdentity << " response=" << name << std::endl;
          return;
        }
        if (++faultPollCount >= 600)
          throw std::runtime_error("wrong-signer gate expired");
        faultScheduler.schedule(ndn::time::milliseconds(100), pollFault);
      };
      faultScheduler.schedule(ndn::time::milliseconds(100), pollFault);
    }

    std::cout << "Spec193 object Repo serving " << repo->servicePrefix()
              << " as " << config.repoNode << " using " << storeRoot << std::endl;
    face.processEvents();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "Spec193 object Repo server error: " << error.what() << std::endl;
    return 2;
  }
}
