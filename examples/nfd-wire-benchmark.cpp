#include <ndn-cxx/face.hpp>
#include <ndn-cxx/lp/nack.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/transport/unix-transport.hpp>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <fstream>
#include <map>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
constexpr std::size_t MAX_REPLAY_RECORDS = 4 * 4096;
constexpr std::uintmax_t MAX_REPLAY_WIRE_BYTES = 4 * 64 * 1024 * 1024;

bool
sampleSource(const ndn::Name& name)
{
  static const ndn::name::Component sourceComponent("DATA");
  return name.size() >= 2 && name[-2] == sourceComponent &&
         name[-1].isSegment() && name[-1].toSegment() % 128 == 0;
}

void
traceSource(const ndn::Name& name, const char* event)
{
  using std::chrono::duration_cast;
  using std::chrono::microseconds;
  const auto steady = duration_cast<microseconds>(Clock::now().time_since_epoch()).count();
  const auto wall = duration_cast<microseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  std::fprintf(stderr, "NDNSF_CONTROL_TIMING role=repo-manifest-source event=%s "
               "steady_us=%lld timestamp_us=%lld requestId=%s\n", event,
               static_cast<long long>(steady), static_cast<long long>(wall),
               name.toUri().c_str());
}

void
pin(int cpu)
{
  if (cpu < 0) return;
  if (cpu >= CPU_SETSIZE) throw std::invalid_argument("invalid test CPU");
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    throw std::runtime_error("cannot pin current test thread");
}

double
cpuTime(clockid_t which)
{
  timespec t = {};
  if (clock_gettime(which, &t) != 0)
    throw std::runtime_error("cannot read thread CPU clock");
  return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
}

double threadCpu() { return cpuTime(CLOCK_THREAD_CPUTIME_ID); }

struct Run
{
  ndn::Face& face;
  const std::vector<ndn::Data>& packets;
  const std::vector<ndn::Name>& names;
  const std::vector<std::size_t>& payloadSizes;
  const ndn::Name& forwardingHint;
  const Clock::time_point until;
  bool traceSources = false;
  std::size_t next = 0;
  std::size_t complete = 0;
  std::size_t timeouts = 0;
  std::size_t nacks = 0;
  std::size_t bad = 0;
  std::size_t dataWire = 0;
  std::size_t payloadBytes = 0;
  std::size_t interestWire = 0;
  std::size_t latencySamples = 0;
  std::array<std::size_t, 8193> latencyUs{};

  std::size_t
  percentile(std::size_t percent) const
  {
    const auto rank = (latencySamples * percent + 99) / 100;
    std::size_t cumulative = 0;
    for (std::size_t i = 0; i < latencyUs.size(); ++i) {
      cumulative += latencyUs[i];
      if (cumulative >= rank) return i;
    }
    return latencyUs.size() - 1;
  }

  void
  send()
  {
    const auto sentAt = Clock::now();
    if (sentAt >= until) return;
    const bool sampleLatency = next % 128 == 0;
    const auto i = next++ % packets.size();
    ndn::Interest request(names[i]);
    request.setCanBePrefix(false);
    request.setInterestLifetime(ndn::time::milliseconds(1500));
    if (!forwardingHint.empty())
      request.setForwardingHint({forwardingHint});
    interestWire += request.wireEncode().size();
    const bool trace = traceSources && sampleSource(packets[i].getName());
    if (trace) traceSource(packets[i].getName(), "request-express");
    face.expressInterest(request,
      [this, i, sentAt, sampleLatency, trace] (const ndn::Interest&, const ndn::Data& packet) {
        if (trace) traceSource(packet.getName(), "request-face-data");
        if (sampleLatency) {
          const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - sentAt).count();
          ++latencyUs[std::min<std::size_t>(latencyUs.size() - 1, latency)];
          ++latencySamples;
        }
        if (packet.wireEncode() != packets[i].wireEncode()) ++bad;
        ++complete;
        dataWire += packet.wireEncode().size();
        payloadBytes += payloadSizes[i];
        send();
      },
      [this] (const ndn::Interest&, const ndn::lp::Nack&) { ++nacks; send(); },
      [this] (const ndn::Interest&) { ++timeouts; send(); });
  }

  void
  start(std::size_t window)
  {
    for (std::size_t i = 0; i < window; ++i) send();
    face.processEvents();
  }
};

