#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace ndnsf::di::tests {
namespace {

using namespace ndn_service_framework;

using namespace std::chrono_literals;

NativeExecutionPlan
makePlan()
{
  NativeExecutionPlan plan;
  plan.serviceName = "/LLM/Qwen";
  plan.modelName = "Qwen/Qwen3-0.6B";
  plan.roles = {"stage0", "stage1", "stage2"};
  plan.dependencies.emplace_back(
    std::vector<std::string>{"stage0"}, std::vector<std::string>{"stage1"},
    "epoch-key", "/activation", "/NDNSF-DI/{sessionId}/{producerRole}/{role}/{sequence}",
    2, 16, std::vector<std::string>{"hidden"});
  plan.dependencies.emplace_back(
    std::vector<std::string>{"stage1"}, std::vector<std::string>{"stage2"},
    "epoch-key", "/activation", "/NDNSF-DI/{sessionId}/{producerRole}/{role}/{sequence}",
    1, 8, std::vector<std::string>{"hidden"});
  return plan;
}

class FixtureRequest
{
public:
  void setPayload(std::string value) { m_payload = std::move(value); }
  const std::string& getPayload() const { return m_payload; }
  bool SerializeToString(std::string* output) const
  {
    *output = m_payload;
    return true;
  }
  bool ParseFromArray(const void* data, size_t size)
  {
    m_payload.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_payload;
};

class FixtureResponse
{
public:
  void setLabel(std::string value) { m_label = std::move(value); }
  const std::string& getLabel() const { return m_label; }
  bool SerializeToString(std::string* output) const
  {
    *output = m_label;
    return true;
  }
  bool ParseFromArray(const void* data, size_t size)
  {
    m_label.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_label;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec170NdnsfDiCoreFlow)

BOOST_AUTO_TEST_CASE(AttemptPlanAndDependencyNamesRemainBound)
{
  ExecutionAttemptAuthority authority;
  const ExecutionAttemptKey attempt{"/request-1", 1};
  BOOST_CHECK_EQUAL(authority.admit(attempt), ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK(authority.isAuthoritative(attempt));
  BOOST_CHECK_THROW(authority.admit({"/request-1", 0}), std::invalid_argument);

  const auto plan = makePlan();
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"stage0", "/provider/p0"},
                               {"stage1", "/provider/p1"},
                               {"stage2", "/provider/p2"}};
  const auto stage1 = roleSpecFor(plan, "stage1", attempt, assignment);
  BOOST_REQUIRE_EQUAL(stage1.inputs.size(), 1);
  BOOST_REQUIRE_EQUAL(stage1.outputs.size(), 1);
  BOOST_CHECK_EQUAL(stage1.inputs.front().requestId, "/request-1");
  BOOST_CHECK_EQUAL(stage1.inputs.front().attemptEpoch, 1);
  BOOST_CHECK_EQUAL(stage1.inputs.front().expectedSegments, 2);
  BOOST_CHECK_EQUAL(stage1.outputs.front().expectedSegments, 1);
  BOOST_CHECK(stage1.inputs.front().plannedDataName.find("/attempt/1/") !=
              std::string::npos);
  BOOST_REQUIRE_EQUAL(plannedSegmentNamesForEdge(stage1.inputs.front()).size(), 2);

