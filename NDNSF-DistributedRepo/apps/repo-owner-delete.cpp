#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/controller-certificate.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/scope.hpp>

#include <boost/asio/signal_set.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace repo = ndnsf_distributed_repo;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MAX_REQUEST_BYTES = 16384;
constexpr std::size_t MAX_REPLY_BYTES = 65536;

void
usage()
{
  std::cout << "Usage: ndnsf-repo-owner-delete --group-prefix NAME --owner-identity NAME "
    "--controller-identity NAME --controller-certificate FILE --trust-schema FILE "
    "--repo-node NAME --provider-identity NAME --service-prefix NAME --request-file FILE "
    "[--timeout-ms 1..60000]\n"
    "Uses existing HOME/PIB/TPM credentials. Request is RepoUserStorageDeleteRequest JSON.\n"
    "Only a validated DELETED reply exits0; PENDING exits2. Signals drain any issued request "
    "within its remaining budget and exit130. Reuse the cleanup ID after uncertain outcomes.\n";
}

std::map<std::string, std::string>
arguments(int argc, char** argv)
{
  const std::set<std::string> required{"--group-prefix", "--owner-identity",
    "--controller-identity", "--controller-certificate", "--trust-schema", "--repo-node",
    "--provider-identity", "--service-prefix", "--request-file"};
  std::map<std::string, std::string> result;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i];
    if ((!required.count(key) && key != "--timeout-ms") || i + 1 >= argc ||
        !result.emplace(key, argv[i + 1]).second)
      throw std::invalid_argument("unknown, duplicate or incomplete option");
  }
  for (const auto& key : required) {
    if (!result.count(key) || result.at(key).empty())
      throw std::invalid_argument("missing required option: " + key);
  }
  return result;
}

std::string
readRequest(const std::string& path)
{
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (descriptor < 0) throw std::runtime_error("Owner delete request file is unavailable");
  const auto closeFile = ndn::make_scope_exit([descriptor] { ::close(descriptor); });
  struct stat info{};
  if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<std::uintmax_t>(info.st_size) > MAX_REQUEST_BYTES)
    throw std::runtime_error("Owner delete request must be a bounded regular file");
  std::string bytes(MAX_REQUEST_BYTES + 1, '\0');
  std::size_t size = 0;
  while (size < bytes.size()) {
    const auto count = ::read(descriptor, bytes.data() + size, bytes.size() - size);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("Owner delete request read failed");
    if (count == 0) break;
    size += static_cast<std::size_t>(count);
  }
  if (size == 0 || size > MAX_REQUEST_BYTES)
    throw std::runtime_error("Owner delete request byte bound exceeded");
  bytes.resize(size);
  return bytes;
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    usage();
    return 0;
  }
  try {
    const auto options = arguments(argc, argv);
    int timeoutMs = 30000;
    if (options.count("--timeout-ms")) {
      const auto& text = options.at("--timeout-ms");
      if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("timeout must be an integer1..60000");
      const auto value = std::stoul(text);
      if (value == 0 || value > 60000)
        throw std::invalid_argument("timeout must be an integer1..60000");
      timeoutMs = static_cast<int>(value);
    }
    const auto request = repo::RepoUserStorageDeleteRequest::fromJson(
      readRequest(options.at("--request-file")));
    const auto alternate = [](const std::string& value) {
      const ndn::Name name(value);
      if (name.empty()) throw std::invalid_argument("target Names must be non-root");
      return name.toUri(ndn::name::UriFormat::ALTERNATE);
    };
    const repo::RepoArtifactTarget target{alternate(options.at("--repo-node")),
      alternate(options.at("--provider-identity")), alternate(options.at("--service-prefix"))};
    const auto service = repo::makeRepoServiceName(ndn::Name(target.servicePrefix),
                                                 "USER_STORAGE_DELETE");
    const std::vector<ndn::Name> providers{ndn::Name(target.providerIdentity)};
    const ndn::Name owner(options.at("--owner-identity"));
    const ndn::Name controller(options.at("--controller-identity"));
    const ndn::Name group(options.at("--group-prefix"));
    if (owner.empty() || controller.empty() || group.empty())
      throw std::invalid_argument("actor and group Names must be non-root");
    ndn::Face face;
    ndn::security::KeyChain keyChain;
    const auto identity = keyChain.getPib().getIdentity(owner);
    keyChain.setDefaultIdentity(identity);
    const auto certificate = identity.getDefaultKey().getDefaultCertificate();
    const auto controllerCertificate = ndn_service_framework::loadControllerCertificate(
      keyChain, controller, options.at("--controller-certificate").c_str());
    ndn_service_framework::CertificatePublisher certificates(face, keyChain, certificate.getName());
    ndn_service_framework::ServiceUser user(face, group,
      certificate, controllerCertificate, options.at("--trust-schema"));
    user.init();
    user.fetchPermissionsFromController(controller);

    bool interrupted = false;
    bool completed = false;
    int result = 1;
    std::string failure;
    std::string replyWire;
    const auto alive = std::make_shared<bool>(true);
    boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
    const auto invalidateCallbacks = ndn::make_scope_exit([alive] { *alive = false; });
    signals.async_wait([&, alive](const boost::system::error_code& error, int) {
      if (!error && *alive) interrupted = true;
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!interrupted && !user.isRequestBootstrapReady(service, providers) &&
           Clock::now() < deadline)
      face.processEvents(ndn::time::milliseconds(10));
    if (!interrupted && user.isRequestBootstrapReady(service, providers) &&
        Clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now()).count();
      if (remaining > 0) {
        repo::RepoClient::requestUserStorageDelete(user, target, request,
          static_cast<int>(remaining),
          [&, alive](const ndn::Name&) {
            if (!*alive || completed) return;
            failure = "Owner delete request timed out; outcome unconfirmed";
            completed = true;
          },
          [&, alive](const ndn_service_framework::ResponseMessage& response) {
            if (!*alive || completed) return;
            try {
              if (!response.getStatus()) throw std::runtime_error(response.getErrorInfo());
              const auto payload = response.getPayload();
              if (payload.size() > MAX_REPLY_BYTES)
                throw std::runtime_error("Owner delete reply byte bound exceeded");
              const std::string wire(payload.begin(), payload.end());
              const auto reply = repo::RepoUserStorageDeleteReply::fromJson(wire);
              reply.validateExpected(target, owner.toUri(ndn::name::UriFormat::ALTERNATE), request);
              replyWire = reply.toJson();
              result = reply.state == "DELETED" ? 0 : 2;
            }
            catch (const std::exception& error) {
              failure = error.what();
            }
            completed = true;
          });
        // Ordinary RequestService has no collaboration-cancel contract. Drain its bounded
        // response/timeout even after a signal instead of claiming remote cancellation.
        while (!completed && Clock::now() < deadline + std::chrono::seconds(1))
          face.processEvents(ndn::time::milliseconds(10));
      }
    }
    if (!completed && !interrupted)
      failure = "Owner delete bootstrap or request drain deadline; outcome unconfirmed";
    *alive = false;
    user.stopProducerPacketValidation();
    face.shutdown();
    face.getIoContext().stop();
    if (!replyWire.empty()) std::cout << replyWire << '\n';
    if (!failure.empty()) std::cerr << "REPO_OWNER_DELETE_FAILED: " << failure << '\n';
    return interrupted ? 130 : result;
  }
  catch (const std::exception& error) {
    std::cerr << "REPO_OWNER_DELETE_FAILED: " << error.what() << '\n';
    return 1;
  }
}
