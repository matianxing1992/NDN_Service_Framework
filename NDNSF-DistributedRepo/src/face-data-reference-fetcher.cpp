#include "face-data-reference-fetcher.hpp"
#include "ndn-service-framework/TimelineTrace.hpp"
#include "ndn-service-framework/exact-data-congestion-window.hpp"

#include <ndn-cxx/security/certificate-fetcher-offline.hpp>
#include <ndn-cxx/security/validation-policy.hpp>
#include <ndn-cxx/security/validator.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace ndnsf_distributed_repo::detail {
namespace {

constexpr std::size_t MAX_ORIGINAL_DATA_PACKET_COUNT = 1000000;
constexpr std::size_t INITIAL_SLOW_START_THRESHOLD = 64;

std::size_t
checkedPacketCount(std::size_t count, uint64_t bytes)
{
  if (count == 0 || count > MAX_ORIGINAL_DATA_PACKET_COUNT || count > bytes ||
      bytes == 0 || bytes > REPO_USER_STORAGE_OBJECT_MAX_BYTES) {
    throw std::invalid_argument("repo-original-packet-spool-budget-invalid");
  }
  return count;
}

struct FetchAdmission
{
  FetchAdmission(std::shared_ptr<std::atomic<bool>> active, std::shared_ptr<void> lifetime)
    : active(std::move(active))
    , lifetime(std::move(lifetime))
  {
    if (this->active->exchange(true)) {
      throw std::runtime_error("repo-original-packet-fetch-busy");
    }
  }

  ~FetchAdmission()
  {
    active->store(false);
  }

  std::shared_ptr<std::atomic<bool>> active;
  std::shared_ptr<void> lifetime;
};

/** Transport geometry only; authenticated digest/signature admission stays with the caller. */
class PacketGeometryPolicy final : public ndn::security::ValidationPolicy
{
public:
  explicit
  PacketGeometryPolicy(std::function<void(const ndn::Data&)> check)
    : m_check(std::move(check))
  {}

  void
  checkPolicy(const ndn::Data& data,
    const std::shared_ptr<ndn::security::ValidationState>& state,
    const ValidationContinuation& next) override
  {
    try {
      m_check(data);
    }
    catch (const std::exception& error) {
      state->fail({ndn::security::ValidationError::POLICY_ERROR, error.what()});
      return;
    }
    next(nullptr, state);
  }

  void
  checkPolicy(const ndn::Interest&,
    const std::shared_ptr<ndn::security::ValidationState>& state,
    const ValidationContinuation&) override
  {
    state->fail({ndn::security::ValidationError::POLICY_ERROR, "Data-only transport policy"});
  }

private:
  std::function<void(const ndn::Data&)> m_check;
};

struct SegmentedTransferResult
{
  std::shared_ptr<FaceDataReferencePacketSpool> spool;
  uint64_t wireBytes = 0;
  std::size_t packets = 0;
  std::size_t timeouts = 0;
  std::size_t nacks = 0;
  std::size_t marks = 0;
  std::string error;
};

/** Upstream owns AIMD/retries. Signals retain resources, never the waiting owner or fetcher. */
struct SegmentedTransfer : std::enable_shared_from_this<SegmentedTransfer>
{
  SegmentedTransfer(ndn::Face& face, ndn::Name prefix, std::size_t count, uint64_t budget)
    : face(face)
    , prefix(std::move(prefix))
    , count(count)
    , budget(budget)
  {
    result.spool = std::make_shared<FaceDataReferencePacketSpool>(count, budget);
  }

  void
  validate(const ndn::Data& data)
  {
    current();
    const auto& name = data.getName();
    if (finishing || name.size() != prefix.size() + 1 || !prefix.isPrefixOf(name) ||
        !name[-1].isSegment())
      throw std::runtime_error("repo-original-segment-name-mismatch");
    const auto index = name[-1].toSegment();
    if (index >= count || name[-1] != ndn::name::Component::fromSegment(index))
      throw std::runtime_error("repo-original-segment-index-mismatch");
    const auto& finalBlock = data.getFinalBlock();
    if ((index == 0 || finalBlock) &&
        (!finalBlock || !finalBlock->isSegment() || finalBlock->toSegment() != count - 1))
      throw std::runtime_error("repo-original-segment-final-block-mismatch");
    const auto bytes = data.wireEncode().size();
    if (bytes == 0 || bytes > ndn::MAX_NDN_PACKET_SIZE || bytes > budget - result.wireBytes ||
        (!offsets.empty() && (offsets.size() != count + 1 ||
                             bytes != offsets[index + 1] - offsets[index])))
      throw std::runtime_error("repo-original-segment-byte-budget");
  }

  void
  received(const ndn::Data& data)
  {
    if (finishing) return;
    try {
      current();
      const auto& block = data.wireEncode();
      const std::vector<uint8_t> wire(block.begin(), block.end());
      result.spool->append(static_cast<std::size_t>(data.getName()[-1].toSegment()), wire);
      result.wireBytes += block.size();
      ++result.packets;
      if (data.getCongestionMark() != 0) ++result.marks;
    }
    catch (const std::exception& error) {
      finish(error.what());
    }
  }

