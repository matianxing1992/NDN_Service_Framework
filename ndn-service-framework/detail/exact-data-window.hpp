#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_EXACT_DATA_WINDOW_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_EXACT_DATA_WINDOW_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ndn_service_framework::detail {

/** AIMD for a known, ordered packet plan. The caller owns slot credits and
 * synchronization. Sequence numbers are one-based publication-plan indices.
 * Mirrors SegmentFetcher's slow start, congestion avoidance and recovery point;
 * no version probe is needed because the complete names are already known.
 */
class ExactDataWindow
{
public:
  explicit
  ExactDataWindow(size_t capacity)
    : m_capacity(std::max<size_t>(1, capacity))
    , m_cwnd(std::min<size_t>(32, m_capacity))
  {
  }

  size_t
  size() const
  {
    return std::min(m_capacity, static_cast<size_t>(std::floor(m_cwnd)));
  }

  void
  acknowledge(size_t sequence)
  {
    m_highData = std::max(m_highData, sequence);
    m_cwnd = std::min<double>(m_capacity,
      m_cwnd + (m_cwnd < m_ssthresh ? 1.0 : 1.0 / std::floor(m_cwnd)));
  }

  void
  congest(size_t highestIssued)
  {
    // The first loss also counts when no Data has arrived yet.
    if (!m_reduced || m_highData > m_recoveryPoint) {
      m_reduced = true;
      m_recoveryPoint = highestIssued;
      m_ssthresh = std::max(2.0, m_cwnd * 0.5);
      m_cwnd = std::min<double>(m_capacity, m_ssthresh);
    }
  }

private:
  size_t m_capacity;
  double m_cwnd;
  double m_ssthresh = std::numeric_limits<double>::infinity();
  size_t m_highData = 0;
  size_t m_recoveryPoint = 0;
  bool m_reduced = false;
};

} // namespace ndn_service_framework::detail
#endif
