#pragma once

#include <mutex>

namespace ndnsf::di {

// Runtime timing records are parsed as line-oriented evidence.  All native
// producers of those records must use the same process-wide mutex so that a
// concurrent ONNX timing line cannot split a dependency timing record.
inline std::mutex&
runtimeTimingOutputMutex()
{
  static std::mutex mutex;
  return mutex;
}

} // namespace ndnsf::di
