#ifndef NDNSF_UAV_GROUND_STATION_TRACKING_CONTROLS_HPP
#define NDNSF_UAV_GROUND_STATION_TRACKING_CONTROLS_HPP

#include "ground-station-service-container.hpp"
#include "../shared/uav-person-json.hpp"

namespace {

std::string
trackingStateText(TrackingJobState state)
{
  switch (state) {
    case TrackingJobState::IDLE: return "IDLE";
    case TrackingJobState::PREPARING: return "PREPARING";
    case TrackingJobState::RUNNING: return "RUNNING";
    case TrackingJobState::CANCELLING: return "CANCELLING";
    case TrackingJobState::COMPLETED: return "COMPLETED";
    case TrackingJobState::FAILED: return "FAILED";
    case TrackingJobState::CANCELLED: return "CANCELLED";
  }
  throw std::logic_error("unknown native tracking state");
}

/** Host-thread presentation controls; only the single completion slot crosses threads. */
class GroundStationTrackingControls
{
private:
  using Json = ndnsf::examples::uav::person_json::Json;

  enum class ProbePhaseState { NOT_STARTED, PENDING, SETTLED, MISSED, SKIPPED };

  struct TelemetryReply
  {
    std::string runId, producer, droneId, nativeState, reason;
    size_t phase = 0;
    uint64_t windows = 0, startedMs = 0, completedMs = 0;
    bool verified = false;
  };

  struct Completion
  {
    bool start = false;
    size_t index = 0;
    TrackingAdmission admission = TrackingAdmission::INVALID;
    TrackingCancelResult cancellation = TrackingCancelResult::WRONG_JOB;
    std::string reason;
  };

  struct Slot
  {
    std::mutex mutex;
    bool alive = true;
    std::optional<Completion> value;
    std::vector<TelemetryReply> telemetry;
  };

public:
  GroundStationTrackingControls(GroundStationServiceContainer& runtime,
                                 std::vector<TrackingJobDefinition> definitions,
                                 bool automatic)
    : m_runtime(runtime)
    , m_autoRequested(automatic)
    , m_autoActive(automatic)
  {
    for (auto& definition : definitions) add(std::move(definition));
    if (automatic && m_definitions.empty()) {
      throw std::invalid_argument("tracking auto-start requires prepared job configurations");
    }
  }

  ~GroundStationTrackingControls()
  {
    std::lock_guard<std::mutex> lock(m_slot->mutex);
    m_slot->alive = false;
    m_slot->value.reset();
    m_slot->telemetry.clear();
  }

  GroundStationTrackingControls(const GroundStationTrackingControls&) = delete;
  GroundStationTrackingControls& operator=(const GroundStationTrackingControls&) = delete;

  const auto& definitions() const { return m_definitions; }
  const TrackingSnapshot& view() const { return m_view; }
  const std::string& message() const { return m_message; }
  bool pending() const { return m_pending; }
  bool automatic() const { return m_autoActive; }
  bool finished() const { return m_autoRequested && m_autoFinished; }
  bool failed() const { return m_storageFailed || m_autoFailed || m_probeFailed; }

  void
  addDefinition(const std::filesystem::path& path)
  {
    if (m_pending || m_autoActive || !idle()) {
      throw std::runtime_error("wait for tracking cleanup before loading another configuration");
    }
    add(loadTrackingJobDefinition(path));
  }

  bool
  canStart(size_t index) const
  {
    return !m_storageFailed && telemetryComplete() &&
      !m_autoActive && !m_pending && idle() && index < m_used.size() && !m_used[index];
  }

  bool
  canCancel() const
  {
    return !m_pending && (m_view.state == TrackingJobState::PREPARING ||
                          m_view.state == TrackingJobState::RUNNING);
  }

  bool
  start(size_t index)
  {
    return canStart(index) && queueStart(index);
  }

