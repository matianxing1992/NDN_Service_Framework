#include "UavObservationSource.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace ndnsf::examples::uav {
namespace {

using boost::property_tree::ptree;

constexpr size_t MAX_JSON_LINE_BYTES = 12 * 1024 * 1024;
constexpr size_t MAX_METADATA_BYTES = 64 * 1024;
constexpr size_t MAX_DECODED_IMAGE_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_JSON_NESTING_DEPTH = 64;
constexpr uint64_t MAX_TIME_US = static_cast<uint64_t>(INT64_MAX);

class StringScalarJsonScanner
{
public:
  explicit StringScalarJsonScanner(std::string_view input) : m_input(input) {}

  bool
  validate()
  {
    skipSpace();
    if (!value(0)) return false;
    skipSpace();
    return m_position == m_input.size();
  }

private:
  void
  skipSpace()
  {
    while (m_position < m_input.size() &&
           (m_input[m_position] == ' ' || m_input[m_position] == '\t' ||
            m_input[m_position] == '\r' || m_input[m_position] == '\n')) {
      ++m_position;
    }
  }

  bool
  string()
  {
    if (m_position >= m_input.size() || m_input[m_position++] != '"') return false;
    while (m_position < m_input.size()) {
      const unsigned char c = static_cast<unsigned char>(m_input[m_position++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c == '\\') {
        if (m_position >= m_input.size()) return false;
        const char escaped = m_input[m_position++];
        if (escaped == 'u') {
          for (int i = 0; i < 4; ++i) {
            if (m_position >= m_input.size()) return false;
            const char digit = m_input[m_position++];
            if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                  (digit >= 'A' && digit <= 'F'))) return false;
          }
        }
        else if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' &&
                 escaped != 'f' && escaped != 'n' && escaped != 'r' && escaped != 't') {
          return false;
        }
      }
    }
    return false;
  }

  bool
  value(size_t depth)
  {
    if (depth > MAX_JSON_NESTING_DEPTH) return false;
    skipSpace();
    if (m_position >= m_input.size()) return false;
    if (m_input[m_position] == '"') return string();
    if (m_input[m_position] == '{') {
      ++m_position;
      skipSpace();
      if (m_position < m_input.size() && m_input[m_position] == '}') {
        ++m_position;
        return true;
      }
      while (m_position < m_input.size()) {
        skipSpace();
        if (!string()) return false;
        skipSpace();
        if (m_position >= m_input.size() || m_input[m_position++] != ':') return false;
        if (!value(depth + 1)) return false;
        skipSpace();
        if (m_position >= m_input.size()) return false;
        const char separator = m_input[m_position++];
        if (separator == '}') return true;
        if (separator != ',') return false;
      }
      return false;
    }
    if (m_input[m_position] == '[') {
      ++m_position;
      skipSpace();
      if (m_position < m_input.size() && m_input[m_position] == ']') {
        ++m_position;
        return true;
      }
      while (m_position < m_input.size()) {
        if (!value(depth + 1)) return false;
        skipSpace();
        if (m_position >= m_input.size()) return false;
        const char separator = m_input[m_position++];
        if (separator == ']') return true;
        if (separator != ',') return false;
      }
      return false;
    }
    // The wire contract requires every scalar, including numeric leaves, to
    // be a JSON string. property_tree otherwise erases the number/string type.
    return false;
  }

private:
  std::string_view m_input;
  size_t m_position = 0;
};

void
checkKeys(const ptree& tree, std::initializer_list<const char*> allowed,
          std::initializer_list<const char*> required)
{
  std::set<std::string> allow;
  std::set<std::string> must;
  for (auto key : allowed) allow.emplace(key);
  for (auto key : required) must.emplace(key);
  std::set<std::string> seen;
  for (const auto& item : tree) {
    if (item.first.empty() || !allow.count(item.first) || !seen.insert(item.first).second) {
      throw std::invalid_argument("unknown, repeated, or malformed object key");
    }
  }
  for (const auto& key : must) {
    if (!seen.count(key)) throw std::invalid_argument("required object key is missing");
  }
}

const ptree&
requiredChild(const ptree& tree, const char* key)
{
  const auto child = tree.get_child_optional(key);
  if (!child) throw std::invalid_argument("required event field is missing");
  return *child;
}

