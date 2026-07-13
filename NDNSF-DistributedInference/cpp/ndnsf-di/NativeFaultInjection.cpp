#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeFaultInjection.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace ndnsf::di {

namespace {

const std::set<std::string> FAULT_TYPES{
  "straggler", "missing-segment", "dependency-digest-mismatch",
  "stale-telemetry", "kv-eviction", "provider-boot-change",
  "late-old-output",
};

bool
matchesPoint(const std::string& type, NativeFaultPoint point)
{
  if (type == "straggler" || type == "kv-eviction" ||
      type == "provider-boot-change") {
    return point == NativeFaultPoint::BeforeCompute;
  }
  if (type == "missing-segment" || type == "dependency-digest-mismatch" ||
      type == "stale-telemetry") {
    return point == NativeFaultPoint::DependencyFetched;
  }
  return type == "late-old-output" && point == NativeFaultPoint::BeforePublish;
}

} // namespace

NativeFaultInjection&
NativeFaultInjection::instance()
{
  static NativeFaultInjection value;
  return value;
}

void
NativeFaultInjection::configure(NativeFaultConfig config)
{
  if (FAULT_TYPES.count(config.type) == 0 || config.role.empty()) {
    throw std::invalid_argument("NDNSF_DI_FAULT_CONFIG_INVALID");
  }
  if ((config.type == "straggler" || config.type == "late-old-output") &&
      config.delayMs == 0) {
    throw std::invalid_argument("NDNSF_DI_FAULT_DELAY_INVALID");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = std::move(config);
  m_injected.store(false);
}

NativeFaultConfig
NativeFaultInjection::config() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_config;
}

bool
NativeFaultInjection::injected() const noexcept
{
  return m_injected.load();
}

void
NativeFaultInjection::checkpoint(NativeFaultPoint point,
                                 const std::string& role,
                                 const std::string& sessionId)
{
  const auto active = config();
  if (active.role != role || !matchesPoint(active.type, point)) {
    return;
  }
  bool expected = false;
  if (!m_injected.compare_exchange_strong(expected, true)) {
    return;
  }
  std::cout << "NDNSF_DI_EXPERIMENT_FAULT_INJECTED"
            << " type=" << active.type
            << " role=" << role
            << " session=" << sessionId << std::endl;
  if (active.type == "straggler" || active.type == "late-old-output") {
    std::this_thread::sleep_for(std::chrono::milliseconds(active.delayMs));
    return;
  }
  throw std::runtime_error("NDNSF_DI_EXPERIMENT_FAULT:" + active.type);
}

} // namespace ndnsf::di
