#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

using Clock = std::chrono::steady_clock;

std::runtime_error cacheError(const char* code, const std::string& message)
{
  return std::runtime_error(std::string(code) + ": " + message);
}

} // namespace

struct OnnxRuntimeSessionCache::Lease::Release
{
  std::shared_ptr<OnnxRuntimeSessionCache::Shared> shared;
  std::string identity;

  ~Release() noexcept;
};

struct OnnxRuntimeSessionCache::Shared
{
  struct Entry
  {
    std::shared_ptr<void> value;
    std::size_t activeLeases = 0;
    bool retiring = false;
    Clock::time_point lastRelease = Clock::now();
  };

  struct Job
  {
    std::condition_variable condition;
    std::size_t waiters = 0;
    bool done = false;
    bool cancelled = false;
    std::exception_ptr error;
  };

  explicit Shared(Config value)
    : config(std::move(value))
  {
    if (config.maxEntries == 0 || config.idleTtl.count() <= 0) {
      throw std::invalid_argument(
        "ONNX Runtime session cache requires positive capacity and idle TTL");
    }
  }

  Config config;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::map<std::string, Entry> entries;
  std::map<std::string, std::shared_ptr<Job>> jobs;
  bool closed = false;
  std::uint64_t loads = 0;
  std::uint64_t hits = 0;
  std::uint64_t activeLeases = 0;

  void release(const std::string& identity) noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = entries.find(identity);
    if (found == entries.end()) {
      condition.notify_all();
      return;
    }
    if (found->second.activeLeases != 0) {
      --found->second.activeLeases;
    }
    if (activeLeases != 0) {
      --activeLeases;
    }
    found->second.lastRelease = Clock::now();
    if ((closed || found->second.retiring) && found->second.activeLeases == 0) {
      entries.erase(found);
    }
    condition.notify_all();
  }
};

OnnxRuntimeSessionCache::Lease::Release::~Release() noexcept
{
  if (shared) {
    shared->release(identity);
  }
}

OnnxRuntimeSessionCache::Lease::Lease(
  std::shared_ptr<void> value,
  std::shared_ptr<Release> release,
  bool cacheHit) noexcept
  : m_value(std::move(value))
  , m_release(std::move(release))
  , m_cacheHit(cacheHit)
{
}

OnnxRuntimeSessionCache::Lease::~Lease() noexcept = default;

OnnxRuntimeSessionCache::Lease::Lease(Lease&& other) noexcept
  : m_value(std::move(other.m_value))
  , m_release(std::move(other.m_release))
  , m_cacheHit(other.m_cacheHit)
{
}

OnnxRuntimeSessionCache::Lease&
OnnxRuntimeSessionCache::Lease::operator=(Lease&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  m_value = std::move(other.m_value);
  m_release = std::move(other.m_release);
  m_cacheHit = other.m_cacheHit;
  return *this;
}

OnnxRuntimeSessionCache::OnnxRuntimeSessionCache()
  : OnnxRuntimeSessionCache(Config{})
{
}

OnnxRuntimeSessionCache::OnnxRuntimeSessionCache(Config config)
  : m_shared(std::make_shared<Shared>(std::move(config)))
{
}

OnnxRuntimeSessionCache::~OnnxRuntimeSessionCache() noexcept
{
  close();
}