  void
  finish(const std::string& error)
  {
    if (!error.empty() && result.error.empty()) result.error = error;
    if (finishing) return;
    finishing = true;
    const auto self = shared_from_this();
    // Leave an emitting signal before stop, then drain its queued cancellations/self-release.
    boost::asio::post(face.getIoContext(), [self] {
      const auto fetcher = self->fetcher.lock();
      if (fetcher) fetcher->stop();
      boost::asio::post(self->face.getIoContext(), [self, fetcher] {
        if (self->result.error.empty()) {
          try {
            self->current();
            if (self->result.packets != self->count)
              throw std::runtime_error("repo-original-segment-count-mismatch");
          }
          catch (const std::exception& error) {
            self->result.error = error.what();
          }
        }
        self->complete(std::move(self->result));
      });
    });
  }

  std::function<void(const std::string&)>
  start(const ndn::Interest& interest, const ndn::SegmentFetcher::Options& options)
  {
    const auto self = shared_from_this();
    const std::weak_ptr<SegmentedTransfer> weak = self;
    validator = std::make_shared<ndn::security::Validator>(
      std::make_unique<PacketGeometryPolicy>([weak](const ndn::Data& data) {
        const auto state = weak.lock();
        if (!state) throw std::runtime_error("repo-original-segment-owner-closed");
        state->validate(data);
      }), std::make_unique<ndn::security::CertificateFetcherOffline>());
    const auto upstream = ndn::SegmentFetcher::start(face, interest, *validator, options);
    fetcher = upstream;
    try {
      upstream->onError.connect([self](uint32_t code, const std::string& reason) {
        self->finish("repo-original-segment-fetcher-" + std::to_string(code) + ": " + reason);
      });
      upstream->afterSegmentValidated.connect([self](const ndn::Data& data) {
        self->received(data);
      });
      upstream->afterSegmentTimedOut.connect([self] { ++self->result.timeouts; });
      upstream->afterSegmentNacked.connect([self] { ++self->result.nacks; });
      upstream->onInOrderComplete.connect([self] { self->finish({}); });
    }
    catch (...) {
      upstream->stop();
      throw;
    }
    return [weak, ownerFace = &face](const std::string& reason) {
      boost::asio::post(ownerFace->getIoContext(), [weak, reason] {
        if (const auto state = weak.lock()) state->finish(reason);
      });
    };
  }

  ndn::Face& face;
  const ndn::Name prefix;
  const std::size_t count;
  const uint64_t budget;
  std::vector<uint64_t> offsets;
  std::function<void()> current;
  std::function<void(SegmentedTransferResult)> complete;
  std::shared_ptr<FetchAdmission> admission;
  std::shared_ptr<ndn::security::Validator> validator;
  std::weak_ptr<ndn::SegmentFetcher> fetcher;
  SegmentedTransferResult result;
  bool finishing = false;
};

ndn::Name
expectedPacketName(const RepoDataReference& reference, uint64_t offset)
{
  if (reference.packetSetMetadata) {
    return ndn::Name(reference.packetSetMetadata->packetNames.at(offset));
  }
  ndn::Name name(reference.dataPrefix);
  if (reference.hasFinalSegment) {
    name.appendSegment(reference.firstSegment + offset);
  }
  return name;
}

struct FetchState : std::enable_shared_from_this<FetchState>
{
  FetchState(ndn::Face& faceArg, const RepoDataReference& referenceArg,
             std::size_t packetCountArg, std::size_t windowSizeArg,
             std::chrono::milliseconds interestLifetimeArg,
             std::chrono::steady_clock::time_point deadlineArg,
             std::shared_ptr<FetchAdmission> admissionArg)
    : face(faceArg)
    , strand(boost::asio::make_strand(faceArg.getIoContext()))
    , reference(referenceArg)
    , packetCount(packetCountArg)
    , windowCapacity(windowSizeArg == 0 ? packetCountArg : std::min(packetCountArg, windowSizeArg))
    , congestion(windowCapacity, ndn::time::milliseconds(
        std::min(interestLifetimeArg, std::chrono::milliseconds(1000)).count()),
        INITIAL_SLOW_START_THRESHOLD)
    , interestLifetime(interestLifetimeArg)
    , deadline(deadlineArg)
    , admission(std::move(admissionArg))
    , spool(std::make_shared<FaceDataReferencePacketSpool>(
        packetCountArg, referenceArg.expectedSize))
  {}

