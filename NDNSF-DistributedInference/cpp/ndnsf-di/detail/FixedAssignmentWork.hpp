#pragma once

#include <cstdint>
#include <string>

namespace ndnsf::di::detail {

// Thread-local work counts, not cache-hit estimates. They count immutable
// factories and complete wire representations, not transient callback copies.
struct FixedAssignmentWork
{
  std::uint64_t placementBuilds = 0;
  std::uint64_t roleBuilds = 0;
  std::uint64_t roleEncodes = 0;
  std::uint64_t sourceEncodes = 0;
  std::uint64_t sourceParses = 0;
  // Actual canonical renderer calls for fixed fields in a V4 role graph,
  // including cold retention. This is distinct from role JSON construction.
  std::uint64_t snapshotFixedEncodes = 0;
};

FixedAssignmentWork& fixedAssignmentWork() noexcept;

// Inclusive scope on the executing thread; do not sum nested scope records.
// Instantiate inside deferred preparation calls. stage is a static label.
// Detailed records are enabled by NDNSF_PHASE_TIMING; counts remain available
// to C++ controls without logging or introducing cross-thread attribution.
class ScopedFixedAssignmentWork final
{
public:
  ScopedFixedAssignmentWork(const std::string& requestId, const char* stage) noexcept;
  ~ScopedFixedAssignmentWork() noexcept;
  ScopedFixedAssignmentWork(const ScopedFixedAssignmentWork&) = delete;
  ScopedFixedAssignmentWork& operator=(const ScopedFixedAssignmentWork&) = delete;

private:
  bool m_enabled;
  FixedAssignmentWork m_before;
  std::string m_requestId;
  const char* m_stage;
  int m_exceptions;
};

} // namespace ndnsf::di::detail
