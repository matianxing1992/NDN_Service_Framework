#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderResourceProbe.hpp"

#include <cctype>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_PROC_TEXT_BYTES = 1024 * 1024;

std::int64_t
nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
safeErrorCode(const std::string& value, const std::string& fallback)
{
  if (value.empty() || value.size() > 64) {
    return fallback;
  }
  for (const unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')) {
      return fallback;
    }
  }
  return value;
}

ResourceTextRead
readProcText(const std::string& path, std::chrono::milliseconds timeout)
{
#ifndef __linux__
  static_cast<void>(path);
  static_cast<void>(timeout);
  return {ResourceProbeStatus::Unsupported, "", "linux-proc-unsupported"};
#else
  if (path != "/proc/meminfo" && path != "/proc/self/status") {
    return {ResourceProbeStatus::Unsupported, "", "unsupported-source"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return {ResourceProbeStatus::TimedOut, "", "probe-timeout"};
  }
  const auto started = std::chrono::steady_clock::now();
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {ResourceProbeStatus::ReadError, "", "proc-open-failed"};
  }
  std::ostringstream content;
  char buffer[4096];
  std::size_t total = 0;
  while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
    const auto count = static_cast<std::size_t>(input.gcount());
    total += count;
    if (total > MAX_PROC_TEXT_BYTES) {
      return {ResourceProbeStatus::MalformedInput, "", "proc-input-too-large"};
    }
    content.write(buffer, static_cast<std::streamsize>(count));
  }
  if (!input.eof()) {
    return {ResourceProbeStatus::ReadError, "", "proc-read-failed"};
  }
  if (std::chrono::steady_clock::now() - started > timeout) {
    return {ResourceProbeStatus::TimedOut, "", "probe-timeout"};
  }
  return {ResourceProbeStatus::Measured, content.str(), ""};
#endif
}

bool
parseKbField(const std::string& text, const std::string& field,
             std::uint64_t& bytes)
{
  std::istringstream lines(text);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos || line.substr(0, colon) != field) {
      continue;
    }
    if (found) {
      return false;
    }
    found = true;
    std::istringstream valueStream(line.substr(colon + 1));
    std::string numberText;
    std::string unit;
    std::string trailing;
    if (!(valueStream >> numberText >> unit) || unit != "kB" ||
        (valueStream >> trailing)) {
      return false;
    }
    std::uint64_t kib = 0;
    const auto result = std::from_chars(
      numberText.data(), numberText.data() + numberText.size(), kib);
    if (result.ec != std::errc() ||
        result.ptr != numberText.data() + numberText.size() ||
        kib > std::numeric_limits<std::uint64_t>::max() / 1024) {
      return false;
    }
    bytes = kib * 1024;
  }
  return found;
}

} // namespace

class LinuxProviderResourceProbe::Impl
{
public:
  Impl(ProviderResourceProbeConfig config, TextReader reader)
    : config(std::move(config))
    , reader(reader ? std::move(reader) : TextReader(readProcText))
  {
    latestSnapshot.source = "linux-proc";
    latestSnapshot.providerName = this->config.providerName;
    latestSnapshot.providerBootId = this->config.providerBootId;
    latestSnapshot.errorCode = "not-sampled";
  }

  ~Impl()
  {
    stop();
  }

