#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace ndnsf_distributed_repo;

namespace {

void
require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<uint8_t>
readWire(const std::filesystem::path& path)
{
  require(std::filesystem::is_regular_file(path) &&
          !std::filesystem::is_symlink(path) &&
          std::filesystem::file_size(path) <= (1U << 20), "invalid oracle wire file");
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "cannot open oracle wire");
  std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>{});
  require(!input.bad() && !bytes.empty(), "cannot read oracle wire");
  return bytes;
}

void
writeWire(const std::filesystem::path& path, const ndn::Block& wire)
{
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wire.data()), wire.size());
  output.close();
  require(output.good(), "cannot write oracle wire");
}

int
seedOriginalPackets(const std::filesystem::path& root, const ndn::Name& producer)
{
  using namespace ndn_service_framework;
  require(!producer.empty(), "Producer prefix must be complete and non-root");
  // Never overwrite another run, follow a symlink, or remove evidence on failure.
  require(::mkdir(root.c_str(), 0700) == 0, "seed root must be a fresh directory");
  std::filesystem::create_directory(root / "originals");
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const auto identity = keys.createIdentity(producer);
  const auto certificate = identity.getDefaultKey().getDefaultCertificate();
  writeWire(root / "producer.cert", certificate.wireEncode());
  {
    std::ofstream producerRecord(root / "producer-prefix.txt");
    producerRecord << producer.toUri(ndn::name::UriFormat::CANONICAL) << '\n';
    producerRecord.close();
    require(producerRecord.good(), "cannot write Producer identity record");
  }
  const std::vector<uint8_t> payload(12000, 0x5a);
  const std::string representation = "{\"encoding\":\"raw\",\"segmentBytes\":4000}";
  PublishedContentName semantics;
  semantics.kind = ContentDataKind::Blob;
  semantics.label = "replica-oracle";
  auto prefix = semantics.contentPrefix(producer, sha256Hex(payload));
  prefix.append("REPRESENTATION")
    .append(makeNamingDigestComponent(computeNamingDigest("ndnsf-naming/representation",
      {{1, 5, std::vector<uint8_t>(representation.begin(), representation.end())}})))
    .append("DATA");
  checkNamingNameSize(ndn::Name(prefix).appendSegment(2));
  std::vector<std::pair<ndn::Name, std::vector<uint8_t>>> packets;
  std::ofstream index(root / "packets.tsv");
  for (uint64_t segment = 0; segment < 3; ++segment) {
    ndn::Data data(ndn::Name(prefix).appendSegment(segment));
    data.setFinalBlock(ndn::name::Component::fromSegment(2));
    data.setFreshnessPeriod(ndn::time::milliseconds(0));
    data.setContent(ndn::span<const uint8_t>(payload.data() + segment * 4000, 4000));
    keys.sign(data, ndn::security::signingByIdentity(producer));
    require(producer.isPrefixOf(data.getName()) &&
            ndn::security::verifySignature(data, identity.getDefaultKey()),
            "Producer signature or namespace mismatch");
    const auto& wire = data.wireEncode();
    packets.emplace_back(data.getName(), std::vector<uint8_t>(wire.begin(), wire.end()));
    writeWire(root / "originals" / (std::to_string(segment) + ".wire"), wire);
    index << data.getName().toUri(ndn::name::UriFormat::CANONICAL) << '\t'
          << sha256Hex(packets.back().second) << '\t' << segment << ".wire\n";
  }
  index.close();
  require(index.good(), "cannot write original packet index");
  for (const std::string replica : {"r1", "r2"}) {
    StorageCapability capacity;
    capacity.repoNode = "/spec201/" + replica;
    capacity.freeBytes = 1U << 20;
    const auto storePath = root / replica;
    {
      RepoCore store(capacity, std::make_shared<FilesystemRepoStoreBackend>(storePath.string()));
      for (const auto& packet : packets) {
        store.putDataPacket(packet.first.toUri(ndn::name::UriFormat::CANONICAL), packet.second);
        require(store.getDataPacket(packet.first.toUri()) == packet.second,
                "replica changed original wire");
      }
    }
    {
      RepoCore reopened(capacity, std::make_shared<FilesystemRepoStoreBackend>(storePath.string()));
      for (const auto& packet : packets)
        require(reopened.getDataPacket(packet.first.toUri()) == packet.second,
                "replica restart changed original wire");
    }
  }
  std::cout << "ORIGINAL_PACKET_SEED_PASS packets=" << packets.size() << " replicas=2\n";
  return 0;
}

