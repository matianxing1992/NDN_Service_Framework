#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/validator-null.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/segmenter.hpp>

#include <boost/asio/post.hpp>

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace py = pybind11;
namespace nsf = ndn_service_framework;

namespace {

std::mutex g_keyChainMutex;

using PyFunctionPtr = std::shared_ptr<py::function>;

int
envIntValue(const char* name, int defaultValue, int minValue, int maxValue)
{
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return defaultValue;
  }
  try {
    int parsed = std::stoi(value);
    parsed = std::max(minValue, parsed);
    parsed = std::min(maxValue, parsed);
    return parsed;
  }
  catch (...) {
    return defaultValue;
  }
}

ndn::time::milliseconds
pythonFacePollTimeout()
{
  static const int pollMs =
    envIntValue("NDNSF_PY_FACE_POLL_MS", 1, 1, 100);
  return ndn::time::milliseconds(pollMs);
}

PyFunctionPtr
keepPyFunction(py::function fn)
{
  return PyFunctionPtr(new py::function(std::move(fn)), [](py::function* value) {
    py::gil_scoped_acquire gil;
    delete value;
  });
}

void
processFaceEvents(ndn::Face& face, ndn::time::milliseconds timeout)
{
  // ndn-cxx stops the io_context when processEvents(timeout) returns by
  // timeout. Python roles pump the Face repeatedly, so restart before each
  // bounded pump to keep later Interests/Data moving.
  face.getIoContext().restart();
  face.processEvents(timeout);
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  std::lock_guard<std::mutex> lock(g_keyChainMutex);
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

ndn::Buffer
toBuffer(const py::bytes& value)
{
  const std::string bytes = value;
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

py::bytes
toPyBytes(const ndn::Buffer& value)
{
  return py::bytes(reinterpret_cast<const char*>(value.data()), value.size());
}

py::dict
largeDataReferenceToDict(const nsf::LargeDataReference& reference)
{
  py::dict output;
  output["data_name"] = reference.dataName.toUri();
  output["object_type"] = reference.objectType;
  output["object_id"] = reference.objectId;
  output["plaintext_size"] = reference.plaintextSize;
  output["encrypted"] = reference.encrypted;
  output["digest"] = reference.digest;
  return output;
}

std::shared_ptr<const nsf::AckSelectionPolicy>
selectionPolicyByName(const std::string& strategy)
{
  if (strategy == "all-selected" || strategy == "all-responders") {
    return nsf::strategy::AllSelected;
  }
  if (strategy == "random-selection" || strategy == "load-balancing") {
    return nsf::strategy::RandomSelection;
  }
  return nsf::strategy::FirstResponding;
}

struct PyServiceResponse
{
  bool status = false;
  py::bytes payload;
  std::string error;
};

struct PyAckDecision
{
  bool status = true;
  py::bytes payload;
  std::string message = "ok";
  bool suppress = false;
};

struct PyAckCandidate
{
  std::string providerName;
  std::string serviceName;
  std::string requestId;
  bool status = false;
  std::string message;
  py::bytes payload;
};

struct PyLargeDataPublishResult
{
  bool success = false;
  std::string encryptedDataName;
  std::string objectId;
  std::string error;
};

struct PyDataPacket
{
  std::string name;
  uint64_t segment = 0;
  py::bytes wire;
};

struct PySegmentHintRange
{
  uint64_t start = 0;
  uint64_t end = 0;
  std::vector<std::string> forwardingHints;
};

PyDataPacket
toPyDataPacket(const ndn::Data& data)
{
  const auto wire = data.wireEncode();
  PyDataPacket packet;
  packet.name = data.getName().toUri();
  if (!data.getName().empty() && data.getName()[-1].isSegment()) {
    packet.segment = data.getName()[-1].toSegment();
  }
  packet.wire = py::bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
  return packet;
}

std::shared_ptr<ndn::Data>
dataFromWireBytes(const py::bytes& wireBytes)
{
  const std::string bytes = wireBytes;
  ndn::Block wire(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  wire.parse();
  return std::make_shared<ndn::Data>(wire);
}

class NativeSegmentedObjectProducer
{
public:
  NativeSegmentedObjectProducer(const std::string& baseName,
                                const py::bytes& payload,
                                const std::string& signingIdentity,
                                size_t maxSegmentSize,
                                int freshnessMs)
    : m_baseName(baseName)
  {
    const auto identityName = signingIdentity.empty() ?
      ndn::Name("/ndnsf/python/segmented-producer") : ndn::Name(signingIdentity);
    getOrCreateIdentity(m_keyChain, identityName);
    m_signingIdentity = identityName;

    m_versionedName = m_baseName;
    m_versionedName.appendVersion(static_cast<uint64_t>(
      ndn::time::toUnixTimestamp(ndn::time::system_clock::now()).count()));

    const std::string bytes = payload;
    ndn::Segmenter segmenter(
      m_keyChain,
      ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                 identityName));
    m_segments = segmenter.segment(
      ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()),
                               bytes.size()),
      m_versionedName,
      maxSegmentSize,
      ndn::time::milliseconds(freshnessMs));
  }

  ~NativeSegmentedObjectProducer()
  {
    stop();
  }

  std::string
  baseName() const
  {
    return m_baseName.toUri();
  }

  std::string
  versionedName() const
  {
    return m_versionedName.toUri();
  }

  size_t
  segmentCount() const
  {
    return m_segments.size();
  }

  void
  start()
  {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
      return;
    }

    m_face.setInterestFilter(
      m_baseName,
      [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        this->serveInterest(interest);
      },
      [] (const ndn::Name&) {},
      [] (const ndn::Name&, const std::string&) {},
      ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                 m_signingIdentity));

    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, ndn::time::milliseconds(50));
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = e.what();
        }
      }
    });
  }

  void
  stop()
  {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) {
      return;
    }
    try {
      m_face.getIoContext().stop();
    }
    catch (const std::exception&) {
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  std::string
  error() const
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
  }

private:
  void
  serveInterest(const ndn::Interest& interest)
  {
    if (m_segments.empty()) {
      return;
    }

    uint64_t segmentNo = 0;
    const auto& name = interest.getName();
    if (!name.empty() && name[-1].isSegment()) {
      segmentNo = name[-1].toSegment();
    }

    if (segmentNo >= m_segments.size()) {
      return;
    }

    m_face.put(*m_segments[segmentNo]);
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_baseName;
  ndn::Name m_versionedName;
  ndn::Name m_signingIdentity;
  std::vector<std::shared_ptr<ndn::Data>> m_segments;
  std::atomic_bool m_running{false};
  std::thread m_thread;
  mutable std::mutex m_errorMutex;
  std::string m_error;
};

class NativeWireDataProducer
{
public:
  NativeWireDataProducer(const std::string& baseName,
                         const std::vector<py::bytes>& packetWires,
                         const std::string& signingIdentity)
    : m_baseName(baseName)
  {
    m_signingIdentity = signingIdentity.empty() ?
      ndn::Name("/ndnsf/python/stored-data-producer") : ndn::Name(signingIdentity);
    getOrCreateIdentity(m_keyChain, m_signingIdentity);
    for (const auto& packetWire : packetWires) {
      auto data = dataFromWireBytes(packetWire);
      if (!data->getName().empty() && data->getName()[-1].isSegment()) {
        m_segments[data->getName()[-1].toSegment()] = data;
      }
      else {
        m_segments[0] = data;
      }
    }
  }

  ~NativeWireDataProducer()
  {
    stop();
  }

  void
  start()
  {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
      return;
    }
    m_face.setInterestFilter(
      m_baseName,
      [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        this->serveInterest(interest);
      },
      [] (const ndn::Name&) {},
      [] (const ndn::Name&, const std::string&) {},
      ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                 m_signingIdentity));
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, ndn::time::milliseconds(50));
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = e.what();
        }
      }
    });
  }

  void
  stop()
  {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) {
      return;
    }
    try {
      m_face.getIoContext().stop();
    }
    catch (const std::exception&) {
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  size_t
  segmentCount() const
  {
    return m_segments.size();
  }

  std::string
  error() const
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
  }

private:
  void
  serveInterest(const ndn::Interest& interest)
  {
    if (m_segments.empty()) {
      return;
    }
    uint64_t segmentNo = 0;
    const auto& name = interest.getName();
    if (!name.empty() && name[-1].isSegment()) {
      segmentNo = name[-1].toSegment();
    }
    auto it = m_segments.find(segmentNo);
    if (it == m_segments.end()) {
      return;
    }
    m_face.put(*it->second);
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_baseName;
  ndn::Name m_signingIdentity;
  std::map<uint64_t, std::shared_ptr<ndn::Data>> m_segments;
  std::atomic_bool m_running{false};
  std::thread m_thread;
  mutable std::mutex m_errorMutex;
  std::string m_error;
};