std::string
stringValue(const ptree& tree)
{
  if (!tree.empty()) throw std::invalid_argument("expected string scalar");
  return tree.data();
}

std::string
stringField(const ptree& tree, const char* key)
{
  return stringValue(requiredChild(tree, key));
}

uint64_t
uintValue(const ptree& tree)
{
  const std::string value = stringValue(tree);
  if (value.empty()) throw std::invalid_argument("empty integer string");
  uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      result > MAX_TIME_US) throw std::invalid_argument("invalid integer string");
  return result;
}

uint64_t
uintField(const ptree& tree, const char* key)
{
  return uintValue(requiredChild(tree, key));
}

double
doubleValue(const ptree& tree)
{
  const std::string value = stringValue(tree);
  if (value.empty()) throw std::invalid_argument("empty decimal string");
  char* end = nullptr;
  errno = 0;
  const double result = std::strtod(value.c_str(), &end);
  if (errno == ERANGE || end != value.c_str() + value.size() || !std::isfinite(result)) {
    throw std::invalid_argument("invalid finite decimal string");
  }
  return result;
}

double
doubleField(const ptree& tree, const char* key)
{
  return doubleValue(requiredChild(tree, key));
}

template<size_t N>
std::array<double, N>
doubleArray(const ptree& tree)
{
  std::array<double, N> result{};
  size_t index = 0;
  for (const auto& item : tree) {
    if (!item.first.empty() || index >= N) throw std::invalid_argument("invalid numeric array shape");
    result[index++] = doubleValue(item.second);
  }
  if (index != N) throw std::invalid_argument("invalid numeric array length");
  return result;
}

template<size_t N>
std::array<double, N>
arrayField(const ptree& tree, const char* key)
{
  return doubleArray<N>(requiredChild(tree, key));
}

template<size_t N>
std::array<double, N * N>
matrixField(const ptree& tree, const char* key)
{
  std::array<double, N * N> result{};
  const auto& rows = requiredChild(tree, key);
  size_t row = 0;
  for (const auto& item : rows) {
    if (!item.first.empty() || row >= N) throw std::invalid_argument("invalid matrix row count");
    const auto values = doubleArray<N>(item.second);
    std::copy(values.begin(), values.end(), result.begin() + row * N);
    ++row;
  }
  if (row != N) throw std::invalid_argument("invalid matrix dimension");
  return result;
}

template<size_t N>
std::optional<std::array<double, N * N>>
optionalMatrix(const ptree& tree, const char* key)
{
  const auto child = tree.get_child_optional(key);
  return child ? std::optional<std::array<double, N * N>>(matrixField<N>(tree, key)) : std::nullopt;
}

template<size_t N>
std::optional<std::array<double, N>>
optionalArray(const ptree& tree, const char* key)
{
  const auto child = tree.get_child_optional(key);
  return child ? std::optional<std::array<double, N>>(doubleArray<N>(*child)) : std::nullopt;
}

std::optional<double>
optionalDouble(const ptree& tree, const char* key)
{
  const auto child = tree.get_child_optional(key);
  return child ? std::optional<double>(doubleValue(*child)) : std::nullopt;
}

SourceIdentity
parseIdentity(const ptree& tree)
{
  checkKeys(tree, {"sourceId", "droneId", "cameraId", "epoch", "worldFrameId",
                   "acquisitionKind", "deliveryMode"},
            {"sourceId", "droneId", "cameraId", "epoch", "worldFrameId",
             "acquisitionKind", "deliveryMode"});
  SourceIdentity result;
  result.sourceId = stringField(tree, "sourceId");
  result.droneId = stringField(tree, "droneId");
  result.cameraId = stringField(tree, "cameraId");
  result.epoch = uintField(tree, "epoch");
  result.worldFrameId = stringField(tree, "worldFrameId");
  const auto acquisition = stringField(tree, "acquisitionKind");
  if (acquisition == "simulated") result.acquisitionKind = AcquisitionKind::Simulated;
  else if (acquisition == "measured") result.acquisitionKind = AcquisitionKind::Measured;
  else throw std::invalid_argument("invalid acquisition kind");
  const auto delivery = stringField(tree, "deliveryMode");
  if (delivery == "generated") result.deliveryMode = DeliveryMode::Generated;
  else if (delivery == "replay") result.deliveryMode = DeliveryMode::Replay;
  else if (delivery == "live-push") result.deliveryMode = DeliveryMode::LivePush;
  else throw std::invalid_argument("invalid delivery mode");
  return result;
}

