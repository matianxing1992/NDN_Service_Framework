#pragma once

#include <mutex>
#include <string>

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

// Submit one complete machine-readable record through the ndn-cxx logging
// backend.  Callers must assemble the whole record before invoking this
// function; mixing multi-insertion stdout records with NDN_LOG output can
// corrupt a line when stdout and stderr are redirected to the same file.
void
logRuntimeEvidence(const std::string& record);

// Diagnostic records use the same process-safe ndn-cxx component but expose
// the caller's intended severity so operators can keep routine traces quiet
// while retaining warnings/errors.  Records must not contain prompt/answer
// text, token keys, logits, state tensors, or key bytes.
void
logRuntimeTrace(const std::string& record);

void
logRuntimeInfo(const std::string& record);

void
logRuntimeWarn(const std::string& record);

void
logRuntimeError(const std::string& record);

} // namespace ndnsf::di