std::vector<PyDataPacket>
makeSegmentedDataPackets(const std::string& baseName,
                         const py::bytes& payload,
                         const std::string& signingIdentity,
                         size_t maxSegmentSize,
                         int freshnessMs)
{
  ndn::KeyChain keyChain;
  const auto identityName = signingIdentity.empty() ?
    ndn::Name("/ndnsf/python/segmented-packets") : ndn::Name(signingIdentity);
  getOrCreateIdentity(keyChain, identityName);

  ndn::Name versionedName(baseName);
  versionedName.appendVersion(static_cast<uint64_t>(
    ndn::time::toUnixTimestamp(ndn::time::system_clock::now()).count()));
  const std::string bytes = payload;
  ndn::Segmenter segmenter(
    keyChain,
    ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                               identityName));
  auto segments = segmenter.segment(
    ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()),
                             bytes.size()),
    versionedName,
    maxSegmentSize,
    ndn::time::milliseconds(freshnessMs));

  std::vector<PyDataPacket> output;
  output.reserve(segments.size());
  for (const auto& segment : segments) {
    output.push_back(toPyDataPacket(*segment));
  }
  return output;
}

PyDataPacket
fetchOneDataPacket(ndn::Face& face,
                   const ndn::Interest& interest,
                   ndn::time::steady_clock::time_point deadline)
{
  std::mutex mutex;
  bool done = false;
  std::optional<PyDataPacket> packet;
  std::string error;

  face.expressInterest(
    interest,
    [&] (const ndn::Interest&, const ndn::Data& data) {
      std::lock_guard<std::mutex> lock(mutex);
      packet = toPyDataPacket(data);
      done = true;
    },
    [&] (const ndn::Interest&, const ndn::lp::Nack& nack) {
      std::lock_guard<std::mutex> lock(mutex);
      error = "Nack: " + std::to_string(static_cast<int>(nack.getReason()));
      done = true;
    },
    [&] (const ndn::Interest&) {
      std::lock_guard<std::mutex> lock(mutex);
      error = "timeout";
      done = true;
    });

  while (ndn::time::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done) {
        break;
      }
    }
    processFaceEvents(face, ndn::time::milliseconds(20));
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (!done) {
    throw std::runtime_error("Data packet fetch timed out: " + interest.getName().toUri());
  }
  if (!error.empty()) {
    throw std::runtime_error("Data packet fetch failed for " + interest.getName().toUri() + ": " + error);
  }
  if (!packet) {
    throw std::runtime_error("Data packet fetch returned no packet: " + interest.getName().toUri());
  }
  return *packet;
}

std::vector<ndn::Name>
hintsForSegment(uint64_t segmentNo, const std::vector<PySegmentHintRange>& ranges)
{
  std::vector<ndn::Name> hints;
  for (const auto& range : ranges) {
    if (segmentNo < range.start || segmentNo > range.end) {
      continue;
    }
    hints.reserve(range.forwardingHints.size());
    for (const auto& hint : range.forwardingHints) {
      hints.emplace_back(hint);
    }
    break;
  }
  return hints;
}

void
applyForwardingHints(ndn::Interest& interest, const std::vector<ndn::Name>& hints)
{
  if (!hints.empty()) {
    interest.setForwardingHint(hints);
  }
}

PyDataPacket
fetchOneDataPacketWithHintFallback(ndn::Face& face,
                                   const ndn::Name& name,
                                   bool canBePrefix,
                                   ndn::time::milliseconds interestLifetime,
                                   ndn::time::steady_clock::time_point deadline,
                                   const std::vector<ndn::Name>& hints)
{
  std::vector<std::vector<ndn::Name>> attempts;
  if (hints.empty()) {
    attempts.emplace_back();
  }
  else {
    attempts.reserve(hints.size() + 1);
    for (const auto& hint : hints) {
      attempts.push_back({hint});
    }
    attempts.emplace_back();
  }

  std::string lastError;
  for (const auto& attemptHints : attempts) {
    ndn::Interest interest(name);
    interest.setCanBePrefix(canBePrefix);
    interest.setMustBeFresh(false);
    interest.setInterestLifetime(interestLifetime);
    applyForwardingHints(interest, attemptHints);
    try {
      return fetchOneDataPacket(face, interest, deadline);
    }
    catch (const std::exception& e) {
      lastError = e.what();
      if (ndn::time::steady_clock::now() >= deadline) {
        break;
      }
    }
  }
  throw std::runtime_error("Data packet fetch failed after hint fallback for " +
                           name.toUri() + ": " + lastError);
}

std::vector<PyDataPacket>
fetchSegmentedDataPackets(const std::string& baseName,
                          int timeoutMs,
                          int interestLifetimeMs,
                          const std::vector<std::string>& forwardingHints)
{
  ndn::Face face;
  const auto deadline = ndn::time::steady_clock::now() + ndn::time::milliseconds(timeoutMs);

  ndn::Interest firstInterest{ndn::Name(baseName)};
  firstInterest.setCanBePrefix(true);
  firstInterest.setMustBeFresh(false);
  firstInterest.setInterestLifetime(ndn::time::milliseconds(interestLifetimeMs));
  std::vector<ndn::Name> hintNames;
  hintNames.reserve(forwardingHints.size());
  for (const auto& hint : forwardingHints) {
    hintNames.emplace_back(hint);
  }
  if (!hintNames.empty()) {
    firstInterest.setForwardingHint(hintNames);
  }
  auto first = fetchOneDataPacket(face, firstInterest, deadline);

  auto firstData = dataFromWireBytes(first.wire);
  auto finalBlock = firstData->getFinalBlock();
  if (!finalBlock || !finalBlock->isSegment()) {
    throw std::runtime_error("First segmented Data has no segment FinalBlockId: " + first.name);
  }
  const auto finalSegment = finalBlock->toSegment();
  const auto versionedName = firstData->getName().getPrefix(-1);

  std::vector<PyDataPacket> packets(finalSegment + 1);
  if (first.segment > finalSegment) {
    throw std::runtime_error("First segment number exceeds FinalBlockId");
  }
  packets[first.segment] = first;

  for (uint64_t segmentNo = 0; segmentNo <= finalSegment; ++segmentNo) {
    if (segmentNo == first.segment) {
      continue;
    }
    ndn::Name segmentName(versionedName);
    segmentName.appendSegment(segmentNo);
    ndn::Interest interest(segmentName);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    interest.setInterestLifetime(ndn::time::milliseconds(interestLifetimeMs));
    if (!hintNames.empty()) {
      interest.setForwardingHint(hintNames);
    }
    packets[segmentNo] = fetchOneDataPacket(face, interest, deadline);
  }
  return packets;
}

py::bytes
fetchSegmentedObjectWithSegmentHints(const std::string& baseName,
                                     int timeoutMs,
                                     int interestLifetimeMs,
                                     const std::vector<PySegmentHintRange>& hintRanges)
{
  ndn::Face face;
  const auto deadline = ndn::time::steady_clock::now() + ndn::time::milliseconds(timeoutMs);

  const auto interestLifetime = ndn::time::milliseconds(interestLifetimeMs);
  auto first = fetchOneDataPacketWithHintFallback(face,
                                                  ndn::Name(baseName),
                                                  true,
                                                  interestLifetime,
                                                  deadline,
                                                  hintsForSegment(0, hintRanges));

  auto firstData = dataFromWireBytes(first.wire);
  auto finalBlock = firstData->getFinalBlock();
  if (!finalBlock || !finalBlock->isSegment()) {
    throw std::runtime_error("First segmented Data has no segment FinalBlockId: " + first.name);
  }
  const auto finalSegment = finalBlock->toSegment();
  const auto versionedName = firstData->getName().getPrefix(-1);

  std::vector<PyDataPacket> packets(finalSegment + 1);
  if (first.segment > finalSegment) {
    throw std::runtime_error("First segment number exceeds FinalBlockId");
  }
  packets[first.segment] = first;

  for (uint64_t segmentNo = 0; segmentNo <= finalSegment; ++segmentNo) {
    if (segmentNo == first.segment) {
      continue;
    }
    ndn::Name segmentName(versionedName);
    segmentName.appendSegment(segmentNo);
    packets[segmentNo] = fetchOneDataPacketWithHintFallback(face,
                                                            segmentName,
                                                            false,
                                                            interestLifetime,
                                                            deadline,
                                                            hintsForSegment(segmentNo, hintRanges));
  }

  std::string output;
  for (const auto& packet : packets) {
    auto data = dataFromWireBytes(packet.wire);
    const auto& content = data->getContent();
    output.append(reinterpret_cast<const char*>(content.value()), content.value_size());
  }
  return py::bytes(output);
}

