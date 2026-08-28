#ifndef NDNSF_EXAMPLES_UAV_DIAGNOSTICS_HPP
#define NDNSF_EXAMPLES_UAV_DIAGNOSTICS_HPP

#include "UavProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <deque>
#include <utility>
#include <algorithm>
#include <optional>

namespace ndnsf::examples::uav {

enum class UavTraceStage
{
  MissionTrigger,
  RequestPublished,
  AckClosed,
  PlanCommitted,
  Selection,
  EvidenceInterest,
  EvidenceDataVerified,
  Execution,
  ReportPublished,
  TerminalAccepted,
  Failure,
};

const char* to_string(UavTraceStage stage) noexcept;

struct UavTraceEvent
{
  uint64_t timestampMs = 0;
  std::string missionId;
  std::string incidentId;
  ndn::Name requestId;
  UavTraceStage stage = UavTraceStage::MissionTrigger;
  ndn::Name requestedDataName;
  ndn::Name returnedDataName;
  ndn::Name producerIdentity;
  ndn::Name signerIdentity;
  uint64_t version = 0;
  std::string contentDigest;
  std::string detail;
};

/** Bounded correlation trace; registered incidents are never sampled away. */
class UavTraceRecorder
{
public:
  explicit UavTraceRecorder(std::size_t maxEvents = 4096,
                            uint32_t sampleRate = 1);

  void registerIncident(const std::string& incidentId);
  bool record(UavTraceEvent event, std::string* reason = nullptr);
  const std::vector<UavTraceEvent>& events() const noexcept { return m_events; }
  std::string snapshot() const;

  static bool containsTransportEndpoint(const UavTraceEvent& event) noexcept;
  static bool hasCompleteLineage(const UavTraceEvent& event) noexcept;

private:
  std::size_t m_maxEvents;
  uint32_t m_sampleRate;
  uint64_t m_seen = 0;
  std::set<std::string> m_registeredIncidents;
  std::vector<UavTraceEvent> m_events;
};

/**
 * Small bounded FIFO used to isolate detector/evidence pressure from command
 * processing.  It deliberately exposes no transport details and drops new
 * work when full, making overload observable instead of unbounded.
 */
class UavBoundedWorkQueue
{
public:
  explicit UavBoundedWorkQueue(std::size_t capacity = 32)
    : m_capacity(std::max<std::size_t>(1, capacity))
  {
  }

  bool tryPush(std::string work);
  std::optional<std::string> tryPop();
  std::size_t size() const noexcept { return m_items.size(); }
  std::size_t capacity() const noexcept { return m_capacity; }
  std::size_t dropped() const noexcept { return m_dropped; }
  bool overloaded() const noexcept { return m_dropped != 0; }

private:
  std::size_t m_capacity;
  std::size_t m_dropped = 0;
  std::deque<std::string> m_items;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_DIAGNOSTICS_HPP
