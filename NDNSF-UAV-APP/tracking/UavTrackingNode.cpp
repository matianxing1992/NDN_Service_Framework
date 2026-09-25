#include "../shared/UavNames.hpp"
#include "../shared/UavTrackingEvidenceStore.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace nsf = ndn_service_framework;
namespace uav = ndnsf::examples::uav;

namespace {

std::atomic<bool> g_stop{false};

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
  std::string workerScript = "NDNSF-UAV-APP/tracking/tracking_worker.py";
  std::string display = "headless";
  std::string runId = "spec191";
  std::string groupPrefix = "/example/uav";
  std::string controller = "/example/uav/controller";
  std::string motionDir;
  size_t windowCount = 1;
  std::string fault = "none";
};

Options
parseOptions(int argc, char** argv)
{
  Options result;
  result.role = option(argc, argv, "--role");
  result.camera = option(argc, argv, "--camera");
  result.videoSource = option(argc, argv, "--video-source");
  result.frameFile = option(argc, argv, "--frame-file");
  result.frameDir = option(argc, argv, "--frame-dir");
  result.model = option(argc, argv, "--model");
  result.workerScript = option(argc, argv, "--worker-script", result.workerScript);
  result.display = option(argc, argv, "--display", result.display);
  result.runId = option(argc, argv, "--run-id", result.runId);
  result.groupPrefix = option(argc, argv, "--group-prefix", result.groupPrefix);
  result.controller = option(argc, argv, "--controller-prefix", result.controller);
  result.motionDir = option(argc, argv, "--motion-dir");
  result.windowCount = static_cast<size_t>(std::stoul(option(argc, argv, "--window-count", "1")));
  result.fault = option(argc, argv, "--fault", "none");
  if ((result.role != "source" && result.role != "compute") ||
      (result.role == "source" && (result.camera.empty() || result.videoSource.empty() ||
                                    (result.frameFile.empty() && result.frameDir.empty()))) ||
      (result.role == "compute" && result.model.empty())) {
    throw std::invalid_argument(
      "usage: --role source --camera UAV1|UAV2|UAV3 --video-source PATH "
      "--model PATH, or --role compute --model PATH");
  }
  if (result.role == "source" &&
      (result.camera != "UAV1" && result.camera != "UAV2" && result.camera != "UAV3")) {
    throw std::invalid_argument("source camera must be UAV1, UAV2, or UAV3");
  }
  if (result.windowCount == 0 || result.windowCount > 60) {
    throw std::invalid_argument("window-count must be in the range 1..60");
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
  if (value > 59) throw std::invalid_argument("window index exceeds replay bound");
  return static_cast<size_t>(value);
}

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
sha256(const ndn::Buffer& bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty()) {
    digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return "sha256:" + digest.toString();
}

std::string
shellQuote(const std::string& value)
{
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') quoted += "'\\''";
    else quoted += character;
  }
  quoted += "'";
  return quoted;
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

int
run(const Options& options)
{
  const ndn::Name identity = options.role == "source"
    ? uav::droneIdentity(options.camera) : ndn::Name("/example/uav/compute");
  if (options.role == "source" && !std::filesystem::is_regular_file(options.videoSource)) {
    throw std::runtime_error("source video is not a regular file: " + options.videoSource);
  }
  if (!std::filesystem::is_regular_file(options.model)) {
    throw std::runtime_error("tracking model is not a regular file: " + options.model);
  }

  ndn::Face face;
  ndn::KeyChain keyChain;
  const auto providerCert = getOrCreateIdentity(keyChain, identity);
  const auto controllerCert = getOrCreateIdentity(keyChain, ndn::Name(options.controller));
  nsf::CertificatePublisher certificatePublisher(face, keyChain, providerCert.getName());
  nsf::ServiceProvider provider(face, ndn::Name(options.groupPrefix), providerCert,
                                controllerCert, "examples/trust-any.conf");
  provider.setPerformanceMode(true);
  provider.setUseTokens(true);

  const ndn::Name service = options.role == "source"
    ? uav::SERVICE_TRACKING_SOURCE : uav::SERVICE_TRACKING_COMPUTE;
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
      [options, identity] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                           const ndn::Name&, const nsf::RequestMessage& request) {
        const auto input = payloadText(request);
        if (input.empty()) return makeError("empty Spec191 request payload");
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
  provider.addCollaborationHandler(
    uav::SERVICE_TRACKING_WINDOW, roles,
    nsf::ServiceProvider::AckStrategyHandler(
      [] (const nsf::RequestMessage&) {
        nsf::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "spec191-collaboration-ready";
        return decision;
      }),
    [options, identity] (nsf::ServiceProvider::CollaborationContext& context,
                         const nsf::RequestMessage& request) {
      const auto fields = requestFields(request);
      const auto mission = uav::fieldOr(fields, "mission_id", options.runId);
      const auto window = uav::fieldOr(fields, "window_id", "window-0");
      const auto motionJson = uav::fieldOr(fields, "motion_window_json", "");
      const auto motionDigest = uav::fieldOr(fields, "motion_window_digest", "");
      if (!motionJson.empty() && (motionDigest.empty() ||
                                  sha256(bufferFrom(motionJson)) != motionDigest)) {
        context.fail("motion window digest verification failed");
        return;
      }
      const auto index = windowIndex(window);
      if (index >= options.windowCount) {
        context.fail("window index exceeds configured replay count");
        return;
      }
      if (mission.empty() || window.empty()) {
        context.fail("Spec191 collaboration requires mission_id and window_id");
        return;
      }
      if (context.role() == "Tracking-Compute") {
        if (options.fault == "worker-failure") {
          context.fail("injected Spec191 worker failure");
          return;
        }
        std::vector<std::string> digests;
        std::vector<ndn::Buffer> inputs;
        std::cout << "SPEC191_TRACKING_COMPUTE_WINDOW_START window=" << window << std::endl;
        for (const auto& camera : {std::string("UAV1"), std::string("UAV2"),
                                   std::string("UAV3")}) {
          const auto producer = uav::droneIdentity(camera);
          const auto dataName = uav::makeUavEvidenceName(
            producer, mission, window, camera + "-" + std::to_string(index + 1), 1);
          const auto payload = context.fetchLarge(dataName, "uav-tracking-input", 10000);
          if (!payload) {
            context.fail("tracking compute could not fetch " + dataName.toUri());
            return;
          }
          digests.push_back(sha256(*payload));
          inputs.push_back(*payload);
        }
        const char* privateRoot = std::getenv("NDNSF_SPEC191_PRIVATE_DIR");
        const auto workerRoot = (privateRoot != nullptr && *privateRoot != '\0'
          ? std::filesystem::path(privateRoot)
          : std::filesystem::temp_directory_path() / ("ndnsf-spec191-" + options.runId)) /
          "worker";
        const auto inputRoot = workerRoot / "verified-input";
        std::filesystem::create_directories(inputRoot);
        for (size_t index = 0; index < inputs.size(); ++index) {
          writeBytes(inputRoot / ("UAV" + std::to_string(index + 1) + ".jpg"), inputs[index]);
        }
        const auto workerOutput = workerRoot / "tracking-result.json";
        const auto workerState = workerRoot / "tracking-state.json";
        const auto motionPath = workerRoot / "motion-window.json";
        if (!motionJson.empty()) {
          writeBytes(motionPath, bufferFrom(motionJson));
        }
        const std::string workerCommand =
          "python3 " + shellQuote(options.workerScript) +
          " --model " + shellQuote(options.model) +
          " --input-dir " + shellQuote(inputRoot.string()) +
          " --output-json " + shellQuote(workerOutput.string()) +
          " --annotated-dir " + shellQuote((workerRoot / "annotated").string()) +
          " --state-file " + shellQuote(workerState.string()) +
          " --run-id " + shellQuote(options.runId) +
          " --mission-id " + shellQuote(mission) +
          " --window-id " + shellQuote(window) +
          " --sequence " + std::to_string(index + 1) +
          " --pts-us " + std::to_string(index * 1000000) +
          (motionJson.empty() ? std::string() :
           " --motion-json " + shellQuote(motionPath.string()) +
           " --motion-digest " + shellQuote(motionDigest) +
           (index == 0 ? std::string() : " --fail-on-motion-unknown"));
        const int workerStatus = std::system(workerCommand.c_str());
        if (workerStatus != 0 || !std::filesystem::is_regular_file(workerOutput)) {
          context.fail("CPU tracking worker failed");
          return;
        }
        const auto resultArchive = workerRoot / "results" / (window + ".json");
        std::filesystem::create_directories(resultArchive.parent_path());
        std::filesystem::copy_file(
          workerOutput, resultArchive, std::filesystem::copy_options::overwrite_existing);
        const auto result = readBytes(workerOutput.string());
        const auto resultName = uav::makeUavMultiViewResultName(
          identity, mission, window, index + 1, 1);
        const auto publishedResultName = context.publishLargeNamed(
          "uav-tracking-result", resultName, result, 7000, 60000);
        if (publishedResultName != resultName) {
          context.fail("tracking result publication failed");
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
        const std::string resultJson(
          reinterpret_cast<const char*>(result.data()), result.size());
        const auto resultDigest = sha256(result);
        context.publishFinalResponse(fieldsBuffer({
          {"accepted", "true"}, {"result_name", resultName.toUri()},
          {"result_digest", resultDigest}, {"result_json", resultJson},
          {"provider", identity.toUri()},
          {"input_uav1_digest", digests[0]}, {"input_uav2_digest", digests[1]},
          {"input_uav3_digest", digests[2]},
          {"motion_window_digest", motionDigest},
        }));
        return;
      }
      if (context.role().rfind("Camera-", 0) != 0 ||
          (options.frameFile.empty() && options.frameDir.empty())) {
        context.fail("source frame input is not configured");
        return;
      }
      try {
        if (options.fault == "missing-view" && options.camera == "UAV2" && index == 1) {
          context.fail("injected missing view");
          return;
        }
        const auto framePath = options.frameDir.empty()
          ? std::filesystem::path(options.frameFile)
          : std::filesystem::path(options.frameDir) /
              ("window-" + std::to_string(index) + ".jpg");
        const auto bytes = readBytes(framePath.string());
        std::cout << "SPEC191_TRACKING_SOURCE_WINDOW_START camera=" << options.camera
                  << " window=" << window << " frame=" << framePath << std::endl;
        uav::FrameEnvelope frame;
        frame.cameraId = options.camera;
        frame.sourceEpoch = 1;
        if (options.fault == "stale-epoch") frame.sourceEpoch = 0;
        frame.sequence = index + 1;
        frame.ptsUs = static_cast<uint64_t>(index) * 1000000;
        frame.width = 1;
        frame.height = 1;
        frame.encoding = "image/jpeg";
        frame.motionMetadata = motionJson;
        frame.bytes.assign(bytes.begin(), bytes.end());
        if (options.fault == "bad-content" && options.camera == "UAV3" && index == 1 &&
            !frame.bytes.empty()) {
          frame.bytes[0] ^= 0xff;
        }
        uav::UavTrackingEvidenceStore store;
        std::string reason;
        const auto publishedIdentity = options.fault == "bad-identity" &&
            options.camera == "UAV3" && index == 1
          ? uav::droneIdentity("UAV1") : identity;
        if (publishedIdentity != identity) {
          std::cerr << "SPEC191_TRACKING_BAD_IDENTITY_INJECTED camera="
                    << options.camera << " window=" << window << std::endl;
        }
        const auto staged = store.stage(publishedIdentity, mission, window, frame, &reason);
        if (!staged) {
          context.fail(reason.empty() ? "source frame staging failed" : reason);
          return;
        }
        const auto published = store.publish(context, "uav-tracking-input", *staged);
        if (published != staged->reference.exactDataName) {
          context.fail("source publication name changed");
          return;
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

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
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
  try {
    return run(parseOptions(argc, argv));
  }
  catch (const std::exception& error) {
    std::cerr << "UavTrackingNode error: " << error.what() << std::endl;
    return 2;
  }
}
