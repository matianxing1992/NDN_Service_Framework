#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_RESOURCE_PROBE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_RESOURCE_PROBE_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ndnsf::di {

enum class ResourceProbeStatus
{
  Measured,
  ReadError,
  Unsupported,
  MalformedInput,
  IdentityMismatch,
  Stale,
  TimedOut
};

inline const char*
toString(ResourceProbeStatus status) noexcept
{
  switch (status) {
  case ResourceProbeStatus::Measured:
    return "measured";
  case ResourceProbeStatus::ReadError:
    return "read-error";
  case ResourceProbeStatus::Unsupported:
    return "unsupported";
  case ResourceProbeStatus::MalformedInput:
    return "malformed-input";
  case ResourceProbeStatus::IdentityMismatch:
    return "identity-mismatch";
  case ResourceProbeStatus::Stale:
    return "stale";
  case ResourceProbeStatus::TimedOut:
    return "timed-out";
  }
  return "unsupported";
}

struct ProviderResourceSnapshot
{
  ResourceProbeStatus status = ResourceProbeStatus::Unsupported;
  std::string source;
  std::string providerName;
  std::string providerBootId;
  std::uint64_t sequence = 0;
  std::int64_t measuredAtMs = 0;
  std::uint64_t hostTotalMemoryBytes = 0;
  std::uint64_t hostAvailableMemoryBytes = 0;
  std::uint64_t processRssBytes = 0;
  std::string errorCode;

  bool
  isMeasured() const noexcept
  {
    return status == ResourceProbeStatus::Measured && !source.empty() &&
           !providerName.empty() && !providerBootId.empty() && sequence > 0 &&
           measuredAtMs > 0 && hostTotalMemoryBytes > 0 &&
           hostAvailableMemoryBytes <= hostTotalMemoryBytes;
  }

  bool
  isFresh(std::int64_t atMs, std::int64_t maximumAgeMs) const noexcept
  {
    return isMeasured() && maximumAgeMs >= 0 && atMs >= measuredAtMs &&
           atMs - measuredAtMs <= maximumAgeMs;
  }

  bool
  matchesIdentity(const std::string& expectedProviderName,
                  const std::string& expectedProviderBootId) const noexcept
  {
    return providerName == expectedProviderName &&
           providerBootId == expectedProviderBootId;
  }
};

struct ProviderResourceProbeConfig
{
  std::string providerName;
  std::string providerBootId;
  std::chrono::milliseconds sampleInterval{1000};
  std::chrono::milliseconds readTimeout{250};
  std::chrono::milliseconds maximumAge{2000};
};

class ProviderResourceProbe
{
public:
  virtual ~ProviderResourceProbe() = default;

  virtual void start() = 0;
  virtual void stop() noexcept = 0;
  virtual ProviderResourceSnapshot
  sample(std::chrono::milliseconds timeout) = 0;
  virtual ProviderResourceSnapshot latest() const = 0;
};

struct ResourceTextRead
{
  ResourceProbeStatus status = ResourceProbeStatus::ReadError;
  std::string content;
  std::string errorCode;
};

class LinuxProviderResourceProbe final : public ProviderResourceProbe
{
public:
  using TextReader = std::function<ResourceTextRead(
    const std::string&, std::chrono::milliseconds)>;

  explicit LinuxProviderResourceProbe(ProviderResourceProbeConfig config,
                                      TextReader reader = {});
  ~LinuxProviderResourceProbe() override;

  LinuxProviderResourceProbe(const LinuxProviderResourceProbe&) = delete;
  LinuxProviderResourceProbe& operator=(const LinuxProviderResourceProbe&) = delete;

  void start() override;
  void stop() noexcept override;
  ProviderResourceSnapshot sample(std::chrono::milliseconds timeout) override;
  ProviderResourceSnapshot latest() const override;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_RESOURCE_PROBE_HPP