bool
sameIdentity(const SourceIdentity& a, const SourceIdentity& b)
{
  return a.sourceId == b.sourceId && a.droneId == b.droneId &&
         a.cameraId == b.cameraId && a.epoch == b.epoch &&
         a.worldFrameId == b.worldFrameId && a.acquisitionKind == b.acquisitionKind &&
         a.deliveryMode == b.deliveryMode;
}

ClockBinding
parseClock(const ptree& tree)
{
  checkKeys(tree, {"revision", "clockId", "clockEpoch", "a", "bUs", "validFromUs",
                   "validUntilUs", "uncertaintyUs"},
            {"revision", "clockId", "clockEpoch", "a", "bUs", "validFromUs",
             "validUntilUs", "uncertaintyUs"});
  return {uintField(tree, "revision"), stringField(tree, "clockId"),
          uintField(tree, "clockEpoch"), doubleField(tree, "a"), doubleField(tree, "bUs"),
          uintField(tree, "validFromUs"), uintField(tree, "validUntilUs"),
          uintField(tree, "uncertaintyUs")};
}

FrameKey
parseFrameKey(const ptree& tree)
{
  checkKeys(tree, {"sourceId", "cameraId", "epoch", "sequence"},
            {"sourceId", "cameraId", "epoch", "sequence"});
  return {stringField(tree, "sourceId"), stringField(tree, "cameraId"),
          uintField(tree, "epoch"), uintField(tree, "sequence")};
}

CalibrationRecord
parseCalibration(const ptree& tree)
{
  checkKeys(tree, {"schema", "cameraId", "revision", "width", "height", "fx", "fy",
                   "cx", "cy", "distortion", "imageTransform", "validFromUs",
                   "validUntilUs", "parameterCovariance", "anchorSigmaPixels",
                   "externalDataRef"},
            {"schema", "cameraId", "revision", "width", "height", "fx", "fy", "cx",
             "cy", "distortion", "imageTransform", "validFromUs", "validUntilUs"});
  CalibrationRecord result;
  result.schema = stringField(tree, "schema");
  result.cameraId = stringField(tree, "cameraId");
  result.revision = uintField(tree, "revision");
  const auto width = uintField(tree, "width");
  const auto height = uintField(tree, "height");
  if (width > UINT32_MAX || height > UINT32_MAX) throw std::invalid_argument("image dimensions overflow");
  result.width = static_cast<uint32_t>(width);
  result.height = static_cast<uint32_t>(height);
  result.fx = doubleField(tree, "fx");
  result.fy = doubleField(tree, "fy");
  result.cx = doubleField(tree, "cx");
  result.cy = doubleField(tree, "cy");
  result.distortion = arrayField<5>(tree, "distortion");
  result.imageTransform = stringField(tree, "imageTransform");
  result.validFromUs = uintField(tree, "validFromUs");
  result.validUntilUs = uintField(tree, "validUntilUs");
  result.parameterCovariance = optionalMatrix<9>(tree, "parameterCovariance");
  result.anchorSigmaPixels = optionalDouble(tree, "anchorSigmaPixels");
  if (const auto reference = tree.get_child_optional("externalDataRef")) {
    checkKeys(*reference, {"exactName", "contentSha256", "producerIdentity"},
              {"exactName", "contentSha256", "producerIdentity"});
    result.externalDataRef = DataRef{stringField(*reference, "exactName"),
                                     stringField(*reference, "contentSha256"),
                                     stringField(*reference, "producerIdentity")};
  }
  return result;
}