  BOOST_CHECK(authority.cancel(attempt));
  BOOST_CHECK_EQUAL(authority.admit(attempt), ExecutionAttemptAdmission::Cancelled);
  const ExecutionAttemptKey retry{"/request-1", 2};
  BOOST_CHECK_EQUAL(authority.admit(retry), ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK_EQUAL(authority.admit(attempt), ExecutionAttemptAdmission::Stale);
  BOOST_CHECK(authority.complete(retry));
  BOOST_CHECK_EQUAL(authority.admit(retry), ExecutionAttemptAdmission::DuplicateTerminal);
}

BOOST_AUTO_TEST_CASE(AsyncDataflowRunsThreeStagePipelineAndRejectsMissingOutput)
{
  const DependencyEdge input{"input", "", "stage0", "/input", 1, 4};
  const DependencyEdge mid{"mid", "stage0", "stage1", "/mid", 1, 4};
  const DependencyEdge out{"out", "stage1", "stage2", "/out", 1, 4};
  const DependencyEdge result{"result", "stage2", "", "/result", 1, 4};
  const std::vector<RoleSpec> roles{
    RoleSpec{"stage0", {input}, {mid}},
    RoleSpec{"stage1", {mid}, {out}},
    RoleSpec{"stage2", {out}, {result}},
  };

  AsyncDataflowRuntime runtime(2);
  const auto resultData = runtime.run(
    "session-1", roles, {{"input", TensorBundle{"input", {1, 2, 3, 4}, 1, 4}}},
    [] (const RoleExecutionContext& context) {
      if (context.role == "stage0") {
        return std::map<std::string, TensorBundle>{
          {"mid", TensorBundle{"mid", {5, 6, 7, 8}, 1, 4}}};
      }
      if (context.role == "stage1") {
        return std::map<std::string, TensorBundle>{
          {"out", TensorBundle{"out", {9, 10, 11, 12}, 1, 4}}};
      }
      return std::map<std::string, TensorBundle>{
        {"result", TensorBundle{"result", {13, 14, 15, 16}, 1, 4}}};
    });
  BOOST_REQUIRE_EQUAL(resultData.roleTimings.size(), 3);
  BOOST_REQUIRE(resultData.outputsByScope.count("result") != 0);
  BOOST_CHECK_EQUAL(resultData.outputsByScope.at("result").payload.back(), 16);

  BOOST_CHECK_THROW(
    runtime.run("session-failure", roles,
                {{"input", TensorBundle{"input", {1}, 1, 1}}},
                [] (const RoleExecutionContext& context) {
                  if (context.role == "stage1") {
                    throw std::runtime_error("injected provider failure");
                  }
                  return std::map<std::string, TensorBundle>{
                    {context.role == "stage0" ? "mid" : "result",
                     TensorBundle{"failure", {1}, 1, 1}}};
                }),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DependencyWaitCoversCompletionCancellationAndDeadline)
{
  DependencyWaitScheduler scheduler(1, 4);
  std::promise<DependencyWaitResult> completedPromise;
  auto completedFuture = completedPromise.get_future();
  BOOST_CHECK_EQUAL(
    scheduler.submit("complete", std::chrono::steady_clock::now() + 1s,
                     [] (const DependencyWaitControl&) {
                       return DependencyWaitStatus::Completed;
                     },
                     [&] (const DependencyWaitResult& result) {
                       completedPromise.set_value(result);
                     }),
    DependencyWaitSubmitResult::Accepted);
  BOOST_REQUIRE(scheduler.waitForIdle(1s));
  BOOST_CHECK_EQUAL(completedFuture.get().status, DependencyWaitStatus::Completed);

  std::promise<DependencyWaitResult> cancelledPromise;
  auto cancelledFuture = cancelledPromise.get_future();
  BOOST_CHECK_EQUAL(
    scheduler.submit("cancel", std::chrono::steady_clock::now() + 1s,
                     [] (const DependencyWaitControl& control) {
                       while (!control.isCancelled()) {
                         std::this_thread::sleep_for(1ms);
                       }
                       return DependencyWaitStatus::Cancelled;
                     },
                     [&] (const DependencyWaitResult& result) {
                       cancelledPromise.set_value(result);
                     }),
    DependencyWaitSubmitResult::Accepted);
  std::this_thread::sleep_for(5ms);
  BOOST_CHECK(scheduler.cancel("cancel"));
  BOOST_REQUIRE(scheduler.waitForIdle(1s));
  BOOST_CHECK_EQUAL(cancelledFuture.get().status, DependencyWaitStatus::Cancelled);

  std::promise<DependencyWaitResult> deadlinePromise;
  auto deadlineFuture = deadlinePromise.get_future();
  BOOST_CHECK_EQUAL(
    scheduler.submit("deadline", std::chrono::steady_clock::now() + 5ms,
                     [] (const DependencyWaitControl& control) {
                       while (!control.deadlineExpired()) {
                         std::this_thread::sleep_for(1ms);
                       }
                       return DependencyWaitStatus::DeadlineExpired;
                     },
                     [&] (const DependencyWaitResult& result) {
                       deadlinePromise.set_value(result);
                     }),
    DependencyWaitSubmitResult::Accepted);
  BOOST_REQUIRE(scheduler.waitForIdle(1s));
  BOOST_CHECK_EQUAL(deadlineFuture.get().status, DependencyWaitStatus::DeadlineExpired);
}

BOOST_AUTO_TEST_CASE(ExecutionEvidenceRoundTripsAndRejectsSecrets)
{
  ExecutionEvidence evidence;
  evidence.providerName = "/provider/p0";
  evidence.providerBootId = "boot-1";
  evidence.evidenceEpoch = 1;
  evidence.runnerKind = RunnerKind::WiringOnly;
  evidence.realCompute = false;
  evidence.deviceKind = "cpu";
  evidence.runtimeVersion = "test-runtime";
  evidence.modelDigest = "sha256:" + std::string(64, 'a');
  evidence.planDigest = "sha256:" + std::string(64, 'b');
  evidence.artifactDigests = {{"stage0", "sha256:" + std::string(64, 'c')}};
  evidence.roles = {"stage0"};
  evidence.loadCompleted = true;
  evidence.warmupCompleted = true;
  evidence.createdAtMs = 1;
  const auto json = executionEvidenceToJson(evidence);
  const auto restored = executionEvidenceFromJson(json);
  BOOST_CHECK_EQUAL(restored.providerName, evidence.providerName);
  BOOST_CHECK_EQUAL(toString(restored.runnerKind), std::string("wiring-only"));
  BOOST_CHECK_EQUAL(restored.artifactDigests.at("stage0"),
                    evidence.artifactDigests.at("stage0"));
  BOOST_CHECK_THROW(executionEvidenceFromJson(json.substr(0, json.size() - 1) +
                                               ",\"token\":\"secret\"}"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentSeparatesBootstrapFromRequests)
{
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment;
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::New);
  BOOST_CHECK_THROW(environment.beginRequest("before-bootstrap"), std::logic_error);

  environment.bootstrap();
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::Ready);
  BOOST_REQUIRE(!environment.snapshot().digest.empty());
  BOOST_CHECK(!environment.user().getAllowedServices().empty());
  BOOST_CHECK(environment.provider().getCurrentPolicyEpoch() > 0);
  BOOST_CHECK_THROW(environment.bootstrap(), std::logic_error);

  auto scope = environment.beginRequest("request-ready-1");
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::RequestActive);
  BOOST_CHECK_EQUAL(scope.snapshotDigest, environment.snapshot().digest);
  BOOST_CHECK_THROW(environment.beginRequest("request-ready-2"), std::logic_error);
  BOOST_CHECK_THROW(environment.resetRequest(scope), std::logic_error);

  environment.markRequestPublished(scope);
  environment.updateRequestResidue(
      scope, ndn_service_framework::test::RequestResidue{1, 0, 0, 0, 0});
  BOOST_CHECK_THROW(environment.resetRequest(scope), std::logic_error);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::Ready);
  BOOST_CHECK(!scope.active);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentAppliesDeterministicPacketFaults)
{
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment;
  environment.bootstrap();

  auto publishData = [&] (const char* suffix) {
    ndn::Data data(ndn::Name("/ndnsf/spec170/fault").append(suffix));
    const std::string payload = suffix;
    data.setContent(payload);
    environment.keyChain().sign(data, ndn::security::signingWithSha256());
    environment.userFace().put(data);
    environment.userFace().processEvents(ndn::time::milliseconds(5));
    environment.providerFace().processEvents(ndn::time::milliseconds(5));
  };

  auto dropped = environment.beginRequest(
      "fault-drop", ndn_service_framework::test::FaultProfile{true, false, false});
  environment.markRequestPublished(dropped);
  publishData("drop");
  BOOST_CHECK_GE(environment.bridgeStats().droppedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 0);
  BOOST_CHECK_EQUAL(environment.bridgeStats().firstDroppedName,
                    "/ndnsf/spec170/fault/drop");
  environment.resetRequest(dropped);

  auto duplicated = environment.beginRequest(
      "fault-duplicate", ndn_service_framework::test::FaultProfile{false, true, false});
  environment.markRequestPublished(duplicated);
  publishData("duplicate");
  BOOST_CHECK_EQUAL(environment.bridgeStats().duplicatedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 2);
  environment.resetRequest(duplicated);

  auto reordered = environment.beginRequest(
      "fault-reorder", ndn_service_framework::test::FaultProfile{false, false, true});
  environment.markRequestPublished(reordered);
  publishData("first");
  BOOST_CHECK_EQUAL(environment.bridgeStats().reorderedPackets, 0);
  BOOST_CHECK_EQUAL(environment.bridgeStats().firstPendingName,
                    "/ndnsf/spec170/fault/first");
  publishData("second");
  BOOST_CHECK_EQUAL(environment.bridgeStats().reorderedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 2);
  environment.resetRequest(reordered);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentBootstrapsThreeProviders)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 3;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  BOOST_CHECK_EQUAL(environment.providerCount(), 3);
  BOOST_CHECK(environment.provider(0).getCurrentPolicyEpoch() > 0);
  BOOST_CHECK(environment.provider(1).getCurrentPolicyEpoch() > 0);
  BOOST_CHECK(environment.provider(2).getCurrentPolicyEpoch() > 0);
  BOOST_CHECK(environment.provider(0).getName() != environment.provider(1).getName());
  BOOST_CHECK(environment.provider(1).getName() != environment.provider(2).getName());
  BOOST_CHECK(!environment.user().getAllowedServices().empty());
  BOOST_CHECK_THROW(environment.providerFace(3), std::out_of_range);
  BOOST_CHECK_THROW(environment.providerPubSub(3), std::out_of_range);