  ~FetchState()
  {
    if (standardCancel) {
      try { standardCancel("repo-original-packet-fetch-cancelled"); }
      catch (...) {}
    }
    if (!measureWindow || !firstSent)
      return;
    try {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finishedAt.value_or(lastObservation) - *firstSent).count();
      if (standardTransport) {
        ndn_service_framework::logControlTiming("repo-fetch", "window",
          ndn::Name(reference.dataPrefix), {
            {"transport", "segment-fetcher"},
            {"status", done && error.empty() ? "complete" : "failed"},
            {"packets", std::to_string(packetCount)},
            {"received", std::to_string(received)},
            {"wire_bytes", std::to_string(receivedWireBytes)},
            {"elapsed_ns", std::to_string(elapsed)},
            {"timeouts", std::to_string(timeoutRetries)},
            {"nacks", std::to_string(nackRetries)},
            {"marks", std::to_string(standardMarks)},
            {"window_metrics", "upstream-private"}});
        return;
      }
      ndn_service_framework::logControlTiming("repo-fetch", "window",
        ndn::Name(reference.dataPrefix), {
          {"status", done && error.empty() ? "complete" : "failed"},
          {"packets", std::to_string(packetCount)},
          {"received", std::to_string(received)},
          {"failed_packet_index", failedPacketIndex ? std::to_string(*failedPacketIndex) : "-1"},
          {"failed_attempt", std::to_string(failedAttempt)},
          {"failed_lifetime_ms", std::to_string(failedLifetime.count())},
          {"wire_bytes", std::to_string(receivedWireBytes)},
          {"elapsed_ns", std::to_string(elapsed)},
          {"initial_cwnd", std::to_string(std::min(
            ndn_service_framework::detail::ExactDataCongestionWindow::INITIAL_WINDOW,
            windowCapacity))},
          {"capacity", std::to_string(windowCapacity)},
          {"peak_cwnd", std::to_string(congestion.peakWindow())},
          {"final_cwnd", std::to_string(congestion.size())},
          {"peak_inflight", std::to_string(peakInFlight)},
          {"mean_inflight", std::to_string(elapsed > 0 ? inFlightNs / elapsed : 0)},
          {"window_full_ns", std::to_string(windowFullNs)},
          {"to_32_ns", std::to_string(windowReachedNs[0])},
          {"to_64_ns", std::to_string(windowReachedNs[1])},
          {"to_128_ns", std::to_string(windowReachedNs[2])},
          {"to_256_ns", std::to_string(windowReachedNs[3])},
          {"rtt_samples", std::to_string(congestion.rttSamples())},
          {"rtt_mean_ns", std::to_string(congestion.rttSamples() > 0
            ? rttTotalNs / congestion.rttSamples() : 0)},
          {"rtt_min_ns", std::to_string(rttMinNs)},
          {"rtt_max_ns", std::to_string(rttMaxNs)},
          {"rto_ns", std::to_string(congestion.rto().count())},
          {"losses", std::to_string(congestion.losses())},
          {"timeouts", std::to_string(timeoutRetries)},
          {"nacks", std::to_string(nackRetries)},
          {"marks", std::to_string(congestion.marks())},
          {"decreases", std::to_string(congestion.decreases())}});
    }
    catch (...) {
      // Observation cannot change transfer results or callback cleanup.
    }
  }

