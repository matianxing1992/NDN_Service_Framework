#ifndef NDNSF_EXACT_DATA_CONGESTION_WINDOW_HPP
#define NDNSF_EXACT_DATA_CONGESTION_WINDOW_HPP

#include <ndn-cxx/util/rtt-estimator.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

namespace ndn_service_framework::detail {

/** Per-batch SegmentFetcher-style AIMD/CWA state, accessed under its owner's lock.
 * Names, authorization, scheduling and buffers remain with the exact fetch owner.
 */
class ExactDataCongestionWindow
{
public:
  static constexpr std::size_t INITIAL_WINDOW = 16;

  explicit
  ExactDataCongestionWindow(std::size_t limit, ndn::time::milliseconds initialRto)
    : m_limit(limit)
    , m_cwnd(std::min(INITIAL_WINDOW, limit))
    , m_rtt(makeRttOptions(initialRto))
    , m_peakWindow(size())
  {
    if (limit == 0) throw std::invalid_argument("exact Data window requires a resource bound");
  }

  std::size_t
  size() const
  {
    return std::min(m_limit, std::max<std::size_t>(1, std::floor(m_cwnd)));
  }

  void
  noteSent(std::size_t index)
  {
    m_highInterest = std::max(m_highInterest, index);
  }

  void
  acknowledge(std::size_t index, std::uint64_t congestionMark,
              std::optional<std::chrono::nanoseconds> sample, std::size_t inFlight)
  {
    m_highData = std::max(m_highData.value_or(0), index);
    if (sample && sample->count() > 0) {
      m_rtt.addMeasurement(ndn::time::nanoseconds(sample->count()),
                           std::max<std::size_t>(1, inFlight));
      ++m_rttSamples;
    }
    if (congestionMark != 0) {
      ++m_marks;
      decrease();
    }
    else {
      m_cwnd = std::min<double>(m_limit, m_cwnd +
        (m_cwnd < m_ssthresh ? m_options.aiStep : m_options.aiStep / std::floor(m_cwnd)));
      m_peakWindow = std::max(m_peakWindow, size());
    }
  }

  void
  loss()
  {
    ++m_losses;
    // Back off once for the recovery flight, not once for every lost packet.
    if (decrease()) m_rtt.backoffRto();
  }

  std::chrono::nanoseconds
  rto() const
  {
    return std::chrono::nanoseconds(m_rtt.getEstimatedRto().count());
  }

  std::size_t
  peakWindow() const
  {
    return m_peakWindow;
  }

  std::size_t
  losses() const
  {
    return m_losses;
  }

  std::size_t
  marks() const
  {
    return m_marks;
  }

  std::size_t
  decreases() const
  {
    return m_decreases;
  }

  std::size_t
  rttSamples() const
  {
    return m_rttSamples;
  }

private:
  static std::shared_ptr<const ndn::util::RttEstimator::Options>
  makeRttOptions(ndn::time::milliseconds initial)
  {
    if (initial.count() <= 0) throw std::invalid_argument("exact Data RTO must be positive");
    auto options = std::make_shared<ndn::util::RttEstimator::Options>();
    options->initialRto = initial;
    options->minRto = std::min(initial, ndn::time::milliseconds(50));
    options->maxRto = initial;
    return options;
  }

  bool
  decrease()
  {
    if (m_recoveryPoint && (!m_highData || *m_highData <= *m_recoveryPoint)) return false;
    m_recoveryPoint = m_highInterest;
    m_ssthresh = std::min<double>(m_limit, std::max(2.0, m_cwnd * m_options.mdCoef));
    m_cwnd = m_ssthresh;
    ++m_decreases;
    return true;
  }

private:
  const std::size_t m_limit;
  ndn::SegmentFetcherOptions m_options;
  double m_cwnd;
  double m_ssthresh = m_options.initSsthresh;
  ndn::util::RttEstimator m_rtt;
  std::size_t m_highInterest = 0;
  std::optional<std::size_t> m_highData;
  std::optional<std::size_t> m_recoveryPoint;
  std::size_t m_peakWindow;
  std::size_t m_losses = 0;
  std::size_t m_marks = 0;
  std::size_t m_decreases = 0;
  std::size_t m_rttSamples = 0;
};

} // namespace ndn_service_framework::detail

#endif // NDNSF_EXACT_DATA_CONGESTION_WINDOW_HPP