int
verifyOriginalPackets(const std::filesystem::path& root, const std::filesystem::path& fetched)
{
  const auto certWire = readWire(root / "producer.cert");
  const ndn::security::Certificate certificate(
    ndn::Block(ndn::span<const uint8_t>(certWire.data(), certWire.size())));
  require(ndn::security::verifySignature(certificate, std::optional<ndn::security::Certificate>(certificate)),
          "invalid seed certificate signature");
  std::ifstream producerRecord(root / "producer-prefix.txt");
  std::string producer;
  require(producerRecord.good() && bool(std::getline(producerRecord, producer)) &&
          producerRecord.peek() == std::char_traits<char>::eof() &&
          ndn::Name(producer) == certificate.getIdentity(),
          "Producer identity record differs from certificate");
  std::ifstream index(root / "packets.tsv");
  require(index.good(), "cannot read original packet index");
  std::string line;
  size_t count = 0;
  while (std::getline(index, line)) {
    std::istringstream fields(line);
    std::string name, digest, filename;
    require(bool(std::getline(fields, name, '\t')) && bool(std::getline(fields, digest, '\t')) &&
            bool(std::getline(fields, filename)) && filename == std::to_string(count) + ".wire",
            "invalid ordered packet index");
    const auto original = readWire(root / "originals" / filename);
    const auto wire = readWire(fetched / filename);
    require(wire == original && sha256Hex(wire) == digest, "fetched wire differs from Producer original");
    const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    require(data.getName() == ndn::Name(name) && certificate.getIdentity().isPrefixOf(data.getName()) &&
            ndn::security::verifySignature(data, std::optional<ndn::security::Certificate>(certificate)),
            "fetched Name, Producer or signature mismatch");
    ++count;
  }
  require(!index.bad() && count == 3, "incomplete fetched packet set");
  std::cout << "ORIGINAL_PACKET_WIRE_ORACLE_PASS packets=" << count << '\n';
  return 0;
}

std::string
packetName(const std::vector<uint8_t>& wire)
{
  ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
  block.parse();
  return ndn::Data(block).getName().toUri(ndn::name::UriFormat::CANONICAL);
}