  // Called under mutex after each occupancy/window change. Keep only aggregates;
  // samples include Face scheduling, not just propagation on the network.
  void
  observeWindow()
  {
    if (!measureWindow)
      return;
    const auto now = std::chrono::steady_clock::now();
    if (!firstSent) {
      firstSent = now;
    }
    else {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - lastObservation).count();
      inFlightNs += static_cast<long double>(observedInFlight) * elapsed;
      if (observedInFlight >= observedWindow)
        windowFullNs += elapsed;
    }
    lastObservation = now;
    observedInFlight = inFlight;
    observedWindow = congestion.size();
    peakInFlight = std::max(peakInFlight, inFlight);
    for (std::size_t i = 0; i < windowReachedNs.size(); ++i) {
      if (windowReachedNs[i] < 0 && congestion.size() >= (32U << i)) {
        windowReachedNs[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
          now - *firstSent).count();
      }
    }
  }

  void
  prepareSegmentedCandidate()
  {
    if (packetCount < 2 || packetCount > MAX_NAMED_PACKET_COUNT ||
        reference.expectedSize > MAX_NAMED_PACKET_WIRE_BYTES)
      return;
    const auto first = packetName(0);
    if (first.empty() || first[-1] != ndn::name::Component::fromSegment(0)) return;
    const auto prefix = first.getPrefix(-1);
    if (prefix.empty()) return;
    for (std::size_t index = 1; index < packetCount; ++index) {
      if (packetName(index) != ndn::Name(prefix).appendSegment(index)) return;
    }
    standardPrefix = prefix;
    bootstrapPending = true;
  }

  void
  startStandard()
  {
    try {
      const auto transfer = std::make_shared<SegmentedTransfer>(
        face, *standardPrefix, packetCount, reference.expectedSize);
      transfer->admission = admission;
      if (reference.packetSetMetadata)
        transfer->offsets = reference.packetSetMetadata->packetOffsets;
      const std::weak_ptr<FetchState> weak = shared_from_this();
      transfer->current = [weak] {
        const auto owner = weak.lock();
        if (!owner) throw std::runtime_error("repo-original-packet-fetch-cancelled");
        if (owner->requireCurrent) owner->requireCurrent();
        std::lock_guard<std::mutex> lock(owner->mutex);
        if (owner->done || owner->standardCancelling ||
            std::chrono::steady_clock::now() >= owner->deadline)
          throw std::runtime_error("repo-original-packet-fetch-cancelled");
      };
      transfer->complete = [weak](SegmentedTransferResult result) {
        const auto owner = weak.lock();
        if (!owner) return;
        std::lock_guard<std::mutex> lock(owner->mutex);
        if (owner->done) return;
        if (!owner->error.empty()) result.error = owner->error;
        if (result.error.empty() && owner->exactSize &&
            result.wireBytes != owner->reference.expectedSize)
          result.error = "repo-original-packet-size-mismatch";
        owner->spool = result.error.empty() ? std::move(result.spool) : nullptr;
        owner->received = result.packets;
        owner->receivedWireBytes = result.wireBytes;
        owner->timeoutRetries += result.timeouts;
        owner->nackRetries += result.nacks;
        owner->standardMarks += result.marks;
        owner->error = std::move(result.error);
        owner->standardCancel = {};
        owner->finishedAt = std::chrono::steady_clock::now();
        owner->done = true;
        owner->condition.notify_all();
      };
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero())
        throw std::runtime_error("repo-original-packet-fetch-deadline");
      ndn::SegmentFetcher::Options options;
      options.probeLatestVersion = false;
      options.inOrder = true;
      options.flowControlWindow = std::min(windowCapacity, options.flowControlWindow);
      options.initSsthresh = std::min(windowCapacity, INITIAL_SLOW_START_THRESHOLD);
      options.interestLifetime = ndn::time::milliseconds(std::min({
        interestLifetime, remaining, std::chrono::milliseconds(1000)}).count());
      // The library schedules retries independently of the Face lifetime. Do
      // not leave an expired network Interest waiting for a longer local RTO.
      options.rttOptions.maxRto = std::min(options.rttOptions.maxRto,
        ndn::time::duration_cast<ndn::time::nanoseconds>(options.interestLifetime));
      options.rttOptions.initialRto = std::min(options.rttOptions.initialRto,
        options.rttOptions.maxRto);
      options.rttOptions.minRto = std::min(options.rttOptions.minRto,
        options.rttOptions.maxRto);
      options.maxTimeout = std::min(options.maxTimeout,
        ndn::time::milliseconds(remaining.count()));
      ndn::Interest interest(packetName(0));
      interest.setMustBeFresh(false);
      if (!reference.forwardingHint.empty())
        interest.setForwardingHint({ndn::Name(reference.forwardingHint)});
      auto cancel = transfer->start(interest, options);
      std::lock_guard<std::mutex> lock(mutex);
      standardCancel = std::move(cancel);
    }
    catch (const std::exception& error) {
      fail(error.what());
    }
  }

  void
  launchWindow()
  {
    if (standardTransport || !checkCurrent())
      return;
    while (true) {
      std::size_t index = 0;
      unsigned attempt = 1;
      std::chrono::milliseconds minimumRto{0};
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done || standardTransport) {
          return;
        }
        if (retryQueue.empty() && nextPacket == packetCount) {
          if (inFlight == 0 && received == packetCount) {
            if (std::chrono::steady_clock::now() >= deadline) {
              failLocked("repo-original-packet-fetch-deadline");
            }
            else if (exactSize && receivedWireBytes != reference.expectedSize) {
              failLocked("repo-original-packet-size-mismatch");
            }
            else {
              observeWindow();
              finishedAt = lastObservation;
              done = true;
              condition.notify_all();
            }
          }
          return;
        }
        if (inFlight >= congestion.size()) {
          return;
        }
        if (bootstrapPending && retryQueue.empty() && nextPacket >= std::min(
              windowCapacity, ndn_service_framework::detail::ExactDataCongestionWindow::INITIAL_WINDOW))
          return;
        if (!retryQueue.empty()) {
          index = retryQueue.front().index;
          attempt = retryQueue.front().attempt;
          minimumRto = retryQueue.front().minimumRto;
          retryQueue.pop_front();
        }
        else {
          index = nextPacket++;
        }
        congestion.noteSent(index);
        ++inFlight;
        observeWindow();
      }

      expressPacket(index, attempt, minimumRto);
    }
  }

  void
  expressPacket(std::size_t index, unsigned attempt, std::chrono::milliseconds minimumRto)
  {
    if (standardTransport || !checkCurrent())
      return;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      fail("repo-original-packet-fetch-deadline");
      return;
    }
    ndn::Interest interest(packetName(index));
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    if (!reference.forwardingHint.empty()) {
      interest.setForwardingHint({ndn::Name(reference.forwardingHint)});
    }

    const auto self = shared_from_this();
    try {
      std::lock_guard<std::mutex> lock(mutex);
      if (done || standardTransport) {
        return;
      }
      const auto rto = std::max({std::chrono::milliseconds(1), minimumRto,
        std::chrono::duration_cast<std::chrono::milliseconds>(congestion.rto())});
      const auto lifetime = std::min({interestLifetime, remaining, rto,
                                      std::chrono::milliseconds(1000)});
      interest.setInterestLifetime(ndn::time::milliseconds(lifetime.count()));
      auto pending = pendingInterests.emplace(
        index, PendingPacket{}).first;
      pending->second.attempt = attempt;
      pending->second.lifetime = lifetime;
      pending->second.sentAt = std::chrono::steady_clock::now();
      // expressInterest posts registration to the Face io_context. Keep the
      // state lock until its handle is installed so an early callback or a
      // sibling failure cannot remove a placeholder before registration.
      pending->second.handle = face.expressInterest(
        interest,
        [self, index, attempt] (const ndn::Interest&, const ndn::Data& data) {
          boost::asio::post(self->strand, [self, index, attempt, data] {
            self->onData(index, attempt, data);
          });
        },
        [self, index, attempt] (const ndn::Interest&, const ndn::lp::Nack&) {
          boost::asio::post(self->strand, [self, index, attempt] {
            self->onFailure(index, attempt, "repo-original-packet-nack");
          });
        },
        [self, index, attempt] (const ndn::Interest&) {
          boost::asio::post(self->strand, [self, index, attempt] {
            self->onFailure(index, attempt, "repo-original-packet-timeout");
          });
        });
    }
    catch (const std::exception&) {
      onFailure(index, attempt, "repo-original-packet-interest-failed");
      return;
    }
  }

  void
  onData(std::size_t index, unsigned attempt, const ndn::Data& data)
  {
    if (standardTransport || !checkCurrent())
      return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto pending = pendingInterests.find(index);
      if (done || standardTransport || pending == pendingInterests.end() ||
          pending->second.attempt != attempt) {
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        failLocked("repo-original-packet-fetch-deadline");
        return;
      }
    }
    std::vector<uint8_t> wire;
    try {
      if (data.getName() != packetName(index)) {
        onFailure(index, attempt, "repo-original-packet-name-mismatch");
        return;
      }
      const auto& block = data.wireEncode();
      if (block.size() == 0 || block.size() > ndn::MAX_NDN_PACKET_SIZE) {
        onFailure(index, attempt, "repo-original-packet-wire-size-invalid");
        return;
      }
      wire.assign(block.begin(), block.end());
    }
    catch (const std::exception&) {
      onFailure(index, attempt, "repo-original-packet-wire-encoding-failed");
      return;
    }

    bool promote = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done || standardTransport) {
        return;
      }
      const auto pending = pendingInterests.find(index);
      if (pending == pendingInterests.end() || pending->second.attempt != attempt) {
        return;
      }
      if (index == 0 && bootstrapPending) {
        bootstrapPending = false;
        const auto& finalBlock = data.getFinalBlock();
        try {
          promote = finalBlock && finalBlock->isSegment() &&
                    finalBlock->toSegment() == packetCount - 1;
        }
        catch (const ndn::tlv::Error&) {
          // Opaque packet sets do not require interpreting segmentation metadata.
        }
        if (promote) {
          standardTransport = true;
          for (auto& [packet, entry] : pendingInterests) {
            (void)packet;
            entry.handle.cancel();
          }
          pendingInterests.clear();
          retryQueue.clear();
          inFlight = 0;
          received = 0;
          receivedWireBytes = 0;
          spool.reset();
        }
      }
      if (!promote) {
        const auto sample = attempt == 1
          ? std::make_optional(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - pending->second.sentAt))
          : std::nullopt;
        congestion.acknowledge(index, data.getCongestionMark(), sample, inFlight);
        if (measureWindow && sample && sample->count() > 0) {
          const auto ns = static_cast<uint64_t>(sample->count());
          rttTotalNs += ns;
          rttMinNs = rttMinNs == 0 ? ns : std::min(rttMinNs, ns);
          rttMaxNs = std::max(rttMaxNs, ns);
        }
        pendingInterests.erase(pending);
        if (inFlight > 0) {
          --inFlight;
        }
        observeWindow();
        if (wire.size() > reference.expectedSize - receivedWireBytes) {
          failLocked("repo-original-packet-size-exceeds-reference");
          return;
        }
        try {
          spool->append(index, wire);
        }
        catch (const std::exception&) {
          failLocked("repo-original-packet-spool-write-failed");
          return;
        }
        receivedWireBytes += wire.size();
        ++received;
      }
    }
    if (promote) startStandard();
    else launchWindow();
  }

  void
  onFailure(std::size_t index, unsigned attempt, const std::string& message)
  {
    if (standardTransport || !checkCurrent())
      return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto pending = pendingInterests.find(index);
      if (done || standardTransport || pending == pendingInterests.end() ||
          pending->second.attempt != attempt)
        return;
      constexpr unsigned MAX_ATTEMPTS = 3;
      if (attempt >= MAX_ATTEMPTS || std::chrono::steady_clock::now() >= deadline ||
          (message != "repo-original-packet-timeout" && message != "repo-original-packet-nack")) {
        if (measureWindow) {
          failedPacketIndex = index;
          failedAttempt = attempt;
          failedLifetime = pending->second.lifetime;
        }
        failLocked(message);
        return;
      }
      pending->second.handle.cancel();
      if (message == "repo-original-packet-timeout")
        ++timeoutRetries;
      else
        ++nackRetries;
      congestion.loss();
      // Fresh ACKs for other packets may lower the shared estimate. Preserve
      // this packet's recovery backoff under the original lifetime/deadline caps.
      const auto minimumRto = std::min(pending->second.lifetime * 2,
                                       std::chrono::milliseconds(1000));
      pendingInterests.erase(pending);
      if (inFlight > 0)
        --inFlight;
      retryQueue.push_back({index, attempt + 1, minimumRto});
      observeWindow();
    }
    // Like SegmentFetcher, retries take priority but share the reduced window.
    // Queued retries retain their immutable slot and bounded attempt number.
    launchWindow();
  }

  void
  fail(const std::string& message)
  {
    std::lock_guard<std::mutex> lock(mutex);
    failLocked(message);
  }

  void
  failLocked(const std::string& message)
  {
    if (done) {
      return;
    }
    if (standardTransport && standardCancel) {
      if (!standardCancelling) {
        standardCancelling = true;
        error = message;
        standardCancel(message);
      }
      return;
    }
    observeWindow();
    finishedAt = lastObservation;
    done = true;
    error = message;
    for (auto& [index, pending] : pendingInterests) {
      (void)index;
      pending.handle.cancel();
    }
    pendingInterests.clear();
    retryQueue.clear();
    spool.reset();
    condition.notify_all();
  }

  bool
  checkCurrent()
  {
    try {
      if (requireCurrent)
        requireCurrent();
      return true;
    }
    catch (...) {
      fail(std::chrono::steady_clock::now() >= deadline
        ? "repo-original-packet-fetch-deadline"
        : "repo-original-packet-fetch-cancelled");
      return false;
    }
  }

  ndn::Name
  packetName(std::size_t index) const
  {
    return names.empty() ? expectedPacketName(reference, index) : names.at(index);
  }

  // Set once before posting launchWindow; the historic reference path retains exact-size checks.
  bool exactSize = true;
  std::vector<ndn::Name> names;
  std::function<void()> requireCurrent;
  ndn::Face& face;
  boost::asio::strand<boost::asio::io_context::executor_type> strand;
  const RepoDataReference reference;
  const std::size_t packetCount;
  const std::size_t windowCapacity;
  ndn_service_framework::detail::ExactDataCongestionWindow congestion;
  const std::chrono::milliseconds interestLifetime;
  const std::chrono::steady_clock::time_point deadline;
  std::shared_ptr<FetchAdmission> admission;
  std::optional<ndn::Name> standardPrefix;
  std::function<void(const std::string&)> standardCancel;
  bool bootstrapPending = false;
  std::atomic<bool> standardTransport{false};
  bool standardCancelling = false;
  std::size_t standardMarks = 0;
  std::mutex mutex;
  std::condition_variable condition;
  std::shared_ptr<FaceDataReferencePacketSpool> spool;
  struct PendingPacket
  {
    ndn::PendingInterestHandle handle;
    unsigned attempt = 0;
    std::chrono::milliseconds lifetime{0};
    std::chrono::steady_clock::time_point sentAt;
  };
  std::map<std::size_t, PendingPacket> pendingInterests;
  struct RetryPacket
  {
    std::size_t index;
    unsigned attempt;
    std::chrono::milliseconds minimumRto;
  };
  std::deque<RetryPacket> retryQueue;
  std::size_t nextPacket = 0;
  std::size_t inFlight = 0;
  std::size_t received = 0;
  std::optional<std::size_t> failedPacketIndex;
  unsigned failedAttempt = 0;
  std::chrono::milliseconds failedLifetime{0};
  uint64_t receivedWireBytes = 0;
  const bool measureWindow = ndn_service_framework::controlTimingEnvEnabled();
  std::optional<std::chrono::steady_clock::time_point> firstSent;
  std::optional<std::chrono::steady_clock::time_point> finishedAt;
  std::chrono::steady_clock::time_point lastObservation;
  std::array<int64_t, 4> windowReachedNs{{-1, -1, -1, -1}};
  std::size_t observedInFlight = 0;
  std::size_t observedWindow = 0;
  std::size_t peakInFlight = 0;
  long double inFlightNs = 0;
  uint64_t windowFullNs = 0;
  uint64_t rttTotalNs = 0;
  uint64_t rttMinNs = 0;
  uint64_t rttMaxNs = 0;
  std::size_t timeoutRetries = 0;
  std::size_t nackRetries = 0;
  bool done = false;
  std::string error;
};

} // namespace

