// Focused production-ServiceController tests; no running NFD or model required.
// After the shared Waf build (do not compile concurrently with it):
// /usr/bin/g++ -B/usr/bin -std=c++17 -I. -Indn-service-framework \
//   -I/usr/local/include/nac-abe \
//   tests/standalone/service-controller-readiness.cpp \
//   -Lbuild-system-j2 -Wl,-rpath,"$PWD/build-system-j2" \
//   -lndn-service-framework $(pkg-config --cflags --libs libndn-cxx libnac-abe ndnsd) \
//   -ldl \
//   -o /tmp/service-controller-readiness
// IMPORTANT: linked NDN-SVS initializes DEFAULT_KEYCHAIN before main(). Protect
// the FIRST process load with the external launcher; never run the binary bare.
// python3 tests/standalone/run-service-controller-readiness.py \
//   /tmp/service-controller-readiness [case-name]
// The Unix-socket forwarder below is a protocol fixture, NOT NFD qualification.
// Explicit live opt-in (Linux, nfd on PATH; retains private logs/config):
// use the same launcher command, replacing [case-name] with --real-nfd.

#include "ndn-service-framework/ServiceController.hpp"

#include <ndn-cxx/lp/packet.hpp>
#include <ndn-cxx/lp/fields.hpp>
#include <ndn-cxx/mgmt/nfd/control-parameters.hpp>
#include <ndn-cxx/mgmt/nfd/control-response.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/pib/pib.hpp>
#include <ndn-cxx/security/tpm/tpm.hpp>
#include <ndn-cxx/transport/unix-transport.hpp>
#include <ndn-cxx/util/scope.hpp>

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Socket = boost::asio::local::stream_protocol::socket;
using Controller = ndn_service_framework::ServiceController;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// Do not reset cached defaults or touch an existing PIB to repair isolation.
// The external launcher sets private paths BEFORE the executable/DSOs load.
// Adopt and verify those paths; never replace them after defaults are cached.
struct Environment
{
  Environment()
  {
#ifdef __linux__
    const auto inheritedRoot = std::getenv("NDNSF_CONTROLLER_READINESS_ROOT");
    const auto launcherPid = std::getenv("NDNSF_CONTROLLER_READINESS_LAUNCHER_PID");
    require(inheritedRoot && launcherPid && std::string(launcherPid) == std::to_string(::getppid()),
            "use run-service-controller-readiness.py to isolate DSO initialization before main");
    root = inheritedRoot;
    const std::string prefix = "/tmp/ndnsf-controller-readiness-";
    require(root.size() == prefix.size() + 8 && root.compare(0, prefix.size(), prefix) == 0 &&
            std::all_of(root.begin() + prefix.size(), root.end(),
                        [](unsigned char c) { return std::isalnum(c) || c == '_'; }),
            "invalid private launcher directory");
    struct stat info{};
    require(::lstat(root.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
            info.st_uid == ::geteuid() && (info.st_mode & 0777) == 0700,
            "launcher directory must be owned, non-symlink and mode-0700");
    const auto pib = std::getenv("NDN_CLIENT_PIB");
    const auto tpm = std::getenv("NDN_CLIENT_TPM");
    require(pib && tpm && std::string(pib) == "pib-sqlite3:" + root + "/pib" &&
            std::string(tpm) == "tpm-file:" + root + "/tpm",
            "PIB/TPM differ from launcher paths; refusing in-process locator repair");
    ndn::KeyChain actual;
    require(actual.getPib().getPibLocator() == "pib-sqlite3:" + root + "/pib" &&
            actual.getTpm().getTpmLocator() == "tpm-file:" + root + "/tpm",
            "default KeyChain did not bind to the private launcher locators");
    const auto policyPath = root + "/empty.policies";
    std::ofstream policy(policyPath);
    policy << "provider-policies\n{\n}\nuser-policies\n{\n}\n";
    policy.close();
    require(bool(policy), "could not write valid empty policy sections");
    // Exercise the production Parser before constructing any fixture/AA.
    // It requires both child nodes even when their policy lists are empty.
    ndn_service_framework::PolicyParser parser;
    const auto policies = parser.parsePolicyFile(policyPath);
    require(policies.first.empty() && policies.second.empty(),
            "empty policy fixture unexpectedly grants a service");
    std::cout << "TEST_POLICY_SCHEMA providers=0 users=0\n";
    std::cout << "TEST_KEYCHAIN_ISOLATION launcher=true pib=" << actual.getPib().getPibLocator()
              << " tpm=" << actual.getTpm().getTpmLocator() << '\n';
#else
    throw std::runtime_error("test launcher isolation requires Linux");
#endif
  }

  ~Environment()
  {
    for (const auto& item : previous) {
      if (item.second) {
        ::setenv(item.first.c_str(), item.second->c_str(), 1);
      }
      else {
        ::unsetenv(item.first.c_str());
      }
    }
    // The launcher retains its own directory for failure evidence. Do not
    // delete a PIB here while DSO-global KeyChains are still alive.
  }

  void set(const std::string& name, const std::string& value)
  {
    if (previous.count(name) == 0) {
      const auto old = std::getenv(name.c_str());
      previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
    }
    require(::setenv(name.c_str(), value.c_str(), 1) == 0, "setenv failed");
  }

  std::string root;
  std::map<std::string, std::optional<std::string>> previous;
};

// A deliberately small NFD protocol fixture. The production Faces use real
// separate Unix streams; the fixture supplies RIB responses and forwards only
// PUBPARAMS Interests/Data. It cannot accidentally satisfy same-Face Interests.
class Forwarder
{
public:
  enum class Mode { Normal, Silent, CachedData, BadSignature, WrongHop, RejectRegistration };