  bool
  cancel()
  {
    if (!canCancel()) return false;
    m_pending = true;
    const std::weak_ptr<Slot> weak = m_slot;
    const bool queued = m_runtime.cancelTracking(m_view.jobId,
      [weak] (TrackingCancelResult result, std::string reason) {
        if (auto slot = weak.lock()) {
          std::lock_guard<std::mutex> lock(slot->mutex);
          if (slot->alive) {
            slot->value = Completion{false, 0, TrackingAdmission::INVALID, result,
                                     reason.substr(0, 1024)};
          }
        }
      });
    if (!queued) rejectQueue();
    return queued;
  }

  void
  tick()
  {
    receiveCompletion();
    receiveTelemetry();
    m_view = m_runtime.runtimeSnapshot().tracking;
    observeTelemetry();
    observeCompletion();
    persist(false, false);
    if (!m_autoActive || m_pending || m_storageFailed) return;
    if (m_next != 0) {
      const auto& previous = m_definitions[m_next - 1];
      if (m_view.runId != previous.runId) return;
      if (m_view.state == TrackingJobState::FAILED || m_view.state == TrackingJobState::CANCELLED) {
        stopAutomatic(false);
        return;
      }
      if (m_view.state != TrackingJobState::COMPLETED || m_view.cleanupState != "CLEAN" ||
          !telemetryComplete()) return;
      if (m_probeFailed) { stopAutomatic(false); return; }
    }
    if (m_next == m_definitions.size()) stopAutomatic(true);
    else if (!queueStart(m_next)) {
      if (m_message.empty()) m_message = "Prepared tracking job cannot be started";
      stopAutomatic(false);
    }
  }

  /** Call after host shutdown while the borrowed container remains readable. */
  bool
  finalize(bool drained)
  {
    m_autoActive = false;
    receiveCompletion();
    receiveTelemetry();
    m_view = m_runtime.runtimeSnapshot().tracking;
    settleUnsuccessfulTelemetry();
    observeCompletion();
    persist(true, drained);
    const bool allCompleted = !m_completed.empty() &&
      std::all_of(m_completed.begin(), m_completed.end(), [] (bool complete) { return complete; });
    const bool success = drained && !m_storageFailed && !m_probeFailed && telemetryComplete() &&
      (m_autoRequested ? allCompleted && !m_autoFailed :
        m_view.state == TrackingJobState::COMPLETED && m_view.cleanupState == "CLEAN");
    if (!m_view.runId.empty()) {
      std::cout << "SPEC228_TRACKING_HOST_FINAL status=" << (success ? "success" : "failure")
                << " run=" << m_view.runId << " cleanup=" << (drained ? "CLEAN" : "INCOMPLETE")
                << std::endl;
    }
    return success;
  }

private:
  bool
  idle() const
  {
    return m_view.state == TrackingJobState::IDLE ||
      ((m_view.state == TrackingJobState::COMPLETED || m_view.state == TrackingJobState::FAILED ||
        m_view.state == TrackingJobState::CANCELLED) && m_view.cleanupState == "CLEAN");
  }

  void
  add(TrackingJobDefinition definition)
  {
    validateTrackingJobDefinition(definition);
    if (m_definitions.size() >= 16) throw std::invalid_argument("prepared tracking catalog is full");
    for (const auto& previous : m_definitions) {
      if (previous.runId == definition.runId || previous.jobId == definition.jobId ||
          previous.outputDirectory.lexically_normal() == definition.outputDirectory.lexically_normal()) {
        throw std::invalid_argument("prepared jobs require unique run/job/output identities");
      }
    }
    m_definitions.push_back(std::move(definition));
    m_used.push_back(false);
    m_completed.push_back(false);
  }

  bool
  queueStart(size_t index)
  {
    if (m_storageFailed || !telemetryComplete() || m_pending || !idle() || index >= m_definitions.size() || m_used[index]) return false;
    m_pending = true;
    m_message.clear();
    const std::weak_ptr<Slot> weak = m_slot;
    const bool queued = m_runtime.startTracking(m_definitions[index],
      [weak, index] (TrackingAdmission result, std::string reason) {
        if (auto slot = weak.lock()) {
          std::lock_guard<std::mutex> lock(slot->mutex);
          if (slot->alive) {
            slot->value = Completion{true, index, result, TrackingCancelResult::WRONG_JOB,
                                     reason.substr(0, 1024)};
          }
        }
      });
    if (!queued) rejectQueue();
    return queued;
  }