FaceDataReferencePacketSpool::FaceDataReferencePacketSpool(
  std::size_t packetCount, uint64_t maxWireBytes)
  : m_file(std::tmpfile(), &std::fclose)
  , m_packetOffsets(checkedPacketCount(packetCount, maxWireBytes), -1)
  , m_packetLengths(packetCount, 0)
  , m_packetPresent(packetCount, false)
  , m_maxWireBytes(maxWireBytes)
{
  if (!m_file || packetCount == 0 || maxWireBytes == 0) {
    throw std::runtime_error("repo-original-packet-spool-initialization-failed");
  }
  if (std::setvbuf(m_file.get(), m_ioBuffer.data(), _IOFBF, m_ioBuffer.size()) != 0) {
    throw std::runtime_error("repo-original-packet-spool-buffer-initialization-failed");
  }
}

uint64_t
FaceDataReferencePacketSpool::totalWireBytes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_totalWireBytes;
}

void
FaceDataReferencePacketSpool::finalize()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_finalized) {
    return;
  }
  m_logicalOffsets.assign(m_packetLengths.size() + 1, 0);
  for (std::size_t i = 0; i < m_packetLengths.size(); ++i) {
    if (!m_packetPresent[i] ||
        m_logicalOffsets[i] > std::numeric_limits<uint64_t>::max() - m_packetLengths[i]) {
      throw std::runtime_error("repo-original-packet-spool-incomplete");
    }
    m_logicalOffsets[i + 1] = m_logicalOffsets[i] + m_packetLengths[i];
  }
  if (m_logicalOffsets.back() != m_totalWireBytes) {
    throw std::runtime_error("repo-original-packet-spool-length-mismatch");
  }
  if (std::fflush(m_file.get()) != 0) {
    throw std::runtime_error("repo-original-packet-spool-flush-failed");
  }
  m_finalized = true;
}