OnnxRuntimeSessionCache::Lease
OnnxRuntimeSessionCache::acquire(const std::string& identity,
                                 Loader loader,
                                 Clock::time_point deadline,
                                 std::function<bool()> cancelled)
{
  if (identity.empty() || !loader) {
    throw std::invalid_argument(
      "ONNX Runtime session cache requires identity and loader");
  }
  const auto shared = m_shared;
  std::shared_ptr<Shared::Job> job;
  bool creator = false;
  {
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (shared->closed) {
      throw cacheError("DI_ONNX_SESSION_CACHE_CLOSED", "cache is closed");
    }
    if (const auto found = shared->entries.find(identity);
        found != shared->entries.end()) {
      if (found->second.retiring) {
        if (found->second.activeLeases == 0) {
          shared->entries.erase(found);
        }
        else {
          throw cacheError("DI_ONNX_SESSION_CACHE_RETIRING",
                           "session entry is retiring");
        }
      }
      else {
        ++found->second.activeLeases;
        ++shared->activeLeases;
        ++shared->hits;
        auto release = std::make_shared<Lease::Release>();
        release->shared = shared;
        release->identity = identity;
        return Lease(found->second.value, std::move(release), true);
      }
    }
    if (const auto found = shared->jobs.find(identity);
        found != shared->jobs.end()) {
      job = found->second;
      ++job->waiters;
    }
    else {
      if (shared->entries.size() >= shared->config.maxEntries) {
        auto victim = shared->entries.end();
        for (auto it = shared->entries.begin(); it != shared->entries.end(); ++it) {
          if (it->second.activeLeases == 0 &&
              (victim == shared->entries.end() ||
               it->second.lastRelease < victim->second.lastRelease)) {
            victim = it;
          }
        }
        if (victim == shared->entries.end()) {
          throw cacheError("DI_ONNX_SESSION_CACHE_CAPACITY",
                           "all resident sessions are active");
        }
        shared->entries.erase(victim);
      }
      job = std::make_shared<Shared::Job>();
      job->waiters = 1;
      shared->jobs.emplace(identity, job);
      creator = true;
    }
  }

  if (!creator) {
    std::unique_lock<std::mutex> lock(shared->mutex);
    while (!job->done) {
      if ((cancelled && cancelled()) || Clock::now() >= deadline) {
        if (job->waiters != 0) {
          --job->waiters;
        }
        if (job->waiters == 0) {
          job->cancelled = true;
        }
        throw cacheError("DI_ONNX_SESSION_CACHE_CANCELLED",
                         "session load waiter expired or cancelled");
      }
      const auto timedOut = deadline == Clock::time_point::max()
        ? (job->condition.wait(lock), false)
        : job->condition.wait_until(lock, deadline) == std::cv_status::timeout;
      if (timedOut && !job->done) {
        if (job->waiters != 0) {
          --job->waiters;
        }
        if (job->waiters == 0) {
          job->cancelled = true;
        }
        throw cacheError("DI_ONNX_SESSION_CACHE_CANCELLED",
                         "session load waiter deadline expired");
      }
    }
    if (job->waiters != 0) {
      --job->waiters;
    }
    if (job->error) {
      std::rethrow_exception(job->error);
    }
    if (shared->closed) {
      throw cacheError("DI_ONNX_SESSION_CACHE_CLOSED", "cache closed while waiting");
    }
    const auto found = shared->entries.find(identity);
    if (found == shared->entries.end() || found->second.retiring) {
      throw cacheError("DI_ONNX_SESSION_CACHE_LOAD_CANCELLED",
                       "completed load was not published");
    }
    ++found->second.activeLeases;
    ++shared->activeLeases;
    ++shared->hits;
    auto release = std::make_shared<Lease::Release>();
    release->shared = shared;
    release->identity = identity;
    return Lease(found->second.value, std::move(release), true);
  }

  std::shared_ptr<void> value;
  try {
    value = loader();
    if (!value) {
      throw cacheError("DI_ONNX_SESSION_CACHE_LOAD_FAILED",
                       "loader returned an empty session");
    }
    std::unique_lock<std::mutex> lock(shared->mutex);
    const bool publish = !shared->closed && !job->cancelled && job->waiters != 0;
    job->done = true;
    if (publish) {
      Shared::Entry entry;
      entry.value = value;
      entry.activeLeases = 1;
      shared->entries.emplace(identity, std::move(entry));
      ++shared->activeLeases;
      ++shared->loads;
      if (job->waiters != 0) {
        --job->waiters;
      }
      shared->jobs.erase(identity);
      job->condition.notify_all();
      auto release = std::make_shared<Lease::Release>();
      release->shared = shared;
      release->identity = identity;
      lock.unlock();
      return Lease(std::move(value), std::move(release), false);
    }
    if (job->waiters != 0) {
      --job->waiters;
    }
    shared->jobs.erase(identity);
    job->condition.notify_all();
    lock.unlock();
    throw cacheError(shared->closed ? "DI_ONNX_SESSION_CACHE_CLOSED"
                                    : "DI_ONNX_SESSION_CACHE_CANCELLED",
                     "session load was not published");
  }
  catch (...) {
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (!job->done) {
      job->error = std::current_exception();
      job->done = true;
      if (job->waiters != 0) {
        --job->waiters;
      }
      shared->jobs.erase(identity);
      job->condition.notify_all();
    }
    throw;
  }
}

bool
OnnxRuntimeSessionCache::evict(const std::string& identity) noexcept
{
  const auto shared = m_shared;
  std::lock_guard<std::mutex> lock(shared->mutex);
  const auto found = shared->entries.find(identity);
  if (found == shared->entries.end()) {
    return false;
  }
  found->second.retiring = true;
  if (found->second.activeLeases == 0) {
    shared->entries.erase(found);
  }
  shared->condition.notify_all();
  return true;
}

void
OnnxRuntimeSessionCache::evictIdle() noexcept
{
  const auto shared = m_shared;
  std::lock_guard<std::mutex> lock(shared->mutex);
  const auto now = Clock::now();
  for (auto it = shared->entries.begin(); it != shared->entries.end();) {
    if (it->second.activeLeases == 0 &&
        now - it->second.lastRelease >= shared->config.idleTtl) {
      it = shared->entries.erase(it);
    }
    else {
      ++it;
    }
  }
  shared->condition.notify_all();
}

void
OnnxRuntimeSessionCache::close() noexcept
{
  const auto shared = m_shared;
  std::lock_guard<std::mutex> lock(shared->mutex);
  if (shared->closed) {
    return;
  }
  shared->closed = true;
  for (auto& item : shared->entries) {
    item.second.retiring = true;
  }
  for (auto it = shared->entries.begin(); it != shared->entries.end();) {
    if (it->second.activeLeases == 0) {
      it = shared->entries.erase(it);
    }
    else {
      ++it;
    }
  }
  for (auto& item : shared->jobs) {
    item.second->cancelled = true;
    item.second->condition.notify_all();
  }
  shared->condition.notify_all();
}

bool
OnnxRuntimeSessionCache::drain(std::chrono::milliseconds timeout)
{
  if (timeout.count() < 0) {
    throw std::invalid_argument("ONNX Runtime session cache drain timeout is negative");
  }
  const auto shared = m_shared;
  std::unique_lock<std::mutex> lock(shared->mutex);
  const auto deadline = Clock::now() + timeout;
  while (!shared->jobs.empty() || shared->activeLeases != 0) {
    if (shared->condition.wait_until(lock, deadline) == std::cv_status::timeout &&
        (!shared->jobs.empty() || shared->activeLeases != 0)) {
      return false;
    }
  }
  shared->entries.clear();
  return true;
}

OnnxRuntimeSessionCache::Counters
OnnxRuntimeSessionCache::counters() const noexcept
{
  const auto shared = m_shared;
  std::lock_guard<std::mutex> lock(shared->mutex);
  return Counters{shared->loads, shared->hits, shared->activeLeases,
                  static_cast<std::uint64_t>(shared->entries.size()),
                  static_cast<std::uint64_t>(shared->jobs.size())};
}

} // namespace ndnsf::di
