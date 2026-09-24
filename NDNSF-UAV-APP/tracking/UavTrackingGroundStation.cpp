#include "UavTrackingCoordinator.hpp"

#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
    if (argv[i] == name) {
      return argv[i + 1];
    }
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
fieldsBuffer(const uav::Fields& fields)
{
  const auto encoded = uav::encodeFields(fields);
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
}

uav::Fields
parseFields(const ndn::Buffer& payload)
{
  return uav::decodeFields(std::string(reinterpret_cast<const char*>(payload.data()),
                                        payload.size()));
}

std::string
sha256Text(const std::string& value)
{
  ndn::util::Sha256 digest;
  digest << value;
  return "sha256:" + digest.toString();
}

int
run(int argc, char** argv)
{
  const std::string runId = option(argc, argv, "--run-id", "spec191");
  const std::string missionId = option(argc, argv, "--mission-id", runId);
  const std::string windowPrefix = option(argc, argv, "--window-prefix", "window");
  const size_t windowCount = static_cast<size_t>(std::stoul(
    option(argc, argv, "--window-count", "1")));
  const std::string fault = option(argc, argv, "--fault", "none");
  const ndn::Name groupPrefix(option(argc, argv, "--group-prefix", "/example/uav"));
  const ndn::Name controllerPrefix(
    option(argc, argv, "--controller-prefix", "/example/uav/controller"));
  const std::string trustSchema = option(argc, argv, "--trust-schema",
                                         "examples/trust-any.conf");
  const ndn::Name userIdentity(
    option(argc, argv, "--user-identity", "/example/uav/gs"));
  const int startDelayMs = std::stoi(option(argc, argv, "--start-delay-ms", "2000"));
  const int controllerReadyTimeoutMs = std::stoi(
    option(argc, argv, "--controller-ready-timeout-ms", "30000"));
  const int controllerReadyRetryMs = std::stoi(
    option(argc, argv, "--controller-ready-retry-ms", "250"));
  const int requestTimeoutMs = std::stoi(
    option(argc, argv, "--request-timeout-ms", "60000"));
  if (missionId.empty() || windowPrefix.empty() || windowCount == 0 || windowCount > 60 ||
      startDelayMs < 0 ||
      controllerReadyTimeoutMs <= 0 || controllerReadyRetryMs <= 0 ||
      requestTimeoutMs <= 1000) {
    throw std::invalid_argument(
      "mission/window and timing values must be valid; request timeout must exceed ACK timeout");
  }

  ndn::Face face;
  ndn::KeyChain keyChain;
  const auto userCert = getOrCreateIdentity(keyChain, userIdentity);
  const auto controllerCert = getOrCreateIdentity(keyChain, controllerPrefix);
  keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(userIdentity));
  ndn_service_framework::CertificatePublisher certPublisher(face, keyChain,
                                                              userCert.getName());
  nsf::ServiceUser user(face, groupPrefix, userCert, controllerCert, trustSchema);
  user.init();
  user.setUseTokens(true);
  nsf::ServiceUser::AdaptiveAdmissionOptions admission;
  admission.enabled = false;
  user.setAdaptiveAdmissionControl(admission);
  user.fetchPermissionsFromController(controllerPrefix);

  const std::vector<ndn::Name> sourceProviders{
    uav::droneIdentity("UAV1"),
    uav::droneIdentity("UAV2"),
    uav::droneIdentity("UAV3"),
  };
  const ndn::Name computeProvider("/example/uav/compute");
  int exitCode = 0;
  ndn::Scheduler scheduler(face.getIoContext());
  const auto bootstrapStarted = std::chrono::steady_clock::now();
  auto beginWindow = std::make_shared<std::function<void(size_t)>>();
  *beginWindow = [&] (size_t windowIndex) {
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - bootstrapStarted).count();
    const auto fail = [&] (const std::string& reason) {
      std::cerr << "SPEC191_TRACKING_PLAN_FAILED reason=" << reason << std::endl;
      exitCode = 1;
      face.getIoContext().stop();
    };
    if (!user.getControllerVersion(uav::SERVICE_TRACKING_WINDOW)) {
      if (elapsedMs >= controllerReadyTimeoutMs) {
        fail("ControllerVersion readiness timeout");
        return;
      }
      std::cout << "SPEC191_TRACKING_WAIT_CONTROLLER_VERSION elapsed_ms="
                << elapsedMs << " retry_ms=" << controllerReadyRetryMs << std::endl;
      scheduler.schedule(ndn::time::milliseconds(controllerReadyRetryMs), [beginWindow, windowIndex] {
        (*beginWindow)(windowIndex);
      });
      return;
    }
    const std::string windowId = windowPrefix + "-" + std::to_string(windowIndex);
    const auto assignment = fieldsBuffer({
      {"schema", "spec191-assignment-v1"},
      {"run_id", runId},
      {"mission_id", missionId},
      {"window_id", windowId},
    });
    const auto request = fieldsBuffer({
      {"schema", "spec191-window-request-v1"},
      {"run_id", runId},
      {"mission_id", missionId},
      {"window_id", windowId},
    });
    auto makeCallbacks = [&, windowIndex] {
      uav::UavTrackingCoordinatorCallbacks callbacks;
      callbacks.onPlanCommitted = [&, windowIndex] (
        const nsf::CollaborationAckClosure& closure,
        const std::vector<nsf::SelectedParticipant>& selected) {
        std::cout << "SPEC191_TRACKING_PLAN_COMMITTED request="
                  << closure.requestId.toUri()
                  << " participants=" << selected.size() << std::endl;
        for (const auto& participant : selected) {
          std::cout << "SPEC191_TRACKING_SELECTED role=" << participant.role
                    << " provider=" << participant.provider.toUri() << std::endl;
        }
        if (fault == "cancel") {
          std::cerr << "SPEC191_TRACKING_CANCELLED window=" << windowIndex << std::endl;
          exitCode = 1;
          face.getIoContext().stop();
        }
      };
      callbacks.onResponse = [&, windowIndex] (const nsf::ResponseMessage& response) {
        const auto fields = parseFields(response.getPayload());
        const auto resultJson = uav::fieldOr(fields, "result_json", "");
        const auto resultDigest = uav::fieldOr(fields, "result_digest", "");
        const bool resultVerified = response.getStatus() &&
          !resultJson.empty() && !resultDigest.empty() &&
          sha256Text(resultJson) == resultDigest;
        if (response.getStatus() && !resultVerified) {
          std::cerr << "SPEC191_TRACKING_RESULT_VERIFY_FAILED"
                    << " expected_digest=" << resultDigest
                    << " result_bytes=" << resultJson.size() << std::endl;
        }
        const bool success = response.getStatus() && resultVerified;
        std::cout << "SPEC191_TRACKING_WINDOW_RESULT window=" << windowIndex
                  << " status=" << (success ? "success" : "failure")
                  << " result_name=" << uav::fieldOr(fields, "result_name", "")
                  << " result_digest=" << resultDigest
                  << " result_bytes=" << resultJson.size()
                  << " result_verified=" << (resultVerified ? "true" : "false")
                  << std::endl;
        if (!success) {
          std::cerr << "SPEC191_TRACKING_RESULT status=failure window=" << windowIndex << std::endl;
          exitCode = 1;
          face.getIoContext().stop();
          return;
        }
        if (windowIndex + 1 < windowCount) {
          scheduler.schedule(ndn::time::milliseconds(0), [beginWindow, windowIndex] {
            (*beginWindow)(windowIndex + 1);
          });
          return;
        }
        std::cout << "SPEC191_TRACKING_RESULT status=success window=" << windowIndex
                  << " windows=" << windowCount << std::endl;
        exitCode = 0;
        face.getIoContext().stop();
      };
      callbacks.onTimeout = [&, windowIndex] (const ndn::Name& requestId) {
        std::cerr << "SPEC191_TRACKING_TIMEOUT window=" << windowIndex
                  << " request=" << requestId.toUri() << std::endl;
        exitCode = 1;
        face.getIoContext().stop();
      };
      callbacks.onFailure = [&, windowIndex] (const std::string& reason) {
        std::cerr << "SPEC191_TRACKING_PLAN_FAILED window=" << windowIndex
                  << " reason=" << reason << std::endl;
        exitCode = 1;
        face.getIoContext().stop();
      };
      return callbacks;
    };
    try {
      auto plan = uav::UavTrackingCoordinator::makePlan(
        sourceProviders, computeProvider, assignment, 1000, requestTimeoutMs);
      const auto requestId = uav::UavTrackingCoordinator{}.begin(
        user, uav::SERVICE_TRACKING_WINDOW, request, std::move(plan),
        makeCallbacks());
      if (requestId.empty()) {
        throw std::runtime_error("BeginCollaboration returned empty request id");
      }
      std::cout << "SPEC191_TRACKING_GS_REQUEST request=" << requestId.toUri()
                << " mission=" << missionId << " window=" << windowId << std::endl;
    }
    catch (const std::exception& error) {
      const std::string reason = error.what();
      if (reason.find("ControllerVersion is not ready") != std::string::npos &&
          elapsedMs < controllerReadyTimeoutMs) {
        std::cout << "SPEC191_TRACKING_WAIT_CONTROLLER_VERSION elapsed_ms="
                  << elapsedMs << " retry_ms=" << controllerReadyRetryMs << std::endl;
        scheduler.schedule(ndn::time::milliseconds(controllerReadyRetryMs), [beginWindow, windowIndex] {
          (*beginWindow)(windowIndex);
        });
        return;
      }
      fail(reason);
    }
  };
  scheduler.schedule(ndn::time::milliseconds(startDelayMs), [beginWindow] { (*beginWindow)(0); });
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::cout << "SPEC191_TRACKING_GS_READY identity=" << userIdentity.toUri()
            << " service=" << uav::SERVICE_TRACKING_WINDOW.toUri() << std::endl;
  while (!g_stop.load() && face.getIoContext().stopped() == false) {
    face.processEvents(ndn::time::milliseconds(100));
    face.getIoContext().restart();
  }
  std::cout << "SPEC191_TRACKING_GS_EXIT status=" << exitCode << std::endl;
  return exitCode;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    return run(argc, argv);
  }
  catch (const std::exception& error) {
    std::cerr << "UavTrackingGroundStation error: " << error.what() << std::endl;
    return 2;
  }
}
