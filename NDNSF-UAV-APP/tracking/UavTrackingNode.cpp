#include "../shared/UavNames.hpp"
#include "../shared/UavObservationRefVerifier.hpp"
#include "../shared/UavObservationSource.hpp"
#include "../shared/UavSourceGeneration.hpp"
#include "../shared/UavTrackingEvidenceStore.hpp"
#include "../shared/UavTrackingResultVerifier.hpp"
#include "../shared/UavTrackingWorkerTransaction.hpp"
#include "../shared/uav-person-computation.hpp"
#include "../shared/uav-person-published-frame.hpp"
#include "../shared/uav-person-trace.hpp"
#include "../shared/uav-person-window-admission.hpp"
#include "../shared/uav-tracking-worker-process.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <link.h>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

namespace nsf = ndn_service_framework;
namespace uav = ndnsf::examples::uav;

namespace {

ndn::Buffer fieldsBuffer(const uav::Fields& fields);

std::atomic<bool> g_stop{false};
std::atomic<bool> cacheProbeReleased{false};

void
onCacheProbeSignal(int)
{
  cacheProbeReleased.store(true);
}

std::mutex g_tracking_transaction_mutex;

void
onSignal(int)
{
  g_stop.store(true);
}

std::string
option(int argc, char** argv, const std::string& name, const std::string& fallback = {})
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == name) return argv[i + 1];
  }
  return fallback;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

ndn::Buffer
bufferFrom(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

nsf::ResponseMessage
makeResponse(const std::string& payload)
{
  nsf::ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("No error");
  auto bytes = bufferFrom(payload);
  response.setPayload(bytes, bytes.size());
  return response;
}

nsf::ResponseMessage
makeError(const std::string& error)
{
  nsf::ResponseMessage response;
  response.setStatus(false);
  response.setErrorInfo(error);
  return response;
}

std::string
payloadText(const nsf::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

struct Options
{
  std::string role;
  std::string camera;
  std::string videoSource;
  std::string frameFile;
  std::string frameDir;
  std::string model;
  std::string trackingProfile;
  std::string reidModel;
  std::string describePersonComputation;
  std::string workerPython = "python3";
  std::string workerScript = "NDNSF-UAV-APP/tracking/tracking_worker.py";
  std::string display = "headless";
  std::string runId = "spec191";
  std::string groupPrefix = "/example/uav";
  std::string controller = "/example/uav/controller";
  std::string trustSchema = "examples/trust-any.conf";
  std::string motionDir;
  std::string observationStreamDir;
  std::string observationSourceId;
  std::string worldFrameId = "synthetic-local";
  std::string acquisitionKind = "simulated";
  std::string deliveryMode = "replay";
  std::string sourceGenerations;
  uint64_t sourceEpoch = 1;
  size_t windowCount = 1;
  std::string fault = "none";
};

Options
parseOptions(int argc, char** argv)
{
  for (const auto name : {"--tracking-profile", "--reid-model", "--describe-person-computation"}) {
    for (int i = 1; i < argc; ++i) {
      if (argv[i] == std::string_view(name) &&
          (i + 1 >= argc || argv[i + 1][0] == '\0' ||
           std::string_view(argv[i + 1]).substr(0, 2) == "--")) {
        throw std::invalid_argument(std::string("missing value for ") + name);
      }
    }
  }
  Options result;
  result.role = option(argc, argv, "--role");
  result.camera = option(argc, argv, "--camera");
  result.videoSource = option(argc, argv, "--video-source");
  result.frameFile = option(argc, argv, "--frame-file");
  result.frameDir = option(argc, argv, "--frame-dir");
  result.model = option(argc, argv, "--model");
  result.trackingProfile = option(argc, argv, "--tracking-profile");
  result.reidModel = option(argc, argv, "--reid-model");
  result.describePersonComputation = option(argc, argv, "--describe-person-computation");
  result.workerPython = option(argc, argv, "--worker-python", result.workerPython);
  result.workerScript = option(argc, argv, "--worker-script", result.workerScript);
  result.display = option(argc, argv, "--display", result.display);
  result.runId = option(argc, argv, "--run-id", result.runId);
  result.groupPrefix = option(argc, argv, "--group-prefix", result.groupPrefix);
  result.controller = option(argc, argv, "--controller-prefix", result.controller);
  result.trustSchema = option(argc, argv, "--trust-schema", result.trustSchema);
  result.motionDir = option(argc, argv, "--motion-dir");
  result.observationStreamDir = option(argc, argv, "--observation-stream-dir");
  result.observationSourceId = option(argc, argv, "--observation-source-id");
  result.worldFrameId = option(argc, argv, "--world-frame-id", result.worldFrameId);
  result.acquisitionKind = option(argc, argv, "--acquisition-kind", result.acquisitionKind);
  result.deliveryMode = option(argc, argv, "--delivery-mode", result.deliveryMode);
  result.sourceGenerations = option(argc, argv, "--source-generations");
  result.sourceEpoch = std::stoull(option(argc, argv, "--source-epoch", "1"));
  result.windowCount = static_cast<size_t>(std::stoul(option(argc, argv, "--window-count", "1")));
  result.fault = option(argc, argv, "--fault", "none");
  if ((!result.trackingProfile.empty() || !result.reidModel.empty() ||
       !result.describePersonComputation.empty()) &&
      (result.role != "compute" || result.trackingProfile.empty() || result.reidModel.empty() ||
       (!result.describePersonComputation.empty() && result.describePersonComputation != "stdout"))) {
    throw std::invalid_argument("person mode requires compute role, profile and ReID model; "
                                "--describe-person-computation accepts stdout only");
  }
  if ((result.role != "source" && result.role != "compute") ||
      (result.role == "source" && (result.camera.empty() ||
                                    (result.videoSource.empty() && result.observationStreamDir.empty()) ||
                                    (result.frameFile.empty() && result.frameDir.empty() &&
                                     result.observationStreamDir.empty()))) ||
      (result.role == "compute" && result.model.empty())) {
    throw std::invalid_argument(
      "usage: --role source --camera UAV1|UAV2|UAV3 --video-source PATH "
      "--model PATH, or --role compute --model PATH");
  }
  if (result.role == "source" &&
      (result.camera != "UAV1" && result.camera != "UAV2" && result.camera != "UAV3")) {
    throw std::invalid_argument("source camera must be UAV1, UAV2, or UAV3");
  }
  if (result.windowCount == 0 || result.windowCount > 600) {
    throw std::invalid_argument("window-count must be in the range 1..600");
  }
  if (result.role == "source" && !result.observationStreamDir.empty()) {
    if (result.observationSourceId.empty() || result.worldFrameId.empty() ||
        result.sourceEpoch == 0 ||
        (result.acquisitionKind != "simulated" && result.acquisitionKind != "measured") ||
        (result.deliveryMode != "generated" && result.deliveryMode != "replay" &&
         result.deliveryMode != "live-push")) {
      throw std::invalid_argument("observation source identity or provenance is invalid");
    }
  }
  return result;
}

size_t
windowIndex(const std::string& window)
{
  constexpr std::string_view prefix = "window-";
  if (window.compare(0, prefix.size(), prefix) != 0 || window.size() == prefix.size()) {
    throw std::invalid_argument("window id must use window-N format");
  }
  const auto value = std::stoul(window.substr(prefix.size()));
  if (value > 599) throw std::invalid_argument("window index exceeds replay bound");
  return static_cast<size_t>(value);
}

class UserCancellationMonitor
{
public:
  UserCancellationMonitor(nsf::ServiceProvider::CollaborationContext& context,
                          std::mutex& commitMutex,
                          std::shared_ptr<std::atomic<bool>> cancellationReceived,
                          std::string runId, std::string missionId,
                          std::string windowId,
                          std::filesystem::path testAckReleasePath = {})
    : m_context(context)
    , m_commitMutex(commitMutex)
    , m_cancellationReceived(std::move(cancellationReceived))
    , m_runId(std::move(runId))
    , m_missionId(std::move(missionId))
    , m_windowId(std::move(windowId))
    , m_testAckReleasePath(std::move(testAckReleasePath))
  {
    m_context.allowData(uav::TRACKING_INPUT_KEY_SCOPE, uav::TRACKING_CANCEL_TOPIC);
    m_thread = std::thread([this] { watch(); });
  }

  ~UserCancellationMonitor()
  {
    m_stopping.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
  }

  UserCancellationMonitor(const UserCancellationMonitor&) = delete;
  UserCancellationMonitor& operator=(const UserCancellationMonitor&) = delete;

private:
  void watch() noexcept
  {
    size_t nextCount = 1;
    while (!m_stopping.load(std::memory_order_acquire)) {
      try {
        const auto events = m_context.waitFor(
          uav::TRACKING_INPUT_KEY_SCOPE, uav::TRACKING_CANCEL_TOPIC, nextCount, 50);
        if (events.size() < nextCount) continue;
        for (size_t i = nextCount - 1; i < events.size(); ++i) {
          const auto& event = events[i];
          nextCount = events.size() + 1;
          if (event.producer != m_context.requesterName() ||
              event.producerRole != "user-control-v1" ||
              event.keyScope != uav::TRACKING_INPUT_KEY_SCOPE ||
              !uav::TRACKING_CANCEL_TOPIC.isPrefixOf(event.topic)) {
            continue;
          }
          const auto fields = uav::decodeFields(std::string(
            reinterpret_cast<const char*>(event.payload.data()), event.payload.size()));
          if (uav::fieldOr(fields, "schema", "") != uav::TRACKING_CANCEL_SCHEMA ||
              uav::fieldOr(fields, "request_id", "") != m_context.sessionId() ||
              uav::fieldOr(fields, "run_id", "") != m_runId ||
              uav::fieldOr(fields, "mission_id", "") != m_missionId ||
              uav::fieldOr(fields, "window_id", "") != m_windowId) {
            continue;
          }
          if (!m_testAckReleasePath.empty()) {
            std::cerr << "SPEC210_USER_CANCEL_WAITING_FOR_TERMINAL_LOCK window="
                      << m_windowId << std::endl;
          }
          {
            // Serialize revocation with the transaction's final authority
            // check and manifest rename. Once ACKed, no later commit can win.
            std::lock_guard<std::mutex> lock(m_commitMutex);
            m_cancellationReceived->store(true, std::memory_order_release);
            m_context.revokeCurrentWorkAuthority();
          }
          if (!m_testAckReleasePath.empty()) {
            std::cerr << "SPEC210_USER_CANCEL_ACK_HELD window=" << m_windowId
                      << std::endl;
            const auto deadline = std::chrono::steady_clock::now() +
              std::chrono::seconds(20);
            while (!std::filesystem::exists(m_testAckReleasePath) &&
                   std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
          }
          m_context.publish(uav::TRACKING_INPUT_KEY_SCOPE,
                            uav::TRACKING_CANCEL_ACK_TOPIC,
                            fieldsBuffer({
                              {"schema", uav::TRACKING_CANCEL_ACK_SCHEMA},
                              {"request_id", m_context.sessionId()},
                              {"run_id", m_runId},
                              {"mission_id", m_missionId},
                              {"window_id", m_windowId},
                              {"authority_revoked", "true"},
                            }));
          std::cerr << "SPEC210_USER_CANCEL_OBSERVED window=" << m_windowId
                    << " request=" << m_context.sessionId() << std::endl;
          std::cerr << "SPEC210_USER_CANCEL_ACK_QUEUED window=" << m_windowId
                    << " authority_revoked=true" << std::endl;
          return;
        }
      }
      catch (const std::exception& error) {
        std::cerr << "SPEC210_USER_CANCEL_MONITOR_FAILED window=" << m_windowId
                  << " reason=" << error.what() << std::endl;
        // A broken cancellation monitor cannot authorize a result commit.
        {
          std::lock_guard<std::mutex> lock(m_commitMutex);
          m_context.revokeCurrentWorkAuthority();
        }
        return;
      }
      catch (...) {
        std::cerr << "SPEC210_USER_CANCEL_MONITOR_FAILED window=" << m_windowId
                  << " reason=nonstandard-exception" << std::endl;
        {
          std::lock_guard<std::mutex> lock(m_commitMutex);
          m_context.revokeCurrentWorkAuthority();
        }
        return;
      }
    }
  }

private:
  nsf::ServiceProvider::CollaborationContext& m_context;
  std::mutex& m_commitMutex;
  std::shared_ptr<std::atomic<bool>> m_cancellationReceived;
  std::string m_runId;
  std::string m_missionId;
  std::string m_windowId;
  std::filesystem::path m_testAckReleasePath;
  std::atomic<bool> m_stopping{false};
  std::thread m_thread;
};

ndn::Buffer
readBytes(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open frame file: " + path);
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0 || size > 8 * 1024 * 1024) {
    throw std::runtime_error("frame file is empty or exceeds 8 MiB");
  }
  input.seekg(0, std::ios::beg);
  ndn::Buffer bytes(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("short read from frame file: " + path);
  }
  return bytes;
}

std::string
jsonQuote(const std::string& value)
{
  std::ostringstream out;
  out << '"';
  for (const unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned>(c) << std::dec;
        }
        else out << static_cast<char>(c);
    }
  }
  out << '"';
  return out.str();
}

std::string
jsonNumber(double value)
{
  if (!std::isfinite(value)) throw std::invalid_argument("non-finite observation value");
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return jsonQuote(out.str());
}

std::string sha256(const ndn::Buffer& bytes);

std::string
sha256File(const std::filesystem::path& path,
           uint64_t maxBytes = std::numeric_limits<uint64_t>::max(),
           const std::function<bool()>& stillAuthorized = {})
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read pinned worker input: " + path.string());
  ndn::util::Sha256 digest;
  std::array<char, 64 * 1024> chunk{};
  uint64_t totalBytes = 0;
  while (input) {
    if (stillAuthorized && !stillAuthorized()) {
      throw std::runtime_error("person artifact inspection cancelled");
    }
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = input.gcount();
    if (count > 0) {
      if (static_cast<uint64_t>(count) > maxBytes - totalBytes) {
        throw std::runtime_error("pinned worker input exceeds byte bound: " + path.string());
      }
      totalBytes += static_cast<uint64_t>(count);
      digest << std::string(chunk.data(), static_cast<size_t>(count));
    }
  }
  if (input.bad()) throw std::runtime_error("failed reading pinned worker input: " + path.string());
  return uav::canonicalSha256Digest(digest.toString());
}

std::string
readPersonProfile(const std::filesystem::path& path)
{
  if (!std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("person profile is not a regular file");
  }
  std::ifstream input(path, std::ios::binary);
  std::string text(uav::MAX_PERSON_PROFILE_BYTES + 1, '\0');
  if (!input) throw std::runtime_error("cannot read person profile");
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(input.gcount()));
  if (input.bad() || text.size() > uav::MAX_PERSON_PROFILE_BYTES) {
    throw std::invalid_argument("person profile exceeds byte bound or cannot be read");
  }
  return text;
}

struct LoadedPersonComputation
{
  uav::UavPersonTrackingProfile profile;
  uav::UavPersonComputationManifest manifest;
};