GroundRecord
parseGround(const ptree& tree)
{
  checkKeys(tree, {"schema", "revision", "worldFrameId", "normal", "offsetM", "roiVertices",
                   "geometryValidity", "parameterCovariance"},
            {"schema", "revision", "worldFrameId", "normal", "offsetM", "roiVertices",
             "geometryValidity"});
  GroundRecord result;
  result.schema = stringField(tree, "schema");
  result.revision = uintField(tree, "revision");
  result.worldFrameId = stringField(tree, "worldFrameId");
  result.normal = arrayField<3>(tree, "normal");
  result.offsetM = doubleField(tree, "offsetM");
  result.geometryValidity = stringField(tree, "geometryValidity");
  const auto& vertices = requiredChild(tree, "roiVertices");
  for (const auto& item : vertices) {
    if (!item.first.empty() || result.roiVertices.size() == 32) {
      throw std::invalid_argument("invalid ROI vertex list");
    }
    result.roiVertices.push_back(doubleArray<3>(item.second));
  }
  result.parameterCovariance = optionalMatrix<4>(tree, "parameterCovariance");
  return result;
}

PoseSample
parsePose(const ptree& tree)
{
  checkKeys(tree, {"schema", "identity", "sampleSequence", "deviceTimeUs", "clockBinding",
                   "positionW", "qWC", "calibrationRevision", "poseCovariance"},
            {"schema", "identity", "sampleSequence", "deviceTimeUs", "clockBinding",
             "positionW", "qWC", "calibrationRevision"});
  PoseSample result;
  result.schema = stringField(tree, "schema");
  result.identity = parseIdentity(requiredChild(tree, "identity"));
  result.sampleSequence = uintField(tree, "sampleSequence");
  result.deviceTimeUs = uintField(tree, "deviceTimeUs");
  result.clockBinding = parseClock(requiredChild(tree, "clockBinding"));
  result.positionW = arrayField<3>(tree, "positionW");
  result.qWC = arrayField<4>(tree, "qWC");
  result.calibrationRevision = uintField(tree, "calibrationRevision");
  result.poseCovariance = optionalMatrix<6>(tree, "poseCovariance");
  return result;
}

int
base64Value(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<uint8_t>
decodeBase64(const std::string& encoded)
{
  if (encoded.empty() || encoded.size() % 4 != 0 ||
      encoded.size() > 4 * ((MAX_DECODED_IMAGE_BYTES + 2) / 3)) {
    throw std::invalid_argument("invalid base64 length");
  }
  size_t padding = 0;
  if (encoded.back() == '=') ++padding;
  if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
  if (padding > 2) throw std::invalid_argument("invalid base64 padding");
  const size_t decodedSize = encoded.size() / 4 * 3 - padding;
  if (decodedSize == 0 || decodedSize > MAX_DECODED_IMAGE_BYTES) {
    throw std::invalid_argument("decoded image size is out of range");
  }
  std::vector<uint8_t> result;
  result.reserve(decodedSize);
  for (size_t i = 0; i < encoded.size(); i += 4) {
    const bool final = i + 4 == encoded.size();
    const int a = base64Value(encoded[i]);
    const int b = base64Value(encoded[i + 1]);
    const int c = encoded[i + 2] == '=' ? 0 : base64Value(encoded[i + 2]);
    const int d = encoded[i + 3] == '=' ? 0 : base64Value(encoded[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (!final && (encoded[i + 2] == '=' || encoded[i + 3] == '=')) ||
        (final && padding == 2 && (encoded[i + 2] != '=' || (b & 0x0f) != 0)) ||
        (final && padding == 1 && (encoded[i + 3] != '=' || (c & 0x03) != 0))) {
      throw std::invalid_argument("invalid or non-canonical base64");
    }
    const uint32_t bits = (static_cast<uint32_t>(a) << 18) |
                          (static_cast<uint32_t>(b) << 12) |
                          (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
    result.push_back(static_cast<uint8_t>((bits >> 16) & 0xff));
    if (encoded[i + 2] != '=') result.push_back(static_cast<uint8_t>((bits >> 8) & 0xff));
    if (encoded[i + 3] != '=') result.push_back(static_cast<uint8_t>(bits & 0xff));
  }
  if (result.size() != decodedSize) throw std::invalid_argument("invalid base64 result length");
  return result;
}

size_t
metadataSize(const ptree& tree, bool skipImage = false)
{
  size_t result = tree.data().size();
  for (const auto& item : tree) {
    if (skipImage && item.first == "imageBase64") continue;
    result += item.first.size() + 2 + item.second.data().size();
    for (const auto& child : item.second) result += metadataSize(child.second);
  }
  return result;
}

bool
isConfiguredIdentity(const SourceIdentity& event, const SourceIdentity& expected)
{
  return sameIdentity(event, expected);
}

} // namespace

StreamObservationSource::StreamObservationSource(int ownedFd, SourceIdentity identity,
                                                 ObservationPolicy policy)
  : m_fd(ownedFd)
  , m_identity(std::move(identity))
  , m_policy(policy)
{
  if (m_fd < 0) throw std::invalid_argument("stream source requires an owned file descriptor");
  if (m_identity.sourceId.empty() || m_identity.cameraId.empty() || m_identity.epoch == 0 ||
      m_policy.maxQueuedFrames == 0 || m_policy.maxQueuedBytes == 0 ||
      m_policy.maxPoseSamples == 0 || m_policy.maxAlignWaitMs == 0) {
    throw std::invalid_argument("stream source identity or policy is invalid");
  }
}

StreamObservationSource::~StreamObservationSource()
{
  stop();
}

void
StreamObservationSource::start(IObservationSink& sink)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_stopped) throw std::logic_error("stream source is stopped");
  if (m_started) throw std::logic_error("stream source can be started only once");
  const int flags = ::fcntl(m_fd, F_GETFL);
  if (flags < 0 || ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    stopLocked();
    throw std::runtime_error("cannot set stream source descriptor nonblocking");
  }
  m_sink = &sink;
  m_started = true;
}