  void
  rejectQueue()
  {
    m_pending = false;
    m_message = "Tracking runtime is closed or another command is pending";
    if (m_autoActive) stopAutomatic(false);
  }

  void
  receiveCompletion()
  {
    std::optional<Completion> value;
    std::vector<TelemetryReply> telemetry;
    {
      std::lock_guard<std::mutex> lock(m_slot->mutex);
      value.swap(m_slot->value);
    }
    if (!value) return;
    m_pending = false;
    m_message = value->reason;
    if (value->start) {
      if (value->admission == TrackingAdmission::ACCEPTED) {
        m_used.at(value->index) = true;
        if (m_autoActive && value->index == m_next) ++m_next;
      }
      else {
        if (m_message.empty()) m_message = "Tracking admission rejected";
        if (m_autoActive) stopAutomatic(false);
      }
    }
    else if (value->cancellation == TrackingCancelResult::UNAUTHORIZED && m_message.empty()) {
      m_message = "Tracking cancellation is not authorized";
    }
  }

  void
  observeCompletion()
  {
    for (size_t i = 0; i < m_definitions.size(); ++i) {
      if (m_used[i] && m_view.runId == m_definitions[i].runId &&
          m_view.state == TrackingJobState::COMPLETED && m_view.cleanupState == "CLEAN" &&
          telemetryComplete() && !m_probeFailed) {
        m_completed[i] = true;
      }
    }
  }

  void
  stopAutomatic(bool success)
  {
    m_autoActive = false;
    m_autoFinished = true;
    m_autoFailed = !success;
  }

  bool
  telemetryComplete() const
  {
    return m_probeRunId.empty() || (m_probePending == 0 && m_probePhases[0] >= ProbePhaseState::SETTLED &&
      m_probePhases[2] >= ProbePhaseState::SETTLED &&
      (m_view.totalWindows <= 1 || m_probePhases[1] >= ProbePhaseState::SETTLED));
  }

  void
  receiveTelemetry()
  {
    std::vector<TelemetryReply> replies;
    {
      std::lock_guard<std::mutex> lock(m_slot->mutex);
      replies.swap(m_slot->telemetry);
    }
    for (auto& reply : replies) {
      if (reply.runId != m_probeRunId || reply.phase != m_activeProbePhase || m_probePending == 0) {
        m_probeFailed = true;
        m_message = "Late or mismatched ordinary telemetry report";
        continue;
      }
      --m_probePending;
      if (!reply.verified) {
        m_probeFailed = true;
        m_message = "Ordinary telemetry failed: " + reply.reason;
      }
      m_probeReports.push_back({{"runId", reply.runId}, {"phase", reply.phase},
        {"producerIdentity", reply.producer}, {"droneId", reply.droneId},
        {"nativeStateAtRequest", reply.nativeState}, {"verifiedWindowsAtRequest", reply.windows},
        {"verified", reply.verified}, {"reason", reply.reason},
        {"startedMonotonicMs", reply.startedMs}, {"completedMonotonicMs", reply.completedMs},
        {"elapsedMs", reply.completedMs >= reply.startedMs ? reply.completedMs - reply.startedMs : 0}});
    }
    if (m_probePending == 0 && m_activeProbePhase < 3 &&
        m_probePhases[m_activeProbePhase] == ProbePhaseState::PENDING) m_probePhases[m_activeProbePhase] = ProbePhaseState::SETTLED;
  }

  void
  settleUnsuccessfulTelemetry()
  {
    if (m_probePending != 0 || (m_view.state != TrackingJobState::FAILED &&
                               m_view.state != TrackingJobState::CANCELLED)) return;
    for (auto& phase : m_probePhases) {
      if (phase == ProbePhaseState::NOT_STARTED) phase = ProbePhaseState::SKIPPED;
    }
    m_probeFailed = true;
  }