void
FaceDataReferencePacketSpool::append(std::size_t packetIndex,
                                     const std::vector<uint8_t>& wire)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (packetIndex >= m_packetLengths.size() || m_packetPresent[packetIndex] ||
      wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE ||
      m_totalWireBytes > m_maxWireBytes ||
      wire.size() > m_maxWireBytes - m_totalWireBytes ||
      wire.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("repo-original-packet-spool-packet-invalid");
  }
  // Sequential writes retain stdio batching. An update stream must position
  // between reading and writing, even when that read ended at physical EOF.
  if (m_readSinceAppend && std::fseek(m_file.get(), 0, SEEK_END) != 0) {
    throw std::runtime_error("repo-original-packet-spool-seek-failed");
  }
  const long offset = std::ftell(m_file.get());
  if (offset < 0 || std::fwrite(wire.data(), 1, wire.size(), m_file.get()) != wire.size()) {
    throw std::runtime_error("repo-original-packet-spool-write-failed");
  }
  m_readSinceAppend = false;
  m_packetOffsets[packetIndex] = offset;
  m_packetLengths[packetIndex] = static_cast<uint32_t>(wire.size());
  m_packetPresent[packetIndex] = true;
  m_totalWireBytes += wire.size();
}

std::vector<uint8_t>
FaceDataReferencePacketSpool::readPacket(std::size_t packetIndex) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (packetIndex >= m_packetLengths.size() || !m_packetPresent[packetIndex]) {
    throw std::out_of_range("repo-original-packet-spool-packet-unavailable");
  }
  const auto length = m_packetLengths[packetIndex];
  std::vector<uint8_t> wire(length);
  m_readSinceAppend = true;
  if (std::fseek(m_file.get(), m_packetOffsets[packetIndex], SEEK_SET) != 0 ||
      std::fread(wire.data(), 1, wire.size(), m_file.get()) != wire.size()) {
    throw std::runtime_error("repo-original-packet-spool-read-failed");
  }
  return wire;
}

