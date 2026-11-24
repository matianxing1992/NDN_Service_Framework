/* Publish an original NAC application object to a network Repo.
 * The historical-read fixture also supports serving the originals from an isolated local volume.
 */
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/Stream.hpp"
#include "ndn-service-framework/historical-object-publisher.hpp"

#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <boost/asio/signal_set.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/string-helper.hpp>

namespace {

namespace repo = ndnsf_distributed_repo;

struct OriginalPackets
{
  repo::PacketSource source;
  std::vector<std::vector<uint8_t>> wires;
};

OriginalPackets
collectOriginals(const ndn::Data& proof, const ndn::Data& ck,
                 const std::vector<std::shared_ptr<const ndn::Data>>& segments)
{
  OriginalPackets originals;
  auto& source = originals.source;
  ndn::util::Sha256 aggregate;
  const auto collect = [&] (const ndn::Data& data) {
    const auto& wire = data.wireEncode();
    source.packetNames.push_back(data.getName().toUri());
    source.packetOffsets.push_back(source.expectedWireBytes);
    originals.wires.emplace_back(wire.begin(), wire.end());
    aggregate << ndn::span<const uint8_t>(wire.data(), wire.size());
    source.expectedWireBytes += wire.size();
  };
  collect(proof);
  collect(ck);
  for (const auto& segment : segments)
    collect(*segment);
  source.packetOffsets.push_back(source.expectedWireBytes);
  source.expectedPacketCount = originals.wires.size();
  source.expectedDigest = "sha256:" + ndn::toHex(*aggregate.computeDigest(), false);
  return originals;
}

void
recordOriginals(const OriginalPackets& originals)
{
  for (std::size_t i = 0; i < originals.wires.size(); ++i) {
    ndn::util::Sha256 digest;
    digest << ndn::span<const uint8_t>(originals.wires[i]);
    std::cout << "FIXTURE_WIRE " << originals.source.packetNames[i] << " "
              << ndn::toHex(*digest.computeDigest(), false) << "\n";
  }
}

std::shared_ptr<repo::RepoNode>
openReplica(const std::string& root, const ndn::Name& route)
{
  if (root.empty() || route.empty())
    throw std::invalid_argument("historical fixture replica needs a store and route");
  repo::StorageCapability capability;
  capability.repoNode = route.toUri();
  capability.repoMode = "persistent";
  capability.freeBytes = 4U << 20;
  return std::make_shared<repo::RepoNode>(ndn::Name(route).append("service"), capability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(root + "/objects",
      capability.freeBytes, 1024, "historical-read",
      repo::FilesystemRecoveryPolicy::KEEP_UNCONFIRMED),
    capability.repoNode, route.toUri(), root + "/catalog.sqlite3");
}

void
storeOriginals(repo::RepoNode& replica, const std::string& id, const ndn::Name& producer,
               const OriginalPackets& originals)
{
  repo::RepoUserStorageInsertRequest request;
  request.operationId = id;
  request.generation = 1;
  auto& reference = request.reference;
  const auto& source = originals.source;
  reference.objectName = source.packetNames.front();
  reference.dataPrefix = producer.toUri();
  auto& index = reference.packetSetMetadata.emplace();
  index.objectName = reference.objectName;
  index.objectType = "original-packet-set";
  index.packetNames = source.packetNames;
  reference.expectedSha256 = index.sha256 = source.expectedDigest.substr(7);
  reference.expectedSize = index.size = source.expectedWireBytes;
  const auto expires = ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count() + 60000;
  const auto principal = producer.toUri();
  const repo::RepoUserStorageGrantRequest grant{
    1, id, principal, reference.objectName, static_cast<uint64_t>(expires)};
  const auto body = grant.toJson();
  const auto response = replica.handleUserStorageGrant({body.begin(), body.end()}, principal);
  const auto granted = repo::RepoUserStorageGrantReply::fromJson({response.begin(), response.end()});
  request.storageBinding = {1, principal, grant.grantId, granted.grant.ownerEpoch};
  const auto status = replica.insertWirePackets(request, originals.wires, principal);
  if (status.state != "DONE")
    throw std::runtime_error("historical fixture INSERT: " + status.message);
}

class NetworkRepoWriter
{
public:
  NetworkRepoWriter(ndn::KeyChain& keys, const ndn::security::Certificate& producer,
                    const ndn::security::Certificate& controller, const ndn::Name& group,
                    const std::string& trustSchema, repo::RepoTarget target)
    : m_certificatePublisher(m_face, keys, producer.getName())
    , m_user(std::make_shared<ndn_service_framework::ServiceUser>(
        m_face, group, producer, controller, trustSchema))
    , m_target(std::move(target))
    , m_client(repo::RepoClient::makePersistent(m_user, [&] {
        repo::PersistentRepoClientConfig config;
        config.servicePrefix = ndn::Name(m_target.servicePrefix);
        config.providerScope = {ndn::Name(m_target.providerIdentity)};
        config.dataOnly = true;
        return config;
      }()))
  {
    m_user->init();
    m_user->fetchPermissionsFromController(controller.getIdentity());
    const ndn::Name prefix(m_target.servicePrefix);
    const std::vector<ndn::Name> providers{ndn::Name(m_target.providerIdentity)};
    const auto grant = repo::makeRepoServiceName(prefix, "USER_STORAGE_GRANT");
    const auto insert = repo::makeRepoServiceName(prefix, "INSERT");
    const auto manifest = repo::makeRepoServiceName(prefix, "MANIFEST");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_user->isRequestBootstrapReady(grant, providers) ||
           !m_user->isRequestBootstrapReady(insert, providers) ||
           !m_user->isRequestBootstrapReady(manifest, providers)) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("historical fixture Repo writer authorization timed out");
      m_face.processEvents(ndn::time::milliseconds(20));
    }
    m_face.getIoContext().restart();
    m_worker = std::thread([this] {
      try {
        auto& io = m_face.getIoContext();
        auto work = boost::asio::make_work_guard(io);
        io.run();
      }
      catch (const std::exception& error) {
        std::cerr << "Historical fixture Repo writer failed: " << error.what() << '\n';
      }
    });
  }

  ~NetworkRepoWriter()
  {
    m_face.getIoContext().stop();
    m_worker.join();
  }

  void
  store(const OriginalPackets& originals, const std::string& id,
        const ndn::security::Certificate& producer, uint64_t expiresAt,
        const repo::RepoOperationOptions& options, bool fixture)
  {
    const auto& source = originals.source;
    repo::RepoWriteAuthorization authorization;
    authorization.target = m_target;
    authorization.writerIdentity = producer.getIdentity().toUri();
    authorization.objectName = source.packetNames.front();
    authorization.grantId = id;
    authorization.expiresAtMs = expiresAt;
    options.requireCurrent();
    const auto grant = m_client.authorizeWrite(authorization, options);
    repo::RepoDataPublication publication;
    publication.target = m_target;
    publication.producerIdentity = authorization.writerIdentity;
    publication.ownerIdentity = grant.ownerIdentity;
    publication.grantId = grant.grantId;
    publication.ownerEpoch = grant.ownerEpoch;
    publication.idempotencyKey = id;
    publication.generation = 1;
    publication.packets = source;
    publication.packets->next = [&originals, index = size_t{0}] (std::vector<uint8_t>& wire) mutable {
      if (index == originals.wires.size())
        return false;
      wire = originals.wires[index++];
      return true;
    };
    publication.verifyWire =
      std::make_shared<const std::function<bool(const std::vector<uint8_t>&)>>(
        [producer] (const std::vector<uint8_t>& wire) {
          const ndn::Data data{ndn::Block(ndn::span<const uint8_t>(wire))};
          return producer.getIdentity().isPrefixOf(data.getName()) &&
                 ndn::security::verifySignature(data, producer);
        });
    const auto result = m_client.publishData(publication, options);
    if (result.state != repo::RepoPublishState::COMMITTED || !result.packetSet ||
        !std::equal(result.packetSet->packetNames.begin(), result.packetSet->packetNames.end(),
                    source.packetNames.begin(), source.packetNames.end(),
                    [](const auto& left, const auto& right) {
                      return ndn::Name(left) == ndn::Name(right);
                    }) ||
        result.packetSet->wireDigest != source.expectedDigest ||
        result.packetSet->wireBytes != source.expectedWireBytes)
      throw std::runtime_error("historical fixture Repo commit identity mismatch");
    std::cout << (fixture ? "FIXTURE_REPO_COMMITTED" : "HISTORICAL_REPO_COMMITTED")
              << " object=" << result.packetSet->objectName
              << " wireDigest=" << result.packetSet->wireDigest << '\n';
  }