py::bytes
fetchKnownSegmentedObjectWithSegmentHints(const std::string& versionedName,
                                          uint64_t segmentCount,
                                          int timeoutMs,
                                          int interestLifetimeMs,
                                          const std::vector<PySegmentHintRange>& hintRanges)
{
  if (segmentCount == 0) {
    return py::bytes();
  }
  ndn::Face face;
  const auto deadline = ndn::time::steady_clock::now() + ndn::time::milliseconds(timeoutMs);
  const auto interestLifetime = ndn::time::milliseconds(interestLifetimeMs);

  std::vector<PyDataPacket> packets(segmentCount);
  for (uint64_t segmentNo = 0; segmentNo < segmentCount; ++segmentNo) {
    ndn::Name segmentName(versionedName);
    segmentName.appendSegment(segmentNo);
    packets[segmentNo] = fetchOneDataPacketWithHintFallback(face,
                                                            segmentName,
                                                            false,
                                                            interestLifetime,
                                                            deadline,
                                                            hintsForSegment(segmentNo, hintRanges));
  }

  std::string output;
  for (const auto& packet : packets) {
    auto data = dataFromWireBytes(packet.wire);
    const auto& content = data->getContent();
    output.append(reinterpret_cast<const char*>(content.value()), content.value_size());
  }
  return py::bytes(output);
}

py::bytes
fetchSegmentedObject(const std::string& baseName,
                     int timeoutMs,
                     int interestLifetimeMs,
                     double initCwnd,
                     const std::vector<std::string>& forwardingHints)
{
  ndn::Face face;
  ndn::security::ValidatorNull validator;
  ndn::SegmentFetcher::Options options;
  options.maxTimeout = ndn::time::milliseconds(timeoutMs);
  options.interestLifetime = ndn::time::milliseconds(interestLifetimeMs);
  options.initCwnd = initCwnd;

  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  ndn::ConstBufferPtr result;
  std::string error;

  ndn::Interest interest{ndn::Name(baseName)};
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(false);
  interest.setInterestLifetime(ndn::time::milliseconds(interestLifetimeMs));
  std::vector<ndn::Name> hintNames;
  hintNames.reserve(forwardingHints.size());
  for (const auto& hint : forwardingHints) {
    hintNames.emplace_back(hint);
  }
  if (!hintNames.empty()) {
    interest.setForwardingHint(hintNames);
  }

  auto fetcher = ndn::SegmentFetcher::start(face, interest, validator, options);
  fetcher->onComplete.connect([&] (ndn::ConstBufferPtr payload) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      result = std::move(payload);
      done = true;
    }
    cv.notify_one();
  });
  fetcher->onError.connect([&] (uint32_t code, const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      error = std::to_string(code) + ": " + message;
      done = true;
    }
    cv.notify_one();
  });

  const auto deadline = ndn::time::steady_clock::now() + ndn::time::milliseconds(timeoutMs);
  while (ndn::time::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done) {
        break;
      }
    }
    processFaceEvents(face, ndn::time::milliseconds(20));
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!done) {
      fetcher->stop();
      throw std::runtime_error("segmented object fetch timed out: " + baseName);
    }
    if (!error.empty()) {
      throw std::runtime_error("segmented object fetch failed for " + baseName + ": " + error);
    }
    if (!result) {
      throw std::runtime_error("segmented object fetch completed without payload: " + baseName);
    }
    return py::bytes(reinterpret_cast<const char*>(result->data()), result->size());
  }
}

struct PyCollaborationAssignment
{
  std::string role;
  std::string service;
  std::string assignedArtifact;
  std::string artifactDataName;
  bool requiresProvisioning = false;
  int provisioningTimeoutMs = 0;
  py::bytes assignmentPayload;
};

struct PyCollaborationData
{
  std::string sessionId;
  std::string keyScope;
  std::string topic;
  std::string producer;
  std::string producerRole;
  uint64_t sequence = 0;
  py::bytes payload;
};