  struct Peer : std::enable_shared_from_this<Peer>
  {
    Peer(Forwarder& owner, Socket stream)
      : owner(owner), stream(std::move(stream)) {}

    void read()
    {
      stream.async_read_some(boost::asio::buffer(input),
        [self = shared_from_this()](const auto& ec, size_t size) {
          if (ec) {
            ++self->owner.closedConnections;
            return;
          }
          self->buffer.insert(self->buffer.end(), self->input.begin(), self->input.begin() + size);
          while (!self->buffer.empty()) {
            const auto decoded = ndn::Block::fromBuffer(ndn::span<const uint8_t>(
              self->buffer.data(), self->buffer.size()));
            if (!std::get<0>(decoded)) {
              require(self->buffer.size() < 65536, "invalid/unbounded fixture frame");
              break;
            }
            const auto packet = std::get<1>(decoded);
            self->buffer.erase(self->buffer.begin(), self->buffer.begin() + packet.size());
            self->owner.receive(self, packet);
          }
          self->read();
        });
    }

    void send(ndn::Block packet)
    {
      output.push_back(std::move(packet));
      if (output.size() == 1) {
        write();
      }
    }

    void write()
    {
      const auto& packet = output.front();
      boost::asio::async_write(stream, boost::asio::buffer(packet.data(), packet.size()),
        [self = shared_from_this()](const auto& ec, size_t) {
          if (!ec) {
            self->output.pop_front();
            if (!self->output.empty()) {
              self->write();
            }
          }
        });
    }

    Forwarder& owner;
    Socket stream;
    std::array<uint8_t, 16384> input;
    std::vector<uint8_t> buffer;
    std::deque<ndn::Block> output;
  };

  Forwarder(boost::asio::io_context& io, const std::string& path,
            ndn::KeyChain& keys, ndn::security::Certificate cert,
            Mode mode, std::chrono::milliseconds delay)
    : io(io), acceptor(io, boost::asio::local::stream_protocol::endpoint(path)),
      keys(keys), cert(std::move(cert)), mode(mode), delay(delay)
  {
    accept();
  }

  void accept()
  {
    acceptor.async_accept([this](const auto& ec, Socket stream) {
      if (!ec) {
        auto peer = std::make_shared<Peer>(*this, std::move(stream));
        peers.push_back(peer);
        peer->read();
        accept();
      }
    });
  }