std::vector<uint8_t>
FaceDataReferencePacketSpool::readRange(uint64_t offset, std::size_t length) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_finalized || offset > m_totalWireBytes ||
      length > m_totalWireBytes - offset) {
    throw std::out_of_range("repo-original-packet-spool-range-invalid");
  }
  std::vector<uint8_t> result(length);
  if (length == 0) {
    return result;
  }

  auto packet = std::upper_bound(m_logicalOffsets.begin(),
                                 m_logicalOffsets.end(), offset);
  std::size_t index = static_cast<std::size_t>(
    std::distance(m_logicalOffsets.begin(), packet) - 1);
  uint64_t cursor = offset;
  std::size_t copied = 0;
  while (copied < length) {
    if (index >= m_packetLengths.size()) {
      throw std::runtime_error("repo-original-packet-spool-index-invalid");
    }
    const auto packetBegin = m_logicalOffsets[index];
    const auto withinPacket = cursor - packetBegin;
    const auto available = static_cast<uint64_t>(m_packetLengths[index]) - withinPacket;
    const auto chunk = static_cast<std::size_t>(
      std::min<uint64_t>(available, length - copied));
    const auto physicalOffset = m_packetOffsets[index] + static_cast<long>(withinPacket);
    m_readSinceAppend = true;
    if (std::fseek(m_file.get(), physicalOffset, SEEK_SET) != 0 ||
        std::fread(result.data() + copied, 1, chunk, m_file.get()) != chunk) {
      throw std::runtime_error("repo-original-packet-spool-read-failed");
    }
    copied += chunk;
    cursor += chunk;
    ++index;
  }
  return result;
}

FaceDataReferenceFetcher::FaceDataReferenceFetcher(
  ndn::Face& face, std::chrono::milliseconds interestLifetime,
  std::chrono::milliseconds totalTimeout, std::size_t windowSize,
  std::shared_ptr<void> lifetime)
  : m_face(face)
  , m_interestLifetime(interestLifetime)
  , m_totalTimeout(totalTimeout)
  , m_windowSize(windowSize)
  , m_lifetime(std::move(lifetime))
{
  if (m_interestLifetime <= std::chrono::milliseconds::zero() ||
      m_totalTimeout <= std::chrono::milliseconds::zero() ||
      m_windowSize > MAX_ORIGINAL_DATA_PACKET_COUNT) {
    throw std::invalid_argument("repo-original-packet-fetch-options-invalid");
  }
}