Admission
StreamObservationSource::processLine(std::string_view line)
{
  try {
    if (line.empty() || line.size() > MAX_JSON_LINE_BYTES ||
        !StringScalarJsonScanner(line).validate()) {
      return {ObservationAdmissionStatus::Rejected, "INVALID_EVENT"};
    }
    std::istringstream input{std::string(line)};
    ptree event;
    boost::property_tree::read_json(input, event);
    checkKeys(event, {"schema", "kind", "payload"}, {"schema", "kind", "payload"});
    if (stringField(event, "schema") != "uav-source-event-v2") {
      return {ObservationAdmissionStatus::Rejected, "INVALID_SCHEMA"};
    }
    const std::string kind = stringField(event, "kind");
    const auto& payload = requiredChild(event, "payload");
    if (metadataSize(payload, kind == "frame") > MAX_METADATA_BYTES) {
      return {ObservationAdmissionStatus::Rejected, "METADATA_TOO_LARGE"};
    }
    if (kind == "calibration") {
      auto record = parseCalibration(payload);
      if (record.cameraId != m_identity.cameraId) {
        return {ObservationAdmissionStatus::Rejected, "IDENTITY_MISMATCH"};
      }
      return m_sink->submitCalibration(std::move(record));
    }
    if (kind == "ground") {
      auto record = parseGround(payload);
      if (record.worldFrameId != m_identity.worldFrameId) {
        return {ObservationAdmissionStatus::Rejected, "IDENTITY_MISMATCH"};
      }
      return m_sink->submitGround(std::move(record));
    }
    if (kind == "pose") {
      auto record = parsePose(payload);
      if (!isConfiguredIdentity(record.identity, m_identity)) {
        return {ObservationAdmissionStatus::Rejected, "IDENTITY_MISMATCH"};
      }
      return m_sink->submitPose(std::move(record));
    }
    if (kind == "gap") {
      checkKeys(payload, {"schema", "identity", "firstSequence", "lastSequence", "reason"},
                {"schema", "identity", "firstSequence", "lastSequence", "reason"});
      FrameGap record;
      record.schema = stringField(payload, "schema");
      record.identity = parseIdentity(requiredChild(payload, "identity"));
      record.firstSequence = uintField(payload, "firstSequence");
      record.lastSequence = uintField(payload, "lastSequence");
      record.reason = stringField(payload, "reason");
      if (!isConfiguredIdentity(record.identity, m_identity)) {
        return {ObservationAdmissionStatus::Rejected, "IDENTITY_MISMATCH"};
      }
      return m_sink->submitGap(std::move(record));
    }
    if (kind == "frame") {
      checkKeys(payload, {"key", "deviceTimeUs", "clockBinding", "width", "height",
                          "encoding", "calibrationRevision", "imageBase64"},
                {"key", "deviceTimeUs", "clockBinding", "width", "height", "encoding",
                 "calibrationRevision", "imageBase64"});
      CapturedFrame record;
      record.key = parseFrameKey(requiredChild(payload, "key"));
      record.deviceTimeUs = uintField(payload, "deviceTimeUs");
      record.clockBinding = parseClock(requiredChild(payload, "clockBinding"));
      const uint64_t width = uintField(payload, "width");
      const uint64_t height = uintField(payload, "height");
      if (width > UINT32_MAX || height > UINT32_MAX) {
        return {ObservationAdmissionStatus::Rejected, "INVALID_FRAME"};
      }
      record.width = static_cast<uint32_t>(width);
      record.height = static_cast<uint32_t>(height);
      record.encoding = stringField(payload, "encoding");
      record.calibrationRevision = uintField(payload, "calibrationRevision");
      record.imageBytes = decodeBase64(stringField(payload, "imageBase64"));
      if (record.key.sourceId != m_identity.sourceId ||
          record.key.cameraId != m_identity.cameraId || record.key.epoch != m_identity.epoch) {
        return {ObservationAdmissionStatus::Rejected, "IDENTITY_MISMATCH"};
      }
      return m_sink->submitFrame(std::move(record));
    }
    return {ObservationAdmissionStatus::Rejected, "INVALID_KIND"};
  }
  catch (const std::exception&) {
    return {ObservationAdmissionStatus::Rejected, "INVALID_EVENT"};
  }
}