std::string
bytesToString(const ndn::Buffer& value)
{
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string
fieldFromText(const std::string& text, const std::string& key)
{
  const auto marker = key + "=";
  const auto begin = text.find(marker);
  if (begin == std::string::npos) {
    return "";
  }
  const auto valueBegin = begin + marker.size();
  const auto valueEnd = text.find(';', valueBegin);
  return text.substr(valueBegin,
                     (valueEnd == std::string::npos ? text.size() : valueEnd) -
                       valueBegin);
}

std::vector<std::string>
splitTextList(const std::string& text)
{
  std::vector<std::string> values;
  size_t begin = 0;
  while (begin <= text.size()) {
    const auto end = text.find(',', begin);
    auto value = text.substr(begin,
                             (end == std::string::npos ? text.size() : end) - begin);
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) {
      values.push_back(value.substr(first, last - first + 1));
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return values;
}

std::vector<std::string>
rolesFromAckPayload(const ndn::Buffer& payload)
{
  const auto text = bytesToString(payload);
  auto roles = splitTextList(fieldFromText(text, "roles"));
  if (!roles.empty()) {
    return roles;
  }
  auto role = fieldFromText(text, "role");
  if (!role.empty()) {
    roles.push_back(role);
  }
  return roles;
}

PyCollaborationData
toPyCollaborationData(const nsf::ServiceProvider::CollaborationData& data)
{
  PyCollaborationData output;
  output.sessionId = data.sessionId;
  output.keyScope = data.keyScope;
  output.topic = data.topic.toUri();
  output.producer = data.producer.toUri();
  output.producerRole = data.producerRole;
  output.sequence = data.sequence;
  output.payload = toPyBytes(data.payload);
  return output;
}

class RoleAssignmentSelectionPolicy final : public nsf::ParticipantSelectionPolicy
{
public:
  RoleAssignmentSelectionPolicy(std::map<std::string, ndn::Name> artifactDataNames,
                                std::map<std::string, ndn::Name> scopeKeyDataNames,
                                std::map<std::string, std::vector<std::string>> roleScopes)
    : m_artifactDataNames(std::move(artifactDataNames))
    , m_scopeKeyDataNames(std::move(scopeKeyDataNames))
    , m_roleScopes(std::move(roleScopes))
  {
  }

  std::vector<nsf::SelectedParticipant>
  select(const std::vector<nsf::AckCandidate>& candidates,
         const std::vector<nsf::CollaborationRoleSpec>& roles) const override
  {
    std::vector<nsf::SelectedParticipant> selected;
    std::map<std::string, std::vector<nsf::AckCandidate>> candidatesByRole;

    for (const auto& candidate : candidates) {
      if (!candidate.ack.getStatus()) {
        continue;
      }
      for (const auto& role : rolesFromAckPayload(candidate.ack.getPayload())) {
        candidatesByRole[role].push_back(candidate);
      }
    }

    std::map<std::string, size_t> providerAssignments;
    for (const auto& role : roles) {
      auto candidatesForRole = candidatesByRole.find(role.role);
      if (candidatesForRole == candidatesByRole.end() ||
          candidatesForRole->second.empty()) {
        continue;
      }
      auto best = candidatesForRole->second.begin();
      for (auto it = candidatesForRole->second.begin();
           it != candidatesForRole->second.end(); ++it) {
        const auto bestCount = providerAssignments[best->providerName.toUri()];
        const auto thisCount = providerAssignments[it->providerName.toUri()];
        if (thisCount < bestCount) {
          best = it;
        }
      }
      providerAssignments[best->providerName.toUri()]++;

      std::string assignment =
        "role=" + role.role +
        ";artifact=" + role.requiredArtifact.toUri() +
        ";requiresProvisioning=" +
        (role.allowDynamicProvisioning ? "1" : "0") +
        ";provisioningTimeoutMs=" + std::to_string(role.provisioningTimeoutMs) + ";";

      auto artifactData = m_artifactDataNames.find(role.role);
      if (artifactData != m_artifactDataNames.end()) {
        assignment += "artifactDataName=" + artifactData->second.toUri() + ";";
      }

      auto scopes = m_roleScopes.find(role.role);
      if (scopes != m_roleScopes.end()) {
        for (const auto& scopeName : scopes->second) {
          auto scopeKeyData = m_scopeKeyDataNames.find(scopeName);
          if (scopeKeyData != m_scopeKeyDataNames.end()) {
            assignment += "scopeKeyData." + scopeName + "=" +
                          scopeKeyData->second.toUri() + ";";
          }
        }
      }

      ndn::Buffer assignmentPayload(reinterpret_cast<const uint8_t*>(assignment.data()),
                                    assignment.size());
      selected.push_back({role.role,
                          best->serviceName,
                          best->providerName,
                          role.requiredArtifact,
                          role.allowDynamicProvisioning,
                          role.provisioningTimeoutMs,
                          std::move(assignmentPayload),
                          *best});
    }
    std::string providerMap;
    for (const auto& participant : selected) {
      providerMap += "roleProvider." + participant.role + "=" +
                     participant.provider.toUri() + ";";
    }
    if (!providerMap.empty()) {
      for (auto& participant : selected) {
        std::string assignment(
          reinterpret_cast<const char*>(participant.assignmentPayload.data()),
          participant.assignmentPayload.size());
        assignment += providerMap;
        participant.assignmentPayload =
          ndn::Buffer(reinterpret_cast<const uint8_t*>(assignment.data()),
                      assignment.size());
      }
    }
    return selected;
  }

private:
  std::map<std::string, ndn::Name> m_artifactDataNames;
  std::map<std::string, ndn::Name> m_scopeKeyDataNames;
  std::map<std::string, std::vector<std::string>> m_roleScopes;
};

class PyCollaborationContext
{
public:
  explicit PyCollaborationContext(nsf::ServiceProvider::CollaborationContext& ctx)
    : m_ctx(&ctx)
  {
  }

  std::string sessionId() const
  {
    return m_ctx->sessionId();
  }

  std::string role() const
  {
    return m_ctx->role();
  }

  std::string localProvider() const
  {
    return m_ctx->localProvider().toUri();
  }

  PyCollaborationAssignment assignment() const
  {
    const auto& native = m_ctx->assignment();
    PyCollaborationAssignment assignment;
    assignment.role = native.role;
    assignment.service = native.service.toUri();
    assignment.assignedArtifact = native.assignedArtifact.toUri();
    assignment.artifactDataName = native.artifactDataName.toUri();
    assignment.requiresProvisioning = native.requiresProvisioning;
    assignment.provisioningTimeoutMs = native.provisioningTimeoutMs;
    assignment.assignmentPayload = toPyBytes(native.assignmentPayload);
    return assignment;
  }

  bool fetchArtifact(const std::string& artifactName, int timeoutMs)
  {
    return m_ctx->fetchArtifact(ndn::Name(artifactName), timeoutMs);
  }

  std::optional<py::bytes> getArtifact(const std::string& artifactName) const
  {
    auto artifact = m_ctx->getArtifact(ndn::Name(artifactName));
    if (!artifact) {
      return std::nullopt;
    }
    return toPyBytes(*artifact);
  }

  std::optional<py::bytes> fetchEncryptedLargeData(const std::string& dataName,
                                                   const std::string& serviceName)
  {
    auto result = m_ctx->fetchEncryptedLargeData(
      ndn::Name(dataName),
      serviceName.empty() ? ndn::Name() : ndn::Name(serviceName));
    if (!result) {
      return std::nullopt;
    }
    return toPyBytes(*result);
  }

  void fail(const std::string& reason)
  {
    m_ctx->fail(reason);
  }

  void publish(const std::string& keyScope,
               const std::string& topic,
               const py::bytes& payload)
  {
    m_ctx->publish(keyScope, ndn::Name(topic), toBuffer(payload));
  }

  std::string publishLarge(const std::string& keyScope,
                           const std::string& topic,
                           const py::bytes& payload,
                           size_t maxSegmentSize,
                           int freshnessMs)
  {
    return m_ctx->publishLarge(keyScope,
                               ndn::Name(topic),
                               toBuffer(payload),
                               maxSegmentSize,
                               freshnessMs).toUri();
  }

  std::string publishLargeNamed(const std::string& keyScope,
                                const std::string& dataName,
                                const py::bytes& payload,
                                size_t maxSegmentSize,
                                int freshnessMs)
  {
    return m_ctx->publishLargeNamed(keyScope,
                                    ndn::Name(dataName),
                                    toBuffer(payload),
                                    maxSegmentSize,
                                    freshnessMs).toUri();
  }

  std::optional<py::bytes> fetchLarge(const std::string& dataName,
                                      const std::string& keyScope,
                                      int timeoutMs)
  {
    auto payload = m_ctx->fetchLarge(ndn::Name(dataName), keyScope, timeoutMs);
    if (!payload) {
      return std::nullopt;
    }
    return toPyBytes(*payload);
  }

  std::optional<PyCollaborationData>
  waitOne(const std::string& keyScope,
          const std::string& topicPrefix,
          int timeoutMs)
  {
    py::gil_scoped_release release;
    auto data = m_ctx->waitOne(keyScope, ndn::Name(topicPrefix), timeoutMs);
    if (!data) {
      return std::nullopt;
    }
    py::gil_scoped_acquire acquire;
    return toPyCollaborationData(*data);
  }

  std::vector<PyCollaborationData>
  waitFor(const std::string& keyScope,
          const std::string& topicPrefix,
          size_t minCount,
          int timeoutMs)
  {
    std::vector<nsf::ServiceProvider::CollaborationData> nativeData;
    {
      py::gil_scoped_release release;
      nativeData = m_ctx->waitFor(keyScope, ndn::Name(topicPrefix), minCount, timeoutMs);
    }
    std::vector<PyCollaborationData> output;
    output.reserve(nativeData.size());
    for (const auto& data : nativeData) {
      output.push_back(toPyCollaborationData(data));
    }
    return output;
  }

  void publishFinalResponse(const py::bytes& payload)
  {
    m_ctx->publishFinalResponse(toBuffer(payload));
  }

private:
  nsf::ServiceProvider::CollaborationContext* m_ctx = nullptr;
};

class NativeServiceProvider
{
public:
  NativeServiceProvider(const std::string& providerId,
                        const std::string& group,
                        const std::string& controller,
                        const std::string& providerPrefix,
                        const std::string& trustSchema,
                        size_t handlerThreads,
                        size_t ackThreads,
                        bool serveCertificates)
    : m_group(group)
    , m_controller(controller)
    , m_providerPrefix(providerPrefix)
    , m_providerIdentity(providerId.empty() ? m_providerPrefix : ndn::Name(m_providerPrefix).append(providerId))
    , m_trustSchema(trustSchema)
    , m_handlerThreads(handlerThreads)
    , m_ackThreads(ackThreads)
    , m_serveCertificates(serveCertificates)
  {
    m_providerCert = getOrCreateIdentity(m_keyChain, m_providerIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controller);
    {
      std::lock_guard<std::mutex> lock(g_keyChainMutex);
      m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_providerIdentity));
    }
    if (m_serveCertificates) {
      m_certPublisher = std::make_unique<nsf::CertificatePublisher>(
        m_face, m_keyChain, m_providerCert.getName());
    }
    m_provider = std::make_unique<nsf::ServiceProvider>(
      m_face, m_group, m_providerCert, m_controllerCert, m_trustSchema);
    m_provider->setPerformanceMode(true);
    m_provider->setUseTokens(true);
    m_provider->setHandlerThreads(m_handlerThreads);
    m_provider->setAckThreads(m_ackThreads);
  }

  ~NativeServiceProvider()
  {
    stop();
  }

  void
  addService(const std::string& serviceName,
             py::function requestHandler,
             std::optional<py::function> ackHandler)
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    m_handlers.emplace(serviceName, requestHandler);
    if (ackHandler) {
      m_ackHandlers.emplace(serviceName, *ackHandler);
    }

    m_provider->addService(
      ndn::Name(serviceName),
      nsf::ServiceProvider::AckStrategyHandler(
        [this, serviceName](const nsf::RequestMessage& request) {
          nsf::ServiceProvider::AckDecision decision;
          auto it = m_ackHandlers.find(serviceName);
          if (it == m_ackHandlers.end()) {
            decision.status = true;
            decision.message = "python-provider-ready";
            return decision;
          }
          py::gil_scoped_acquire gil;
          try {
            py::object result = it->second(toPyBytes(request.getPayload()));
            if (py::isinstance<PyAckDecision>(result)) {
              auto pyDecision = result.cast<PyAckDecision>();
              decision.status = pyDecision.status;
              decision.suppressAck = pyDecision.suppress;
              decision.message = pyDecision.message;
              decision.payload = toBuffer(pyDecision.payload);
            }
            else {
              decision.status = result.cast<bool>();
              decision.message = decision.status ? "python-provider-ready" : "python-provider-rejected";
            }
          }
          catch (const py::error_already_set& e) {
            decision.status = false;
            decision.suppressAck = true;
            decision.message = e.what();
          }
          return decision;
        }),
      nsf::ServiceProvider::RequestHandler(
        [this, serviceName](const ndn::Name&,
                            const ndn::Name&,
                            const ndn::Name&,
                            const ndn::Name&,
                            const nsf::RequestMessage& request) {
          nsf::ResponseMessage response;
          py::gil_scoped_acquire gil;
          try {
            py::object result = m_handlers.at(serviceName)(toPyBytes(request.getPayload()));
            if (py::isinstance<PyServiceResponse>(result)) {
              auto pyResponse = result.cast<PyServiceResponse>();
              response.setStatus(pyResponse.status);
              response.setErrorInfo(pyResponse.error.empty() ? "No error" : pyResponse.error);
              auto payload = toBuffer(pyResponse.payload);
              response.setPayload(payload, payload.size());
            }
            else {
              auto payload = toBuffer(result.cast<py::bytes>());
              response.setStatus(true);
              response.setErrorInfo("No error");
              response.setPayload(payload, payload.size());
            }
          }
          catch (const py::error_already_set& e) {
            response.setStatus(false);
            response.setErrorInfo(e.what());
          }
          return response;
        }));
  }

  void
  addCollaborationService(const std::string& serviceName,
                          const std::vector<std::string>& allowedRoles,
                          py::function collaborationHandler,
                          std::optional<py::function> ackHandler)
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    m_collaborationHandlers.emplace(serviceName, collaborationHandler);
    if (ackHandler) {
      m_collaborationAckHandlers.emplace(serviceName, *ackHandler);
    }

    m_provider->addCollaborationHandler(
      ndn::Name(serviceName),
      allowedRoles,
      nsf::ServiceProvider::AckStrategyHandler(
        [this, serviceName](const nsf::RequestMessage& request) {
          nsf::ServiceProvider::AckDecision decision;
          auto it = m_collaborationAckHandlers.find(serviceName);
          if (it == m_collaborationAckHandlers.end()) {
            decision.status = true;
            decision.message = "python-collaboration-provider-ready";
            return decision;
          }
          py::gil_scoped_acquire gil;
          try {
            py::object result = it->second(toPyBytes(request.getPayload()));
            if (py::isinstance<PyAckDecision>(result)) {
              auto pyDecision = result.cast<PyAckDecision>();
              decision.status = pyDecision.status;
              decision.suppressAck = pyDecision.suppress;
              decision.message = pyDecision.message;
              decision.payload = toBuffer(pyDecision.payload);
            }
            else {
              decision.status = result.cast<bool>();
              decision.message = decision.status ?
                "python-collaboration-provider-ready" :
                "python-collaboration-provider-rejected";
            }
          }
          catch (const py::error_already_set& e) {
            decision.status = false;
            decision.suppressAck = true;
            decision.message = e.what();
          }
          return decision;
        }),
      nsf::ServiceProvider::CollaborationHandler(
        [this, serviceName](nsf::ServiceProvider::CollaborationContext& ctx,
                            const nsf::RequestMessage& request) {
          py::gil_scoped_acquire gil;
          try {
            PyCollaborationContext pyCtx(ctx);
            m_collaborationHandlers.at(serviceName)(pyCtx, toPyBytes(request.getPayload()));
          }
          catch (const py::error_already_set& e) {
            ctx.fail(e.what());
          }
        }));
  }

  void
  start()
  {
    if (m_running.exchange(true)) {
      return;
    }
    m_provider->init();
    m_provider->fetchPermissionsFromController(m_controller);
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, pythonFacePollTimeout());
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = e.what();
          m_running = false;
        }
      }
    });
  }

  void
  run()
  {
    start();
    while (m_running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      throwIfError();
    }
  }

  void
  stop()
  {
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void
  throwIfError()
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error.empty()) {
      throw std::runtime_error(m_error);
    }
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_group;
  ndn::Name m_controller;
  ndn::Name m_providerPrefix;
  ndn::Name m_providerIdentity;
  std::string m_trustSchema;
  size_t m_handlerThreads = 4;
  size_t m_ackThreads = 2;
  bool m_serveCertificates = true;
  ndn::security::Certificate m_providerCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<nsf::CertificatePublisher> m_certPublisher;
  std::unique_ptr<nsf::ServiceProvider> m_provider;
  std::map<std::string, py::function> m_handlers;
  std::map<std::string, py::function> m_ackHandlers;
  std::map<std::string, py::function> m_collaborationHandlers;
  std::map<std::string, py::function> m_collaborationAckHandlers;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_errorMutex;
  std::string m_error;
};