template<typename F>
void
requireThrowsContaining(F&& operation,
                        const std::string& expected,
                        const std::string& message)
{
  try {
    operation();
  }
  catch (const std::exception& e) {
    require(std::string(e.what()).find(expected) != std::string::npos,
            message + ": " + e.what());
    return;
  }
  throw std::runtime_error(message + ": no exception");
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 4) {
    try {
      if (std::string(argv[1]) == "--seed-original-packets")
        return seedOriginalPackets(argv[2], ndn::Name(argv[3]));
      if (std::string(argv[1]) == "--verify-original-packets")
        return verifyOriginalPackets(argv[2], argv[3]);
    }
    catch (const std::exception& error) {
      std::cerr << "ORIGINAL_PACKET_ORACLE_FAIL " << error.what() << '\n';
      return 1;
    }
  }
  if (argc != 1) {
    std::cerr << "Usage: DistributedRepoExactPacketTest [--seed-original-packets ROOT PRODUCER | --verify-original-packets ROOT FETCH_DIR]\n";
    return 2;
  }
  char directory[] = "/tmp/ndnsf-repo-exact-packet-XXXXXX";
  if (!::mkdtemp(directory)) throw std::runtime_error("cannot create exact packet fixture");
  const std::filesystem::path root(directory);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
  const auto openStore = [&] { return makeTieredRepoStore(
    makeFilesystemRepoStore((root / "store").string(), 1U << 20, 1U << 20), 4096); };

  StorageCapability capability;
  capability.repoNode = "/repo/exact";
  capability.freeBytes = 8 * 1024 * 1024;
  capability.storageClasses = {"ndn-data"};

  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  const ndn::Name identityName = ndn::Name("/test/repo/exact-publisher").append(std::to_string(::getpid()));
  try {
    keyChain.createIdentity(identityName);
  }
  catch (const std::exception&) {
  }
  const ndn::security::SigningInfo signingInfo(
    ndn::security::SigningInfo::SIGNER_TYPE_ID, identityName);
  const std::vector<uint8_t> payload(18000, 0x5a);
  using namespace ndn_service_framework;
  const std::string representation = "{\"encoding\":\"raw\",\"segmentBytes\":4096}";
  auto prefix = makeProducerDataPrefix(identityName, "BLOB");
  prefix.append(makeNamingDigestComponent(sha256Hex(payload))).append("REPRESENTATION")
    .append(makeNamingDigestComponent(computeNamingDigest("ndnsf-naming/representation",
      {{1, 5, std::vector<uint8_t>(representation.begin(), representation.end())}}))).append("DATA");
  const auto versionedName = prefix.toUri(ndn::name::UriFormat::CANONICAL);
  const auto rootName = ndn::Name(prefix).appendSegment(0).toUri(ndn::name::UriFormat::CANONICAL);

  RepoObjectManifest storedManifest;
  std::vector<std::vector<uint8_t>> originalWires;
  {
    RepoNode node(ndn::Name("/NDNSF/DistributedRepo"), capability,
                  openStore());
    const auto status = RepoClient::insertPayload(
      node, versionedName, payload, keyChain, signingInfo, {}, 4096);
    require(status.state == "DONE", "exact packet insertion failed: " + status.message);

    storedManifest = RepoClient::getManifest(node, rootName);
    require(storedManifest.segmentCount > 1, "test payload was not segmented");
    require(storedManifest.packetNames.size() == storedManifest.segmentCount,
            "manifest does not index every exact Data name");
    for (const auto& name : storedManifest.packetNames) {
      require(ndn::Name(name).getPrefix(-1) == prefix && ndn::Name(name)[-1].isSegment(),
              "Repo changed original versioned Data name: " + name);
      auto wire = RepoClient::getDataPacket(node, name);
      require(packetName(wire) == name, "stored wire name differs from exact key");
      originalWires.push_back(std::move(wire));
    }
    require(RepoClient::getDataPackets(node, storedManifest) == originalWires,
            "packet-set consumer changed order or wire bytes");
    require(RepoClient::insertPayload(node, versionedName, payload, keyChain, signingInfo, {}, 4096).state == "DONE",
            "Producer payload reuse failed");
    require(RepoClient::getDataPackets(node, storedManifest) == originalWires,
            "Producer payload reuse re-signed an immutable packet");
    requireThrowsContaining(
      [&] { RepoClient::insertPayload(node, "/foreign/producer/NDNSF/BLOB/DATA", payload,
                                      keyChain, signingInfo, {}, 4096); },
      "repo-publisher-prefix-mismatch", "foreign Producer prefix was accepted");

    auto wrongCount = storedManifest;
    ++wrongCount.segmentCount;
    requireThrowsContaining(
      [&] { RepoClient::getDataPackets(node, wrongCount); },
      "repo-packet-index-invalid",
      "packet-set consumer accepted count mismatch");

    auto duplicate = storedManifest;
    duplicate.packetNames.back() = duplicate.packetNames.front();
    requireThrowsContaining(
      [&] { RepoClient::getDataPackets(node, duplicate); },
      "duplicate",
      "packet-set consumer accepted duplicate name");

    auto missing = storedManifest;
    missing.packetNames.back() = versionedName + "/seg=999999";
    requireThrowsContaining(
      [&] { RepoClient::getDataPackets(node, missing); },
      "not an exact NDN Data packet",
      "packet-set consumer accepted missing packet");

    bool aliasFound = true;
    try {
      node.get(versionedName + "/ndn-data/0");
    }
    catch (const std::out_of_range&) {
      aliasFound = false;
    }
    require(!aliasFound, "packet API created a /ndn-data/N alias");

    RepoDataReference reference;
    reference.objectName = rootName;
    reference.dataPrefix = versionedName;
    reference.expectedSha256 = sha256Hex([&] {
      std::vector<uint8_t> joined;
      for (const auto& wire : originalWires) {
        joined.insert(joined.end(), wire.begin(), wire.end());
      }
      return joined;
    }());
    const auto repeated = node.insertWirePackets(reference, originalWires);
    require(repeated.state == "DONE", "idempotent packet insertion failed");

    auto conflicting = originalWires;
    conflicting.front().back() ^= 0x01;
    const auto conflict = node.insertWirePackets(reference, conflicting);
    require(conflict.state == "FAILED", "same-name different-wire conflict was accepted");
    require(conflict.message.find("conflict") != std::string::npos,
            "conflict failure did not explain immutable-name conflict");
  }

  {
    RepoNode restarted(ndn::Name("/NDNSF/DistributedRepo"), capability,
                       openStore());
    const auto manifest = RepoClient::getManifest(restarted, rootName);
    require(manifest.packetNames == storedManifest.packetNames,
            "packet-name index did not survive restart");
    require(RepoClient::getDataPackets(restarted, manifest) == originalWires,
            "packet-set consumer changed wire bytes across restart");
  }

  std::cout << "EXACT_DATA_PACKET_STORAGE_OK" << std::endl;
  return 0;
}