  void
  observeTelemetry()
  {
    if (m_view.runId.empty()) return;
    if (m_view.runId != m_probeRunId) {
      if (!telemetryComplete()) {
        m_probeFailed = true;
        m_message = "Previous ordinary telemetry did not settle before mode switch";
        return;
      }
      m_probeRunId = m_view.runId;
      m_probePhases = {};
      m_probeReports = Json::array();
      m_probeFailed = false;
      m_lastTelemetry.clear();
    }
    if (m_probePending != 0) return;
    if (m_view.state == TrackingJobState::FAILED || m_view.state == TrackingJobState::CANCELLED) {
      settleUnsuccessfulTelemetry();
      return;
    }
    const auto found = std::find_if(m_definitions.begin(), m_definitions.end(),
      [this] (const auto& definition) { return definition.runId == m_view.runId; });
    if (found == m_definitions.end()) return;
    const bool active = m_view.state == TrackingJobState::PREPARING ||
                        m_view.state == TrackingJobState::RUNNING;
    const bool complete = m_view.state == TrackingJobState::COMPLETED && m_view.cleanupState == "CLEAN";
    size_t phase = 3;
    if (m_probePhases[0] == ProbePhaseState::NOT_STARTED && active) phase = 0;
    else if (m_probePhases[1] == ProbePhaseState::NOT_STARTED && m_view.state == TrackingJobState::RUNNING &&
             m_view.completedWindows > 0 && m_view.completedWindows < m_view.totalWindows) phase = 1;
    else if (complete && m_probePhases[2] == ProbePhaseState::NOT_STARTED) {
      if (m_probePhases[0] != ProbePhaseState::SETTLED || (m_view.totalWindows > 1 && m_probePhases[1] != ProbePhaseState::SETTLED)) {
        m_probeFailed = true;
        if (m_probePhases[0] != ProbePhaseState::SETTLED) {
          m_probePhases[0] = ProbePhaseState::MISSED;
        }
        if (m_view.totalWindows > 1 && m_probePhases[1] != ProbePhaseState::SETTLED) {
          m_probePhases[1] = ProbePhaseState::MISSED;
        }
        m_message = "Ordinary telemetry phase missed before native terminal";
      }
      phase = 2;
    }
    if (phase == 3) return;
    m_activeProbePhase = phase;
    m_probePhases[phase] = ProbePhaseState::PENDING;
    std::vector<std::pair<ndn::Name, std::string>> participants;
    for (const auto& source : found->sources) {
      participants.emplace_back(source.producerIdentity, source.droneId);
    }
    participants.emplace_back(found->computeIdentity, found->computeDroneId);
    for (const auto& [producer, droneId] : participants) {
      ++m_probePending;
      TelemetryReply reply;
      reply.runId = m_view.runId;
      reply.phase = phase;
      reply.producer = producer.toUri();
      reply.droneId = droneId;
      reply.nativeState = trackingStateText(m_view.state);
      reply.windows = m_view.completedWindows;
      const std::weak_ptr<Slot> weak = m_slot;
      auto completion = [weak, reply] (bool verified, std::string reason,
                                       uint64_t started, uint64_t ended) mutable {
        reply.verified = verified;
        reply.reason = std::move(reason);
        reply.startedMs = started;
        reply.completedMs = ended;
        if (auto slot = weak.lock()) {
          std::lock_guard<std::mutex> lock(slot->mutex);
          if (slot->alive && slot->telemetry.size() < 4) slot->telemetry.push_back(std::move(reply));
        }
      };
      if (!m_runtime.requestTrackingTelemetry(found->jobId, producer, droneId, completion)) {
        const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
        completion(false, "telemetry admission rejected", now, now);
      }
    }
  }

