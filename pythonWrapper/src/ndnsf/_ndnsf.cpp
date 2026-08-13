#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/ExecutionLease.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/Stream.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/validator-null.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/segmenter.hpp>

#include <boost/asio/post.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

const ndn::Name&
nfdCommandIdentity()
{
  static const ndn::Name identity([] {
    const char* configured = std::getenv("NDNSF_NFD_COMMAND_IDENTITY");
    return (configured != nullptr && *configured != '\0') ?
      ndn::Name(configured) : ndn::Name("/localhost/operator");
  }());
  return identity;
}

std::optional<ndn::security::Certificate>
loadControllerCertificateOverride(const ndn::Name& controller)
{
  const char* certPath = std::getenv("NDNSF_CONTROLLER_CERT_FILE");
  if (certPath == nullptr || *certPath == '\0') {
    return std::nullopt;
  }

  auto cert = ndn::io::load<ndn::security::Certificate>(certPath);
  if (cert == nullptr || !cert->isValid()) {
    throw std::runtime_error("NDNSF_CONTROLLER_CERT_FILE is not a valid certificate: " +
                             std::string(certPath));
  }
  if (cert->getIdentity() != controller) {
    throw std::runtime_error("NDNSF_CONTROLLER_CERT_FILE identity " +
                             cert->getIdentity().toUri() +
                             " does not match controller " +
                             controller.toUri());
  }
  return *cert;
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

py::bytes
toPyStreamContentDigest(const nsf::StreamContentDigest& value)
{
  return py::bytes(reinterpret_cast<const char*>(value.data()), value.size());
}

nsf::StreamContentDigest
streamContentDigestFromPyBytes(const py::bytes& value)
{
  const std::string bytes = value;
  if (bytes.size() != nsf::StreamContentDigest{}.size()) {
    throw std::invalid_argument("stream content digest must contain exactly 32 bytes");
  }

  nsf::StreamContentDigest digest{};
  std::copy(bytes.begin(), bytes.end(), digest.begin());
  return digest;
}

py::bytes
toPyBlockWire(const ndn::Block& block)
{
  if (!block.isValid()) {
    return py::bytes();
  }
  return py::bytes(reinterpret_cast<const char*>(block.data()), block.size());
}

ndn::Block
blockFromExactPyBytes(const py::bytes& value)
{
  const std::string bytes = value;
  if (bytes.empty()) {
    throw std::invalid_argument("encoded TLV block must not be empty");
  }

  try {
    ndn::Block block(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    if (block.size() != bytes.size()) {
      throw std::invalid_argument("encoded TLV block has trailing bytes");
    }
    return block;
  }
  catch (const std::invalid_argument&) {
    throw;
  }
  catch (const std::exception& error) {
    throw std::invalid_argument(std::string("invalid encoded TLV block: ") +
                                error.what());
  }
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

py::dict
networkTelemetrySnapshotToDict(const nsf::NetworkTelemetrySnapshot& snapshot)
{
  py::dict output;
  output["provider_name"] = snapshot.providerName.toUri();
  output["service_name"] = snapshot.serviceName.toUri();
  output["peer_name"] = snapshot.peerName.toUri();
  output["edge_name"] = snapshot.edgeName;
  output["kind"] = nsf::toString(snapshot.kind);
  output["rtt_ms"] = snapshot.rttMs;
  output["first_byte_ms"] = snapshot.firstByteMs;
  output["elapsed_ms"] = snapshot.elapsedMs;
  output["encoded_bytes"] = snapshot.encodedBytes;
  output["wire_bytes"] = snapshot.wireBytes;
  output["goodput_mbps"] = snapshot.goodputMbps;
  output["received_segments"] = snapshot.receivedSegments;
  output["timeout_count"] = snapshot.timeoutCount;
  output["nack_count"] = snapshot.nackCount;
  output["sample_count"] = snapshot.sampleCount;
  output["last_updated_ms"] = snapshot.lastUpdatedMs;
  output["confidence"] = snapshot.confidence;
  output["stale"] = snapshot.stale;
  output["data_name"] = snapshot.dataName.toUri();
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
  std::string requestId;
  std::string dataName;
  std::string signerCertificate;
  std::string wireDigest;
};

struct PyAckDecision
{
  bool status = true;
  py::bytes payload;
  std::string message = "ok";
  bool suppress = false;
  py::dict reservationLease;
  py::dict selectionInputKeyOffer;
  uint64_t pendingStateTtlMs = 0;
};

template<typename Contract>
std::optional<Contract>
toDeploymentControlContract(const py::dict& fields)
{
  if (fields.empty()) return std::nullopt;
  Contract contract;
  for (const auto& item : fields) {
    contract.setField(py::cast<std::string>(item.first),
                      py::cast<std::string>(item.second));
  }
  return contract;
}

py::dict
makeAckRequestContext(const nsf::RequestMessage& request)
{
  auto fieldsToDict = [] (const auto& fields) {
    py::dict output;
    for (const auto& field : fields) output[field.first.c_str()] = field.second;
    return output;
  };
  py::dict context;
  context["user_token"] = request.getUserToken();
  context["strategy"] = request.getStrategy();
  context["request_mode"] = request.getRequestMode();
  context["target_provider"] = request.getTargetProvider().toUri();
  context["request_capabilities"] = request.hasRequestCapabilities() ?
    fieldsToDict(request.getRequestCapabilities().getFields()) : py::dict();
  context["deployment_intent"] = request.hasDeploymentIntent() ?
    fieldsToDict(request.getDeploymentIntent().getFields()) : py::dict();
  context["encrypted_request_input"] = request.hasEncryptedRequestInput() ?
    fieldsToDict(request.getEncryptedRequestInput().getFields()) : py::dict();
  return context;
}

struct PyAckCandidate
{
  std::string providerName;
  std::string serviceName;
  std::string requestId;
  bool status = false;
  std::string message;
  py::bytes payload;
  py::object telemetry = py::none();
};

struct PyCollaborationAckClosure
{
  std::string requestId;
  std::vector<PyAckCandidate> candidates;
  std::string digest;
  uint64_t closedAtUs = 0;
  uint64_t requestDeadlineUs = 0;
};

struct PyLargeDataPublishResult
{
  bool success = false;
  std::string encryptedDataName;
  std::string objectId;
  std::string error;
};

struct PySignedAppDataResult
{
  bool success = false;
  std::string dataName;
  std::string signerCertificate;
  py::bytes payload;
  std::string error;
};

struct PyDataPacket
{
  std::string name;
  uint64_t segment = 0;
  py::bytes wire;
  py::bytes content;
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
  packet.content = py::bytes(
    reinterpret_cast<const char*>(data.getContent().value()),
    data.getContent().value_size());
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

PyDataPacket
decodeDataPacket(const py::bytes& wireBytes)
{
  return toPyDataPacket(*dataFromWireBytes(wireBytes));
}

bool
verifyDataPacketSignature(const py::bytes& wireBytes, const py::bytes& publicKeyDer)
{
  const std::string key = publicKeyDer;
  return ndn::security::verifySignature(
    *dataFromWireBytes(wireBytes),
    ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(key.data()), key.size()));
}

bool
verifyDetachedSha256Signature(const py::bytes& payloadBytes,
                              const py::bytes& signatureBytes,
                              const py::bytes& publicKeyDer)
{
  const std::string payload = payloadBytes;
  const std::string signature = signatureBytes;
  const std::string key = publicKeyDer;
  const ndn::InputBuffers inputs{
    ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size())
  };
  return ndn::security::verifySignature(
    inputs,
    ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(signature.data()), signature.size()),
    ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(key.data()), key.size()));
}

bool
verifyDataPacketDigest(const py::bytes& wireBytes)
{
  return ndn::security::verifySignature(
    *dataFromWireBytes(wireBytes),
    std::optional<ndn::security::Certificate>{});
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
    getOrCreateIdentity(m_keyChain, nfdCommandIdentity());

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
      [this] (const ndn::Name& prefix, const std::string& reason) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = "failed to register stored Data prefix " + prefix.toUri() +
                  ": " + reason;
      },
      ndn::security::SigningInfo(
        ndn::security::SigningInfo::SIGNER_TYPE_ID,
        nfdCommandIdentity()));

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

class NativeFileSegmentedObjectProducer
{
public:
  NativeFileSegmentedObjectProducer(const std::string& baseName,
                                    const std::string& filePath,
                                    const std::string& signingIdentity,
                                    size_t maxSegmentSize,
                                    int freshnessMs,
                                    bool digestSigning)
    : m_baseName(baseName)
    , m_filePath(filePath)
    , m_maxSegmentSize(maxSegmentSize)
    , m_freshnessMs(freshnessMs)
    , m_digestSigning(digestSigning)
  {
    if (m_maxSegmentSize == 0) {
      throw std::invalid_argument("file segmented producer max segment size must be positive");
    }
    std::ifstream stream(m_filePath, std::ios::binary | std::ios::ate);
    if (!stream) {
      throw std::runtime_error("cannot open file segmented producer payload: " + m_filePath);
    }
    const auto end = stream.tellg();
    if (end < 0) {
      throw std::runtime_error("cannot determine file segmented producer payload size");
    }
    m_fileSize = static_cast<uint64_t>(end);
    // The producer serves one NDN segment per Interest.  Re-opening the
    // payload for every Interest turns a large artifact into millions of
    // open/seek/close cycles and defeats the bounded streaming design.  Keep
    // one descriptor for the producer thread and seek it for each segment.
    m_fileStream.open(m_filePath, std::ios::binary);
    if (!m_fileStream) {
      throw std::runtime_error("cannot open file segmented producer stream: " + m_filePath);
    }
    m_segmentCount = std::max<uint64_t>(
      1, (m_fileSize + m_maxSegmentSize - 1) / m_maxSegmentSize);

    m_signingIdentity = signingIdentity.empty() ?
      ndn::Name("/ndnsf/python/file-segmented-producer") : ndn::Name(signingIdentity);
    getOrCreateIdentity(m_keyChain, m_signingIdentity);
    getOrCreateIdentity(m_keyChain, nfdCommandIdentity());
    const auto certificate = m_keyChain.getPib()
      .getIdentity(m_signingIdentity)
      .getDefaultKey()
      .getDefaultCertificate();
    const auto publicKey = certificate.getPublicKey();
    m_publicKeyDer.assign(publicKey.begin(), publicKey.end());
    m_versionedName = m_baseName;
    m_versionedName.appendVersion(static_cast<uint64_t>(
      ndn::time::toUnixTimestamp(ndn::time::system_clock::now()).count()));
  }

  ~NativeFileSegmentedObjectProducer()
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
    {
      std::lock_guard<std::mutex> lock(m_registrationMutex);
      m_registrationComplete = false;
      m_registrationSucceeded = false;
      m_registrationError.clear();
    }
    m_face.setInterestFilter(
      m_baseName,
      [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        this->serveInterest(interest);
      },
      [this] (const ndn::Name&) {
        {
          std::lock_guard<std::mutex> lock(m_registrationMutex);
          m_registrationComplete = true;
          m_registrationSucceeded = true;
        }
        m_registrationCv.notify_all();
      },
      [this] (const ndn::Name& prefix, const std::string& reason) {
        const auto error = "failed to register file Data prefix " +
                           prefix.toUri() + ": " + reason;
        {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = error;
        }
        {
          std::lock_guard<std::mutex> lock(m_registrationMutex);
          m_registrationComplete = true;
          m_registrationSucceeded = false;
          m_registrationError = error;
        }
        m_registrationCv.notify_all();
      },
      ndn::security::SigningInfo(
        ndn::security::SigningInfo::SIGNER_TYPE_ID,
        nfdCommandIdentity()));
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, ndn::time::milliseconds(50));
        }
        catch (const std::exception& e) {
          {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            m_error = e.what();
          }
          bool notifyRegistration = false;
          {
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            if (!m_registrationComplete) {
              m_registrationComplete = true;
              m_registrationSucceeded = false;
              m_registrationError = e.what();
              notifyRegistration = true;
            }
          }
          if (notifyRegistration) {
            m_registrationCv.notify_all();
          }
        }
      }
    });
    std::unique_lock<std::mutex> lock(m_registrationMutex);
    const bool completed = m_registrationCv.wait_for(
      lock, std::chrono::seconds(5),
      [this] { return m_registrationComplete; });
    const bool succeeded = completed && m_registrationSucceeded;
    const std::string registrationError = completed ?
      m_registrationError : "timed out registering file Data prefix " +
                            m_baseName.toUri();
    lock.unlock();
    if (!succeeded) {
      {
        std::lock_guard<std::mutex> errorLock(m_errorMutex);
        m_error = registrationError;
      }
      stop();
      throw std::runtime_error(registrationError);
    }
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

  std::string baseName() const { return m_baseName.toUri(); }
  std::string versionedName() const { return m_versionedName.toUri(); }
  uint64_t segmentCount() const { return m_segmentCount; }
  uint64_t fileSize() const { return m_fileSize; }
  uint64_t dataCount() const { return m_dataCount.load(); }
  uint64_t wireBytes() const { return m_wireBytes.load(); }
  double signingMs() const
  {
    return static_cast<double>(m_signingNanoseconds.load()) / 1000000.0;
  }
  py::bytes publicKeyDer() const
  {
    return py::bytes(
      reinterpret_cast<const char*>(m_publicKeyDer.data()), m_publicKeyDer.size());
  }
  std::string error() const
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
  }

private:
  void
  serveInterest(const ndn::Interest& interest)
  {
    uint64_t segmentNo = 0;
    const auto& interestName = interest.getName();
    if (!interestName.empty() && interestName[-1].isSegment()) {
      segmentNo = interestName[-1].toSegment();
    }
    if (segmentNo >= m_segmentCount) {
      return;
    }

    const uint64_t offset = segmentNo * m_maxSegmentSize;
    const size_t length = static_cast<size_t>(
      std::min<uint64_t>(m_maxSegmentSize, m_fileSize - std::min(offset, m_fileSize)));
    std::vector<uint8_t> content(length);
    if (length > 0) {
      m_fileStream.clear();
      m_fileStream.seekg(static_cast<std::streamoff>(offset));
      m_fileStream.read(reinterpret_cast<char*>(content.data()),
                        static_cast<std::streamsize>(length));
      if (static_cast<size_t>(m_fileStream.gcount()) != length) {
        throw std::runtime_error("short read from file segmented producer payload");
      }
    }

    ndn::Name dataName = m_versionedName;
    dataName.appendSegment(segmentNo);
    ndn::Data data(dataName);
    data.setFreshnessPeriod(ndn::time::milliseconds(m_freshnessMs));
    data.setFinalBlock(ndn::name::Component::fromSegment(m_segmentCount - 1));
    data.setContent(content);
    const auto signingStarted = std::chrono::steady_clock::now();
    if (m_digestSigning) {
      m_keyChain.sign(
        data,
        ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_SHA256));
    }
    else {
      m_keyChain.sign(
        data,
        ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                   m_signingIdentity));
    }
    const auto signingElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - signingStarted).count();
    const auto wire = data.wireEncode();
    m_signingNanoseconds.fetch_add(static_cast<uint64_t>(signingElapsed));
    m_wireBytes.fetch_add(wire.size());
    m_dataCount.fetch_add(1);
    m_face.put(data);
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_baseName;
  std::string m_filePath;
  std::ifstream m_fileStream;
  ndn::Name m_versionedName;
  ndn::Name m_signingIdentity;
  size_t m_maxSegmentSize;
  int m_freshnessMs;
  bool m_digestSigning;
  uint64_t m_fileSize = 0;
  uint64_t m_segmentCount = 0;
  std::atomic<uint64_t> m_dataCount{0};
  std::atomic<uint64_t> m_wireBytes{0};
  std::atomic<uint64_t> m_signingNanoseconds{0};
  std::vector<uint8_t> m_publicKeyDer;
  std::atomic_bool m_running{false};
  std::thread m_thread;
  mutable std::mutex m_errorMutex;
  std::string m_error;
  std::mutex m_registrationMutex;
  std::condition_variable m_registrationCv;
  bool m_registrationComplete = false;
  bool m_registrationSucceeded = false;
  std::string m_registrationError;
};

class NativeWireDataProducer
{
public:
  NativeWireDataProducer(const std::string& baseName,
                         const std::vector<py::bytes>& packetWires,
                         const std::string& signingIdentity,
                         const std::vector<std::string>& forwardingRoutePrefixes)
    : m_baseName(baseName)
  {
    m_signingIdentity = signingIdentity.empty() ?
      ndn::Name("/ndnsf/python/stored-data-producer") : ndn::Name(signingIdentity);
    getOrCreateIdentity(m_keyChain, m_signingIdentity);
    for (const auto& prefix : forwardingRoutePrefixes) {
      if (!prefix.empty()) {
        m_forwardingRoutePrefixes.emplace_back(prefix);
      }
    }
    for (const auto& packetWire : packetWires) {
      auto data = dataFromWireBytes(packetWire);
      if (!m_baseName.isPrefixOf(data->getName())) {
        throw std::invalid_argument("stored Data name is outside producer prefix: " +
                                    data->getName().toUri());
      }
      const auto inserted = m_packetsByName.emplace(data->getName(), data);
      if (!inserted.second && inserted.first->second->wireEncode() != data->wireEncode()) {
        throw std::invalid_argument("conflicting stored Data wire for name: " +
                                    data->getName().toUri());
      }
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
    for (const auto& prefix : m_forwardingRoutePrefixes) {
      m_face.registerPrefix(
        prefix,
        [] (const ndn::Name&) {},
        [this] (const ndn::Name& failedPrefix, const std::string& reason) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = "stored Data forwarding route registration failed for " +
                    failedPrefix.toUri() + ": " + reason;
        });
    }
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
    return m_packetsByName.size();
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
    if (m_packetsByName.empty()) {
      return;
    }
    const auto& name = interest.getName();
    const auto exact = m_packetsByName.find(name);
    if (exact != m_packetsByName.end()) {
      m_face.put(*exact->second);
      return;
    }
    if (interest.getCanBePrefix()) {
      for (const auto& item : m_packetsByName) {
        if (name.isPrefixOf(item.first)) {
          m_face.put(*item.second);
          return;
        }
      }
    }
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_baseName;
  ndn::Name m_signingIdentity;
  std::map<ndn::Name, std::shared_ptr<ndn::Data>> m_packetsByName;
  std::map<uint64_t, std::shared_ptr<ndn::Data>> m_segments;
  std::vector<ndn::Name> m_forwardingRoutePrefixes;
  std::atomic_bool m_running{false};
  std::thread m_thread;
  mutable std::mutex m_errorMutex;
  std::string m_error;
};

class NativeRepoDataPlaneProducer
{
public:
  NativeRepoDataPlaneProducer(py::function lookup,
                              const std::string& signingIdentity,
                              const std::vector<std::string>& forwardingRoutePrefixes)
    : m_lookup(keepPyFunction(std::move(lookup)))
  {
    m_signingIdentity = signingIdentity.empty() ?
      ndn::Name("/ndnsf/python/repo-data-plane") : ndn::Name(signingIdentity);
    getOrCreateIdentity(m_keyChain, m_signingIdentity);
    for (const auto& prefix : forwardingRoutePrefixes) {
      if (!prefix.empty()) {
        m_forwardingRoutePrefixes.emplace_back(prefix);
      }
    }
  }

  ~NativeRepoDataPlaneProducer()
  {
    stop();
  }

  void
  activatePrefix(const std::string& prefixText)
  {
    const ndn::Name prefix(prefixText);
    bool inserted = false;
    {
      std::lock_guard<std::mutex> lock(m_prefixMutex);
      inserted = m_prefixes.insert(prefix).second;
    }
    if (!inserted) {
      return;
    }
    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), [this, prefix] {
        this->registerDataPrefix(prefix);
      });
    }
  }

  void
  start()
  {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
      return;
    }
    for (const auto& prefix : m_forwardingRoutePrefixes) {
      m_face.registerPrefix(
        prefix,
        [] (const ndn::Name&) {},
        [this] (const ndn::Name& failedPrefix, const std::string& reason) {
          this->setError("repo data-plane route registration failed for " +
                         failedPrefix.toUri() + ": " + reason);
        });
    }
    std::vector<ndn::Name> prefixes;
    {
      std::lock_guard<std::mutex> lock(m_prefixMutex);
      prefixes.assign(m_prefixes.begin(), m_prefixes.end());
    }
    for (const auto& prefix : prefixes) {
      registerDataPrefix(prefix);
    }
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, ndn::time::milliseconds(25));
        }
        catch (const std::exception& e) {
          setError(e.what());
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
  activePrefixCount() const
  {
    std::lock_guard<std::mutex> lock(m_prefixMutex);
    return m_prefixes.size();
  }

  uint64_t interestCount() const { return m_interestCount.load(); }
  uint64_t hitCount() const { return m_hitCount.load(); }
  uint64_t missCount() const { return m_missCount.load(); }
  size_t threadCount() const { return m_running.load() ? 1 : 0; }

  std::string
  error() const
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
  }