private:
  ndn::Face m_face;
  ndn_service_framework::CertificatePublisher m_certificatePublisher;
  std::shared_ptr<ndn_service_framework::ServiceUser> m_user;
  repo::RepoTarget m_target;
  repo::RepoClient m_client;
  std::thread m_worker;
};

int
serveReplica(const std::string& root, const ndn::Name& route)
{
  ndn::Face face;
  auto replica = openReplica(root, route);
  replica->registerOriginalDataServing(face);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!replica->isOriginalDataServingReady() && std::chrono::steady_clock::now() < deadline)
    face.processEvents(ndn::time::milliseconds(50));
  if (!replica->isOriginalDataServingReady())
    throw std::runtime_error("historical fixture Repo route did not register");
  boost::asio::signal_set signals(face.getIoContext(), SIGTERM, SIGINT);
  signals.async_wait([&face] (const boost::system::error_code& error, int) {
    if (!error)
      face.shutdown();
  });
  std::cout << "HISTORICAL_REPO_READY route=" << route << std::endl;
  face.processEvents();
  return 0;
}

std::string
readPayload(const std::string& path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  if (size < 0 || size > 32 * 1024 * 1024)
    throw std::runtime_error("cannot read payload within the historical object size limit");
  std::string bytes(static_cast<size_t>(size), '\0');
  input.seekg(0);
  if (!input.read(bytes.data(), size) || input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("payload read failed or file size changed");
  return bytes;
}

} // namespace