  static void
  writeStatusText(const std::filesystem::path& path, const std::string& text)
  {
    if (text.size() > 65536) throw std::runtime_error("tracking status byte bound");
    const auto temporary = path.parent_path() / ("." + path.filename().string() + ".tmp");
    if (std::filesystem::is_symlink(path) || std::filesystem::is_symlink(temporary)) {
      throw std::runtime_error("tracking status cannot replace a symlink");
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    output.flush();
    if (!output) throw std::runtime_error("tracking status write failed");
    output.close();
    std::filesystem::rename(temporary, path);
  }

  void
  persist(bool final, bool drained)
  {
    if (m_view.runId.empty() || m_storageFailed) return;
    const auto found = std::find_if(m_definitions.begin(), m_definitions.end(),
      [this] (const auto& definition) { return definition.runId == m_view.runId; });
    if (found == m_definitions.end()) return;
    try {
      using Json = ndnsf::examples::uav::person_json::Json;
      Json sources = Json::array();
      for (const auto& source : found->sources) {
        sources.push_back({{"cameraId", source.cameraId}, {"droneId", source.droneId},
          {"producerIdentity", source.producerIdentity.toUri()}, {"sourceId", source.sourceId},
          {"sourceEpoch", source.sourceEpoch}});
      }
      const Json status = {{"schema", "uav-main-tracking-job-v1"}, {"jobId", m_view.jobId},
        {"runId", m_view.runId}, {"missionId", m_view.missionId},
        {"wireMissionId", m_view.wireMissionId}, {"generation", m_view.generation},
        {"trackingMode", m_view.mode == TrackingMode::PERSON ? "person" : "vehicle"},
        {"state", trackingStateText(m_view.state)}, {"actualDevice", m_view.actualDevice},
        {"verifiedResultWindows", m_view.completedWindows}, {"totalWindows", m_view.totalWindows},
        {"lastResultDigest", m_view.lastResultDigest}, {"failureStage", m_view.failureStage},
        {"reason", m_view.reason.substr(0, 1024)}, {"cancelState", m_view.cancelState},
        {"cleanupState", m_view.cleanupState}, {"sources", sources},
        {"computeProducer", found->computeIdentity.toUri()},
        {"updatedMonotonicMs", m_view.updatedMonotonicMs},
        {"hostShutdown", final ? (drained ? "CLEAN" : "INCOMPLETE") : "RUNNING"},
        {"ordinaryTelemetryComplete", telemetryComplete()}, {"ordinaryTelemetryFailed", m_probeFailed}};
      const Json telemetry = {{"schema", "uav-main-tracking-telemetry-v1"},
        {"runId", m_probeRunId}, {"clockDomain", "CLOCK_MONOTONIC_MS"},
        {"complete", telemetryComplete()}, {"failed", m_probeFailed},
        {"phaseStates", {static_cast<int>(m_probePhases[0]), static_cast<int>(m_probePhases[1]),
                         static_cast<int>(m_probePhases[2])}}, {"reports", m_probeReports}};
      const auto telemetryText = telemetry.dump();
      if (telemetryText != m_lastTelemetry) {
        writeStatusText(found->outputDirectory / "tracking-telemetry.json", telemetryText);
        m_lastTelemetry = telemetryText;
      }
      const auto text = status.dump();
      if (m_lastStatus != text) {
        writeStatusText(found->outputDirectory / "tracking-status.json", text);
        m_lastStatus = text;
      }
    }
    catch (const std::exception& error) {
      m_storageFailed = true;
      m_message = error.what();
      if (m_autoActive) stopAutomatic(false);
      std::cerr << "SPEC228_TRACKING_STATUS_FAILED reason=" << m_message << std::endl;
    }
  }

private:
  GroundStationServiceContainer& m_runtime;
  std::shared_ptr<Slot> m_slot = std::make_shared<Slot>();
  std::vector<TrackingJobDefinition> m_definitions;
  std::vector<bool> m_used;
  std::vector<bool> m_completed;
  TrackingSnapshot m_view;
  bool m_pending = false;
  bool m_autoRequested = false;
  bool m_autoActive = false;
  bool m_autoFinished = false;
  bool m_autoFailed = false;
  bool m_storageFailed = false;
  size_t m_next = 0;
  std::string m_message;
  std::string m_lastStatus;
  std::string m_probeRunId;
  std::array<ProbePhaseState, 3> m_probePhases{};
  size_t m_probePending = 0;
  size_t m_activeProbePhase = 3;
  bool m_probeFailed = false;
  Json m_probeReports = Json::array();
  std::string m_lastTelemetry;
};

} // namespace

#endif // NDNSF_UAV_GROUND_STATION_TRACKING_CONTROLS_HPP
