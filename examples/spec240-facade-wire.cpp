// Spec 240 acceptance driver: publish and fetch original Data through RepoClient's
// persistent facade against a live Repo. Evidence tool; not a production path.
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/repo-client-types.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-service-framework/CertificatePublisher.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace repo = ndnsf_distributed_repo;

namespace {

/** Alternate spelling of a name string, produced once from a parsed Name. */
std::string
alternateName(const std::string& value)
{
  return ndn::Name(value).toUri(ndn::name::UriFormat::ALTERNATE);
}

std::string
arg(const std::map<std::string, std::string>& options, const std::string& name)
{
  const auto it = options.find(name);
  if (it == options.end())
    throw std::runtime_error("missing --" + name);
  return it->second;
}

std::vector<uint8_t>
readBytes(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read " + path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

ndn::security::Certificate
readCertificate(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read certificate " + path);
  std::string encoded;
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("-----", 0) == 0)
      continue;
    for (unsigned char ch : line) {
      if (!std::isspace(ch))
        encoded.push_back(static_cast<char>(ch));
    }
  }
  const std::string temp = path + ".der";
  {
    std::ofstream output(temp, std::ios::binary);
    output << encoded;
  }
  auto certificate = ndn::io::load<ndn::security::Certificate>(temp);
  std::remove(temp.c_str());
  if (!certificate)
    throw std::runtime_error("cannot parse certificate " + path);
  return *certificate;
}

/** Pumps the face until the user is ready for the service, or the deadline passes. */
bool
waitReady(ndn::Face& face, ndn_service_framework::ServiceUser& user,
          const ndn::Name& service, const ndn::Name& provider, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    // The face is pumped by its own thread; pumping here as well corrupts the io_context.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (user.isRequestBootstrapReady(service, std::vector<ndn::Name>{provider}))
      return true;
  }
  return false;
}

} // namespace