std::shared_ptr<FaceDataReferencePacketSpool>
FaceDataReferenceFetcher::operator()(const RepoDataReference& reference,
  std::function<void()> requireCurrent) const
{
  reference.validateExactPacketSet();
  if (m_face.getIoContext().get_executor().running_in_this_thread()) {
    throw std::logic_error("repo-original-packet-fetch-on-face-thread");
  }
  if (reference.dataPrefix.empty() || reference.expectedSize == 0 ||
      reference.expectedSize > REPO_USER_STORAGE_OBJECT_MAX_BYTES) {
    throw std::invalid_argument("repo-original-packet-reference-invalid");
  }
  const ndn::Name prefix(reference.dataPrefix);
  if (prefix.empty()) {
    throw std::invalid_argument("repo-original-packet-prefix-invalid");
  }
  if (!reference.forwardingHint.empty() && ndn::Name(reference.forwardingHint).empty()) {
    throw std::invalid_argument("repo-original-packet-forwarding-hint-invalid");
  }

  uint64_t packetCount = 1;
  if (reference.packetSetMetadata) {
    packetCount = reference.packetSetMetadata->packetNames.size();
  }
  else if (reference.hasFinalSegment) {
    if (reference.finalSegment < reference.firstSegment) {
      throw std::invalid_argument("repo-original-packet-range-invalid");
    }
    const auto lastOffset = reference.finalSegment - reference.firstSegment;
    if (lastOffset == std::numeric_limits<uint64_t>::max()) {
      throw std::invalid_argument("repo-original-packet-range-overflow");
    }
    packetCount = lastOffset + 1;
  }
  if (packetCount > reference.expectedSize ||
      packetCount > MAX_ORIGINAL_DATA_PACKET_COUNT ||
      packetCount > std::numeric_limits<std::size_t>::max()) {
    throw std::length_error("repo-original-packet-count-unbounded");
  }

  return fetch(reference, static_cast<std::size_t>(packetCount), {}, true,
               std::move(requireCurrent));
}

std::shared_ptr<FaceDataReferencePacketSpool>
FaceDataReferenceFetcher::fetchNamed(const std::vector<ndn::Name>& names,
  uint64_t maxWireBytes, const ndn::Name& forwardingHint,
  std::function<void()> requireCurrent) const
{
  if (names.empty() || names.size() > MAX_NAMED_PACKET_COUNT || maxWireBytes < names.size() ||
      maxWireBytes > MAX_NAMED_PACKET_WIRE_BYTES || forwardingHint.empty() ||
      forwardingHint.wireEncode().size() > 4096)
    throw std::invalid_argument("repo-original-named-packet-budget-invalid");
  std::size_t nameBytes = 0;
  std::set<ndn::Name> unique;
  for (const auto& name : names) {
    const auto text = name.toUri(ndn::name::UriFormat::CANONICAL);
    if (name.empty() || text.size() > 4096 || !unique.insert(name).second ||
        text.size() > MAX_NAMED_PACKET_NAME_BYTES - nameBytes)
      throw std::invalid_argument("repo-original-named-packet-name-invalid");
    nameBytes += text.size();
  }
  RepoDataReference routing;
  routing.dataPrefix = names.front().toUri();
  routing.forwardingHint = forwardingHint.toUri();
  routing.expectedSize = maxWireBytes;
  return fetch(routing, names.size(), names, false, std::move(requireCurrent));
}

std::shared_ptr<FaceDataReferencePacketSpool>
FaceDataReferenceFetcher::fetch(const RepoDataReference& reference, std::size_t packetCount,
  std::vector<ndn::Name> names, bool exactSize, std::function<void()> requireCurrent) const
{
  if (m_face.getIoContext().get_executor().running_in_this_thread())
    throw std::logic_error("repo-original-packet-fetch-on-face-thread");
  if (requireCurrent)
    requireCurrent();

  const auto deadline = std::chrono::steady_clock::now() + m_totalTimeout;
  auto admission = std::make_shared<FetchAdmission>(m_fetchActive, m_lifetime);
  auto state = std::make_shared<FetchState>(m_face, reference,
    static_cast<std::size_t>(packetCount), m_windowSize, m_interestLifetime, deadline, admission);
  state->exactSize = exactSize;
  state->names = std::move(names);
  state->requireCurrent = std::move(requireCurrent);
  state->prepareSegmentedCandidate();
  boost::asio::post(state->strand, [state] { state->launchWindow(); });

  std::unique_lock<std::mutex> lock(state->mutex);
  while (!state->done) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      lock.unlock();
      boost::asio::post(state->strand, [state] {
        state->fail("repo-original-packet-fetch-deadline");
      });
      throw std::runtime_error("repo-original-packet-fetch-deadline");
    }
    const auto wakeAt = std::min(deadline, now + std::chrono::milliseconds(25));
    if (state->condition.wait_until(lock, wakeAt, [&] { return state->done; }))
      break;

    // Runtime close/cancellation must wake a quiet Face wait without waiting for
    // the network deadline. Keep the admission lease in FetchState until the
    // posted cancellation drains the outstanding Face callbacks.
    lock.unlock();
    bool current = true;
    try {
      if (state->requireCurrent)
        state->requireCurrent();
    }
    catch (...) {
      current = false;
    }
    if (!current) {
      const auto error = std::chrono::steady_clock::now() >= deadline
        ? "repo-original-packet-fetch-deadline" : "repo-original-packet-fetch-cancelled";
      boost::asio::post(state->strand, [state, error] { state->fail(error); });
      throw std::runtime_error(error);
    }
    lock.lock();
  }
  if (!state->error.empty()) {
    const auto error = state->error;
    state->spool.reset();
    lock.unlock();
    throw std::runtime_error(error);
  }
  auto spool = std::move(state->spool);
  lock.unlock();
  spool->finalize();
  if (state->requireCurrent)
    state->requireCurrent();
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error("repo-original-packet-fetch-deadline");
  }
  // Named reads can retain an Actor owner in the lease. Keep it until any queued callbacks drain.
  if (state->exactSize)
    state->admission.reset();
  return spool;
}

} // namespace ndnsf_distributed_repo::detail