private:
  void
  setError(const std::string& error)
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_error = error;
  }

  void
  registerDataPrefix(const ndn::Name& prefix)
  {
    m_face.setInterestFilter(
      prefix,
      [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        this->serveInterest(interest);
      },
      [] (const ndn::Name&) {},
      [this] (const ndn::Name& failedPrefix, const std::string& reason) {
        this->setError("repo data-plane prefix registration failed for " +
                       failedPrefix.toUri() + ": " + reason);
      },
      ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                 m_signingIdentity));
  }

  void
  serveInterest(const ndn::Interest& interest)
  {
    ++m_interestCount;
    try {
      py::gil_scoped_acquire gil;
      py::object result = (*m_lookup)(
        interest.getName().toUri(), interest.getCanBePrefix());
      if (result.is_none()) {
        ++m_missCount;
        return;
      }
      auto data = dataFromWireBytes(result.cast<py::bytes>());
      const bool nameMatches =
        interest.getName() == data->getName() ||
        (interest.getCanBePrefix() && interest.getName().isPrefixOf(data->getName()));
      if (!nameMatches) {
        ++m_missCount;
        setError("repo data-plane callback returned mismatched Data name " +
                 data->getName().toUri());
        return;
      }
      m_face.put(*data);
      ++m_hitCount;
    }
    catch (const std::exception& e) {
      ++m_missCount;
      setError(e.what());
    }
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_signingIdentity;
  PyFunctionPtr m_lookup;
  std::vector<ndn::Name> m_forwardingRoutePrefixes;
  mutable std::mutex m_prefixMutex;
  std::set<ndn::Name> m_prefixes;
  std::atomic_bool m_running{false};
  std::thread m_thread;
  std::atomic_uint64_t m_interestCount{0};
  std::atomic_uint64_t m_hitCount{0};
  std::atomic_uint64_t m_missCount{0};
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

// Convenience/test signer only. Production predictive streams should let the
// application sign with its own ndn-python (or another NDN) keychain and pass
// the resulting exact wire to StreamPublisher::push().
py::bytes
makeSignedData(const std::string& name,
               const py::bytes& content,
               const std::string& signingIdentity,
               int freshnessMs)
{
  if (freshnessMs < 0) {
    throw std::invalid_argument("freshness_ms must be nonnegative");
  }
  ndn::KeyChain keyChain;
  const auto identityName = signingIdentity.empty() ?
    ndn::Name("/ndnsf/python/signed-data") : ndn::Name(signingIdentity);
  getOrCreateIdentity(keyChain, identityName);

  ndn::Data data(ndn::Name{name});
  data.setFreshnessPeriod(ndn::time::milliseconds(freshnessMs));
  const std::string bytes = content;
  data.setContent(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  keyChain.sign(data,
    ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                               identityName));
  const auto wire = data.wireEncode();
  return py::bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
}

std::string
makePredictiveDataNameUri(const std::string& mappingRoot,
                          uint64_t mappingVersion,
                          uint64_t sequence)
{
  return nsf::makePredictiveDataName(
           ndn::Name(mappingRoot), mappingVersion, sequence).toUri();
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

struct PyAdaptiveSegmentFetchResult
{
  uint64_t totalSegments = 0;
  uint64_t deliveredSegments = 0;
  uint64_t interestCount = 0;
  uint64_t retransmissionCount = 0;
  uint64_t duplicateCount = 0;
  uint64_t timeoutCount = 0;
  uint64_t logicalBytes = 0;
  uint64_t dataWireBytes = 0;
  uint64_t interestWireBytes = 0;
  uint64_t wireBytes = 0;
  uint64_t retransmittedBytes = 0;
  uint64_t maximumInFlight = 0;
  double finalWindow = 0.0;
};

PyAdaptiveSegmentFetchResult
fetchAdaptiveSegmentedDataPackets(
  const std::string& baseName,
  int timeoutMs,
  int interestLifetimeMs,
  uint32_t initialWindow,
  uint32_t maximumWindow,
  uint32_t maximumRetries,
  uint32_t persistenceBacklogLimit,
  const std::vector<std::string>& forwardingHints,
  py::function onPacket)
{
  if (timeoutMs <= 0 || interestLifetimeMs <= 0 || initialWindow == 0 ||
      maximumWindow < initialWindow || persistenceBacklogLimit == 0 ||
      !onPacket) {
    throw std::invalid_argument(
      "adaptive segmented fetch requires bounded positive options and callback");
  }
  ndn::Face face;
  const auto deadline =
    ndn::time::steady_clock::now() + ndn::time::milliseconds(timeoutMs);
  std::vector<ndn::Name> hintNames;
  hintNames.reserve(forwardingHints.size());
  for (const auto& hint : forwardingHints) {
    hintNames.emplace_back(hint);
  }

  ndn::Interest firstInterest{ndn::Name(baseName)};
  firstInterest.setCanBePrefix(true);
  firstInterest.setMustBeFresh(false);
  firstInterest.setInterestLifetime(ndn::time::milliseconds(interestLifetimeMs));
  if (!hintNames.empty()) {
    firstInterest.setForwardingHint(hintNames);
  }
  std::optional<PyDataPacket> first;
  std::string firstError;
  uint32_t firstAttempts = 0;
  while (!first && firstAttempts <= maximumRetries &&
         ndn::time::steady_clock::now() < deadline) {
    ++firstAttempts;
    try {
      first = fetchOneDataPacket(face, firstInterest, deadline);
    }
    catch (const std::exception& error) {
      firstError = error.what();
    }
  }
  if (!first) {
    throw std::runtime_error(firstError.empty() ?
      "adaptive segmented fetch failed before first Data" : firstError);
  }
  const auto firstData = dataFromWireBytes(first->wire);
  const auto finalBlock = firstData->getFinalBlock();
  if (!finalBlock || !finalBlock->isSegment()) {
    throw std::runtime_error(
      "First adaptive segmented Data has no segment FinalBlockId: " + first->name);
  }
  const uint64_t finalSegment = finalBlock->toSegment();
  if (finalSegment >= (uint64_t{1} << 32) || first->segment > finalSegment) {
    throw std::runtime_error(
      "adaptive segmented fetch exceeds segment-count safety bound");
  }
  const uint64_t totalSegments = finalSegment + 1;
  const auto versionedName = firstData->getName().getPrefix(-1);

  PyAdaptiveSegmentFetchResult result;
  result.totalSegments = totalSegments;
  result.interestCount = firstAttempts;
  result.retransmissionCount = firstAttempts - 1;
  result.maximumInFlight = 1;
  result.logicalBytes = firstData->getContent().value_size();
  result.dataWireBytes = py::cast<std::string>(first->wire).size();
  result.interestWireBytes = firstInterest.wireEncode().size() * firstAttempts;
  result.wireBytes = result.dataWireBytes + result.interestWireBytes;
  result.retransmittedBytes =
    firstInterest.wireEncode().size() * (firstAttempts - 1);
  if (firstAttempts > 1) {
    result.retransmittedBytes += result.dataWireBytes;
  }
  onPacket(*first);
  result.deliveredSegments = 1;

  enum SegmentState : uint8_t {
    Missing,
    InFlight,
    Delivered,
  };
  std::vector<uint8_t> states(static_cast<size_t>(totalSegments), Missing);
  std::vector<uint32_t> attempts(static_cast<size_t>(totalSegments), 0);
  states[first->segment] = Delivered;
  attempts[first->segment] = firstAttempts;
  std::deque<uint64_t> retryQueue;
  uint64_t nextNew = 0;
  uint64_t inFlight = 0;
  double congestionWindow = initialWindow;
  std::string error;

  auto nextSegment = [&] () -> std::optional<uint64_t> {
    while (!retryQueue.empty()) {
      const uint64_t segmentNo = retryQueue.front();
      retryQueue.pop_front();
      if (states[segmentNo] == Missing) {
        return segmentNo;
      }
    }
    while (nextNew < totalSegments) {
      const uint64_t candidate = nextNew++;
      if (states[candidate] == Missing) {
        return candidate;
      }
    }
    return std::nullopt;
  };

  auto schedule = [&] {
    const uint64_t windowBound = std::min<uint64_t>(
      static_cast<uint64_t>(std::floor(congestionWindow)),
      persistenceBacklogLimit);
    while (error.empty() && inFlight < windowBound) {
      const auto segment = nextSegment();
      if (!segment) {
        break;
      }
      const uint64_t segmentNo = *segment;
      states[segmentNo] = InFlight;
      ++attempts[segmentNo];
      ++inFlight;
      ++result.interestCount;
      if (attempts[segmentNo] > 1) {
        ++result.retransmissionCount;
      }
      result.maximumInFlight = std::max(result.maximumInFlight, inFlight);

      ndn::Name segmentName(versionedName);
      segmentName.appendSegment(segmentNo);
      ndn::Interest interest(segmentName);
      interest.setCanBePrefix(false);
      interest.setMustBeFresh(false);
      interest.setInterestLifetime(
        ndn::time::milliseconds(interestLifetimeMs));
      if (!hintNames.empty()) {
        interest.setForwardingHint(hintNames);
      }
      const auto interestWireBytes = interest.wireEncode().size();
      result.interestWireBytes += interestWireBytes;
      result.wireBytes += interestWireBytes;
      if (attempts[segmentNo] > 1) {
        result.retransmittedBytes += interestWireBytes;
      }
      face.expressInterest(
        interest,
        [&, segmentNo] (const ndn::Interest&, const ndn::Data& data) {
          if (states[segmentNo] != InFlight) {
            ++result.duplicateCount;
            return;
          }
          --inFlight;
          const auto packet = toPyDataPacket(data);
          if (packet.segment != segmentNo ||
              data.getName().getPrefix(-1) != versionedName) {
            error = "received substituted segment";
            return;
          }
          try {
            // Synchronous callback completion is persistence backpressure:
            // no replacement Interest is scheduled until it returns.
            onPacket(packet);
          }
          catch (const py::error_already_set& exception) {
            error = std::string("packet callback failed: ") + exception.what();
            return;
          }
          states[segmentNo] = Delivered;
          ++result.deliveredSegments;
          result.logicalBytes += data.getContent().value_size();
          const auto packetWireBytes =
            py::cast<std::string>(packet.wire).size();
          result.dataWireBytes += packetWireBytes;
          result.wireBytes += packetWireBytes;
          if (attempts[segmentNo] > 1) {
            result.retransmittedBytes += packetWireBytes;
          }
          congestionWindow = std::min<double>(
            maximumWindow,
            congestionWindow + 1.0 / std::max(1.0, congestionWindow));
        },
        [&, segmentNo] (const ndn::Interest&, const ndn::lp::Nack&) {
          if (states[segmentNo] != InFlight) {
            return;
          }
          --inFlight;
          states[segmentNo] = Missing;
          congestionWindow = std::max(1.0, std::floor(congestionWindow / 2.0));
          if (attempts[segmentNo] > maximumRetries) {
            error = "retry budget exhausted after Nack";
          }
          else {
            retryQueue.push_back(segmentNo);
          }
        },
        [&, segmentNo] (const ndn::Interest&) {
          if (states[segmentNo] != InFlight) {
            return;
          }
          --inFlight;
          ++result.timeoutCount;
          states[segmentNo] = Missing;
          congestionWindow = std::max(1.0, std::floor(congestionWindow / 2.0));
          if (attempts[segmentNo] > maximumRetries) {
            error = "retry budget exhausted after timeout";
          }
          else {
            retryQueue.push_back(segmentNo);
          }
        });
    }
  };

  while (result.deliveredSegments < totalSegments && error.empty() &&
         ndn::time::steady_clock::now() < deadline) {
    schedule();
    processFaceEvents(face, pythonFacePollTimeout());
  }
  if (result.deliveredSegments != totalSegments && error.empty()) {
    error = "operation deadline reached";
  }
  if (!error.empty()) {
    throw std::runtime_error(
      "adaptive segmented fetch failed for " + baseName + ": " + error);
  }
  result.finalWindow = congestionWindow;
  return result;
}

PyDataPacket
fetchExactDataPacket(const std::string& dataName,
                     int timeoutMs,
                     int interestLifetimeMs,
                     const std::vector<std::string>& forwardingHints)
{
  ndn::Face face;
  const auto deadline = ndn::time::steady_clock::now() +
    ndn::time::milliseconds(timeoutMs);
  std::vector<ndn::Name> hintNames;
  for (const auto& hint : forwardingHints) {
    hintNames.emplace_back(hint);
  }
  auto packet = fetchOneDataPacketWithHintFallback(
    face,
    ndn::Name(dataName),
    false,
    ndn::time::milliseconds(interestLifetimeMs),
    deadline,
    hintNames);
  if (packet.name != dataName) {
    throw std::runtime_error("exact Data fetch name mismatch: requested=" + dataName +
                             " received=" + packet.name);
  }
  return packet;
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
  std::string selectionDigest;
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

py::dict
selectionStatusToDict(const nsf::SelectionExecutionStatus& status)
{
  py::list members;
  for (const auto& member : status.memberStatuses) {
    py::dict value;
    value["providerName"] = member.providerName.toUri();
    value["serviceName"] = member.serviceName.toUri();
    value["requestId"] = member.requestId.toUri();
    value["selectionDigest"] = member.selectionDigest;
    value["role"] = member.role;
    value["operationId"] = member.operationId;
    value["operation"] = member.operation;
    value["state"] = member.state;
    value["reasonCode"] = member.reasonCode;
    value["message"] = member.message;
    value["attempt"] = member.attempt;
    value["epoch"] = member.epoch;
    value["sequence"] = member.sequence;
    value["progressKnown"] = member.progressKnown;
    value["progress"] = member.progress;
    value["createdAtMs"] = member.createdAtMs;
    value["updatedAtMs"] = member.updatedAtMs;
    value["expiresAtMs"] = member.expiresAtMs;
    value["detailsSchema"] = member.detailsSchema;
    value["detailsPayload"] = toPyBytes(member.detailsPayload);
    members.append(std::move(value));
  }
  py::dict output;
  output["providerName"] = status.providerName.toUri();
  output["serviceName"] = status.serviceName.toUri();
  output["requestId"] = status.requestId.toUri();
  output["selectionDigest"] = status.selectionDigest;
  output["state"] = nsf::selectionExecutionStateToString(status.state);
  output["message"] = status.message;
  output["responseName"] = status.responseName.toUri();
  output["receivedAtUs"] = status.receivedAtUs;
  output["queuedAtUs"] = status.queuedAtUs;
  output["runningAtUs"] = status.runningAtUs;
  output["completedAtUs"] = status.completedAtUs;
  output["updatedAtUs"] = status.updatedAtUs;
  output["decisionReceipt"] = toPyBytes(status.decisionReceipt);
  output["memberStatuses"] = std::move(members);
  return output;
}

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

std::string
decodeBase64Url(const std::string& encoded)
{
  auto valueOf = [] (char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-' || ch == '+') return 62;
    if (ch == '_' || ch == '/') return 63;
    return -1;
  };
  std::string decoded;
  int bits = 0;
  int bitCount = 0;
  for (const char ch : encoded) {
    if (ch == '=') {
      break;
    }
    const int value = valueOf(ch);
    if (value < 0) {
      throw std::invalid_argument("malformed typed provider capability base64");
    }
    bits = (bits << 6) | value;
    bitCount += 6;
    if (bitCount >= 8) {
      bitCount -= 8;
      decoded.push_back(static_cast<char>((bits >> bitCount) & 0xff));
    }
  }
  return decoded;
}

std::optional<boost::property_tree::ptree>
providerCapabilityFromAckPayload(const ndn::Buffer& payload)
{
  const auto text = bytesToString(payload);
  const auto encoded = fieldFromText(text, "providerCapabilityHint");
  if (encoded.empty()) {
    return std::nullopt;
  }
  boost::property_tree::ptree root;
  std::istringstream input(decodeBase64Url(encoded.rfind("json64:", 0) == 0 ?
                                           encoded.substr(7) : encoded));
  try {
    boost::property_tree::read_json(input, root);
  }
  catch (const std::exception& exc) {
    throw std::invalid_argument(
      std::string("malformed typed provider capability JSON: ") + exc.what());
  }
  const auto schema = root.get<std::string>("schema", "ndnsf-provider-capability-v1");
  if (schema != "ndnsf-provider-capability-v1" &&
      schema != "ndnsf-provider-capability-v2") {
    throw std::invalid_argument("unknown typed provider capability schema: " + schema);
  }
  if (!root.get_child_optional("servicePayload")) {
    if (const auto snakePayload = root.get_child_optional("service_payload")) {
      // Python dataclass producers intentionally serialize public fields in
      // snake_case, while the native producer emits the wire-contract
      // camelCase spelling. Normalize both at this language boundary.
      root.put_child("servicePayload", *snakePayload);
    }
  }
  if (!root.get_child_optional("servicePayload")) {
    throw std::invalid_argument("typed provider capability has no servicePayload");
  }
  return root;
}

bool
mixedAckReaderEnabled()
{
  const char* value = std::getenv("NDNSF_ACK_COMPATIBILITY_MODE");
  return value != nullptr && std::string(value) == "mixed";
}

std::string
typedServiceString(const boost::property_tree::ptree& root, const std::string& key)
{
  const auto path = "servicePayload." + key;
  if (const auto scalar = root.get_optional<std::string>(path)) {
    return *scalar;
  }
  if (const auto child = root.get_child_optional(path)) {
    std::ostringstream values;
    bool first = true;
    for (const auto& item : *child) {
      if (!first) values << ',';
      values << item.second.get_value<std::string>();
      first = false;
    }
    return values.str();
  }
  return "";
}

double
typedServiceNumber(const boost::property_tree::ptree& root,
                   const std::string& key,
                   const std::string& runtimeKey = "")
{
  if (!runtimeKey.empty()) {
    if (const auto value = root.get_optional<double>("runtimeHint." + runtimeKey)) {
      return *value;
    }
  }
  return root.get<double>("servicePayload." + key, 0.0);
}

std::vector<std::string>
rolesFromAckPayload(const ndn::Buffer& payload)
{
  if (const auto capability = providerCapabilityFromAckPayload(payload)) {
    auto roles = splitTextList(typedServiceString(*capability, "roles"));
    if (!roles.empty()) {
      return roles;
    }
    auto role = typedServiceString(*capability, "role");
    return role.empty() ? std::vector<std::string>{} : std::vector<std::string>{role};
  }
  if (!mixedAckReaderEnabled()) {
    return {};
  }
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

double
numericFieldFromText(const std::string& text, const std::string& key)
{
  const auto value = fieldFromText(text, key);
  if (value.empty()) {
    return 0.0;
  }
  try {
    return std::stod(value);
  }
  catch (...) {
    return 0.0;
  }
}

struct CapacityAckScore
{
  double pendingWork = 0.0;
  double readyQueue = 0.0;
  double waitingInputs = 0.0;
  double activeWorkers = 0.0;
  double idleWorkers = 0.0;
  double workers = 0.0;
};

double
capacityPressure(const CapacityAckScore& score)
{
  const auto componentPressure =
    score.readyQueue + score.waitingInputs + score.activeWorkers;
  if (score.pendingWork > 0.0 && componentPressure > 0.0) {
    return score.pendingWork;
  }
  if (score.pendingWork > 0.0) {
    return score.pendingWork;
  }
  return componentPressure;
}

CapacityAckScore
capacityScoreFromAckPayload(const ndn::Buffer& payload)
{
  if (const auto capability = providerCapabilityFromAckPayload(payload)) {
    return CapacityAckScore{
      typedServiceNumber(*capability, "queue", "queueLength"),
      typedServiceNumber(*capability, "readyQueue"),
      typedServiceNumber(*capability, "waitingInputs"),
      typedServiceNumber(*capability, "activeWorkers", "activeWorkCount"),
      typedServiceNumber(*capability, "idleWorkers"),
      typedServiceNumber(*capability, "workers"),
    };
  }
  if (!mixedAckReaderEnabled()) {
    return {};
  }
  const auto text = bytesToString(payload);
  return CapacityAckScore{
    numericFieldFromText(text, "queue"),
    numericFieldFromText(text, "readyQueue"),
    numericFieldFromText(text, "waitingInputs"),
    numericFieldFromText(text, "activeWorkers"),
    numericFieldFromText(text, "idleWorkers"),
    numericFieldFromText(text, "workers"),
  };
}

bool
isBetterCapacityAck(const nsf::AckCandidate& current,
                    const nsf::AckCandidate& best,
                    const std::map<std::string, size_t>& providerAssignments,
                    const std::map<std::string, size_t>& admissionBias)
{
  const auto currentProvider = current.providerName.toUri();
  const auto bestProvider = best.providerName.toUri();
  const auto currentAssignments =
    (providerAssignments.count(currentProvider) ? providerAssignments.at(currentProvider) : 0) +
    (admissionBias.count(currentProvider) ? admissionBias.at(currentProvider) : 0);
  const auto bestAssignments =
    (providerAssignments.count(bestProvider) ? providerAssignments.at(bestProvider) : 0) +
    (admissionBias.count(bestProvider) ? admissionBias.at(bestProvider) : 0);
  if (currentAssignments != bestAssignments) {
    return currentAssignments < bestAssignments;
  }

  const auto currentScore = capacityScoreFromAckPayload(current.ack.getPayload());
  const auto bestScore = capacityScoreFromAckPayload(best.ack.getPayload());

  const auto currentPressure = capacityPressure(currentScore);
  const auto bestPressure = capacityPressure(bestScore);
  if (currentPressure != bestPressure) {
    return currentPressure < bestPressure;
  }
  if (currentScore.readyQueue != bestScore.readyQueue) {
    return currentScore.readyQueue < bestScore.readyQueue;
  }
  if (currentScore.waitingInputs != bestScore.waitingInputs) {
    return currentScore.waitingInputs < bestScore.waitingInputs;
  }
  if (currentScore.activeWorkers != bestScore.activeWorkers) {
    return currentScore.activeWorkers < bestScore.activeWorkers;
  }
  if (currentScore.idleWorkers != bestScore.idleWorkers) {
    return currentScore.idleWorkers > bestScore.idleWorkers;
  }
  if (currentScore.workers != bestScore.workers) {
    return currentScore.workers > bestScore.workers;
  }
  return false;
}

std::map<std::string, size_t>
admissionBiasFromEnv()
{
  std::map<std::string, size_t> output;
  const char* raw = std::getenv("NDNSF_COLLAB_ADMISSION_BIAS");
  if (raw == nullptr || *raw == '\0') {
    return output;
  }
  const std::string text(raw);
  size_t begin = 0;
  while (begin <= text.size()) {
    const auto end = text.find(';', begin);
    const auto item = text.substr(
      begin,
      end == std::string::npos ? std::string::npos : end - begin);
    const auto delimiter = item.rfind('=');
    if (delimiter != std::string::npos && delimiter > 0) {
      const auto provider = item.substr(0, delimiter);
      try {
        const auto value = std::stoul(item.substr(delimiter + 1));
        output[provider] = static_cast<size_t>(value);
      }
      catch (...) {
      }
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return output;
}

std::string
preferredProviderForRole(const std::map<std::string, std::string>& preferences,
                         const std::string& role)
{
  auto found = preferences.find(role);
  if (found != preferences.end()) {
    return found->second;
  }
  if (!role.empty() && role.front() == '/') {
    found = preferences.find(role.substr(1));
    if (found != preferences.end()) {
      return found->second;
    }
  }
  else if (!role.empty()) {
    found = preferences.find("/" + role);
    if (found != preferences.end()) {
      return found->second;
    }
  }
  return "";
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

py::list
ackCandidatesToPyList(const std::vector<nsf::AckCandidate>& candidates)
{
  py::list pyCandidates;
  for (const auto& candidate : candidates) {
    PyAckCandidate item;
    item.providerName = candidate.providerName.toUri();
    item.serviceName = candidate.serviceName.toUri();
    item.requestId = candidate.requestId.toUri();
    item.status = candidate.ack.getStatus();
    item.message = candidate.ack.getMessage();
    item.payload = toPyBytes(candidate.ack.getPayload());
    if (candidate.telemetry) {
      item.telemetry = networkTelemetrySnapshotToDict(*candidate.telemetry);
    }
    pyCandidates.append(py::cast(item));
  }
  return pyCandidates;
}

std::vector<PyAckCandidate>
ackCandidatesToPyVector(const std::vector<nsf::AckCandidate>& candidates)
{
  std::vector<PyAckCandidate> output;
  output.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    PyAckCandidate item;
    item.providerName = candidate.providerName.toUri();
    item.serviceName = candidate.serviceName.toUri();
    item.requestId = candidate.requestId.toUri();
    item.status = candidate.ack.getStatus();
    item.message = candidate.ack.getMessage();
    item.payload = toPyBytes(candidate.ack.getPayload());
    if (candidate.telemetry) {
      item.telemetry = networkTelemetrySnapshotToDict(*candidate.telemetry);
    }
    output.push_back(std::move(item));
  }
  return output;
}

class RoleAssignmentSelectionPolicy final : public nsf::ParticipantSelectionPolicy
{
public:
  RoleAssignmentSelectionPolicy(std::map<std::string, ndn::Name> artifactDataNames,
                                std::map<std::string, ndn::Name> scopeKeyDataNames,
                                std::map<std::string, std::vector<std::string>> roleScopes,
                                std::map<std::string, std::string> roleProviderPreference,
                                PyFunctionPtr ackObserver = nullptr)
    : m_artifactDataNames(std::move(artifactDataNames))
    , m_scopeKeyDataNames(std::move(scopeKeyDataNames))
    , m_roleScopes(std::move(roleScopes))
    , m_roleProviderPreference(std::move(roleProviderPreference))
    , m_ackObserver(std::move(ackObserver))
  {
  }

  std::vector<nsf::SelectedParticipant>
  select(const std::vector<nsf::AckCandidate>& candidates,
         const std::vector<nsf::CollaborationRoleSpec>& roles) const override
  {
    if (m_ackObserver) {
      py::gil_scoped_acquire gil;
      try {
        (*m_ackObserver)(ackCandidatesToPyList(candidates));
      }
      catch (const py::error_already_set& e) {
        PyErr_WriteUnraisable(e.value().ptr());
      }
    }

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
    const auto admissionBias = admissionBiasFromEnv();
    for (const auto& role : roles) {
      auto candidatesForRole = candidatesByRole.find(role.role);
      std::vector<nsf::AckCandidate> eligible;
      const auto preferredProvider =
        preferredProviderForRole(m_roleProviderPreference, role.role);
      if (!preferredProvider.empty()) {
        for (const auto& candidate : candidates) {
          if (candidate.ack.getStatus() &&
              candidate.providerName.toUri() == preferredProvider) {
            eligible.push_back(candidate);
          }
        }
      }
      else if (candidatesForRole != candidatesByRole.end()) {
        eligible = candidatesForRole->second;
      }
      if (eligible.empty()) {
        continue;
      }
      auto best = eligible.begin();
      for (auto it = eligible.begin(); it != eligible.end(); ++it) {
        if (!preferredProvider.empty() &&
            best->providerName.toUri() == preferredProvider) {
          continue;
        }
        if (isBetterCapacityAck(*it, *best, providerAssignments, admissionBias)) {
          best = it;
        }
      }
      providerAssignments[best->providerName.toUri()]++;

      std::string assignment;
      if (!role.assignmentPayload.empty()) {
        assignment.assign(
          reinterpret_cast<const char*>(role.assignmentPayload.data()),
          role.assignmentPayload.size());
      }
      else {
        assignment =
          "role=" + role.role +
          ";artifact=" + role.requiredArtifact.toUri() +
          ";requiresProvisioning=" +
          (role.allowDynamicProvisioning ? "1" : "0") +
          ";provisioningTimeoutMs=" +
          std::to_string(role.provisioningTimeoutMs) + ";";
        if (!role.appRequirement.empty()) {
          assignment.append(
            reinterpret_cast<const char*>(role.appRequirement.data()),
            role.appRequirement.size());
          if (!assignment.empty() && assignment.back() != ';') {
            assignment.push_back(';');
          }
        }
      }

      if (role.assignmentPayload.empty()) {
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

        const auto ackPayloadText = bytesToString(best->ack.getPayload());
        const auto leaseId = fieldFromText(ackPayloadText, "leaseId");
        if (!leaseId.empty()) {
          assignment += "leaseId=" + leaseId + ";";
        }
        const auto resourceBindingProof =
          fieldFromText(ackPayloadText, "resourceBindingProof");
        if (!resourceBindingProof.empty()) {
          assignment += "resourceBindingProof=" + resourceBindingProof + ";";
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
    if (std::getenv("NDNSF_PY_COLLAB_SELECTION_TRACE") != nullptr) {
      std::cout << "NDNSF_PY_COLLAB_SELECTION candidates=" << candidates.size()
                << " roles=" << roles.size()
                << " selected=" << selected.size();
      for (const auto& participant : selected) {
        std::cout << " roleProvider." << participant.role << "="
                  << participant.provider.toUri()
                  << " assignmentPayloadBytes="
                  << participant.assignmentPayload.size();
      }
      std::cout << std::endl;
    }
    // Core publishes each collaboration assignment as an authenticated,
    // provider-specific Selection projection.  Do not repeat the complete
    // roleProvider.* map inside every opaque assignment: the plan already
    // binds that map and the participant needs only its own exact projection.
    return selected;
  }

private:
  std::map<std::string, ndn::Name> m_artifactDataNames;
  std::map<std::string, ndn::Name> m_scopeKeyDataNames;
  std::map<std::string, std::vector<std::string>> m_roleScopes;
  std::map<std::string, std::string> m_roleProviderPreference;
  PyFunctionPtr m_ackObserver;
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
    assignment.selectionDigest = native.selectionDigest;
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

  void allowData(const std::string& keyScope, const std::string& topicPrefix)
  {
    m_ctx->allowData(keyScope, ndn::Name(topicPrefix));
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

  std::optional<py::bytes> fetchLargeExact(const std::string& dataName,
                                           const std::string& keyScope,
                                           int timeoutMs,
                                           size_t expectedSegments)
  {
    auto payload = m_ctx->fetchLarge(ndn::Name(dataName), keyScope, timeoutMs,
                                     expectedSegments);
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

  void reportOperationStatus(const py::dict& payload)
  {
    nsf::ServiceProvider::ServiceOperationStatus status;
    auto text = [&payload](const char* name, const std::string& fallback = "") {
      if (!payload.contains(name) || payload[name].is_none()) {
        return fallback;
      }
      return py::cast<std::string>(payload[name]);
    };
    auto integer = [&payload](const char* name, uint64_t fallback) {
      if (!payload.contains(name) || payload[name].is_none()) {
        return fallback;
      }
      return py::cast<uint64_t>(payload[name]);
    };
    status.operationId = text("operation_id");
    status.operation = text("operation");
    status.role = text("role");
    status.attempt = integer("attempt", 1);
    status.epoch = integer("epoch", 1);
    status.sequence = integer("sequence", 1);
    status.state = text("state", "QUEUED");
    status.reasonCode = text("reason_code");
    status.message = text("message");
    status.progressKnown = payload.contains("progress_known") &&
                           py::cast<bool>(payload["progress_known"]);
    status.progress = payload.contains("progress") ?
      py::cast<double>(payload["progress"]) : 0.0;
    status.createdAtMs = integer("created_at_ms", 0);
    status.updatedAtMs = integer("updated_at_ms", 0);
    status.expiresAtMs = integer("expires_at_ms", 0);
    status.detailsSchema = text("details_schema");
    if (payload.contains("details_payload")) {
      status.detailsPayload = toBuffer(py::cast<py::bytes>(payload["details_payload"]));
    }
    m_ctx->reportOperationStatus(std::move(status));
  }

private:
  nsf::ServiceProvider::CollaborationContext* m_ctx = nullptr;
};

class PyOpaqueSelectionParticipant final : public nsf::OpaqueSelectionParticipant
{
public:
  PyOpaqueSelectionParticipant(
      std::string id, uint32_t version,
      std::shared_ptr<py::function> prepare,
      std::shared_ptr<py::function> onCommitted,
      std::shared_ptr<py::function> onAborted)
    : m_id(std::move(id))
    , m_version(version)
    , m_prepare(std::move(prepare))
    , m_onCommitted(std::move(onCommitted))
    , m_onAborted(std::move(onAborted))
  {
    if (m_id.empty() || m_version == 0 || !m_prepare || !m_onCommitted ||
        !m_onAborted) {
      throw std::invalid_argument(
          "opaque Selection participant registration is incomplete");
    }
  }

  std::string participantId() const override
  {
    return m_id;
  }

  uint32_t participantVersion() const override
  {
    return m_version;
  }

  nsf::OpaqueSelectionPrepareResult
  prepare(const nsf::AuthenticatedSelectionContext& context,
          ndn::span<const uint8_t> payload) override
  {
    py::gil_scoped_acquire gil;
    py::dict immutableContext;
    immutableContext["transaction_id"] = context.transactionId;
    immutableContext["service_name"] = context.serviceName.toUri();
    immutableContext["request_id"] = context.requestId.toUri();
    immutableContext["attempt"] = context.attempt;
    immutableContext["selection_identity"] = context.selectionIdentity;
    immutableContext["selection_payload_digest"] =
        context.selectionPayloadDigest;
    immutableContext["provider_identity"] = context.providerIdentity.toUri();
    immutableContext["provider_boot_epoch"] = context.providerBootEpoch;
    immutableContext["expires_at_unix_ms"] = context.expiresAtUnixMs;
    immutableContext["provider_token_record_ref"] =
        context.providerTokenRecordRef;
    immutableContext["lease_record_ref"] =
        context.leaseRecordRef.value_or("");
    const py::object result =
        (*m_prepare)(std::move(immutableContext),
                     py::bytes(reinterpret_cast<const char*>(payload.data()),
                               payload.size()));
    const auto fields = result.cast<py::dict>();
    if (!fields.contains("commit_blob") ||
        !fields.contains("acceptance_payload")) {
      throw std::invalid_argument(
          "opaque prepare must return commit_blob and acceptance_payload");
    }
    nsf::OpaqueSelectionPrepareResult prepared;
    prepared.participantId = m_id;
    prepared.participantVersion = m_version;
    prepared.commitBlob =
        toBuffer(py::cast<py::bytes>(fields["commit_blob"]));
    prepared.acceptancePayload =
        toBuffer(py::cast<py::bytes>(fields["acceptance_payload"]));
    prepared.commitBlobDigest = nsf::GenericSelectionTxnStore::digest(
        {prepared.commitBlob.data(), prepared.commitBlob.size()});
    prepared.acceptancePayloadDigest =
        nsf::GenericSelectionTxnStore::digest(
            {prepared.acceptancePayload.data(),
             prepared.acceptancePayload.size()});
    return prepared;
  }

  void
  onCommitted(const nsf::GenericCommittedSelectionView& committed) override
  {
    py::gil_scoped_acquire gil;
    py::dict view;
    view["transaction_id"] = committed.transactionId;
    view["participant_id"] = committed.participantId;
    view["participant_version"] = committed.participantVersion;
    view["service_name"] = committed.serviceName.toUri();
    view["request_id"] = committed.requestId.toUri();
    view["attempt"] = committed.attempt;
    view["selection_identity"] = committed.selectionIdentity;
    view["selection_payload_digest"] = committed.selectionPayloadDigest;
    view["provider_identity"] = committed.providerIdentity.toUri();
    view["provider_boot_epoch"] = committed.providerBootEpoch;
    view["provider_token_record_ref"] =
        committed.providerTokenRecordRef;
    view["lease_record_ref"] = committed.leaseRecordRef.value_or("");
    view["commit_blob"] = toPyBytes(committed.commitBlob);
    view["commit_blob_digest"] = committed.commitBlobDigest;
    view["acceptance_payload"] = toPyBytes(committed.acceptancePayload);
    view["acceptance_payload_digest"] =
        committed.acceptancePayloadDigest;
    view["committed_at_unix_ms"] = committed.committedAtUnixMs;
    view["expires_at_unix_ms"] = committed.expiresAtUnixMs;
    (*m_onCommitted)(std::move(view));
  }

  void
  onAborted(const std::string& transactionId,
            const std::string& reasonCode) override
  {
    py::gil_scoped_acquire gil;
    (*m_onAborted)(transactionId, reasonCode);
  }

private:
  std::string m_id;
  uint32_t m_version = 0;
  std::shared_ptr<py::function> m_prepare;
  std::shared_ptr<py::function> m_onCommitted;
  std::shared_ptr<py::function> m_onAborted;
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
                        bool serveCertificates,
                        const std::string& bootstrapToken)
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
    if (auto controllerCert = loadControllerCertificateOverride(m_controller)) {
      m_controllerCert = *controllerCert;
    }
    else {
      m_controllerCert = getOrCreateIdentity(m_keyChain, m_controller);
    }
    if (!bootstrapToken.empty()) {
      m_providerCert = nsf::ensureControllerSignedCertificate(
        m_face, m_keyChain, m_controller, m_providerIdentity,
        m_providerIdentity, bootstrapToken);
    }
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

  std::string
  providerBootEpoch() const
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    return m_provider->getProviderBootEpoch();
  }

  std::string
  providerIdentity() const
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    return m_provider->getName().toUri();
  }

  std::string
  providerSigningKeyName() const
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    return m_provider->getSigningKeyName().toUri();
  }

  std::string
  providerSigningCertificateName() const
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    return m_provider->getSigningCertificateName().toUri();
  }

  void
  addService(const std::string& serviceName,
             py::function requestHandler,
             std::optional<py::function> ackHandler,
             bool includeRequestContext = false,
             bool includeAckContext = false)
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    m_handlers.emplace(serviceName, requestHandler);
    if (ackHandler) {
      m_ackHandlers.emplace(serviceName, *ackHandler);
    }

    auto ackAdapter = nsf::ServiceProvider::AckStrategyHandler(
        [this, serviceName, includeAckContext](const nsf::RequestMessage& request) {
          nsf::ServiceProvider::AckDecision decision;
          auto it = m_ackHandlers.find(serviceName);
          if (it == m_ackHandlers.end()) {
            decision.status = true;
            decision.message = "python-provider-ready";
            return decision;
          }
          py::gil_scoped_acquire gil;
          try {
            py::object result = includeAckContext ?
              it->second(makeAckRequestContext(request), toPyBytes(request.getPayload())) :
              it->second(toPyBytes(request.getPayload()));
            if (py::isinstance<PyAckDecision>(result)) {
              auto pyDecision = result.cast<PyAckDecision>();
              decision.status = pyDecision.status;
              decision.suppressAck = pyDecision.suppress;
              decision.message = pyDecision.message;
              decision.payload = toBuffer(pyDecision.payload);
              decision.reservationLease =
                toDeploymentControlContract<nsf::ReservationLease>(
                  pyDecision.reservationLease);
              decision.selectionInputKeyOffer =
                toDeploymentControlContract<nsf::SelectionInputKeyOffer>(
                  pyDecision.selectionInputKeyOffer);
              decision.pendingStateTtlMs = pyDecision.pendingStateTtlMs;
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
        });
    auto requestAdapter = nsf::ServiceProvider::RequestHandler(
        [this, serviceName, includeRequestContext](const ndn::Name& requesterIdentity,
                            const ndn::Name& providerName,
                            const ndn::Name& resolvedServiceName,
                            const ndn::Name& requestId,
                            const nsf::RequestMessage& request) {
          nsf::ResponseMessage response;
          py::gil_scoped_acquire gil;
          try {
            py::object result;
            if (includeRequestContext) {
              py::dict context;
              context["requesterIdentity"] = requesterIdentity.toUri();
              context["providerName"] = providerName.toUri();
              context["serviceName"] = resolvedServiceName.toUri();
              context["requestId"] = requestId.toUri();
              result = m_handlers.at(serviceName)(
                std::move(context), toPyBytes(request.getPayload()));
            }
            else {
              result = m_handlers.at(serviceName)(toPyBytes(request.getPayload()));
            }
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
        });

    m_provider->addService(
      ndn::Name(serviceName),
      std::move(ackAdapter),
      std::move(requestAdapter),
      nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
  }

  void
  configureOpaqueSelectionStore(const std::string& walPath,
                                py::bytes storageKey,
                                const std::string& storageKeyEpoch,
                                uint64_t maxPrepareMs)
  {
    nsf::GenericSelectionTxnOptions options;
    options.maxPrepareTime = std::chrono::milliseconds(maxPrepareMs);
    m_opaqueSelectionStore =
        std::make_shared<nsf::GenericSelectionTxnStore>(
            walPath, toBuffer(storageKey), storageKeyEpoch, options);
    m_provider->setGenericSelectionTxnStore(m_opaqueSelectionStore);
  }

  void
  registerOpaqueSelectionParticipant(
      const std::string& serviceName,
      const std::string& participantId,
      uint32_t participantVersion,
      py::function prepare,
      py::function onCommitted,
      py::function onAborted)
  {
    if (!m_opaqueSelectionStore)
      throw std::logic_error(
          "configure opaque Selection store before participant registration");
    auto participant = std::make_shared<PyOpaqueSelectionParticipant>(
        participantId, participantVersion,
        keepPyFunction(std::move(prepare)),
        keepPyFunction(std::move(onCommitted)),
        keepPyFunction(std::move(onAborted)));
    m_opaqueSelectionParticipants[serviceName] = participant;
    m_provider->registerOpaqueSelectionParticipant(
        ndn::Name(serviceName), participant);
  }

  void
  setDeploymentPrepareHandler(py::function handler)
  {
    auto callback = keepPyFunction(std::move(handler));
    m_deploymentPrepareHandler = callback;
    m_provider->setDeploymentPrepareHandler(
      [callback](const ndn::Name& requester, const ndn::Name& provider,
                 const ndn::Name& service, const ndn::Name& requestId,
                 const nsf::RequestMessage& request,
                 const nsf::DeploymentPlan& plan,
                 const std::string& selectionDigest) {
        py::gil_scoped_acquire gil;
        py::dict context;
        context["requester_identity"] = requester.toUri();
        context["provider_identity"] = provider.toUri();
        context["service_name"] = service.toUri();
        context["request_id"] = requestId.toUri();
        context["request_payload"] = toPyBytes(request.getPayload());
        context["selection_digest"] = selectionDigest;
        context["deployment_plan_digest"] = plan.computeDigest();
        context["deployment_plan"] = plan.getFields();
        py::object result = (*callback)(context);
        if (py::isinstance<nsf::ProviderReadyMessage>(result)) {
          return result.cast<nsf::ProviderReadyMessage>();
        }
        auto fields = result.cast<std::map<std::string, std::string>>();
        nsf::ProviderReadyMessage ready;
        for (const auto& field : fields) ready.setField(field.first, field.second);
        return ready;
      });
  }

  void
  setR1SelectionDecisionHandler(const std::string& serviceName,
                                py::function handler)
  {
    auto callback = keepPyFunction(std::move(handler));
    m_r1SelectionDecisionHandlers[serviceName] = callback;
    m_provider->setR1SelectionDecisionHandler(
      ndn::Name(serviceName),
      [callback](const nsf::SelectionDecision& decision) {
        py::gil_scoped_acquire gil;
        py::object result = (*callback)(py::cast(decision.getFields()));
        if (py::isinstance<nsf::SelectionDecisionReceipt>(result)) {
          auto receipt = result.cast<nsf::SelectionDecisionReceipt>();
          receipt.setField("decisionDigest", decision.computeDigest());
          receipt.setField("reservationId", decision.getField("reservationId"));
          return receipt;
        }
        const auto fields = result.cast<std::map<std::string, std::string>>();
        nsf::SelectionDecisionReceipt receipt;
        for (const auto& field : fields)
          receipt.setField(field.first, field.second);
        // Python may use a JSON canonical form internally. Wire authority is
        // the exact C++ SelectionDecision digest received by Core.
        receipt.setField("decisionDigest", decision.computeDigest());
        receipt.setField("reservationId", decision.getField("reservationId"));
        return receipt;
      });
  }

  void
  setR1ReservationTerminalHandler(const std::string& serviceName,
                                  py::function handler)
  {
    auto callback = keepPyFunction(std::move(handler));
    m_r1ReservationTerminalHandlers[serviceName] = callback;
    m_provider->setR1ReservationTerminalHandler(
      ndn::Name(serviceName), [callback](const std::string& reservationId,
                                        const std::string& cause) {
        py::gil_scoped_acquire gil;
        (*callback)(reservationId, cause);
      });
  }

  void
  addCollaborationService(const std::string& serviceName,
                          const std::vector<std::string>& allowedRoles,
                          py::function collaborationHandler,
                          std::optional<py::function> ackHandler,
                          bool includeAckContext = false)
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
        [this, serviceName, includeAckContext](const nsf::RequestMessage& request) {
          nsf::ServiceProvider::AckDecision decision;
          auto it = m_collaborationAckHandlers.find(serviceName);
          if (it == m_collaborationAckHandlers.end()) {
            decision.status = true;
            decision.message = "python-collaboration-provider-ready";
            return decision;
          }
          py::gil_scoped_acquire gil;
          try {
            py::object result = includeAckContext ?
              it->second(makeAckRequestContext(request), toPyBytes(request.getPayload())) :
              it->second(toPyBytes(request.getPayload()));
            if (py::isinstance<PyAckDecision>(result)) {
              auto pyDecision = result.cast<PyAckDecision>();
              decision.status = pyDecision.status;
              decision.suppressAck = pyDecision.suppress;
              decision.message = pyDecision.message;
              decision.payload = toBuffer(pyDecision.payload);
              decision.reservationLease =
                toDeploymentControlContract<nsf::ReservationLease>(
                  pyDecision.reservationLease);
              decision.selectionInputKeyOffer =
                toDeploymentControlContract<nsf::SelectionInputKeyOffer>(
                  pyDecision.selectionInputKeyOffer);
              decision.pendingStateTtlMs = pyDecision.pendingStateTtlMs;
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

  /// Publish service info via NDNSD with capacity telemetry in meta info.
  void
  publishServiceInfo(const std::string& serviceName,
                     int serviceLifetimeSeconds,
                     const py::dict& metaInfo)
  {
    std::map<std::string, std::string> meta;
    for (const auto& [key, value] : metaInfo) {
      meta[py::str(key).cast<std::string>()] = py::str(value).cast<std::string>();
    }
    m_provider->publishServiceInfo(
      ndn::Name(serviceName),
      serviceLifetimeSeconds,
      std::move(meta));
  }

  void updateNdnsdMeta(const std::string& key, const std::string& value)
  {
    m_provider->updateNdnsdMeta(key, value);
  }

  void setNdnsdMeta(const py::dict& metaInfo)
  {
    std::map<std::string, std::string> meta;
    for (const auto& [key, value] : metaInfo) {
      meta[py::str(key).cast<std::string>()] = py::str(value).cast<std::string>();
    }
    m_provider->setNdnsdMeta(meta);
  }

  void startNdnsdPeriodicPublish(int intervalSeconds)
  {
    m_provider->startNdnsdPeriodicPublish(intervalSeconds);
  }

  std::shared_ptr<nsf::LiveStreamPublisher>
  createLiveStream(const nsf::LiveStreamDefinition& definition)
  {
    start();
    return m_provider->createLiveStream(definition);
  }

  std::shared_ptr<nsf::StreamPublisher>
  createStream(const nsf::StreamConfig& config)
  {
    start();
    return m_provider->createStream(config);
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
  std::shared_ptr<py::function> m_deploymentPrepareHandler;
  std::map<std::string, std::shared_ptr<py::function>>
    m_r1SelectionDecisionHandlers;
  std::map<std::string, std::shared_ptr<py::function>>
    m_r1ReservationTerminalHandlers;
  std::shared_ptr<nsf::GenericSelectionTxnStore> m_opaqueSelectionStore;
  std::map<std::string, std::shared_ptr<PyOpaqueSelectionParticipant>>
    m_opaqueSelectionParticipants;
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
                          bool serveCertificates,
                          const std::string& bootstrapTokenFile)
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
    if (!bootstrapTokenFile.empty()) {
      m_controller->setBootstrapTokenFile(bootstrapTokenFile);
    }
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
                    bool serveCertificates,
                    const std::string& bootstrapToken)
    : m_group(group)
    , m_controller(controller)
    , m_userIdentity(userIdentity)
    , m_trustSchema(trustSchema)
    , m_permissionWaitMs(permissionWaitMs)
  {
    m_userCert = getOrCreateIdentity(m_keyChain, m_userIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controller);
    if (!bootstrapToken.empty()) {
      m_userCert = nsf::ensureControllerSignedCertificate(
        m_face, m_keyChain, m_controller, m_userIdentity,
        m_userIdentity, bootstrapToken);
    }
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

  template<typename Predicate>
  bool
  waitForRuntimeResult(std::condition_variable& cv,
                       std::unique_lock<std::mutex>& lock,
                       const std::chrono::steady_clock::time_point& deadline,
                       Predicate predicate)
  {
    while (!predicate()) {
      if (!m_running.load()) {
        lock.unlock();
        throwIfError();
        lock.lock();
        return false;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return false;
      }
      const auto nextCheck = std::min(deadline, now + std::chrono::milliseconds(50));
      cv.wait_until(lock, nextCheck, predicate);
    }
    return true;
  }

  PyServiceResponse
  requestService(const std::string& serviceName,
                 const py::bytes& requestPayload,
                 int ackTimeoutMs,
                 int timeoutMs,
                 const std::string& strategy,
                 const std::string& requestedRequestId = "",
                 const std::optional<nsf::DeploymentIntent>& deploymentIntent = std::nullopt,
                 const std::optional<nsf::RequestCapabilities>& requestCapabilities = std::nullopt)
  {
    PyServiceResponse output;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    auto payload = toBuffer(requestPayload);
    auto selection = selectionPolicyByName(strategy);

    auto submit = [&, payload, selection, deploymentIntent, requestCapabilities] {
      auto onResponse = [&](const nsf::ResponseMessage& response) {
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(mutex);
          output.status = response.getStatus();
          output.payload = toPyBytes(response.getPayload());
          output.error = response.getErrorInfo();
          output.dataName = response.getDataName();
          output.signerCertificate = response.getSignerCertificate();
          output.wireDigest = response.getWireDigest();
          done = true;
          cv.notify_one();
      };
      auto onTimeout = [&](const ndn::Name& requestId) {
          std::lock_guard<std::mutex> lock(mutex);
          output.status = false;
          output.error = "timeout: " + requestId.toUri();
          done = true;
          cv.notify_one();
      };
      ndn::Name actualRequestId;
      if (deploymentIntent || requestCapabilities) {
        nsf::RequestMessage request;
        auto mutablePayload = payload;
        request.setPayload(mutablePayload, mutablePayload.size());
        if (deploymentIntent) request.setDeploymentIntent(*deploymentIntent);
        if (requestCapabilities) request.setRequestCapabilities(*requestCapabilities);
        const auto nativeSelection = strategy == "all-selected" ?
          nsf::ServiceUser::AckSelectionStrategy::AllSelected :
          (strategy == "random" ?
             nsf::ServiceUser::AckSelectionStrategy::RandomSelection :
             nsf::ServiceUser::AckSelectionStrategy::FirstRespondingSelection);
        actualRequestId = m_user->RequestService(
          {}, ndn::Name(serviceName), std::move(request), ackTimeoutMs,
          nativeSelection, timeoutMs, onTimeout, onResponse,
          requestedRequestId.empty() ? ndn::Name() : ndn::Name(requestedRequestId));
      }
      else {
        actualRequestId = m_user->RequestService(
          ndn::Name(serviceName), payload, ackTimeoutMs, selection, timeoutMs,
          onResponse,
          onTimeout,
          requestedRequestId.empty() ? ndn::Name() : ndn::Name(requestedRequestId));
      }
      std::lock_guard<std::mutex> lock(mutex);
      output.requestId = actualRequestId.toUri();
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), submit);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs + 3000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      if (waitForRuntimeResult(cv, lock, deadline, [&done] { return done; })) {
        return output;
      }
      output.status = false;
      output.error = m_running.load() ? "local deadline" : "runtime stopped";
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
  requestServiceTargeted(const std::string& providerName,
                         const std::string& serviceName,
                         const py::bytes& requestPayload,
                         int timeoutMs)
  {
    struct TargetedSyncState
    {
      std::mutex mutex;
      std::condition_variable cv;
      PyServiceResponse output;
      std::atomic_bool terminalClaimed{false};
      bool done = false;
    };
    auto state = std::make_shared<TargetedSyncState>();

    auto payload = toBuffer(requestPayload);
    auto submit = [this, providerName, serviceName, payload, timeoutMs, state]() mutable {
      nsf::RequestMessage request;
      request.setPayload(payload, payload.size());
      m_user->RequestServiceTargeted(
        ndn::Name(providerName),
        ndn::Name(serviceName),
        std::move(request),
        timeoutMs,
        [state](const ndn::Name& requestId) {
          if (state->terminalClaimed.exchange(true)) {
            return;
          }
          std::lock_guard<std::mutex> lock(state->mutex);
          state->output.status = false;
          state->output.error = "timeout: " + requestId.toUri();
          state->done = true;
          state->cv.notify_one();
        },
        [state](const nsf::ResponseMessage& response) {
          if (state->terminalClaimed.exchange(true)) {
            return;
          }
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(state->mutex);
          state->output.status = response.getStatus();
          state->output.payload = toPyBytes(response.getPayload());
          state->output.error = response.getErrorInfo();
          state->output.dataName = response.getDataName();
          state->output.signerCertificate = response.getSignerCertificate();
          state->output.wireDigest = response.getWireDigest();
          state->done = true;
          state->cv.notify_one();
        });
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), submit);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(std::max(timeoutMs, 0) + 500);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(state->mutex);
      if (waitForRuntimeResult(state->cv, lock, deadline,
                               [state] { return state->done; })) {
        return state->output;
      }
      state->terminalClaimed.store(true);
      PyServiceResponse output;
      output.status = false;
      output.error = m_running.load() ? "local deadline" : "runtime stopped";
      return output;
    }

    std::lock_guard<std::mutex> callLock(m_callMutex);
    submit();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(timeoutMs, 0) + 500);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->done) {
          return state->output;
        }
      }
      py::gil_scoped_release release;
      processFaceEvents(m_face, pythonFacePollTimeout());
    }
    state->terminalClaimed.store(true);
    PyServiceResponse output;
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
      if (!waitForRuntimeResult(state->cv, lock, deadline,
                                [&state] { return state->done; })) {
        PyLargeDataPublishResult output;
        output.success = false;
        output.error = m_running.load() ? "local deadline" : "runtime stopped";
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

  PySignedAppDataResult
  publishSignedAppData(const std::string& dataName,
                       const py::bytes& payload,
                       int freshnessMs)
  {
    const auto input = toBuffer(payload);
    struct PublishState
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      bool success = false;
      std::string name;
      std::string error;
    };
    auto state = std::make_shared<PublishState>();
    auto submit = [this, dataName, input, freshnessMs, state] {
      try {
        m_user->publishSignedAppData(
          ndn::Name(dataName), input, ndn::time::milliseconds(freshnessMs));
        // Preserve the caller's URI spelling. ndn-cxx may render an equivalent
        // component using percent escapes (for example ':' as '%3A').
        state->name = dataName;
        state->success = true;
      }
      catch (const std::exception& e) {
        state->error = e.what();
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
      if (!waitForRuntimeResult(state->cv, lock, deadline,
                                [&state] { return state->done; })) {
        PySignedAppDataResult output;
        output.error = m_running.load() ? "local deadline" : "runtime stopped";
        return output;
      }
    }
    else {
      std::lock_guard<std::mutex> callLock(m_callMutex);
      submit();
    }
    PySignedAppDataResult output;
    output.success = state->success;
    output.dataName = state->name;
    output.error = state->error;
    return output;
  }

  PySignedAppDataResult
  fetchSignedAppData(const std::string& dataName,
                     const std::string& expectedSigner,
                     int timeoutMs)
  {
    if (timeoutMs <= 0) {
      throw std::invalid_argument("timeout_ms must be positive");
    }
    start();
    struct FetchState
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      bool success = false;
      ndn::Buffer payload;
      std::string name;
      std::string signerCertificate;
      std::string error;
    };
    auto state = std::make_shared<FetchState>();
    boost::asio::post(m_face.getIoContext(),
      [this, dataName, expectedSigner, timeoutMs, state] {
        try {
          m_user->fetchSignedAppData(
            ndn::Name(dataName), ndn::Name(expectedSigner), timeoutMs,
            [state, dataName](const ndn::Data& data) {
              std::lock_guard<std::mutex> lock(state->mutex);
              const auto& content = data.getContent();
              state->payload = ndn::Buffer(content.value(), content.value_size());
              state->name = dataName;
              if (data.getSignatureInfo().hasKeyLocator() &&
                  data.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name) {
                state->signerCertificate =
                  data.getSignatureInfo().getKeyLocator().getName().toUri();
              }
              state->success = true;
              state->done = true;
              state->cv.notify_one();
            },
            [state](const ndn::Name&, const std::string& reason) {
              std::lock_guard<std::mutex> lock(state->mutex);
              if (state->done) {
                return;
              }
              state->error = reason;
              state->done = true;
              state->cv.notify_one();
            });
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->error = e.what();
          state->done = true;
          state->cv.notify_one();
        }
      });
    {
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs + 1000),
                         [state] { return state->done; });
    }
    PySignedAppDataResult output;
    output.success = state->success;
    output.dataName = state->name;
    output.signerCertificate = state->signerCertificate;
    output.payload = toPyBytes(state->payload);
    output.error = state->done ? state->error : "local deadline";
    return output;
  }

  nsf::CollaborationPlan
  buildCollaborationPlan(const std::string& serviceName,
                         const std::vector<std::map<std::string, py::object>>& roles,
                         const std::map<std::string, std::vector<std::string>>& keyScopes,
                         const std::vector<std::map<std::string, py::object>>& dependencies,
                         const std::map<std::string, std::string>& artifactDataNames,
                         const std::map<std::string, std::string>& scopeKeyDataNames,
                         const std::map<std::string, std::vector<std::string>>& roleScopes,
                         int ackTimeoutMs,
                         int timeoutMs,
                         const std::map<std::string, std::string>& roleProviderAssignments = {},
                         PyFunctionPtr ackObserver = nullptr)
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
      auto assignmentIt = entry.find("assignment_payload");
      if (assignmentIt != entry.end() && !assignmentIt->second.is_none()) {
        role.assignmentPayload =
          toBuffer(assignmentIt->second.cast<py::bytes>());
      }
      plan.roles.push_back(std::move(role));
    }

    for (const auto& entry : keyScopes) {
      plan.keyScopes.push_back({entry.first, entry.second});
    }
    std::string sharedAssignmentMetadata;
    for (const auto& entry : scopeKeyDataNames) {
      if (entry.first.empty() || entry.second.empty()) {
        throw std::runtime_error(
          "collaboration scope-key Data reference is incomplete");
      }
      sharedAssignmentMetadata +=
        "scopeKeyData." + entry.first + "=" + entry.second + ";";
    }
    plan.sharedAssignmentMetadata = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(sharedAssignmentMetadata.data()),
      sharedAssignmentMetadata.size());

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
      roleScopes,
      roleProviderAssignments,
      std::move(ackObserver));
    return plan;
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
                       int timeoutMs,
                       py::object ackObserver = py::none(),
                       const std::map<std::string, std::string>& roleProviderAssignments = {},
                       const std::string& requestedRequestId = "")
  {
    PyFunctionPtr observer;
    if (!ackObserver.is_none()) {
      observer = keepPyFunction(ackObserver.cast<py::function>());
    }
    auto plan = buildCollaborationPlan(serviceName,
                                       roles,
                                       keyScopes,
                                       dependencies,
                                       artifactDataNames,
                                       scopeKeyDataNames,
                                       roleScopes,
                                       ackTimeoutMs,
                                       timeoutMs,
                                       roleProviderAssignments,
                                       std::move(observer));

    PyServiceResponse output;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool submitted = false;
    auto payload = toBuffer(initialPayload);

    auto submit = [&, payload, plan = std::move(plan), requestedRequestId]() mutable {
      const auto requestId = m_user->RequestCollaboration(
        ndn::Name(serviceName),
        payload,
        std::move(plan),
        [&](const nsf::ResponseMessage& response) {
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(mutex);
          output.status = response.getStatus();
          output.payload = toPyBytes(response.getPayload());
          output.error = response.getErrorInfo();
          output.dataName = response.getDataName();
          output.signerCertificate = response.getSignerCertificate();
          output.wireDigest = response.getWireDigest();
          done = true;
          cv.notify_one();
        },
        [&](const ndn::Name& requestId) {
          std::lock_guard<std::mutex> lock(mutex);
          output.status = false;
          output.error = "timeout: " + requestId.toUri();
          done = true;
          cv.notify_one();
        },
        requestedRequestId.empty() ? ndn::Name() : ndn::Name(requestedRequestId));
      {
        std::lock_guard<std::mutex> lock(mutex);
        output.requestId = requestId.toUri();
        submitted = true;
      }
      cv.notify_one();
    };

    if (m_running.load()) {
      boost::asio::post(m_face.getIoContext(), std::move(submit));
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs + 3000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      if (waitForRuntimeResult(cv, lock, deadline,
                               [&done, &submitted] { return done && submitted; })) {
        return output;
      }
      output.status = false;
      output.error = m_running.load() ? "local deadline" : "runtime stopped";
      return output;
    }

    std::lock_guard<std::mutex> callLock(m_callMutex);
    submit();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done && submitted) {
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

  std::string
  beginCollaboration(const std::string& serviceName,
                     const py::bytes& initialPayload,
                     py::function onAckClosed,
                     py::function onResponse,
                     py::function onTimeout,
                     int ackTimeoutMs,
                     int timeoutMs,
                     const std::string& requestedRequestId = "",
                     py::object ackCoveragePredicate = py::none())
  {
    start();
    auto payload = toBuffer(initialPayload);
    auto ackClosedCallback = keepPyFunction(std::move(onAckClosed));
    auto responseCallback = keepPyFunction(std::move(onResponse));
    auto timeoutCallback = keepPyFunction(std::move(onTimeout));
    PyFunctionPtr ackCoverageCallback;
    if (!ackCoveragePredicate.is_none()) {
      ackCoverageCallback = keepPyFunction(
        ackCoveragePredicate.cast<py::function>());
    }
    auto actualRequestId = std::make_shared<std::string>();
    std::mutex mutex;
    std::condition_variable cv;
    bool submitted = false;
    std::string submissionError;

    boost::asio::post(m_face.getIoContext(),
      [this, serviceName, payload, ackTimeoutMs, timeoutMs, requestedRequestId,
       ackClosedCallback = std::move(ackClosedCallback),
       responseCallback = std::move(responseCallback),
       timeoutCallback = std::move(timeoutCallback),
       ackCoverageCallback = std::move(ackCoverageCallback),
       actualRequestId, &mutex, &cv, &submitted, &submissionError]() mutable {
        try {
          const auto requestId = m_user->BeginCollaboration(
            ndn::Name(serviceName),
            payload,
            ackTimeoutMs,
            timeoutMs,
            [ackClosedCallback](const nsf::CollaborationAckClosure& closure) {
              py::gil_scoped_acquire gil;
              PyCollaborationAckClosure output;
              output.requestId = closure.requestId.toUri();
              output.candidates = ackCandidatesToPyVector(closure.candidates);
              output.digest = closure.digest;
              output.closedAtUs = closure.closedAtUs;
              output.requestDeadlineUs = closure.requestDeadlineUs;
              try {
                (*ackClosedCallback)(output);
              }
              catch (const py::error_already_set& error) {
                PyErr_WriteUnraisable(error.value().ptr());
              }
            },
            [responseCallback, actualRequestId](
                const nsf::ResponseMessage& response) {
              py::gil_scoped_acquire gil;
              PyServiceResponse output;
              output.status = response.getStatus();
              output.payload = toPyBytes(response.getPayload());
              output.error = response.getErrorInfo();
              output.requestId = *actualRequestId;
              output.dataName = response.getDataName();
              output.signerCertificate = response.getSignerCertificate();
              output.wireDigest = response.getWireDigest();
              try {
                (*responseCallback)(output);
              }
              catch (const py::error_already_set& error) {
                PyErr_WriteUnraisable(error.value().ptr());
              }
            },
            [timeoutCallback](const ndn::Name& requestId) {
              py::gil_scoped_acquire gil;
              try {
                (*timeoutCallback)(requestId.toUri());
              }
              catch (const py::error_already_set& error) {
                PyErr_WriteUnraisable(error.value().ptr());
              }
            },
            requestedRequestId.empty() ?
              ndn::Name() : ndn::Name(requestedRequestId),
            [ackCoverageCallback](const std::vector<nsf::AckCandidate>& candidates) {
              if (!ackCoverageCallback) {
                return false;
              }
              py::gil_scoped_acquire gil;
              try {
                py::object result = (*ackCoverageCallback)(
                  ackCandidatesToPyList(candidates));
                return result.cast<bool>();
              }
              catch (const py::error_already_set& error) {
                PyErr_WriteUnraisable(error.value().ptr());
              }
              return false;
            });
          {
            std::lock_guard<std::mutex> lock(mutex);
            *actualRequestId = requestId.toUri();
            submitted = true;
          }
        }
        catch (const std::exception& error) {
          std::lock_guard<std::mutex> lock(mutex);
          submissionError = error.what();
          submitted = true;
        }
        cv.notify_one();
      });

    {
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      if (!cv.wait_for(lock, std::chrono::seconds(3),
                       [&submitted] { return submitted; })) {
        throw std::runtime_error(
          "timed out submitting deferred collaboration");
      }
    }
    if (!submissionError.empty()) {
      throw std::runtime_error(submissionError);
    }
    return *actualRequestId;
  }

  bool
  commitCollaborationPlan(
      const std::string& serviceName,
      const std::string& requestId,
      const std::string& ackClosedDigest,
      const std::vector<std::map<std::string, py::object>>& roles,
      const std::map<std::string, std::vector<std::string>>& keyScopes,
      const std::vector<std::map<std::string, py::object>>& dependencies,
      const std::map<std::string, std::string>& artifactDataNames,
      const std::map<std::string, std::string>& scopeKeyDataNames,
      const std::map<std::string, std::vector<std::string>>& roleScopes,
      int ackTimeoutMs,
      int timeoutMs,
      const std::map<std::string, std::string>& roleProviderAssignments = {})
  {
    auto plan = buildCollaborationPlan(
      serviceName, roles, keyScopes, dependencies, artifactDataNames,
      scopeKeyDataNames, roleScopes, ackTimeoutMs, timeoutMs,
      roleProviderAssignments);
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool result = false;
    std::string error;
    boost::asio::post(m_face.getIoContext(),
      [this, requestId, ackClosedDigest, plan = std::move(plan),
       &mutex, &cv, &done, &result, &error]() mutable {
        try {
          result = m_user->CommitCollaborationPlan(
            ndn::Name(requestId), ackClosedDigest, std::move(plan));
        }
        catch (const std::exception& exception) {
          error = exception.what();
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          done = true;
        }
        cv.notify_one();
      });
    {
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      if (!cv.wait_for(lock, std::chrono::seconds(3),
                       [&done] { return done; })) {
        throw std::runtime_error(
          "timed out committing deferred collaboration plan");
      }
    }
    if (!error.empty()) {
      throw std::runtime_error(error);
    }
    return result;
  }

  void
  requestCollaborationAsync(const std::string& serviceName,
                            const py::bytes& initialPayload,
                            const std::vector<std::map<std::string, py::object>>& roles,
                            const std::map<std::string, std::vector<std::string>>& keyScopes,
                            const std::vector<std::map<std::string, py::object>>& dependencies,
                            const std::map<std::string, std::string>& artifactDataNames,
                            const std::map<std::string, std::string>& scopeKeyDataNames,
                            const std::map<std::string, std::vector<std::string>>& roleScopes,
                            py::function onResponse,
                            py::function onTimeout,
                            int ackTimeoutMs,
                            int timeoutMs,
                            const std::map<std::string, std::string>& roleProviderAssignments = {},
                            const std::string& requestedRequestId = "")
  {
    start();
    auto payload = toBuffer(initialPayload);
    auto plan = buildCollaborationPlan(serviceName,
                                       roles,
                                       keyScopes,
                                       dependencies,
                                       artifactDataNames,
                                       scopeKeyDataNames,
                                       roleScopes,
                                       ackTimeoutMs,
                                       timeoutMs,
                                       roleProviderAssignments);
    auto responseCallback = keepPyFunction(std::move(onResponse));
    auto timeoutCallback = keepPyFunction(std::move(onTimeout));
    boost::asio::post(m_face.getIoContext(),
      [this, serviceName, payload, plan = std::move(plan), requestedRequestId,
       responseCallback = std::move(responseCallback),
       timeoutCallback = std::move(timeoutCallback)]() mutable {
        m_user->RequestCollaboration(
          ndn::Name(serviceName),
          payload,
          std::move(plan),
          [responseCallback, requestedRequestId](const nsf::ResponseMessage& response) mutable {
            py::gil_scoped_acquire gil;
            PyServiceResponse output;
            output.status = response.getStatus();
            output.payload = toPyBytes(response.getPayload());
            output.error = response.getErrorInfo();
            output.requestId = requestedRequestId;
            output.dataName = response.getDataName();
            output.signerCertificate = response.getSignerCertificate();
            output.wireDigest = response.getWireDigest();
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
          },
          requestedRequestId.empty() ? ndn::Name() : ndn::Name(requestedRequestId));
      });
  }

  PyServiceResponse
  requestServiceSelect(const std::string& serviceName,
                       const py::bytes& requestPayload,
                       py::function selector,
                       int ackTimeoutMs,
                       int timeoutMs,
                       const std::string& requestStrategy,
                       const std::optional<nsf::DeploymentIntent>& deploymentIntent = std::nullopt,
                       const std::optional<nsf::RequestCapabilities>& requestCapabilities = std::nullopt)
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
          if (candidate.telemetry) {
            item.telemetry = networkTelemetrySnapshotToDict(*candidate.telemetry);
          }
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

    auto submit = [&, payload, handler, nativeStrategy, deploymentIntent, requestCapabilities] {
      nsf::RequestMessage requestMessage;
      auto mutablePayload = payload;
      requestMessage.setPayload(mutablePayload, mutablePayload.size());
      requestMessage.setStrategy(nativeStrategy);
      if (deploymentIntent) requestMessage.setDeploymentIntent(*deploymentIntent);
      if (requestCapabilities) requestMessage.setRequestCapabilities(*requestCapabilities);
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
      if (waitForRuntimeResult(cv, lock, deadline, [&done] { return done; })) {
        return output;
      }
      output.status = false;
      output.error = m_running.load() ? "local deadline" : "runtime stopped";
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
       timeoutCallback = std::move(timeoutCallback)]() mutable {
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
  requestServiceTargetedAsync(const std::string& providerName,
                              const std::string& serviceName,
                              const py::bytes& requestPayload,
                              py::function onResponse,
                              py::function onTimeout,
                              int timeoutMs)
  {
    start();
    auto payload = toBuffer(requestPayload);
    auto responseCallback = keepPyFunction(std::move(onResponse));
    auto timeoutCallback = keepPyFunction(std::move(onTimeout));
    auto terminalClaimed = std::make_shared<std::atomic_bool>(false);
    boost::asio::post(m_face.getIoContext(),
      [this, providerName, serviceName, payload, timeoutMs,
       responseCallback = std::move(responseCallback),
       timeoutCallback = std::move(timeoutCallback),
       terminalClaimed]() mutable {
        nsf::RequestMessage request;
        request.setPayload(payload, payload.size());
        m_user->RequestServiceTargeted(
          ndn::Name(providerName),
          ndn::Name(serviceName),
          std::move(request),
          timeoutMs,
          [timeoutCallback, terminalClaimed](const ndn::Name& requestId) mutable {
            if (terminalClaimed->exchange(true)) {
              return;
            }
            py::gil_scoped_acquire gil;
            try {
              (*timeoutCallback)(requestId.toUri());
            }
            catch (const py::error_already_set& e) {
              PyErr_WriteUnraisable(e.value().ptr());
            }
          },
          [responseCallback, terminalClaimed](const nsf::ResponseMessage& response) mutable {
            if (terminalClaimed->exchange(true)) {
              return;
            }
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

  std::vector<std::tuple<std::string, std::string, size_t>>
  getAllowedServices() const
  {
    return m_user->getAllowedServices();
  }

  /// Return received NDNSD service details as a list of dicts.
  py::list
  getNdnsdServices() const
  {
    py::list result;
    try {
      auto details = m_user->getNdnsdReceivedDetails();
      for (const auto& [key, detail] : details) {
        py::dict entry;
        entry["provider"] = detail.applicationPrefix.toUri();
        entry["serviceName"] = detail.serviceName.toUri();
        entry["serviceLifetime"] = detail.serviceLifetime;
        entry["publishTimestamp"] = static_cast<int64_t>(detail.publishTimestamp);
        py::dict meta;
        for (const auto& [mk, mv] : detail.serviceMetaInfo) {
          meta[py::str(mk)] = py::str(mv);
        }
        entry["serviceMetaInfo"] = meta;
        result.append(entry);
      }
    } catch (const std::exception& e) {
      // NDNSD may not be enabled
    }
    return result;
  }

  py::object
  queryCollaborationStatus(const std::string& providerName,
                           const std::string& serviceName,
                           const std::string& selectionDigest,
                           int timeoutMs)
  {
    if (providerName.empty() || serviceName.empty() || selectionDigest.empty() ||
        timeoutMs <= 0) {
      throw std::invalid_argument("invalid collaboration status query binding");
    }
    start();
    struct QueryState {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      std::optional<nsf::SelectionExecutionStatus> result;
    };
    auto state = std::make_shared<QueryState>();
    boost::asio::post(m_face.getIoContext(),
      [this, providerName, serviceName, selectionDigest, timeoutMs,
       state] {
        m_user->QuerySelectionStatus(
          ndn::Name(providerName), ndn::Name(serviceName), selectionDigest,
          [state](const nsf::SelectionExecutionStatus& status) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = status;
            state->done = true;
            state->cv.notify_one();
          },
          [state](const ndn::Name&) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
            state->cv.notify_one();
          }, timeoutMs);
      });
    {
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs + 250),
                         [state] { return state->done; });
    }
    if (!state->result) return py::none();
    return selectionStatusToDict(*state->result);
  }

  py::list
  getCollaborationStatusSnapshot(const std::string& requestId,
                                 int timeoutMs)
  {
    if (requestId.empty() || timeoutMs <= 0) {
      throw std::invalid_argument("invalid collaboration request status binding");
    }
    start();
    struct SnapshotState {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      std::vector<nsf::SelectionExecutionStatus> result;
    };
    auto state = std::make_shared<SnapshotState>();
    boost::asio::post(m_face.getIoContext(), [this, requestId, state] {
      auto result = m_user->GetCollaborationStatusSnapshot(ndn::Name(requestId));
      std::lock_guard<std::mutex> lock(state->mutex);
      state->result = std::move(result);
      state->done = true;
      state->cv.notify_one();
    });
    {
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [state] { return state->done; });
    }
    py::list output;
    for (const auto& item : state->result) {
      output.append(selectionStatusToDict(item));
    }
    return output;
  }

  std::shared_ptr<nsf::LiveStreamConsumerHandle>
  openLiveStream(const nsf::LiveStreamDescriptor& descriptor,
                 py::function onItem,
                 const std::string& startMode,
                 const std::string& prefetchPolicy,
                 size_t aggregateInterestLimit,
                 bool enableFecRecovery,
                 uint64_t interestLifetimeMs,
                 std::optional<py::function> onStatus)
  {
    start();
    auto itemCallback = keepPyFunction(std::move(onItem));
    PyFunctionPtr statusCallback;
    if (onStatus) statusCallback = keepPyFunction(std::move(*onStatus));
    nsf::LiveStreamOpenOptions options;
    if (startMode == "latest") options.start = nsf::LiveStreamStart::Latest;
    else if (startMode == "beginning") options.start = nsf::LiveStreamStart::Beginning;
    else throw std::invalid_argument("start must be latest or beginning");
    if (prefetchPolicy == "mapped-pressure") {
      options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::MappedPressure;
    }
    else if (prefetchPolicy == "mapped-live-v1-future-on") {
      options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::MappedLiveFutureOn;
    }
    else if (prefetchPolicy == "mapped-live-v1-future-off") {
      options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::MappedLiveFutureOff;
    }
    else if (prefetchPolicy == "adaptive-sample-atomic") {
      options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
    }
    else throw std::invalid_argument("unknown live-stream prefetch policy");
    options.aggregateInterestLimit = aggregateInterestLimit;
    options.enableFecRecovery = enableFecRecovery;
    options.interestLifetimeMs = interestLifetimeMs;
    options.onItem = [itemCallback] (const nsf::VerifiedLiveStreamItem& item) {
      py::gil_scoped_acquire gil;
      py::object result = (*itemCallback)(item);
      if (py::isinstance<py::bool_>(result)) {
        return py::cast<bool>(result) ? nsf::LiveStreamItemAdmission::acceptItem() :
          nsf::LiveStreamItemAdmission::rejectItem("application-rejected");
      }
      return result.cast<nsf::LiveStreamItemAdmission>();
    };
    if (statusCallback) {
      options.onStatus = [statusCallback] (const nsf::LiveStreamStatus& status) {
        py::gil_scoped_acquire gil;
        (*statusCallback)(status);
      };
    }
    return m_user->openLiveStream(descriptor, std::move(options));
  }

  std::shared_ptr<nsf::PredictiveStreamSubscriber>
  subscribeStream(const nsf::PredictiveStreamDescriptor& descriptor,
                  py::function onItem,
                  const std::string& startMode,
                  const std::optional<std::string>& prefetchPolicy,
                  size_t aggregateInterestLimit,
                  bool enableFecRecovery,
                  bool requireFullDelivery,
                  uint64_t interestLifetimeMs,
                  std::optional<py::function> onStatus)
  {
    start();
    auto itemCallback = keepPyFunction(std::move(onItem));
    PyFunctionPtr statusCallback;
    if (onStatus) statusCallback = keepPyFunction(std::move(*onStatus));

    nsf::StreamSubscriptionOptions options;
    if (startMode == "latest") options.start = nsf::LiveStreamStart::Latest;
    else if (startMode == "beginning") options.start = nsf::LiveStreamStart::Beginning;
    else throw std::invalid_argument("start must be latest or beginning");

    if (prefetchPolicy) {
      if (*prefetchPolicy == "mapped-pressure") {
        options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::MappedPressure;
      }
      else if (*prefetchPolicy == "mapped-live-v1-future-on") {
        options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::MappedLiveFutureOn;
      }
      else if (*prefetchPolicy == "mapped-live-v1-future-off") {
        options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::MappedLiveFutureOff;
      }
      else if (*prefetchPolicy == "adaptive-sample-atomic") {
        options.prefetchPolicy = nsf::LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
      }
      else {
        throw std::invalid_argument("unknown live-stream prefetch policy");
      }
    }
    options.aggregateInterestLimit = aggregateInterestLimit;
    options.enableFecRecovery = enableFecRecovery;
    options.requireFullDelivery = requireFullDelivery;
    options.interestLifetimeMs = interestLifetimeMs;
    options.onItem = [itemCallback] (const nsf::VerifiedLiveStreamItem& item) {
      py::gil_scoped_acquire gil;
      py::object result = (*itemCallback)(item);
      if (py::isinstance<py::bool_>(result)) {
        return py::cast<bool>(result) ?
          nsf::LiveStreamItemAdmission::acceptItem() :
          nsf::LiveStreamItemAdmission::rejectItem("application-rejected");
      }
      return result.cast<nsf::LiveStreamItemAdmission>();
    };
    if (statusCallback) {
      options.onStatus = [statusCallback] (const nsf::LiveStreamStatus& status) {
        py::gil_scoped_acquire gil;
        (*statusCallback)(status);
      };
    }
    return m_user->subscribeStream(descriptor, std::move(options));
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

template<typename T>
void
bindDeploymentControlMessage(py::module_& m, const char* name)
{
  py::class_<T>(m, name)
    .def(py::init<>())
    .def_property("version", &T::getVersion, &T::setVersion)
    .def("set_field", &T::setField, py::arg("name"), py::arg("value"))
    .def("has_field", &T::hasField, py::arg("name"))
    .def("get_field", &T::getField, py::arg("name"),
         py::return_value_policy::copy)
    .def_property_readonly("fields", [] (const T& value) {
      return value.getFields();
    })
    .def("digest", &T::computeDigest)
    .def("wire_encode", [] (const T& value) {
      return toPyBlockWire(value.WireEncode());
    })
    .def_static("decode", [] (const py::bytes& wire) {
      T value;
      if (!value.WireDecode(blockFromExactPyBytes(wire))) {
        throw std::invalid_argument("invalid deployment-control wire encoding");
      }
      return value;
    }, py::arg("wire"));
}

} // namespace

PYBIND11_MODULE(_ndnsf, m)
{
  bindDeploymentControlMessage<nsf::DeploymentIntent>(m, "NativeDeploymentIntent");
  bindDeploymentControlMessage<nsf::ProviderCapabilityOffer>(m, "NativeProviderCapabilityOffer");
  bindDeploymentControlMessage<nsf::DeploymentPlan>(m, "NativeDeploymentPlan");
  bindDeploymentControlMessage<nsf::ProviderReadyMessage>(m, "NativeProviderReadyMessage");
  bindDeploymentControlMessage<nsf::ReadyAcknowledgement>(m, "NativeReadyAcknowledgement");
  bindDeploymentControlMessage<nsf::ExecutionActivateMessage>(m, "NativeExecutionActivateMessage");
  bindDeploymentControlMessage<nsf::SecureStatusQuery>(m, "NativeSecureStatusQuery");
  bindDeploymentControlMessage<nsf::SecureStatusSnapshot>(m, "NativeSecureStatusSnapshot");
  bindDeploymentControlMessage<nsf::RequestCapabilities>(m, "NativeRequestCapabilities");
  bindDeploymentControlMessage<nsf::EncryptedRequestInput>(m, "NativeEncryptedRequestInput");
  bindDeploymentControlMessage<nsf::SelectionInputKeyOffer>(m, "NativeSelectionInputKeyOffer");
  bindDeploymentControlMessage<nsf::SelectionInputKeyGrant>(m, "NativeSelectionInputKeyGrant");
  bindDeploymentControlMessage<nsf::ReservationLease>(m, "NativeReservationLease");
  bindDeploymentControlMessage<nsf::SelectionDecision>(m, "NativeSelectionDecision");
  bindDeploymentControlMessage<nsf::SelectionDecisionReceipt>(m, "NativeSelectionDecisionReceipt");
  bindDeploymentControlMessage<nsf::RecipientEncryptedAssignment>(m, "NativeRecipientEncryptedAssignment");
  bindDeploymentControlMessage<nsf::StageInputEvidence>(m, "NativeStageInputEvidence");
  bindDeploymentControlMessage<nsf::StageAbort>(m, "NativeStageAbort");
  bindDeploymentControlMessage<nsf::SelectionDecisionTombstone>(m, "NativeSelectionDecisionTombstone");
  m.def("make_opaque_control_handle", &nsf::makeOpaqueControlHandle,
        py::arg("bytes") = 24);
  m.def("is_valid_opaque_control_handle", &nsf::isValidOpaqueControlHandle,
        py::arg("handle"));

  py::class_<nsf::StreamFecInfo>(m, "NativeStreamFecInfo")
    .def(py::init<>())
    .def_readwrite("scheme", &nsf::StreamFecInfo::scheme)
    .def_readwrite("data_shards", &nsf::StreamFecInfo::dataShards)
    .def_readwrite("parity_shards", &nsf::StreamFecInfo::parityShards)
    .def_readwrite("symbol_index", &nsf::StreamFecInfo::symbolIndex)
    .def_readwrite("symbol_count", &nsf::StreamFecInfo::symbolCount)
    .def_readwrite("data_lengths", &nsf::StreamFecInfo::dataLengths)
    .def_readwrite("source_block_id", &nsf::StreamFecInfo::sourceBlockId)
    .def_readwrite("repair_symbol", &nsf::StreamFecInfo::repairSymbol)
    .def_readwrite("metadata", &nsf::StreamFecInfo::metadata)
    .def_property_readonly("enabled", &nsf::StreamFecInfo::enabled);

  py::class_<nsf::StreamChunk>(m, "NativeStreamChunk")
    .def(py::init<>())
    .def_readwrite("stream_id", &nsf::StreamChunk::streamId)
    .def_readwrite("session_epoch", &nsf::StreamChunk::sessionEpoch)
    .def_readwrite("seq", &nsf::StreamChunk::seq)
    .def_property("payload",
      [] (const nsf::StreamChunk& chunk) {
        return py::bytes(reinterpret_cast<const char*>(chunk.payload.data()),
                         chunk.payload.size());
      },
      [] (nsf::StreamChunk& chunk, const py::bytes& value) {
        const auto bytes = static_cast<std::string>(value);
        chunk.payload.assign(bytes.begin(), bytes.end());
      })
    .def_readwrite("content_type", &nsf::StreamChunk::contentType)
    .def_readwrite("capture_ms", &nsf::StreamChunk::captureMs)
    .def_readwrite("arrival_ms", &nsf::StreamChunk::arrivalMs)
    .def_readwrite("deadline_ms", &nsf::StreamChunk::deadlineMs)
    .def_readwrite("key_chunk", &nsf::StreamChunk::keyChunk)
    .def_readwrite("frame_id", &nsf::StreamChunk::frameId)
    .def_readwrite("frame_first_seq", &nsf::StreamChunk::frameFirstSeq)
    .def_readwrite("frame_last_seq", &nsf::StreamChunk::frameLastSeq)
    .def_readwrite("segment_index", &nsf::StreamChunk::segmentIndex)
    .def_readwrite("segment_count", &nsf::StreamChunk::segmentCount)
    .def_readwrite("fec", &nsf::StreamChunk::fec)
    .def_readwrite("metadata", &nsf::StreamChunk::metadata);

  py::class_<nsf::StreamNameMapEntry>(m, "NativeStreamNameMapEntry")
    .def(py::init<>())
    .def_property("original_name",
      [] (const nsf::StreamNameMapEntry& entry) {
        return entry.originalName.toUri();
      },
      [] (nsf::StreamNameMapEntry& entry, const std::string& value) {
        entry.originalName = ndn::Name(value);
      })
    .def_readwrite("tombstone", &nsf::StreamNameMapEntry::tombstone)
    .def_readwrite("group_id", &nsf::StreamNameMapEntry::groupId)
    .def_readwrite("sample_class", &nsf::StreamNameMapEntry::sampleClass)
    .def_readwrite("group_item_index", &nsf::StreamNameMapEntry::groupItemIndex)
    .def_readwrite("predicted_source_items",
                   &nsf::StreamNameMapEntry::predictedSourceItems)
    .def_readwrite("predicted_repair_items",
                   &nsf::StreamNameMapEntry::predictedRepairItems)
    .def_static("from_name",
      [] (const std::string& name) {
        return nsf::StreamNameMapEntry::fromName(ndn::Name(name));
      }, py::arg("name"))
    .def_static("from_grouped_name",
      [] (const std::string& name, std::string groupId, std::string sampleClass,
          uint64_t groupItemIndex, uint64_t predictedSourceItems,
          uint64_t predictedRepairItems) {
        return nsf::StreamNameMapEntry::fromGroupedName(
          ndn::Name(name), std::move(groupId), std::move(sampleClass),
          groupItemIndex, predictedSourceItems, predictedRepairItems);
      }, py::arg("name"), py::arg("group_id"), py::arg("sample_class"),
         py::arg("group_item_index"), py::arg("predicted_source_items"),
         py::arg("predicted_repair_items"))
    .def_static("make_tombstone", &nsf::StreamNameMapEntry::makeTombstone)
    .def("is_tombstone", &nsf::StreamNameMapEntry::isTombstone)
    .def_property_readonly("has_group_binding",
                           &nsf::StreamNameMapEntry::hasGroupBinding)
    .def_property_readonly("predicted_group_items",
                           &nsf::StreamNameMapEntry::predictedGroupItems);

  py::class_<nsf::StreamNameMapBlock>(m, "NativeStreamNameMapBlock")
    .def(py::init<>())
    .def_readwrite("contract_version", &nsf::StreamNameMapBlock::contractVersion)
    .def_readwrite("stream_id", &nsf::StreamNameMapBlock::streamId)
    .def_readwrite("session_epoch", &nsf::StreamNameMapBlock::sessionEpoch)
    .def_readwrite("mapping_version", &nsf::StreamNameMapBlock::mappingVersion)
    .def_readwrite("block_number", &nsf::StreamNameMapBlock::blockNumber)
    .def_readwrite("block_capacity", &nsf::StreamNameMapBlock::blockCapacity)
    .def_readwrite("first_cursor", &nsf::StreamNameMapBlock::firstCursor)
    .def_property("previous_content_digest",
      [] (const nsf::StreamNameMapBlock& block) -> py::object {
        if (!block.previousContentDigest) {
          return py::none();
        }
        return toPyStreamContentDigest(*block.previousContentDigest);
      },
      [] (nsf::StreamNameMapBlock& block, const py::object& value) {
        if (value.is_none()) {
          block.previousContentDigest.reset();
          return;
        }
        block.previousContentDigest =
          streamContentDigestFromPyBytes(value.cast<py::bytes>());
      })
    .def_readwrite("entries", &nsf::StreamNameMapBlock::entries)
    .def("validate", [] (const nsf::StreamNameMapBlock& block) -> py::object {
      const auto error = block.validate();
      return error ? py::cast(*error) : py::none();
    })
    .def("wire_encode", [] (const nsf::StreamNameMapBlock& block) {
      return toPyBlockWire(block.wireEncode());
    })
    .def_static("decode", [] (const py::bytes& wire) {
      const auto encoded = blockFromExactPyBytes(wire);
      nsf::StreamNameMapBlock block;
      if (!block.wireDecode(encoded)) {
        throw std::invalid_argument("invalid StreamNameMapBlock wire encoding");
      }
      return block;
    }, py::arg("wire"))
    .def("canonical_content", [] (const nsf::StreamNameMapBlock& block) {
      return toPyBlockWire(block.canonicalContent());
    })
    .def("content_digest", [] (const nsf::StreamNameMapBlock& block) {
      return toPyStreamContentDigest(block.contentDigest());
    })
    .def("fits_signed_wire_budget", &nsf::StreamNameMapBlock::fitsSignedWireBudget,
         py::arg("signed_envelope_overhead"), py::arg("configured_wire_cap"))
    .def("last_cursor", &nsf::StreamNameMapBlock::lastCursor);

  m.def("make_stream_name_map_root",
    [] (const std::string& provider, const std::string& streamId) {
      return nsf::makeStreamNameMapRoot(ndn::Name(provider), streamId).toUri();
    }, py::arg("provider"), py::arg("stream_id"));

  m.def("make_stream_name_map_block_name",
    [] (const std::string& mappingRoot, uint64_t mappingVersion,
        uint64_t blockNumber) {
      return nsf::makeStreamNameMapBlockName(ndn::Name(mappingRoot),
                                             mappingVersion,
                                             blockNumber).toUri();
    }, py::arg("mapping_root"), py::arg("mapping_version"),
       py::arg("block_number"));

  py::class_<nsf::StreamCursorFrontiers>(m, "NativeStreamCursorFrontiers")
    .def(py::init<>())
    .def_readwrite("oldest_retained", &nsf::StreamCursorFrontiers::oldestRetained)
    .def_readwrite("latest_join", &nsf::StreamCursorFrontiers::latestJoin)
    .def_readwrite("latest_produced", &nsf::StreamCursorFrontiers::latestProduced)
    .def_readwrite("mapping_committed_through",
                   &nsf::StreamCursorFrontiers::mappingCommittedThrough)
    .def_readwrite("next_reserved", &nsf::StreamCursorFrontiers::nextReserved)
    .def("validate", [] (const nsf::StreamCursorFrontiers& frontiers,
                         uint64_t blockCapacity,
                         uint64_t checkpointBlock) -> py::object {
      const auto error = frontiers.validate(blockCapacity, checkpointBlock);
      return error ? py::cast(*error) : py::none();
    }, py::arg("block_capacity"), py::arg("checkpoint_block"));

  py::class_<nsf::StreamNameMapCheckpoint>(m, "NativeStreamNameMapCheckpoint")
    .def(py::init<>())
    .def_readwrite("frontiers", &nsf::StreamNameMapCheckpoint::frontiers)
    .def_readwrite("block_number", &nsf::StreamNameMapCheckpoint::blockNumber)
    .def_property("content_digest",
      [] (const nsf::StreamNameMapCheckpoint& checkpoint) {
        return toPyStreamContentDigest(checkpoint.contentDigest);
      },
      [] (nsf::StreamNameMapCheckpoint& checkpoint, const py::bytes& value) {
        checkpoint.contentDigest = streamContentDigestFromPyBytes(value);
      });

  py::class_<nsf::StreamNameMapResolverConfig>(m, "NativeStreamNameMapResolverConfig")
    .def(py::init<>())
    .def_readwrite("contract_version", &nsf::StreamNameMapResolverConfig::contractVersion)
    .def_readwrite("stream_id", &nsf::StreamNameMapResolverConfig::streamId)
    .def_readwrite("session_epoch", &nsf::StreamNameMapResolverConfig::sessionEpoch)
    .def_readwrite("mapping_version", &nsf::StreamNameMapResolverConfig::mappingVersion)
    .def_readwrite("block_capacity", &nsf::StreamNameMapResolverConfig::blockCapacity)
    .def_property("expected_provider",
      [] (const nsf::StreamNameMapResolverConfig& config) {
        return config.expectedProvider.toUri();
      },
      [] (nsf::StreamNameMapResolverConfig& config, const std::string& value) {
        config.expectedProvider = ndn::Name(value);
      })
    .def_property("mapping_root",
      [] (const nsf::StreamNameMapResolverConfig& config) {
        return config.mappingRoot.toUri();
      },
      [] (nsf::StreamNameMapResolverConfig& config, const std::string& value) {
        config.mappingRoot = ndn::Name(value);
      })
    .def_property("payload_prefix",
      [] (const nsf::StreamNameMapResolverConfig& config) {
        return config.payloadPrefix.toUri();
      },
      [] (nsf::StreamNameMapResolverConfig& config, const std::string& value) {
        config.payloadPrefix = ndn::Name(value);
      })
    .def_readwrite("signed_wire_cap", &nsf::StreamNameMapResolverConfig::signedWireCap)
    .def_readwrite("max_verified_blocks",
                   &nsf::StreamNameMapResolverConfig::maxVerifiedBlocks)
    .def_readwrite("max_quarantine_blocks",
                   &nsf::StreamNameMapResolverConfig::maxQuarantineBlocks)
    .def_readwrite("max_reverse_entries",
                   &nsf::StreamNameMapResolverConfig::maxReverseEntries)
    .def_readwrite("max_original_name_wire_bytes",
                   &nsf::StreamNameMapResolverConfig::maxOriginalNameWireBytes);

  py::class_<nsf::VerifiedStreamNameMapData>(m, "NativeVerifiedStreamNameMapData")
    .def(py::init<>())
    .def_property("data_name",
      [] (const nsf::VerifiedStreamNameMapData& input) {
        return input.dataName.toUri();
      },
      [] (nsf::VerifiedStreamNameMapData& input, const std::string& value) {
        input.dataName = ndn::Name(value);
      })
    .def_property("verified_provider",
      [] (const nsf::VerifiedStreamNameMapData& input) {
        return input.verifiedProvider.toUri();
      },
      [] (nsf::VerifiedStreamNameMapData& input, const std::string& value) {
        input.verifiedProvider = ndn::Name(value);
      })
    .def_readwrite("content_type", &nsf::VerifiedStreamNameMapData::contentType)
    .def_readwrite("has_final_block", &nsf::VerifiedStreamNameMapData::hasFinalBlock)
    .def_readwrite("signed_wire_size", &nsf::VerifiedStreamNameMapData::signedWireSize)
    .def_property("content",
      [] (const nsf::VerifiedStreamNameMapData& input) {
        return toPyBlockWire(input.content);
      },
      [] (nsf::VerifiedStreamNameMapData& input, const py::bytes& value) {
        input.content = blockFromExactPyBytes(value);
      })
    .def_readwrite("received_monotonic_ms",
                   &nsf::VerifiedStreamNameMapData::receivedMonotonicMs)
    .def_readwrite("required_before_monotonic_ms",
                   &nsf::VerifiedStreamNameMapData::requiredBeforeMonotonicMs);

  py::enum_<nsf::StreamNameMapAdmissionDisposition>(
      m, "NativeStreamNameMapAdmissionDisposition")
    .value("ADMITTED", nsf::StreamNameMapAdmissionDisposition::Admitted)
    .value("DUPLICATE", nsf::StreamNameMapAdmissionDisposition::Duplicate)
    .value("QUARANTINED", nsf::StreamNameMapAdmissionDisposition::Quarantined)
    .value("REJECTED", nsf::StreamNameMapAdmissionDisposition::Rejected)
    .value("FATAL_SESSION", nsf::StreamNameMapAdmissionDisposition::FatalSession);

  py::enum_<nsf::StreamNameMapTiming>(m, "NativeStreamNameMapTiming")
    .value("UNCLASSIFIED", nsf::StreamNameMapTiming::Unclassified)
    .value("AHEAD", nsf::StreamNameMapTiming::Ahead)
    .value("LATE", nsf::StreamNameMapTiming::Late);

  py::class_<nsf::StreamNameMapAdmissionResult>(
      m, "NativeStreamNameMapAdmissionResult")
    .def_property_readonly("disposition_name",
      [] (const nsf::StreamNameMapAdmissionResult& result) {
        return std::string(nsf::toString(result.disposition));
      })
    .def_property_readonly("timing_name",
      [] (const nsf::StreamNameMapAdmissionResult& result) {
        return std::string(nsf::toString(result.timing));
      })
    .def_readonly("reason", &nsf::StreamNameMapAdmissionResult::reason)
    .def_readonly("state_changed", &nsf::StreamNameMapAdmissionResult::stateChanged)
    .def_readonly("mapping_committed_through",
                  &nsf::StreamNameMapAdmissionResult::mappingCommittedThrough)
    .def_property_readonly("accepted", &nsf::StreamNameMapAdmissionResult::accepted)
    .def_property_readonly("fatal", &nsf::StreamNameMapAdmissionResult::fatal);

  py::class_<nsf::StreamNameMapResolution>(m, "NativeStreamNameMapResolution")
    .def_readonly("cursor", &nsf::StreamNameMapResolution::cursor)
    .def_property_readonly("original_name",
      [] (const nsf::StreamNameMapResolution& resolution) {
        return resolution.originalName.empty() ? std::string()
                                               : resolution.originalName.toUri();
      })
    .def_readonly("tombstone", &nsf::StreamNameMapResolution::tombstone)
    .def_readonly("terminal_unproduced",
                  &nsf::StreamNameMapResolution::terminalUnproduced)
    .def_readonly("group_id", &nsf::StreamNameMapResolution::groupId)
    .def_readonly("sample_class", &nsf::StreamNameMapResolution::sampleClass)
    .def_readonly("group_item_index", &nsf::StreamNameMapResolution::groupItemIndex)
    .def_readonly("predicted_source_items",
                  &nsf::StreamNameMapResolution::predictedSourceItems)
    .def_readonly("predicted_repair_items",
                  &nsf::StreamNameMapResolution::predictedRepairItems)
    .def_property_readonly("timing_name",
      [] (const nsf::StreamNameMapResolution& resolution) {
        return std::string(nsf::toString(resolution.timing));
      })
    .def_property_readonly("schedulable", &nsf::StreamNameMapResolution::schedulable)
    .def_property_readonly("has_group_binding",
                           &nsf::StreamNameMapResolution::hasGroupBinding)
    .def_property_readonly("predicted_group_items",
                           &nsf::StreamNameMapResolution::predictedGroupItems);

  py::class_<nsf::StreamNameResolverState>(m, "NativeStreamNameResolverState")
    .def(py::init<>())
    .def("reset", &nsf::StreamNameResolverState::reset,
         py::arg("config"), py::arg("checkpoint"))
    .def("admit_verified_block", &nsf::StreamNameResolverState::admitVerifiedBlock,
         py::arg("input"))
    .def("admit_verified_wire",
      [] (nsf::StreamNameResolverState& resolver,
          nsf::VerifiedStreamNameMapData input,
          const py::bytes& contentWire) {
        try {
          input.content = blockFromExactPyBytes(contentWire);
        }
        catch (const std::invalid_argument&) {
          // Keep malformed wire on the same structured Core rejection path as
          // C++. The synthetic empty Content is never admitted or exposed.
          input.content = ndn::makeEmptyBlock(ndn::tlv::Content);
        }
        return resolver.admitVerifiedBlock(input);
      }, py::arg("input"), py::arg("content_wire"))
    .def("refresh_checkpoint", &nsf::StreamNameResolverState::refreshCheckpoint,
         py::arg("checkpoint"))
    .def("lookup", &nsf::StreamNameResolverState::lookup,
         py::arg("cursor"))
    .def("resolve", [] (const nsf::StreamNameResolverState& resolver,
                        nsf::StreamCursor cursor) -> py::object {
      const auto name = resolver.resolve(cursor);
      return name ? py::cast(name->toUri()) : py::none();
    }, py::arg("cursor"))
    .def("reverse_resolve", [] (const nsf::StreamNameResolverState& resolver,
                                const std::string& originalName) -> py::object {
      const auto cursor = resolver.reverseResolve(ndn::Name(originalName));
      return cursor ? py::cast(*cursor) : py::none();
    }, py::arg("original_name"))
    .def("mark_terminal_unproduced",
         &nsf::StreamNameResolverState::markTerminalUnproduced,
         py::arg("cursor"))
    .def("evict_local_block", &nsf::StreamNameResolverState::evictLocalBlock,
         py::arg("block_number"))
    .def("frontiers", &nsf::StreamNameResolverState::frontiers)
    .def("checkpoint", &nsf::StreamNameResolverState::checkpoint)
    .def("faulted", &nsf::StreamNameResolverState::faulted)
    .def("verified_block_count", &nsf::StreamNameResolverState::verifiedBlockCount)
    .def("quarantined_block_count",
         &nsf::StreamNameResolverState::quarantinedBlockCount)
    .def("binding_count", &nsf::StreamNameResolverState::bindingCount)
    .def("diagnostics", &nsf::StreamNameResolverState::diagnostics);

  py::class_<nsf::StreamMetrics>(m, "NativeStreamMetrics")
    .def(py::init<>())
    .def_readwrite("produced", &nsf::StreamMetrics::produced)
    .def_readwrite("evicted", &nsf::StreamMetrics::evicted)
    .def_readwrite("received", &nsf::StreamMetrics::received)
    .def_readwrite("emitted", &nsf::StreamMetrics::emitted)
    .def_readwrite("duplicates", &nsf::StreamMetrics::duplicates)
    .def_readwrite("stale", &nsf::StreamMetrics::stale)
    .def_readwrite("gaps", &nsf::StreamMetrics::gaps)
    .def_readwrite("timeouts", &nsf::StreamMetrics::timeouts)
    .def_readwrite("nacks", &nsf::StreamMetrics::nacks)
    .def_readwrite("overflows", &nsf::StreamMetrics::overflows)
    .def_readwrite("max_pending", &nsf::StreamMetrics::maxPending)
    .def_readwrite("bytes_produced", &nsf::StreamMetrics::bytesProduced)
    .def_readwrite("bytes_received", &nsf::StreamMetrics::bytesReceived);

  py::class_<nsf::StreamProducerBuffer>(m, "NativeStreamProducerBuffer")
    .def(py::init<size_t>(), py::arg("max_chunks") = 600)
    .def("put", &nsf::StreamProducerBuffer::put)
    .def("get", &nsf::StreamProducerBuffer::get)
    .def("sequences", &nsf::StreamProducerBuffer::sequences)
    .def("size", &nsf::StreamProducerBuffer::size)
    .def_property_readonly("metrics", &nsf::StreamProducerBuffer::metrics);

  py::class_<nsf::StreamConsumerReorderBuffer>(m, "NativeStreamConsumerReorderBuffer")
    .def(py::init<std::string, uint64_t, uint64_t, size_t, size_t>(),
         py::arg("stream_id"), py::arg("session_epoch"),
         py::arg("next_seq") = 0, py::arg("max_pending") = 512,
         py::arg("history") = 1024)
    .def("reset", &nsf::StreamConsumerReorderBuffer::reset,
         py::arg("stream_id"), py::arg("session_epoch"), py::arg("next_seq") = 0)
    .def("push", &nsf::StreamConsumerReorderBuffer::push)
    .def("missing_sequences", &nsf::StreamConsumerReorderBuffer::missingSequences,
         py::arg("limit") = 32)
    .def("pending_sequences", &nsf::StreamConsumerReorderBuffer::pendingSequences,
         py::arg("limit") = 0)
    .def("drain_ready", &nsf::StreamConsumerReorderBuffer::drainReady)
    .def("skip_to", &nsf::StreamConsumerReorderBuffer::skipTo)
    .def_property_readonly("next_seq", &nsf::StreamConsumerReorderBuffer::nextSeq)
    .def_property_readonly("pending_count", &nsf::StreamConsumerReorderBuffer::pendingCount)
    .def_property_readonly("pending_bytes", &nsf::StreamConsumerReorderBuffer::pendingBytes)
    .def_property_readonly("metrics", &nsf::StreamConsumerReorderBuffer::metrics);

  py::enum_<nsf::StreamPrefetchPhase>(m, "NativeStreamPrefetchPhase")
    .value("INACTIVE", nsf::StreamPrefetchPhase::Inactive)
    .value("CHASING", nsf::StreamPrefetchPhase::Chasing)
    .value("ADJUSTING", nsf::StreamPrefetchPhase::Adjusting)
    .value("FETCHING", nsf::StreamPrefetchPhase::Fetching)
    .value("RECOVERING", nsf::StreamPrefetchPhase::Recovering)
    .value("STOPPED", nsf::StreamPrefetchPhase::Stopped);

  py::class_<nsf::StreamFetchDecision>(m, "NativeStreamFetchDecision")
    .def(py::init<>())
    .def_readwrite("window", &nsf::StreamFetchDecision::window)
    .def_readwrite("lookahead", &nsf::StreamFetchDecision::lookahead)
    .def_readwrite("interest_lifetime_ms", &nsf::StreamFetchDecision::interestLifetimeMs)
    .def_readwrite("missing_timeout_ms", &nsf::StreamFetchDecision::missingTimeoutMs)
    .def_readwrite("sample_demand", &nsf::StreamFetchDecision::sampleDemand)
    .def_readwrite("packet_demand", &nsf::StreamFetchDecision::packetDemand)
    .def_readwrite("hold_ms", &nsf::StreamFetchDecision::holdMs)
    .def_readwrite("recovery_checkpoint_ms", &nsf::StreamFetchDecision::recoveryCheckpointMs)
    .def_readwrite("remaining_recovery_budget_ms", &nsf::StreamFetchDecision::remainingRecoveryBudgetMs)
    .def_readwrite("mapping_begin_block", &nsf::StreamFetchDecision::mappingBeginBlock)
    .def_readwrite("mapping_end_block", &nsf::StreamFetchDecision::mappingEndBlock)
    .def_readwrite("payload_begin_cursor", &nsf::StreamFetchDecision::payloadBeginCursor)
    .def_readwrite("payload_end_cursor", &nsf::StreamFetchDecision::payloadEndCursor)
    .def_readwrite("aggregate_in_flight_limit", &nsf::StreamFetchDecision::aggregateInFlightLimit)
    .def_readwrite("mapping_budget", &nsf::StreamFetchDecision::mappingBudget)
    .def_readwrite("payload_budget", &nsf::StreamFetchDecision::payloadBudget)
    .def_readwrite("retransmission_budget", &nsf::StreamFetchDecision::retransmissionBudget)
    .def_readwrite("future_wait_count", &nsf::StreamFetchDecision::futureWaitCount)
    .def_readwrite("terminal_unproduced_advice", &nsf::StreamFetchDecision::terminalUnproducedAdvice)
    .def_readwrite("later_cursor_advice", &nsf::StreamFetchDecision::laterCursorAdvice)
    .def_readwrite("atomic_expansions", &nsf::StreamFetchDecision::atomicExpansions)
    .def_readwrite("atomic_deferrals", &nsf::StreamFetchDecision::atomicDeferrals)
    .def_readwrite("pressure", &nsf::StreamFetchDecision::pressure)
    .def_readwrite("live_edge_confidence", &nsf::StreamFetchDecision::liveEdgeConfidence)
    .def_readwrite("mapping_ready", &nsf::StreamFetchDecision::mappingReady)
    .def_readwrite("future_wait", &nsf::StreamFetchDecision::futureWait)
    .def_readwrite("congestion_hold", &nsf::StreamFetchDecision::congestionHold)
    .def_readwrite("retransmission_eligible", &nsf::StreamFetchDecision::retransmissionEligible)
    .def_readwrite("phase", &nsf::StreamFetchDecision::phase)
    .def_property_readonly("phase_name", [] (const nsf::StreamFetchDecision& value) {
      return std::string(nsf::toString(value.phase));
    })
    .def_readwrite("policy_mode", &nsf::StreamFetchDecision::policyMode)
    .def_readwrite("detector_profile", &nsf::StreamFetchDecision::detectorProfile)
    .def_readwrite("mapping_wait_reason", &nsf::StreamFetchDecision::mappingWaitReason)
    .def_readwrite("capacity_reason", &nsf::StreamFetchDecision::capacityReason)
    .def_readwrite("reason", &nsf::StreamFetchDecision::reason);

  py::class_<nsf::StreamAdaptiveFetcherState>(m, "NativeStreamAdaptiveFetcherState")
    .def(py::init<>())
    .def_readwrite("rtt_ms", &nsf::StreamAdaptiveFetcherState::rttMs)
    .def_readwrite("timeout_pressure", &nsf::StreamAdaptiveFetcherState::timeoutPressure)
    .def_readwrite("nack_pressure", &nsf::StreamAdaptiveFetcherState::nackPressure)
    .def_readwrite("duplicate_pressure", &nsf::StreamAdaptiveFetcherState::duplicatePressure)
    .def_readwrite("backlog_pressure", &nsf::StreamAdaptiveFetcherState::backlogPressure)
    .def_readwrite("min_window", &nsf::StreamAdaptiveFetcherState::minWindow)
    .def_readwrite("base_window", &nsf::StreamAdaptiveFetcherState::baseWindow)
    .def_readwrite("max_window", &nsf::StreamAdaptiveFetcherState::maxWindow)
    .def_readwrite("min_lookahead", &nsf::StreamAdaptiveFetcherState::minLookahead)
    .def_readwrite("base_lookahead", &nsf::StreamAdaptiveFetcherState::baseLookahead)
    .def_readwrite("max_lookahead", &nsf::StreamAdaptiveFetcherState::maxLookahead)
    .def_readwrite("min_interest_lifetime_ms", &nsf::StreamAdaptiveFetcherState::minInterestLifetimeMs)
    .def_readwrite("max_interest_lifetime_ms", &nsf::StreamAdaptiveFetcherState::maxInterestLifetimeMs)
    .def_readwrite("min_missing_timeout_ms", &nsf::StreamAdaptiveFetcherState::minMissingTimeoutMs)
    .def_readwrite("max_missing_timeout_ms", &nsf::StreamAdaptiveFetcherState::maxMissingTimeoutMs)
    .def_readwrite("live_edge_change_threshold", &nsf::StreamAdaptiveFetcherState::liveEdgeChangeThreshold)
    .def_readwrite("live_edge_period_similarity", &nsf::StreamAdaptiveFetcherState::liveEdgePeriodSimilarity)
    .def_readwrite("live_edge_window", &nsf::StreamAdaptiveFetcherState::liveEdgeWindow)
    .def_readwrite("live_edge_stable_required", &nsf::StreamAdaptiveFetcherState::liveEdgeStableRequired)
    .def_readwrite("detection_period_ms", &nsf::StreamAdaptiveFetcherState::detectionPeriodMs)
    .def_readwrite("recovery_reserve_packets", &nsf::StreamAdaptiveFetcherState::recoveryReservePackets)
    .def_readwrite("aggregate_in_flight_limit", &nsf::StreamAdaptiveFetcherState::aggregateInFlightLimit)
    .def_readwrite("mapping_reserve", &nsf::StreamAdaptiveFetcherState::mappingReserve)
    .def_readwrite("retransmission_reserve", &nsf::StreamAdaptiveFetcherState::retransmissionReserve)
    .def_readwrite("mapping_block_capacity", &nsf::StreamAdaptiveFetcherState::mappingBlockCapacity)
    .def_readwrite("chase_multiplier", &nsf::StreamAdaptiveFetcherState::chaseMultiplier)
    .def_readwrite("adjust_multiplier", &nsf::StreamAdaptiveFetcherState::adjustMultiplier)
    .def_readwrite("congestion_decrease_multiplier", &nsf::StreamAdaptiveFetcherState::congestionDecreaseMultiplier)
    .def_readwrite("detector_profile", &nsf::StreamAdaptiveFetcherState::detectorProfile)
    .def("observe_rtt", &nsf::StreamAdaptiveFetcherState::observeRtt,
         py::arg("sample_ms"), py::arg("alpha") = 0.25)
    .def("record_timeout", py::overload_cast<>(
           &nsf::StreamAdaptiveFetcherState::recordTimeout))
    .def("record_timeout_evidence", py::overload_cast<uint64_t, bool, bool>(
           &nsf::StreamAdaptiveFetcherState::recordTimeout),
         py::arg("cursor"), py::arg("known_produced"), py::arg("was_future"))
    .def("record_nack", py::overload_cast<>(
           &nsf::StreamAdaptiveFetcherState::recordNack))
    .def("record_nack_reason", py::overload_cast<uint64_t, const std::string&>(
           &nsf::StreamAdaptiveFetcherState::recordNack),
         py::arg("cursor"), py::arg("reason"))
    .def("record_congestion_mark", &nsf::StreamAdaptiveFetcherState::recordCongestionMark,
         py::arg("cursor"), py::arg("mark"))
    .def("record_duplicate", &nsf::StreamAdaptiveFetcherState::recordDuplicate)
    .def("set_backlog_pressure", &nsf::StreamAdaptiveFetcherState::setBacklogPressure)
    .def("decay", &nsf::StreamAdaptiveFetcherState::decay, py::arg("factor") = 0.85)
    .def("reset_live", &nsf::StreamAdaptiveFetcherState::resetLive,
         py::arg("session_epoch"), py::arg("next_seq"), py::arg("sample_period_ms"),
         py::arg("now_ms") = 0)
    .def("configure_mapped_live", &nsf::StreamAdaptiveFetcherState::configureMappedLive,
         py::arg("aggregate_limit"), py::arg("mapping_reserve"),
         py::arg("retransmission_reserve"), py::arg("block_capacity"),
         py::arg("detector_profile"))
    .def("reset_mapped_live", &nsf::StreamAdaptiveFetcherState::resetMappedLive,
         py::arg("session_epoch"), py::arg("next_cursor"),
         py::arg("sample_period_ms"), py::arg("latest_produced_cursor"),
         py::arg("mapping_committed_through_cursor"),
         py::arg("next_reserved_cursor"), py::arg("now_ms") = 0)
    .def("update_mapping_frontier", &nsf::StreamAdaptiveFetcherState::updateMappingFrontier,
         py::arg("mapping_committed_through_cursor"),
         py::arg("next_reserved_cursor"))
    .def("advance_next_cursor", &nsf::StreamAdaptiveFetcherState::advanceNextCursor,
         py::arg("next_cursor"))
    .def("set_mapped_live_policy_enabled",
         &nsf::StreamAdaptiveFetcherState::setMappedLivePolicyEnabled,
         py::arg("enabled"))
    .def("set_in_flight", &nsf::StreamAdaptiveFetcherState::setInFlight,
         py::arg("mapping"), py::arg("payload"), py::arg("retransmission"))
    .def("observe_accepted_sample", &nsf::StreamAdaptiveFetcherState::observeAcceptedSample,
         py::arg("session_epoch"), py::arg("sample_id"), py::arg("arrival_ms"),
         py::arg("retrieval_delay_ms"), py::arg("segment_count") = 1,
         py::arg("known_produced") = true)
    .def("observe_sample_extent", &nsf::StreamAdaptiveFetcherState::observeSampleExtent,
         py::arg("predicted_count"), py::arg("actual_count"))
    .def("begin_recovery", &nsf::StreamAdaptiveFetcherState::beginRecovery,
         py::arg("now_ms"), py::arg("playout_deadline_ms"))
    .def("record_recovery", &nsf::StreamAdaptiveFetcherState::recordRecovery,
         py::arg("completed"))
    .def("record_invalid_observation", &nsf::StreamAdaptiveFetcherState::recordInvalidObservation)
    .def("stop_live", &nsf::StreamAdaptiveFetcherState::stopLive)
    .def_property_readonly("phase_name", [] (const nsf::StreamAdaptiveFetcherState& value) {
      return std::string(nsf::toString(value.phase()));
    })
    .def_property_readonly("invalid_observations", &nsf::StreamAdaptiveFetcherState::invalidObservations)
    .def("decide", &nsf::StreamAdaptiveFetcherState::decide,
         py::arg("now_ms") = 0, py::arg("playout_deadline_ms") = 0);

  py::enum_<nsf::LiveStreamFecScheme>(m, "NativeLiveStreamFecScheme")
    .value("NONE", nsf::LiveStreamFecScheme::None)
    .value("XOR_ONE_REPAIR", nsf::LiveStreamFecScheme::XorOneRepair)
    .value("GF256_TWO_REPAIR", nsf::LiveStreamFecScheme::Gf256TwoRepair);

  py::class_<nsf::SampleClassProfile>(m, "NativeSampleClassProfile")
    .def(py::init<>())
    .def_readwrite("class_id", &nsf::SampleClassProfile::classId)
    .def_readwrite("seed_source_items", &nsf::SampleClassProfile::seedSourceItems)
    .def_readwrite("hard_max_source_items",
                   &nsf::SampleClassProfile::hardMaxSourceItems)
    .def_readwrite("history_capacity", &nsf::SampleClassProfile::historyCapacity)
    .def_readwrite("safety_margin_items",
                   &nsf::SampleClassProfile::safetyMarginItems)
    .def_static("bounded", &nsf::SampleClassProfile::bounded,
                py::arg("class_id"), py::arg("seed_source_items"),
                py::arg("hard_max_source_items"),
                py::arg("history_capacity") = 32,
                py::arg("safety_margin_items") = 1)
    .def("validate", [] (const nsf::SampleClassProfile& value) -> py::object {
      const auto error = value.validate();
      return error ? py::cast(*error) : py::none();
    });

  py::class_<nsf::SampleClassPredictionStatus>(m,
      "NativeSampleClassPredictionStatus")
    .def_readonly("class_id", &nsf::SampleClassPredictionStatus::classId)
    .def_readonly("prediction", &nsf::SampleClassPredictionStatus::prediction)
    .def_readonly("observations", &nsf::SampleClassPredictionStatus::observations)
    .def_readonly("underpredictions",
                  &nsf::SampleClassPredictionStatus::underpredictions)
    .def_readonly("underpredicted_items",
                  &nsf::SampleClassPredictionStatus::underpredictedItems)
    .def_readonly("overpredictions",
                  &nsf::SampleClassPredictionStatus::overpredictions)
    .def_readonly("overpredicted_items",
                  &nsf::SampleClassPredictionStatus::overpredictedItems);

  py::class_<nsf::LiveStreamSamplePredictor>(m, "NativeLiveStreamSamplePredictor")
    .def(py::init<std::vector<nsf::SampleClassProfile>>(),
         py::arg("profiles") = std::vector<nsf::SampleClassProfile>{})
    .def("reset", &nsf::LiveStreamSamplePredictor::reset)
    .def("predict", [] (const nsf::LiveStreamSamplePredictor& predictor,
                         const std::string& classId) {
      return predictor.predict(classId);
    })
    .def("observe", &nsf::LiveStreamSamplePredictor::observe)
    .def("status", &nsf::LiveStreamSamplePredictor::status)
    .def("statuses", &nsf::LiveStreamSamplePredictor::statuses);

  py::class_<nsf::LiveStreamFecOptions>(m, "NativeLiveStreamFecOptions")
    .def(py::init<>())
    .def_readwrite("scheme", &nsf::LiveStreamFecOptions::scheme)
    .def_readwrite("max_source_items", &nsf::LiveStreamFecOptions::maxSourceItems)
    .def_property("source_items",
      [] (const nsf::LiveStreamFecOptions& value) { return value.maxSourceItems; },
      [] (nsf::LiveStreamFecOptions& value, size_t count) { value.maxSourceItems = count; })
    .def_readwrite("max_source_bytes", &nsf::LiveStreamFecOptions::maxSourceBytes)
    .def_readwrite("recovery_budget_ms", &nsf::LiveStreamFecOptions::recoveryBudgetMs)
    .def_readwrite("repair_symbols", &nsf::LiveStreamFecOptions::repairSymbols)
    .def_static("none", &nsf::LiveStreamFecOptions::none)
    .def_static("xor_one_repair", &nsf::LiveStreamFecOptions::xorOneRepair,
                py::arg("source_items"), py::arg("max_source_bytes"),
                py::arg("recovery_budget_ms") = 500)
    .def_static("gf256_two_repair", &nsf::LiveStreamFecOptions::gf256TwoRepair,
                py::arg("source_items"), py::arg("max_source_bytes"),
                py::arg("recovery_budget_ms") = 500)
    .def_property_readonly("recovery_capacity", &nsf::LiveStreamFecOptions::recoveryCapacity)
    .def_property_readonly("enabled", &nsf::LiveStreamFecOptions::enabled)
    .def("validate", [] (const nsf::LiveStreamFecOptions& value) -> py::object {
      const auto error = value.validate();
      return error ? py::cast(*error) : py::none();
	    });

  py::class_<nsf::StreamAdvancedOptions>(m, "NativeStreamAdvancedOptions")
    .def(py::init<>())
    .def_readwrite("mapping_block_capacity",
                   &nsf::StreamAdvancedOptions::mappingBlockCapacity)
    .def_readwrite("mapping_ahead_blocks",
                   &nsf::StreamAdvancedOptions::mappingAheadBlocks)
    .def_readwrite("retained_items",
                   &nsf::StreamAdvancedOptions::retainedItems)
    .def_readwrite("max_name_reservations",
                   &nsf::StreamAdvancedOptions::maxNameReservations)
    .def_readwrite("max_pending_interests",
                   &nsf::StreamAdvancedOptions::maxPendingInterests)
    .def_readwrite("signed_wire_cap",
                   &nsf::StreamAdvancedOptions::signedWireCap)
    .def_readwrite("startup_timeout_ms",
                   &nsf::StreamAdvancedOptions::startupTimeoutMs);

  py::class_<nsf::StreamConfig>(m, "NativeStreamConfig")
    .def(py::init<>())
    .def_readwrite("stream_id", &nsf::StreamConfig::streamId)
    .def_property("data_prefix",
      [] (const nsf::StreamConfig& value) { return value.dataPrefix.toUri(); },
      [] (nsf::StreamConfig& value, const std::string& name) {
        value.dataPrefix = ndn::Name(name);
      })
    .def_readwrite("sample_period_ms", &nsf::StreamConfig::samplePeriodMs)
    .def_readwrite("sample_classes", &nsf::StreamConfig::sampleClasses)
    .def_readwrite("fec", &nsf::StreamConfig::fec)
    .def_readwrite("session_epoch", &nsf::StreamConfig::sessionEpoch)
    .def_readwrite("advanced", &nsf::StreamConfig::advanced);

  py::class_<nsf::LiveStreamDefinition>(m, "NativeLiveStreamDefinition")
    .def(py::init<>())
    .def_readwrite("contract_version", &nsf::LiveStreamDefinition::contractVersion)
    .def_readwrite("stream_id", &nsf::LiveStreamDefinition::streamId)
    .def_property("provider",
      [] (const nsf::LiveStreamDefinition& value) { return value.provider.toUri(); },
      [] (nsf::LiveStreamDefinition& value, const std::string& name) { value.provider = ndn::Name(name); })
    .def_property("semantic_data_prefix",
      [] (const nsf::LiveStreamDefinition& value) { return value.semanticDataPrefix.toUri(); },
      [] (nsf::LiveStreamDefinition& value, const std::string& name) { value.semanticDataPrefix = ndn::Name(name); })
    .def_readwrite("session_epoch", &nsf::LiveStreamDefinition::sessionEpoch)
    .def_readwrite("mapping_version", &nsf::LiveStreamDefinition::mappingVersion)
    .def_readwrite("mapping_block_capacity", &nsf::LiveStreamDefinition::mappingBlockCapacity)
    .def_readwrite("mapping_ahead_blocks", &nsf::LiveStreamDefinition::mappingAheadBlocks)
    .def_readwrite("retained_items", &nsf::LiveStreamDefinition::retainedItems)
    .def_readwrite("max_name_reservations", &nsf::LiveStreamDefinition::maxNameReservations)
    .def_readwrite("max_pending_interests", &nsf::LiveStreamDefinition::maxPendingInterests)
    .def_readwrite("signed_wire_cap", &nsf::LiveStreamDefinition::signedWireCap)
    .def_readwrite("sample_period_ms", &nsf::LiveStreamDefinition::samplePeriodMs)
    .def_readwrite("sample_classes", &nsf::LiveStreamDefinition::sampleClasses)
    .def_readwrite("fec", &nsf::LiveStreamDefinition::fec)
    .def_property_readonly("mapping_root", [] (const nsf::LiveStreamDefinition& value) {
      return value.mappingRoot().toUri();
    })
    .def("validate", [] (const nsf::LiveStreamDefinition& value) -> py::object {
      const auto error = value.validate();
      return error ? py::cast(*error) : py::none();
    });

  py::class_<nsf::LiveStreamItemReservation>(m, "NativeLiveStreamItemReservation")
    .def(py::init<>())
    .def_readonly("cursor", &nsf::LiveStreamItemReservation::cursor)
    .def_property_readonly("original_name", [] (const nsf::LiveStreamItemReservation& value) {
      return value.originalName.toUri();
    })
    .def_readonly("session_epoch", &nsf::LiveStreamItemReservation::sessionEpoch)
    .def_readonly("mapping_version", &nsf::LiveStreamItemReservation::mappingVersion);

  py::class_<nsf::LiveStreamGroupReservation>(m, "NativeLiveStreamGroupReservation")
    .def_property_readonly("group_id", [] (const nsf::LiveStreamGroupReservation& value) {
      return value.groupId;
    })
    .def_readonly("sources", &nsf::LiveStreamGroupReservation::sources)
    .def_readonly("repairs", &nsf::LiveStreamGroupReservation::repairs);

  py::enum_<nsf::LiveStreamItemKind>(m, "NativeLiveStreamItemKind")
    .value("SOURCE", nsf::LiveStreamItemKind::Source)
    .value("REPAIR", nsf::LiveStreamItemKind::Repair);

  py::class_<nsf::LiveStreamSampleReservation>(m,
      "NativeLiveStreamSampleReservation")
    .def_readonly("sample_id", &nsf::LiveStreamSampleReservation::sampleId)
    .def_readonly("sample_class", &nsf::LiveStreamSampleReservation::sampleClass)
    .def_readonly("predicted_source_items",
                  &nsf::LiveStreamSampleReservation::predictedSourceItems)
    .def_readonly("group", &nsf::LiveStreamSampleReservation::group);

  py::class_<nsf::LiveStreamReadiness>(m, "NativeLiveStreamReadiness")
    .def(py::init<>())
    .def_readwrite("measured_sample_period_ms", &nsf::LiveStreamReadiness::measuredSamplePeriodMs)
    .def_readwrite("safe_join_cursor", &nsf::LiveStreamReadiness::safeJoinCursor);

  py::class_<nsf::LiveStreamDescriptor>(m, "NativeLiveStreamDescriptor")
    .def(py::init<>())
    .def_readwrite("definition", &nsf::LiveStreamDescriptor::definition)
    .def_readwrite("checkpoint", &nsf::LiveStreamDescriptor::checkpoint)
    .def_readwrite("measured_sample_period_ms", &nsf::LiveStreamDescriptor::measuredSamplePeriodMs)
    .def_readwrite("safe_join_cursor", &nsf::LiveStreamDescriptor::safeJoinCursor)
    .def("validate", [] (const nsf::LiveStreamDescriptor& value) -> py::object {
      const auto error = value.validate();
      return error ? py::cast(*error) : py::none();
    });

  py::enum_<nsf::LiveStreamLifecycleState>(m, "NativeLiveStreamLifecycleState")
    .value("PREPARING", nsf::LiveStreamLifecycleState::Preparing)
    .value("ACTIVE", nsf::LiveStreamLifecycleState::Active)
    .value("STOPPED", nsf::LiveStreamLifecycleState::Stopped)
    .value("FAILED", nsf::LiveStreamLifecycleState::Failed);

  py::enum_<nsf::LiveStreamItemProvenance>(m, "NativeLiveStreamItemProvenance")
    .value("SIGNED_DATA", nsf::LiveStreamItemProvenance::SignedData)
    .value("FEC_RECOVERED", nsf::LiveStreamItemProvenance::FecRecovered);

  py::class_<nsf::VerifiedLiveStreamItem>(m, "NativeVerifiedLiveStreamItem")
    .def_readonly("cursor", &nsf::VerifiedLiveStreamItem::cursor)
    .def_property_readonly("original_name", [] (const nsf::VerifiedLiveStreamItem& value) {
      return value.originalName.toUri();
    })
    .def_property_readonly("verified_provider", [] (const nsf::VerifiedLiveStreamItem& value) {
      return value.verifiedProvider.toUri();
    })
    .def_property_readonly("content", [] (const nsf::VerifiedLiveStreamItem& value) {
      return py::bytes(reinterpret_cast<const char*>(value.content.data()), value.content.size());
    })
    .def_readonly("provenance", &nsf::VerifiedLiveStreamItem::provenance)
    .def_readonly("received_ms", &nsf::VerifiedLiveStreamItem::receivedMs);

  py::class_<nsf::LiveStreamItemAdmission>(m, "NativeLiveStreamItemAdmission")
    .def_readonly("accepted", &nsf::LiveStreamItemAdmission::accepted)
    .def_readonly("reason", &nsf::LiveStreamItemAdmission::reason)
    .def_static("accept_item", &nsf::LiveStreamItemAdmission::acceptItem)
    .def_static("reject_item", &nsf::LiveStreamItemAdmission::rejectItem);

  py::class_<nsf::LiveStreamSampleObservation>(m, "NativeLiveStreamSampleObservation")
    .def(py::init<>())
    .def_readwrite("sample_id", &nsf::LiveStreamSampleObservation::sampleId)
    .def_readwrite("arrival_ms", &nsf::LiveStreamSampleObservation::arrivalMs)
    .def_readwrite("retrieval_delay_ms", &nsf::LiveStreamSampleObservation::retrievalDelayMs)
    .def_readwrite("item_count", &nsf::LiveStreamSampleObservation::itemCount);

  py::class_<nsf::LiveStreamStatus>(m, "NativeLiveStreamStatus")
    .def_readonly("state", &nsf::LiveStreamStatus::state)
    .def_readonly("frontiers", &nsf::LiveStreamStatus::frontiers)
    .def_readonly("retained_items", &nsf::LiveStreamStatus::retainedItems)
    .def_readonly("pending_interests", &nsf::LiveStreamStatus::pendingInterests)
    .def_readonly("mapping_blocks", &nsf::LiveStreamStatus::mappingBlocks)
    .def_readonly("in_flight", &nsf::LiveStreamStatus::inFlight)
    .def_readonly("delivered", &nsf::LiveStreamStatus::delivered)
    .def_readonly("rejected", &nsf::LiveStreamStatus::rejected)
    .def_readonly("recovered", &nsf::LiveStreamStatus::recovered)
    .def_readonly("timeouts", &nsf::LiveStreamStatus::timeouts)
    .def_readonly("nacks", &nsf::LiveStreamStatus::nacks)
    .def_readonly("retry_attempts", &nsf::LiveStreamStatus::retryAttempts)
    .def_readonly("late_arrivals", &nsf::LiveStreamStatus::lateArrivals)
    .def_readonly("deadline_skips", &nsf::LiveStreamStatus::deadlineSkips)
    .def_readonly("retry_exhaustions", &nsf::LiveStreamStatus::retryExhaustions)
    .def_readonly("mapping_interests", &nsf::LiveStreamStatus::mappingInterests)
    .def_readonly("mapping_data_responses", &nsf::LiveStreamStatus::mappingDataResponses)
    .def_readonly("mapping_new_data_responses",
                  &nsf::LiveStreamStatus::mappingNewDataResponses)
    .def_readonly("payload_interests", &nsf::LiveStreamStatus::payloadInterests)
    .def_readonly("initial_payload_interests", &nsf::LiveStreamStatus::initialPayloadInterests)
    .def_readonly("retry_payload_interests", &nsf::LiveStreamStatus::retryPayloadInterests)
    .def_readonly("payload_source_interests", &nsf::LiveStreamStatus::payloadSourceInterests)
    .def_readonly("initial_payload_source_interests",
                  &nsf::LiveStreamStatus::initialPayloadSourceInterests)
    .def_readonly("retry_payload_source_interests",
                  &nsf::LiveStreamStatus::retryPayloadSourceInterests)
    .def_readonly("payload_repair_interests", &nsf::LiveStreamStatus::payloadRepairInterests)
    .def_readonly("initial_payload_repair_interests",
                  &nsf::LiveStreamStatus::initialPayloadRepairInterests)
    .def_readonly("retry_payload_repair_interests",
                  &nsf::LiveStreamStatus::retryPayloadRepairInterests)
    .def_readonly("payload_unclassified_interests",
                  &nsf::LiveStreamStatus::payloadUnclassifiedInterests)
    .def_readonly("payload_source_data_admissions",
                  &nsf::LiveStreamStatus::payloadSourceDataAdmissions)
    .def_readonly("payload_repair_data_responses",
                  &nsf::LiveStreamStatus::payloadRepairDataResponses)
    .def_readonly("payload_repair_data_consumed",
                  &nsf::LiveStreamStatus::payloadRepairDataConsumed)
    .def_readonly("payload_application_useful_interests",
                  &nsf::LiveStreamStatus::payloadApplicationUsefulInterests)
    .def_readonly("payload_protection_only_interests",
                  &nsf::LiveStreamStatus::payloadProtectionOnlyInterests)
    .def_readonly("payload_nonproductive_interests",
                  &nsf::LiveStreamStatus::payloadNonproductiveInterests)
    .def_readonly("payload_unresolved_interests",
                  &nsf::LiveStreamStatus::payloadUnresolvedInterests)
    .def_readonly("future_payload_interests", &nsf::LiveStreamStatus::futurePayloadInterests)
    .def_readonly("initial_future_payload_interests",
                  &nsf::LiveStreamStatus::initialFuturePayloadInterests)
    .def_readonly("retry_future_payload_interests",
                  &nsf::LiveStreamStatus::retryFuturePayloadInterests)
    .def_readonly("future_cursor_horizon",
                  &nsf::LiveStreamStatus::futureCursorHorizon)
    .def_readonly("retry_successes", &nsf::LiveStreamStatus::retrySuccesses)
    .def_readonly("retry_suppressions", &nsf::LiveStreamStatus::retrySuppressions)
    .def_readonly("retry_suppression_reasons",
                  &nsf::LiveStreamStatus::retrySuppressionReasons)
    .def_readonly("declared_recovery_capacity",
                  &nsf::LiveStreamStatus::declaredRecoveryCapacity)
    .def_readonly("recovery_eligible_sources",
                  &nsf::LiveStreamStatus::recoveryEligibleSources)
    .def_readonly("terminal_missing_sources",
                  &nsf::LiveStreamStatus::terminalMissingSources)
    .def_readonly("recoverable_groups",
                  &nsf::LiveStreamStatus::recoverableGroups)
    .def_readonly("recovered_groups",
                  &nsf::LiveStreamStatus::recoveredGroups)
    .def_readonly("recovery_attempts", &nsf::LiveStreamStatus::recoveryAttempts)
    .def_readonly("recovery_exhaustions", &nsf::LiveStreamStatus::recoveryExhaustions)
    .def_readonly("recovery_control_interests",
                  &nsf::LiveStreamStatus::recoveryControlInterests)
    .def_readonly("recovery_frontier_interests",
                  &nsf::LiveStreamStatus::recoveryFrontierInterests)
    .def_readonly("recovery_group_interests",
                  &nsf::LiveStreamStatus::recoveryGroupInterests)
    .def_readonly("recovery_coalesced_waiters",
                  &nsf::LiveStreamStatus::recoveryCoalescedWaiters)
    .def_readonly("recovery_metadata_cache_hits",
                  &nsf::LiveStreamStatus::recoveryMetadataCacheHits)
    .def_readonly("next_deliver_cursor",
                  &nsf::LiveStreamStatus::nextDeliverCursor)
    .def_readonly("ready_queue_depth",
                  &nsf::LiveStreamStatus::readyQueueDepth)
    .def_readonly("oldest_ready_cursor",
                  &nsf::LiveStreamStatus::oldestReadyCursor)
    .def_readonly("terminal_gap_queue_depth",
                  &nsf::LiveStreamStatus::terminalGapQueueDepth)
    .def_readonly("drain_wake_count",
                  &nsf::LiveStreamStatus::drainWakeCount)
    .def_readonly("stale_ready_drops",
                  &nsf::LiveStreamStatus::staleReadyDrops)
    .def_readonly("terminal_gap_superseded",
                  &nsf::LiveStreamStatus::terminalGapSuperseded)
    .def_readonly("mapping_bytes", &nsf::LiveStreamStatus::mappingBytes)
    .def_readonly("provider_future_interests", &nsf::LiveStreamStatus::providerFutureInterests)
    .def_readonly("provider_future_hits", &nsf::LiveStreamStatus::providerFutureHits)
    .def_readonly("provider_initial_future_interests",
                  &nsf::LiveStreamStatus::providerInitialFutureInterests)
    .def_readonly("provider_initial_future_hits",
                  &nsf::LiveStreamStatus::providerInitialFutureHits)
    .def_readonly("provider_retry_future_interests",
                  &nsf::LiveStreamStatus::providerRetryFutureInterests)
    .def_readonly("provider_retry_future_hits",
                  &nsf::LiveStreamStatus::providerRetryFutureHits)
    .def_readonly("sample_class_predictions",
                  &nsf::LiveStreamStatus::sampleClassPredictions)
    .def_readonly("reason", &nsf::LiveStreamStatus::reason)
    .def_readonly("fetch_decision", &nsf::LiveStreamStatus::fetchDecision);

  py::enum_<nsf::PublishedLiveStreamPacketKind>(m, "NativePublishedLiveStreamPacketKind")
    .value("MAPPING", nsf::PublishedLiveStreamPacketKind::Mapping)
    .value("SOURCE", nsf::PublishedLiveStreamPacketKind::Source)
    .value("REPAIR", nsf::PublishedLiveStreamPacketKind::Repair);

  py::class_<nsf::PublishedLiveStreamPacket>(m, "NativePublishedLiveStreamPacket")
    .def_readonly("kind", &nsf::PublishedLiveStreamPacket::kind)
    .def_readonly("stream_id", &nsf::PublishedLiveStreamPacket::streamId)
    .def_readonly("session_epoch", &nsf::PublishedLiveStreamPacket::sessionEpoch)
    .def_readonly("mapping_version", &nsf::PublishedLiveStreamPacket::mappingVersion)
    .def_property_readonly("cursor", [] (const nsf::PublishedLiveStreamPacket& value) -> py::object {
      return value.cursor ? py::cast(*value.cursor) : py::none();
    })
    .def_property_readonly("data_name", [] (const nsf::PublishedLiveStreamPacket& value) {
      return value.dataName.toUri();
    })
    .def_property_readonly("provider", [] (const nsf::PublishedLiveStreamPacket& value) {
      return value.provider.toUri();
    })
    .def_property_readonly("signed_data_wire", [] (const nsf::PublishedLiveStreamPacket& value) {
      return py::bytes(reinterpret_cast<const char*>(value.signedDataWire.data()),
                       value.signedDataWire.size());
    })
    .def_property_readonly("wire_digest", [] (const nsf::PublishedLiveStreamPacket& value) {
      return py::bytes(reinterpret_cast<const char*>(value.wireDigest.data()), value.wireDigest.size());
    })
    .def_readonly("materialized_monotonic_us",
                  &nsf::PublishedLiveStreamPacket::materializedMonotonicUs);

  py::class_<nsf::PublishedPacketFeedOptions>(m, "NativePublishedPacketFeedOptions")
    .def(py::init<>())
    .def_readwrite("from_cursor", &nsf::PublishedPacketFeedOptions::fromCursor)
    .def_readwrite("max_queued_packets", &nsf::PublishedPacketFeedOptions::maxQueuedPackets)
    .def_readwrite("max_queued_bytes", &nsf::PublishedPacketFeedOptions::maxQueuedBytes);

  py::class_<nsf::PublishedPacketFeedStatus>(m, "NativePublishedPacketFeedStatus")
    .def_readonly("queued_packets", &nsf::PublishedPacketFeedStatus::queuedPackets)
    .def_readonly("queued_bytes", &nsf::PublishedPacketFeedStatus::queuedBytes)
    .def_readonly("dropped_packets", &nsf::PublishedPacketFeedStatus::droppedPackets)
    .def_readonly("first_dropped_cursor", &nsf::PublishedPacketFeedStatus::firstDroppedCursor)
    .def_readonly("last_dropped_cursor", &nsf::PublishedPacketFeedStatus::lastDroppedCursor)
    .def_readonly("closed", &nsf::PublishedPacketFeedStatus::closed);

  py::class_<nsf::PublishedPacketFeed, std::shared_ptr<nsf::PublishedPacketFeed>>(
      m, "NativePublishedPacketFeed")
    .def("take_available", &nsf::PublishedPacketFeed::takeAvailable)
    .def("status", &nsf::PublishedPacketFeed::status)
    .def("close", &nsf::PublishedPacketFeed::close);

  py::class_<nsf::LiveStreamPublisher, std::shared_ptr<nsf::LiveStreamPublisher>>(
      m, "NativeLiveStreamPublisher")
    .def("reserve_ahead", [] (nsf::LiveStreamPublisher& publisher, const std::string& name) {
      return publisher.reserveAhead(ndn::Name(name));
    })
    .def("reserve_many_ahead", [] (nsf::LiveStreamPublisher& publisher,
                                    const std::vector<std::string>& names) {
      std::vector<ndn::Name> converted;
      for (const auto& name : names) converted.emplace_back(name);
      return publisher.reserveAhead(converted);
    })
    .def("reserve_group", [] (nsf::LiveStreamPublisher& publisher,
                               const std::string& groupId,
                               const std::vector<std::string>& sourceNames,
                               const std::vector<std::string>& repairNames) {
      std::vector<ndn::Name> sources;
      std::vector<ndn::Name> repairs;
      for (const auto& name : sourceNames) sources.emplace_back(name);
      for (const auto& name : repairNames) repairs.emplace_back(name);
      return publisher.reserveGroup(groupId, sources, repairs);
    })
    .def("announce_sample", [] (nsf::LiveStreamPublisher& publisher,
                                  uint64_t sampleId,
                                  const std::string& sampleClass,
                                  const py::function& nameFactory) {
      return publisher.announceSample(
        sampleId, sampleClass,
        [nameFactory] (size_t index, nsf::LiveStreamItemKind kind) {
          py::gil_scoped_acquire acquire;
          const auto kindName = kind == nsf::LiveStreamItemKind::Source ?
            "source" : "repair";
          return ndn::Name(nameFactory(index, kindName).cast<std::string>());
        });
    }, py::arg("sample_id"), py::arg("sample_class"), py::arg("name_factory"))
    .def("prepare_sample_extent", &nsf::LiveStreamPublisher::prepareSampleExtent,
         py::arg("reservation"), py::arg("actual_source_items"))
    .def("publish", [] (nsf::LiveStreamPublisher& publisher,
                         const nsf::LiveStreamItemReservation& reservation,
                         const py::bytes& content) {
      const auto buffer = toBuffer(content);
      publisher.publish(reservation, std::vector<uint8_t>(buffer.begin(), buffer.end()));
    })
    .def("publish_group", [] (nsf::LiveStreamPublisher& publisher,
                               const nsf::LiveStreamGroupReservation& reservation,
                               const std::vector<py::bytes>& contents) {
      std::vector<std::vector<uint8_t>> converted;
      for (const auto& content : contents) {
        const auto buffer = toBuffer(content);
        converted.emplace_back(buffer.begin(), buffer.end());
      }
      publisher.publishGroup(reservation, converted);
    })
    .def("publish_sample", [] (nsf::LiveStreamPublisher& publisher,
                                 const nsf::LiveStreamSampleReservation& reservation,
                                 const std::vector<py::bytes>& contents) {
      std::vector<std::vector<uint8_t>> converted;
      for (const auto& content : contents) {
        const auto buffer = toBuffer(content);
        converted.emplace_back(buffer.begin(), buffer.end());
      }
      publisher.publishSample(reservation, converted);
    })
    .def("activate", &nsf::LiveStreamPublisher::activate)
    .def("open_published_packet_feed", &nsf::LiveStreamPublisher::openPublishedPacketFeed)
    .def("status", &nsf::LiveStreamPublisher::status)
	    .def("stop", &nsf::LiveStreamPublisher::stop);

  py::class_<nsf::StreamPublisher, std::shared_ptr<nsf::StreamPublisher>>(
      m, "NativeStreamPublisher")
    .def("start", [] (nsf::StreamPublisher& publisher) {
      py::gil_scoped_release release;
      return publisher.start();
    })
    .def("push", [] (nsf::StreamPublisher& publisher, const py::bytes& data) {
      auto buffer = toBuffer(data);
      auto ndnData = std::make_shared<ndn::Data>(
        ndn::Block(ndn::span<const uint8_t>(buffer.data(), buffer.size())));
      py::gil_scoped_release release;
      publisher.push(ndnData);
    }, py::arg("signed_data"))
    .def("flush", &nsf::StreamPublisher::flush)
    .def("status", &nsf::StreamPublisher::status)
    .def("stop", &nsf::StreamPublisher::stop);

  py::class_<nsf::PredictiveStreamCheckpoint>(
      m, "NativePredictiveStreamCheckpoint")
    .def(py::init<>())
    .def_readwrite("initial_sample_id",
      &nsf::PredictiveStreamCheckpoint::initialSampleId)
    .def_readwrite("oldest_retained_sample_id",
      &nsf::PredictiveStreamCheckpoint::oldestRetainedSampleId)
    .def_readwrite("latest_produced_sample_id",
      &nsf::PredictiveStreamCheckpoint::latestProducedSampleId)
    .def_readwrite("next_expected_sample_id",
      &nsf::PredictiveStreamCheckpoint::nextExpectedSampleId);

  py::class_<nsf::PredictiveStreamDescriptor>(
      m, "NativePredictiveStreamDescriptor")
    .def_property_readonly("definition",
      [] (const nsf::PredictiveStreamDescriptor& d) {
        return d.definition;
      })
    .def_property_readonly("checkpoint",
      [] (const nsf::PredictiveStreamDescriptor& d) {
        return d.checkpoint;
      })
    .def_property_readonly("frontier_name",
      [] (const nsf::PredictiveStreamDescriptor& d) {
        return d.frontierName.toUri();
      });

  py::class_<nsf::LiveStreamConsumerHandle, std::shared_ptr<nsf::LiveStreamConsumerHandle>>(
      m, "NativeLiveStreamConsumerHandle")
    .def("start", &nsf::LiveStreamConsumerHandle::start)
    .def("observe_accepted_sample", &nsf::LiveStreamConsumerHandle::observeAcceptedSample)
    .def("status", &nsf::LiveStreamConsumerHandle::status)
    .def("stop", &nsf::LiveStreamConsumerHandle::stop);

  py::class_<nsf::PredictiveStreamSubscriber,
             std::shared_ptr<nsf::PredictiveStreamSubscriber>>(
      m, "NativePredictiveStreamSubscriber")
    .def("start", &nsf::PredictiveStreamSubscriber::start)
    .def("status", &nsf::PredictiveStreamSubscriber::status)
    .def("stop", &nsf::PredictiveStreamSubscriber::stop);

  py::enum_<nsf::ExecutionLeaseState>(m, "ExecutionLeaseState")
    .value("PREPARED", nsf::ExecutionLeaseState::Prepared)
    .value("COMMITTED", nsf::ExecutionLeaseState::Committed)
    .value("EXECUTING", nsf::ExecutionLeaseState::Executing)
    .value("ABORTED", nsf::ExecutionLeaseState::Aborted)
    .value("RELEASED", nsf::ExecutionLeaseState::Released)
    .value("EXPIRED", nsf::ExecutionLeaseState::Expired);

  py::class_<nsf::GenericExecutionLease>(m, "GenericExecutionLease")
    .def(py::init<>())
    .def_readwrite("schema", &nsf::GenericExecutionLease::schema)
    .def_readwrite("lease_id", &nsf::GenericExecutionLease::leaseId)
    .def_readwrite("provider_name", &nsf::GenericExecutionLease::providerName)
    .def_readwrite("provider_epoch", &nsf::GenericExecutionLease::providerEpoch)
    .def_readwrite("requester_name", &nsf::GenericExecutionLease::requesterName)
    .def_readwrite("request_id", &nsf::GenericExecutionLease::requestId)
    .def_readwrite("service_name", &nsf::GenericExecutionLease::serviceName)
    .def_readwrite("plan_digest", &nsf::GenericExecutionLease::planDigest)
    .def_readwrite("resource_binding_schema",
                   &nsf::GenericExecutionLease::resourceBindingSchema)
    .def_property("resource_binding_proof",
                  [] (const nsf::GenericExecutionLease& lease) {
                    return toPyBytes(lease.resourceBindingProof);
                  },
                  [] (nsf::GenericExecutionLease& lease, const py::bytes& value) {
                    lease.resourceBindingProof = toBuffer(value);
                  })
    .def_readwrite("conflict_keys", &nsf::GenericExecutionLease::conflictKeys)
    .def_readwrite("state", &nsf::GenericExecutionLease::state)
    .def_readwrite("expires_at_ms", &nsf::GenericExecutionLease::expiresAtMs)
    .def_readwrite("execution_deadline_ms",
                   &nsf::GenericExecutionLease::executionDeadlineMs)
    .def_readwrite("idempotency_key", &nsf::GenericExecutionLease::idempotencyKey);

  py::class_<nsf::ExecutionLeaseBinding>(m, "ExecutionLeaseBinding")
    .def(py::init<>())
    .def_readwrite("requester_name", &nsf::ExecutionLeaseBinding::requesterName)
    .def_readwrite("request_id", &nsf::ExecutionLeaseBinding::requestId)
    .def_readwrite("service_name", &nsf::ExecutionLeaseBinding::serviceName)
    .def_readwrite("plan_digest", &nsf::ExecutionLeaseBinding::planDigest)
    .def_readwrite("resource_binding_schema",
                   &nsf::ExecutionLeaseBinding::resourceBindingSchema)
    .def_property("resource_binding_proof",
                  [] (const nsf::ExecutionLeaseBinding& binding) {
                    return toPyBytes(binding.resourceBindingProof);
                  },
                  [] (nsf::ExecutionLeaseBinding& binding, const py::bytes& value) {
                    binding.resourceBindingProof = toBuffer(value);
                  });

  py::class_<nsf::ExecutionLeaseResult>(m, "ExecutionLeaseResult")
    .def_readonly("status", &nsf::ExecutionLeaseResult::status)
    .def_readonly("operation", &nsf::ExecutionLeaseResult::operation)
    .def_readonly("reason_code", &nsf::ExecutionLeaseResult::reasonCode)
    .def_readonly("lease", &nsf::ExecutionLeaseResult::lease)
    .def_readonly("retry_after_ms", &nsf::ExecutionLeaseResult::retryAfterMs)
    .def_readonly("idempotent_replay",
                  &nsf::ExecutionLeaseResult::idempotentReplay);

  py::class_<nsf::ExecutionLeaseCounters>(m, "ExecutionLeaseCounters")
    .def_readonly("prepared", &nsf::ExecutionLeaseCounters::prepared)
    .def_readonly("committed", &nsf::ExecutionLeaseCounters::committed)
    .def_readonly("activated", &nsf::ExecutionLeaseCounters::activated)
    .def_readonly("aborted", &nsf::ExecutionLeaseCounters::aborted)
    .def_readonly("released", &nsf::ExecutionLeaseCounters::released)
    .def_readonly("expired", &nsf::ExecutionLeaseCounters::expired)
    .def_readonly("renewed", &nsf::ExecutionLeaseCounters::renewed)
    .def_readonly("idempotent_replay",
                  &nsf::ExecutionLeaseCounters::idempotentReplay)
    .def_readonly("conflict", &nsf::ExecutionLeaseCounters::conflict)
    .def_readonly("stale_epoch", &nsf::ExecutionLeaseCounters::staleEpoch)
    .def_readonly("cleanup_timeout", &nsf::ExecutionLeaseCounters::cleanupTimeout)
    .def_readonly("rejected_by_reason",
                  &nsf::ExecutionLeaseCounters::rejectedByReason)
    .def_readonly("active_prepared", &nsf::ExecutionLeaseCounters::activePrepared)
    .def_readonly("active_committed", &nsf::ExecutionLeaseCounters::activeCommitted)
    .def_readonly("active_executing", &nsf::ExecutionLeaseCounters::activeExecuting);

  py::class_<nsf::ProviderExecutionLeaseTable>(m, "ProviderExecutionLeaseTable")
    .def(py::init<std::string>(), py::arg("provider_epoch") = "")
    .def_property_readonly("provider_epoch",
                           &nsf::ProviderExecutionLeaseTable::providerEpoch)
    .def("prepare", &nsf::ProviderExecutionLeaseTable::prepare,
         py::arg("lease"), py::arg("now_ms"))
    .def("commit", &nsf::ProviderExecutionLeaseTable::commit,
         py::arg("lease_id"), py::arg("provider_epoch"),
         py::arg("requester_name"), py::arg("idempotency_key"),
         py::arg("now_ms"))
    .def("validate_and_activate",
         &nsf::ProviderExecutionLeaseTable::validateAndActivate,
         py::arg("lease_id"), py::arg("provider_epoch"), py::arg("binding"),
         py::arg("idempotency_key"), py::arg("now_ms"),
         py::arg("execution_deadline_ms"))
    .def("validate", &nsf::ProviderExecutionLeaseTable::validate,
         py::arg("lease_id"), py::arg("provider_epoch"), py::arg("binding"),
         py::arg("now_ms"))
    .def("abort", &nsf::ProviderExecutionLeaseTable::abort,
         py::arg("lease_id"), py::arg("provider_epoch"),
         py::arg("requester_name"), py::arg("idempotency_key"),
         py::arg("now_ms"))
    .def("renew", &nsf::ProviderExecutionLeaseTable::renew,
         py::arg("lease_id"), py::arg("provider_epoch"),
         py::arg("requester_name"), py::arg("idempotency_key"),
         py::arg("now_ms"),
         py::arg("expires_at_ms"))
    .def("release", &nsf::ProviderExecutionLeaseTable::release,
         py::arg("lease_id"), py::arg("provider_epoch"),
         py::arg("requester_name"), py::arg("idempotency_key"),
         py::arg("now_ms"))
    .def("cleanup_expired", &nsf::ProviderExecutionLeaseTable::cleanupExpired,
         py::arg("now_ms"))
    .def("find", &nsf::ProviderExecutionLeaseTable::find,
         py::arg("lease_id"))
    .def("has_active_conflict_key",
         &nsf::ProviderExecutionLeaseTable::hasActiveConflictKey,
         py::arg("conflict_key"), py::arg("now_ms"))
    .def("has_pinned_binding_proof",
         [] (nsf::ProviderExecutionLeaseTable& table,
             const py::bytes& proof, uint64_t nowMs) {
           return table.hasPinnedBindingProof(toBuffer(proof), nowMs);
         },
         py::arg("resource_binding_proof"), py::arg("now_ms"))
    .def("counters", &nsf::ProviderExecutionLeaseTable::counters,
         py::arg("now_ms"));

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
    .def_readwrite("error", &PyServiceResponse::error)
    .def_readwrite("request_id", &PyServiceResponse::requestId)
    .def_readwrite("data_name", &PyServiceResponse::dataName)
    .def_readwrite("signer_certificate", &PyServiceResponse::signerCertificate)
    .def_readwrite("wire_digest", &PyServiceResponse::wireDigest);

  py::class_<PyAckDecision>(m, "AckDecision")
    .def(py::init<>())
    .def_readwrite("status", &PyAckDecision::status)
    .def_readwrite("payload", &PyAckDecision::payload)
    .def_readwrite("message", &PyAckDecision::message)
    .def_readwrite("suppress", &PyAckDecision::suppress)
    .def_readwrite("reservation_lease", &PyAckDecision::reservationLease)
    .def_readwrite("selection_input_key_offer", &PyAckDecision::selectionInputKeyOffer)
    .def_readwrite("pending_state_ttl_ms", &PyAckDecision::pendingStateTtlMs);

  py::class_<PyAckCandidate>(m, "AckCandidate")
    .def(py::init<>())
    .def_readwrite("provider_name", &PyAckCandidate::providerName)
    .def_readwrite("service_name", &PyAckCandidate::serviceName)
    .def_readwrite("request_id", &PyAckCandidate::requestId)
    .def_readwrite("status", &PyAckCandidate::status)
    .def_readwrite("message", &PyAckCandidate::message)
    .def_readwrite("payload", &PyAckCandidate::payload)
    .def_readwrite("telemetry", &PyAckCandidate::telemetry);

  py::class_<PyCollaborationAckClosure>(m, "CollaborationAckClosure")
    .def(py::init<>())
    .def_readwrite("request_id", &PyCollaborationAckClosure::requestId)
    .def_readwrite("candidates", &PyCollaborationAckClosure::candidates)
    .def_readwrite("digest", &PyCollaborationAckClosure::digest)
    .def_readwrite("closed_at_us", &PyCollaborationAckClosure::closedAtUs)
    .def_readwrite(
      "request_deadline_us", &PyCollaborationAckClosure::requestDeadlineUs);

  py::class_<PyLargeDataPublishResult>(m, "LargeDataPublishResult")
    .def(py::init<>())
    .def_readwrite("success", &PyLargeDataPublishResult::success)
    .def_readwrite("encrypted_data_name", &PyLargeDataPublishResult::encryptedDataName)
    .def_readwrite("object_id", &PyLargeDataPublishResult::objectId)
    .def_readwrite("error", &PyLargeDataPublishResult::error);

  py::class_<PySignedAppDataResult>(m, "SignedAppDataResult")
    .def(py::init<>())
    .def_readwrite("success", &PySignedAppDataResult::success)
    .def_readwrite("data_name", &PySignedAppDataResult::dataName)
    .def_readwrite("signer_certificate", &PySignedAppDataResult::signerCertificate)
    .def_readwrite("payload", &PySignedAppDataResult::payload)
    .def_readwrite("error", &PySignedAppDataResult::error);

  py::class_<PyCollaborationAssignment>(m, "CollaborationAssignment")
    .def(py::init<>())
    .def_readwrite("role", &PyCollaborationAssignment::role)
    .def_readwrite("service", &PyCollaborationAssignment::service)
    .def_readwrite("assigned_artifact", &PyCollaborationAssignment::assignedArtifact)
    .def_readwrite("artifact_data_name", &PyCollaborationAssignment::artifactDataName)
    .def_readwrite("requires_provisioning", &PyCollaborationAssignment::requiresProvisioning)
    .def_readwrite("provisioning_timeout_ms", &PyCollaborationAssignment::provisioningTimeoutMs)
    .def_readwrite("selection_digest", &PyCollaborationAssignment::selectionDigest)
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

  py::class_<NativeFileSegmentedObjectProducer>(m, "FileSegmentedObjectProducer")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  size_t,
                  int,
                  bool>(),
         py::arg("base_name"),
         py::arg("file_path"),
         py::arg("signing_identity") = "",
         py::arg("max_segment_size") = 6000,
         py::arg("freshness_ms") = 60000,
         py::arg("digest_signing") = true)
    .def("start", &NativeFileSegmentedObjectProducer::start)
    .def("stop", &NativeFileSegmentedObjectProducer::stop)
    .def_property_readonly("base_name", &NativeFileSegmentedObjectProducer::baseName)
    .def_property_readonly("versioned_name", &NativeFileSegmentedObjectProducer::versionedName)
    .def_property_readonly("segment_count", &NativeFileSegmentedObjectProducer::segmentCount)
    .def_property_readonly("file_size", &NativeFileSegmentedObjectProducer::fileSize)
    .def_property_readonly("data_count", &NativeFileSegmentedObjectProducer::dataCount)
    .def_property_readonly("wire_bytes", &NativeFileSegmentedObjectProducer::wireBytes)
    .def_property_readonly("signing_ms", &NativeFileSegmentedObjectProducer::signingMs)
    .def_property_readonly("public_key_der", &NativeFileSegmentedObjectProducer::publicKeyDer)
    .def_property_readonly("error", &NativeFileSegmentedObjectProducer::error);

  py::class_<PyDataPacket>(m, "DataPacket")
    .def(py::init<>())
    .def_readwrite("name", &PyDataPacket::name)
    .def_readwrite("segment", &PyDataPacket::segment)
    .def_readwrite("wire", &PyDataPacket::wire)
    .def_readwrite("content", &PyDataPacket::content);

  m.def("verify_data_packet_signature", &verifyDataPacketSignature,
        py::arg("wire"), py::arg("public_key_der"));
  m.def("verify_detached_sha256_signature", &verifyDetachedSha256Signature,
        py::arg("payload"), py::arg("signature"), py::arg("public_key_der"));
  m.def("verify_data_packet_digest", &verifyDataPacketDigest,
        py::arg("wire"));

  py::class_<PySegmentHintRange>(m, "SegmentHintRange")
    .def(py::init<>())
    .def_readwrite("start", &PySegmentHintRange::start)
    .def_readwrite("end", &PySegmentHintRange::end)
    .def_readwrite("forwarding_hints", &PySegmentHintRange::forwardingHints);

  py::class_<NativeWireDataProducer>(m, "StoredDataProducer")
    .def(py::init<const std::string&,
                  const std::vector<py::bytes>&,
                  const std::string&,
                  const std::vector<std::string>&>(),
         py::arg("base_name"),
         py::arg("packet_wires"),
         py::arg("signing_identity") = "",
         py::arg("forwarding_route_prefixes") = std::vector<std::string>{})
    .def("start", &NativeWireDataProducer::start)
    .def("stop", &NativeWireDataProducer::stop)
    .def_property_readonly("segment_count", &NativeWireDataProducer::segmentCount)
    .def_property_readonly("error", &NativeWireDataProducer::error);

  py::class_<NativeRepoDataPlaneProducer>(m, "RepoDataPlaneProducer")
    .def(py::init<py::function,
                  const std::string&,
                  const std::vector<std::string>&>(),
         py::arg("lookup"),
         py::arg("signing_identity") = "",
         py::arg("forwarding_route_prefixes") = std::vector<std::string>{})
    .def("activate_prefix", &NativeRepoDataPlaneProducer::activatePrefix)
    .def("start", &NativeRepoDataPlaneProducer::start)
    .def("stop", &NativeRepoDataPlaneProducer::stop)
    .def_property_readonly("active_prefix_count",
                           &NativeRepoDataPlaneProducer::activePrefixCount)
    .def_property_readonly("interest_count",
                           &NativeRepoDataPlaneProducer::interestCount)
    .def_property_readonly("hit_count", &NativeRepoDataPlaneProducer::hitCount)
    .def_property_readonly("miss_count", &NativeRepoDataPlaneProducer::missCount)
    .def_property_readonly("thread_count", &NativeRepoDataPlaneProducer::threadCount)
    .def_property_readonly("error", &NativeRepoDataPlaneProducer::error);

  m.def("make_segmented_data_packets",
        &makeSegmentedDataPackets,
        py::arg("base_name"),
        py::arg("payload"),
        py::arg("signing_identity") = "",
        py::arg("max_segment_size") = 6000,
        py::arg("freshness_ms") = 60000);

  m.def("make_signed_data",
        &makeSignedData,
        py::arg("name"),
        py::arg("content"),
        py::arg("signing_identity") = "",
        py::arg("freshness_ms") = 300);

  m.def("make_predictive_data_name",
        &makePredictiveDataNameUri,
        py::arg("mapping_root"),
        py::arg("mapping_version"),
        py::arg("sequence"));

  m.def("decode_data_packet",
        &decodeDataPacket,
        py::arg("wire"));

  m.def("fetch_segmented_data_packets",
        &fetchSegmentedDataPackets,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 10000,
        py::arg("forwarding_hints") = std::vector<std::string>{});

  py::class_<PyAdaptiveSegmentFetchResult>(m, "AdaptiveSegmentFetchResult")
    .def_readonly("total_segments",
                  &PyAdaptiveSegmentFetchResult::totalSegments)
    .def_readonly("delivered_segments",
                  &PyAdaptiveSegmentFetchResult::deliveredSegments)
    .def_readonly("interest_count",
                  &PyAdaptiveSegmentFetchResult::interestCount)
    .def_readonly("retransmission_count",
                  &PyAdaptiveSegmentFetchResult::retransmissionCount)
    .def_readonly("duplicate_count",
                  &PyAdaptiveSegmentFetchResult::duplicateCount)
    .def_readonly("timeout_count",
                  &PyAdaptiveSegmentFetchResult::timeoutCount)
    .def_readonly("logical_bytes",
                  &PyAdaptiveSegmentFetchResult::logicalBytes)
    .def_readonly("data_wire_bytes",
                  &PyAdaptiveSegmentFetchResult::dataWireBytes)
    .def_readonly("interest_wire_bytes",
                  &PyAdaptiveSegmentFetchResult::interestWireBytes)
    .def_readonly("wire_bytes", &PyAdaptiveSegmentFetchResult::wireBytes)
    .def_readonly("retransmitted_bytes",
                  &PyAdaptiveSegmentFetchResult::retransmittedBytes)
    .def_readonly("maximum_in_flight",
                  &PyAdaptiveSegmentFetchResult::maximumInFlight)
    .def_readonly("final_window",
                  &PyAdaptiveSegmentFetchResult::finalWindow);

  m.def("fetch_adaptive_segmented_data_packets",
        &fetchAdaptiveSegmentedDataPackets,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 1000,
        py::arg("initial_window") = 4,
        py::arg("maximum_window") = 64,
        py::arg("maximum_retries") = 5,
        py::arg("persistence_backlog_limit") = 16,
        py::arg("forwarding_hints") = std::vector<std::string>{},
        py::arg("on_packet"));

  m.def("fetch_exact_data_packet",
        &fetchExactDataPacket,
        py::arg("data_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 2000,
        py::arg("forwarding_hints") = std::vector<std::string>{},
        py::call_guard<py::gil_scoped_release>());

  m.def("fetch_segmented_object",
        &fetchSegmentedObject,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 10000,
        py::arg("init_cwnd") = 8.0,
        py::arg("forwarding_hints") = std::vector<std::string>{},
        py::call_guard<py::gil_scoped_release>());

  m.def("fetch_segmented_object_with_segment_hints",
        &fetchSegmentedObjectWithSegmentHints,
        py::arg("base_name"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 10000,
        py::arg("hint_ranges") = std::vector<PySegmentHintRange>{},
        py::call_guard<py::gil_scoped_release>());
  m.def("fetch_known_segmented_object_with_segment_hints",
        &fetchKnownSegmentedObjectWithSegmentHints,
        py::arg("versioned_name"),
        py::arg("segment_count"),
        py::arg("timeout_ms") = 30000,
        py::arg("interest_lifetime_ms") = 10000,
        py::arg("hint_ranges") = std::vector<PySegmentHintRange>{},
        py::call_guard<py::gil_scoped_release>());

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
    .def("allow_data", &PyCollaborationContext::allowData,
         py::arg("key_scope"),
         py::arg("topic_prefix"))
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
    .def("fetch_large_exact", &PyCollaborationContext::fetchLargeExact,
         py::arg("data_name"),
         py::arg("key_scope"),
         py::arg("timeout_ms") = 5000,
         py::arg("expected_segments"))
    .def("wait_one", &PyCollaborationContext::waitOne,
         py::arg("key_scope"),
         py::arg("topic_prefix"),
         py::arg("timeout_ms") = 5000)
    .def("wait_for", &PyCollaborationContext::waitFor,
         py::arg("key_scope"),
         py::arg("topic_prefix"),
         py::arg("min_count"),
         py::arg("timeout_ms") = 5000)
    .def("report_operation_status", &PyCollaborationContext::reportOperationStatus,
         py::arg("status"))
    .def("publish_final_response", &PyCollaborationContext::publishFinalResponse,
         py::arg("payload"));

  py::class_<NativeServiceController>(m, "NativeServiceController")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::vector<std::string>&,
                  bool,
                  const std::string&>(),
         py::arg("controller_prefix") = "/example/hello/controller",
         py::arg("policy_file") = "examples/hello.policies",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("bootstrap_identities") = std::vector<std::string>{},
         py::arg("serve_certificates") = true,
         py::arg("bootstrap_token_file") = "")
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
                  bool,
                  const std::string&>(),
         py::arg("provider_id") = "",
         py::arg("group") = "/example/hello/group",
         py::arg("controller") = "/example/hello/controller",
         py::arg("provider_prefix") = "/example/hello/provider",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("handler_threads") = 4,
         py::arg("ack_threads") = 2,
         py::arg("serve_certificates") = true,
         py::arg("bootstrap_token") = "")
    .def("add_service", &NativeServiceProvider::addService,
         py::arg("service"),
         py::arg("request_handler"),
         py::arg("ack_handler") = std::optional<py::function>(),
         py::arg("include_request_context") = false,
         py::arg("include_ack_context") = false)
    .def("set_deployment_prepare_handler",
         &NativeServiceProvider::setDeploymentPrepareHandler,
         py::arg("handler"))
    .def_property_readonly("provider_boot_epoch",
         &NativeServiceProvider::providerBootEpoch)
    .def_property_readonly("provider_identity",
         &NativeServiceProvider::providerIdentity)
    .def_property_readonly("provider_signing_key_name",
         &NativeServiceProvider::providerSigningKeyName)
    .def_property_readonly("provider_signing_certificate_name",
         &NativeServiceProvider::providerSigningCertificateName)
    .def("configure_opaque_selection_store",
         &NativeServiceProvider::configureOpaqueSelectionStore,
         py::arg("wal_path"), py::arg("storage_key"),
         py::arg("storage_key_epoch"), py::arg("max_prepare_ms") = 1000)
    .def("register_opaque_selection_participant",
         &NativeServiceProvider::registerOpaqueSelectionParticipant,
         py::arg("service"), py::arg("participant_id"),
         py::arg("participant_version"), py::arg("prepare"),
         py::arg("on_committed"), py::arg("on_aborted"))
    .def("set_r1_selection_decision_handler",
         &NativeServiceProvider::setR1SelectionDecisionHandler,
         py::arg("service"), py::arg("handler"))
    .def("set_r1_reservation_terminal_handler",
         &NativeServiceProvider::setR1ReservationTerminalHandler,
         py::arg("service"), py::arg("handler"))
    .def("add_collaboration_service", &NativeServiceProvider::addCollaborationService,
         py::arg("service"),
         py::arg("allowed_roles"),
         py::arg("collaboration_handler"),
         py::arg("ack_handler") = std::optional<py::function>(),
         py::arg("include_ack_context") = false)
    .def("start", &NativeServiceProvider::start)
    .def("publish_service_info", &NativeServiceProvider::publishServiceInfo,
         py::arg("service_name"), py::arg("service_lifetime_seconds"), py::arg("meta_info") = py::dict())
    .def("update_ndnsd_meta", &NativeServiceProvider::updateNdnsdMeta,
         py::arg("key"), py::arg("value"))
    .def("set_ndnsd_meta", &NativeServiceProvider::setNdnsdMeta,
         py::arg("meta"))
    .def("start_ndnsd_periodic_publish", &NativeServiceProvider::startNdnsdPeriodicPublish,
         py::arg("interval_seconds"))
	    .def("create_live_stream", &NativeServiceProvider::createLiveStream,
	         py::arg("definition"))
    .def("create_stream", &NativeServiceProvider::createStream,
         py::arg("config"))
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
                  bool,
                  const std::string&>(),
         py::arg("group") = "/example/hello/group",
         py::arg("controller") = "/example/hello/controller",
         py::arg("user") = "/example/hello/user",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("permission_wait_ms") = 1500,
         py::arg("handler_threads") = 2,
         py::arg("ack_threads") = 2,
         py::arg("adaptive_admission") = false,
         py::arg("serve_certificates") = true,
         py::arg("bootstrap_token") = "")
	    .def("open_live_stream", &NativeServiceUser::openLiveStream,
         py::arg("descriptor"), py::arg("on_item"),
         py::arg("start") = "latest",
         py::arg("prefetch_policy") = "mapped-pressure",
         py::arg("aggregate_interest_limit") = 64,
         py::arg("enable_fec_recovery") = false,
         py::arg("interest_lifetime_ms") = 500,
	         py::arg("on_status") = std::optional<py::function>())
    .def("subscribe_stream", &NativeServiceUser::subscribeStream,
         py::arg("descriptor"), py::arg("on_item"),
         py::arg("start") = "latest",
         py::arg("prefetch_policy") = std::optional<std::string>(),
         py::arg("aggregate_interest_limit") = 64,
         py::arg("enable_fec_recovery") = true,
         py::arg("require_full_delivery") = false,
         py::arg("interest_lifetime_ms") = 500,
         py::arg("on_status") = std::optional<py::function>())
    .def("request_service", &NativeServiceUser::requestService,
         py::arg("service"),
         py::arg("payload"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("strategy") = "first-responding",
         py::arg("request_id") = "",
         py::arg("deployment_intent") = std::nullopt,
         py::arg("request_capabilities") = std::nullopt)
    .def("request_service_targeted", &NativeServiceUser::requestServiceTargeted,
         py::arg("provider"),
         py::arg("service"),
         py::arg("payload"),
         py::arg("timeout_ms") = 5000)
    .def("request_service_select", &NativeServiceUser::requestServiceSelect,
         py::arg("service"),
         py::arg("payload"),
         py::arg("selector"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("request_strategy") = "first-responding",
         py::arg("deployment_intent") = std::nullopt,
         py::arg("request_capabilities") = std::nullopt)
    .def("request_service_async", &NativeServiceUser::requestServiceAsync,
         py::arg("service"),
         py::arg("payload"),
         py::arg("on_response"),
         py::arg("on_timeout"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("strategy") = "first-responding")
    .def("request_service_targeted_async", &NativeServiceUser::requestServiceTargetedAsync,
         py::arg("provider"),
         py::arg("service"),
         py::arg("payload"),
         py::arg("on_response"),
         py::arg("on_timeout"),
         py::arg("timeout_ms") = 5000)
    .def("publish_encrypted_large_data", &NativeServiceUser::publishEncryptedLargeData,
         py::arg("service"),
         py::arg("payload"),
         py::arg("object_label") = "",
         py::arg("freshness_ms") = 60000)
    .def("publish_signed_app_data", &NativeServiceUser::publishSignedAppData,
         py::arg("data_name"), py::arg("payload"),
         py::arg("freshness_ms") = 60000)
    .def("fetch_signed_app_data", &NativeServiceUser::fetchSignedAppData,
         py::arg("data_name"), py::arg("expected_signer"),
         py::arg("timeout_ms") = 5000)
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
         py::arg("timeout_ms") = 10000,
         py::arg("ack_observer") = py::none(),
         py::arg("role_provider_assignments") = std::map<std::string, std::string>{},
         py::arg("request_id") = "")
    .def("begin_collaboration", &NativeServiceUser::beginCollaboration,
         py::arg("service"), py::arg("payload"),
         py::arg("on_ack_closed"), py::arg("on_response"),
         py::arg("on_timeout"), py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 10000, py::arg("request_id") = "",
         py::arg("ack_coverage_predicate") = py::none())
    .def("commit_collaboration_plan",
         &NativeServiceUser::commitCollaborationPlan,
         py::arg("service"), py::arg("request_id"),
         py::arg("ack_closed_digest"), py::arg("roles"),
         py::arg("key_scopes"), py::arg("dependencies"),
         py::arg("artifact_data_names"), py::arg("scope_key_data_names"),
         py::arg("role_scopes"), py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 10000,
         py::arg("role_provider_assignments") =
           std::map<std::string, std::string>{})
    .def("request_collaboration_async", &NativeServiceUser::requestCollaborationAsync,
         py::arg("service"),
         py::arg("payload"),
         py::arg("roles"),
         py::arg("key_scopes"),
         py::arg("dependencies"),
         py::arg("artifact_data_names"),
         py::arg("scope_key_data_names"),
         py::arg("role_scopes"),
         py::arg("on_response"),
         py::arg("on_timeout"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 10000,
         py::arg("role_provider_assignments") = std::map<std::string, std::string>{},
         py::arg("request_id") = "")
    .def("query_collaboration_status", &NativeServiceUser::queryCollaborationStatus,
         py::arg("provider"), py::arg("service"),
         py::arg("selection_digest"), py::arg("timeout_ms") = 500)
    .def("get_collaboration_status_snapshot",
         &NativeServiceUser::getCollaborationStatusSnapshot,
         py::arg("request_id"), py::arg("timeout_ms") = 500)
    .def("start", &NativeServiceUser::start)
    .def("stop", &NativeServiceUser::stop)
    .def("get_allowed_services", &NativeServiceUser::getAllowedServices)
    .def("get_ndnsd_services", &NativeServiceUser::getNdnsdServices)
    .def("pump", &NativeServiceUser::pump);
}
