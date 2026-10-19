#include "UavTrackingCoordinator.hpp"

#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "../shared/UavSourceGeneration.hpp"
#include "../shared/UavTrackingResultVerifier.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/asio/post.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nsf = ndn_service_framework;
namespace uav = ndnsf::examples::uav;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_cancel_requested{false};

void
onSignal(int)
{
  g_cancel_requested.store(true);
}

void
onCancelSignal(int)
{
  g_cancel_requested.store(true);
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

void
requireOptionValue(int argc, char** argv, const std::string& name)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != name) continue;
    if (i + 1 >= argc || argv[i + 1][0] == '\0' ||
        (argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) {
      throw std::invalid_argument(name + " requires a value");
    }
    return;
  }
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
  return uav::canonicalSha256Digest(digest.toString());
}

std::string
readMotionWindow(const std::string& directory, const std::string& window)
{
  if (directory.empty()) return {};
  const auto path = std::filesystem::path(directory) / (window + ".json");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("motion window is not available: " + path.string());
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0 || size > 256 * 1024) {
    throw std::runtime_error("motion window is empty or exceeds 256 KiB");
  }
  input.seekg(0, std::ios::beg);
  std::string payload(static_cast<size_t>(size), '\0');
  input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
    throw std::runtime_error("short read from motion window: " + path.string());
  }
  if (!payload.empty() && payload.back() == '\n') payload.pop_back();
  return payload;
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
  const std::string motionDir = option(argc, argv, "--motion-dir");
  const std::string observationSchema = option(argc, argv, "--observation-schema");
  const std::string observationSourceEpoch = option(argc, argv, "--source-epoch", "0");
  const std::string observationSourceUav1 = option(argc, argv, "--source-uav1-id");
  const std::string observationSourceUav2 = option(argc, argv, "--source-uav2-id");
  const std::string observationSourceUav3 = option(argc, argv, "--source-uav3-id");
  const std::string sourceGenerationsPath = option(argc, argv, "--source-generations");
  requireOptionValue(argc, argv, "--calibration-provider");
  const std::string calibrationProviderText = option(argc, argv, "--calibration-provider");
  const ndn::Name calibrationProvider(calibrationProviderText);
  if (!calibrationProviderText.empty() &&
      (calibrationProvider.empty() || calibrationProviderText.front() != '/')) {
    throw std::invalid_argument("--calibration-provider must be an absolute NDN identity name");
  }
  uint64_t observationEpoch = 0;
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
  if (missionId.empty() || windowPrefix.empty() || windowCount == 0 || windowCount > 600 ||
      startDelayMs < 0 ||
      controllerReadyTimeoutMs <= 0 || controllerReadyRetryMs <= 0 ||
      requestTimeoutMs <= 1000) {
    throw std::invalid_argument(
      "mission/window and timing values must be valid; request timeout must exceed ACK timeout");
  }
  if (!observationSchema.empty() &&
      (observationSchema != "uav-observation-v2" || observationSourceEpoch == "0" ||
       observationSourceUav1.empty() || observationSourceUav2.empty() ||
       observationSourceUav3.empty())) {
    throw std::invalid_argument("observation request identity is incomplete");
  }
  if (!observationSchema.empty()) {
    observationEpoch = std::stoull(observationSourceEpoch);
    if (observationEpoch == 0 || std::to_string(observationEpoch) != observationSourceEpoch) {
      throw std::invalid_argument("observation source epoch must be a positive canonical integer");
    }
  }
  else if (!sourceGenerationsPath.empty()) {
    throw std::invalid_argument("source generation manifest requires the observation request schema");
  }
  const std::array<std::string, 3> initialSourceIds{
    observationSourceUav1, observationSourceUav2, observationSourceUav3};
  std::optional<uav::UavSourceGenerationSchedule> sourceGenerations;
  if (!observationSchema.empty()) {
    sourceGenerations = sourceGenerationsPath.empty()
      ? uav::makeUavSourceGenerationSchedule(observationEpoch, initialSourceIds, windowCount)
      : uav::loadUavSourceGenerationSchedule(sourceGenerationsPath, windowCount);
    const auto& initialGeneration = sourceGenerations->generations.front();
    if (initialGeneration.sourceEpoch != observationEpoch ||
        initialGeneration.sourceIds != initialSourceIds) {
      throw std::invalid_argument("source generation manifest initial identity does not match GroundStation options");
    }
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
  std::optional<ndn::Name> activeRequestId;
  std::optional<ndn::Name> activeComputeProvider;
  size_t activeWindowIndex = 0;
  bool cancellationInProgress = false;
  bool cancellationCompletionPending = false;
  std::optional<std::pair<bool, ndn::Buffer>> deferredCancellationResponse;
  std::shared_ptr<std::function<void(bool, const ndn::Buffer&)>>
    cancellationResponseHandler;
  std::thread cancellationAckThread;
  const auto cancelActiveCollaboration = [&] (const ndn::Name& requestId,
                                               size_t windowIndex) {
    if (cancellationInProgress) return;
    cancellationInProgress = true;
    bool cancelNoticeQueued = false;
    if (activeComputeProvider) {
      cancelNoticeQueued = user.publishCollaborationData(
        *activeComputeProvider, requestId, uav::TRACKING_INPUT_KEY_SCOPE,
        uav::TRACKING_CANCEL_TOPIC, fieldsBuffer({
          {"schema", uav::TRACKING_CANCEL_SCHEMA},
          {"request_id", requestId.toUri()},
          {"run_id", runId},
          {"mission_id", missionId},
          {"window_id", windowPrefix + "-" + std::to_string(windowIndex)},
        }));
    }
    std::cerr << "SPEC210_CANCEL_NOTICE_QUEUED window=" << windowIndex
              << " accepted=" << (cancelNoticeQueued ? "true" : "false") << std::endl;
    auto ackState = std::make_shared<std::atomic<int>>(0);
    if (cancelNoticeQueued && activeComputeProvider) {
      const ndn::Name expectedProvider = *activeComputeProvider;
      const auto expectedRunId = runId;
      const auto expectedMissionId = missionId;
      const auto expectedWindowId = windowPrefix + "-" + std::to_string(windowIndex);
      const auto ackTimeout = fault == "user-cancel-terminal-response-race" ||
        fault == "user-cancel-terminal-response-cancel-wins-race"
        ? std::chrono::seconds(20) : std::chrono::seconds(5);
      try {
        cancellationAckThread = std::thread(
          [&user, ackState, requestId, expectedProvider, expectedRunId,
           expectedMissionId, expectedWindowId, ackTimeout] {
            try {
              const auto deadline = std::chrono::steady_clock::now() +
                ackTimeout;
              size_t nextCount = 1;
              while (std::chrono::steady_clock::now() < deadline) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - std::chrono::steady_clock::now()).count();
                const auto waitMs = static_cast<int>(std::max<int64_t>(
                  1, std::min<int64_t>(100, remaining)));
                const auto records = user.waitForVerifiedCollaborationData(
                  requestId, uav::TRACKING_INPUT_KEY_SCOPE,
                  uav::TRACKING_CANCEL_ACK_TOPIC, nextCount, waitMs, false);
                for (const auto& record : records) {
                  if (record.requestId != requestId ||
                      record.keyScope != uav::TRACKING_INPUT_KEY_SCOPE ||
                      record.topic != uav::TRACKING_CANCEL_ACK_TOPIC ||
                      record.producer != expectedProvider ||
                      record.producerRole != "Tracking-Compute") {
                    continue;
                  }
                  const auto fields = uav::decodeFields(std::string(
                    reinterpret_cast<const char*>(record.payload.data()),
                    record.payload.size()));
                  if (uav::fieldOr(fields, "schema", "") ==
                        uav::TRACKING_CANCEL_ACK_SCHEMA &&
                      uav::fieldOr(fields, "request_id", "") == requestId.toUri() &&
                      uav::fieldOr(fields, "run_id", "") == expectedRunId &&
                      uav::fieldOr(fields, "mission_id", "") == expectedMissionId &&
                      uav::fieldOr(fields, "window_id", "") == expectedWindowId &&
                      uav::fieldOr(fields, "authority_revoked", "") == "true") {
                    ackState->store(1, std::memory_order_release);
                    return;
                  }
                }
                nextCount = records.size() + 1;
              }
              ackState->store(2, std::memory_order_release);
            }
            catch (...) {
              ackState->store(3, std::memory_order_release);
            }
          });
      }
      catch (const std::exception&) {
        ackState->store(3, std::memory_order_release);
      }
    }
    else {
      ackState->store(4, std::memory_order_release);
    }

    auto finishCancellation = std::make_shared<std::function<void()>>();
    auto pollCancellation = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weakPollCancellation =
      pollCancellation;
    *finishCancellation = [&, ackState, requestId, windowIndex] {
      if (cancellationAckThread.joinable()) cancellationAckThread.join();
      const int ackStatus = ackState->load(std::memory_order_acquire);
      if (ackStatus == 1) {
        std::cerr << "SPEC210_CANCEL_ACK_CONFIRMED window=" << windowIndex
                  << " provider=" << activeComputeProvider->toUri() << std::endl;
      }
      else {
        const char* reason = ackStatus == 2 ? "timeout" :
          (ackStatus == 3 ? "wait-failed" : "publication-unavailable");
        std::cerr << "SPEC210_CANCEL_ACK_UNCONFIRMED window=" << windowIndex
                  << " reason=" << reason << std::endl;
      }
      const bool cancelled = user.CancelCollaboration(requestId);
      auto finishOutcome = std::make_shared<std::function<void(bool)>>();
      *finishOutcome = [&, requestId, windowIndex] (bool cancelAccepted) {
        if (!cancelAccepted && deferredCancellationResponse && cancellationResponseHandler &&
            *cancellationResponseHandler) {
          auto response = std::move(*deferredCancellationResponse);
          deferredCancellationResponse.reset();
          cancellationCompletionPending = false;
          cancellationInProgress = false;
          std::cerr << "SPEC210_CANCEL_RACE_TERMINAL_RESPONSE_WON window="
                    << windowIndex << std::endl;
          (*cancellationResponseHandler)(response.first, response.second);
          return;
        }
        deferredCancellationResponse.reset();
        cancellationCompletionPending = false;
        if (activeRequestId && *activeRequestId == requestId) {
          activeRequestId.reset();
        }
        std::cerr << "SPEC191_TRACKING_CANCELLED window=" << windowIndex
                  << " accepted=" << (cancelAccepted ? "true" : "false") << std::endl;
        exitCode = 1;
        face.getIoContext().stop();
      };
      if (!cancelled && !deferredCancellationResponse) {
        // CancelCollaboration(false) means ServiceUser already removed the
        // pending call. Its response callback may still be queued on the
        // ServiceUser worker pool, so allow that callback to reach the Face
        // event loop before declaring cancellation terminal.
        cancellationCompletionPending = true;
        std::cerr << "SPEC210_CANCEL_RACE_WAITING_FOR_RESPONSE window="
                  << windowIndex << std::endl;
        const auto deadline = std::chrono::steady_clock::now() +
          std::chrono::seconds(5);
        auto pollResponse = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> weakPollResponse = pollResponse;
        *pollResponse = [&, weakPollResponse, finishOutcome, deadline, windowIndex] {
          if (deferredCancellationResponse) {
            (*finishOutcome)(false);
            return;
          }
          if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "SPEC210_CANCEL_RACE_RESPONSE_TIMEOUT window="
                      << windowIndex << std::endl;
            (*finishOutcome)(false);
            return;
          }
          if (const auto poll = weakPollResponse.lock()) {
            scheduler.schedule(ndn::time::milliseconds(25), [poll] { (*poll)(); });
          }
        };
        scheduler.schedule(ndn::time::milliseconds(25), [pollResponse] {
          (*pollResponse)();
        });
        return;
      }
      (*finishOutcome)(cancelled);
    };
    *pollCancellation = [&, ackState, weakPollCancellation, finishCancellation] {
      if (ackState->load(std::memory_order_acquire) == 0) {
        if (const auto poll = weakPollCancellation.lock()) {
          scheduler.schedule(ndn::time::milliseconds(25), [poll] { (*poll)(); });
        }
        return;
      }
      (*finishCancellation)();
    };
    scheduler.schedule(ndn::time::milliseconds(0), [pollCancellation] {
      (*pollCancellation)();
    });
  };
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
    uav::UavSourceGeneration windowGeneration;
    if (!observationSchema.empty()) {
      try {
        windowGeneration = uav::uavSourceGenerationForWindow(*sourceGenerations, windowIndex);
      }
      catch (const std::exception& error) {
        fail(error.what());
        return;
      }
      if (windowGeneration.firstWindowIndex == windowIndex && windowIndex != 0) {
        std::cout << "SPEC210_SOURCE_GENERATION_ACTIVATED first_window=" << windowIndex
                  << " source_epoch=" << windowGeneration.sourceEpoch << std::endl;
      }
    }
    const auto motionJson = readMotionWindow(motionDir, windowId);
    const auto motionDigest = motionJson.empty() ? std::string() : sha256Text(motionJson);
    auto assignment = fieldsBuffer({
      {"schema", "spec191-assignment-v1"},
      {"run_id", runId},
      {"mission_id", missionId},
      {"window_id", windowId},
    });
    if (!calibrationProvider.empty()) {
      auto assignmentText = std::string(
        reinterpret_cast<const char*>(assignment.data()), assignment.size());
      assignmentText += ";roleProvider.Calibration-Provider=" +
                        calibrationProvider.toUri();
      assignment = ndn::Buffer(
        reinterpret_cast<const uint8_t*>(assignmentText.data()), assignmentText.size());
    }
    uav::Fields requestFields{
      {"schema", "spec191-window-request-v1"},
      {"run_id", runId},
      {"mission_id", missionId},
      {"window_id", windowId},
    };
    if (!observationSchema.empty()) {
      requestFields.emplace("observation_schema", observationSchema);
      requestFields.emplace("source_epoch", std::to_string(windowGeneration.sourceEpoch));
      requestFields.emplace("source_uav1_id", windowGeneration.sourceIds[0]);
      requestFields.emplace("source_uav2_id", windowGeneration.sourceIds[1]);
      requestFields.emplace("source_uav3_id", windowGeneration.sourceIds[2]);
    }
    if (!motionJson.empty()) {
      requestFields.emplace("motion_schema", "spec191-motion-window-v1");
      requestFields.emplace("motion_window_digest", motionDigest);
      requestFields.emplace("motion_window_json", motionJson);
    }
    const auto request = fieldsBuffer(requestFields);
    auto makeCallbacks = [&, windowIndex, motionDigest, windowGeneration] {
      auto selectedComputeProvider = std::make_shared<ndn::Name>();
      auto responseHandler = std::make_shared<
        std::function<void(bool, const ndn::Buffer&)>>();
      cancellationResponseHandler = responseHandler;
      uav::UavTrackingCoordinatorCallbacks callbacks;
      callbacks.onPlanCommitted = [&, windowIndex, selectedComputeProvider] (
        const nsf::CollaborationAckClosure& closure,
        const std::vector<nsf::SelectedParticipant>& selected) {
        const auto requestId = closure.requestId;
        const auto selectedParticipants = selected;
        boost::asio::post(face.getIoContext(),
          [&, windowIndex, selectedComputeProvider, requestId, selectedParticipants] {
            activeRequestId = requestId;
            activeWindowIndex = windowIndex;
            std::cout << "SPEC191_TRACKING_PLAN_COMMITTED request="
                      << requestId.toUri()
                      << " participants=" << selectedParticipants.size() << std::endl;
            for (const auto& participant : selectedParticipants) {
              std::cout << "SPEC191_TRACKING_SELECTED role=" << participant.role
                        << " provider=" << participant.provider.toUri() << std::endl;
              if (participant.role == "Tracking-Compute") {
                *selectedComputeProvider = participant.provider;
                activeComputeProvider = participant.provider;
              }
            }
            if (fault == "cancel") {
              scheduler.schedule(ndn::time::milliseconds(0),
                [&, requestId, windowIndex] {
                  cancelActiveCollaboration(requestId, windowIndex);
                });
            }
          });
      };
      *responseHandler = [&, windowIndex, motionDigest, selectedComputeProvider,
                          windowGeneration] (
        bool responseStatus, const ndn::Buffer& responsePayload) {
        const auto fields = parseFields(responsePayload);
        const auto accepted = uav::fieldOr(fields, "accepted", "");
        const auto failureReason = uav::fieldOr(fields, "failure_reason", "");
        const auto resultJson = uav::fieldOr(fields, "result_json", "");
        const auto resultDigest = uav::fieldOr(fields, "result_digest", "");
        const bool resultDigestVerified = responseStatus && accepted == "true" &&
          !resultJson.empty() && !resultDigest.empty() &&
          sha256Text(resultJson) == resultDigest;
        bool resultContextVerified = observationSchema.empty();
        std::string resultContextError;
        if (resultDigestVerified && !observationSchema.empty()) {
          try {
            uav::UavTrackingResultExpectation expected;
            expected.runId = runId;
            expected.missionId = missionId;
            expected.windowId = windowPrefix + "-" + std::to_string(windowIndex);
            expected.sequence = windowIndex + 1;
            expected.sourceEpoch = windowGeneration.sourceEpoch;
            expected.sourceIds = windowGeneration.sourceIds;
            expected.inputDigests = {
              uav::fieldOr(fields, "input_uav1_digest", ""),
              uav::fieldOr(fields, "input_uav2_digest", ""),
              uav::fieldOr(fields, "input_uav3_digest", ""),
            };
            expected.computeProvider = *selectedComputeProvider;
            uav::validateUavTrackingResultContext(
              uav::fieldOr(fields, "result_name", ""),
              uav::fieldOr(fields, "provider", ""), resultJson, expected);
            resultContextVerified = true;
          }
          catch (const std::exception& error) {
            resultContextError = error.what();
          }
        }
        const bool resultVerified = resultDigestVerified && resultContextVerified;
        const bool motionVerified = motionDigest.empty() ||
          uav::fieldOr(fields, "motion_window_digest", "") == motionDigest;
        if (responseStatus && !motionVerified) {
          std::cerr << "SPEC191_TRACKING_MOTION_VERIFY_FAILED window=" << windowIndex
                    << " expected_digest=" << motionDigest
                    << " received_digest="
                    << uav::fieldOr(fields, "motion_window_digest", "") << std::endl;
        }
        if (responseStatus && accepted == "true" && !resultDigestVerified) {
          std::cerr << "SPEC191_TRACKING_RESULT_VERIFY_FAILED"
                    << " expected_digest=" << resultDigest
                    << " result_bytes=" << resultJson.size() << std::endl;
        }
        if (responseStatus && accepted == "true" && resultDigestVerified &&
            !resultContextVerified) {
          std::cerr << "SPEC191_TRACKING_RESULT_CONTEXT_VERIFY_FAILED window=" << windowIndex
                    << " reason=" << resultContextError << std::endl;
        }
        const bool success = responseStatus && resultVerified && motionVerified;
        std::cout << "SPEC191_TRACKING_WINDOW_RESULT window=" << windowIndex
                  << " status=" << (success ? "success" : "failure")
                  << " response_status=" << (responseStatus ? "success" : "failure")
                  << " result_name=" << uav::fieldOr(fields, "result_name", "")
                  << " result_digest=" << resultDigest
                  << " result_bytes=" << resultJson.size()
                  << " result_verified=" << (resultVerified ? "true" : "false")
                  << " failure_reason=" << failureReason
                  << std::endl;
        if (!success) {
          std::cerr << "SPEC191_TRACKING_RESULT status=failure window=" << windowIndex;
          if (!failureReason.empty()) {
            std::cerr << " reason=" << failureReason;
          }
          std::cerr << std::endl;
          exitCode = 1;
          activeRequestId.reset();
          face.getIoContext().stop();
          return;
        }
        if (windowIndex + 1 < windowCount) {
          activeRequestId.reset();
          scheduler.schedule(ndn::time::milliseconds(0), [beginWindow, windowIndex] {
            (*beginWindow)(windowIndex + 1);
          });
          return;
        }
        std::cout << "SPEC191_TRACKING_RESULT status=success window=" << windowIndex
                  << " windows=" << windowCount << std::endl;
        activeRequestId.reset();
        exitCode = 0;
        face.getIoContext().stop();
      };
      callbacks.onResponse = [&, windowIndex, responseHandler] (
        const nsf::ResponseMessage& response) {
        const bool responseStatus = response.getStatus();
        const auto responsePayload = response.getPayload();
        if (fault == "user-cancel-terminal-response-race" ||
            fault == "user-cancel-terminal-response-cancel-wins-race") {
          const char* runRoot = std::getenv("NDNSF_SPEC191_RUN_ROOT");
          if (runRoot != nullptr && *runRoot != '\0') {
            const auto releasePath = std::filesystem::path(runRoot) /
              "private/spec210-terminal-response-race/release-response-callback";
            std::cerr << "SPEC210_TERMINAL_RESPONSE_CALLBACK_HELD window="
                      << windowIndex << std::endl;
            const auto deadline = std::chrono::steady_clock::now() +
              std::chrono::seconds(20);
            while (!std::filesystem::exists(releasePath) &&
                   std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
          }
        }
        boost::asio::post(face.getIoContext(),
          [&, windowIndex, responseHandler, responseStatus, responsePayload] {
            if (cancellationInProgress) {
              // The Face event loop serializes this callback with cancellation
              // completion. Retain a response whose pending call was already
              // removed so CancelCollaboration(false) cannot discard it.
              deferredCancellationResponse.emplace(responseStatus, responsePayload);
              std::cerr << "SPEC210_TERMINAL_RESPONSE_DEFERRED window="
                        << windowIndex << std::endl;
              return;
            }
            (*responseHandler)(responseStatus, responsePayload);
          });
      };
      callbacks.onTimeout = [&, windowIndex] (const ndn::Name& requestId) {
        boost::asio::post(face.getIoContext(), [&, windowIndex, requestId] {
          if (cancellationInProgress) return;
          std::cerr << "SPEC191_TRACKING_TIMEOUT window=" << windowIndex
                    << " request=" << requestId.toUri() << std::endl;
          activeRequestId.reset();
          exitCode = 1;
          face.getIoContext().stop();
        });
      };
      callbacks.onFailure = [&, windowIndex] (const std::string& reason) {
        boost::asio::post(face.getIoContext(), [&, windowIndex, reason] {
          if (cancellationInProgress) return;
          std::cerr << "SPEC191_TRACKING_PLAN_FAILED window=" << windowIndex
                    << " reason=" << reason << std::endl;
          activeRequestId.reset();
          exitCode = 1;
          face.getIoContext().stop();
        });
      };
      return callbacks;
    };
    try {
      auto plan = uav::UavTrackingCoordinator::makePlan(
        sourceProviders, computeProvider, assignment, 1000, requestTimeoutMs,
        calibrationProvider);
      const auto requestId = uav::UavTrackingCoordinator{}.begin(
        user, uav::SERVICE_TRACKING_WINDOW, request, std::move(plan),
        makeCallbacks());
      if (requestId.empty()) {
        throw std::runtime_error("BeginCollaboration returned empty request id");
      }
      activeRequestId = requestId;
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
  std::signal(SIGUSR1, onCancelSignal);
  std::cout << "SPEC191_TRACKING_GS_READY identity=" << userIdentity.toUri()
            << " service=" << uav::SERVICE_TRACKING_WINDOW.toUri() << std::endl;
  while (!g_stop.load() && face.getIoContext().stopped() == false) {
    face.processEvents(ndn::time::milliseconds(100));
    face.getIoContext().restart();
    if (g_cancel_requested.exchange(false)) {
      if (activeRequestId) {
        cancelActiveCollaboration(*activeRequestId, activeWindowIndex);
      }
      else {
        g_stop.store(true);
        face.getIoContext().stop();
      }
    }
  }
  if (cancellationAckThread.joinable()) cancellationAckThread.join();
  if (g_cancel_requested.exchange(false) && activeRequestId) {
    cancelActiveCollaboration(*activeRequestId, activeWindowIndex);
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