class NativeServiceController
{
public:
  NativeServiceController(const std::string& controllerPrefix,
                          const std::string& policyFile,
                          const std::string& trustSchema,
                          const std::vector<std::string>& bootstrapIdentities,
                          bool serveCertificates)
    : m_controllerPrefix(controllerPrefix)
    , m_policyFile(policyFile)
    , m_trustSchema(trustSchema)
    , m_validator(m_face)
    , m_serveCertificates(serveCertificates)
  {
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controllerPrefix);
    {
      std::lock_guard<std::mutex> lock(g_keyChainMutex);
      m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_controllerPrefix));
    }
    for (const auto& identity : bootstrapIdentities) {
      if (!identity.empty()) {
        getOrCreateIdentity(m_keyChain, ndn::Name(identity));
      }
    }
    if (!m_trustSchema.empty()) {
      m_validator.load(m_trustSchema);
    }
    if (m_serveCertificates) {
      m_certPublisher = std::make_unique<nsf::CertificatePublisher>(
        m_face, m_keyChain, m_controllerCert.getName());
      const auto rootIdentity = m_controllerPrefix.getPrefix(-1);
      if (!rootIdentity.empty() && rootIdentity != m_controllerPrefix) {
        try {
          m_rootCertPublisher = std::make_unique<nsf::CertificatePublisher>(
            m_face, m_keyChain, rootIdentity);
        }
        catch (const std::exception&) {
        }
      }
    }
    m_controller = std::make_unique<nsf::ServiceController>(
      m_face, m_controllerCert, m_validator, m_policyFile);
    m_controller->setControllerPrefix(m_controllerPrefix);
  }

  ~NativeServiceController()
  {
    stop();
  }

  void
  start()
  {
    if (m_running.exchange(true)) {
      return;
    }
    m_thread = std::thread([this] {
      try {
        m_controller->run();
      }
      catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = e.what();
      }
      m_running = false;
    });
  }

  void
  run()
  {
    if (m_running.exchange(true)) {
      return;
    }
    try {
      m_controller->run();
    }
    catch (const std::exception& e) {
      {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = e.what();
      }
      m_running = false;
      throw;
    }
    m_running = false;
  }

  void
  stop()
  {
    m_running = false;
    m_face.shutdown();
    m_face.getIoContext().stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void
  throwIfError()
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error.empty()) {
      throw std::runtime_error(m_error);
    }
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_controllerPrefix;
  std::string m_policyFile;
  std::string m_trustSchema;
  ndn::ValidatorConfig m_validator;
  bool m_serveCertificates = true;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<nsf::CertificatePublisher> m_certPublisher;
  std::unique_ptr<nsf::CertificatePublisher> m_rootCertPublisher;
  std::unique_ptr<nsf::ServiceController> m_controller;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_errorMutex;
  std::string m_error;
};

class NativeServiceUser
{
public:
  NativeServiceUser(const std::string& group,
                    const std::string& controller,
                    const std::string& userIdentity,
                    const std::string& trustSchema,
                    int permissionWaitMs,
                    size_t handlerThreads,
                    size_t ackThreads,
                    bool adaptiveAdmission,
                    bool serveCertificates)
    : m_group(group)
    , m_controller(controller)
    , m_userIdentity(userIdentity)
    , m_trustSchema(trustSchema)
    , m_permissionWaitMs(permissionWaitMs)
  {
    m_userCert = getOrCreateIdentity(m_keyChain, m_userIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controller);
    {
      std::lock_guard<std::mutex> lock(g_keyChainMutex);
      m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_userIdentity));
    }
    if (serveCertificates) {
      m_certPublisher = std::make_unique<nsf::CertificatePublisher>(
        m_face, m_keyChain, m_userCert.getName());
    }
    m_user = std::make_unique<nsf::ServiceUser>(
      m_face, m_group, m_userCert, m_controllerCert, m_trustSchema);
    m_user->setPerformanceMode(true);
    m_user->setUseTokens(true);
    m_user->setHandlerThreads(handlerThreads);
    m_user->setAckProcessingThreads(ackThreads);
    nsf::ServiceUser::AdaptiveAdmissionOptions admission;
    admission.enabled = adaptiveAdmission;
    m_user->setAdaptiveAdmissionControl(admission);
    m_user->fetchPermissionsFromController(m_controller);
    m_user->init();
    pump(m_permissionWaitMs);
  }

  ~NativeServiceUser()
  {
    stop();
  }

  void
  start()
  {
    if (m_running.exchange(true)) {
      return;
    }
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, pythonFacePollTimeout());
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = e.what();
          m_running = false;
        }
      }
    });
  }

  void
  stop()
  {
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void
  throwIfError()
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error.empty()) {
      throw std::runtime_error(m_error);
    }
  }