  ProviderResourceSnapshot
  sample(std::chrono::milliseconds timeout)
  {
    ProviderResourceSnapshot snapshot;
    snapshot.source = "linux-proc";
    snapshot.providerName = config.providerName;
    snapshot.providerBootId = config.providerBootId;
    {
      std::lock_guard<std::mutex> lock(mutex);
      snapshot.sequence = ++sequence;
    }
    snapshot.measuredAtMs = nowMs();
    if (snapshot.providerName.empty() || snapshot.providerBootId.empty()) {
      snapshot.status = ResourceProbeStatus::IdentityMismatch;
      snapshot.errorCode = "probe-identity-missing";
      store(snapshot);
      return snapshot;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
      snapshot.status = ResourceProbeStatus::TimedOut;
      snapshot.errorCode = "probe-timeout";
      store(snapshot);
      return snapshot;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&] {
      const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      return value > std::chrono::milliseconds::zero() ? value :
             std::chrono::milliseconds::zero();
    };
    const auto memory = reader("/proc/meminfo", remaining());
    if (memory.status != ResourceProbeStatus::Measured) {
      snapshot.status = memory.status;
      snapshot.errorCode = safeErrorCode(memory.errorCode, "meminfo-read-failed");
      store(snapshot);
      return snapshot;
    }
    const auto process = reader("/proc/self/status", remaining());
    if (process.status != ResourceProbeStatus::Measured) {
      snapshot.status = process.status;
      snapshot.errorCode = safeErrorCode(process.errorCode, "status-read-failed");
      store(snapshot);
      return snapshot;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      snapshot.status = ResourceProbeStatus::TimedOut;
      snapshot.errorCode = "probe-timeout";
      store(snapshot);
      return snapshot;
    }
    if (!parseKbField(memory.content, "MemTotal", snapshot.hostTotalMemoryBytes) ||
        !parseKbField(memory.content, "MemAvailable",
                      snapshot.hostAvailableMemoryBytes) ||
        !parseKbField(process.content, "VmRSS", snapshot.processRssBytes) ||
        snapshot.hostAvailableMemoryBytes > snapshot.hostTotalMemoryBytes) {
      snapshot.status = ResourceProbeStatus::MalformedInput;
      snapshot.hostTotalMemoryBytes = 0;
      snapshot.hostAvailableMemoryBytes = 0;
      snapshot.processRssBytes = 0;
      snapshot.errorCode = "proc-memory-malformed";
      store(snapshot);
      return snapshot;
    }
    snapshot.status = ResourceProbeStatus::Measured;
    store(snapshot);
    return snapshot;
  }

  void
  start()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (worker.joinable()) {
      return;
    }
    stopping = false;
    worker = std::thread([this] {
      while (true) {
        sample(config.readTimeout);
        std::unique_lock<std::mutex> lock(mutex);
        if (condition.wait_for(lock, config.sampleInterval,
                               [this] { return stopping; })) {
          break;
        }
      }
    });
  }

  void
  stop() noexcept
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    condition.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }

  ProviderResourceSnapshot
  latest() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto snapshot = latestSnapshot;
    if (snapshot.isMeasured() &&
        !snapshot.isFresh(nowMs(), config.maximumAge.count())) {
      snapshot.status = ResourceProbeStatus::Stale;
      snapshot.errorCode = "sample-stale";
    }
    return snapshot;
  }

private:
  void
  store(const ProviderResourceSnapshot& snapshot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    latestSnapshot = snapshot;
  }

public:
  ProviderResourceProbeConfig config;
  TextReader reader;
  mutable std::mutex mutex;
  std::condition_variable condition;
  ProviderResourceSnapshot latestSnapshot;
  std::uint64_t sequence = 0;
  bool stopping = false;
  std::thread worker;
};

LinuxProviderResourceProbe::LinuxProviderResourceProbe(
  ProviderResourceProbeConfig config, TextReader reader)
  : m_impl(std::make_unique<Impl>(std::move(config), std::move(reader)))
{
}

LinuxProviderResourceProbe::~LinuxProviderResourceProbe() = default;

void
LinuxProviderResourceProbe::start()
{
  m_impl->start();
}

void
LinuxProviderResourceProbe::stop() noexcept
{
  m_impl->stop();
}

ProviderResourceSnapshot
LinuxProviderResourceProbe::sample(std::chrono::milliseconds timeout)
{
  return m_impl->sample(timeout);
}

ProviderResourceSnapshot
LinuxProviderResourceProbe::latest() const
{
  return m_impl->latest();
}

} // namespace ndnsf::di