  void receive(const std::shared_ptr<Peer>& source, const ndn::Block& wire)
  {
    ndn::lp::Packet lp(wire);
    require(lp.has<ndn::lp::FragmentField>(), "fixture received empty LP packet");
    const auto fragment = lp.get<ndn::lp::FragmentField>();
    const auto decoded = ndn::Block::fromBuffer(ndn::span<const uint8_t>(
      &*fragment.first, static_cast<size_t>(fragment.second - fragment.first)));
    require(std::get<0>(decoded), "invalid network packet");
    const auto packet = std::get<1>(decoded);
    if (packet.type() == ndn::tlv::Interest) {
      const ndn::Interest interest(packet);
      const auto& name = interest.getName();
      if (ndn::Name("/localhost/nfd/rib").isPrefixOf(name) && name.size() > 4) {
        controllerPeer = source;
        ndn::nfd::ControlParameters params(name[4].blockFromValue());
        params.setFaceId(100).setOrigin(ndn::nfd::ROUTE_ORIGIN_APP).setCost(0);
        const bool isRoot = params.getName() == cert.getIdentity();
        auto respond = [this, source, name, params, isRoot] {
          ndn::nfd::ControlResponse response;
          const bool reject = mode == Mode::RejectRegistration && !isRoot;
          response.setCode(reject ? 403 : 200).setText(reject ? "test registration denied" : "OK");
          response.setBody(params.wireEncode());
          ndn::Data data(name);
          data.setContent(response.wireEncode());
          keys.sign(data, ndn::security::signingWithSha256());
          source->send(data.wireEncode());
          if (isRoot) {
            rootRegistered = true;
          }
        };
        if (isRoot && delay.count() > 0) {
          auto timer = std::make_shared<boost::asio::steady_timer>(io, delay);
          timer->async_wait([timer, respond](const auto& ec) { if (!ec) respond(); });
        }
        else {
          respond();
        }
        return;
      }
      require(ndn::Name(cert.getIdentity()).append("PUBPARAMS").isPrefixOf(name),
              "unexpected fixture Interest");
      ++probeInterests;
      require(source != controllerPeer, "probe was expressed on the authority connection");
      require(interest.getHopLimit() == 1, "probe must stop at the local NFD");
      require(interest.getMustBeFresh() && interest.getCanBePrefix(), "wrong probe selectors");
      if (mode == Mode::Silent) {
        return;
      }
      if (mode == Mode::CachedData) {
        ndn::Data data(ndn::Name(name).appendVersion());
        data.setContent("signed but never reached the authority");
        keys.sign(data, ndn::security::signingByCertificate(cert));
        source->send(data.wireEncode());
        return;
      }
      if (!rootRegistered) {
        ndn::lp::Packet nack(interest.wireEncode());
        nack.add<ndn::lp::NackField>(ndn::lp::NackHeader().setReason(ndn::lp::NackReason::NO_ROUTE));
        source->send(nack.wireEncode());
        return;
      }
      require(controllerPeer != nullptr, "missing authority connection");
      probePeer = source;
      auto forwarded = interest;
      if (mode != Mode::WrongHop) {
        forwarded.setHopLimit(0); // NFD decrements at ingress, permits local egress only
      }
      ++forwardedInterests;
      controllerPeer->send(forwarded.wireEncode());
    }
    else if (packet.type() == ndn::tlv::Data) {
      require(source == controllerPeer && probePeer != nullptr, "unexpected fixture Data");
      ndn::Data data(packet);
      if (mode == Mode::BadSignature) {
        keys.sign(data, ndn::security::signingWithSha256());
      }
      ++forwardedData;
      probePeer->send(data.wireEncode());
    }
  }

  boost::asio::io_context& io;
  boost::asio::local::stream_protocol::acceptor acceptor;
  ndn::KeyChain& keys;
  ndn::security::Certificate cert;
  Mode mode;
  std::chrono::milliseconds delay;
  std::vector<std::shared_ptr<Peer>> peers;
  std::shared_ptr<Peer> controllerPeer, probePeer;
  bool rootRegistered = false;
  size_t probeInterests = 0, forwardedInterests = 0, forwardedData = 0, closedConnections = 0;
};

struct Fixture
{
  Fixture(Environment& env, Forwarder::Mode mode = Forwarder::Mode::Normal,
          std::chrono::milliseconds delay = 0ms, bool withForwarder = true)
    : cert(keys.createIdentity(ndn::Name("/test/controller-readiness")).getDefaultKey().getDefaultCertificate()),
      socket(env.root + "/nfd-" + std::to_string(++serial) + ".sock")
  {
    env.set("NDN_CLIENT_TRANSPORT", "unix://" + socket);
    if (withForwarder) {
      forwarder = std::make_unique<Forwarder>(io, socket, keys, cert, mode, delay);
    }
    face = std::make_unique<ndn::Face>(nullptr, io, keys);
    validator = std::make_unique<ndn::ValidatorConfig>(*face);
    controller = std::make_unique<Controller>(*face, cert, *validator, env.root + "/empty.policies");
  }

  void drain()
  {
    io.restart();
    io.run_for(75ms); // execute removal callbacks after the temporary probe is gone
  }

