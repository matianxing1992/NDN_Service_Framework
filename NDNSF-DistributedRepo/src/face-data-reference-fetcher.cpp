#include "face-data-reference-fetcher.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ndnsf_distributed_repo::detail {
namespace {

constexpr std::size_t MAX_ORIGINAL_DATA_PACKET_COUNT = 1000000;

std::size_t
checkedPacketCount(std::size_t count, uint64_t bytes)
{
  if (count == 0 || count > MAX_ORIGINAL_DATA_PACKET_COUNT || count > bytes ||
      bytes == 0 || bytes > REPO_USER_STORAGE_LIMIT_BYTES) {
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
    , windowSize(windowSizeArg)
    , interestLifetime(interestLifetimeArg)
    , deadline(deadlineArg)
    , admission(std::move(admissionArg))
    , spool(std::make_shared<FaceDataReferencePacketSpool>(
        packetCountArg, referenceArg.expectedSize))
  {}

  void
  launchWindow()
  {
    while (true) {
      std::size_t index = 0;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done) {
          return;
        }
        if (nextPacket == packetCount) {
          if (inFlight == 0 && received == packetCount) {
            if (std::chrono::steady_clock::now() >= deadline) {
              failLocked("repo-original-packet-fetch-deadline");
            }
            else if (receivedWireBytes != reference.expectedSize) {
              failLocked("repo-original-packet-size-mismatch");
            }
            else {
              done = true;
              condition.notify_all();
            }
          }
          return;
        }
        if (inFlight >= windowSize) {
          return;
        }
        index = nextPacket++;
        ++inFlight;
      }

      expressPacket(index, 1);
    }
  }

  void
  expressPacket(std::size_t index, unsigned attempt)
  {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      fail("repo-original-packet-fetch-deadline");
      return;
    }
    ndn::Interest interest(expectedPacketName(reference, index));
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    interest.setInterestLifetime(ndn::time::milliseconds(
      std::min({interestLifetime, remaining, std::chrono::milliseconds(1000)}).count()));
    if (!reference.forwardingHint.empty()) {
      interest.setForwardingHint({ndn::Name(reference.forwardingHint)});
    }

    const auto self = shared_from_this();
    try {
      std::lock_guard<std::mutex> lock(mutex);
      if (done) {
        return;
      }
      auto pending = pendingInterests.emplace(
        index, PendingPacket{}).first;
      pending->second.attempt = attempt;
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
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto pending = pendingInterests.find(index);
      if (done || pending == pendingInterests.end() || pending->second.attempt != attempt) {
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        failLocked("repo-original-packet-fetch-deadline");
        return;
      }
    }
    std::vector<uint8_t> wire;
    try {
      if (data.getName() != expectedPacketName(reference, index)) {
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

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done) {
        return;
      }
      const auto pending = pendingInterests.find(index);
      if (pending == pendingInterests.end() || pending->second.attempt != attempt) {
        return;
      }
      pendingInterests.erase(pending);
      if (inFlight > 0) {
        --inFlight;
      }
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
    launchWindow();
  }

  void
  onFailure(std::size_t index, unsigned attempt, const std::string& message)
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto pending = pendingInterests.find(index);
      if (done || pending == pendingInterests.end() || pending->second.attempt != attempt)
        return;
      constexpr unsigned MAX_ATTEMPTS = 3;
      if (attempt >= MAX_ATTEMPTS || std::chrono::steady_clock::now() >= deadline ||
          (message != "repo-original-packet-timeout" && message != "repo-original-packet-nack")) {
        failLocked(message);
        return;
      }
      pending->second.handle.cancel();
      pendingInterests.erase(pending);
    }
    // Retransmission keeps the same immutable slot and window charge. A new
    // Interest gets a fresh Nonce; stale callbacks cannot consume its attempt.
    expressPacket(index, attempt + 1);
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
    done = true;
    error = message;
    for (auto& [index, pending] : pendingInterests) {
      (void)index;
      pending.handle.cancel();
    }
    pendingInterests.clear();
    spool.reset();
    condition.notify_all();
  }

  ndn::Face& face;
  boost::asio::strand<boost::asio::io_context::executor_type> strand;
  const RepoDataReference reference;
  const std::size_t packetCount;
  const std::size_t windowSize;
  const std::chrono::milliseconds interestLifetime;
  const std::chrono::steady_clock::time_point deadline;
  std::shared_ptr<FetchAdmission> admission;
  std::mutex mutex;
  std::condition_variable condition;
  std::shared_ptr<FaceDataReferencePacketSpool> spool;
  struct PendingPacket
  {
    ndn::PendingInterestHandle handle;
    unsigned attempt = 0;
  };
  std::map<std::size_t, PendingPacket> pendingInterests;
  std::size_t nextPacket = 0;
  std::size_t inFlight = 0;
  std::size_t received = 0;
  uint64_t receivedWireBytes = 0;
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
  if (std::fseek(m_file.get(), 0, SEEK_END) != 0) {
    throw std::runtime_error("repo-original-packet-spool-seek-failed");
  }
  const long offset = std::ftell(m_file.get());
  if (offset < 0 || std::fwrite(wire.data(), 1, wire.size(), m_file.get()) != wire.size()) {
    throw std::runtime_error("repo-original-packet-spool-write-failed");
  }
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
      m_windowSize == 0 || m_windowSize > 256) {
    throw std::invalid_argument("repo-original-packet-fetch-options-invalid");
  }
}

std::shared_ptr<FaceDataReferencePacketSpool>
FaceDataReferenceFetcher::operator()(const RepoDataReference& reference) const
{
  reference.validateExactPacketSet();
  if (m_face.getIoContext().get_executor().running_in_this_thread()) {
    throw std::logic_error("repo-original-packet-fetch-on-face-thread");
  }
  if (reference.dataPrefix.empty() || reference.expectedSize == 0 ||
      reference.expectedSize > REPO_USER_STORAGE_LIMIT_BYTES) {
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

  const auto deadline = std::chrono::steady_clock::now() + m_totalTimeout;
  auto admission = std::make_shared<FetchAdmission>(m_fetchActive, m_lifetime);
  auto state = std::make_shared<FetchState>(m_face, reference,
    static_cast<std::size_t>(packetCount), m_windowSize, m_interestLifetime, deadline, admission);
  boost::asio::post(state->strand, [state] { state->launchWindow(); });

  std::unique_lock<std::mutex> lock(state->mutex);
  if (!state->condition.wait_until(lock, deadline, [&] { return state->done; })) {
    lock.unlock();
    boost::asio::post(state->strand, [state] {
      state->fail("repo-original-packet-fetch-deadline");
    });
    throw std::runtime_error("repo-original-packet-fetch-deadline");
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
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error("repo-original-packet-fetch-deadline");
  }
  state->admission.reset();
  return spool;
}

} // namespace ndnsf_distributed_repo::detail