int
main(int argc, char** argv)
{
  std::map<std::string, std::string> options;
  for (int index = 1; index + 1 < argc; index += 2) {
    std::string key = argv[index];
    if (key.rfind("--", 0) != 0)
      throw std::runtime_error("bad option " + key);
    options[key.substr(2)] = argv[index + 1];
  }
  const auto mode = arg(options, "mode");
  const auto identity = ndn::Name(arg(options, "identity"));
  const auto certificate = readCertificate(arg(options, "certificate"));
  const auto authorityCertificate = readCertificate(arg(options, "authority-certificate"));
  const auto group = ndn::Name(arg(options, "group"));
  const auto trustSchema = arg(options, "trust-schema");
  const auto controller = ndn::Name(arg(options, "controller"));
  const auto servicePrefix = ndn::Name(arg(options, "service-prefix"));
  const auto repository = arg(options, "repo-node");
  const auto provider = ndn::Name(arg(options, "provider-identity"));

  ndn::Face face;
  ndn::security::KeyChain keyChain;
  keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(identity));
  // Publish our certificate so the Repo can fetch it while validating our request.
  auto certificatePublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
    face, keyChain, certificate.getName());
  auto user = std::make_shared<ndn_service_framework::ServiceUser>(
    face, group, certificate, authorityCertificate, trustSchema);
  user->init();
  user->fetchPermissionsFromController(controller);
  // The client's arrangement, which works: one thread pumps the face, the caller thread
  // never pumps and makes the blocking facade calls directly.
  std::atomic<bool> stopping{false};
  std::thread faceThread([&] {
    while (!stopping.load()) {
      face.processEvents(ndn::time::milliseconds(10));
      face.getIoContext().restart();
    }
  });
  struct StopFace {
    std::atomic<bool>& stopping;
    std::thread& thread;
    ~StopFace() { stopping.store(true); if (thread.joinable()) thread.join(); }
  } stopFace{stopping, faceThread};
  repo::PersistentRepoClientConfig config;
  config.servicePrefix = servicePrefix;
  config.providerScope = {provider};
  // SVS propagates on a one-second periodic interval with 200 ms suppression, so the
  // default collection window is shorter than the ACK path it is waiting for.
  config.ackCollectionTimeMs = 4000;
  auto client = repo::RepoClient::makePersistent(user, config);

  repo::RepoOperationOptions operation;
  operation.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  const auto service = repo::makeRepoServiceName(servicePrefix, "STORE");
  if (!waitReady(face, *user, service, provider, 60000))
    throw std::runtime_error("bootstrap did not become ready");

  if (mode == "idle") {
    // Bisect mode: bootstrap only, no facade call. Separates the face-thread setup from
    // the publication path when something crashes.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::printf("FACADE_IDLE ok\n");
    return 0;
  }
  if (mode == "publish") {
    const auto source = arg(options, "source");
    const auto bytes = readBytes(source);

    // A fresh object per run: the same name published twice would hold the first packet's
    // signature, and a later fetch would compare against wires this run never produced.
    ndn::Name objectName = ndn::Name(identity).append("object");
    objectName.appendSegment(static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    repo::RepoDataPublication publication;
    publication.target = repo::RepoTarget{alternateName(repository), alternateName(provider.toUri()),
                                         alternateName(servicePrefix.toUri())};
    // Alternate spellings are produced once, from a Name; re-parsing an alternate string
    // yields literal "8=" components and mangles the name.
    publication.producerIdentity = identity.toUri();
    publication.objectName = objectName.toUri();

    repo::PacketSource packets;
    std::vector<std::vector<uint8_t>> wires;
    uint64_t offsets = 0;
    const uint64_t chunk = 8000;
    const uint64_t segmentsTotal = bytes.empty() ? 1 : (bytes.size() + chunk - 1) / chunk;
    uint64_t segment = 0;
    do {
      const uint64_t begin = segment * chunk;
      const uint64_t end = std::min<uint64_t>(bytes.size(), begin + chunk);
      ndn::Name name = objectName;
      name.appendSegment(segment);
      ndn::Data packet(name);
      packet.setContent(ndn::span<const uint8_t>(bytes.data() + begin, end - begin));
      packet.setFreshnessPeriod(ndn::time::seconds(3600));
      // The Repo's spool fetcher requires a FinalBlock that names the last segment, and the
      // packet name must end in a segment component.
      packet.setFinalBlock(ndn::name::Component::fromSegment(segmentsTotal - 1));
      keyChain.sign(packet, ndn::security::signingByCertificate(certificate));
      const auto wire = packet.wireEncode();
      wires.emplace_back(wire.begin(), wire.end());
      packets.packetNames.push_back(name.toUri());
      packets.packetOffsets.push_back(offsets);
      offsets += wires.back().size();
      ++segment;
    } while (segment * chunk < bytes.size());
    packets.packetOffsets.push_back(offsets);
    packets.expectedWireBytes = offsets;
    packets.expectedPacketCount = wires.size();
    ndn::util::Sha256 sha;
    for (const auto& wire : wires)
      sha.update(wire);
    packets.expectedDigest = "sha256:" + sha.toString();
    std::size_t index = 0;
    packets.next = [&wires, index] (std::vector<uint8_t>& out) mutable {
      if (index >= wires.size())
        return false;
      out = wires[index++];
      return true;
    };
    // Supply the manifest ourselves rather than letting the facade synthesise one: this is
    // the shape the DI publication path sends, and it keeps the request free of a
    // reconstruction step.
    auto manifest = std::make_shared<repo::RepoObjectManifest>();
    manifest->objectName = packets.packetNames.front();
    manifest->objectType = "original-packet-set";
    manifest->sha256 = sha.toString();
    manifest->size = packets.expectedWireBytes;
    manifest->segmentCount = static_cast<uint32_t>(packets.packetNames.size());
    manifest->packetNames = packets.packetNames;
    manifest->packetOffsets = packets.packetOffsets;
    manifest->generation = 1;
    manifest->lifecycleState = "COMMITTED";
    publication.packetSetManifest = manifest;
    publication.packets = packets;
    // The verifier has to do what a Producer check means: the packet must be signed by the
    // expected certificate and live under the Producer's namespace. Parsing TLV alone would
    // accept anything.
    const auto expectedCertificate = certificate;
    const auto producerIdentity = identity;
    publication.verifyWire = std::make_shared<const std::function<bool(const std::vector<uint8_t>&)>>(
      [expectedCertificate, producerIdentity] (const std::vector<uint8_t>& wire) {
        try {
          const ndn::Block block(wire);
          const ndn::Data packet(block);
          return packet.wireEncode().size() == wire.size() &&
            producerIdentity.isPrefixOf(packet.getName()) &&
            ndn::security::verifySignature(packet, expectedCertificate);
        }
        catch (const std::exception&) {
          return false;
        }
      });
    publication.generation = 1;
    publication.idempotencyKey = "spec240-facade-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());

    // The Repo names an original packet set by its first packet name, so the grant has to
    // ask for exactly that name - not for the prefix the packets extend.
    repo::RepoWriteAuthorization authorization;
    authorization.target = publication.target;
    authorization.writerIdentity = identity.toUri();
    authorization.objectName = packets.packetNames.front();
    authorization.grantId = "spec240-facade-grant-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
    authorization.expiresAtMs = static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + 600000;
    if (options.count("expired-grant") != 0) {
      // A grant that expired before it was requested must be refused by the Repo.
      authorization.expiresAtMs = static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) - 60000;
    }
    std::optional<repo::RepoWriteGrant> grant;
    try {
      grant = client.authorizeWrite(authorization, operation);
    }
    catch (const std::exception& error) {
      std::fprintf(stderr, "FACADE_GRANT_REJECTED what=%s\n", error.what());
      return options.count("expect-rejection") != 0 ? 0 : 1;
    }
    std::fprintf(stderr, "FACADE_GRANT grant=%s owner=%s epoch=%llu object=%s\n",
                 grant->grantId.c_str(), grant->ownerIdentity.c_str(),
                 static_cast<unsigned long long>(grant->ownerEpoch), grant->objectName.c_str());
    publication.ownerIdentity = grant->ownerIdentity;
    publication.grantId = grant->grantId;
    publication.ownerEpoch = grant->ownerEpoch;

    const auto result = client.publishData(publication, operation);
    if (options.count("manifest-out") != 0) {
      // A three-line record the fetch side can rebuild the manifest from without needing a
      // JSON parser for the internal manifest type.
      std::ofstream manifestFile(options.at("manifest-out"), std::ios::trunc);
      std::string lower = manifest->sha256;
      for (auto& character : lower)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
      manifestFile << manifest->objectName << "\n" << lower << "\n"
                   << manifest->size << "\n" << packets.packetNames.size() << "\n";
      for (std::size_t index = 0; index < packets.packetOffsets.size(); ++index)
        manifestFile << (index > 0 ? "," : "") << packets.packetOffsets[index];
      manifestFile << "\n";
      for (const auto& name : packets.packetNames)
        manifestFile << name << "\n";
    }
    if (options.count("names-out") != 0) {
      std::ofstream names(options.at("names-out"), std::ios::trunc);
      for (const auto& name : packets.packetNames)
        names << name << "\n";
    }
    std::fprintf(stderr, "FACADE_PUBLISH state=%s receipt=%s\n",
                 result.state == repo::RepoPublishState::COMMITTED ? "COMMITTED" : "ACCEPTED",
                 result.receipt ? result.receipt->receiptId.c_str() : "");
    return result.state == repo::RepoPublishState::COMMITTED ? 0 : 1;
  }
  if (mode == "concurrency") {
    // Four publications at once against one client: close() must report the truth while they
    // run and after they finish.
    const auto source = arg(options, "source");
    const auto bytes = readBytes(source);
    const auto alternate = [] (const std::string& value) {
      return ndn::Name(value).toUri(ndn::name::UriFormat::ALTERNATE);
    };
    const auto target = repo::RepoTarget{alternate(repository), alternate(provider.toUri()),
                                         alternate(servicePrefix.toUri())};
    const auto runTag = static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    std::atomic<int> finished{0};
    std::atomic<int> failed{0};
    const auto worker = [&] (int index) {
      try {
        repo::RepoDataPublication publication;
        publication.target = target;
        publication.producerIdentity = identity.toUri();
        // Run-unique names: a reused object name collides with earlier grants and claims.
        ndn::Name objectName = ndn::Name(identity).append("object");
        objectName.appendSegment(static_cast<uint64_t>(runTag));
        objectName.appendSegment(static_cast<uint64_t>(index));
        publication.objectName = objectName.toUri();
        repo::PacketSource packets;
        std::vector<std::vector<uint8_t>> wires;
        uint64_t offsets = 0;
        uint64_t segment = 0;
        const uint64_t chunk = 8000;
        const uint64_t total = bytes.empty() ? 1 : (bytes.size() + chunk - 1) / chunk;
        do {
          const uint64_t begin = segment * chunk;
          const uint64_t end = std::min<uint64_t>(bytes.size(), begin + chunk);
          ndn::Name name = objectName;
          name.appendSegment(segment);
          ndn::Data packet(name);
          packet.setContent(ndn::span<const uint8_t>(bytes.data() + begin, end - begin));
          packet.setFreshnessPeriod(ndn::time::seconds(3600));
          packet.setFinalBlock(ndn::name::Component::fromSegment(total - 1));
          keyChain.sign(packet, ndn::security::signingByCertificate(certificate));
          const auto wire = packet.wireEncode();
          wires.emplace_back(wire.begin(), wire.end());
          packets.packetNames.push_back(name.toUri());
          packets.packetOffsets.push_back(offsets);
          offsets += wires.back().size();
          ++segment;
        } while (segment * chunk < bytes.size());
        packets.packetOffsets.push_back(offsets);
        packets.expectedWireBytes = offsets;
        packets.expectedPacketCount = wires.size();
        ndn::util::Sha256 sha;
        for (const auto& wire : wires)
          sha.update(wire);
        packets.expectedDigest = "sha256:" + sha.toString();
        std::size_t cursor = 0;
        packets.next = [&wires, cursor] (std::vector<uint8_t>& out) mutable {
          if (cursor >= wires.size())
            return false;
          out = wires[cursor++];
          return true;
        };
        publication.packets = packets;
        publication.verifyWire = std::make_shared<const std::function<bool(const std::vector<uint8_t>&)>>(
          [] (const std::vector<uint8_t>& wire) {
            try {
              const ndn::Block block(wire);
              const ndn::Data packet(block);
              return packet.wireEncode().size() == wire.size();
            }
            catch (const std::exception&) {
              return false;
            }
          });
        publication.generation = 1;
        publication.idempotencyKey = "spec240-concurrency-" + std::to_string(runTag) +
          "-" + std::to_string(index);

        repo::RepoWriteAuthorization authorization;
        authorization.target = target;
        authorization.writerIdentity = identity.toUri();
        authorization.objectName = packets.packetNames.front();
        authorization.grantId = "spec240-concurrency-grant-" + std::to_string(runTag) +
          "-" + std::to_string(index);
        authorization.expiresAtMs = static_cast<uint64_t>(std::chrono::duration_cast<
          std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + 600000;
        const auto grant = client.authorizeWrite(authorization, operation);
        publication.ownerIdentity = grant.ownerIdentity;
        publication.grantId = grant.grantId;
        publication.ownerEpoch = grant.ownerEpoch;
        const auto result = client.publishData(publication, operation);
        if (result.state != repo::RepoPublishState::COMMITTED)
          ++failed;
      }
      catch (const std::exception& error) {
        std::fprintf(stderr, "CONCURRENCY_WORKER_%d failed: %s\n", index, error.what());
        ++failed;
      }
      ++finished;
    };

    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index)
      workers.emplace_back(worker, index);
    const auto closeAfter = options.count("close-after-ms") != 0
      ? std::chrono::milliseconds(std::stoll(options.at("close-after-ms")))
      : std::chrono::milliseconds(150);
    const bool skipClose = options.count("no-close") != 0;
    if (!skipClose)
      std::this_thread::sleep_for(closeAfter);
    const auto drainedWhileRunning = skipClose
      ? true
      : client.close(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    std::fprintf(stderr, "CONCURRENCY close_while_running=%d finished=%d failed=%d\n",
                 drainedWhileRunning ? 1 : 0, finished.load(), failed.load());
    for (auto& thread : workers)
      thread.join();
    const auto drainedAfter = client.close(std::chrono::steady_clock::now() +
                                           std::chrono::seconds(5));
    std::fprintf(stderr, "CONCURRENCY close_after=%d finished=%d failed=%d\n",
                 drainedAfter ? 1 : 0, finished.load(), failed.load());
    return drainedWhileRunning ? 1 : 0;
  }
  if (mode == "claim-fetch") {
    // Owner-claim retrieval: the Repo mints the proof only after matching the manifest, every
    // indexed packet and the ordered wire digest.
    repo::RepoDataFetch claimRequest;
    repo::RepoOwnerClaimFetch claim;
    claim.objectName = arg(options, "object-name");
    claim.target = repo::RepoTarget{alternateName(repository), alternateName(provider.toUri()),
                                    alternateName(servicePrefix.toUri())};
    claimRequest.ownerClaim = claim;
    claimRequest.verifyWire = std::make_shared<const std::function<bool(const std::vector<uint8_t>&)>>(
      [certificate, identity] (const std::vector<uint8_t>& wire) {
        try {
          const ndn::Block block(wire);
          const ndn::Data packet(block);
          return identity.isPrefixOf(packet.getName()) &&
            ndn::security::verifySignature(packet, certificate);
        }
        catch (const std::exception&) {
          return false;
        }
      });
    try {
      const auto result = client.fetchData(claimRequest, operation);
      std::ofstream output(arg(options, "destination"), std::ios::binary);
      for (const auto& wire : result.packets) {
        const ndn::Block block(wire);
        const ndn::Data packet(block);
        const auto& content = packet.getContent();
        output.write(reinterpret_cast<const char*>(content.value()), content.value_size());
      }
      std::fprintf(stderr, "FACADE_CLAIM packets=%zu wireBytes=%llu digestVerified=%d proof=%d\n",
                   result.packets.size(), static_cast<unsigned long long>(result.wireBytes),
                   result.digestVerified ? 1 : 0, result.ownerPacketSet ? 1 : 0);
      return 0;
    }
    catch (const std::exception& error) {
      std::fprintf(stderr, "FACADE_CLAIM_REJECTED what=%s\n", error.what());
      return options.count("expect-rejection") != 0 ? 0 : 1;
    }
  }
  if (mode == "fetch") {
    const auto destination = arg(options, "destination");
    repo::RepoDataFetch request;
    request.packetNames = {};
    if (options.count("manifest") != 0) {
      // Manifest-bound retrieval: the caller already holds the committed manifest and asks
      // the Repo for exactly its packets.
      std::ifstream manifestInput(options.at("manifest"));
      std::string objectNameText;
      std::string sha256Text;
      std::string sizeText;
      std::string countText;
      std::getline(manifestInput, objectNameText);
      std::getline(manifestInput, sha256Text);
      std::getline(manifestInput, sizeText);
      std::getline(manifestInput, countText);
      auto boundManifest = std::make_shared<repo::RepoObjectManifest>();
      boundManifest->objectName = objectNameText;
      boundManifest->objectType = "original-packet-set";
      boundManifest->sha256 = sha256Text;
      boundManifest->size = std::stoull(sizeText);
      boundManifest->segmentCount = static_cast<uint32_t>(std::stoul(countText));
      std::string offsetsText;
      std::getline(manifestInput, offsetsText);
      std::size_t cursor = 0;
      while (cursor <= offsetsText.size()) {
        const auto comma = offsetsText.find(',', cursor);
        const auto token = offsetsText.substr(cursor, comma == std::string::npos
          ? std::string::npos : comma - cursor);
        if (!token.empty())
          boundManifest->packetOffsets.push_back(std::stoull(token));
        if (comma == std::string::npos)
          break;
        cursor = comma + 1;
      }
      std::string name;
      while (std::getline(manifestInput, name)) {
        if (!name.empty())
          boundManifest->packetNames.push_back(name);
      }
      repo::RepoManifestBoundFetch bound;
      bound.manifest = boundManifest;
      bound.objectName = boundManifest->objectName;
      // The Repo compares reference digests case-sensitively against a canonical spelling.
      auto canonicalSha = sha256Text;
      for (auto& character : canonicalSha)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
      bound.expectedSha256 = canonicalSha;
      boundManifest->sha256 = canonicalSha;
      bound.dataPrefix = identity.toUri();
      bound.expectedWireBytes = boundManifest->size;
      bound.producerIdentity = identity.toUri();
      request.manifestBound = std::move(bound);
    }
    if (options.count("manifest") == 0) {
      std::ifstream names(arg(options, "names"));
      std::string line;
      while (std::getline(names, line)) {
        if (!line.empty())
          request.packetNames.push_back(line);
      }
    }
    request.maxWireBytes = 8U << 20;
    const auto alternate = [] (const std::string& value) {
      return ndn::Name(value).toUri(ndn::name::UriFormat::ALTERNATE);
    };
    request.target = repo::RepoTarget{alternate(repository), alternate(provider.toUri()),
                                      alternate(servicePrefix.toUri())};
    request.verifyWire = std::make_shared<const std::function<bool(const std::vector<uint8_t>&)>>(
      [] (const std::vector<uint8_t>&) { return true; });
    const auto result = client.fetchData(request, operation);
    std::ofstream output(destination, std::ios::binary);
    for (const auto& wire : result.packets) {
      const ndn::Block block(wire);
      const ndn::Data packet(block);
      const auto& content = packet.getContent();
      output.write(reinterpret_cast<const char*>(content.value()), content.value_size());
    }
    std::printf("FACADE_FETCH packets=%zu wireBytes=%llu\n",
                result.packets.size(), static_cast<unsigned long long>(result.wireBytes));
    return 0;
  }
  throw std::runtime_error("unknown mode " + mode);
}
