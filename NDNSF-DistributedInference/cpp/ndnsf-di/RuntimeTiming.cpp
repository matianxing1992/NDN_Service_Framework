#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <ndn-cxx/util/logger.hpp>

namespace ndnsf::di {

NDN_LOG_INIT(ndnsf.di.RuntimeEvidence);

void
logRuntimeEvidence(const std::string& record)
{
  // WARN is intentional: qualification runs normally use *=WARN, so required
  // evidence remains available while verbose component logs stay filtered.
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_WARN(record);
}

void
logRuntimeTrace(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_TRACE(record);
}

void
logRuntimeInfo(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_INFO(record);
}

void
logRuntimeWarn(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_WARN(record);
}

void
logRuntimeError(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_ERROR(record);
}

} // namespace ndnsf::di