  auto scope = environment.beginRequest("three-provider-ready");
  environment.markRequestPublished(scope);
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentRunsGenericRequestLifecycle)
{
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  auto scope = environment.beginRequest("fixture-request-1");

  const auto serviceName = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  bool requestPublished = false;
  bool providerReceived = false;
  bool handlerCalled = false;
  bool ackPublished = false;
  bool responsePublished = false;
  bool responseReceived = false;
  std::vector<uint8_t> deferredResponseWire;
  ndn::Name deferredResponseName;

  environment.provider().addHandler<FixtureRequest, FixtureResponse>(
      serviceName,
      std::function<void(const ndn::Name&, const FixtureRequest&, FixtureResponse&)>(
          [&] (const ndn::Name& requester, const FixtureRequest& request,
               FixtureResponse& response) {
            BOOST_CHECK_EQUAL(requester, environment.user().getName());
            BOOST_CHECK_EQUAL(request.getPayload(), "fixture-payload");
            handlerCalled = true;
            response.setLabel("fixture-response");
          }));

  environment.providerPubSub().subscribeToProducer(
      environment.profile().userNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto parsed = parseRequestNameV2(publication.name);
        if (!parsed || parsed->serviceName != serviceName) {
          return;
        }
        providerReceived = true;
        ndn::Block requestBlock(publication.data);
        RequestMessage publishedRequest;
        BOOST_REQUIRE(publishedRequest.WireDecode(requestBlock));
        const auto response = environment.provider().handleDecryptedRequestByName(
            publication.name, requestBlock);
        BOOST_REQUIRE(response.getStatus());

        RequestAckMessage ack;
        ack.setStatus(true);
        ack.setMessage("fixture-ack");
        ack.setUserToken(publishedRequest.getUserToken());
        ack.setProviderToken("fixture-provider-token");
        const auto ackName = makeRequestAckNameV2(
            providerName, parsed->requesterName, parsed->serviceName,
            parsed->requestId);
        const auto ackBlock = ack.WireEncode();
        environment.providerPubSub().publish(
            ackName, ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));
        ackPublished = true;

        deferredResponseName = makeResponseNameV2(
            providerName, parsed->requesterName, parsed->serviceName,
            parsed->requestId);
        const auto responseBlock = response.WireEncode();
        deferredResponseWire.assign(responseBlock.data(),
                                    responseBlock.data() + responseBlock.size());
      },
      true);

  environment.userPubSub().subscribeToProducer(
      environment.profile().providerNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto parsedAck = parseRequestAckNameV2(publication.name);
        if (parsedAck && parsedAck->serviceName == serviceName) {
          ndn::Block ackBlock(publication.data);
          if (environment.user().handleRequestAckByName(publication.name, ackBlock)) {
            if (!responsePublished && !deferredResponseWire.empty()) {
              environment.providerPubSub().publish(
                  deferredResponseName,
                  ndn::span<const uint8_t>(deferredResponseWire.data(),
                                           deferredResponseWire.size()));
              responsePublished = true;
            }
          }
          return;
        }

        const auto parsedResponse = parseResponseNameV2(publication.name);
        if (!parsedResponse || parsedResponse->serviceName != serviceName) {
          return;
        }
        ndn::Block responseBlock(publication.data);
        responseReceived = environment.user().handleDecryptedResponseByName(
            publication.name, responseBlock);
      },
      true);

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedServiceName,
           const RequestMessage& requestMessage, size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), 1);
        BOOST_CHECK_EQUAL(providers.front(), providerName);
        BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        const auto requestBlock = requestMessage.WireEncode();
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
        requestPublished = true;
        environment.markRequestPublished(scope);
      });

  FixtureRequest request;
  request.setPayload("fixture-payload");
  bool typedCallbackCalled = false;
  const auto requestId = environment.user().RequestService<FixtureRequest, FixtureResponse>(
      {providerName}, serviceName, request,
      std::function<void(const FixtureResponse&)>(
          [&] (const FixtureResponse& response) {
            BOOST_CHECK_EQUAL(response.getLabel(), "fixture-response");
            typedCallbackCalled = true;
          }),
      std::function<void()>([] { BOOST_FAIL("fixture request unexpectedly timed out"); }),
      1000, tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());

  environment.pumpUntil([&] { return typedCallbackCalled; });
  BOOST_CHECK(requestPublished);
  BOOST_CHECK(providerReceived);
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(ackPublished);
  BOOST_CHECK(responsePublished);
  BOOST_CHECK(responseReceived);
  BOOST_CHECK(typedCallbackCalled);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentRunsThreeProviderCustomSelection)
{
  test::BootstrapProfile profile;
  profile.providerCount = 3;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto scope = environment.beginRequest("three-provider-custom-selection");

  const auto serviceName = environment.profile().serviceName;
  std::vector<ndn::Name> providerNames;
  providerNames.reserve(environment.providerCount());
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    providerNames.push_back(environment.provider(index).getName());
  }

  using DeferredResponse =
      std::pair<ndn::Name, std::vector<uint8_t>>;
  std::vector<std::map<std::string, DeferredResponse>> deferredResponses(
      environment.providerCount());
  std::vector<size_t> finalResponseCounts(environment.providerCount(), 0);
  bool requestPublished = false;
  bool customSelectionCalled = false;
  bool responseReceived = false;
  bool timedOut = false;

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    provider.addHandler<FixtureRequest, FixtureResponse>(
        serviceName,
        std::function<void(const ndn::Name&, const FixtureRequest&, FixtureResponse&)>(
            [&, index] (const ndn::Name&, const FixtureRequest& request,
                        FixtureResponse& response) {
              BOOST_CHECK_EQUAL(request.getPayload(), "custom-selection");
              response.setLabel("provider-" + std::to_string(index));
            }));

    environment.providerPubSub(index).subscribeToProducer(
        environment.profile().userNode,
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (auto parsedRequest = parseRequestNameV2(publication.name)) {
            if (!parsedRequest->serviceName.equals(serviceName)) {
              return;
            }
            ndn::Block requestBlock(publication.data);
            RequestMessage requestMessage;
            BOOST_REQUIRE(requestMessage.WireDecode(requestBlock));
            const auto response = environment.provider(index)
                                      .handleDecryptedRequestByName(
                                          publication.name, requestBlock);
            BOOST_REQUIRE(response.getStatus());

            RequestAckMessage ack;
            ack.setStatus(true);
            ack.setMessage("provider-" + std::to_string(index));
            ack.setUserToken(requestMessage.getUserToken());
            ack.setProviderToken(
                "fixture-provider-token-" + std::to_string(index));
            const auto ackName = makeRequestAckNameV2(
                providerNames[index], parsedRequest->requesterName,
                parsedRequest->serviceName, parsedRequest->requestId);
            const auto ackBlock = ack.WireEncode();
            environment.providerPubSub(index).publish(
                ackName,
                ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));

            const auto responseName = makeResponseNameV2(
                providerNames[index], parsedRequest->requesterName,
                parsedRequest->serviceName, parsedRequest->requestId);
            const auto responseBlock = response.WireEncode();
            deferredResponses[index][parsedRequest->requestId.toUri()] =
                std::make_pair(
                    responseName,
                    std::vector<uint8_t>(
                        responseBlock.data(),
                        responseBlock.data() + responseBlock.size()));
            return;
          }

          const auto selection = parseServiceSelectionNameV2(publication.name);
          if (!selection || !selection->serviceName.equals(serviceName) ||
              !selection->providerName.equals(providerNames[index])) {
            return;
          }
          const auto it =
              deferredResponses[index].find(selection->requestId.toUri());
          if (it == deferredResponses[index].end()) {
            return;
          }
          environment.providerPubSub(index).publish(
              it->second.first,
              ndn::span<const uint8_t>(it->second.second.data(),
                                       it->second.second.size()));
          ++finalResponseCounts[index];
        },
        true);

    auto providerNode = environment.profile().providerNode;
    if (index > 0) {
      providerNode.append("p" + std::to_string(index));
    }
    environment.userPubSub().subscribeToProducer(
        providerNode,
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (auto parsedAck = parseRequestAckNameV2(publication.name)) {
            if (parsedAck->serviceName.equals(serviceName)) {
              ndn::Block ackBlock(publication.data);
              environment.user().handleRequestAckByName(
                  publication.name, ackBlock);
            }
            return;
          }
          if (auto parsedResponse = parseResponseNameV2(publication.name)) {
            if (!parsedResponse->serviceName.equals(serviceName)) {
              return;
            }
            ndn::Block responseBlock(publication.data);
            responseReceived =
                environment.user().handleDecryptedResponseByName(
                    publication.name, responseBlock) || responseReceived;
          }
        },
        true);
  }

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&,
           const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedServiceName,
           const RequestMessage& requestMessage,
           size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), providerNames.size());
        BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        const auto requestBlock = requestMessage.WireEncode();
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
        requestPublished = true;
        environment.markRequestPublished(scope);
      });

  RequestMessage requestMessage;
  const std::string payload = "custom-selection";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  requestMessage.setPayload(requestPayload, requestPayload.size());
  const auto requestId = environment.user().RequestService(
      providerNames, serviceName, requestMessage, 40,
      ServiceUser::AckCandidatesHandler(
          [&] (const std::vector<AckSelectionCandidate>& candidates) {
            customSelectionCalled = true;
            std::vector<AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              if (candidate.providerName.equals(providerNames[1])) {
                selected.push_back(candidate);
              }
            }
            BOOST_REQUIRE_EQUAL(selected.size(), 1);
            const auto selectedRequestId = selected.front().requestId;
            ServiceSelectionMessage selection;
            selection.setRequestIDs({selectedRequestId.toUri()});
            selection.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
            SelectionProviderEntry providerEntry;
            providerEntry.providerName = providerNames[1];
            selection.addProviderEntry(providerEntry);
            const auto selectionName = makeServiceSelectionNameV2(
                environment.user().getName(), providerNames[1], serviceName,
                selectedRequestId);
            const auto selectionBlock = selection.WireEncode();
            environment.userPubSub().publish(
                selectionName,
                ndn::span<const uint8_t>(
                    selectionBlock.data(), selectionBlock.size()));
            return selected;
          }),
      1000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage& response) {
        FixtureResponse typedResponse;
        const auto responsePayload = response.getPayload();
        BOOST_REQUIRE(typedResponse.ParseFromArray(
            responsePayload.data(), responsePayload.size()));
        BOOST_CHECK_EQUAL(typedResponse.getLabel(), "provider-1");
      },
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());

  environment.pumpUntil([&] { return responseReceived; });
  BOOST_CHECK(requestPublished);
  BOOST_CHECK(customSelectionCalled);
  BOOST_CHECK(!timedOut);
  BOOST_CHECK(responseReceived);
  BOOST_CHECK_EQUAL(finalResponseCounts[0], 0);
  BOOST_CHECK_EQUAL(finalResponseCounts[1], 1);
  BOOST_CHECK_EQUAL(finalResponseCounts[2], 0);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
