#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_COLLABORATION_INPUT_BUDGET_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_COLLABORATION_INPUT_BUDGET_HPP
#include "../CollaborationDataflow.hpp"
#include <memory>
#include <mutex>

namespace ndn_service_framework::detail {
/** One Provider-wide accounting owner. Reservations include queued owners;
 * leases are shared with immutable results and release on the final owner. */
class CollaborationInputBudget : public std::enable_shared_from_this<CollaborationInputBudget>
{
public:
  struct Amount { size_t objects = 0, bytes = 0, callbacks = 0, reads = 0; };
  std::shared_ptr<void> reserve(const std::string& service, Amount amount,
                              const CollaborationAdmissionLimits& limits)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& used = m_usage[service];
    const auto fits = [](size_t value, size_t used, size_t limit) {
      return used <= limit && value <= limit - used;
    };
    if (!fits(amount.objects, used.objects, limits.maxRetainedObjects) ||
        !fits(amount.bytes, used.bytes, limits.maxRetainedBytes) ||
        !fits(amount.bytes, m_bytes, GLOBAL_BYTES) ||
        !fits(amount.callbacks, used.callbacks, limits.maxQueuedCallbacks) ||
        !fits(amount.reads, m_reads, GLOBAL_READS) ||
        !fits(amount.reads, used.reads, limits.maxInFlightReads)) {
      if (!(used.objects || used.bytes || used.callbacks || used.reads)) m_usage.erase(service);
      return {};
    }
    used.objects += amount.objects;
    used.bytes += amount.bytes;
    used.callbacks += amount.callbacks;
    used.reads += amount.reads;
    m_bytes += amount.bytes;
    m_reads += amount.reads;
    auto owner = shared_from_this();
    return std::shared_ptr<void>(this, [owner, service, amount](void*) {
      std::lock_guard<std::mutex> guard(owner->m_mutex);
      auto& used = owner->m_usage.at(service);
      used.objects -= amount.objects;
      used.bytes -= amount.bytes;
      used.callbacks -= amount.callbacks;
      used.reads -= amount.reads;
      owner->m_bytes -= amount.bytes;
      owner->m_reads -= amount.reads;
      if (!(used.objects || used.bytes || used.callbacks || used.reads)) owner->m_usage.erase(service);
    });
  }
private:
  static constexpr size_t GLOBAL_BYTES = 256 * 1024 * 1024;
  static constexpr size_t GLOBAL_READS = 256;
  std::mutex m_mutex;
  std::map<std::string, Amount> m_usage;
  size_t m_bytes = 0;
  size_t m_reads = 0;
};
} // namespace ndn_service_framework::detail
#endif
