#ifndef NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_SESSION_CACHE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_SESSION_CACHE_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ndnsf::di {

/**
 * Provider-owned cache for immutable, already-loaded CPU ONNX sessions.
 *
 * The cache deliberately stores an opaque value.  The ONNX adapter owns its
 * concrete session type; callers receive a move-only lease and must create a
 * fresh request wrapper around the value.  Request IDs, grants, KV state and
 * execution evidence never enter this owner.
 */
class OnnxRuntimeSessionCache
{
public:
  struct Shared;

  struct Config
  {
    std::chrono::milliseconds idleTtl{120000};
    std::size_t maxEntries = 1;
  };

  struct Counters
  {
    std::uint64_t loads = 0;
    std::uint64_t hits = 0;
    std::uint64_t activeLeases = 0;
    std::uint64_t residentEntries = 0;
    std::uint64_t inFlightLoads = 0;
  };

  class Lease
  {
  public:
    Lease() noexcept = default;
    ~Lease() noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    bool valid() const noexcept { return static_cast<bool>(m_value); }
    explicit operator bool() const noexcept { return valid(); }
    bool cacheHit() const noexcept { return m_cacheHit; }
    std::shared_ptr<void> value() const noexcept { return m_value; }

  private:
    struct Release;
    Lease(std::shared_ptr<void> value,
          std::shared_ptr<Release> release,
          bool cacheHit) noexcept;
    std::shared_ptr<void> m_value;
    std::shared_ptr<Release> m_release;
    bool m_cacheHit = false;
    friend class OnnxRuntimeSessionCache;
  };

  using Loader = std::function<std::shared_ptr<void>()>;

  OnnxRuntimeSessionCache();
  explicit OnnxRuntimeSessionCache(Config config);
  ~OnnxRuntimeSessionCache() noexcept;
  OnnxRuntimeSessionCache(const OnnxRuntimeSessionCache&) = delete;
  OnnxRuntimeSessionCache& operator=(const OnnxRuntimeSessionCache&) = delete;

  Lease acquire(const std::string& identity,
                Loader loader,
                std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::time_point::max(),
                std::function<bool()> cancelled = {});

  bool evict(const std::string& identity) noexcept;
  void evictIdle() noexcept;
  void close() noexcept;
  bool drain(std::chrono::milliseconds timeout);
  Counters counters() const noexcept;

private:
  std::shared_ptr<Shared> m_shared;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_SESSION_CACHE_HPP