  static unsigned serial;
  boost::asio::io_context io;
  ndn::KeyChain keys;
  ndn::security::Certificate cert;
  std::string socket;
  std::unique_ptr<Forwarder> forwarder;
  std::unique_ptr<ndn::Face> face;
  std::unique_ptr<ndn::ValidatorConfig> validator;
  std::unique_ptr<Controller> controller;
};
unsigned Fixture::serial = 0;

std::string failure(Controller& controller)
{
  try {
    controller.start();
  }
  catch (const std::exception& e) {
    return e.what();
  }
  throw std::runtime_error("start() unexpectedly succeeded");
}

void contains(const std::string& actual, const std::string& expected)
{
  require(actual.find(expected) != std::string::npos,
          "expected '" + expected + "', got '" + actual + "'");
}

#ifdef __linux__
// Applied only in the opt-in test process, before fork/exec: NFD inherits it.
// Unix transports and the NFD network-interface monitor (Netlink) are allowed;
// all other socket domains, including IPv4/IPv6/packet sockets, are denied.
void restrictSocketDomains()
{
  const sock_filter filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 4),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 2, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                     const_cast<sock_filter*>(filter)};
  require(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
          ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0,
          "cannot enforce Unix/Netlink-only sockets; real-NFD test refused");
}

class NfdChild
{
public:
  NfdChild(const std::string& config, const std::string& log)
  {
    const int fd = ::open(log.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    require(fd >= 0, "cannot create private NFD log");
    const pid_t parent = ::getpid();
    pid = ::fork();
    if (pid == 0) {
      // Also clean up if the test process is interrupted/killed before its RAII
      // cleanup runs. No process-group signals and no searches for other NFDs.
      if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent ||
          ::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) {
        ::_exit(126);
      }
      ::close(fd);
      ::unsetenv("NDN_LOG"); // use this test's explicit Forwarder DEBUG config
      ::execlp("nfd", "nfd", "--config", config.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::close(fd);
    require(pid > 0, "cannot fork isolated NFD");
  }

  ~NfdChild() { stop(); }

  bool running()
  {
    if (pid <= 0) return false;
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return true;
    pid = -1; // reaped, or not our child: never signal this PID again
    return false;
  }

  void stop() noexcept
  {
    if (!running()) return;
    ::kill(pid, SIGTERM);
    const auto deadline = Clock::now() + 2s;
    while (running() && Clock::now() < deadline) {
      std::this_thread::sleep_for(20ms);
    }
    if (running()) {
      forcedKill = true;
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      pid = -1;
    }
  }

  pid_t pid = -1;
  int status = -1;
  bool forcedKill = false;
};

void realNfd(Environment& env)
{
  const auto configPath = env.root + "/nfd.conf";
  const auto logPath = env.root + "/nfd.log";
  const auto socketPath = env.root + "/nfd.sock";
  std::cout << "REAL_NFD_ARTIFACT_DIR=" << env.root << std::endl;
  std::ofstream config(configPath);
  config << "general\n{\n}\n"
            "log\n{\n  default_level WARN\n  Forwarder DEBUG\n}\n"
            "tables\n{\n  cs_max_packets 0\n}\n"
            "face_system\n{\n  unix\n  {\n    path " << socketPath << "\n  }\n}\n"
            "authorizations\n{\n  authorize\n  {\n    certfile any\n    privileges\n    {\n"
            "      faces\n      fib\n      cs\n      strategy-choice\n    }\n  }\n}\n"
            "rib\n{\n  localhost_security\n  {\n    trust-anchor\n    {\n      type any\n    }\n  }\n}\n";
  config.close();
  require(bool(config), "cannot write isolated NFD config");
  // Omitting TCP/UDP/Ethernet/WebSocket sections disables their factories.
  // Seccomp independently enforces the absence of IPv4/IPv6/packet sockets.
  restrictSocketDomains();
  NfdChild child(configPath, logPath);
  const auto launchDeadline = Clock::now() + 5s;
  while (!std::filesystem::is_socket(socketPath)) {
    require(child.running(), "isolated NFD exited before creating its socket; inspect nfd.log");
    require(Clock::now() < launchDeadline, "isolated NFD socket startup timed out");
    std::this_thread::sleep_for(20ms);
  }

  ndn::KeyChain keys;
  const auto cert = keys.createIdentity(ndn::Name("/test/controller-readiness-live"))
                     .getDefaultKey().getDefaultCertificate();
  ndn::Face face(nullptr, keys);
  ndn::ValidatorConfig validator(face);
  Controller controller(face, cert, validator, env.root + "/empty.policies");
  const auto prefix = ndn::Name(cert.getIdentity()).append("PUBPARAMS").append("readiness");
  std::set<ndn::Name> receivedNames;
  ndn::ScopedInterestFilterHandle observed = face.setInterestFilter(prefix,
    [&](const ndn::InterestFilter&, const ndn::Interest& interest) {
      require(interest.getName().size() == prefix.size() + 2, "missing 128-bit probe suffix");
      require(interest.getHopLimit() == 0, "real NFD did not deliver HopLimit=0");
      receivedNames.insert(interest.getName());
    });
  for (int iteration = 0; iteration < 2; ++iteration) {
    require(child.running(), "NFD exited before Controller start");
    controller.start(); // real linked implementation; never calls Forwarder fixture
    require(receivedNames.size() == static_cast<size_t>(iteration + 1),
            "start succeeded without a new network-delivered random probe");
  }
  require(child.running(), "NFD exited during Controller readiness");
  child.stop(); // flush logs before checking forwarder evidence and cleanup status
  require(!child.forcedKill && WIFEXITED(child.status) && WEXITSTATUS(child.status) == 0,
          "isolated NFD did not exit cleanly after SIGTERM");

  std::ifstream log(logPath);
  std::vector<std::string> lines;
  for (std::string line; std::getline(log, line);) lines.push_back(std::move(line));
  const std::regex inInterest("onIncomingInterest in=([0-9]+)");
  const std::regex outInterest("onOutgoingInterest out=([0-9]+)");
  const std::regex inData("onIncomingData in=([0-9]+)");
  const std::regex outData("onOutgoingData out=([0-9]+)");
  for (const auto& name : receivedNames) {
    std::set<std::string> requesters, authorities, dataSenders, dataReceivers;
    for (const auto& line : lines) {
      std::smatch match;
      if (line.find("interest=" + name.toUri()) != std::string::npos) {
        if (std::regex_search(line, match, inInterest) && line.find("hop-limit=1") != std::string::npos)
          requesters.insert(match[1].str());
        if (std::regex_search(line, match, outInterest)) authorities.insert(match[1].str());
      }
      if (line.find("data=" + name.toUri() + "/") != std::string::npos) {
        if (std::regex_search(line, match, inData)) dataSenders.insert(match[1].str());
        if (std::regex_search(line, match, outData)) dataReceivers.insert(match[1].str());
      }
    }
    bool roundTrip = false;
    for (const auto& requester : requesters) {
      for (const auto& authority : authorities) {
        if (requester != authority && dataSenders.count(authority) && dataReceivers.count(requester)) {
          roundTrip = true;
          std::cout << "REAL_NFD_ROUND_TRIP probeFace=" << requester << " authorityFace=" << authority
                    << " hopLimit=1->0 name=" << name << '\n';
        }
      }
    }
    require(roundTrip, "NFD log does not prove an independent-Face Interest/Data round trip");
  }
  std::cout << "PASS real-nfd isolated=true randomProbes=2 nfdExit=0\n";
}
#endif
} // namespace

int main(int argc, char** argv)
{
  std::unique_ptr<Environment> environment;
  try {
    environment = std::make_unique<Environment>();
  }
  catch (const std::exception& e) {
    std::cerr << "FAIL test isolation: " << e.what() << '\n';
    return 1;
  }
  auto& env = *environment;
  if (argc == 2 && std::string(argv[1]) == "--real-nfd") {
    try {
#ifdef __linux__
      realNfd(env);
      return 0;
#else
      throw std::runtime_error("real-NFD isolation requires Linux");
#endif
    }
    catch (const std::exception& e) {
      std::cerr << "FAIL real-nfd: " << e.what() << '\n';
      return 1;
    }
  }
  using Mode = Forwarder::Mode;
  const std::vector<std::pair<std::string, std::function<void()>>> cases = {
    {"no-nfd", [&] {
      Fixture f(env, Mode::Normal, 0ms, false);
      const auto begin = Clock::now();
      const auto error = failure(*f.controller);
      contains(error, "connect");
      require(Clock::now() - begin < 2s, "missing Unix socket did not fail promptly");
    }},
    {"delayed-success-and-restart", [&] {
      Fixture f(env, Mode::Normal, 400ms);
      const auto begin = Clock::now();
      f.controller->start();
      const auto elapsed = Clock::now() - begin;
      require(elapsed >= 400ms && elapsed < 2s, "startup ignored readiness or retained fixed drains");
      require(f.forwarder->peers.size() == 2, "expected separate authority/probe connections");
      require(f.forwarder->forwardedInterests > 0 && f.forwarder->forwardedData > 0,
              "missing actual two-connection round trip");
      require(f.forwarder->probeInterests < 20, "Nacks caused an unbounded retry loop");
      f.drain();
      f.controller->start();
      require(f.forwarder->peers.size() == 3, "restart did not create a new independent probe");
      f.drain();
      require(f.forwarder->closedConnections >= 2, "temporary probe connections leaked");
    }},
    {"cancel-before-start-and-reset", [&] {
      Fixture f(env);
      f.controller->cancelStart();
      const auto begin = Clock::now();
      contains(failure(*f.controller), "cancelled");
      require(Clock::now() - begin < 100ms, "pre-cancelled start blocked");
      require(f.forwarder->peers.empty(), "pre-cancelled start performed network I/O");
      f.controller->resetStartCancellation();
      f.controller->start();
      f.drain();
    }},
    {"cancel-during-start", [&] {
      Fixture f(env, Mode::Silent);
      std::thread cancel([&] { std::this_thread::sleep_for(150ms); f.controller->cancelStart(); });
      auto join = ndn::make_scope_exit([&] { cancel.join(); });
      const auto begin = Clock::now();
      contains(failure(*f.controller), "cancelled");
      require(Clock::now() - begin < 750ms, "cancellation exceeded the bounded check interval");
      f.drain();
    }},
    {"native-stop-sequence", [&] {
      Fixture f(env, Mode::Silent);
      std::thread stop([&] {
        std::this_thread::sleep_for(150ms);
        f.controller->cancelStart();
        f.face->shutdown();
        f.io.stop();
      });
      auto join = ndn::make_scope_exit([&] { stop.join(); });
      const auto begin = Clock::now();
      contains(failure(*f.controller), "cancelled");
      require(Clock::now() - begin < 750ms, "stop was undone or spun until deadline");
    }},
    {"stopped-event-loop", [&] {
      Fixture f(env, Mode::Silent);
      std::thread stop([&] { std::this_thread::sleep_for(150ms); f.io.stop(); });
      auto join = ndn::make_scope_exit([&] { stop.join(); });
      const auto begin = Clock::now();
      contains(failure(*f.controller), "event loop stopped");
      require(Clock::now() - begin < 750ms, "stopped loop was restarted/spun");
    }},
    {"registration-rejected", [&] {
      Fixture f(env, Mode::RejectRegistration);
      contains(failure(*f.controller), "prefix registration failed");
      f.drain();
    }},
    {"cached-data-without-authority", [&] {
      Fixture f(env, Mode::CachedData);
      contains(failure(*f.controller), "did not reach the authority Face");
      require(f.forwarder->forwardedInterests == 0, "fixture forwarded the cached-data probe");
      f.drain();
    }},
    {"invalid-signature", [&] {
      Fixture f(env, Mode::BadSignature);
      contains(failure(*f.controller), "invalid authority Data");
      f.drain();
    }},
    {"wrong-hop", [&] {
      Fixture f(env, Mode::WrongHop);
      contains(failure(*f.controller), "did not reach the authority Face");
      f.drain();
    }},
    {"timeout-without-hot-loop", [&] {
      Fixture f(env, Mode::Silent);
      const auto cpuBegin = std::clock();
      const auto begin = Clock::now();
      contains(failure(*f.controller), "readiness timeout");
      const auto elapsed = Clock::now() - begin;
      require(elapsed >= 10s && elapsed < 11s, "deadline is not ten seconds from entry");
      const double cpuSeconds = double(std::clock() - cpuBegin) / CLOCKS_PER_SEC;
      require(cpuSeconds < 2.0, "idle startup busy-spun instead of blocking");
      require(f.forwarder->probeInterests <= 45, "unbounded timeout retransmissions");
      f.drain();
    }},
  };
  size_t count = 0;
  size_t failed = 0;
  for (const auto& item : cases) {
    if (argc > 1 && item.first != argv[1]) {
      continue;
    }
    ++count;
    try {
      item.second();
      std::cout << "PASS " << item.first << '\n';
    }
    catch (const std::exception& e) {
      ++failed;
      std::cerr << "FAIL " << item.first << ": " << e.what() << '\n';
    }
  }
  std::cout << count - failed << '/' << count << " focused Controller tests passed\n";
  return count > 0 && failed == 0 ? 0 : 1;
}