int
main(int argc, char** argv)
{
  const bool fixture = std::filesystem::path(argv[0]).filename() == "ndnsf-historical-fixture";
  const bool serve = fixture && argc == 3 && std::string(argv[1]) == "--serve-repo";
  const bool serveCommitted = !fixture && argc == 3 && std::string(argv[1]) == "--serve-committed";
  const bool help = argc == 2 && std::string(argv[1]) == "--help";
  if (help || (fixture ? (argc != 2 && !serve) :
               (argc != 3 || (!serveCommitted && std::string(argv[1]) != "--config")))) {
    auto& out = help ? std::cout : std::cerr;
    if (fixture)
      out << "usage: ndnsf-historical-fixture [--serve-repo] <config.json>\n";
    else
      out << "usage: ndnsf-historical-publish (--config|--serve-committed) <config.json>\n"
             "  --serve-committed serves the committed originals until SIGTERM or 60 seconds.\n"
             "  pib, identity, controller, controller_certificate, service, object_prefix,\n"
             "  payload_file, repo_provider, repo_route, repo_service_prefix, group, trust_schema\n";
    return help ? 0 : 2;
  }
  try {
    boost::property_tree::ptree config;
    boost::property_tree::read_json(argv[serve || !fixture ? 2 : 1], config);
    auto field = [&config] (const std::string& key, bool required = true) {
      const auto at = config.find(key);
      if (at == config.not_found()) {
        if (required)
          throw std::runtime_error("missing " + key);
        return std::string{};
      }
      if (config.count(key) != 1 || !at->second.empty() ||
          (required && at->second.data().empty()))
        throw std::invalid_argument("invalid or repeated configuration field " + key);
      return at->second.data();
    };
    if (!fixture) {
      for (const char* key : {"attributes", "payload", "repo_store", "controller_pib"}) {
        if (config.count(key) != 0)
          throw std::invalid_argument(std::string("unsupported publisher configuration field ") + key);
      }
    }
    const auto repoStore = field("repo_store", false);
    const ndn::Name repoRoute(field("repo_route", !fixture));
    const auto repoProvider = field("repo_provider", !fixture);
    if (serve)
      return serveReplica(repoStore, repoRoute);
    if (!repoProvider.empty() && (!repoStore.empty() || repoRoute.empty()))
      throw std::invalid_argument("network Repo writer needs a route and no local store");
    const auto replica = repoStore.empty() ? nullptr : openReplica(repoStore, repoRoute);
    const std::string pib = field("pib");
    const ndn::Name identity(field("identity"));
    const ndn::Name controller(field("controller"));
    const ndn::Name service(field("service"));
    // One or more attributes, comma separated: each becomes its own object under the same generation, so a
    // run can check both a permission that survives the rotation and one that does not.
    std::vector<std::string> attributes;
    if (fixture) {
      std::stringstream list(field("attributes"));
      std::string item;
      while (std::getline(list, item, ','))
        if (!item.empty())
          attributes.push_back(item);
    }
    else
      attributes.push_back(ndn_service_framework::makeAbeServiceAttribute(service, "PERMISSION"));
    const auto objectPrefix = fixture ?
      ndn_service_framework::makeProducerDataPrefix(identity, "OBJECT") : ndn::Name(field("object_prefix"));
    const std::string payload = fixture ? field("payload") : readPayload(field("payload_file"));

    ndn::KeyChain keyChain("pib-sqlite3:" + pib, "tpm-file:", false);
    const auto certificate = keyChain.getPib().getIdentity(identity).getDefaultKey()
      .getDefaultCertificate();
    const auto controllerCert = [&] {
      const auto certificatePath = field("controller_certificate", !repoProvider.empty());
      if (!certificatePath.empty()) {
        const auto pinned = ndn::io::load<ndn::security::Certificate>(certificatePath);
        if (!pinned || pinned->getIdentity() != controller)
          throw std::invalid_argument("historical fixture Controller certificate mismatch");
        return *pinned;
      }
      ndn::KeyChain controllerChain("pib-sqlite3:" + field("controller_pib"), "tpm-file:", false);
      return controllerChain.getPib().getIdentity(controller).getDefaultKey().getDefaultCertificate();
    }();
    ndn::Face face;
    std::unique_ptr<NetworkRepoWriter> networkRepo;
    if (!repoProvider.empty()) {
      networkRepo = std::make_unique<NetworkRepoWriter>(
        keyChain, certificate, controllerCert, ndn::Name(field("group")), field("trust_schema"),
        repo::RepoTarget{repoRoute.toUri(), repoProvider, field("repo_service_prefix")});
    }

    const auto onFailure = [] (const ndn::Name& name, const std::string& reason) {
      std::cout << "FIXTURE_REGISTER_FAILED name=" << name.toUri() << " reason=" << reason
                << std::endl << std::flush;
    };
    ndn::util::Sha256 hash;
    hash << payload;
    ndn::Name paramsDataName;
    using namespace ndn_service_framework;
    std::vector<std::unique_ptr<StoredSignedPacketProducer>> origins;
    if (attributes.empty())
      throw std::invalid_argument("historical fixture needs a service read permission");
    for (const auto& attribute : attributes) {
      if (attribute != makeAbeServiceAttribute(service, "PERMISSION"))
        throw std::invalid_argument("historical fixture attribute does not match the service");
      publishHistoricalObject(face, keyChain, certificate, controllerCert, service,
        objectPrefix,
        ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
        [&] (const HistoricalObjectPublication& publication) {
          const auto& proof = publication.mapping;
          const auto& ck = publication.ck;
          const auto& packets = publication.segments;
          const auto payloadRoot = packets.front()->getName().getPrefix(-1);
          const auto suffix = payloadRoot[-1].toUri();
          paramsDataName = publication.status.getAbePublicParametersName();
          const auto& materialVersion = publication.status.getAbeMaterialVersion();
          if (replica || networkRepo) {
            const auto originals = collectOriginals(proof, ck, packets);
            publication.requireCurrent();
            if (networkRepo) {
              repo::RepoOperationOptions operation;
              operation.deadline = publication.deadline;
              operation.requireCurrent = [&] { publication.requireCurrent(); };
              networkRepo->store(originals, suffix, certificate,
                                 publication.status.getValidUntilMs(), operation, fixture);
            }
            else {
              storeOriginals(*replica, suffix, identity, originals);
            }
            if (fixture)
              recordOriginals(originals);
          }
          else {
            face.setInterestFilter(proof.getName(),
              [&face, proof] (const ndn::InterestFilter&, const ndn::Interest&) { face.put(proof); },
              onFailure);
            face.setInterestFilter(ck.getName(),
              [&face, ck] (const ndn::InterestFilter&, const ndn::Interest&) { face.put(ck); },
              onFailure);
            for (const auto& packet : packets) {
              face.setInterestFilter(packet->getName(),
                [&face, packet] (const ndn::InterestFilter&, const ndn::Interest& interest) {
                  if (interest.getName() == packet->getName())
                    face.put(*packet);
                }, onFailure);
            }
          }
          if (serveCommitted) {
            // Register only this object's mapping, exact CK and payload root:
            // another publication under the same Producer may proceed independently.
            const auto copyWire = [] (const ndn::Data& packet) {
              const auto& wire = packet.wireEncode();
              return ndn::Buffer(wire.begin(), wire.end());
            };
            const auto retain = [&] (const ndn::Name& prefix,
                                     const std::vector<ndn::Buffer>& wires) {
              origins.push_back(std::make_unique<StoredSignedPacketProducer>(face, prefix, wires));
            };
            retain(proof.getName(), {copyWire(proof)});
            retain(ck.getName(), {copyWire(ck)});
            std::vector<ndn::Buffer> segments;
            segments.reserve(packets.size());
            for (const auto& packet : packets)
              segments.push_back(copyWire(*packet));
            retain(payloadRoot, segments);
          }
          std::cout << (fixture ? "FIXTURE_OBJECT" : "HISTORICAL_OBJECT")
                    << " ck=" << ck.getName().toUri()
                    << " payload=" << payloadRoot.toUri()
                    << " publication=" << proof.getName().toUri()
                    << " service=" << service.toUri()
                    << " attribute=" << attribute
                    << " producer=" << identity.toUri()
                    << " generation=" << materialVersion.controllerGenerationTimestamp
                    << " epoch=" << materialVersion.controllerEpoch
                    << " parameters=" << publication.status.getAbePublicParametersName().toUri()
                    << " parametersDigest=" << publication.status.getAbePublicParametersDigest()
                    << " plaintextDigest=sha256:" << hash.toString()
                    << " bytes=" << payload.size() << " segments=" << packets.size() << std::endl;
          std::cout << (fixture ? "FIXTURE_RECEIPT" : "HISTORICAL_RECEIPT")
                    << " object=" << payloadRoot.toUri()
                    << " name=" << publication.receiptFullName.toUri() << std::endl;
        }, fixture ? 64 : 7000);
    }
    if (serveCommitted)
      networkRepo.reset();
    for (auto& origin : origins)
      origin->start();
    if (fixture)
      std::cout << "FIXTURE_READY objects=" << attributes.size()
                << " paramsName=" << paramsDataName.toUri() << std::endl << std::flush;
    if (serveCommitted) {
      boost::asio::signal_set signals(face.getIoContext(), SIGTERM, SIGINT);
      signals.async_wait([&face] (const boost::system::error_code& error, int) {
        if (!error)
          face.shutdown();
      });
      std::cout << "HISTORICAL_ORIGIN_STARTED identity=" << identity << std::endl;
      face.processEvents(ndn::time::seconds(60));
    }
    else if (!replica && !networkRepo)
      face.processEvents();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << (fixture ? "FIXTURE_FAILED" : "HISTORICAL_PUBLISH_FAILED")
              << " error=" << error.what() << std::endl;
    return 1;
  }
}
