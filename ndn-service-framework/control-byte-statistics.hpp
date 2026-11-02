#ifndef NDN_SERVICE_FRAMEWORK_CONTROL_BYTE_STATISTICS_HPP
#define NDN_SERVICE_FRAMEWORK_CONTROL_BYTE_STATISTICS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace ndn_service_framework {

/** Logical plaintext representations, not unique objects or network byte totals. */
enum class ControlByteKind : std::size_t
{
  REQUEST_PREPARED,
  ACK_DECODED,
  ACK_DECODE_FAILED,
  SELECTION_PREPARED,
  ASSIGNMENT_INLINE,
  ASSIGNMENT_EXISTING_REFERENCE,
  ASSIGNMENT_EXTERNAL_PLAINTEXT,
  ASSIGNMENT_EXTERNAL_REFERENCE,
  REQUEST_INPUT_INLINE_MODE,
  REQUEST_INPUT_SANITIZED_MODE,
  REQUEST_INPUT_EXPLICIT_DISCOVERY_MODE,
  REQUEST_INPUT_EMPTY_DISCOVERY_MODE,
  REQUEST_INPUT_PUBLISHED,
  REQUEST_INPUT_PACKET_PUBLISHED,
  RESPONSE_DECODED,
  COUNT
};

/** Bounded request inventory with no user callbacks or retained message contents. */
class ControlByteStatistics
{
public:
  struct Entry
  {
    std::uint64_t bytes = 0;
    std::uint64_t calls = 0;
  };

  struct Snapshot
  {
    std::array<Entry, static_cast<std::size_t>(ControlByteKind::COUNT)> entries{};
    bool complete = true;
    bool sealed = false;
    bool responseBytesCaptured = false;
  };

  explicit ControlByteStatistics(bool captureResponseBytes = false)
    : m_captureResponseBytes(captureResponseBytes)
  {
  }

  bool
  capturesResponseBytes() const noexcept
  {
    return m_captureResponseBytes;
  }

  void
  invalidate() noexcept
  {
    m_failed.store(true);
  }

  /** Late records after sealing are excluded from the terminal inventory. */
  void
  record(ControlByteKind kind, std::uint64_t bytes) noexcept
  {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_values.sealed) return;
      const auto index = static_cast<std::size_t>(kind);
      if (index >= m_values.entries.size()) {
        m_values.complete = false;
        return;
      }
      auto& entry = m_values.entries[index];
      if (bytes > std::numeric_limits<std::uint64_t>::max() - entry.bytes ||
          entry.calls == std::numeric_limits<std::uint64_t>::max()) {
        m_values.complete = false;
        return;
      }
      entry.bytes += bytes;
      ++entry.calls;
    }
    catch (...) {
      // Observation cannot replace the authoritative protocol outcome.
      m_failed.store(true);
    }
  }

  Snapshot
  snapshot() const noexcept
  {
    return read(false);
  }

  /** Seal once before exposing the terminal request outcome to readers. */
  Snapshot
  seal() noexcept
  {
    return read(true);
  }

private:
  Snapshot
  read(bool seal) const noexcept
  {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (seal) m_values.sealed = true;
      auto result = m_values;
      result.responseBytesCaptured = m_captureResponseBytes;
      result.complete = result.complete && !m_failed.load();
      return result;
    }
    catch (...) {
      m_failed.store(true);
      Snapshot result;
      result.complete = false;
      return result;
    }
  }

  mutable std::mutex m_mutex;
  mutable std::atomic<bool> m_failed{false};
  mutable Snapshot m_values;
  const bool m_captureResponseBytes;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONTROL_BYTE_STATISTICS_HPP