int
main(int argc, char** argv)
{
  if (argc != 14 && argc != 15) {
    std::cerr << "Usage: ndnsf-nfd-wire-benchmark CONSUMER_SOCKET PAYLOAD WINDOW SAMPLE "
                 "MAX_PACKETS PRODUCER_CPU CONSUMER_CPU MODE SECONDS PRODUCER_SOCKET "
                 "ROUTER_PID PREFIX WIRE_FILE [FORWARDING_HINT]\n";
    return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 2;
  }
  try {
    const std::string consumerSocket = argv[1];
    const auto payload = static_cast<std::size_t>(std::stoul(argv[2]));
    const auto window = static_cast<std::size_t>(std::stoul(argv[3]));
    const auto sample = static_cast<std::size_t>(std::stoul(argv[4]));
    const auto records = static_cast<std::size_t>(std::stoul(argv[5]));
    const int producerCpu = std::stoi(argv[6]);
    const int consumerCpu = std::stoi(argv[7]);
    const std::string mode = argv[8];
    const auto duration = std::chrono::seconds(std::stoul(argv[9]));
    const std::string producerSocket = argv[10];
    const ndn::Name forwardingHint = argc == 15 ? ndn::Name(argv[14]) : ndn::Name{};
    if ((payload != 7000 && payload != 8400) || window == 0 || window > 4096 || records < window ||
        records > MAX_REPLAY_RECORDS || (mode != "ordinary" && mode != "digest") ||
        (argc == 15 && forwardingHint.empty()) ||
        duration < std::chrono::seconds(1) || duration > std::chrono::seconds(120)) {
      return 2;
    }
    const auto nfdPid = std::stoi(argv[11]);
    if (nfdPid <= 1) return 2;
    clockid_t nfdClock;
    if (clock_getcpuclockid(nfdPid, &nfdClock) != 0)
      throw std::runtime_error("cannot read clock for the private NFD");
    ndn::KeyChain keys;
    const auto identity = keys.getPib().getIdentity(ndn::Name("/spec271-nfd-fixture"));
    const ndn::Name prefix(argv[12]);
    const std::filesystem::path fixture(argv[13]);
    if (prefix.empty() || !std::filesystem::is_regular_file(fixture) ||
        std::filesystem::file_size(fixture) > MAX_REPLAY_WIRE_BYTES) {
      throw std::runtime_error("invalid replay fixture");
    }
    std::ifstream input(fixture, std::ios::binary);
    if (!input) throw std::runtime_error("missing committed original-wire fixture");
    std::vector<ndn::Data> packets;
    std::vector<ndn::Name> names;
    std::vector<std::size_t> payloadSizes;
    std::map<ndn::Name, std::size_t> byName;
    while (input.peek() != std::istream::traits_type::eof()) {
      ndn::Data data(ndn::Block::fromStream(input));
      if (packets.size() == records || data.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE ||
          !prefix.isPrefixOf(data.getName())) {
        throw std::runtime_error("invalid committed original-wire fixture");
      }
      const auto& dataName = data.getName();
      const bool source = dataName[-1].isSegment();
      const bool manifestRoot = !source && dataName[-1] == ndn::name::Component("MANIFEST");
      const auto& requestName = mode == "digest" && !manifestRoot ?
        data.getFullName() : dataName;
      if (!byName.emplace(requestName, packets.size()).second) {
        throw std::runtime_error("duplicate original-wire fixture packet");
      }
      names.push_back(requestName);
      if (source && data.getContent().value_size() != payload) {
        throw std::runtime_error("original fixture source payload geometry mismatch");
      }
      payloadSizes.push_back(source ? data.getContent().value_size() : 0);
      packets.push_back(std::move(data));
    }
    if (packets.size() < window ||
        std::none_of(payloadSizes.begin(), payloadSizes.end(), [](auto size) { return size != 0; })) {
      throw std::runtime_error("short committed original-wire fixture or missing source segments");
    }
    boost::asio::io_context producerIo;
    boost::asio::io_context consumerIo;
    ndn::Face producer(std::make_shared<ndn::UnixTransport>(producerSocket), producerIo, keys);
    ndn::Face consumer(std::make_shared<ndn::UnixTransport>(consumerSocket), consumerIo, keys);
    std::promise<void> registered;
    auto ready = registered.get_future();
    std::atomic<std::size_t> producerCount{0};
    std::atomic<bool> traceProducer{false};
    const char* traceSetting = std::getenv("NDNSF_NFD_WIRE_SOURCE_TIMING");
    const bool traceSources = traceSetting != nullptr && std::string_view(traceSetting) == "1";
    producer.setInterestFilter(ndn::InterestFilter(prefix),
      [&] (const ndn::InterestFilter&, const ndn::Interest& request) {
        const auto& hint = request.getForwardingHint();
        if (forwardingHint.empty() ? !hint.empty() : hint.size() != 1 || hint[0] != forwardingHint)
          throw std::runtime_error("unexpected replay forwarding hint");
        bool trace = false;
        if (traceSources && traceProducer.load(std::memory_order_relaxed)) {
          const auto& name = request.getName();
          const auto sourceName = name[-1].isImplicitSha256Digest() ? name.getPrefix(-1) : name;
          trace = sampleSource(sourceName);
          if (trace) traceSource(sourceName, "producer-receive-interest");
        }
        const auto found = byName.find(request.getName());
        if (found == byName.end()) throw std::runtime_error("unexpected producer name");
        const auto& data = packets[found->second];
        if (!request.matchesData(data)) {
          throw std::runtime_error("unexpected producer name");
        }
        if (trace) traceSource(data.getName(), "producer-submit-data");
        producer.put(data);
        producerCount.fetch_add(1);
      });
    producer.registerPrefix(forwardingHint.empty() ? prefix : forwardingHint,
      [&] (const ndn::Name&) { registered.set_value(); },
      [&] (const ndn::Name&, const std::string& error) {
        registered.set_exception(std::make_exception_ptr(std::runtime_error(error)));
      }, ndn::security::signingByIdentity(identity));
    std::string threadError;
    std::thread server([&] {
      try { pin(producerCpu); producer.processEvents(); }
      catch (const std::exception& e) { threadError = e.what(); }
    });
    const auto stop = [&] { producerIo.stop(); if (server.joinable()) server.join(); };
    try {
      if (ready.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
        throw std::runtime_error("signed private NFD route did not complete");
      ready.get();
      pin(consumerCpu);
      clockid_t producerClock;
      if (pthread_getcpuclockid(server.native_handle(), &producerClock) != 0)
        throw std::runtime_error("cannot read private Producer CPU clock");
      Run initial{consumer, packets, names, payloadSizes, forwardingHint,
                  Clock::now() + std::chrono::seconds(5)};
      initial.start(window);
      if (initial.complete == 0 || initial.complete != initial.next ||
          initial.timeouts || initial.nacks || initial.bad)
        throw std::runtime_error("warmup did not qualify");
      const auto producerCpuStart = cpuTime(producerClock);
      const auto nfdCpuStart = cpuTime(nfdClock);
      const auto startCpu = threadCpu();
      const auto start = Clock::now();
      if (traceSources) traceProducer.store(true, std::memory_order_relaxed);
      Run measured{consumer, packets, names, payloadSizes, forwardingHint, start + duration,
                   traceSources};
      measured.start(window);
      const auto wall = std::chrono::duration<double>(Clock::now() - start).count();
      const auto cpu = threadCpu() - startCpu;
      const auto nfdCpu = cpuTime(nfdClock) - nfdCpuStart;
      const auto producerCpuSeconds = cpuTime(producerClock) - producerCpuStart;
      stop();
      if (!threadError.empty()) throw std::runtime_error(threadError);
      std::cout << std::fixed << std::setprecision(8)
                << mode << ',' << payload << ',' << window << ',' << sample << ',' << measured.next << ','
                << measured.complete << ',' << measured.timeouts << ',' << measured.nacks << ','
                << measured.bad << ',' << measured.payloadBytes << ',' << measured.interestWire << ','
                << measured.dataWire << ',' << wall << ',' << cpu << ','
                << nfdCpu << ',' << producerCpuSeconds << ',' << producerCount.load() << ','
                << measured.latencySamples << ',' << measured.percentile(50) << ','
                << measured.percentile(95) << '\n';
      return measured.complete > 0 && measured.complete == measured.next &&
        !measured.timeouts && !measured.nacks && !measured.bad &&
        producerCount.load() == initial.next + measured.next ? 0 : 1;
    }
    catch (...) {
      stop();
      throw;
    }
  }
  catch (const std::exception& e) {
    std::cerr << "probe: " << e.what() << '\n';
    return 1;
  }
}