PyServiceResponse
  requestService(const std::string& serviceName,
                 const py::bytes& requestPayload,
                 int ackTimeoutMs,
                 int timeoutMs,
                 const std::string& strategy)
  {
    PyServiceResponse output;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    auto payload = toBuffer(requestPayload);
    auto selection = selectionPolicyByName(strategy);

    auto submit = [&, payload, selection] {
      m_user->RequestService(
        ndn::Name(serviceName),
        payload,
        ackTimeoutMs,
        selection,
        timeoutMs,
        [&](const nsf::ResponseMessage& response) {
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(mutex);
          output.status = response.getStatus();
          output.payload = toPyBytes(response.getPayload());
          output.error = response.getErrorInfo();
          done = true;
          cv.notify_one();
        },
        [&](const ndn::Name& requestId) {
          std::lock_guard<std::mutex> lock(mutex);
          output.status = false;
          output.error = "timeout: " + requestId.toUri();
          done = true;
          cv.notify_one();
        });
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), submit);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs + 3000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_until(lock, deadline, [&done] { return done; });
      if (done) {
        return output;
      }
      output.status = false;
      output.error = "local deadline";
      return output;
    }

    std::lock_guard<std::mutex> callLock(m_callMutex);
    submit();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done) {
          return output;
        }
      }
      py::gil_scoped_release release;
      processFaceEvents(m_face, pythonFacePollTimeout());
    }
    output.status = false;
    output.error = "local deadline";
    return output;
  }

  PyLargeDataPublishResult
  publishEncryptedLargeData(const std::string& serviceName,
                            const py::bytes& payload,
                            const std::string& objectLabel,
                            int freshnessMs)
  {
    auto data = toBuffer(payload);
    std::vector<uint8_t> plaintext(data.begin(), data.end());
    struct PublishState
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      nsf::LargeDataPublishResult result;
    };
    auto state = std::make_shared<PublishState>();
    auto submit = [this, serviceName, plaintext = std::move(plaintext),
                   objectLabel, freshnessMs, state] {
      auto ctx = m_user->prepareServiceRequest(serviceName);
      try {
        state->result = m_user->publishEncryptedLargeData(
          ctx,
          plaintext,
          objectLabel,
          ndn::time::milliseconds(freshnessMs));
      }
      catch (const std::exception& e) {
        state->result.success = false;
        state->result.errorMessage = e.what();
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      state->done = true;
      state->cv.notify_one();
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), std::move(submit));
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(30000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(state->mutex);
      if (!state->cv.wait_until(lock, deadline, [&state] { return state->done; })) {
        PyLargeDataPublishResult output;
        output.success = false;
        output.error = "local deadline";
        return output;
      }
    }
    else {
      std::lock_guard<std::mutex> callLock(m_callMutex);
      submit();
    }

    PyLargeDataPublishResult output;
    output.success = state->result.success;
    output.encryptedDataName = state->result.encryptedDataName.toUri();
    output.objectId = state->result.objectId;
    output.error = state->result.errorMessage;
    return output;
  }

  PyServiceResponse
  requestCollaboration(const std::string& serviceName,
                       const py::bytes& initialPayload,
                       const std::vector<std::map<std::string, py::object>>& roles,
                       const std::map<std::string, std::vector<std::string>>& keyScopes,
                       const std::vector<std::map<std::string, py::object>>& dependencies,
                       const std::map<std::string, std::string>& artifactDataNames,
                       const std::map<std::string, std::string>& scopeKeyDataNames,
                       const std::map<std::string, std::vector<std::string>>& roleScopes,
                       int ackTimeoutMs,
                       int timeoutMs)
  {
    nsf::CollaborationPlan plan;
    plan.ackCollectionTimeMs = ackTimeoutMs;
    plan.timeoutMs = timeoutMs;

    for (const auto& entry : roles) {
      nsf::CollaborationRoleSpec role;
      auto roleIt = entry.find("role");
      if (roleIt == entry.end()) {
        throw std::runtime_error("collaboration role entry missing 'role'");
      }
      role.role = py::cast<std::string>(roleIt->second);
      auto serviceIt = entry.find("service");
      role.service = serviceIt == entry.end() ?
        ndn::Name(serviceName) :
        ndn::Name(py::cast<std::string>(serviceIt->second));
      auto artifactIt = entry.find("artifact");
      if (artifactIt != entry.end()) {
        role.requiredArtifact = ndn::Name(py::cast<std::string>(artifactIt->second));
      }
      auto dynamicIt = entry.find("allow_dynamic_provisioning");
      if (dynamicIt != entry.end()) {
        role.allowDynamicProvisioning = py::cast<bool>(dynamicIt->second);
      }
      auto timeoutIt = entry.find("provisioning_timeout_ms");
      if (timeoutIt != entry.end()) {
        role.provisioningTimeoutMs = py::cast<int>(timeoutIt->second);
      }
      auto minIt = entry.find("min_providers");
      if (minIt != entry.end()) {
        role.minProviders = py::cast<size_t>(minIt->second);
      }
      auto maxIt = entry.find("max_providers");
      if (maxIt != entry.end()) {
        role.maxProviders = py::cast<size_t>(maxIt->second);
      }
      auto reqIt = entry.find("app_requirement");
      if (reqIt != entry.end() && !reqIt->second.is_none()) {
        role.appRequirement = toBuffer(reqIt->second.cast<py::bytes>());
      }
      plan.roles.push_back(std::move(role));
    }

    for (const auto& entry : keyScopes) {
      plan.keyScopes.push_back({entry.first, entry.second});
    }

    auto readStringList = [](const std::map<std::string, py::object>& dict,
                             const std::string& key) {
      auto it = dict.find(key);
      if (it == dict.end() || it->second.is_none()) {
        return std::vector<std::string>{};
      }
      return py::cast<std::vector<std::string>>(it->second);
    };

    for (const auto& entry : dependencies) {
      nsf::CollaborationDependency dep;
      dep.producers = readStringList(entry, "producers");
      dep.consumers = readStringList(entry, "consumers");
      auto scopeIt = entry.find("key_scope");
      if (scopeIt != entry.end()) {
        dep.keyScope = py::cast<std::string>(scopeIt->second);
      }
      auto topicIt = entry.find("topic_prefix");
      if (topicIt != entry.end()) {
        dep.topicPrefix = ndn::Name(py::cast<std::string>(topicIt->second));
      }
      auto requiredIt = entry.find("required");
      if (requiredIt != entry.end()) {
        dep.required = py::cast<bool>(requiredIt->second);
      }
      plan.dependencies.push_back(std::move(dep));
    }

    std::map<std::string, ndn::Name> nativeArtifactDataNames;
    for (const auto& entry : artifactDataNames) {
      nativeArtifactDataNames.emplace(entry.first, ndn::Name(entry.second));
    }
    std::map<std::string, ndn::Name> nativeScopeKeyDataNames;
    for (const auto& entry : scopeKeyDataNames) {
      nativeScopeKeyDataNames.emplace(entry.first, ndn::Name(entry.second));
    }
    plan.participantSelector = std::make_shared<RoleAssignmentSelectionPolicy>(
      std::move(nativeArtifactDataNames),
      std::move(nativeScopeKeyDataNames),
      roleScopes);

    PyServiceResponse output;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    auto payload = toBuffer(initialPayload);

    auto submit = [&, payload, plan = std::move(plan)]() mutable {
      m_user->RequestCollaboration(
        ndn::Name(serviceName),
        payload,
        std::move(plan),
        [&](const nsf::ResponseMessage& response) {
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(mutex);
          output.status = response.getStatus();
          output.payload = toPyBytes(response.getPayload());
          output.error = response.getErrorInfo();
          done = true;
          cv.notify_one();
        },
        [&](const ndn::Name& requestId) {
          std::lock_guard<std::mutex> lock(mutex);
          output.status = false;
          output.error = "timeout: " + requestId.toUri();
          done = true;
          cv.notify_one();
        });
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), std::move(submit));
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs + 3000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_until(lock, deadline, [&done] { return done; });
      if (done) {
        return output;
      }
      output.status = false;
      output.error = "local deadline";
      return output;
    }

    std::lock_guard<std::mutex> callLock(m_callMutex);
    submit();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done) {
          return output;
        }
      }
      py::gil_scoped_release release;
      processFaceEvents(m_face, pythonFacePollTimeout());
    }
    output.status = false;
    output.error = "local deadline";
    return output;
  }

  PyServiceResponse
  requestServiceSelect(const std::string& serviceName,
                       const py::bytes& requestPayload,
                       py::function selector,
                       int ackTimeoutMs,
                       int timeoutMs,
                       const std::string& requestStrategy)
  {
    PyServiceResponse output;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    auto payload = toBuffer(requestPayload);
    auto selectorFn = keepPyFunction(std::move(selector));
    const size_t nativeStrategy =
      (requestStrategy == "all-selected" || requestStrategy == "all-responders") ?
      nsf::tlv::AllSelected : nsf::tlv::FirstResponding;

    nsf::ServiceUser::AckCandidatesHandler handler =
      [selectorFn](const std::vector<nsf::AckSelectionCandidate>& candidates) {
        py::gil_scoped_acquire gil;
        py::list pyCandidates;
        for (const auto& candidate : candidates) {
          PyAckCandidate item;
          item.providerName = candidate.providerName.toUri();
          item.serviceName = candidate.serviceName.toUri();
          item.requestId = candidate.requestId.toUri();
          item.status = candidate.ack.getStatus();
          item.message = candidate.ack.getMessage();
          item.payload = toPyBytes(candidate.ack.getPayload());
          pyCandidates.append(py::cast(item));
        }

        std::vector<std::string> selectedProviderNames;
        py::object selected = (*selectorFn)(pyCandidates);
        selectedProviderNames = selected.cast<std::vector<std::string>>();

        std::vector<nsf::AckSelectionCandidate> selectedCandidates;
        for (const auto& providerName : selectedProviderNames) {
          for (const auto& candidate : candidates) {
            if (candidate.ack.getStatus() &&
                candidate.providerName.toUri() == providerName) {
              selectedCandidates.push_back(candidate);
              break;
            }
          }
        }
        return selectedCandidates;
      };

    auto submit = [&, payload, handler, nativeStrategy] {
      nsf::RequestMessage requestMessage;
      auto mutablePayload = payload;
      requestMessage.setPayload(mutablePayload, mutablePayload.size());
      requestMessage.setStrategy(nativeStrategy);
      m_user->RequestService(
        std::vector<ndn::Name>{},
        ndn::Name(serviceName),
        requestMessage,
        ackTimeoutMs,
        handler,
        timeoutMs,
        [&](const ndn::Name& requestId) {
          std::lock_guard<std::mutex> lock(mutex);
          output.status = false;
          output.error = "timeout: " + requestId.toUri();
          done = true;
          cv.notify_one();
        },
        [&](const nsf::ResponseMessage& response) {
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(mutex);
          output.status = response.getStatus();
          output.payload = toPyBytes(response.getPayload());
          output.error = response.getErrorInfo();
          done = true;
          cv.notify_one();
        },
        nativeStrategy);
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), submit);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs + 3000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_until(lock, deadline, [&done] { return done; });
      if (done) {
        return output;
      }
      output.status = false;
      output.error = "local deadline";
      return output;
    }

    std::lock_guard<std::mutex> callLock(m_callMutex);
    submit();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done) {
          return output;
        }
      }
      py::gil_scoped_release release;
      processFaceEvents(m_face, pythonFacePollTimeout());
    }
    output.status = false;
    output.error = "local deadline";
    return output;
  }

  void
  requestServiceAsync(const std::string& serviceName,
                      const py::bytes& requestPayload,
                      py::function onResponse,
                      py::function onTimeout,
                      int ackTimeoutMs,
                      int timeoutMs,
                      const std::string& strategy)
  {
    start();
    auto payload = toBuffer(requestPayload);
    auto selection = selectionPolicyByName(strategy);
    auto responseCallback = keepPyFunction(std::move(onResponse));
    auto timeoutCallback = keepPyFunction(std::move(onTimeout));
    boost::asio::post(m_face.getIoContext(),
      [this, serviceName, payload, selection, ackTimeoutMs, timeoutMs,
       responseCallback = std::move(responseCallback),
       timeoutCallback = std::move(timeoutCallback)] {
        m_user->RequestService(
          ndn::Name(serviceName),
          payload,
          ackTimeoutMs,
          selection,
          timeoutMs,
          [responseCallback](const nsf::ResponseMessage& response) mutable {
            py::gil_scoped_acquire gil;
            PyServiceResponse output;
            output.status = response.getStatus();
            output.payload = toPyBytes(response.getPayload());
            output.error = response.getErrorInfo();
            try {
              (*responseCallback)(output);
            }
            catch (const py::error_already_set& e) {
              PyErr_WriteUnraisable(e.value().ptr());
            }
          },
          [timeoutCallback](const ndn::Name& requestId) mutable {
            py::gil_scoped_acquire gil;
            try {
              (*timeoutCallback)(requestId.toUri());
            }
            catch (const py::error_already_set& e) {
              PyErr_WriteUnraisable(e.value().ptr());
            }
          });
      });
  }

  void
  pump(int milliseconds)
  {
    if (m_running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
      return;
    }
    std::lock_guard<std::mutex> callLock(m_callMutex);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
      processFaceEvents(m_face, pythonFacePollTimeout());
    }
  }

  std::vector<std::tuple<std::string, std::string, std::string>>
  getAllowedServices() const
  {
    return m_user->getAllowedServices();
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_group;
  ndn::Name m_controller;
  ndn::Name m_userIdentity;
  std::string m_trustSchema;
  int m_permissionWaitMs = 1500;
  ndn::security::Certificate m_userCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<nsf::CertificatePublisher> m_certPublisher;
  std::unique_ptr<nsf::ServiceUser> m_user;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_callMutex;
  std::mutex m_errorMutex;
  std::string m_error;
};

} // namespace