struct LoadedNativePaths
{
  std::set<std::string> libraries;
  std::exception_ptr error;
};

int
collectLoadedNativePath(dl_phdr_info* info, size_t, void* context) noexcept
{
  auto& paths = *static_cast<LoadedNativePaths*>(context);
  try {
    const std::string name = info->dlpi_name == nullptr ? "" : info->dlpi_name;
    if (name.empty() || name == "linux-vdso.so.1") return 0;
    paths.libraries.insert(std::filesystem::canonical(name).generic_string());
    if (paths.libraries.size() >= 256) {
      throw std::runtime_error("person native inventory exceeds 256 objects including executable");
    }
    return 0;
  }
  catch (...) {
    paths.error = std::current_exception();
    return 1;
  }
}

LoadedPersonComputation
loadPersonComputation(const Options& options,
                      const std::function<bool()>& stillAuthorized = [] { return !g_stop.load(); })
{
  namespace json = ndn_service_framework::json;
  using Kind = json::JsonValue::Kind;
  LoadedPersonComputation result;
  result.profile = uav::parseUavPersonTrackingProfile(readPersonProfile(options.trackingProfile));
  uav::requireUavPersonWindowCapacity(result.profile, options.windowCount);
  uint64_t totalBytes = 0;
  const auto hashArtifact = [&] (const std::filesystem::path& path) {
    if (!stillAuthorized()) throw std::runtime_error("person artifact inspection cancelled");
    constexpr uint64_t MAX_ARTIFACT_BYTES = 1024ULL * 1024 * 1024;
    if (!std::filesystem::is_regular_file(path)) {
      throw std::invalid_argument("person artifact is not a regular file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path);
    if (size > MAX_ARTIFACT_BYTES || size > 4 * MAX_ARTIFACT_BYTES - totalBytes) {
      throw std::invalid_argument("person artifact inventory exceeds byte bound");
    }
    totalBytes += size;
    return sha256File(path, size, stillAuthorized);
  };
  if (hashArtifact(options.model) != result.profile.detectorDigest ||
      hashArtifact(options.reidModel) != result.profile.reidDigest) {
    throw std::invalid_argument("person model files do not match the tracking profile");
  }
  const auto workerDir = std::filesystem::path(options.workerScript).parent_path();
  std::map<std::string, std::string> backend;
  backend.emplace("tracking_worker.py", hashArtifact(options.workerScript));
  for (const auto helper : {"person_reid.py", "person_profile.py", "observation_schema.py",
                            "motion_schema.py", "motion_estimator.py", "byte_track_state.py",
                            "person_runtime.py", "requirements-person.lock"}) {
    backend.emplace(helper, hashArtifact(workerDir / helper));
  }
  const auto runtime = uav::runUavTrackingWorkerProcess(
    {options.workerPython, (workerDir / "person_runtime.py").string(), "--lock",
     (workerDir / "requirements-person.lock").string()},
    std::chrono::seconds(60), 256 * 1024, stillAuthorized);
  if (runtime.outcome != uav::UavTrackingWorkerOutcome::SUCCESS) {
    throw std::runtime_error("person runtime inspection failed: " + runtime.stderrText.substr(0, 1024));
  }
  const auto diagnostic = json::parseJson(runtime.stdoutText, 256 * 1024);
  const auto& root = json::requireObject(diagnostic);
  json::requireExactKeys(root, {"schema", "pythonVersion", "lockDigest", "distributions", "artifacts"});
  if (json::member(root, "schema", Kind::String).string != "uav-person-runtime-v1" ||
      json::member(root, "lockDigest", Kind::String).string != backend.at("requirements-person.lock")) {
    throw std::invalid_argument("person runtime schema or lock mismatch");
  }
  const auto& artifacts = json::member(root, "artifacts", Kind::Object).object;
  const auto& byteTrack = json::member(artifacts, "byteTrackConfig", Kind::Object).object;
  backend.emplace("bytetrack.yaml", json::member(byteTrack, "digest", Kind::String).string);
  backend.emplace("runtime/fingerprint", sha256(bufferFrom(json::renderJson(diagnostic))));
  backend.emplace("runtime/torchreid-source",
                  sha256(bufferFrom("f8cd150fdf77e8d9e1ed143b7f308c2c609ded50")));
  LoadedNativePaths paths;
  if (dl_iterate_phdr(collectLoadedNativePath, &paths) != 0) {
    if (paths.error) std::rethrow_exception(paths.error);
    throw std::runtime_error("cannot enumerate person native artifacts");
  }
  const auto executable = std::filesystem::canonical("/proc/self/exe");
  std::map<std::string, std::string> native;
  native.emplace("executable:" + executable.generic_string(), hashArtifact(executable));
  for (const auto& path : paths.libraries) {
    native.emplace("library:" + path, hashArtifact(path));
  }
  result.manifest = uav::makeUavPersonComputationManifest(result.profile, backend, native);
  return result;
}

template<typename Range, typename Encode>
std::string
jsonArray(const Range& values, Encode encode)
{
  std::ostringstream out;
  out << '[';
  bool first = true;
  for (const auto& value : values) {
    if (!first) out << ',';
    first = false;
    out << encode(value);
  }
  out << ']';
  return out.str();
}

template<typename Range>
std::string
jsonNumericArray(const Range& values)
{
  return jsonArray(values, [] (double value) { return jsonNumber(value); });
}

std::string
clockBindingJson(const uav::ClockBinding& clock)
{
  std::ostringstream out;
  out << "{\"revision\":" << jsonQuote(std::to_string(clock.revision))
      << ",\"clockId\":" << jsonQuote(clock.clockId)
      << ",\"clockEpoch\":" << jsonQuote(std::to_string(clock.clockEpoch))
      << ",\"a\":" << jsonNumber(clock.a)
      << ",\"bUs\":" << jsonNumber(clock.bUs)
      << ",\"validFromUs\":" << jsonQuote(std::to_string(clock.validFromUs))
      << ",\"validUntilUs\":" << jsonQuote(std::to_string(clock.validUntilUs))
      << ",\"uncertaintyUs\":" << jsonQuote(std::to_string(clock.uncertaintyUs)) << '}';
  return out.str();
}

std::string
calibrationJson(const uav::CalibrationRecord& cal)
{
  std::ostringstream out;
  out << "{\"schema\":" << jsonQuote(cal.schema)
      << ",\"cameraId\":" << jsonQuote(cal.cameraId)
      << ",\"revision\":" << jsonQuote(std::to_string(cal.revision))
      << ",\"width\":" << jsonQuote(std::to_string(cal.width))
      << ",\"height\":" << jsonQuote(std::to_string(cal.height))
      << ",\"fx\":" << jsonNumber(cal.fx) << ",\"fy\":" << jsonNumber(cal.fy)
      << ",\"cx\":" << jsonNumber(cal.cx) << ",\"cy\":" << jsonNumber(cal.cy)
      << ",\"distortion\":" << jsonNumericArray(cal.distortion)
      << ",\"imageTransform\":" << jsonQuote(cal.imageTransform)
      << ",\"validFromUs\":" << jsonQuote(std::to_string(cal.validFromUs))
      << ",\"validUntilUs\":" << jsonQuote(std::to_string(cal.validUntilUs))
      << ",\"anchorSigmaPixels\":";
  if (cal.anchorSigmaPixels) out << jsonNumber(*cal.anchorSigmaPixels);
  else out << "null";
  out << ",\"parameterCovariance\":";
  if (cal.parameterCovariance) out << jsonNumericArray(*cal.parameterCovariance);
  else out << "null";
  out << '}';
  return out.str();
}

std::string
groundJson(const uav::GroundRecord& ground)
{
  std::ostringstream out;
  out << "{\"schema\":" << jsonQuote(ground.schema)
      << ",\"revision\":" << jsonQuote(std::to_string(ground.revision))
      << ",\"worldFrameId\":" << jsonQuote(ground.worldFrameId)
      << ",\"normal\":" << jsonNumericArray(ground.normal)
      << ",\"offsetM\":" << jsonNumber(ground.offsetM)
      << ",\"roiVertices\":" << jsonArray(ground.roiVertices, [] (const auto& point) {
           return jsonNumericArray(point);
         })
      << ",\"geometryValidity\":" << jsonQuote(ground.geometryValidity)
      << ",\"parameterCovariance\":";
  if (ground.parameterCovariance) out << jsonNumericArray(*ground.parameterCovariance);
  else out << "null";
  out << '}';
  return out.str();
}

std::string
frameGapJson(const uav::FrameGap& gap)
{
  const auto& identity = gap.identity;
  std::ostringstream out;
  out << "{\"schema\":" << jsonQuote(gap.schema)
      << ",\"identity\":{\"sourceId\":" << jsonQuote(identity.sourceId)
      << ",\"droneId\":" << jsonQuote(identity.droneId)
      << ",\"cameraId\":" << jsonQuote(identity.cameraId)
      << ",\"epoch\":" << jsonQuote(std::to_string(identity.epoch))
      << ",\"worldFrameId\":" << jsonQuote(identity.worldFrameId)
      << ",\"acquisitionKind\":" << jsonQuote(
           identity.acquisitionKind == uav::AcquisitionKind::Measured ? "measured" : "simulated")
      << ",\"deliveryMode\":" << jsonQuote(
           identity.deliveryMode == uav::DeliveryMode::Generated ? "generated" :
           identity.deliveryMode == uav::DeliveryMode::LivePush ? "live-push" : "replay") << "}"
      << ",\"firstSequence\":" << jsonQuote(std::to_string(gap.firstSequence))
      << ",\"lastSequence\":" << jsonQuote(std::to_string(gap.lastSequence))
      << ",\"reason\":" << jsonQuote(gap.reason) << '}';
  return out.str();
}

std::string
poseSampleJson(const uav::PoseSample& pose)
{
  const auto& identity = pose.identity;
  std::ostringstream out;
  out << "{\"schema\":" << jsonQuote(pose.schema)
      << ",\"identity\":{\"sourceId\":" << jsonQuote(identity.sourceId)
      << ",\"droneId\":" << jsonQuote(identity.droneId)
      << ",\"cameraId\":" << jsonQuote(identity.cameraId)
      << ",\"epoch\":" << jsonQuote(std::to_string(identity.epoch))
      << ",\"worldFrameId\":" << jsonQuote(identity.worldFrameId)
      << ",\"acquisitionKind\":" << jsonQuote(
           identity.acquisitionKind == uav::AcquisitionKind::Measured ? "measured" : "simulated")
      << ",\"deliveryMode\":" << jsonQuote(
           identity.deliveryMode == uav::DeliveryMode::Generated ? "generated" :
           identity.deliveryMode == uav::DeliveryMode::LivePush ? "live-push" : "replay") << "}"
      << ",\"sampleSequence\":" << jsonQuote(std::to_string(pose.sampleSequence))
      << ",\"deviceTimeUs\":" << jsonQuote(std::to_string(pose.deviceTimeUs))
      << ",\"clockBinding\":" << clockBindingJson(pose.clockBinding)
      << ",\"positionW\":" << jsonNumericArray(pose.positionW)
      << ",\"qWC\":" << jsonNumericArray(pose.qWC)
      << ",\"calibrationRevision\":" << jsonQuote(std::to_string(pose.calibrationRevision));
  if (pose.poseCovariance) out << ",\"poseCovariance\":" << jsonNumericArray(*pose.poseCovariance);
  out << '}';
  return out.str();
}

uav::ObservationDataObject
makeObservationObject(const std::string& category, const ndn::Name& name,
                      const ndn::Name& producer, const std::string& payload)
{
  const auto bytes = bufferFrom(payload);
  uav::ObservationDataObject object;
  object.category = category;
  object.exactName = name.toUri();
  object.contentSha256 = sha256(bytes);
  object.producerIdentity = producer.toUri();
  object.payload.assign(bytes.begin(), bytes.end());
  return object;
}

std::string
dataRefJson(const ndn::Name& name, const std::string& digest, const ndn::Name& producer)
{
  return "{\"exactName\":" + jsonQuote(name.toUri()) +
    ",\"contentSha256\":" + jsonQuote(digest) +
    ",\"producerIdentity\":" + jsonQuote(producer.toUri()) + '}';
}

std::string
observationSnapshotJson(const uav::AlignedFrame& aligned, const ndn::Name& producer,
                        std::vector<uav::ObservationDataObject>& dependencies,
                        ndn_service_framework::ServiceProvider::CollaborationContext& context,
                        const std::string& fault)
{
  const auto& identity = aligned.identity;
  const auto& frame = aligned.frame;
  const auto frameDigest = sha256(ndn::Buffer(frame.imageBytes.begin(), frame.imageBytes.end()));
  const auto acquisition = identity.acquisitionKind == uav::AcquisitionKind::Measured
    ? "measured" : "simulated";
  const char* delivery = identity.deliveryMode == uav::DeliveryMode::Generated ? "generated" :
    identity.deliveryMode == uav::DeliveryMode::LivePush ? "live-push" : "replay";
  const auto imageName = uav::makeUavObservationDataName(
    producer, frame.key.cameraId, frame.key.epoch, frame.key.sequence, "FRAME", 1);
  const auto nextFaultRevision = [] (uint64_t revision) {
    if (revision >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::invalid_argument("reference fault requires room for a distinct revision");
    }
    return revision + 1;
  };
  auto publishStatic = [&] (const std::string& category, ndn::Name semanticPrefix,
                            const std::string& payload) {
    const std::string categoryOption = category == "CLOCK" ? "clock" :
                                      category == "CALIBRATION" ? "calibration" : "ground";
    const bool missing = category == "CLOCK" && fault == "missing-reference-clock";
    const bool revisionMismatch = fault == "bad-reference-revision-" + categoryOption;
    if (missing || revisionMismatch) {
      const auto revision = std::stoull(semanticPrefix.get(-1).toUri());
      semanticPrefix = semanticPrefix.getPrefix(-1).append(
        std::to_string(nextFaultRevision(revision)));
    }
    if (missing) {
      // This dedicated semantic prefix has never been published. Do not commit
      // CLOCK and then pretend dropping a DTO removes its retained packets.
      auto name = semanticPrefix;
      name.appendVersion(1);
      dependencies.push_back(makeObservationObject(category, name, producer, payload));
      return name;
    }
    auto publishedPayload = payload;
    if (fault == "bad-reference-content-" + categoryOption) {
      publishedPayload.front() ^= 1;
    }
    const ndn::Buffer protectedBytes(publishedPayload.begin(), publishedPayload.end());
    const auto publication = context.publishImmutableProtectedData(
      semanticPrefix, protectedBytes);
    if (!publication.success) {
      throw std::runtime_error("static " + category + " publication failed: " +
                               publication.errorMessage);
    }
    auto object = makeObservationObject(category, publication.dataName, producer, payload);
    if (publication.contentSha256 != sha256(protectedBytes)) {
      throw std::runtime_error("static " + category + " publication digest changed");
    }
    dependencies.push_back(std::move(object));
    return publication.dataName;
  };
  auto clock = frame.clockBinding;
  auto calibration = aligned.calibration;
  auto ground = aligned.ground;
  if (fault == "bad-reference-content-clock") clock.revision = nextFaultRevision(clock.revision);
  if (fault == "bad-reference-content-calibration" && calibration) {
    if (calibration->externalDataRef) {
      throw std::invalid_argument("content fault requires an owned calibration record");
    }
    calibration->revision = nextFaultRevision(calibration->revision);
  }
  if (fault == "bad-reference-content-ground" && ground) {
    ground->revision = nextFaultRevision(ground->revision);
  }
  const auto clockPayload = clockBindingJson(clock);
  dependencies.clear();
  const auto clockName = publishStatic("CLOCK", uav::makeUavClockDataName(
    producer, clock.revision, 1).getPrefix(-1), clockPayload);
  std::optional<ndn::Name> calibrationName;
  std::optional<ndn::Name> calibrationProducer;
  std::optional<std::string> calibrationDigest;
  if (calibration) {
    if (calibration->externalDataRef) {
      const auto& reference = *calibration->externalDataRef;
      const ndn::Name referenceName(reference.exactName);
      const ndn::Name referenceProducer(reference.producerIdentity);
      uav::requireObservationReferenceNameShape(
        uav::parseObservationReference(reference.exactName, reference.contentSha256,
                                       reference.producerIdentity, "CALIBRATION"),
        "CALIBRATION", frame.key.cameraId, frame.key.epoch, frame.key.sequence);
      const auto expectedName = uav::makeUavCalibrationDataName(
        referenceProducer, frame.key.cameraId, calibration->revision,
        referenceName.get(-1).toVersion());
      const auto validDigest = [&reference] {
        if (reference.contentSha256.size() != 71 ||
            reference.contentSha256.compare(0, 7, "sha256:") != 0) return false;
        return std::all_of(reference.contentSha256.begin() + 7,
                           reference.contentSha256.end(), [] (unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
      };
      if (referenceProducer.empty() || referenceProducer.toUri() != reference.producerIdentity ||
          referenceName.empty() || referenceName.toUri() != reference.exactName ||
          referenceName != expectedName || !validDigest()) {
        throw std::invalid_argument("external calibration DataRef is invalid");
      }
      calibrationName = referenceName;
      calibrationProducer = referenceProducer;
      calibrationDigest = reference.contentSha256;
    }
    else {
      const auto calibrationPayload = calibrationJson(*calibration);
      calibrationName = publishStatic("CALIBRATION", uav::makeUavCalibrationDataName(
        producer, frame.key.cameraId, calibration->revision, 1).getPrefix(-1),
        calibrationPayload);
      calibrationDigest = dependencies.back().contentSha256;
      calibrationProducer = producer;
    }
  }
  std::optional<std::string> groundPayload;
  std::optional<ndn::Name> groundName;
  if (ground) {
    groundPayload = groundJson(*ground);
    groundName = publishStatic("GROUND", uav::makeUavGroundDataName(
      producer, ground->revision, 1).getPrefix(-1), *groundPayload);
  }
  std::vector<std::string> posePayloads;
  std::vector<ndn::Name> poseNames;
  if (aligned.poseStatus == uav::PoseStatus::Aligned) {
    for (const auto& pose : aligned.poseEndpoints) {
      posePayloads.push_back(poseSampleJson(pose));
      poseNames.push_back(uav::makeUavObservationDataName(
        producer, frame.key.cameraId, frame.key.epoch, frame.key.sequence,
        "POSE", pose.sampleSequence));
      dependencies.push_back(makeObservationObject(
        "POSE", poseNames.back(), producer, posePayloads.back()));
    }
  }

  std::ostringstream out;
  out << "{\"schema\":\"uav-observation-v2\",\"identity\":{"
      << "\"sourceId\":" << jsonQuote(identity.sourceId)
      << ",\"droneId\":" << jsonQuote(identity.droneId)
      << ",\"cameraId\":" << jsonQuote(identity.cameraId)
      << ",\"epoch\":" << jsonQuote(std::to_string(identity.epoch))
      << ",\"worldFrameId\":" << jsonQuote(identity.worldFrameId)
      << ",\"acquisitionKind\":" << jsonQuote(acquisition)
      << ",\"deliveryMode\":" << jsonQuote(delivery) << "}"
      << ",\"key\":{\"sourceId\":" << jsonQuote(frame.key.sourceId)
      << ",\"cameraId\":" << jsonQuote(frame.key.cameraId)
      << ",\"epoch\":" << jsonQuote(std::to_string(frame.key.epoch))
      << ",\"sequence\":" << jsonQuote(std::to_string(frame.key.sequence)) << "}"
      << ",\"captureTimeUs\":" << jsonQuote(std::to_string(aligned.captureTimeUs))
      << ",\"timeUncertaintyUs\":" << jsonQuote(std::to_string(aligned.timeUncertaintyUs))
      << ",\"width\":" << jsonQuote(std::to_string(frame.width))
      << ",\"height\":" << jsonQuote(std::to_string(frame.height))
      << ",\"imageRef\":" << dataRefJson(imageName, frameDigest, producer)
      << ",\"clockRef\":" << dataRefJson(
           clockName, dependencies.front().contentSha256, producer);

  if (calibrationName) {
    if (!calibrationDigest || !calibrationProducer) {
      throw std::logic_error("calibration reference is incomplete");
    }
    out << ",\"calibrationRef\":" << dataRefJson(
      *calibrationName, *calibrationDigest, *calibrationProducer);
  }
  if (groundName) {
    const auto& object = *std::find_if(dependencies.begin(), dependencies.end(),
      [] (const auto& item) { return item.category == "GROUND"; });
    out << ",\"groundRef\":" << dataRefJson(
      *groundName, object.contentSha256, producer);
  }
  out << ",\"poseSampleRefs\":" << jsonArray(poseNames, [&] (const ndn::Name& name) {
           const auto found = std::find_if(dependencies.begin(), dependencies.end(),
             [&] (const auto& item) { return item.exactName == name.toUri(); });
           if (found == dependencies.end()) throw std::logic_error("pose observation reference disappeared");
           return dataRefJson(name, found->contentSha256, producer);
         });

  out << ",\"poseStatus\":" << jsonQuote(aligned.poseStatus == uav::PoseStatus::Aligned
                                               ? "aligned" : "unknown");
  if (aligned.poseStatus == uav::PoseStatus::Aligned) {
    if (!aligned.positionW || !aligned.qWC) {
      throw std::logic_error("aligned observation has no pose estimate");
    }
    out << ",\"positionW\":" << jsonNumericArray(*aligned.positionW)
        << ",\"qWC\":";
    out << jsonNumericArray(*aligned.qWC);
    if (aligned.poseCovariance) out << ",\"poseCovariance\":" << jsonNumericArray(*aligned.poseCovariance);
  }
  std::string reason = aligned.reason;
  if (!aligned.calibration) {
    if (!reason.empty()) reason += ";";
    reason += "CALIBRATION_MISSING";
  }
  if (!aligned.ground) {
    if (!reason.empty()) reason += ";";
    reason += "GROUND_MISSING";
  }
  out << ",\"geometryValidity\":"
      << jsonQuote(aligned.ground ? aligned.ground->geometryValidity : "illustrative")
      << ",\"reason\":" << jsonQuote(reason) << '}';
  return out.str();
}

std::string
sha256(const ndn::Buffer& bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty()) {
    digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return uav::canonicalSha256Digest(digest.toString());
}

using JsonTree = boost::property_tree::ptree;

JsonTree
parseJson(const std::string& bytes, const std::string& label)
{
  std::istringstream input(bytes);
  JsonTree result;
  try {
    boost::property_tree::read_json(input, result);
  }
  catch (const std::exception& error) {
    throw std::runtime_error(label + " is invalid JSON: " + error.what());
  }
  return result;
}

std::string
jsonString(const JsonTree& object, const std::string& key)
{
  const auto value = object.get_optional<std::string>(key);
  if (!value || value->empty()) {
    throw std::runtime_error("observation field is missing: " + key);
  }
  return *value;
}

uint64_t
jsonPositiveInteger(const JsonTree& object, const std::string& key)
{
  const auto text = jsonString(object, key);
  size_t parsed = 0;
  const auto value = std::stoull(text, &parsed);
  if (parsed != text.size() || value == 0) {
    throw std::runtime_error("observation field is not a positive integer: " + key);
  }
  return value;
}

uint64_t
personUnsignedField(const JsonTree& object, const std::string& key, uint64_t maximum)
{
  const auto text = jsonString(object, key);
  uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value > maximum) {
    throw std::invalid_argument("invalid bounded person observation field: " + key);
  }
  return value;
}

std::string
personBackendInputJson(const uav::UavPersonWindowContext& context,
                       const std::string& previousBackendState)
{
  std::ostringstream out;
  out << "{\"schema\":\"uav-person-backend-input-v1\",\"runId\":" << jsonQuote(context.runId)
      << ",\"missionId\":" << jsonQuote(context.missionId)
      << ",\"windowId\":" << jsonQuote(context.windowId)
      << ",\"sequence\":" << context.sequence
      << ",\"computationDigest\":" << jsonQuote(context.computationDigest) << ",\"frames\":[";
  for (size_t i = 0; i < context.frames.size(); ++i) {
    if (i != 0) out << ',';
    const auto& frame = context.frames[i];
    out << "{\"cameraId\":" << jsonQuote(frame.cameraId)
        << ",\"sourceId\":" << jsonQuote(frame.sourceId)
        << ",\"inputDigest\":" << jsonQuote(frame.inputDigest)
        << ",\"sourceEpoch\":" << frame.sourceEpoch
        << ",\"trackerGeneration\":" << frame.trackerGeneration
        << ",\"sequence\":" << frame.sequence << ",\"commonPtsUs\":" << frame.commonPtsUs
        << ",\"width\":" << frame.width << ",\"height\":" << frame.height << '}';
  }
  out << "],\"previousBackendState\":"
      << (previousBackendState.empty() ? "null" : previousBackendState) << '}';
  return out.str();
}

std::filesystem::path
trackingWorkerRoot(const Options& options)
{
  const char* privateRoot = std::getenv("NDNSF_SPEC191_PRIVATE_DIR");
  return (privateRoot != nullptr && *privateRoot != '\0'
    ? std::filesystem::path(privateRoot)
    : std::filesystem::temp_directory_path() / ("ndnsf-spec191-" + options.runId)) / "worker";
}

std::vector<std::string>
personSessionArguments(const Options& options)
{
  const auto root = trackingWorkerRoot(options);
  return {options.workerPython, options.workerScript, "--person-session",
          "--model", options.model, "--reid-model", options.reidModel,
          "--tracking-profile", options.trackingProfile,
          "--input-dir", (root / "verified-input").string(),
          "--person-window", (root / "person-backend-input.json").string(),
          "--output-json", (root / "tracking-result.json").string(),
          "--annotated-dir", (root / "annotated").string(),
          "--run-id", options.runId, "--mission-id", options.runId,
          "--window-id", "window-0", "--sequence", "1", "--pts-us", "0"};
}

std::string
personSessionRequest(const uav::UavPersonWindowContext& context, const std::string& jobId)
{
  std::ostringstream out;
  out << "{\"schema\":\"uav-person-worker-request-v1\",\"jobId\":" << jsonQuote(jobId)
      << ",\"runId\":" << jsonQuote(context.runId)
      << ",\"missionId\":" << jsonQuote(context.missionId)
      << ",\"windowId\":" << jsonQuote(context.windowId)
      << ",\"sequence\":" << context.sequence
      << ",\"computationDigest\":" << jsonQuote(context.computationDigest) << "}\n";
  return out.str();
}

struct VerifiedObservationReference
{
  std::string category;
  std::string exactName;
  std::string contentSha256;
  std::string producerIdentity;
  std::string fileName;
  ndn::Buffer payload;
};

void
requireObjectFields(const JsonTree& object, const std::set<std::string>& required,
                    const std::set<std::string>& optional, const std::string& label)
{
  std::set<std::string> seen;
  for (const auto& child : object) {
    if (child.first.empty() || !seen.insert(child.first).second ||
        (required.count(child.first) == 0 && optional.count(child.first) == 0)) {
      throw std::runtime_error(label + " has an unknown or duplicate field");
    }
  }
  for (const auto& field : required) {
    if (seen.count(field) == 0) {
      throw std::runtime_error(label + " is missing field " + field);
    }
  }
}

uav::ParsedObservationReference
parseDataRef(const JsonTree& reference, const std::string& category)
{
  requireObjectFields(reference, {"exactName", "contentSha256", "producerIdentity"}, {},
                      category + " DataRef");
  const auto nameText = jsonString(reference, "exactName");
  return uav::parseObservationReference(nameText, jsonString(reference, "contentSha256"),
                                        jsonString(reference, "producerIdentity"), category);
}

void
requireReferenceNameShape(const uav::ParsedObservationReference& reference,
                          const std::string& category,
                          const std::string& camera, uint64_t epoch, uint64_t sequence)
{
  uav::requireObservationReferenceNameShape(reference, category, camera, epoch, sequence);
}

void
validateDataRef(const JsonTree& reference, const ndn::Name& expectedName,
                const ndn::Name& expectedProducer, const std::string& expectedDigest,
                const std::string& category)
{
  const auto parsed = parseDataRef(reference, category);
  uav::validateObservationReference(parsed, expectedName, expectedProducer,
                                    expectedDigest, category);
}

std::string
bufferText(const ndn::Buffer& bytes)
{
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void
writeBytes(const std::filesystem::path& path, const ndn::Buffer& bytes)
{
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("cannot create worker input: " + path.string());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("cannot write worker input: " + path.string());
}

ndn::Buffer
fieldsBuffer(const uav::Fields& fields)
{
  const auto encoded = uav::encodeFields(fields);
  return bufferFrom(encoded);
}

uav::Fields
requestFields(const nsf::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return uav::decodeFields(std::string(reinterpret_cast<const char*>(payload.data()),
                                       payload.size()));
}

uav::SourceIdentity
observationIdentity(const Options& options, const std::string& sourceId, uint64_t sourceEpoch)
{
  uav::SourceIdentity identity;
  identity.sourceId = sourceId;
  identity.droneId = options.camera;
  identity.cameraId = options.camera;
  identity.epoch = sourceEpoch;
  identity.worldFrameId = options.worldFrameId;
  identity.acquisitionKind = options.acquisitionKind == "measured"
    ? uav::AcquisitionKind::Measured : uav::AcquisitionKind::Simulated;
  if (options.deliveryMode == "generated") identity.deliveryMode = uav::DeliveryMode::Generated;
  else if (options.deliveryMode == "live-push") identity.deliveryMode = uav::DeliveryMode::LivePush;
  else identity.deliveryMode = uav::DeliveryMode::Replay;
  return identity;
}

uav::AlignedFrame
readObservationStream(const Options& options, const std::filesystem::path& path,
                      const std::string& window, const uav::SourceIdentity& sourceIdentity)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("cannot open observation stream: " + path.string());
  const uav::ObservationPolicy policy;
  const auto steadyNowUs = [] {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  bool sourceOwnsFd = false;
  try {
    uav::ObservationIngress ingress(sourceIdentity, policy, steadyNowUs);
    uav::StreamObservationSource source(fd, sourceIdentity, policy);
    sourceOwnsFd = true;
    source.start(ingress);
    bool eof = false;
    bool sawReadableInput = false;
    bool hasBufferedEvents = false;
    size_t loggedGaps = 0;
    const auto streamDeadline = steadyNowUs() +
      static_cast<uint64_t>(policy.maxSourceAgeMs) * 1000;
    while (!eof) {
      if (g_stop.load()) throw std::runtime_error("observation stream cancelled");
      const auto now = steadyNowUs();
      if (now >= streamDeadline) {
        throw std::runtime_error("observation stream exceeded its source-age deadline");
      }
      const auto remainingMs = (streamDeadline - now + 999) / 1000;
      if (!hasBufferedEvents) {
        pollfd descriptor{fd, POLLIN, 0};
        const int pollTimeoutMs = static_cast<int>(std::min<uint64_t>(remainingMs, 50));
        const int ready = ::poll(&descriptor, 1, pollTimeoutMs);
        if (ready < 0) {
          if (errno == EINTR) continue;
          throw std::runtime_error("observation stream poll failed");
        }
        if (ready == 0) continue;
        if ((descriptor.revents & POLLNVAL) != 0 ||
            ((descriptor.revents & POLLERR) != 0 && (descriptor.revents & POLLIN) == 0)) {
          throw std::runtime_error("observation stream poll reported an I/O error");
        }
        if ((descriptor.revents & POLLIN) != 0) {
          sawReadableInput = true;
        }
        else if ((descriptor.revents & POLLHUP) != 0 && !sawReadableInput) {
          // A read-only FIFO reports HUP before its first writer connects. Do not
          // confuse that state with EOF; wait for input, but remain bounded.
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        else if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
          continue;
        }
      }
      hasBufferedEvents = false;
      const auto result = source.pump(64);
      // Preserve accepted source decisions even when a later event in this
      // pump fails. The owned ledger is append-only until this source closes.
      const auto gaps = ingress.frameGaps();
      for (; loggedGaps < gaps.size(); ++loggedGaps) {
        std::cout << ("{\"event\":\"SOURCE_FRAME_GAP\",\"gap\":" +
                      frameGapJson(gaps[loggedGaps]) + '}') << std::endl;
      }
      if (result.rejectedEvents != 0) {
        throw std::runtime_error("observation stream rejected an event: " + result.lastError);
      }
      eof = result.eof;
      hasBufferedEvents = result.hasBufferedEvents;
    }
    if (ingress.queuedFrames() > 1) {
      throw std::runtime_error("observation stream must contain exactly one frame");
    }
    const auto index = windowIndex(window);
    const uav::FrameKey key{sourceIdentity.sourceId, sourceIdentity.cameraId,
                            sourceIdentity.epoch, static_cast<uint64_t>(index + 1)};
    // EOF means no later event can supply this frame's pose. Advance the same
    // steady-clock domain through the configured wait bound so absent pose
    // degrades to an image-only observation instead of failing the window.
    const auto deadline = steadyNowUs() +
      static_cast<uint64_t>(policy.maxAlignWaitMs) * 1000;
    auto result = ingress.take(key, deadline);
    if (result.status != uav::AlignedObservationStatus::Ready || !result.aligned) {
      throw std::runtime_error("observation stream did not produce a ready frame: " + result.reason);
    }
    return std::move(*result.aligned);
  }
  catch (...) {
    if (!sourceOwnsFd) ::close(fd);
    throw;
  }
}

int
run(const Options& options)
{
  std::optional<LoadedPersonComputation> personComputation;
  if (!options.trackingProfile.empty()) {
    personComputation = loadPersonComputation(options);
    if (!options.describePersonComputation.empty()) {
      std::cout << personComputation->manifest.canonicalJson << std::endl;
      return 0;
    }
  }
  if (options.role == "compute" && options.fault == "user-cancel-before-person-commit" &&
      !personComputation) {
    throw std::invalid_argument("person precommit cancellation fault requires person computation");
  }
  if (options.role == "compute" && options.fault == "unauthorized-frame-cache" &&
      !personComputation) {
    throw std::invalid_argument("unauthorized cache fault requires person computation");
  }
  if (options.trustSchema.empty() || !std::filesystem::is_regular_file(options.trustSchema)) {
    throw std::runtime_error("trust schema is not a regular file: " + options.trustSchema);
  }
  const ndn::Name identity = options.role == "source"
    ? uav::droneIdentity(options.camera) : ndn::Name("/example/uav/compute");
  if (options.role == "source" && !options.videoSource.empty() &&
      !std::filesystem::is_regular_file(options.videoSource)) {
    throw std::runtime_error("source video is not a regular file: " + options.videoSource);
  }
  if ((options.role == "compute" || options.observationStreamDir.empty()) &&
      !std::filesystem::is_regular_file(options.model)) {
    throw std::runtime_error("tracking model is not a regular file: " + options.model);
  }
  const std::string modelDigest = options.role == "compute"
    ? sha256File(options.model) : std::string();
  const std::string workerImplementationDigest = options.role == "compute"
    ? sha256(bufferFrom(sha256File(options.workerScript) + sha256File(
        std::filesystem::path(options.workerScript).parent_path() / "motion_estimator.py")))
    : std::string();
  std::optional<uav::UavSourceGenerationSchedule> sourceGenerations;
  if (!options.sourceGenerations.empty()) {
    sourceGenerations = uav::loadUavSourceGenerationSchedule(
      options.sourceGenerations, options.windowCount);
    if (options.sourceEpoch != sourceGenerations->generations.front().sourceEpoch) {
      throw std::invalid_argument("source generation manifest initial epoch does not match the process");
    }
    if (options.role == "source") {
      const size_t cameraIndex = static_cast<size_t>(options.camera.back() - '1');
      if (cameraIndex >= sourceGenerations->generations.front().sourceIds.size() ||
          options.observationSourceId !=
            sourceGenerations->generations.front().sourceIds[cameraIndex]) {
        throw std::invalid_argument("source generation manifest initial sourceId does not match the process");
      }
    }
  }

  ndn::Face face;
  ndn::KeyChain keyChain;
  // Provider drains its handlers before this root closes the child; Face/KeyChain outlive both.
  const auto personWorkerSession = personComputation && options.role == "compute"
    ? std::make_shared<uav::UavTrackingWorkerSession>(personSessionArguments(options),
        personComputation->profile.limits.at("maxChildLogBytes")) : nullptr;
  const auto providerCert = getOrCreateIdentity(keyChain, identity);
  const auto controllerCert = getOrCreateIdentity(keyChain, ndn::Name(options.controller));
  nsf::CertificatePublisher certificatePublisher(face, keyChain, providerCert.getName());
  nsf::ServiceProvider provider(face, ndn::Name(options.groupPrefix), providerCert,
                                controllerCert, options.trustSchema);
  provider.setPerformanceMode(true);
  provider.setUseTokens(true);

  const ndn::Name service = options.role == "source"
    ? uav::SERVICE_TRACKING_SOURCE : uav::SERVICE_TRACKING_COMPUTE;
  const auto publishedFrameStore = options.role == "source" && !options.observationStreamDir.empty()
    ? std::make_shared<uav::UavPersonPublishedFrameStore>(identity, options.camera) : nullptr;
  provider.addService(
    service,
    nsf::ServiceProvider::AckStrategyHandler(
      [] (const nsf::RequestMessage&) {
        nsf::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "spec191-ready";
        return decision;
      }),
    nsf::ServiceProvider::RequestHandler(
      [options, identity, publishedFrameStore]
      (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&,
                           const ndn::Name&, const nsf::RequestMessage& request) {
        const auto input = payloadText(request);
        if (input.empty()) return makeError("empty Spec191 request payload");
        if (options.role == "source") {
          const auto fields = requestFields(request);
          if (uav::fieldOr(fields, "schema", "") == "uav-person-source-frame-read-v1") {
            try {
              if (!publishedFrameStore) {
                throw std::invalid_argument("source frame reads require observation-stream mode");
              }
              auto bytes = publishedFrameStore->read(fields, requesterIdentity);
              nsf::ResponseMessage response;
              response.setStatus(true);
              response.setErrorInfo("No error");
              response.setPayload(bytes, bytes.size());
              return response;
            }
            catch (const std::exception& error) {
              return makeError(error.what());
            }
          }
        }
        std::ostringstream output;
        output << "schema=spec191-tracking-node-v1;runId=" << options.runId
               << ";provider=" << identity.toUri();
        if (options.role == "source") {
          output << ";camera=" << options.camera
                 << ";videoSourceDigest=local-only;windowPayload=" << input;
        }
        else {
          output << ";algorithm=yolo-bytetrack-cpu-v1;model=" << options.model
                 << ";display=" << options.display << ";verifiedInput=" << input;
        }
        return makeResponse(output.str());
      }),
    nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
  const std::vector<nsf::CollaborationRole> roles =
    options.role == "source" ?
      std::vector<nsf::CollaborationRole>{"Camera-" + options.camera} :
      std::vector<nsf::CollaborationRole>{"Tracking-Compute"};
  const auto personWindowAdmission = std::make_shared<uav::UavPersonWindowAdmission>();
  provider.addCollaborationHandler(
    uav::SERVICE_TRACKING_WINDOW, roles,
    nsf::ServiceProvider::AckStrategyHandler(
      [] (const nsf::RequestMessage&) {
        nsf::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "spec191-collaboration-ready";
        return decision;
      }),
      [options, identity, sourceGenerations, modelDigest, workerImplementationDigest,
       personComputation, personWindowAdmission, publishedFrameStore, personWorkerSession]
      (nsf::ServiceProvider::CollaborationContext& context,
                         const nsf::RequestMessage& request) {
      const auto userCancellationReceived =
        std::make_shared<std::atomic<bool>>(false);
      const auto finishUserCancellation = [&] {
        if (!userCancellationReceived->load(std::memory_order_acquire)) return false;
        context.completeRole();
        std::cerr << "SPEC210_USER_CANCEL_ROLE_CLOSED request="
                  << context.sessionId()
                  << std::endl;
        return true;
      };
      const auto failRole = [&] (const std::string& reason) {
        if (context.role() == "Tracking-Compute" && finishUserCancellation()) {
          std::cerr << "SPEC210_USER_CANCEL_FAILURE_SUPPRESSED reason="
                    << reason.substr(0, 256) << std::endl;
          return;
        }
        context.fail(reason);
        if (context.role() != "Tracking-Compute") return;
        try {
          context.publishFinalFailureResponse(fieldsBuffer({
            {"accepted", "false"},
            {"failure_code", "tracking-compute-failed"},
            {"failure_reason", reason.substr(0, 512)},
          }), reason.substr(0, 512));
        }
        catch (const std::exception&) {
          std::cerr << "SPEC191_TRACKING_FAILURE_RESPONSE_FAILED" << std::endl;
        }
      };
      const auto fields = requestFields(request);
      if (context.role() == "Tracking-Compute") {
        try {
          if (personComputation) {
            uav::requireUavPersonRequestBindingFields(fields, personComputation->profile,
                                                     personComputation->manifest);
          }
          else if (uav::hasUavPersonRequestBindingFields(fields)) {
            throw std::invalid_argument("person request requires a locally configured person compute");
          }
        }
        catch (const std::exception& error) {
          failRole(error.what());
          return;
        }
      }
      const auto mission = uav::fieldOr(fields, "mission_id", options.runId);
      const auto window = uav::fieldOr(fields, "window_id", "window-0");
      size_t index = 0;
      try {
        index = personComputation
          ? uav::parseUavPersonWindowIndex(personComputation->profile, window)
          : windowIndex(window);
      }
      catch (const std::exception& error) {
        failRole(error.what());
        return;
      }
      if (index >= options.windowCount) {
        failRole("window index exceeds configured replay count");
        return;
      }
      const uav::UavSourceGeneration* sourceGeneration = nullptr;
      if (sourceGenerations) {
        try {
          sourceGeneration = &uav::uavSourceGenerationForWindow(*sourceGenerations, index);
        }
        catch (const std::exception& error) {
          failRole(error.what());
          return;
        }
      }
      const auto motionJson = uav::fieldOr(fields, "motion_window_json", "");
      const auto motionDigest = uav::fieldOr(fields, "motion_window_digest", "");
      const auto observationSchema = uav::fieldOr(fields, "observation_schema", "");
      const bool useObservations = observationSchema == "uav-observation-v2";
      if (personComputation && context.role() == "Tracking-Compute" && !useObservations) {
        failRole("person tracking requires verified observation-v2 inputs");
        return;
      }
      if (!observationSchema.empty() && !useObservations) {
        failRole("unsupported observation request schema");
        return;
      }
      const auto sourceEpochText = uav::fieldOr(fields, "source_epoch", "0");
      uint64_t observationEpoch = 0;
      if (useObservations) {
        try {
          observationEpoch = std::stoull(sourceEpochText);
        }
        catch (const std::exception&) {
          failRole("observation source epoch is invalid");
          return;
        }
        if (observationEpoch == 0 || !motionJson.empty()) {
          failRole("v2 observations cannot be mixed with legacy motion metadata");
          return;
        }
        if (sourceGeneration != nullptr) {
          if (observationEpoch != sourceGeneration->sourceEpoch) {
            failRole("request source epoch does not match the configured generation");
            return;
          }
          for (size_t sourceIndex = 0; sourceIndex < 3; ++sourceIndex) {
            const auto sourceKey = "source_uav" + std::to_string(sourceIndex + 1) + "_id";
            if (uav::fieldOr(fields, sourceKey, "") !=
                sourceGeneration->sourceIds[sourceIndex]) {
              failRole("request source identity does not match the configured generation");
              return;
            }
          }
        }
      }
      if (!motionJson.empty() && (motionDigest.empty() ||
                                  sha256(bufferFrom(motionJson)) != motionDigest)) {
        failRole("motion window digest verification failed");
        return;
      }
      if (mission.empty() || window.empty()) {
        failRole("Spec191 collaboration requires mission_id and window_id");
        return;
      }
      const uav::person_json::Json personTraceScope = {
        {"runId", options.runId}, {"missionId", mission}, {"windowId", window},
        {"sequence", index + 1}, {"sourceEpoch", observationEpoch}};
      if (context.role() == "Tracking-Compute") {
        std::unique_lock<std::mutex> personWindowLock;
        if (personComputation) {
          personWindowLock = personWindowAdmission->tryAcquire();
          if (!personWindowLock.owns_lock()) {
            failRole("PERSON_COMPUTE_BUSY");
            return;
          }
        }
        std::mutex workAuthorityCommitMutex;
        std::unique_ptr<UserCancellationMonitor> cancellationMonitor;
        if (useObservations) {
          std::filesystem::path testAckReleasePath;
          if (options.fault == "user-cancel-terminal-response-race" ||
              options.fault == "user-cancel-terminal-response-cancel-wins-race") {
            const char* runRoot = std::getenv("NDNSF_SPEC191_RUN_ROOT");
            if (runRoot == nullptr || *runRoot == '\0') {
              throw std::runtime_error(
                "terminal response race fault requires the MiniNDN run root");
            }
            testAckReleasePath = std::filesystem::path(runRoot) /
              "private/spec210-terminal-response-race/release-ack";
          }
          cancellationMonitor = std::make_unique<UserCancellationMonitor>(
            context, workAuthorityCommitMutex, userCancellationReceived,
            options.runId, mission, window, std::move(testAckReleasePath));
        }
        if (options.fault == "worker-failure") {
          failRole("injected Spec191 worker failure");
          return;
        }
        try {
        std::vector<std::string> digests;
        std::vector<ndn::Buffer> inputs;
        std::vector<ndn::Buffer> observationInputs;
        std::vector<std::string> observationNames;
        std::vector<std::string> observationDigests;
        std::vector<std::string> observationSourceIds;
        std::vector<std::string> imageNames;
        std::vector<std::vector<VerifiedObservationReference>> observationReferences;
        uav::UavPersonWindowContext personContext;
        if (personComputation) {
          personContext.runId = options.runId;
          personContext.missionId = mission;
          personContext.windowId = window;
          personContext.sequence = index + 1;
          personContext.computationDigest = personComputation->manifest.computationDigest;
        }
        std::cout << "SPEC191_TRACKING_COMPUTE_WINDOW_START window=" << window << std::endl;
        for (const auto& camera : {std::string("UAV1"), std::string("UAV2"),
                                   std::string("UAV3")}) {
          if (personComputation) {
            auto trace = personTraceScope;
            trace["cameraId"] = camera;
            trace["sourceId"] = uav::fieldOr(fields, "source_uav" + camera.substr(3) + "_id", "");
            uav::emitUavPersonTrace(std::move(trace), "fetch-start");
          }
          const auto producer = uav::droneIdentity(camera);
          const auto dataName = useObservations
            ? uav::makeUavObservationDataName(
                producer, camera, observationEpoch, index + 1, "FRAME", 1)
            : uav::makeUavEvidenceName(
                producer, mission, window, camera + "-" + std::to_string(index + 1), 1);
          if (options.fault == "unauthorized-frame-cache" && personComputation &&
              camera == "UAV3" && index == 1) {
            cacheProbeReleased.store(false);
            std::cout << "SPEC219_UNAUTHORIZED_CACHE_FETCH_READY name=" << dataName.toUri()
                      << std::endl;
            const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!cacheProbeReleased.load() && !g_stop.load() &&
                   context.hasCurrentWorkAuthority() &&
                   std::chrono::steady_clock::now() < gateDeadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!cacheProbeReleased.load() || !context.hasCurrentWorkAuthority()) {
              failRole("unauthorized cache observer gate expired or authority revoked");
              return;
            }
          }
          std::thread cancelTriggerThread;
          if (options.fault == "cancel-during-active-fetch" && camera == "UAV3") {
            cancelTriggerThread = std::thread([&context, window] {
              // Hold the source's UAV3 publication in this fault so this timer
              // fires only after the corresponding producer-bound fetch is in
              // flight, rather than between selection and dispatch.
              std::this_thread::sleep_for(std::chrono::milliseconds(250));
              const bool revoked = context.revokeCurrentWorkAuthority();
              std::cout << "SPEC210_ACTIVE_FETCH_CANCEL_TRIGGERED camera=UAV3 window="
                        << window << " authority_revoked=" << (revoked ? "true" : "false")
                        << std::endl;
            });
          }
          const auto payload = personComputation
            ? context.fetchReusableLargeFromProducerCurrent(
                dataName, uav::TRACKING_INPUT_KEY_SCOPE, producer, 10000)
            : context.fetchLargeFromProducerCurrent(
                dataName, uav::TRACKING_INPUT_KEY_SCOPE, producer, 10000);
          if (cancelTriggerThread.joinable()) cancelTriggerThread.join();
          if (!payload) {
            if (options.fault == "cancel-during-active-fetch" && camera == "UAV3") {
              std::cerr << "SPEC210_ACTIVE_FETCH_CANCELLED camera=UAV3 window="
                        << window << std::endl;
            }
            failRole("tracking compute could not fetch " + dataName.toUri());
            return;
          }
          if (payload->empty() || payload->size() > 8U * 1024U * 1024U) {
            failRole("tracking image exceeds its declared size limit");
            return;
          }
          if (personComputation &&
              payload->size() > personComputation->profile.limits.at("maxFrameBytes")) {
            throw std::invalid_argument("person frame exceeds its profile byte bound");
          }
          digests.push_back(sha256(*payload));
          inputs.push_back(*payload);
          imageNames.push_back(dataName.toUri());
          if (useObservations) {
            const auto sourceKey = "source_uav" + camera.substr(3) + "_id";
            const auto sourceId = uav::fieldOr(fields, sourceKey, "");
            if (sourceId.empty()) {
              failRole("observation request omitted source identity for " + camera);
              return;
            }
            const auto observationName = uav::makeUavObservationDataName(
              producer, camera, observationEpoch, index + 1, "OBSERVATION", 1);
            const auto observationPayload = personComputation
              ? context.fetchReusableLargeFromProducerCurrent(
                  observationName, "uav-tracking-observation", producer, 10000)
              : context.fetchLargeFromProducerCurrent(
                  observationName, "uav-tracking-observation", producer, 10000);
            if (!observationPayload) {
              failRole("tracking compute could not fetch " + observationName.toUri());
              return;
            }
            if (observationPayload->empty() || observationPayload->size() > 64U * 1024U) {
              failRole("tracking observation sidecar exceeds 64 KiB");
              return;
            }
            const auto observationTree = parseJson(bufferText(*observationPayload),
                                                   "fetched observation sidecar");
            requireObjectFields(observationTree,
              {"schema", "identity", "key", "captureTimeUs", "timeUncertaintyUs", "width",
               "height", "imageRef", "clockRef", "poseSampleRefs", "poseStatus",
               "geometryValidity", "reason"},
              {"calibrationRef", "groundRef", "positionW", "qWC", "poseCovariance"},
              "FrameObservation");
            if (jsonString(observationTree, "schema") != "uav-observation-v2") {
              failRole("tracking compute fetched an unsupported observation schema");
              return;
            }
            const auto& identityTree = observationTree.get_child("identity");
            const auto& keyTree = observationTree.get_child("key");
            requireObjectFields(identityTree,
              {"sourceId", "droneId", "cameraId", "epoch", "worldFrameId", "acquisitionKind",
               "deliveryMode"}, {}, "FrameObservation.identity");
            requireObjectFields(keyTree, {"sourceId", "cameraId", "epoch", "sequence"}, {},
                                "FrameObservation.key");
            const auto observationDroneId = jsonString(identityTree, "droneId");
            const auto observationWorldFrameId = jsonString(identityTree, "worldFrameId");
            const auto observationAcquisitionKind = jsonString(identityTree, "acquisitionKind");
            const auto observationDeliveryMode = jsonString(identityTree, "deliveryMode");
            if (jsonString(identityTree, "cameraId") != camera ||
                jsonString(identityTree, "sourceId") != sourceId ||
                jsonPositiveInteger(identityTree, "epoch") != observationEpoch ||
                sourceId.size() > 128 || observationDroneId.empty() ||
                observationDroneId.size() > 128 || observationWorldFrameId.empty() ||
                observationWorldFrameId.size() > 128 ||
                observationWorldFrameId != options.worldFrameId ||
                (observationAcquisitionKind != "simulated" &&
                 observationAcquisitionKind != "measured") ||
                observationAcquisitionKind != options.acquisitionKind ||
                (observationDeliveryMode != "generated" &&
                 observationDeliveryMode != "replay" &&
                 observationDeliveryMode != "live-push") ||
                observationDeliveryMode != options.deliveryMode ||
                jsonString(keyTree, "cameraId") != camera ||
                jsonString(keyTree, "sourceId") != sourceId ||
                jsonPositiveInteger(keyTree, "epoch") != observationEpoch ||
                jsonPositiveInteger(keyTree, "sequence") != index + 1) {
              failRole("fetched observation sidecar identity does not match the requested frame");
              return;
            }
            const auto geometryValidity = jsonString(observationTree, "geometryValidity");
            if (geometryValidity != "illustrative" && geometryValidity != "calibrated-plane" &&
                geometryValidity != "analytic-fixture") {
              failRole("fetched observation sidecar has invalid geometryValidity");
              return;
            }
            const auto poseStatus = jsonString(observationTree, "poseStatus");
            const auto reason = observationTree.get_optional<std::string>("reason");
            if (!reason || (poseStatus == "unknown" && reason->empty()) ||
                (poseStatus != "aligned" && poseStatus != "unknown")) {
              failRole("fetched observation sidecar has invalid pose status or reason");
              return;
            }
            const auto actualImageDigest = sha256(*payload);
            const auto& imageRef = observationTree.get_child("imageRef");
            validateDataRef(imageRef, dataName, producer, actualImageDigest, "FRAME");

            std::vector<VerifiedObservationReference> verifiedReferences;
            std::set<std::string> referenceNames{dataName.toUri(), observationName.toUri()};
            auto fetchReference = [&] (const std::string& category,
                                       const JsonTree& reference,
                                       const std::string& filename) -> bool {
              const auto dataRef = parseDataRef(reference, category);
              requireReferenceNameShape(dataRef, category, camera,
                                        observationEpoch, index + 1);
              auto expectedProducer = producer;
              if (category == "CALIBRATION") {
                const auto selectedCalibrationProvider =
                  context.assignment().roleProviders.find("Calibration-Provider");
                if (selectedCalibrationProvider !=
                    context.assignment().roleProviders.end()) {
                  expectedProducer = selectedCalibrationProvider->second;
                }
              }
              if (dataRef.producerIdentity != expectedProducer) {
                failRole(category == "CALIBRATION"
                  ? "CALIBRATION reference is not owned by the selected Calibration-Provider"
                  : category + " reference is not owned by the requested source");
                return false;
              }
              const auto exactName = dataRef.exactName.toUri();
              if (!referenceNames.insert(exactName).second) {
                failRole("observation closure contains a duplicate exact Data name");
                return false;
              }
              std::thread cancelReferenceTriggerThread;
              if ((options.fault == "user-cancel-during-active-reference-fetch" ||
                   options.fault == "user-cancel-after-source-switch-during-active-reference-fetch" ||
                   options.fault == "recover-active-reference-fetch") &&
                  camera == "UAV3" && category == "POSE") {
                std::cout << "SPEC210_ACTIVE_REFERENCE_FETCH_STARTED camera=UAV3 window="
                          << window << " category=POSE" << std::endl;
              }
              if (options.fault == "cancel-during-active-reference-fetch" &&
                  camera == "UAV3" && category == "POSE") {
                cancelReferenceTriggerThread = std::thread([&context, window, category] {
                  // The POSE object is deliberately held by the source in this
                  // fault.  Revoke only after its producer-bound fetch is live.
                  std::this_thread::sleep_for(std::chrono::milliseconds(250));
                  const bool revoked = context.revokeCurrentWorkAuthority();
                  std::cout << "SPEC210_ACTIVE_REFERENCE_CANCEL_TRIGGERED camera=UAV3 window="
                            << window << " category=" << category
                            << " authority_revoked=" << (revoked ? "true" : "false")
                            << std::endl;
                });
              }
              std::optional<ndn::Buffer> objectPayload;
              try {
                if (category == "POSE") {
                  objectPayload = personComputation
                    ? context.fetchReusableLargeFromProducerCurrent(
                        dataRef.exactName, "uav-tracking-observation",
                        dataRef.producerIdentity, 10000)
                    : context.fetchLargeFromProducerCurrent(
                        dataRef.exactName, "uav-tracking-observation",
                        dataRef.producerIdentity, 10000);
                }
                else {
                  objectPayload = context.fetchImmutableProtectedDataCurrent(
                    dataRef.exactName, dataRef.producerIdentity,
                    std::min(context.assignment().workDeadline,
                             std::chrono::steady_clock::now() + std::chrono::seconds(10)));
                }
              }
              catch (...) {
                if (cancelReferenceTriggerThread.joinable()) {
                  cancelReferenceTriggerThread.join();
                }
                throw;
              }
              if (cancelReferenceTriggerThread.joinable()) {
                cancelReferenceTriggerThread.join();
              }
              if (!objectPayload) {
                if (options.fault == "cancel-during-active-reference-fetch" &&
                    camera == "UAV3" && category == "POSE") {
                  std::cerr << "SPEC210_ACTIVE_REFERENCE_CANCELLED camera=UAV3 window="
                            << window << " category=" << category << std::endl;
                }
                failRole("tracking compute could not fetch " + exactName);
                return false;
              }
              if (options.fault == "recover-active-reference-fetch" &&
                  camera == "UAV3" && category == "POSE") {
                std::cout << "SPEC210_ACTIVE_REFERENCE_FETCH_COMPLETED camera=UAV3 window="
                          << window << " category=POSE" << std::endl;
              }
              if (objectPayload->empty() || objectPayload->size() > 64U * 1024U) {
                failRole(category + " reference exceeds 64 KiB");
                return false;
              }
              const auto actualDigest = sha256(*objectPayload);
              if (actualDigest != dataRef.contentSha256) {
                failRole("observation dependency content digest mismatch for " + exactName);
                return false;
              }
              if (options.fault.rfind("bad-reference-revision-", 0) == 0 && camera == "UAV3") {
                std::cout << "SPEC210_REFERENCE_DIGEST_VERIFIED camera=" << camera
                          << " window=" << window << " category=" << category
                          << " exactName=" << exactName << std::endl;
              }
              const auto dependencyTree = parseJson(bufferText(*objectPayload),
                                                    "fetched " + category + " dependency");
              ndn::Name expectedName;
              if (category == "CLOCK") {
                requireObjectFields(dependencyTree,
                  {"revision", "clockId", "clockEpoch", "a", "bUs", "validFromUs",
                   "validUntilUs", "uncertaintyUs"}, {}, "CLOCK record");
                expectedName = uav::makeUavClockDataName(
                  dataRef.producerIdentity, jsonPositiveInteger(dependencyTree, "revision"),
                  dataRef.exactName.get(-1).toVersion());
              }
              else if (category == "CALIBRATION") {
                requireObjectFields(dependencyTree,
                  {"schema", "cameraId", "revision", "width", "height", "fx", "fy", "cx", "cy",
                   "distortion", "imageTransform", "validFromUs", "validUntilUs",
                   "anchorSigmaPixels", "parameterCovariance"}, {}, "CALIBRATION record");
                if (jsonString(dependencyTree, "cameraId") != camera) {
                  failRole("CALIBRATION record camera does not match its reference name");
                  return false;
                }
                expectedName = uav::makeUavCalibrationDataName(
                  dataRef.producerIdentity, camera, jsonPositiveInteger(dependencyTree, "revision"),
                  dataRef.exactName.get(-1).toVersion());
              }
              else if (category == "GROUND") {
                requireObjectFields(dependencyTree,
                  {"schema", "revision", "worldFrameId", "normal", "offsetM", "roiVertices",
                   "geometryValidity", "parameterCovariance"}, {}, "GROUND record");
                expectedName = uav::makeUavGroundDataName(
                  dataRef.producerIdentity, jsonPositiveInteger(dependencyTree, "revision"),
                  dataRef.exactName.get(-1).toVersion());
              }
              else {
                requireObjectFields(dependencyTree,
                  {"schema", "identity", "sampleSequence", "deviceTimeUs", "clockBinding",
                   "positionW", "qWC", "calibrationRevision"}, {"poseCovariance"}, "POSE record");
                const auto& poseIdentity = dependencyTree.get_child("identity");
                requireObjectFields(poseIdentity,
                  {"sourceId", "droneId", "cameraId", "epoch", "worldFrameId", "acquisitionKind",
                   "deliveryMode"}, {}, "POSE identity");
                if (jsonString(poseIdentity, "cameraId") != camera ||
                    jsonString(poseIdentity, "sourceId") != sourceId ||
                    jsonPositiveInteger(poseIdentity, "epoch") != observationEpoch ||
                    jsonString(poseIdentity, "droneId") != observationDroneId ||
                    jsonString(poseIdentity, "worldFrameId") != observationWorldFrameId ||
                    jsonString(poseIdentity, "acquisitionKind") != observationAcquisitionKind ||
                    jsonString(poseIdentity, "deliveryMode") != observationDeliveryMode) {
                  failRole("POSE record identity does not match its observation");
                  return false;
                }
                expectedName = uav::makeUavObservationDataName(
                  dataRef.producerIdentity, camera, observationEpoch, index + 1, "POSE",
                  jsonPositiveInteger(dependencyTree, "sampleSequence"));
              }
              if (expectedName != dataRef.exactName) {
                failRole(category + " exact name does not match its record revision");
                return false;
              }
              verifiedReferences.push_back({category, expectedName.toUri(), actualDigest,
                                            dataRef.producerIdentity.toUri(), filename, *objectPayload});
              return true;
            };

            const auto& clockRef = observationTree.get_child("clockRef");
            if (!fetchReference("CLOCK", clockRef, camera + ".reference-CLOCK-0.json")) return;

            const auto calibrationRef = observationTree.get_child_optional("calibrationRef");
            if (calibrationRef) {
              if (!fetchReference("CALIBRATION", *calibrationRef,
                                  camera + ".reference-CALIBRATION-0.json")) return;
            }

            const auto groundRef = observationTree.get_child_optional("groundRef");
            if (groundRef) {
              if (!fetchReference("GROUND", *groundRef,
                                  camera + ".reference-GROUND-0.json")) return;
            }

            const auto& poseRefs = observationTree.get_child("poseSampleRefs");
            if ((poseStatus == "aligned" && (poseRefs.size() < 1 || poseRefs.size() > 2 ||
                                             !observationTree.get_child_optional("positionW") ||
                                             !observationTree.get_child_optional("qWC"))) ||
                (poseStatus == "unknown" && (poseRefs.size() != 0 ||
                                             observationTree.get_child_optional("positionW") ||
                                             observationTree.get_child_optional("qWC") ||
                                             observationTree.get_child_optional("poseCovariance")))) {
              failRole("observation pose status does not match its pose references or estimate");
              return;
            }
            size_t poseIndex = 0;
            for (const auto& reference : poseRefs) {
              if (!reference.first.empty()) {
                failRole("poseSampleRefs must be a JSON array");
                return;
              }
              if (!fetchReference("POSE", reference.second,
                  camera + ".reference-POSE-" + std::to_string(poseIndex) + ".json")) return;
              ++poseIndex;
            }
            if (personComputation) {
              const auto& limits = personComputation->profile.limits;
              auto& frame = personContext.frames[inputs.size() - 1];
              frame.cameraId = camera;
              frame.sourceId = sourceId;
              frame.inputDigest = actualImageDigest;
              frame.sourceEpoch = observationEpoch;
              frame.trackerGeneration = 1;
              frame.sequence = index + 1;
              frame.commonPtsUs = static_cast<int64_t>(personUnsignedField(
                observationTree, "captureTimeUs", std::numeric_limits<int64_t>::max()));
              frame.clockUncertaintyUs = personUnsignedField(
                observationTree, "timeUncertaintyUs", std::numeric_limits<uint64_t>::max());
              frame.width = static_cast<int>(personUnsignedField(
                observationTree, "width", limits.at("maxFrameWidth")));
              frame.height = static_cast<int>(personUnsignedField(
                observationTree, "height", limits.at("maxFrameHeight")));
              if (frame.width == 0 || frame.height == 0) {
                throw std::invalid_argument("person observation has empty image dimensions");
              }
              const auto clock = std::find_if(verifiedReferences.begin(), verifiedReferences.end(),
                [] (const auto& reference) { return reference.category == "CLOCK"; });
              if (clock == verifiedReferences.end()) {
                throw std::invalid_argument("person observation has no verified clock");
              }
              const auto clockTree = parseJson(bufferText(clock->payload), "verified person CLOCK");
              const auto scaleText = jsonString(clockTree, "a");
              const auto offsetText = jsonString(clockTree, "bUs");
              size_t scaleEnd = 0;
              size_t offsetEnd = 0;
              const auto scale = std::stod(scaleText, &scaleEnd);
              const auto offset = std::stod(offsetText, &offsetEnd);
              const auto validFrom = personUnsignedField(clockTree, "validFromUs",
                                                         std::numeric_limits<int64_t>::max());
              const auto validUntil = personUnsignedField(clockTree, "validUntilUs",
                                                          std::numeric_limits<int64_t>::max());
              uav::ClockBinding binding;
              binding.revision = jsonPositiveInteger(clockTree, "revision");
              binding.clockEpoch = jsonPositiveInteger(clockTree, "clockEpoch");
              binding.clockId = jsonString(clockTree, "clockId");
              binding.a = scale;
              binding.bUs = offset;
              binding.validFromUs = validFrom;
              binding.validUntilUs = validUntil;
              binding.uncertaintyUs = personUnsignedField(clockTree, "uncertaintyUs",
                                                         frame.clockUncertaintyUs);
              if (scaleEnd != scaleText.size() || offsetEnd != offsetText.size() ||
                  !uav::isCaptureTimeWithinClock(binding, static_cast<uint64_t>(frame.commonPtsUs))) {
                throw std::invalid_argument("person observation has an invalid clock binding");
              }
              frame.clockAvailable = true;
              auto trace = personTraceScope;
              trace["cameraId"] = camera;
              trace["sourceId"] = sourceId;
              trace["commonPtsUs"] = frame.commonPtsUs;
              uav::emitUavPersonTrace(std::move(trace), "admitted");
            }
            observationSourceIds.push_back(sourceId);
            observationNames.push_back(observationName.toUri());
            observationDigests.push_back(sha256(*observationPayload));
            observationInputs.push_back(*observationPayload);
            observationReferences.push_back(std::move(verifiedReferences));
          }
        }
        const auto workerRoot = trackingWorkerRoot(options);
        const auto inputRoot = workerRoot / "verified-input";
        std::filesystem::create_directories(inputRoot);
        for (size_t index = 0; index < inputs.size(); ++index) {
          writeBytes(inputRoot / ("UAV" + std::to_string(index + 1) + ".jpg"), inputs[index]);
          if (useObservations) {
            writeBytes(inputRoot / ("UAV" + std::to_string(index + 1) + ".observation.json"),
                       observationInputs[index]);
            for (const auto& reference : observationReferences[index]) {
              writeBytes(inputRoot / reference.fileName, reference.payload);
            }
          }
        }
        const auto observationManifestPath = workerRoot / "verified-observation-inputs.json";
        if (useObservations) {
          std::ostringstream manifest;
          manifest << "{\"schema\":\"uav-verified-inputs-v2\",\"sourceEpoch\":"
                   << jsonQuote(std::to_string(observationEpoch)) << ",\"entries\":[";
          for (size_t i = 0; i < observationInputs.size(); ++i) {
            if (i != 0) manifest << ',';
            const auto camera = "UAV" + std::to_string(i + 1);
            manifest << "{\"cameraId\":" << jsonQuote(camera)
                     << ",\"sourceId\":" << jsonQuote(observationSourceIds[i])
                     << ",\"producerIdentity\":" << jsonQuote(uav::droneIdentity(camera).toUri())
                     << ",\"imageName\":" << jsonQuote(imageNames[i])
                     << ",\"imageDigest\":" << jsonQuote(digests[i])
                     << ",\"observationName\":" << jsonQuote(observationNames[i])
                     << ",\"observationDigest\":" << jsonQuote(observationDigests[i])
                     << ",\"references\":[";
            for (size_t referenceIndex = 0;
                 referenceIndex < observationReferences[i].size(); ++referenceIndex) {
              if (referenceIndex != 0) manifest << ',';
              const auto& reference = observationReferences[i][referenceIndex];
              manifest << "{\"category\":" << jsonQuote(reference.category)
                       << ",\"exactName\":" << jsonQuote(reference.exactName)
                       << ",\"contentSha256\":" << jsonQuote(reference.contentSha256)
                       << ",\"producerIdentity\":" << jsonQuote(reference.producerIdentity)
                       << ",\"fileName\":" << jsonQuote(reference.fileName) << '}';
            }
            manifest << "]}";
          }
          manifest << "]}";
          const auto manifestText = manifest.str();
          writeBytes(observationManifestPath, bufferFrom(manifestText));
        }
        const auto workerOutput = workerRoot / "tracking-result.json";
        const auto workerState = workerRoot / "tracking-state.json";
        const auto motionPath = workerRoot / "motion-window.json";
        const auto candidateState = workerRoot / "candidate-state" / (window + ".json");
        const auto personInputPath = workerRoot / "person-backend-input.json";
        const auto resultName = uav::makeUavMultiViewResultName(
          identity, mission, window, index + 1, 1);
        if (!motionJson.empty()) {
          writeBytes(motionPath, bufferFrom(motionJson));
        }
        const auto makeWorkerArguments = [&] (
          const std::optional<std::filesystem::path>& stateInput) {
          std::vector<std::string> arguments = {
            options.workerPython, options.workerScript,
            "--model", options.model,
            "--input-dir", inputRoot.string(),
            "--output-json", workerOutput.string(),
            "--annotated-dir", (workerRoot / "annotated").string(),
            "--run-id", options.runId,
            "--mission-id", mission,
            "--window-id", window,
            "--sequence", std::to_string(index + 1),
            "--pts-us", "0",
          };
          if (stateInput) {
            arguments.insert(arguments.end(), {"--state-file", stateInput->string()});
          }
          if (useObservations) {
            arguments.insert(arguments.end(), {
              "--candidate-state-file", candidateState.string(),
              "--observation-dir", inputRoot.string(),
              "--observation-manifest", observationManifestPath.string(),
            });
          }
          if (!motionJson.empty()) {
            arguments.insert(arguments.end(), {
              "--motion-json", motionPath.string(), "--motion-digest", motionDigest,
            });
            if (index != 0) {
              arguments.push_back("--fail-on-motion-unknown");
            }
          }
          return arguments;
        };
        const auto runWorker = [&] (const std::vector<std::string>& arguments,
                                   const std::string& jobId = std::string()) {
          const auto stillAuthorized = [&] {
            return !g_stop.load() &&
                   !userCancellationReceived->load(std::memory_order_acquire) &&
                   context.hasCurrentWorkAuthority();
          };
          if (personComputation &&
              loadPersonComputation(options, stillAuthorized).manifest.canonicalJson !=
                personComputation->manifest.canonicalJson) {
            personWorkerSession->close();
            throw std::runtime_error("person computation artifacts changed after startup");
          }
          if (personComputation) uav::emitUavPersonTrace(personTraceScope, "worker-start");
          auto status = personComputation
            ? personWorkerSession->exchange(personSessionRequest(personContext, jobId),
                "SPEC219_PERSON_SESSION_DONE jobId=" + jobId + "\n",
                std::chrono::milliseconds(personComputation->profile.limits.at("workerTimeoutMs")),
                stillAuthorized)
            : uav::runUavTrackingWorkerProcess(arguments, std::chrono::milliseconds(30000),
                16 * 1024 * 1024, stillAuthorized);
          if (personComputation) uav::emitUavPersonTrace(personTraceScope, "worker-end");
          std::cout << status.stdoutText;
          std::cerr << status.stderrText;
          if (status.outcome != uav::UavTrackingWorkerOutcome::SUCCESS) {
            std::cerr << "TRACKING_WORKER_FAILED outcome=" << static_cast<int>(status.outcome)
                      << " exit=" << status.exitCode << " system_error=" << status.systemError
                      << std::endl;
          }
          return status;
        };
        ndn::Buffer result;
        if (useObservations) {
          std::lock_guard<std::mutex> transactionLock(g_tracking_transaction_mutex);
          std::ostringstream transactionIdentity;
          transactionIdentity << "uav-tracking-window-input-v1\n"
                              << options.runId << '\n' << mission << '\n' << window << '\n'
                              << observationEpoch << '\n' << modelDigest << '\n'
                              << workerImplementationDigest << '\n';
          if (personComputation) {
            transactionIdentity << personComputation->manifest.computationDigest << '\n';
          }
          for (size_t cameraIndex = 0; cameraIndex < 3; ++cameraIndex) {
            transactionIdentity << observationSourceIds[cameraIndex] << '\n'
                                << digests[cameraIndex] << '\n'
                                << observationNames[cameraIndex] << '\n'
                                << observationDigests[cameraIndex] << '\n';
            for (const auto& reference : observationReferences[cameraIndex]) {
              transactionIdentity << reference.category << '\n' << reference.exactName << '\n'
                                  << reference.contentSha256 << '\n'
                                  << reference.producerIdentity << '\n';
            }
          }
          const auto inputDigest = sha256(bufferFrom(transactionIdentity.str()));
          const auto missionKey = sha256(bufferFrom(mission)).substr(7);
          uav::UavTrackingWorkerTransaction transactions(workerRoot / "transactions" / missionKey,
            personComputation
              ? personComputation->profile.limits.at("maxRetainedTransactionWindows") : 0);
          const auto validateResult = [&] (const ndn::Buffer& bytes) {
            uav::UavTrackingResultExpectation expected;
            expected.runId = options.runId;
            expected.missionId = mission;
            expected.windowId = window;
            expected.sequence = index + 1;
            expected.sourceEpoch = observationEpoch;
            expected.sourceIds = {observationSourceIds[0], observationSourceIds[1],
                                  observationSourceIds[2]};
            expected.inputDigests = {digests[0], digests[1], digests[2]};
            expected.computeProvider = identity;
            if (personComputation) {
              expected.personTracking = uav::UavPersonTrackingResultExpectation{
                personComputation->profile, personComputation->manifest.computationDigest};
            }
            validateUavTrackingResultContext(resultName.toUri(), identity.toUri(),
              bufferText(bytes), expected);
          };
          const auto cached = transactions.loadCommittedResult(window, inputDigest);
          if (cached) {
            result = *cached;
            validateResult(result);
            if (personComputation) uav::emitUavPersonTrace(personTraceScope, "cached");
          }
          else {
            transactions.requireCapacity(window);
            std::optional<std::filesystem::path> stateInput;
            if (index > 0) {
              stateInput = transactions.committedStatePath(
                "window-" + std::to_string(index - 1));
              if (!stateInput) {
                failRole("previous Spec210 window has no committed tracker state");
                return;
              }
            }
            std::error_code ignored;
            std::filesystem::remove(workerOutput, ignored);
            std::filesystem::remove(candidateState, ignored);
            std::optional<uav::UavPersonTrackingEngine> personEngine;
            if (personComputation) {
              personEngine.emplace(personComputation->profile);
              if (stateInput) {
                personEngine->restoreState(bufferText(readBytes(stateInput->string())), personContext);
              }
              const auto input = personBackendInputJson(personContext, personEngine->backendStateJson());
              if (input.size() > personComputation->profile.limits.at("maxSnapshotBytes")) {
                throw std::invalid_argument("person backend input exceeds snapshot byte bound");
              }
              writeBytes(personInputPath, bufferFrom(input));
            }
            const auto workerArguments = personComputation
              ? std::vector<std::string>() : makeWorkerArguments(stateInput);
            // Record the actual dispatch boundary in every mode so source
            // failures can be distinguished from worker execution failures.
            std::cout << "SPEC210_TRACKING_WORKER_INVOKED window="
                      << window << std::endl;
            const auto workerStatus = runWorker(workerArguments, inputDigest);
            if (personComputation &&
                workerStatus.outcome == uav::UavTrackingWorkerOutcome::SUCCESS &&
                std::filesystem::is_regular_file(workerOutput)) {
              const auto backend = uav::parseUavPersonBackendWindow(
                bufferText(readBytes(workerOutput.string())), personContext, personComputation->profile);
              uav::emitUavPersonTrace(personTraceScope, "association-start");
              const auto decision = personEngine->processWindow(personContext, backend);
              uav::emitUavPersonTrace(personTraceScope, "association-end");
              result = bufferFrom(uav::makeUavPersonTrackingResultJson(
                decision, backend, personComputation->profile,
                {observationNames[0], observationNames[1], observationNames[2]},
                {observationDigests[0], observationDigests[1], observationDigests[2]}));
              validateResult(result);
              std::filesystem::create_directories(candidateState.parent_path());
              writeBytes(candidateState, bufferFrom(decision.candidateStateJson));
              writeBytes(workerOutput, result);
            }
            if (workerStatus.outcome != uav::UavTrackingWorkerOutcome::SUCCESS ||
                !std::filesystem::is_regular_file(workerOutput) ||
                !std::filesystem::is_regular_file(candidateState)) {
              failRole("CPU tracking worker failed to produce a candidate result/state pair");
              return;
            }
            result = readBytes(workerOutput.string());
            validateResult(result);
            const auto candidateStateBytes = readBytes(candidateState.string());
            if (options.fault == "user-cancel-before-person-commit" && index == 0) {
              // Pause only the test ordering, outside the authority mutex.
              // Cancellation must still revoke the actual context before commit.
              std::cout << "SPEC219_PERSON_PRECOMMIT_READY window=" << window << std::endl;
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              while (!userCancellationReceived->load(std::memory_order_acquire) &&
                     !g_stop.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
              if (!userCancellationReceived->load(std::memory_order_acquire)) {
                throw std::runtime_error("person precommit cancellation test gate was not cancelled");
              }
            }
            bool committed = false;
            {
              // The cancellation monitor takes the same mutex before
              // revoking authority. This orders the final check+rename against
              // the ACK, so no commit can occur after confirmed revocation.
              std::lock_guard<std::mutex> lock(workAuthorityCommitMutex);
              committed = transactions.commit(window, inputDigest, result,
                candidateStateBytes,
                [&context] { return context.hasCurrentWorkAuthority(); });
            }
            if (!committed) {
              if (personWorkerSession) personWorkerSession->close();
              if (options.fault == "user-cancel-during-active-reference-fetch" ||
                  options.fault == "user-cancel-after-source-switch-during-active-reference-fetch" ||
                  options.fault == "user-cancel-before-person-commit") {
                std::cout << "SPEC210_USER_CANCEL_CANDIDATE_DISCARDED window="
                          << window << std::endl;
              }
              failRole("collaboration authority was revoked before tracker state commit");
              return;
            }
            if (personComputation) uav::emitUavPersonTrace(personTraceScope, "committed");
          }
        }
        else {
          const auto workerArguments = makeWorkerArguments(workerState);
          const auto workerStatus = runWorker(workerArguments);
          if (workerStatus.outcome != uav::UavTrackingWorkerOutcome::SUCCESS ||
              !std::filesystem::is_regular_file(workerOutput)) {
            failRole("CPU tracking worker failed");
            return;
          }
          result = readBytes(workerOutput.string());
        }
        const auto resultArchive = workerRoot / "results" / (window + ".json");
        std::filesystem::create_directories(resultArchive.parent_path());
        writeBytes(resultArchive, result);
        if (!context.hasCurrentWorkAuthority()) {
          if (!finishUserCancellation()) {
            failRole("collaboration authority was revoked before tracking result publication");
          }
          return;
        }
        const auto publishedResultName = personComputation
          ? context.publishReusableLargeNamed("uav-tracking-result", resultName, result, 7000, 60000)
          : context.publishLargeNamed("uav-tracking-result", resultName, result, 7000, 60000);
        if (publishedResultName != resultName) {
          failRole("tracking result publication failed");
          return;
        }
        const bool dropResponse = options.fault == "response-loss" ||
          (options.fault == "late-response-loss" && index + 1 == options.windowCount);
        if (dropResponse) {
          std::cout << "SPEC191_TRACKING_RESPONSE_DROPPED window=" << window
                    << " fault=" << options.fault << std::endl;
          return;
        }
        // The terminal Response is the GS consumption boundary.  Carry the
        // same result JSON through the request-scoped large-response path so
        // the User actually fetches and authenticates the segmented result;
        // the producer-owned named publication above remains the durable
        // application product/reference.
        ndn::Buffer responseResult = result;
        if (options.fault == "stale-generation-result" && useObservations &&
            sourceGeneration != nullptr &&
            sourceGeneration != &sourceGenerations->generations.front() &&
            sourceGeneration->firstWindowIndex == index) {
          const auto current = std::find_if(
            sourceGenerations->generations.begin(), sourceGenerations->generations.end(),
            [index] (const auto& item) { return item.firstWindowIndex == index; });
          if (current == sourceGenerations->generations.begin() ||
              current == sourceGenerations->generations.end()) {
            throw std::runtime_error("cannot find the prior source generation for stale-result injection");
          }
          const auto& previous = *std::prev(current);
          std::string staleJson(bufferText(result));
          size_t replacedIds = 0;
          for (size_t sourceIndex = 0; sourceIndex < 3; ++sourceIndex) {
            const auto currentToken = jsonQuote(sourceGeneration->sourceIds[sourceIndex]);
            const auto previousToken = jsonQuote(previous.sourceIds[sourceIndex]);
            size_t position = 0;
            while ((position = staleJson.find(currentToken, position)) != std::string::npos) {
              staleJson.replace(position, currentToken.size(), previousToken);
              position += previousToken.size();
              ++replacedIds;
            }
          }
          size_t replacedEpochs = 0;
          const auto epochKey = jsonQuote("sourceEpoch");
          size_t position = 0;
          while ((position = staleJson.find(epochKey, position)) != std::string::npos) {
            const auto colon = staleJson.find(':', position + epochKey.size());
            if (colon == std::string::npos) break;
            size_t valueStart = colon + 1;
            while (valueStart < staleJson.size() &&
                   std::isspace(static_cast<unsigned char>(staleJson[valueStart]))) {
              ++valueStart;
            }
            size_t valueEnd = valueStart;
            while (valueEnd < staleJson.size() &&
                   std::isdigit(static_cast<unsigned char>(staleJson[valueEnd]))) {
              ++valueEnd;
            }
            if (staleJson.compare(valueStart, valueEnd - valueStart,
                                  std::to_string(sourceGeneration->sourceEpoch)) == 0) {
              const auto priorEpoch = std::to_string(previous.sourceEpoch);
              staleJson.replace(valueStart, valueEnd - valueStart, priorEpoch);
              position = valueStart + priorEpoch.size();
              ++replacedEpochs;
            }
            else {
              position = valueEnd;
            }
          }
          if (replacedIds == 0 || replacedEpochs == 0) {
            throw std::runtime_error("stale-generation result injection did not find current identity fields");
          }
          responseResult = bufferFrom(staleJson);
          std::cerr << "SPEC210_STALE_GENERATION_RESPONSE_INJECTED window=" << window
                    << " source_epoch=" << previous.sourceEpoch << std::endl;
        }
        const std::string resultJson(
          reinterpret_cast<const char*>(responseResult.data()), responseResult.size());
        const auto resultDigest = sha256(responseResult);
        const bool responseWinsTerminalRace =
          options.fault == "user-cancel-terminal-response-race";
        const bool cancellationWinsTerminalRace =
          options.fault == "user-cancel-terminal-response-cancel-wins-race";
        const auto waitForTerminalRaceRelease = [&] (const char* marker) {
          const char* runRoot = std::getenv("NDNSF_SPEC191_RUN_ROOT");
          if (runRoot == nullptr || *runRoot == '\0') {
            throw std::runtime_error(
              "terminal response race fault requires the MiniNDN run root");
          }
          const auto releasePath = std::filesystem::path(runRoot) /
            "private/spec210-terminal-response-race/release-terminal";
          std::cout << marker << " window=" << window << std::endl;
          const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(30);
          while (!std::filesystem::exists(releasePath) &&
                 std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          if (!std::filesystem::exists(releasePath)) {
            throw std::runtime_error(
              "terminal response race test gate was not released");
          }
        };
        // In the cancel-wins ordering, pause immediately before taking the
        // terminal mutex. The test releases this gate only after the cancel
        // monitor has revoked authority and is holding its ACK.
        if (cancellationWinsTerminalRace) {
          waitForTerminalRaceRelease("SPEC210_TERMINAL_RESPONSE_READY");
        }
        bool terminalResponsePublished = false;
        {
          // Serialize the terminal success boundary with cancellation revocation
          // just like the transaction manifest commit. Once cancellation is
          // ACKed, no later successful result can be published.
          std::lock_guard<std::mutex> lock(workAuthorityCommitMutex);
          if (responseWinsTerminalRace) {
            waitForTerminalRaceRelease("SPEC210_TERMINAL_RESPONSE_LOCKED");
          }
          if (context.hasCurrentWorkAuthority()) {
            context.publishFinalResponse(fieldsBuffer({
              {"accepted", "true"}, {"result_name", resultName.toUri()},
              {"result_digest", resultDigest}, {"result_json", resultJson},
              {"provider", identity.toUri()},
              {"input_uav1_digest", digests[0]}, {"input_uav2_digest", digests[1]},
              {"input_uav3_digest", digests[2]},
              {"motion_window_digest", motionDigest},
            }));
            terminalResponsePublished = true;
            if (responseWinsTerminalRace || cancellationWinsTerminalRace) {
              std::cout << "SPEC210_TERMINAL_RESPONSE_PUBLISHED window="
                        << window << std::endl;
            }
          }
        }
        if (!terminalResponsePublished) {
          if (personWorkerSession) personWorkerSession->close();
          if (!finishUserCancellation()) {
            failRole("collaboration authority was revoked before terminal result publication");
          }
          return;
        }
        if (options.fault == "user-cancel-during-active-reference-fetch" ||
            options.fault == "user-cancel-after-source-switch-during-active-reference-fetch") {
          std::cout << "SPEC210_USER_CANCEL_LATE_WORKER_COMPLETED window="
                    << window << std::endl;
        }
        return;
        }
        catch (const std::exception& error) {
          if (personWorkerSession) personWorkerSession->close();
          failRole(std::string("tracking observation verification failed: ") + error.what());
          return;
        }
      }
      if (context.role().rfind("Camera-", 0) != 0 ||
          (options.frameFile.empty() && options.frameDir.empty() &&
           options.observationStreamDir.empty())) {
        failRole("source frame input is not configured");
        return;
      }
      try {
        if (options.fault == "missing-view" && options.camera == "UAV2" && index == 1) {
          failRole("injected missing view");
          return;
        }
        const auto framePath = !options.observationStreamDir.empty()
          ? std::filesystem::path(options.observationStreamDir) /
              ("window-" + std::to_string(index) + ".jsonl")
          : options.frameDir.empty()
              ? std::filesystem::path(options.frameFile)
              : std::filesystem::path(options.frameDir) /
                  ("window-" + std::to_string(index) + ".jpg");
        std::cout << "SPEC191_TRACKING_SOURCE_WINDOW_START camera=" << options.camera
                  << " window=" << window << " frame=" << framePath << std::endl;
        uav::FrameEnvelope frame;
        frame.cameraId = options.camera;
      const size_t cameraIndex = static_cast<size_t>(options.camera.back() - '1');
      const std::string sourceId = sourceGeneration != nullptr
        ? sourceGeneration->sourceIds[cameraIndex] : options.observationSourceId;
      const uint64_t sourceEpoch = sourceGeneration != nullptr
        ? sourceGeneration->sourceEpoch : options.sourceEpoch;
      const auto expectedSourceIdentity = observationIdentity(options, sourceId, sourceEpoch);
      frame.sourceEpoch = sourceEpoch;
        if (options.fault == "stale-epoch") frame.sourceEpoch = 0;
        frame.sequence = index + 1;
        frame.encoding = "image/jpeg";
        if (!options.observationStreamDir.empty()) {
          auto aligned = readObservationStream(options, framePath, window,
                                               expectedSourceIdentity);
          if (uav::fieldOr(fields, "tracking_mode", "") == "indoor-person-reid") {
            auto trace = personTraceScope;
            trace["cameraId"] = options.camera;
            trace["sourceId"] = sourceId;
            trace["sourceEpoch"] = frame.sourceEpoch;
            trace["commonPtsUs"] = aligned.captureTimeUs;
            uav::emitUavPersonTrace(std::move(trace), "capture");
          }
          frame.sequence = aligned.frame.key.sequence;
          frame.ptsUs = aligned.captureTimeUs;
          frame.width = aligned.frame.width;
          frame.height = aligned.frame.height;
          frame.encoding = aligned.frame.encoding;
          frame.timeUncertaintyUs = aligned.timeUncertaintyUs;
          frame.observationMetadata = observationSnapshotJson(
            aligned, identity, frame.observationObjects, context,
            options.camera == "UAV3" && index == 1 ? options.fault : "");
          frame.bytes = std::move(aligned.frame.imageBytes);
        }
        else {
          const auto bytes = readBytes(framePath.string());
          frame.ptsUs = static_cast<uint64_t>(index) * 1000000;
          frame.width = 1;
          frame.height = 1;
          frame.motionMetadata = motionJson;
          frame.bytes.assign(bytes.begin(), bytes.end());
        }
        if (options.fault == "bad-content" && options.camera == "UAV3" && index == 1 &&
            !frame.bytes.empty()) {
          frame.bytes[0] ^= 0xff;
        }
        if (options.fault == "alter-committed-window" && options.camera == "UAV3" &&
            index == 0 && uav::fieldOr(fields, "tracking_mode", "") == "indoor-person-reid" &&
            uav::fieldOr(fields, "f4_altered_frame", "") == "1") {
          if (frame.bytes.empty()) throw std::runtime_error("altered replay needs a nonempty FRAME");
          frame.bytes[0] ^= 1;
          std::cout << "SPEC219_COMMITTED_WINDOW_ALTERED_FRAME camera=UAV3 window=window-0"
                    << std::endl;
        }
        if ((options.fault == "duplicate-reference-pose" ||
             options.fault == "overlimit-reference-pose") &&
            options.camera == "UAV3" && index == 1 &&
            !frame.observationMetadata.empty()) {
          const std::string key = "\"poseSampleRefs\"";
          const auto fieldOffset = frame.observationMetadata.find(key);
          const auto arrayStart = frame.observationMetadata.find('[', fieldOffset);
          const auto arrayEnd = frame.observationMetadata.find(']', arrayStart);
          const auto firstStart = frame.observationMetadata.find('{', arrayStart);
          const auto firstEnd = frame.observationMetadata.find('}', firstStart);
          if (fieldOffset == std::string::npos || arrayStart == std::string::npos ||
              arrayEnd == std::string::npos || firstStart == std::string::npos ||
              firstEnd == std::string::npos || firstEnd > arrayEnd) {
            throw std::runtime_error("cannot inject observation pose reference shape fault");
          }
          const auto firstReference = frame.observationMetadata.substr(
            firstStart, firstEnd - firstStart + 1);
          const size_t referenceCount = options.fault == "duplicate-reference-pose" ? 2 : 3;
          std::string replacement = "[";
          for (size_t referenceIndex = 0; referenceIndex < referenceCount; ++referenceIndex) {
            if (referenceIndex != 0) replacement += ',';
            replacement += firstReference;
          }
          replacement += ']';
          frame.observationMetadata.replace(arrayStart,
            arrayEnd - arrayStart + 1, replacement);
          std::cerr << "SPEC210_REFERENCE_SHAPE_INJECTED camera=UAV3 window=" << window
                    << " shape=" << options.fault << std::endl;
        }
        std::string badReferenceField;
        std::string badReferenceCategory;
        if (options.fault == "bad-reference-digest" ||
            options.fault == "bad-reference-digest-clock") {
          badReferenceField = "clockRef";
          badReferenceCategory = "CLOCK";
        }
        else if (options.fault == "bad-reference-digest-calibration") {
          badReferenceField = "calibrationRef";
          badReferenceCategory = "CALIBRATION";
        }
        else if (options.fault == "bad-reference-digest-ground") {
          badReferenceField = "groundRef";
          badReferenceCategory = "GROUND";
        }
        else if (options.fault == "bad-reference-digest-pose") {
          badReferenceField = "poseSampleRefs";
          badReferenceCategory = "POSE";
        }
        if (!badReferenceField.empty() && options.camera == "UAV3" && index == 1 &&
            !frame.observationMetadata.empty()) {
          const auto referenceOffset = frame.observationMetadata.find(
            "\"" + badReferenceField + "\"");
          const auto digestField = frame.observationMetadata.find(
            "\"contentSha256\":\"", referenceOffset);
          if (referenceOffset == std::string::npos || digestField == std::string::npos) {
            throw std::runtime_error("cannot inject observation dependency digest fault");
          }
          const auto digestValue =
            digestField + std::string("\"contentSha256\":\"sha256:").size();
          if (digestValue >= frame.observationMetadata.size() ||
              !((frame.observationMetadata[digestValue] >= '0' &&
                 frame.observationMetadata[digestValue] <= '9') ||
                (frame.observationMetadata[digestValue] >= 'a' &&
                 frame.observationMetadata[digestValue] <= 'f'))) {
            throw std::runtime_error("observation dependency digest is malformed");
          }
          frame.observationMetadata[digestValue] =
            frame.observationMetadata[digestValue] == '0' ? '1' : '0';
          std::cerr << "SPEC191_TRACKING_BAD_REFERENCE_DIGEST_INJECTED camera="
                    << options.camera << " window=" << window
                    << " category=" << badReferenceCategory << std::endl;
        }
        std::string referenceFaultKind;
        std::string referenceFaultCategory;
        std::string referenceFaultField;
        for (const auto* kind : {"content", "producer", "epoch", "revision"}) {
          const auto prefix = std::string("bad-reference-") + kind + "-";
          if (options.fault.rfind(prefix, 0) != 0) continue;
          referenceFaultKind = kind;
          const auto category = options.fault.substr(prefix.size());
          if (category == "clock") {
            referenceFaultCategory = "CLOCK";
            referenceFaultField = "clockRef";
          }
          else if (category == "calibration") {
            referenceFaultCategory = "CALIBRATION";
            referenceFaultField = "calibrationRef";
          }
          else if (category == "ground") {
            referenceFaultCategory = "GROUND";
            referenceFaultField = "groundRef";
          }
          else if (category == "pose") {
            referenceFaultCategory = "POSE";
            referenceFaultField = "poseSampleRefs";
          }
          else {
            throw std::runtime_error("unsupported observation reference fault category");
          }
        }
        const bool injectReferenceFault = !referenceFaultKind.empty() &&
          options.camera == "UAV3" && index == 1;
        if (injectReferenceFault && referenceFaultKind != "content" &&
            referenceFaultKind != "revision") {
          const auto referenceOffset = frame.observationMetadata.find(
            "\"" + referenceFaultField + "\"");
          const auto field = referenceFaultKind == "producer"
            ? "\"producerIdentity\":\"" : "\"exactName\":\"";
          const auto fieldOffset = frame.observationMetadata.find(field, referenceOffset);
          if (referenceOffset == std::string::npos || fieldOffset == std::string::npos) {
            throw std::runtime_error("cannot inject observation reference identity fault");
          }
          const auto valueStart = fieldOffset + std::string(field).size();
          const auto valueEnd = frame.observationMetadata.find('"', valueStart);
          if (valueEnd == std::string::npos) {
            throw std::runtime_error("observation reference identity is malformed");
          }
          auto replacement = frame.observationMetadata.substr(valueStart, valueEnd - valueStart);
          if (referenceFaultKind == "producer") {
            replacement = "/example/uav/drone/UNAUTHORIZED";
          }
          else {
            const auto epochPart = "/" + options.camera + "/" + std::to_string(sourceEpoch) + "/";
            const auto epochOffset = replacement.find(epochPart);
            if (epochOffset == std::string::npos) {
              throw std::runtime_error("observation reference epoch is not present");
            }
            replacement.replace(epochOffset, epochPart.size(),
              "/" + options.camera + "/" + std::to_string(sourceEpoch + 1) + "/");
          }
          frame.observationMetadata.replace(valueStart, valueEnd - valueStart, replacement);
        }
        const bool reusablePersonObjects =
          uav::fieldOr(fields, "tracking_mode", "") == "indoor-person-reid";
        uav::UavTrackingEvidenceStore store;
        std::string reason;
        const auto publishedIdentity = options.fault == "bad-identity" &&
            options.camera == "UAV3" && index == 1
          ? uav::droneIdentity("UAV1") : identity;
        if (publishedIdentity != identity) {
          std::cerr << "SPEC191_TRACKING_BAD_IDENTITY_INJECTED camera="
                    << options.camera << " window=" << window << std::endl;
        }
        auto staged = store.stage(publishedIdentity, mission, window, frame, &reason);
        if (!staged) {
          context.fail(reason.empty() ? "source frame staging failed" : reason);
          return;
        }
        if (injectReferenceFault) {
          if (referenceFaultKind == "content" && referenceFaultCategory == "POSE") {
            auto object = std::find_if(staged->observationObjects.begin(),
                                      staged->observationObjects.end(),
              [&] (const auto& candidate) { return candidate.category == referenceFaultCategory; });
            if (object == staged->observationObjects.end() || object->payload.empty()) {
              throw std::runtime_error("cannot inject observation dependency content fault");
            }
            object->payload.front() ^= 1;
            // The publication is internally consistent, but its source-signed
            // sidecar still binds the original bytes. Exercise received content.
            object->contentSha256 = sha256(ndn::Buffer(object->payload.begin(), object->payload.end()));
          }
          std::cout << "SPEC210_REFERENCE_OBJECT_INJECTED camera=UAV3 window=" << window
                    << " kind=" << referenceFaultKind
                    << " category=" << referenceFaultCategory << std::endl;
        }
        ndn::Name published;
        if (uav::fieldOr(fields, "tracking_mode", "") == "indoor-person-reid") {
          auto trace = personTraceScope;
          trace["cameraId"] = options.camera;
          trace["sourceId"] = sourceId;
          trace["sourceEpoch"] = frame.sourceEpoch;
          trace["commonPtsUs"] = frame.ptsUs;
          uav::emitUavPersonTrace(std::move(trace), "publication-start");
        }
        const size_t heldReferenceWindow = options.fault ==
            "user-cancel-after-source-switch-during-active-reference-fetch" ? 1 : 0;
        if (options.fault == "missing-reference-clock" && options.camera == "UAV3" &&
            index == 1) {
          auto incompletePublication = *staged;
          incompletePublication.observationObjects.erase(
            std::remove_if(incompletePublication.observationObjects.begin(),
                           incompletePublication.observationObjects.end(),
              [] (const auto& object) { return object.category == "CLOCK"; }),
            incompletePublication.observationObjects.end());
          std::cout << "SPEC210_REFERENCE_OMITTED camera=UAV3 window=" << window
                    << " category=CLOCK" << std::endl;
          published = store.publish(context, uav::TRACKING_INPUT_KEY_SCOPE,
                                    incompletePublication, 60000, reusablePersonObjects);
        }
        else if ((options.fault == "cancel-during-active-reference-fetch" ||
             options.fault == "user-cancel-during-active-reference-fetch" ||
             options.fault == "user-cancel-after-source-switch-during-active-reference-fetch" ||
             options.fault == "recover-active-reference-fetch") &&
            options.camera == "UAV3" && index == heldReferenceWindow) {
          std::vector<uav::ObservationDataObject> delayedPoses;
          std::copy_if(staged->observationObjects.begin(), staged->observationObjects.end(),
                       std::back_inserter(delayedPoses),
                       [] (const auto& object) { return object.category == "POSE"; });
          if (delayedPoses.empty()) {
            throw std::runtime_error("active-reference cancellation fault requires POSE objects");
          }
          auto partialPublication = *staged;
          partialPublication.observationObjects.erase(
            std::remove_if(partialPublication.observationObjects.begin(),
                           partialPublication.observationObjects.end(),
              [] (const auto& object) { return object.category == "POSE"; }),
            partialPublication.observationObjects.end());
          published = store.publish(context, uav::TRACKING_INPUT_KEY_SCOPE,
                                    partialPublication, 60000, reusablePersonObjects);
          std::cout << "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 window="
                    << window << " category=POSE" << std::endl;
          const auto holdDuration = options.fault == "user-cancel-during-active-reference-fetch" ||
              options.fault == "user-cancel-after-source-switch-during-active-reference-fetch"
            ? std::chrono::seconds(8) : std::chrono::seconds(5);
          std::this_thread::sleep_for(holdDuration);
          for (const auto& pose : delayedPoses) {
            const ndn::Name poseName(pose.exactName);
            const ndn::Buffer poseBytes(pose.payload.begin(), pose.payload.end());
            const auto publishedPose = reusablePersonObjects
              ? context.publishReusableLargeNamed(
                  "uav-tracking-observation", poseName, poseBytes, 1400, 60000)
              : context.publishLargeNamed(
                  "uav-tracking-observation", poseName, poseBytes, 1400, 60000);
            if (publishedPose != poseName) {
              throw std::runtime_error("delayed POSE publication name changed");
            }
          }
          if (options.fault == "recover-active-reference-fetch") {
            std::cout << "SPEC210_ACTIVE_REFERENCE_SOURCE_RECOVERED camera=UAV3 window="
                      << window << " category=POSE" << std::endl;
          }
        }
        else {
          if (options.fault == "cancel-during-active-fetch" && options.camera == "UAV3" &&
              index == 0) {
            std::cout << "SPEC210_ACTIVE_FETCH_SOURCE_HELD camera=UAV3 window="
                      << window << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
          }
          published = store.publish(context, uav::TRACKING_INPUT_KEY_SCOPE,
                                    *staged, 60000, reusablePersonObjects);
        }
        if (published != staged->reference.exactDataName) {
          context.fail("source publication name changed");
          return;
        }
        if (publishedFrameStore &&
            uav::fieldOr(fields, "tracking_mode", "") == "indoor-person-reid") {
          uav::UavPersonFrameReadBinding binding;
          binding.runId = options.runId;
          binding.missionId = mission;
          binding.windowId = window;
          binding.cameraId = options.camera;
          binding.sourceId = sourceId;
          binding.sourceEpoch = frame.sourceEpoch;
          binding.sequence = frame.sequence;
          binding.frameName = published.toUri();
          binding.inputDigest = sha256(staged->payload);
          publishedFrameStore->retainPublished(binding, staged->payload, context.requesterName());
          auto trace = personTraceScope;
          trace["cameraId"] = options.camera;
          trace["sourceId"] = sourceId;
          trace["sourceEpoch"] = frame.sourceEpoch;
          trace["commonPtsUs"] = frame.ptsUs;
          uav::emitUavPersonTrace(std::move(trace), "published");
        }
        std::cout << "SPEC191_TRACKING_SOURCE_PUBLISHED camera=" << options.camera
                  << " window=" << window << " name=" << published.toUri() << std::endl;
        context.completeRole();
      }
      catch (const std::exception& error) {
        context.fail(std::string("source publication failed: ") + error.what());
      }
    });
  provider.init();
  provider.fetchPermissionsFromController(ndn::Name(options.controller));

  std::cout << "SPEC191_TRACKING_NODE_READY role=" << options.role
            << " identity=" << identity.toUri()
            << " service=" << service.toUri()
            << " display=" << options.display << std::endl;
  while (!g_stop.load()) {
    face.processEvents(ndn::time::milliseconds(100));
    face.getIoContext().restart();
  }
  std::cout << "SPEC191_TRACKING_NODE_EXIT identity=" << identity.toUri() << std::endl;
  return 0;
}

} // namespace

int
main(int argc, char** argv)
{
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  try {
    const auto options = parseOptions(argc, argv);
    if (options.fault == "unauthorized-frame-cache") {
      std::signal(SIGUSR2, onCacheProbeSignal);
    }
    return run(options);
  }
  catch (const std::exception& error) {
    std::cerr << "UavTrackingNode error: " << error.what() << std::endl;
    return 2;
  }
}
