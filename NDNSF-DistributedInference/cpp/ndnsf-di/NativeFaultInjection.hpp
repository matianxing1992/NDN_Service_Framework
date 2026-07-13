#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_FAULT_INJECTION_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_FAULT_INJECTION_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace ndnsf::di {

enum class NativeFaultPoint
{
  DependencyFetched,
  BeforeCompute,
  BeforePublish,
};

struct NativeFaultConfig
{
  std::string type;
  std::string role;
  std::uint64_t delayMs = 0;
};

class NativeFaultInjection
{
public:
  static NativeFaultInjection& instance();

  void configure(NativeFaultConfig config);
  NativeFaultConfig config() const;
  bool injected() const noexcept;
  void checkpoint(NativeFaultPoint point,
                  const std::string& role,
                  const std::string& sessionId);

private:
  mutable std::mutex m_mutex;
  NativeFaultConfig m_config;
  std::atomic<bool> m_injected{false};
};

} // namespace ndnsf::di

#endif