StreamPumpResult
StreamObservationSource::pump(size_t maxEvents)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_started || m_stopped || !m_sink) {
    throw std::logic_error("stream source must be started before pumping");
  }
  if (maxEvents == 0) throw std::invalid_argument("pump event limit must be positive");
  StreamPumpResult result;
  size_t completed = 0;
  auto record = [&result] (const Admission& admission) {
    if (admission.status == ObservationAdmissionStatus::Accepted) ++result.acceptedEvents;
    else if (admission.status == ObservationAdmissionStatus::Duplicate) ++result.duplicateEvents;
    else {
      ++result.rejectedEvents;
      result.lastError = admission.reason;
    }
  };

  while (completed < maxEvents) {
    if (m_pendingInput.empty()) {
      char buffer[64 * 1024];
      const ssize_t count = ::read(m_fd, buffer, sizeof(buffer));
      if (count > 0) {
        m_pendingInput.assign(buffer, static_cast<size_t>(count));
      }
      else if (count == 0) {
        m_eof = true;
        if (!m_currentLine.empty() && !m_discardingLine) {
          ++result.rejectedEvents;
          result.lastError = "TRUNCATED_LINE";
          m_currentLine.clear();
        }
        m_discardingLine = false;
        break;
      }
      else if (errno == EINTR) {
        continue;
      }
      else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      else {
        ++result.rejectedEvents;
        result.lastError = "STREAM_READ_ERROR";
        stopLocked();
        break;
      }
    }

    size_t consumed = 0;
    while (consumed < m_pendingInput.size() && completed < maxEvents) {
      const char c = m_pendingInput[consumed++];
      if (m_discardingLine) {
        if (c == '\n') m_discardingLine = false;
        continue;
      }
      if (c == '\n') {
        record(processLine(m_currentLine));
        m_currentLine.clear();
        ++completed;
        continue;
      }
      if (m_currentLine.size() == MAX_JSON_LINE_BYTES) {
        m_currentLine.clear();
        m_discardingLine = true;
        ++result.rejectedEvents;
        result.lastError = "LINE_TOO_LARGE";
        ++completed;
        continue;
      }
      m_currentLine.push_back(c);
    }
    m_pendingInput.erase(0, consumed);
  }
  result.hasBufferedEvents = m_pendingInput.find('\n') != std::string::npos;
  result.eof = m_eof;
  return result;
}

void
StreamObservationSource::stop() noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  stopLocked();
}

void
StreamObservationSource::stopLocked() noexcept
{
  if (m_stopped) return;
  m_stopped = true;
  m_sink = nullptr;
  m_currentLine.clear();
  m_pendingInput.clear();
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
}

} // namespace ndnsf::examples::uav
