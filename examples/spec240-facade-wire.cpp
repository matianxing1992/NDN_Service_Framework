// Spec 240 acceptance driver: publish and fetch original Data through RepoClient's
// persistent facade against a live Repo. Evidence tool; not a production path.
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/repo-client-types.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/io.hpp>
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
    face.processEvents(ndn::time::milliseconds(20));
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
  auto user = std::make_shared<ndn_service_framework::ServiceUser>(
    face, group, certificate, certificate, authorityCertificate, trustSchema, keyChain);
  user->init();
  user->fetchPermissionsFromController(controller);

  repo::PersistentRepoClientConfig config;
  config.servicePrefix = servicePrefix;
  config.providerScope = {provider};
  auto client = repo::RepoClient::makePersistent(user, config);

  repo::RepoOperationOptions operation;
  operation.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  const auto service = repo::makeRepoServiceName(servicePrefix, "STORE");
  if (!waitReady(face, *user, service, provider, 60000))
    throw std::runtime_error("bootstrap did not become ready");

  if (mode == "publish") {
    const auto source = arg(options, "source");
    const auto bytes = readBytes(source);
    repo::RepoDataPublication publication;
    publication.target = repo::RepoTarget{repository, provider.toUri(), servicePrefix.toUri()};
    publication.producerIdentity = identity.toUri();
    publication.objectName = ndn::Name(identity).append("object").appendSegment(0).toUri();

    repo::PacketSource packets;
    packets.expectedWireBytes = 0;
    // One Data packet per segment, built by the caller and verified by the caller's own
    // verifier: the Repo must not invent Producer trust.
    std::vector<std::vector<uint8_t>> wires;
    uint64_t offsets = 0;
    uint64_t segment = 0;
    const uint64_t chunk = 8000;
    while (segment * chunk < bytes.size() || segment == 0) {
      const uint64_t begin = segment * chunk;
      const uint64_t end = std::min<uint64_t>(bytes.size(), begin + chunk);
      const ndn::Name name = ndn::Name(publication.objectName).appendSegment(segment);
      ndn::Data packet(name);
      packet.setContent(ndn::span<const uint8_t>(bytes.data() + begin, end - begin));
      packet.setFreshnessPeriod(ndn::time::seconds(3600));
      keyChain.sign(packet, ndn::security::signingByCertificate(certificate));
      const auto wire = packet.wireEncode();
      wires.emplace_back(wire.begin(), wire.end());
      packets.packetNames.push_back(name.toUri());
      packets.packetOffsets.push_back(offsets);
      offsets += wires.back().size();
      ++segment;
    }
    packets.packetOffsets.push_back(offsets);
    packets.expectedWireBytes = offsets;
    std::size_t index = 0;
    packets.next = [&wires, index] (std::vector<uint8_t>& out) mutable {
      if (index >= wires.size())
        return false;
      out = wires[index++];
      return true;
    };
    packets.expectedPacketCount = wires.size();
    ndn::util::Sha256 digest;
    for (const auto& wire : wires)
      digest.update(wire);
    packets.expectedDigest = "sha256:" + digest.toString();
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
    publication.idempotencyKey = "spec240-facade-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
    const auto result = client.publishData(publication, operation);
    std::printf("FACADE_PUBLISH state=%s receipt=%s bytes=%llu\n",
                result.state == repo::RepoPublishState::COMMITTED ? "COMMITTED" : "ACCEPTED",
                result.receipt ? result.receipt->receiptId.c_str() : "",
                static_cast<unsigned long long>(result.packetSet ? result.packetSet->wireBytes : 0));
    return 0;
  }
  if (mode == "fetch") {
    const auto destination = arg(options, "destination");
    repo::RepoDataFetch request;
    request.packetNames = {};
    std::ifstream names(arg(options, "names"));
    std::string line;
    while (std::getline(names, line)) {
      if (!line.empty())
        request.packetNames.push_back(line);
    }
    request.maxWireBytes = 8U << 20;
    request.target = repo::RepoTarget{repository, provider.toUri(), servicePrefix.toUri()};
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