PYBIND11_MODULE(_ndnsf, m)
{
  m.def("encode_large_data_reference_payload",
        [](const std::string& dataName,
           const std::string& objectType,
           const std::string& objectId,
           size_t plaintextSize,
           bool encrypted,
           const std::string& digest) {
          nsf::LargeDataReference reference;
          reference.dataName = ndn::Name(dataName);
          reference.objectType = objectType;
          reference.objectId = objectId;
          reference.plaintextSize = plaintextSize;
          reference.encrypted = encrypted;
          reference.digest = digest;
          const auto payload = nsf::encodeLargeDataReferencePayload(reference);
          return toPyBytes(payload);
        },
        py::arg("data_name"),
        py::arg("object_type") = "",
        py::arg("object_id") = "",
        py::arg("plaintext_size") = 0,
        py::arg("encrypted") = true,
        py::arg("digest") = "");

  m.def("parse_large_data_reference_payload",
        [](const py::bytes& payload) -> py::object {
          const auto reference = nsf::parseLargeDataReferencePayload(toBuffer(payload));
          if (!reference) {
            return py::none();
          }
          return largeDataReferenceToDict(*reference);
        },
        py::arg("payload"));

  py::class_<PyServiceResponse>(m, "ServiceResponse")
    .def(py::init<>())
    .def_readwrite("status", &PyServiceResponse::status)
    .def_readwrite("payload", &PyServiceResponse::payload)
    .def_readwrite("error", &PyServiceResponse::error);

  py::class_<PyAckDecision>(m, "AckDecision")
    .def(py::init<>())
    .def_readwrite("status", &PyAckDecision::status)
    .def_readwrite("payload", &PyAckDecision::payload)
    .def_readwrite("message", &PyAckDecision::message)
    .def_readwrite("suppress", &PyAckDecision::suppress);

  py::class_<PyAckCandidate>(m, "AckCandidate")
    .def(py::init<>())
    .def_readwrite("provider_name", &PyAckCandidate::providerName)
    .def_readwrite("service_name", &PyAckCandidate::serviceName)
    .def_readwrite("request_id", &PyAckCandidate::requestId)
    .def_readwrite("status", &PyAckCandidate::status)
    .def_readwrite("message", &PyAckCandidate::message)
    .def_readwrite("payload", &PyAckCandidate::payload);

  py::class_<PyLargeDataPublishResult>(m, "LargeDataPublishResult")
    .def(py::init<>())
    .def_readwrite("success", &PyLargeDataPublishResult::success)
    .def_readwrite("encrypted_data_name", &PyLargeDataPublishResult::encryptedDataName)
    .def_readwrite("object_id", &PyLargeDataPublishResult::objectId)
    .def_readwrite("error", &PyLargeDataPublishResult::error);

  py::class_<PyCollaborationAssignment>(m, "CollaborationAssignment")
    .def(py::init<>())
    .def_readwrite("role", &PyCollaborationAssignment::role)
    .def_readwrite("service", &PyCollaborationAssignment::service)
    .def_readwrite("assigned_artifact", &PyCollaborationAssignment::assignedArtifact)
    .def_readwrite("artifact_data_name", &PyCollaborationAssignment::artifactDataName)
    .def_readwrite("requires_provisioning", &PyCollaborationAssignment::requiresProvisioning)
    .def_readwrite("provisioning_timeout_ms", &PyCollaborationAssignment::provisioningTimeoutMs)
    .def_readwrite("assignment_payload", &PyCollaborationAssignment::assignmentPayload);

  py::class_<PyCollaborationData>(m, "CollaborationData")
    .def(py::init<>())
    .def_readwrite("session_id", &PyCollaborationData::sessionId)
    .def_readwrite("key_scope", &PyCollaborationData::keyScope)
    .def_readwrite("topic", &PyCollaborationData::topic)
    .def_readwrite("producer", &PyCollaborationData::producer)
    .def_readwrite("producer_role", &PyCollaborationData::producerRole)
    .def_readwrite("sequence", &PyCollaborationData::sequence)
    .def_readwrite("payload", &PyCollaborationData::payload);

  py::class_<NativeSegmentedObjectProducer>(m, "SegmentedObjectProducer")
    .def(py::init<const std::string&,
                  const py::bytes&,
                  const std::string&,
                  size_t,
                  int>(),
         py::arg("base_name"),
         py::arg("payload"),
         py::arg("signing_identity") = "",
         py::arg("max_segment_size") = 6000,
         py::arg("freshness_ms") = 60000)
    .def("start", &NativeSegmentedObjectProducer::start)
    .def("stop", &NativeSegmentedObjectProducer::stop)
    .def_property_readonly("base_name", &NativeSegmentedObjectProducer::baseName)
    .def_property_readonly("versioned_name", &NativeSegmentedObjectProducer::versionedName)
    .def_property_readonly("segment_count", &NativeSegmentedObjectProducer::segmentCount)
    .def_property_readonly("error", &NativeSegmentedObjectProducer::error);

  py::class_<PyDataPacket>(m, "DataPacket")
    .def(py::init<>())
    .def_readwrite("name", &PyDataPacket::name)
    .def_readwrite("segment", &PyDataPacket::segment)
    .def_readwrite("wire", &PyDataPacket::wire);

  py::class_<PySegmentHintRange>(m, "SegmentHintRange")
    .def(py::init<>())
    .def_readwrite("start", &PySegmentHintRange::start)
    .def_readwrite("end", &PySegmentHintRange::end)
    .def_readwrite("forwarding_hints", &PySegmentHintRange::forwardingHints);

  py::class_<NativeWireDataProducer>(m, "StoredDataProducer")
    .def(py::init<const std::string&, const std::vector<py::bytes>&, const std::string&>(),
         py::arg("base_name"),
         py::arg("packet_wires"),
         py::arg("signing_identity") = "")
    .def("start", &NativeWireDataProducer::start)
    .def("stop", &NativeWireDataProducer::stop)
    .def_property_readonly("segment_count", &NativeWireDataProducer::segmentCount)
    .def_property_readonly("error", &NativeWireDataProducer::error);

  m.def("make_segmented_data_packets",
        &makeSegmentedDataPackets,
        py::arg("base_name"),
        py::arg("payload"),
        py::arg("signing_identity") = "",
        py::arg("max_segment_size") = 6000,
        py::arg("freshness_ms") = 60000);

  m.def("fetch_segmented_data_packets",
        &fetchSegmentedDataPackets,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 1000,
        py::arg("forwarding_hints") = std::vector<std::string>{});

  m.def("fetch_segmented_object",
        &fetchSegmentedObject,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 1000,
        py::arg("init_cwnd") = 8.0,
        py::arg("forwarding_hints") = std::vector<std::string>{});

  m.def("fetch_segmented_object_with_segment_hints",
        &fetchSegmentedObjectWithSegmentHints,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 1000,
        py::arg("hint_ranges") = std::vector<PySegmentHintRange>{});
  m.def("fetch_known_segmented_object_with_segment_hints",
        &fetchKnownSegmentedObjectWithSegmentHints,
        py::arg("versioned_name"),
        py::arg("segment_count"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 1000,
        py::arg("hint_ranges") = std::vector<PySegmentHintRange>{});

  py::class_<PyCollaborationContext>(m, "CollaborationContext")
    .def_property_readonly("session_id", &PyCollaborationContext::sessionId)
    .def_property_readonly("role", &PyCollaborationContext::role)
    .def_property_readonly("local_provider", &PyCollaborationContext::localProvider)
    .def_property_readonly("assignment", &PyCollaborationContext::assignment)
    .def("fetch_artifact", &PyCollaborationContext::fetchArtifact,
         py::arg("artifact_name"),
         py::arg("timeout_ms") = 5000)
    .def("get_artifact", &PyCollaborationContext::getArtifact,
         py::arg("artifact_name"))
    .def("fetch_encrypted_large_data", &PyCollaborationContext::fetchEncryptedLargeData,
         py::arg("data_name"),
         py::arg("service") = "")
    .def("fail", &PyCollaborationContext::fail,
         py::arg("reason"))
    .def("publish", &PyCollaborationContext::publish,
         py::arg("key_scope"),
         py::arg("topic"),
         py::arg("payload"))
    .def("publish_large", &PyCollaborationContext::publishLarge,
         py::arg("key_scope"),
         py::arg("topic"),
         py::arg("payload"),
         py::arg("max_segment_size") = 7000,
         py::arg("freshness_ms") = 60000)
    .def("publish_large_named", &PyCollaborationContext::publishLargeNamed,
         py::arg("key_scope"),
         py::arg("data_name"),
         py::arg("payload"),
         py::arg("max_segment_size") = 7000,
         py::arg("freshness_ms") = 60000)
    .def("fetch_large", &PyCollaborationContext::fetchLarge,
         py::arg("data_name"),
         py::arg("key_scope"),
         py::arg("timeout_ms") = 5000)
    .def("wait_one", &PyCollaborationContext::waitOne,
         py::arg("key_scope"),
         py::arg("topic_prefix"),
         py::arg("timeout_ms") = 5000)
    .def("wait_for", &PyCollaborationContext::waitFor,
         py::arg("key_scope"),
         py::arg("topic_prefix"),
         py::arg("min_count"),
         py::arg("timeout_ms") = 5000)
    .def("publish_final_response", &PyCollaborationContext::publishFinalResponse,
         py::arg("payload"));

  py::class_<NativeServiceController>(m, "NativeServiceController")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::vector<std::string>&,
                  bool>(),
         py::arg("controller_prefix") = "/example/hello/controller",
         py::arg("policy_file") = "examples/hello.policies",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("bootstrap_identities") = std::vector<std::string>{},
         py::arg("serve_certificates") = true)
    .def("start", &NativeServiceController::start)
    .def("run", &NativeServiceController::run, py::call_guard<py::gil_scoped_release>())
    .def("stop", &NativeServiceController::stop);

  py::class_<NativeServiceProvider>(m, "NativeServiceProvider")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::string&,
                  size_t,
                  size_t,
                  bool>(),
         py::arg("provider_id") = "",
         py::arg("group") = "/example/hello/group",
         py::arg("controller") = "/example/hello/controller",
         py::arg("provider_prefix") = "/example/hello/provider",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("handler_threads") = 4,
         py::arg("ack_threads") = 2,
         py::arg("serve_certificates") = true)
    .def("add_service", &NativeServiceProvider::addService,
         py::arg("service"),
         py::arg("request_handler"),
         py::arg("ack_handler") = std::optional<py::function>())
    .def("add_collaboration_service", &NativeServiceProvider::addCollaborationService,
         py::arg("service"),
         py::arg("allowed_roles"),
         py::arg("collaboration_handler"),
         py::arg("ack_handler") = std::optional<py::function>())
    .def("start", &NativeServiceProvider::start)
    .def("run", &NativeServiceProvider::run, py::call_guard<py::gil_scoped_release>())
    .def("stop", &NativeServiceProvider::stop);

  py::class_<NativeServiceUser>(m, "NativeServiceUser")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::string&,
                  int,
                  size_t,
                  size_t,
                  bool,
                  bool>(),
         py::arg("group") = "/example/hello/group",
         py::arg("controller") = "/example/hello/controller",
         py::arg("user") = "/example/hello/user",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("permission_wait_ms") = 1500,
         py::arg("handler_threads") = 2,
         py::arg("ack_threads") = 2,
         py::arg("adaptive_admission") = false,
         py::arg("serve_certificates") = true)
    .def("request_service", &NativeServiceUser::requestService,
         py::arg("service"),
         py::arg("payload"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("strategy") = "first-responding")
    .def("request_service_select", &NativeServiceUser::requestServiceSelect,
         py::arg("service"),
         py::arg("payload"),
         py::arg("selector"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("request_strategy") = "first-responding")
    .def("request_service_async", &NativeServiceUser::requestServiceAsync,
         py::arg("service"),
         py::arg("payload"),
         py::arg("on_response"),
         py::arg("on_timeout"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("strategy") = "first-responding")
    .def("publish_encrypted_large_data", &NativeServiceUser::publishEncryptedLargeData,
         py::arg("service"),
         py::arg("payload"),
         py::arg("object_label") = "",
         py::arg("freshness_ms") = 60000)
    .def("request_collaboration", &NativeServiceUser::requestCollaboration,
         py::arg("service"),
         py::arg("payload"),
         py::arg("roles"),
         py::arg("key_scopes"),
         py::arg("dependencies"),
         py::arg("artifact_data_names"),
         py::arg("scope_key_data_names"),
         py::arg("role_scopes"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 10000)
    .def("start", &NativeServiceUser::start)
    .def("stop", &NativeServiceUser::stop)
    .def("get_allowed_services", &NativeServiceUser::getAllowedServices)
    .def("pump", &NativeServiceUser::pump);
}
