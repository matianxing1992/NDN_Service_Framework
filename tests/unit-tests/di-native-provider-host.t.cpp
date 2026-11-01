/* spec182 T009-A/T009-B/T009-C: Spec182Registration + Spec182SharedLease +
 * Spec182ProviderHost frozen selectors.
 *
 * Registration 全部通过真实注册（addScopedService/addScopedCollaborationHandler）
 * 与 V2 request/selection 投递驱动：真实 inline/worker dispatch、真实 Face
 * 完成路径、真实 cleanup，不以独立 bool 代替。六 selector 对应
 * specs/182-native-di-python-bindings/contracts/native-provider-lifecycle-design.md
 * 六路径 gate 表（Selection/ack/finish/cleanup）。
 *
 * SharedLease 三个 selector 通过真实 ExecutionLeaseService::handle + wire
 * （encode/decode）驱动共享 host state：双 target 争同槽、跨 target
 * 非 Prepare 操作拒绝、target 关闭后执行中槽不被提前释放。
 *
 * ProviderHost 通过真实 Core scoped registration 驱动
 * NativeInferenceProvider（CD-014）host surface：双 target 共享固定 lease
 * 入口与 host 一致性 fence、duplicate/close/re-register、stop 的幂等 fence、
 * close 后新 request 不再建立 pending、固定 lease 入口在 target close 与
 * re-serve 后仍对 sibling/新 target 真实路由（wire 级深度断言见
 * Spec182ProviderHost 内固定入口真实 dispatch；collab handler 真实执行与
 * 真实 NFD 多入口留 T016）。
 */

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"

#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include <ndn-cxx/security/verification-helpers.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <fstream>
#include <filesystem>
#include <limits>
#include <cstdlib>
#include <unistd.h>

namespace ndn_service_framework::test {

namespace {

template <typename Predicate>
bool
waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

ndn::Buffer
makeRequestWire(const std::string& payload)
{
  const auto request = makeRequestMessageWithUserToken(payload);
  const auto requestBlock = request.WireEncode();
  return ndn::Buffer(requestBlock.data(), requestBlock.size());
}

// request 与其 wire 同源返回：token 播种与 accept 注入必须携带同一个
// user token（m_useTokens 默认 true，user token 进 replay hash；默认
// fixture token 是常量，多个 accept 必须显式取不同 user token）。
std::pair<RequestMessage, ndn::Buffer>
makeAuthenticatedRequestWire(const std::string& payload,
                             const std::string& userToken)
{
  const auto request = makeRequestMessageWithUserToken(payload, userToken);
  const auto requestBlock = request.WireEncode();
  return {request, ndn::Buffer(requestBlock.data(), requestBlock.size())};
}

ServiceProvider::AckStrategyHandler
makeAcceptingAckHandler()
{
  return ServiceProvider::AckStrategyHandler(
    [] (const RequestMessage&) {
      ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "accept";
      return decision;
    });
}

ServiceProvider::RequestHandler
makeCountingRequestHandler(std::atomic<int>& executions)
{
  return ServiceProvider::RequestHandler(
    [&executions] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                   const ndn::Name&, const RequestMessage&) {
      ++executions;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });
}

class Spec212CountingPacketStore final :
  public ndn_service_framework::EncryptedLargeDataRangeStore
{
public:
  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitFile(const std::string&, const std::filesystem::path&, std::uint64_t,
             const std::function<void()>&) override
  {
    ++commitCalls;
    return {};
  }

  std::atomic<unsigned> commitCalls{0};
};

// 真实 accept：把请求投到 provider 的解密完成入口。LocalMock 默认
// handler pool 为 0，decode 与 accept commit 同步完成；启用 worker 的
// 编排必须先 drain handler pool 再泵 io（commit 经 io post 落 Face
// 线程），此处只保证同步面可观察。
void
injectRequest(ServiceProvider& provider,
              const ndn::Name& requesterName,
              const ndn::Name& serviceName,
              const ndn::Name& requestId,
              const ndn::Buffer& requestWire)
{
  provider.OnRequestDecryptionSuccessCallbackV2(
    requesterName, serviceName, requestId, requestWire);
}

// 解码 selection 并计算其 digest（dispatch 的 status 按 digest 记账）。
std::string
selectionDigestFor(const ndn::Buffer& selection)
{
  ServiceSelectionMessage selectionMessage;
  auto [ok, block] = ndn::Block::fromBuffer(
    ndn::span<const uint8_t>(selection.data(), selection.size()));
  if (!ok || !selectionMessage.WireDecode(block)) {
    return std::string();
  }
  return computeSelectionDigest(selectionMessage);
}

// Collaboration dispatch 是 protected；子类暴露受控测试入口（样板同
// generic-dynamic-api-collaboration-status.t.cpp）。
class RegistrationCollabProvider : public LocalServiceProvider
{
public:
  using LocalServiceProvider::LocalServiceProvider;

  // 排队一个哨兵任务；等它执行完即此前 post 到 handler pool 的任务
  // （例如异步 decode + accept commit）都已同步完成。
  void drainHandlerWorker()
  {
    auto drained = std::make_shared<std::promise<void>>();
    auto ready = drained->get_future();
    BOOST_REQUIRE(m_handlerPool.post([drained] { drained->set_value(); }));
    BOOST_REQUIRE(ready.wait_for(std::chrono::seconds(1)) ==
                  std::future_status::ready);
  }

  void dispatchCollabForTest(const ndn::Name& requesterName,
                             const ndn::Name& requestId,
                             const ndn::Name& serviceName,
                             const ndn::Name& requestPayload,
                             const std::string& role,
                             const std::string& selectionDigest)
  {
    auto request = makeRequestMessageWithUserToken(requestPayload.toUri());
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = role;
    assignment.service = serviceName;
    BOOST_REQUIRE(dispatchCollaborationExecutionAsync(
      requesterName, identity, serviceName, requestId,
      request, std::move(assignment), selectionDigest));
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec204RecoveryPolicy)
BOOST_AUTO_TEST_CASE(TypedAndJsonPoliciesRejectActualTransportBoundaryViolations)
{
  using namespace ndnsf::di;
  const NativeJson value{{"privateScratchRoot", "/tmp/spec204-private"},
    {"repoForwardingHints", {"/repo/A"}}, {"maxPayloadBytes", 128U},
    {"maxEncodedBytes", 256U}, {"maxMetadataBytes", 512U}, {"maxCheckpointBytes", 1024U},
    {"maxProtectedPlaintextBytes", 1024U}, {"maxProtectedWireBytes", 2048U},
    {"maxTransportEnvelopeBytes", 4096U}};
  auto policy = nativeProviderKvRecoveryConfigFromJson(nativeCanonicalJson(value));
  BOOST_CHECK_NO_THROW(policy.validate());
  int probes = 0;
  policy.protectedLimits.cancelled = [&] { ++probes; return false; };
  policy.portableLimits.portable.cancelled = [&] { ++probes; return false; };
  policy.validate();
  BOOST_CHECK_EQUAL(probes, 0);
  BOOST_CHECK(!policy.protectedLimits.cancelled());
  BOOST_CHECK(!policy.portableLimits.portable.cancelled());
  BOOST_CHECK_EQUAL(probes, 2);
  const auto rejects = [](const NativeJson& changed) {
    BOOST_CHECK_THROW(nativeProviderKvRecoveryConfigFromJson(nativeCanonicalJson(changed)), std::exception);
  };
  auto changed = value; changed["repoForwardingHints"] = NativeJson::array(); rejects(changed);
  changed = value; changed["repoForwardingHints"] = std::vector<std::string>(9, "/repo/A"); rejects(changed);
  changed = value; changed["repoForwardingHints"] = {"/" + std::string(1100, 'a')}; rejects(changed);
  changed = value; changed["maxProtectedWireBytes"] = std::uint64_t(std::numeric_limits<int>::max()) + 1;
  changed["maxTransportEnvelopeBytes"] = std::uint64_t(std::numeric_limits<int>::max()) + 4096; rejects(changed);
  changed = value; changed["maxPayloadBytes"] = 257U; rejects(changed);
  changed = value; changed["maxCheckpointBytes"] = 0U; rejects(changed);
  changed = value; changed["privateScratchRoot"] = "relative"; rejects(changed);
  changed = value; changed["recipientKey"] = "not-policy"; rejects(changed);
  auto typed = policy; typed.repoForwardingHints.clear();
  BOOST_CHECK_THROW(typed.validate(), std::invalid_argument);
  typed = policy; typed.portableLimits.portable.maxPayloadBytes = 257;
  BOOST_CHECK_THROW(typed.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PublicConfigOwnsPolicyValuesAcrossFileChangesAndRelativeLaunchPaths)
{
  using namespace ndnsf::di;
  std::string pattern = (std::filesystem::temp_directory_path() / "spec204-policy-XXXXXX").string();
  std::vector<char> path(pattern.begin(), pattern.end()); path.push_back('\0');
  BOOST_REQUIRE(::mkdtemp(path.data()));
  struct Scratch {
    std::filesystem::path root;
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  } scratch{path.data()};
  const auto policyPath = scratch.root / "policy.json";
  NativeJson policy{{"privateScratchRoot", scratch.root.string()},
    {"repoForwardingHints", {"/repo/A"}}, {"maxPayloadBytes", 128U},
    {"maxEncodedBytes", 256U}, {"maxMetadataBytes", 512U}, {"maxCheckpointBytes", 1024U},
    {"maxProtectedPlaintextBytes", 1024U}, {"maxProtectedWireBytes", 2048U},
    {"maxTransportEnvelopeBytes", 4096U}};
  const auto writePolicy = [&] {
    std::ofstream output(policyPath); output << nativeCanonicalJson(policy);
    BOOST_REQUIRE(output.good());
  };
  writePolicy();
  std::vector<std::string> arguments{"provider", "--provider", "/provider/A", "--trust-schema",
    (scratch.root / "schema").string(), "--service", "/LLM/Generate", "--kv-recovery-config", policyPath.string()};
  const auto parse = [&] {
    std::vector<const char*> argv;
    for (const auto& argument : arguments) argv.push_back(argument.c_str());
    return ProviderConfig::fromCommandLine(static_cast<int>(argv.size()), argv.data());
  };
  const auto first = parse(); const auto same = parse();
  BOOST_CHECK(first.equivalent(same));
  auto launchArguments = arguments; launchArguments.erase(launchArguments.begin());
  launchArguments.back() = "policy.json";
  const auto launchPath = scratch.root / "launch.json";
  {
    std::ofstream output(launchPath);
    output << nativeCanonicalJson(NativeJson{{"schema", "ndnsf-di-native-provider-launch-v1"},
      {"arguments", launchArguments}});
    BOOST_REQUIRE(output.good());
  }
  BOOST_CHECK(first.equivalent(ProviderConfig::fromFile(launchPath)));
  policy["maxPayloadBytes"] = 129U; writePolicy();
  BOOST_CHECK(!first.equivalent(parse()));
  std::filesystem::remove(policyPath);
  BOOST_CHECK(first.equivalent(same));
  BOOST_CHECK_THROW(parse(), std::invalid_argument);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Spec182Registration)

// Selector 1 (ack 窗口): ack 决策已由真实 ack worker 作出、commit 尚未落
// 入 Face 时 close。closed gate 必须把 commit 降级为负路径：pending 不
// store、request handler 不执行。
BOOST_AUTO_TEST_CASE(Spec182RegistrationCloseBeforeAckFinish)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-reg-close-before-ack",
                                   "tpm-memory:spec182-reg-close-before-ack");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/alice");
  const ndn::Name providerName("/spec182/provider/host-close-ack");
  const ndn::Name serviceName("/Inference/Spec182RegistrationA");
  const ndn::Name requestId("/request/close-before-ack-finish");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-close-ack"));
  LocalServiceProvider provider(face,
                                ndn::Name("/spec182/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  std::atomic<int> ackHandlerCalls{0};
  std::atomic<int> requestHandlerCalls{0};
  provider.setAckThreads(1);
  ServiceProvider::ServiceRegistration handle = provider.addScopedService(
    serviceName,
    ServiceProvider::AckStrategyHandler(
      [&] (const RequestMessage&) {
        ++ackHandlerCalls;
        // 拉开决策与 commit 之间的真实窗口：主线程在 worker 睡醒前 close。
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "accept";
        return decision;
      }),
    makeCountingRequestHandler(requestHandlerCalls),
    ServiceProvider::ServiceMode::Normal);

  const auto requestWire = makeRequestWire("payload-before-close");
  injectRequest(provider, requesterName, serviceName, requestId, requestWire);

  // ack worker 已进入 handler（决策途中）-> close 先于 commit。
  BOOST_REQUIRE(waitUntil([&] { return ackHandlerCalls.load() == 1; },
                          std::chrono::seconds(5)));
  handle.close();
  BOOST_CHECK(handle.closed());
  // close() 消耗 handle（RAII 释放语义）；注册代次已随 entry 关闭。
  BOOST_CHECK(!handle.valid());

  // worker 睡醒后把 commit post 到 Face；pump 执行它（closed gate 命中）。
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  pumpFace(face, ndn::time::milliseconds(800));

  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId));
  BOOST_CHECK_EQUAL(requestHandlerCalls.load(), 0);
}

BOOST_AUTO_TEST_CASE(Spec194AsyncAckCarriesOuterIdentityAndCancelsOnRegistrationClose)
{
  ndn::security::KeyChain keyChain("pib-memory:spec194-async-ack",
                                   "tpm-memory:spec194-async-ack");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec194/user/alice");
  const ndn::Name providerName("/spec194/provider/async-ack");
  const ndn::Name serviceName("/Inference/Spec194AsyncAck");
  const ndn::Name requestId("/request/spec194-async-ack");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec194/aa-async-ack"));
  RegistrationCollabProvider provider(face, ndn::Name("/spec194/group"),
                                      providerCert, aaCert,
                                      "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName, tlv::ProviderPermission,
                           providerName, serviceName));
  provider.setAckThreads(1);

  std::mutex completionMutex;
  std::optional<ServiceProvider::AckRequestContext> observedContext;
  ServiceProvider::AsyncAckCompletion retainedCompletion;
  auto handle = provider.addScopedAsyncCollaborationHandler(
    serviceName, std::vector<CollaborationRole>{},
    ServiceProvider::AsyncAckStrategyHandler(
      [&] (const ServiceProvider::AckRequestContext& context,
           const RequestMessage&, ServiceProvider::AsyncAckCompletion complete) {
        std::lock_guard<std::mutex> lock(completionMutex);
        observedContext = context;
        retainedCompletion = std::move(complete);
      }),
    ServiceProvider::CollaborationHandler(
      [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));

  injectRequest(provider, requesterName, serviceName, requestId,
                makeRequestWire("spec194-async-ack"));
  BOOST_REQUIRE(waitUntil([&] {
    std::lock_guard<std::mutex> lock(completionMutex);
    return observedContext.has_value() && static_cast<bool>(retainedCompletion);
  }, std::chrono::seconds(2)));

  ServiceProvider::AckRequestContext context;
  ServiceProvider::AsyncAckCompletion complete;
  {
    std::lock_guard<std::mutex> lock(completionMutex);
    context = *observedContext;
    complete = retainedCompletion;
  }
  BOOST_CHECK(context.requesterIdentity == requesterName);
  BOOST_CHECK(context.serviceName == serviceName);
  BOOST_CHECK(context.requestId == requestId);
  BOOST_CHECK_EQUAL(context.registrationGeneration, handle.generation());
  BOOST_CHECK(context.deadline > std::chrono::steady_clock::now());
  BOOST_CHECK(!context.isCancelled());

  handle.close();
  BOOST_CHECK(context.isCancelled());
  ServiceProvider::AckDecision accepted;
  accepted.status = true;
  accepted.message = "late async completion";
  complete(accepted);
  complete(accepted);
  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId));
}

BOOST_AUTO_TEST_CASE(Spec194AsyncAckDeadlineCompletesAndCancelsRetainedHandler)
{
  ndn::security::KeyChain keyChain("pib-memory:spec194-async-ack-deadline",
                                   "tpm-memory:spec194-async-ack-deadline");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec194/user/deadline");
  const ndn::Name providerName("/spec194/provider/async-ack-deadline");
  const ndn::Name serviceName("/Inference/Spec194AsyncAckDeadline");
  const ndn::Name requestId("/request/spec194-async-ack-deadline");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec194/aa-async-ack-deadline"));
  RegistrationCollabProvider provider(face, ndn::Name("/spec194/group"),
                                      providerCert, aaCert,
                                      "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName, tlv::ProviderPermission,
                           providerName, serviceName));
  provider.setAckThreads(1);

  std::mutex completionMutex;
  std::optional<ServiceProvider::AckRequestContext> observedContext;
  ServiceProvider::AsyncAckCompletion retainedCompletion;
  auto handle = provider.addScopedAsyncCollaborationHandler(
    serviceName, std::vector<CollaborationRole>{},
    ServiceProvider::AsyncAckStrategyHandler(
      [&] (const ServiceProvider::AckRequestContext& context,
           const RequestMessage&, ServiceProvider::AsyncAckCompletion complete) {
        std::lock_guard<std::mutex> lock(completionMutex);
        observedContext = context;
        retainedCompletion = std::move(complete);
      }),
    ServiceProvider::CollaborationHandler(
      [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));

  injectRequest(provider, requesterName, serviceName, requestId,
                makeRequestWire("spec194-async-ack-deadline"));
  BOOST_REQUIRE(waitUntil([&] {
    std::lock_guard<std::mutex> lock(completionMutex);
    return observedContext.has_value() && static_cast<bool>(retainedCompletion);
  }, std::chrono::seconds(2)));

  ServiceProvider::AckRequestContext context;
  ServiceProvider::AsyncAckCompletion complete;
  {
    std::lock_guard<std::mutex> lock(completionMutex);
    context = *observedContext;
    complete = retainedCompletion;
  }
  BOOST_CHECK(!context.isCancelled());
  const auto latestExpected = context.deadline + std::chrono::milliseconds(100);
  while (!context.isCancelled() &&
         std::chrono::steady_clock::now() < latestExpected) {
    pumpFace(face, ndn::time::milliseconds(20));
  }
  const auto completedAt = std::chrono::steady_clock::now();
  BOOST_CHECK(context.isCancelled());
  BOOST_CHECK(completedAt >= context.deadline);
  BOOST_CHECK(completedAt <= latestExpected);
  complete(ServiceProvider::AckDecision{});
  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId));
}

BOOST_AUTO_TEST_CASE(Spec194AsyncAckCompletionAfterProviderDestructionIsSafe)
{
  ndn::security::KeyChain keyChain("pib-memory:spec194-async-ack-destroyed",
                                   "tpm-memory:spec194-async-ack-destroyed");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec194/user/destroyed");
  const ndn::Name providerName("/spec194/provider/async-ack-destroyed");
  const ndn::Name serviceName("/Inference/Spec194AsyncAckDestroyed");
  const ndn::Name requestId("/request/spec194-async-ack-destroyed");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec194/aa-async-ack-destroyed"));
  ServiceProvider::AsyncAckCompletion retainedCompletion;
  {
    RegistrationCollabProvider provider(face, ndn::Name("/spec194/group"),
                                        providerCert, aaCert,
                                        "examples/trust-any.conf");
    provider.applyPermissionResponse(
      makePermissionResponse(providerName, tlv::ProviderPermission,
                             providerName, serviceName));
    provider.setAckThreads(1);

    std::mutex completionMutex;
    bool handlerEntered = false;
    auto handle = provider.addScopedAsyncCollaborationHandler(
      serviceName, std::vector<CollaborationRole>{},
      ServiceProvider::AsyncAckStrategyHandler(
        [&] (const ServiceProvider::AckRequestContext&,
             const RequestMessage&, ServiceProvider::AsyncAckCompletion complete) {
          std::lock_guard<std::mutex> lock(completionMutex);
          handlerEntered = true;
          retainedCompletion = std::move(complete);
        }),
      ServiceProvider::CollaborationHandler(
        [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));

    injectRequest(provider, requesterName, serviceName, requestId,
                  makeRequestWire("spec194-async-ack-destroyed"));
    BOOST_REQUIRE(waitUntil([&] {
      std::lock_guard<std::mutex> lock(completionMutex);
      return handlerEntered && static_cast<bool>(retainedCompletion);
    }, std::chrono::seconds(2)));
  }

  BOOST_REQUIRE(static_cast<bool>(retainedCompletion));
  ServiceProvider::AckDecision late;
  late.status = true;
  late.message = "completion after provider destruction";
  retainedCompletion(std::move(late));
  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(Spec194AsyncAckLateSuccessAfterFaceDeadlineIsNegative)
{
  ndn::security::KeyChain keyChain("pib-memory:spec194-async-ack-late",
                                   "tpm-memory:spec194-async-ack-late");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec194/user/late");
  const ndn::Name providerName("/spec194/provider/async-ack-late");
  const ndn::Name serviceName("/Inference/Spec194AsyncAckLate");
  const ndn::Name requestId("/request/spec194-async-ack-late");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec194/aa-async-ack-late"));
  RegistrationCollabProvider provider(face, ndn::Name("/spec194/group"),
                                      providerCert, aaCert,
                                      "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName, tlv::ProviderPermission,
                           providerName, serviceName));
  provider.setAckThreads(1);

  std::mutex completionMutex;
  std::optional<ServiceProvider::AckRequestContext> observedContext;
  ServiceProvider::AsyncAckCompletion retainedCompletion;
  auto handle = provider.addScopedAsyncCollaborationHandler(
    serviceName, std::vector<CollaborationRole>{},
    ServiceProvider::AsyncAckStrategyHandler(
      [&] (const ServiceProvider::AckRequestContext& context,
           const RequestMessage&, ServiceProvider::AsyncAckCompletion complete) {
        std::lock_guard<std::mutex> lock(completionMutex);
        observedContext = context;
        retainedCompletion = std::move(complete);
      }),
    ServiceProvider::CollaborationHandler(
      [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));

  std::optional<RequestAckMessage> observedAck;
  provider.setLocalPublicationHandler(
    [&] (const ndn::Name&, const ndn::Buffer& wire) {
      auto [ok, block] = ndn::Block::fromBuffer(
        ndn::span<const std::uint8_t>(wire.data(), wire.size()));
      if (ok) {
        RequestAckMessage ack;
        if (ack.WireDecode(block))
          observedAck = std::move(ack);
      }
    });
  injectRequest(provider, requesterName, serviceName, requestId,
                makeRequestWire("spec194-async-ack-late"));
  BOOST_REQUIRE(waitUntil([&] {
    std::lock_guard<std::mutex> lock(completionMutex);
    return observedContext.has_value() && static_cast<bool>(retainedCompletion);
  }, std::chrono::seconds(2)));

  ServiceProvider::AckRequestContext context;
  ServiceProvider::AsyncAckCompletion complete;
  {
    std::lock_guard<std::mutex> lock(completionMutex);
    context = *observedContext;
    complete = retainedCompletion;
  }
  std::this_thread::sleep_until(context.deadline + std::chrono::milliseconds(20));
  ServiceProvider::AckDecision lateSuccess;
  lateSuccess.status = true;
  lateSuccess.message = "late success must be rejected";
  complete(std::move(lateSuccess));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_REQUIRE(observedAck);
  BOOST_CHECK(!observedAck->getStatus());
  BOOST_CHECK(context.isCancelled());
}

BOOST_AUTO_TEST_CASE(Spec194AsyncAckLocalPublicationCanDestroyProvider)
{
  ndn::security::KeyChain keyChain("pib-memory:spec194-async-ack-local-destroy",
                                   "tpm-memory:spec194-async-ack-local-destroy");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec194/user/local-destroy");
  const ndn::Name providerName("/spec194/provider/async-ack-local-destroy");
  const ndn::Name serviceName("/Inference/Spec194AsyncAckLocalDestroy");
  const ndn::Name requestId("/request/spec194-async-ack-local-destroy");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/spec194/aa-async-ack-local-destroy"));
  auto provider = std::make_unique<RegistrationCollabProvider>(
    face, ndn::Name("/spec194/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->applyPermissionResponse(
    makePermissionResponse(providerName, tlv::ProviderPermission,
                           providerName, serviceName));
  provider->setAckThreads(1);
  auto handle = provider->addScopedAsyncCollaborationHandler(
    serviceName, std::vector<CollaborationRole>{},
    ServiceProvider::AsyncAckStrategyHandler(
      [] (const ServiceProvider::AckRequestContext&,
          const RequestMessage&, ServiceProvider::AsyncAckCompletion complete) {
        ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "accept before local publication";
        complete(std::move(decision));
      }),
    ServiceProvider::CollaborationHandler(
      [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {}));

  std::atomic<bool> publicationEntered{false};
  provider->setLocalPublicationHandler(
    [&] (const ndn::Name&, const ndn::Buffer&) {
      publicationEntered.store(true, std::memory_order_release);
      provider.reset();
    });
  injectRequest(*provider, requesterName, serviceName, requestId,
                makeRequestWire("spec194-async-ack-local-destroy"));

  BOOST_REQUIRE(waitUntil([&] {
    pumpFace(face, ndn::time::milliseconds(10));
    return publicationEntered.load(std::memory_order_acquire);
  }, std::chrono::seconds(2)));
  BOOST_CHECK(!provider);
}

// Selector 2 (selection 换代窗口, worker 面): 真实 accept 把 R1 绑定到
// gen1；close + 同名重注册为 gen2 后，R1 的旧 selection 必须被拒绝——
// 绑定保留在 pending cleanup 边界内，dispatch 在 worker 路径入口拒绝。
BOOST_AUTO_TEST_CASE(Spec182RegistrationOldSelectionAfterReregister)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-reg-old-selection",
                                   "tpm-memory:spec182-reg-old-selection");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/alice");
  const ndn::Name providerName("/spec182/provider/host-old-selection");
  const ndn::Name serviceName("/Inference/Spec182RegistrationB");
  const ndn::Name requestId("/request/old-generation");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-old-selection"));
  RegistrationCollabProvider provider(face,
                                      ndn::Name("/spec182/group"),
                                      providerCert,
                                      aaCert,
                                      "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  std::atomic<int> executionsGen1{0};
  std::atomic<int> executionsGen2{0};
  provider.setHandlerThreads(1); // worker 面：dispatch 走真实 worker post 链。
  auto handle1 = provider.addScopedService(
    serviceName,
    makeAcceptingAckHandler(),
    makeCountingRequestHandler(executionsGen1),
    ServiceProvider::ServiceMode::Normal);

  // 真实 accept R1：先种已知 provider token（accept commit 复用它），再
  // 投递 wire。worker 面 decode 跑在 handler pool，decode 完成后 commit
  // 经 io post 落 Face 线程——drain 只保证 decode 完成，还需泵 io 使
  // finishDecodedRequestOnEventLoop 落地（绑定写入），之后才能 close。
  const auto [request1, requestWire1] =
    makeAuthenticatedRequestWire("payload-old-generation", "spec182-user-token-gen1");
  provider.addPendingRequestForTokenTest(
    requesterName, serviceName, requestId, request1, "provider-token-gen1");
  provider.OnRequestDecryptionSuccessCallbackV2(
    requesterName, serviceName, requestId, requestWire1);
  provider.drainHandlerWorker();
  pumpFace(face, ndn::time::milliseconds(300));
  BOOST_REQUIRE(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId));

  handle1.close();

  // 同名重建（addScoped 自身先 drain closed parked entry）。
  auto handle2 = provider.addScopedService(
    serviceName,
    makeAcceptingAckHandler(),
    makeCountingRequestHandler(executionsGen2),
    ServiceProvider::ServiceMode::Normal);
  BOOST_CHECK(handle2.valid());
  BOOST_CHECK(!handle2.closed());

  // gen1 的旧 selection 到达 gen2 注册名：token 匹配（pending 保留在
  // close 边界内），dispatch 在 worker 路径入口被代次 fence 拒绝。
  const auto selection = makeSelectionBuffer(requestId, "provider-token-gen1");
  const auto digest = selectionDigestFor(selection);
  BOOST_REQUIRE(!digest.empty());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName, providerName, serviceName, requestId, selection);
  SelectionExecutionStatus status;
  BOOST_REQUIRE(waitUntil(
    [&] {
      const auto found = provider.getSelectionExecutionStatus(digest);
      if (!found) return false;
      status = *found;
      return true;
    },
    std::chrono::seconds(5)));
  BOOST_CHECK_EQUAL(executionsGen1.load(), 0);
  BOOST_CHECK_EQUAL(executionsGen2.load(), 0);
  BOOST_CHECK(status.state == SelectionExecutionState::Failed);
  BOOST_CHECK(status.message.find("generation changed") != std::string::npos);
}

// Selector 3 (inline 面): 默认 pool=0 的 inline dispatch 走同一
// fencePendingRegistrationExecution gate——inline 不能绕过代次绑定。
// 正向对照：换代后同一 service 的新请求 R2 在 inline 面执行成功。
BOOST_AUTO_TEST_CASE(Spec182RegistrationInlineDispatchFence)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-reg-inline-fence",
                                   "tpm-memory:spec182-reg-inline-fence");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/alice");
  const ndn::Name providerName("/spec182/provider/host-inline-fence");
  const ndn::Name serviceName("/Inference/Spec182RegistrationC");
  const ndn::Name oldRequestId("/request/old-generation-inline");
  const ndn::Name newRequestId("/request/successor-inline");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-inline-fence"));
  LocalServiceProvider provider(face,
                                ndn::Name("/spec182/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  std::atomic<int> executionsGen1{0};
  std::atomic<int> executionsGen2{0};
  auto handle1 = provider.addScopedService(
    serviceName,
    makeAcceptingAckHandler(),
    makeCountingRequestHandler(executionsGen1),
    ServiceProvider::ServiceMode::Normal);

  // 真实 accept R1（inline：LocalMock 全链同步）。R2 必须用独立 user
  // token 的 wire，否则 request replay hash 拒绝 R2。
  const auto [request1, requestWire1] =
    makeAuthenticatedRequestWire("payload-old-generation-inline",
                                 "spec182-user-token-old-inline");
  provider.addPendingRequestForTokenTest(
    requesterName, serviceName, oldRequestId, request1, "provider-token-old");
  injectRequest(provider, requesterName, serviceName, oldRequestId, requestWire1);
  BOOST_REQUIRE(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, oldRequestId));

  handle1.close();
  auto handle2 = provider.addScopedService(
    serviceName,
    makeAcceptingAckHandler(),
    makeCountingRequestHandler(executionsGen2),
    ServiceProvider::ServiceMode::Normal);

  // 旧 selection 在 inline 面被同一 fence 拒（inline 不能绕过代次绑定）。
  const auto oldSelection = makeSelectionBuffer(oldRequestId, "provider-token-old");
  const auto oldDigest = selectionDigestFor(oldSelection);
  BOOST_REQUIRE(!oldDigest.empty());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName, providerName, serviceName, oldRequestId, oldSelection);
  BOOST_CHECK_EQUAL(executionsGen1.load(), 0);
  BOOST_CHECK_EQUAL(executionsGen2.load(), 0);
  const auto rejected = provider.getSelectionExecutionStatus(oldDigest);
  BOOST_REQUIRE(rejected);
  BOOST_CHECK(rejected->state == SelectionExecutionState::Failed);
  BOOST_CHECK(rejected->message.find("generation changed") != std::string::npos);

  // 正向对照：gen2 的新请求 R2（独立 user token 的 wire）正常 accept +
  // inline 执行。
  const auto [request2, requestWire2] =
    makeAuthenticatedRequestWire("payload-successor-inline",
                                 "spec182-user-token-new-inline");
  provider.addPendingRequestForTokenTest(
    requesterName, serviceName, newRequestId, request2, "provider-token-new");
  injectRequest(provider, requesterName, serviceName, newRequestId, requestWire2);
  BOOST_REQUIRE(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, newRequestId));
  const auto newSelection = makeSelectionBuffer(newRequestId, "provider-token-new");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName, providerName, serviceName, newRequestId, newSelection);
  BOOST_CHECK_EQUAL(executionsGen2.load(), 1);
}

// Selector 4 (cleanup 边界, collab): role A 的 pending cleanup（真实
// cleanupPendingRequestState）不得擦除 collaboration 代次绑定；换代后
// 同一 request 的兄弟 role B dispatch 必须被 collab mismatch gate 拒绝。
BOOST_AUTO_TEST_CASE(Spec182RegistrationSiblingAfterPendingCleanup)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-reg-sibling-cleanup",
                                   "tpm-memory:spec182-reg-sibling-cleanup");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/sibling");
  const ndn::Name providerName("/spec182/provider/host-sibling-cleanup");
  const ndn::Name serviceName("/Inference/Spec182RegistrationCollabD");
  const ndn::Name requestId("/request/sibling-after-cleanup");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-sibling-cleanup"));
  RegistrationCollabProvider provider(face,
                                      ndn::Name("/spec182/group"),
                                      providerCert,
                                      aaCert,
                                      "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));
  provider.setHandlerThreads(1);

  std::atomic<int> roleACalls{0};
  std::atomic<int> roleBCalls{0};
  auto handle1 = provider.addScopedCollaborationHandler(
    serviceName,
    std::vector<CollaborationRole>{},
    makeAcceptingAckHandler(),
    ServiceProvider::CollaborationHandler(
      [&] (ServiceProvider::CollaborationContext& ctx, const RequestMessage&) {
        if (ctx.role() == "role-a") {
          ++roleACalls;
        }
        else if (ctx.role() == "role-b") {
          ++roleBCalls;
        }
      }));

  // 真实 collab dispatch role A：prepare 把 requestId -> gen1 绑定写入
  // m_collaborationRegistrationStates，worker 执行 handler。
  provider.dispatchCollabForTest(requesterName, requestId, serviceName,
                                 ndn::Name("payload-role-a"), "role-a",
                                 "sel-role-a");
  BOOST_REQUIRE(waitUntil([&] { return roleACalls.load() == 1; },
                          std::chrono::seconds(5)));

  // role A 请求生命周期结束：pending cleanup 真实函数（只擦 pending 表
  // 与 request 绑定，不擦 collab 代次绑定）。
  provider.cleanupPendingRequestStateForTest(
    requesterName, serviceName, requestId);

  // 换代后同一 request 的兄弟 role B：绑定仍在 -> mismatch 拒绝。
  handle1.close();
  auto handle2 = provider.addScopedCollaborationHandler(
    serviceName,
    std::vector<CollaborationRole>{},
    makeAcceptingAckHandler(),
    ServiceProvider::CollaborationHandler(
      [&] (ServiceProvider::CollaborationContext& ctx, const RequestMessage&) {
        if (ctx.role() == "role-b") {
          ++roleBCalls;
        }
      }));
  BOOST_CHECK(handle2.valid());
  BOOST_CHECK(!handle2.closed());

  provider.dispatchCollabForTest(requesterName, requestId, serviceName,
                                 ndn::Name("payload-role-b"), "role-b",
                                 "sel-role-b");
  BOOST_CHECK_EQUAL(roleBCalls.load(), 0);
  const auto status = provider.getSelectionExecutionStatus("sel-role-b");
  BOOST_REQUIRE(status);
  BOOST_CHECK(status->state == SelectionExecutionState::Failed);
  BOOST_CHECK(status->message.find(
                "collaboration registration generation changed") !=
              std::string::npos);
}

// Selector 5 (析构顺序): Provider 先析构、handle 后析构时，close 是
// 无害 no-op；generation 状态随 Provider 析构关闭但 handle 仍可查询。
BOOST_AUTO_TEST_CASE(Spec182RegistrationCloseAfterProviderDestruction)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-reg-close-after-dtor",
                                   "tpm-memory:spec182-reg-close-after-dtor");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/spec182/provider/host-dtor-order");
  const ndn::Name serviceName("/Inference/Spec182RegistrationE");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-dtor-order"));

  ServiceProvider::ServiceRegistration handle;
  BOOST_CHECK(!handle.valid());
  {
    auto provider = std::make_unique<LocalServiceProvider>(
      face,
      ndn::Name("/spec182/group"),
      providerCert,
      aaCert,
      "examples/trust-any.conf");
    std::atomic<int> executions{0};
    handle = provider->addScopedService(
      serviceName,
      makeAcceptingAckHandler(),
      makeCountingRequestHandler(executions),
      ServiceProvider::ServiceMode::Normal);
    BOOST_CHECK(handle.valid());
    BOOST_CHECK(!handle.closed());
    provider.reset(); // Provider 先析构：关闭所有 owned registrations。
  }

  // Provider 析构关闭了注册代次；handle 仍持有 state 供 closed() 查询，
  // 但 valid()（= 可用注册）为 false。
  BOOST_CHECK(handle.closed());
  BOOST_CHECK(!handle.valid());
  BOOST_CHECK_NO_THROW(handle.close()); // 析构后 close = harmless no-op。
  BOOST_CHECK(handle.closed());
  BOOST_CHECK(!handle.valid());

  ServiceProvider::ServiceRegistration moved = std::move(handle);
  BOOST_CHECK(moved.closed());
  BOOST_CHECK(!moved.valid());
  BOOST_CHECK(!handle.valid());
  BOOST_CHECK_NO_THROW(moved.close());
  // moved 的 RAII 析构在函数作用域结束时自然发生；若其抛异常则测试失败。
}

// Selector 6 (cleanup 精确性): cleanup 一个 pendingKey 不得波及其它
// 请求。R2 的 pending 与绑定在 R1 cleanup 后必须完整：R2 selection 在
// 真实 dispatch 下执行成功，且 R1 自身已清。
BOOST_AUTO_TEST_CASE(Spec182RegistrationCleanupDoesNotEraseSuccessor)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-reg-cleanup-successor",
                                   "tpm-memory:spec182-reg-cleanup-successor");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/alice");
  const ndn::Name providerName("/spec182/provider/host-cleanup-successor");
  const ndn::Name serviceName("/Inference/Spec182RegistrationF");
  const ndn::Name firstRequestId("/request/first");
  const ndn::Name secondRequestId("/request/successor");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-cleanup-successor"));
  LocalServiceProvider provider(face,
                                ndn::Name("/spec182/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  std::atomic<int> executions{0};
  auto handle = provider.addScopedService(
    serviceName,
    makeAcceptingAckHandler(),
    makeCountingRequestHandler(executions),
    ServiceProvider::ServiceMode::Normal);

  // R1 与 R2 先后真实 accept（R2 用独立 user token 的 wire，防 request
  // replay hash 误伤）：两个 pendingKey 各自的绑定都已写入。
  const auto [request1, requestWire1] =
    makeAuthenticatedRequestWire("payload-cleanup-successor-first",
                                 "spec182-user-token-cleanup-first");
  const auto [request2, requestWire2] =
    makeAuthenticatedRequestWire("payload-cleanup-successor-second",
                                 "spec182-user-token-cleanup-second");
  provider.addPendingRequestForTokenTest(
    requesterName, serviceName, firstRequestId, request1, "provider-token-first");
  injectRequest(provider, requesterName, serviceName, firstRequestId, requestWire1);
  BOOST_REQUIRE(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, firstRequestId));
  provider.addPendingRequestForTokenTest(
    requesterName, serviceName, secondRequestId, request2, "provider-token-second");
  injectRequest(provider, requesterName, serviceName, secondRequestId, requestWire2);
  BOOST_REQUIRE(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, secondRequestId));

  // R1 生命周期结束（真实 cleanupPendingRequestState）。
  provider.cleanupPendingRequestStateForTest(
    requesterName, serviceName, firstRequestId);
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, firstRequestId));
  // 同 requester/service 的 R2 不受影响：pending 未被宽擦。
  BOOST_CHECK(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, secondRequestId));

  // R2 的真实 selection dispatch 执行成功（绑定被 claim 且匹配）。
  const auto selection =
    makeSelectionBuffer(secondRequestId, "provider-token-second");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName, providerName, serviceName, secondRequestId, selection);
  BOOST_CHECK_EQUAL(executions.load(), 1);
}

BOOST_AUTO_TEST_SUITE_END() // Spec182Registration

namespace {

using ndnsf::di::ExecutionLeaseRequestContext;
using ndnsf::di::ExecutionLeaseService;
using ndnsf::di::LeaseOperation;
using ndnsf::di::LeaseOperationRequest;
using ndnsf::di::LeaseOperationResponse;
using ndnsf::di::SharedExecutionLeaseState;

constexpr char HOST_PROVIDER_NAME[] = "/provider/Host";
constexpr char HOST_EPOCH[] = "host-epoch";
constexpr char MODEL_A_NAME[] = "/Inference/ModelA";
constexpr char MODEL_B_NAME[] = "/Inference/ModelB";

const ndn::Buffer PROOF_A{1, 2, 3};
const ndn::Buffer PROOF_B{4, 5, 6};

// One physical compute slot: both targets resolve to the same conflict key,
// so slot contention is visible across services of the same host.
auto
sameSlotResolver(const ndnsf::di::LeaseOperationRequest&,
                 const ndnsf::di::ExecutionLeaseRequestContext&)
{
  return std::vector<std::string>{"compute-slot:0"};
}

LeaseOperationResponse
leaseHandle(ExecutionLeaseService& service, const std::string& requester,
            const std::string& routeRequestId, const LeaseOperationRequest& request,
            uint64_t nowMs)
{
  ExecutionLeaseRequestContext context{requester, HOST_PROVIDER_NAME,
                                       ndnsf::di::EXECUTION_LEASE_SERVICE_NAME,
                                       routeRequestId};
  return ndnsf::di::decodeLeaseOperationResponse(
    service.handle(context, ndnsf::di::encodeLeaseOperationRequest(request), nowMs));
}

LeaseOperationRequest
prepareFor(const std::string& requestId, const std::string& planDigest,
           const std::string& idempotencyKey, const std::string& targetService,
           const ndn::Buffer& proof)
{
  LeaseOperationRequest request;
  request.operation = LeaseOperation::Prepare;
  request.requestId = requestId;
  request.planDigest = planDigest;
  request.idempotencyKey = idempotencyKey;
  request.targetServiceName = targetService;
  request.resourceBindingProof = proof;
  request.roles = {"/Backbone"};
  request.expiresAtMs = 100000;
  return request;
}

LeaseOperationRequest
leaseIdOperation(LeaseOperation operation, const std::string& leaseId,
                 const std::string& idempotencyKey, const std::string& targetService,
                 const std::string& providerEpoch, uint64_t expiresAtMs = 0)
{
  LeaseOperationRequest request;
  request.operation = operation;
  request.requestId = "route-holder";
  request.planDigest = "plan-holder";
  request.idempotencyKey = idempotencyKey;
  request.targetServiceName = targetService;
  request.leaseId = leaseId;
  request.providerEpoch = providerEpoch;
  request.expiresAtMs = expiresAtMs;
  return request;
}

std::shared_ptr<SharedExecutionLeaseState>
sharedHostLeaseState()
{
  return std::make_shared<SharedExecutionLeaseState>(HOST_EPOCH);
}

ExecutionLeaseService
makeModelAService(const std::shared_ptr<SharedExecutionLeaseState>& shared)
{
  return ExecutionLeaseService(HOST_PROVIDER_NAME, MODEL_A_NAME, sameSlotResolver, shared);
}

ExecutionLeaseService
makeModelBService(const std::shared_ptr<SharedExecutionLeaseState>& shared)
{
  return ExecutionLeaseService(HOST_PROVIDER_NAME, MODEL_B_NAME, sameSlotResolver, shared);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182SharedLease)

// 两个 targets 争同一槽只允许一个 Prepare 成功；host epoch 共享，任一 target
// 不能通过单独重置表绕过预留；槽释放后另一 target 的等待者续订成功。
BOOST_AUTO_TEST_CASE(Spec182SharedLeaseCrossServiceConflict)
{
  const auto shared = sharedHostLeaseState();
  auto serviceA = makeModelAService(shared);
  auto serviceB = makeModelBService(shared);

  // Same underlying host table and epoch across both targets.
  BOOST_REQUIRE_EQUAL(&serviceA.table(), &serviceB.table());
  BOOST_CHECK_EQUAL(serviceA.table().providerEpoch(), HOST_EPOCH);
  BOOST_CHECK_EQUAL(serviceB.table().providerEpoch(), HOST_EPOCH);

  // A reserves the only slot.
  const auto preparedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                     prepareFor("req-a1", "plan-a1", "prep-a1",
                                                MODEL_A_NAME, PROOF_A),
                                     1000);
  BOOST_REQUIRE(preparedA.status);
  BOOST_CHECK_EQUAL(preparedA.reasonCode, "OK");
  BOOST_CHECK_EQUAL(preparedA.providerEpoch, HOST_EPOCH);
  BOOST_REQUIRE_EQUAL(preparedA.conflictKeys.size(), 1);
  BOOST_CHECK_EQUAL(preparedA.conflictKeys.front(), "compute-slot:0");
  const std::string leaseA = preparedA.leaseId;

  // B wants the same slot through the shared table: waitlisted, not granted.
  const auto waitlistedB = leaseHandle(serviceB, "/user/two", "route-b1",
                                       prepareFor("req-b1", "plan-b1", "prep-b1",
                                                  MODEL_B_NAME, PROOF_B),
                                       1100);
  BOOST_REQUIRE(!waitlistedB.status);
  BOOST_CHECK_EQUAL(waitlistedB.reasonCode, "LEASE_CAPACITY_REJECTED");
  BOOST_CHECK_EQUAL(waitlistedB.retryAfterMs, 100);
  BOOST_CHECK(waitlistedB.leaseId.empty());

  // The host cannot double-book its own physical slot either; the probe joins
  // the shared waitlist behind B.
  const auto waitlistedA = leaseHandle(serviceA, "/user/one", "route-a2",
                                       prepareFor("req-a2", "plan-a2", "prep-a2",
                                                  MODEL_A_NAME, PROOF_A),
                                       1200);
  BOOST_CHECK(!waitlistedA.status);
  BOOST_CHECK_EQUAL(waitlistedA.reasonCode, "LEASE_CAPACITY_REJECTED");

  // A cleans up its own lease (Abort of a Prepared row).
  const auto abortedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                    leaseIdOperation(LeaseOperation::Abort, leaseA,
                                                     "abort-a1", MODEL_A_NAME, HOST_EPOCH),
                                    1300);
  BOOST_REQUIRE(abortedA.status);
  BOOST_CHECK_EQUAL(abortedA.reasonCode, "OK");

  // B's retry of the same request now succeeds on the freed slot.
  const auto preparedB = leaseHandle(serviceB, "/user/two", "route-b1",
                                     prepareFor("req-b1", "plan-b1", "prep-b1",
                                                MODEL_B_NAME, PROOF_B),
                                     1400);
  BOOST_REQUIRE(preparedB.status);
  BOOST_CHECK_EQUAL(preparedB.reasonCode, "OK");
  BOOST_CHECK_EQUAL(preparedB.providerEpoch, HOST_EPOCH);
  BOOST_CHECK_EQUAL(preparedB.leaseId, "host-epoch-lease-2");

  // Symmetric direction: B holds the slot (A's queued probe waits in FIFO
  // order), then B cleans up its own lease and A's retry is granted.
  const auto queuedA = leaseHandle(serviceA, "/user/one", "route-a2",
                                   prepareFor("req-a2", "plan-a2", "prep-a2",
                                              MODEL_A_NAME, PROOF_A),
                                   1500);
  BOOST_REQUIRE(!queuedA.status);
  BOOST_CHECK_EQUAL(queuedA.reasonCode, "LEASE_CAPACITY_REJECTED");
  const auto abortedB = leaseHandle(serviceB, "/user/two", "route-b1",
                                    leaseIdOperation(LeaseOperation::Abort,
                                                     preparedB.leaseId, "abort-b1",
                                                     MODEL_B_NAME, HOST_EPOCH),
                                    1600);
  BOOST_REQUIRE(abortedB.status);
  const auto preparedA2 = leaseHandle(serviceA, "/user/one", "route-a2",
                                      prepareFor("req-a2", "plan-a2", "prep-a2",
                                                 MODEL_A_NAME, PROOF_A),
                                      1700);
  BOOST_REQUIRE(preparedA2.status);
  BOOST_CHECK_EQUAL(preparedA2.leaseId, "host-epoch-lease-3");

  // The waitlist round trip is fully symmetric: B is now the queued party
  // (fresh request; replaying the aborted prep-b1 idempotency is refused by
  // Core's state revalidation, not by the waitlist).
  const auto queuedB = leaseHandle(serviceB, "/user/two", "route-b2",
                                   prepareFor("req-b2", "plan-b2", "prep-b2",
                                              MODEL_B_NAME, PROOF_B),
                                   1800);
  BOOST_REQUIRE(!queuedB.status);
  BOOST_CHECK_EQUAL(queuedB.reasonCode, "LEASE_CAPACITY_REJECTED");
  BOOST_CHECK_EQUAL(queuedB.retryAfterMs, 100);
}

// 另一 target 不能 Commit/Abort/Renew/Release 前者 lease；未知 lease 保持 Core
// 缺失处理；同一 target 上 Core 的 requester/epoch/重放验证不被 find 替代。
BOOST_AUTO_TEST_CASE(Spec182SharedLeaseTargetBinding)
{
  const auto shared = sharedHostLeaseState();
  auto serviceA = makeModelAService(shared);
  auto serviceB = makeModelBService(shared);

  const auto preparedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                     prepareFor("req-a1", "plan-a1", "prep-a1",
                                                MODEL_A_NAME, PROOF_A),
                                     1000);
  BOOST_REQUIRE(preparedA.status);
  const std::string leaseA = preparedA.leaseId;

  const auto committedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                      leaseIdOperation(LeaseOperation::Commit, leaseA,
                                                       "com-a1", MODEL_A_NAME, HOST_EPOCH),
                                      1100);
  BOOST_REQUIRE(committedA.status);
  BOOST_CHECK_EQUAL(committedA.state, "COMMITTED");

  // Same-target idempotent replay of the commit is still served while the
  // row is in the recorded state (Core replay validation untouched).
  const auto replayedCommit = leaseHandle(serviceA, "/user/one", "route-a1",
                                          leaseIdOperation(LeaseOperation::Commit, leaseA,
                                                           "com-a1", MODEL_A_NAME,
                                                           HOST_EPOCH),
                                          1150);
  BOOST_REQUIRE(replayedCommit.status);
  BOOST_CHECK_EQUAL(replayedCommit.reasonCode, "OK");

  // Production activation path (table accessor): Committed -> Executing.
  ndn_service_framework::ExecutionLeaseBinding binding;
  binding.requesterName = "/user/one";
  binding.requestId = "req-a1";
  binding.serviceName = MODEL_A_NAME;
  binding.planDigest = "plan-a1";
  binding.resourceBindingSchema = "ndnsf-di-binding-v1";
  binding.resourceBindingProof = PROOF_A;
  const auto activated = serviceA.table().validateAndActivate(
    leaseA, HOST_EPOCH, binding, "act-a1", 1200, 100000);
  BOOST_REQUIRE(activated.status);

  // The true lease owner itself is refused through the other target's route:
  // the row is pinned to ModelA before any requester/epoch/state check.
  for (LeaseOperation operation : {LeaseOperation::Commit, LeaseOperation::Abort,
                                   LeaseOperation::Renew, LeaseOperation::Release}) {
    const auto crossTarget = leaseHandle(
      serviceB, "/user/one", "route-b1",
      leaseIdOperation(operation, leaseA, "x-" + std::to_string(static_cast<int>(operation)),
                       MODEL_B_NAME, HOST_EPOCH, 50000),
      1300);
    BOOST_CHECK(!crossTarget.status);
    BOOST_CHECK_EQUAL(crossTarget.reasonCode, "LEASE_SERVICE_MISMATCH");
    // No lease detail of the other service's row leaks through this route.
    BOOST_CHECK(crossTarget.leaseId.empty());
    BOOST_CHECK(crossTarget.state.empty());
    BOOST_CHECK(crossTarget.conflictKeys.empty());
  }

  // The cross-target attempts never touched the row.
  const auto preserved = serviceB.table().find(leaseA);
  BOOST_REQUIRE(preserved);
  BOOST_CHECK(preserved->state == ndn_service_framework::ExecutionLeaseState::Executing);

  // Unknown lease keeps Core's missing handling, not a target verdict.
  const auto unknown = leaseHandle(serviceB, "/user/two", "route-b1",
                                   leaseIdOperation(LeaseOperation::Release,
                                                    "host-epoch-lease-404", "rel-x",
                                                    MODEL_B_NAME, HOST_EPOCH),
                                   1300);
  BOOST_REQUIRE(!unknown.status);
  BOOST_CHECK_EQUAL(unknown.reasonCode, "LEASE_NOT_FOUND");

  // Same target still delegates authorization to Core.
  const auto wrongRequester = leaseHandle(serviceA, "/user/nine", "route-a9",
                                          leaseIdOperation(LeaseOperation::Abort, leaseA,
                                                           "abort-z", MODEL_A_NAME,
                                                           HOST_EPOCH),
                                          1300);
  BOOST_REQUIRE(!wrongRequester.status);
  BOOST_CHECK_EQUAL(wrongRequester.reasonCode, "LEASE_REQUESTER_MISMATCH");
  const auto staleEpoch = leaseHandle(serviceA, "/user/one", "route-a1",
                                      leaseIdOperation(LeaseOperation::Commit, leaseA,
                                                       "com-stale", MODEL_A_NAME,
                                                       "epoch-stale"),
                                      1300);
  BOOST_REQUIRE(!staleEpoch.status);
  BOOST_CHECK_EQUAL(staleEpoch.reasonCode, "LEASE_STALE_EPOCH");

  // Legitimate cleanup by the owning target at the executing lease's safe
  // release point; the other target is still refused on the Released row.
  const auto releasedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                     leaseIdOperation(LeaseOperation::Release, leaseA,
                                                      "rel-a1", MODEL_A_NAME, HOST_EPOCH),
                                     1400);
  BOOST_REQUIRE(releasedA.status);
  BOOST_CHECK_EQUAL(releasedA.state, "RELEASED");

  // Same-target idempotent replay of the release is still served (row kept in
  // the recorded Released state) — Core replay tombstones are not bypassed by
  // the target pre-check.
  const auto replayedRelease = leaseHandle(serviceA, "/user/one", "route-a1",
                                           leaseIdOperation(LeaseOperation::Release, leaseA,
                                                            "rel-a1", MODEL_A_NAME,
                                                            HOST_EPOCH),
                                           1500);
  BOOST_REQUIRE(replayedRelease.status);
  BOOST_CHECK_EQUAL(replayedRelease.reasonCode, "OK");

  const auto stillBound = leaseHandle(serviceB, "/user/one", "route-b1",
                                      leaseIdOperation(LeaseOperation::Commit, leaseA,
                                                       "com-a1", MODEL_B_NAME, HOST_EPOCH),
                                      1600);
  BOOST_CHECK(!stillBound.status);
  BOOST_CHECK_EQUAL(stillBound.reasonCode, "LEASE_SERVICE_MISMATCH");
}

// 关闭 target A 的 service 实例不提前释放其执行中槽：shared state 保留该行，
// B 无法操作或盗用该槽；重新 serve A 也不能继承；只有 A target 自身的 owner
// 流（Abort/Release）能在安全点清理旧 lease，之后 B 继续 Prepare/执行。
BOOST_AUTO_TEST_CASE(Spec182ClosingServicePreservesSharedLeaseOwner)
{
  const auto shared = sharedHostLeaseState();
  auto serviceB = makeModelBService(shared);
  const std::string leaseA = [&] {
    auto serviceA = makeModelAService(shared);
    const auto preparedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                       prepareFor("req-a1", "plan-a1", "prep-a1",
                                                  MODEL_A_NAME, PROOF_A),
                                       1000);
    BOOST_REQUIRE(preparedA.status);
    const auto committedA = leaseHandle(serviceA, "/user/one", "route-a1",
                                        leaseIdOperation(LeaseOperation::Commit,
                                                         preparedA.leaseId, "com-a1",
                                                         MODEL_A_NAME, HOST_EPOCH),
                                        1100);
    BOOST_REQUIRE(committedA.status);
    ndn_service_framework::ExecutionLeaseBinding binding;
    binding.requesterName = "/user/one";
    binding.requestId = "req-a1";
    binding.serviceName = MODEL_A_NAME;
    binding.planDigest = "plan-a1";
    binding.resourceBindingSchema = "ndnsf-di-binding-v1";
    binding.resourceBindingProof = PROOF_A;
    const auto activated = serviceA.table().validateAndActivate(
      preparedA.leaseId, HOST_EPOCH, binding, "act-a1", 1200, 100000);
    BOOST_REQUIRE(activated.status);
    return preparedA.leaseId;
  }(); // serviceA closed here: its instance is gone while its lease executes.

  // Closing the instance did not free the executing row from the shared state.
  const auto preserved = serviceB.table().find(leaseA);
  BOOST_REQUIRE(preserved);
  BOOST_CHECK(preserved->state == ndn_service_framework::ExecutionLeaseState::Executing);
  BOOST_CHECK_EQUAL(serviceB.table().providerEpoch(), HOST_EPOCH);

  // B still cannot take or clean the closed target's executing slot.
  const auto waitlistedB = leaseHandle(serviceB, "/user/two", "route-b1",
                                       prepareFor("req-b1", "plan-b1", "prep-b1",
                                                  MODEL_B_NAME, PROOF_B),
                                       1300);
  BOOST_REQUIRE(!waitlistedB.status);
  BOOST_CHECK_EQUAL(waitlistedB.reasonCode, "LEASE_CAPACITY_REJECTED");
  for (LeaseOperation operation : {LeaseOperation::Abort, LeaseOperation::Release}) {
    const auto crossTarget = leaseHandle(
      serviceB, "/user/one", "route-b1",
      leaseIdOperation(operation, leaseA, "x-" + std::to_string(static_cast<int>(operation)),
                       MODEL_B_NAME, HOST_EPOCH),
      1300);
    BOOST_REQUIRE(!crossTarget.status);
    BOOST_CHECK_EQUAL(crossTarget.reasonCode, "LEASE_SERVICE_MISMATCH");
  }

  // Re-serving A over the same shared state cannot steal the old executing
  // slot either.
  auto serviceA2 = makeModelAService(shared);
  const auto waitlistedA2 = leaseHandle(serviceA2, "/user/two", "route-a2",
                                        prepareFor("req-a2", "plan-a2", "prep-a2",
                                                   MODEL_A_NAME, PROOF_B),
                                        1400);
  BOOST_REQUIRE(!waitlistedA2.status);
  BOOST_CHECK_EQUAL(waitlistedA2.reasonCode, "LEASE_CAPACITY_REJECTED");

  // The A-target owner flow still routes cleanup of the old record: the
  // executing lease returns at its safe release point.
  const auto releasedA = leaseHandle(serviceA2, "/user/one", "route-a1",
                                     leaseIdOperation(LeaseOperation::Release, leaseA,
                                                      "rel-a1", MODEL_A_NAME, HOST_EPOCH),
                                     1500);
  BOOST_REQUIRE(releasedA.status);
  BOOST_CHECK_EQUAL(releasedA.state, "RELEASED");

  // The freed slot serves B normally: closing A never blocks B's execution.
  const auto preparedB = leaseHandle(serviceB, "/user/two", "route-b1",
                                     prepareFor("req-b1", "plan-b1", "prep-b1",
                                                MODEL_B_NAME, PROOF_B),
                                     1600);
  BOOST_REQUIRE(preparedB.status);
  BOOST_CHECK_EQUAL(preparedB.reasonCode, "OK");
  BOOST_CHECK_EQUAL(preparedB.providerEpoch, HOST_EPOCH);
}

BOOST_AUTO_TEST_SUITE_END() // Spec182SharedLease

namespace {

using ndnsf::di::NativeAdapterRegistry;
using ndnsf::di::NativeInferenceProvider;
using ndnsf::di::NativeProviderHandlerConfig;
using ndnsf::di::NativeServiceDefinition;
using ndnsf::di::NativeServiceRegistration;

constexpr const char* NATIVE_HOST_PROVIDER_NAME = HOST_PROVIDER_NAME;
constexpr char NATIVE_HOST_BOOT_ID[] = "host-boot-epoch";
constexpr char HOST_SERVICE_A[] = "/Inference/Spec182HostA";
constexpr char HOST_SERVICE_B[] = "/Inference/Spec182HostB";

// host.serve 组装 runtime 时只要求 runnerFactory 非空；生命周期 suite 从不
// dispatch 到 handler（collab 真实执行留 T016），factory 一律不得被调用。
class UnusedRunnerFactory final : public ndnsf::di::NativeModelRunnerFactory
{
public:
  std::shared_ptr<ndnsf::di::NativeModelRunner>
  create(const ndnsf::di::NativeModelRunnerSpec&) const override
  {
    throw std::logic_error("runner factory must not be called by host "
                           "lifecycle tests");
  }
};

NativeProviderHandlerConfig
makeHostConfig(const std::string& serviceName,
               std::size_t workerCount = 1,
               bool leaseOn = false)
{
  NativeProviderHandlerConfig config;
  config.plan.executionPolicy = "DATA_DRIVEN_V2";
  config.executionPolicy = "DATA_DRIVEN_V2";
  config.localProviderName = NATIVE_HOST_PROVIDER_NAME;
  config.providerBootId = NATIVE_HOST_BOOT_ID;
  config.workerCount = workerCount;
  config.runnerFactory = std::make_shared<UnusedRunnerFactory>();
  if (leaseOn) {
    // Host injects the shared lease table; the caller only binds the target.
    config.executionLeaseTable = nullptr;
    config.executionLeaseTargetService = serviceName;
  }
  return config;
}

NativeServiceDefinition
makeHostService(const std::string& serviceName,
                ServiceProvider::AckStrategyHandler ackHandler)
{
  NativeServiceDefinition service;
  service.serviceName = serviceName;
  service.allowedRoles = {"/Backbone"};
  service.ackHandler = std::move(ackHandler);
  return service;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182ProviderHost)

BOOST_AUTO_TEST_CASE(Spec212EmptyServiceWithdrawalClosesOnlyItsRegistration)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-empty-withdrawal",
                                   "tpm-memory:spec212-empty-withdrawal");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-empty-withdrawal"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-empty-withdrawal"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  std::atomic<unsigned> ownerResolverCalls{0};
  auto firstService = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  firstService.withdrawalPersistencePolicyId = "spec212-empty-service-policy";
  firstService.handoffOwnerContextResolver = [&ownerResolverCalls](
      const ndn::Name&, const ndn::Name&, const std::string&)
      -> ndnsf::di::NativeConversationHandoffOwnerContextV1 {
    ++ownerResolverCalls;
    throw std::runtime_error("empty withdrawal must not resolve an Owner");
  };
  auto first = host->serve(
    firstService,
    makeHostConfig(HOST_SERVICE_A));
  auto sibling = host->serve(
    makeHostService(HOST_SERVICE_B, makeAcceptingAckHandler()),
    makeHostConfig(HOST_SERVICE_B));

  ndnsf::di::NativeWithdrawalOptionsV1 options;
  options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  options.drainDeadline = options.deadline + std::chrono::seconds(1);
  options.maxConversations = 1;
  options.maxTotalBytes = 1024;
  options.persistencePolicyId = "spec212-empty-service-policy";
  BOOST_CHECK_THROW(sibling.withdraw(options), std::logic_error);
  BOOST_CHECK(!sibling.closed());
  auto invalid = options;
  invalid.drainDeadline = invalid.deadline - std::chrono::milliseconds(1);
  BOOST_CHECK_THROW(first.withdraw(invalid), std::invalid_argument);
  BOOST_CHECK(!first.closed());
  auto wrongPolicy = options;
  wrongPolicy.persistencePolicyId = "different-policy";
  BOOST_CHECK_THROW(first.withdraw(wrongPolicy), std::invalid_argument);
  BOOST_CHECK(!first.closed());
  auto withdrawal = first.withdraw(options);
  BOOST_REQUIRE(withdrawal.valid());
  BOOST_CHECK(!withdrawal.operationId().empty());
  const auto result = withdrawal.result(std::chrono::seconds(2));
  BOOST_CHECK(result.closed);
  BOOST_CHECK(!result.forced);
  BOOST_CHECK_EQUAL(result.reason, "EMPTY");
  BOOST_CHECK(result.conversationResults.empty());
  BOOST_CHECK_EQUAL(ownerResolverCalls.load(), 0U);
  BOOST_CHECK_EQUAL(result.withdrawalId, withdrawal.operationId());
  BOOST_CHECK(withdrawal.status() == ndnsf::di::NativeWithdrawalStatusV1::Closed);
  BOOST_CHECK(!withdrawal.cancel());
  BOOST_CHECK(first.closed());
  BOOST_CHECK(!sibling.closed());
  BOOST_CHECK(first.drain(std::chrono::seconds(2)));
  BOOST_CHECK(sibling.drain(std::chrono::seconds(2)));
  face.shutdown();
  face.processEvents(ndn::time::milliseconds(20));
  host.reset();
  provider.reset();
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalClosesTwoEmptyServices)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-provider-withdrawal",
                                   "tpm-memory:spec212-provider-withdrawal");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-provider-withdrawal"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-provider-withdrawal"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  std::atomic<unsigned> ownerResolverCalls{0};
  const auto makeWithdrawable = [&ownerResolverCalls](const std::string& name) {
    auto service = makeHostService(name, makeAcceptingAckHandler());
    service.withdrawalPersistencePolicyId = "spec212-provider-policy";
    service.handoffOwnerContextResolver = [&ownerResolverCalls](
        const ndn::Name&, const ndn::Name&, const std::string&)
        -> ndnsf::di::NativeConversationHandoffOwnerContextV1 {
      ++ownerResolverCalls;
      throw std::runtime_error("empty Provider withdrawal must not resolve an Owner");
    };
    return service;
  };
  ndnsf::di::NativeProviderCollaborationRuntime firstRuntime;
  auto firstDefinition = makeWithdrawable(HOST_SERVICE_A);
  firstDefinition.runtimeObserver = [&firstRuntime](
      const ndnsf::di::NativeProviderCollaborationRuntime& runtime) {
    firstRuntime = runtime;
  };
  auto first = host->serve(firstDefinition, makeHostConfig(HOST_SERVICE_A));
  auto second = host->serve(makeWithdrawable(HOST_SERVICE_B),
                            makeHostConfig(HOST_SERVICE_B));

  ndnsf::di::NativeWithdrawalOptionsV1 options;
  options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  options.drainDeadline = options.deadline + std::chrono::seconds(2);
  options.maxConversations = 2;
  options.maxTotalBytes = 1024;
  options.persistencePolicyId = "spec212-provider-policy";
  auto competingFence = firstRuntime.beginConversationSaveFence(
    "/spec212/owner", HOST_SERVICE_A, "conversation-busy-0001");
  BOOST_REQUIRE(competingFence);
  BOOST_CHECK_THROW(host->withdraw(options), std::logic_error);
  BOOST_CHECK(!first.closed());
  BOOST_CHECK(!second.closed());
  competingFence.reset();
  auto withdrawal = host->withdraw(options);
  BOOST_REQUIRE(withdrawal.valid());
  auto duplicate = host->withdraw(options);
  BOOST_CHECK_EQUAL(duplicate.operationId(), withdrawal.operationId());
  auto changed = options;
  ++changed.maxTotalBytes;
  BOOST_CHECK_THROW(host->withdraw(changed), std::logic_error);
  const auto result = withdrawal.result(std::chrono::seconds(5));
  BOOST_CHECK(result.scope == ndnsf::di::NativeWithdrawalScopeV1::Provider);
  BOOST_CHECK(result.closed);
  BOOST_CHECK(!result.forced);
  BOOST_CHECK_EQUAL(result.reason, "EMPTY");
  BOOST_CHECK(result.conversationResults.empty());
  BOOST_CHECK_EQUAL(result.withdrawalId, withdrawal.operationId());
  BOOST_CHECK_EQUAL(ownerResolverCalls.load(), 0U);
  BOOST_CHECK(first.closed());
  BOOST_CHECK(second.closed());
  BOOST_CHECK_THROW(host->serve(makeWithdrawable(HOST_SERVICE_A),
    makeHostConfig(HOST_SERVICE_A)), std::logic_error);
  BOOST_CHECK(first.drain(std::chrono::seconds(2)));
  BOOST_CHECK(second.drain(std::chrono::seconds(2)));
  BOOST_CHECK(host->drain(std::chrono::seconds(2)));
  face.shutdown();
  face.processEvents(ndn::time::milliseconds(20));
  host.reset();
  provider.reset();
}

BOOST_AUTO_TEST_CASE(Spec212V2BootstrapUsesHandoffAckPath)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-v2-bootstrap",
                                   "tpm-memory:spec212-v2-bootstrap");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName(HOST_PROVIDER_NAME);
  const ndn::Name serviceName(HOST_SERVICE_A);
  const ndn::Name requesterName("/spec212/user-v2-bootstrap");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-v2-bootstrap"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-v2-bootstrap"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->applyPermissionResponse(makePermissionResponse(
    providerName, tlv::ProviderPermission, providerName, serviceName));
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  unsigned modelAckCalls = 0;
  auto service = makeHostService(HOST_SERVICE_A,
    [&modelAckCalls](const RequestMessage&) {
      ++modelAckCalls;
      ServiceProvider::AckDecision decision;
      decision.status = false;
      decision.message = "model ACK must not inspect handoff bootstrap";
      return decision;
    });
  service.handoffOwnerContextResolver = [] (const ndn::Name&, const ndn::Name&,
                                           const std::string&) {
    return ndnsf::di::NativeConversationHandoffOwnerContextV1{};
  };
  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));
  auto request = makeRequestMessageWithUserToken("", "spec212-v2-bootstrap-token");
  request.setRequestMode(tlv::TargetedBootstrapRequest);
  request.setTargetProvider(providerName);
  RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  capabilities.setField("NativeConversationHandoffV2", "required");
  request.setRequestCapabilities(capabilities);
  const ndn::Name requestId("/v2-bootstrap-request");
  BOOST_CHECK(!provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId));
  const auto wire = request.WireEncode();
  injectRequest(*provider, requesterName, serviceName, requestId,
                ndn::Buffer(wire.data(), wire.data() + wire.size()));
  BOOST_CHECK(provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId));
  BOOST_CHECK_EQUAL(modelAckCalls, 0U);

  ndnsf::di::NativeConversationHandoffControlV2 control;
  control.control.action = ndnsf::di::NativeConversationHandoffActionV1::Begin;
  control.control.operationId = "spec212-v2-bootstrap-operation";
  control.control.conversationId = "conversation-v2-bootstrap";
  control.control.checkpointDigest = "sha256:" + std::string(64, 'a');
  control.control.destinationRoleMapDigest = "sha256:" + std::string(64, 'b');
  control.control.deadlineMs = 1'800'000'000'000;
  control.sourceProvidersByRole = {{"/Backbone", HOST_PROVIDER_NAME}};
  ndnsf::di::NativeJson sourceRoles = ndnsf::di::NativeJson::array();
  sourceRoles.push_back(ndnsf::di::NativeJson::array(
    {"/Backbone", HOST_PROVIDER_NAME}));
  control.control.sourceRoleMapDigest = ndnsf::di::nativePlanningDigest(
    ndnsf::di::nativeCanonicalJson(sourceRoles));
  control.trigger = ndnsf::di::NativeConversationSaveTriggerV2::UserSwitch;
  const auto controlWire = ndnsf::di::nativeConversationCanonicalJson(
    ndnsf::di::nativeConversationHandoffControlV2ToJson(control));
  auto payloadRequest = makeRequestMessageWithUserToken(
    controlWire, "spec212-v2-bootstrap-payload-token");
  payloadRequest.setRequestMode(tlv::TargetedBootstrapRequest);
  payloadRequest.setTargetProvider(providerName);
  payloadRequest.setRequestCapabilities(capabilities);
  const ndn::Name payloadRequestId("/v2-bootstrap-payload-request");
  BOOST_CHECK(!provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, payloadRequestId));
  const auto payloadWire = payloadRequest.WireEncode();
  injectRequest(*provider, requesterName, serviceName, payloadRequestId,
                ndn::Buffer(payloadWire.data(), payloadWire.data() + payloadWire.size()));
  BOOST_CHECK(provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, payloadRequestId));
  BOOST_CHECK_EQUAL(modelAckCalls, 0U);

  ndnsf::di::NativeWithdrawalQueryV1 query;
  query.conversationId = "conversation-v2-bootstrap";
  query.checkpointDigest = control.control.checkpointDigest;
  query.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60 * 1000;
  const auto queryWire = ndnsf::di::nativeConversationCanonicalJson(
    ndnsf::di::nativeWithdrawalQueryV1ToJson(query));
  auto queryRequest = makeRequestMessageWithUserToken(
    queryWire, "spec212-withdrawal-query-bootstrap-token");
  queryRequest.setRequestMode(tlv::TargetedBootstrapRequest);
  queryRequest.setTargetProvider(providerName);
  RequestCapabilities queryCapabilities;
  queryCapabilities.setField("RequestScopedConfidentialityV1", "required");
  queryRequest.setRequestCapabilities(queryCapabilities);
  const ndn::Name queryRequestId("/withdrawal-query-bootstrap-request");
  BOOST_CHECK(!provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, queryRequestId));
  const auto queryRequestWire = queryRequest.WireEncode();
  injectRequest(*provider, requesterName, serviceName, queryRequestId,
                ndn::Buffer(queryRequestWire.data(),
                            queryRequestWire.data() + queryRequestWire.size()));
  BOOST_CHECK(provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, queryRequestId));
  BOOST_CHECK_EQUAL(modelAckCalls, 0U);

  registration.close();
  BOOST_CHECK(registration.drain(std::chrono::seconds(2)));
  face.shutdown();
  face.processEvents(ndn::time::milliseconds(20));
  host.reset();
  provider.reset();
}

BOOST_AUTO_TEST_CASE(Spec212ProviderSignsOnlyOwnWithdrawalIntentData)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-provider-intent",
                                   "tpm-memory:spec212-provider-intent");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName(HOST_PROVIDER_NAME);
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-intent"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-intent"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->useSigningKeyChainForTest(keyChain);

  ndnsf::di::NativeWithdrawalIntentV1 intent;
  intent.withdrawalId = "withdrawal-001";
  intent.ownerIdentity = "/spec212/user";
  intent.serviceName = HOST_SERVICE_A;
  intent.providerBootId = "boot-a";
  intent.registrationGeneration = 17;
  intent.deadlineMs = 1'800'000'000'000;
  intent.conversationId = "conversation-a-0001";
  intent.checkpointDigest = "sha256:" + std::string(64, 'a');
  intent.sourceRoleMapDigest = "sha256:" + std::string(64, 'b');
  intent.localRoles = {"/Stage/0"};
  intent.policyDigest = "sha256:" + std::string(64, 'c');
  auto proof = ndnsf::di::nativeSignWithdrawalIntentProofV1(
    providerName, intent, [provider](ndn::Data& packet) {
      provider->signProducerData(packet);
    });
  const auto& encoded = proof.wireEncode();
  const ndn::Buffer original(encoded.data(), encoded.data() + encoded.size());
  BOOST_CHECK_NO_THROW(ndnsf::di::nativeValidateWithdrawalIntentProofV1(
    original, providerName, intent, intent.deadlineMs - 1,
    [&providerCert](const ndn::Data& packet, const ndn::Name& expected) {
      return expected == providerCert.getIdentity() &&
        ndn::security::verifySignature(packet, providerCert);
    }));

  ndn::Data foreign(ndn::Name("/spec212/foreign/withdrawal-intent/v1/id/digest"));
  BOOST_CHECK_THROW(provider->signProducerData(foreign), std::invalid_argument);
  ndn::Data prefixOnly(providerName);
  BOOST_CHECK_THROW(provider->signProducerData(prefixOnly), std::invalid_argument);
  provider.reset();
  face.shutdown();
  face.processEvents(ndn::time::milliseconds(10));
}

BOOST_AUTO_TEST_CASE(Spec212CoreDrainWaiterTracksAdmittedCallback)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-core-drain-waiter",
                                   "tpm-memory:spec212-core-drain-waiter");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-core-drain-waiter"));
  LocalServiceProvider provider(
    face, ndn::Name("/spec212/group-core-drain-waiter"), providerCert, aaCert,
    "examples/trust-any.conf");
  const ndn::Name serviceName(HOST_SERVICE_A);
  auto registration = provider.addScopedTargetedService(
    serviceName,
    [] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
        const ndn::Name&, const RequestMessage&,
        ServiceProvider::AsyncTargetedCompletion) {});
  auto waitForCallbacks = registration.makeTargetedHandlerDrainWaiter();

  // Hold the exact Core admission state without entering the native wrapper.
  auto releaseAdmission = ServiceProviderTestAccess::beginTargetedHandler(
    provider, serviceName);
  BOOST_REQUIRE(releaseAdmission);
  registration.close();
  auto draining = std::async(std::launch::async, [&] {
    return waitForCallbacks(std::chrono::seconds(2));
  });
  const bool waitedForAdmission =
    draining.wait_for(std::chrono::milliseconds(50)) ==
      std::future_status::timeout;
  releaseAdmission();

  BOOST_CHECK(waitedForAdmission);
  BOOST_CHECK(draining.get());
  BOOST_CHECK(!ServiceProviderTestAccess::beginTargetedHandler(
    provider, serviceName));
}

BOOST_AUTO_TEST_CASE(Spec212HandoffRegistrationDrainWaitsForOwnerResolver)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-handoff-drain",
                                   "tpm-memory:spec212-handoff-drain");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-handoff-drain"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-handoff-drain"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());

  std::mutex resolverMutex;
  std::condition_variable resolverChanged;
  bool resolverEntered = false;
  bool resolverReleased = false;
  std::atomic<unsigned> resolverCalls{0};
  auto service = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  service.handoffOwnerContextResolver = [&](const ndn::Name&,
                                             const ndn::Name&,
                                             const std::string&)
      -> ndnsf::di::NativeConversationHandoffOwnerContextV1 {
    resolverCalls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(resolverMutex);
    resolverEntered = true;
    resolverChanged.notify_all();
    resolverChanged.wait(lock, [&] { return resolverReleased; });
    return {};
  };
  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));

  ndnsf::di::NativeConversationHandoffControlV1 control;
  control.operationId = "spec212-drain-operation-0001";
  control.conversationId = "spec212-drain-conversation-0001";
  control.checkpointDigest = "sha256:" + std::string(64, '6');
  control.sourceRoleMapDigest = "sha256:" + std::string(64, '7');
  control.destinationRoleMapDigest = "sha256:" + std::string(64, '8');
  control.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) +
    5 * 60 * 1000;
  const auto makeRequest = [](const auto& value) {
    const auto wire = ndnsf::di::nativeConversationCanonicalJson(
      ndnsf::di::nativeConversationHandoffControlV1ToJson(value));
    ndn::Buffer payload(wire.begin(), wire.end());
    RequestMessage result;
    result.setPayload(payload, payload.size());
    return result;
  };
  const auto request = makeRequest(control);

  bool completionCalled = false;
  bool responseStatus = true;
  auto invocation = std::async(std::launch::async, [&] {
    return provider->invokeAsyncTargetedHandlerForTest(
      ndn::Name(HOST_SERVICE_A), ndn::Name("/spec212/user"),
      ndn::Name("/spec212/handoff/1"), request,
      [&](ResponseMessage response) {
        responseStatus = response.getStatus();
        completionCalled = true;
      });
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(resolverMutex);
    entered = resolverChanged.wait_for(lock, std::chrono::seconds(2),
      [&] { return resolverEntered; });
  }
  if (!entered) {
    {
      std::lock_guard<std::mutex> lock(resolverMutex);
      resolverReleased = true;
    }
    resolverChanged.notify_all();
    BOOST_CHECK_MESSAGE(false, "handoff Owner resolver was not entered");
    (void)invocation.get();
    return;
  }

  bool duplicateCompleted = false;
  bool duplicateStatus = false;
  BOOST_CHECK(provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ndn::Name("/spec212/user"),
    ndn::Name("/spec212/handoff/duplicate"), makeRequest(control),
    [&](ResponseMessage response) {
      duplicateStatus = response.getStatus();
      duplicateCompleted = true;
    }));
  BOOST_CHECK(duplicateCompleted);
  BOOST_CHECK(duplicateStatus);
  BOOST_CHECK_EQUAL(resolverCalls.load(std::memory_order_relaxed), 1U);

  auto conflictingControl = control;
  conflictingControl.destinationRoleMapDigest =
    "sha256:" + std::string(64, '9');
  bool conflictingRetryCompleted = false;
  bool conflictingRetryStatus = true;
  BOOST_CHECK(provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ndn::Name("/spec212/user"),
    ndn::Name("/spec212/handoff/conflict"), makeRequest(conflictingControl),
    [&](ResponseMessage response) {
      conflictingRetryStatus = response.getStatus();
      conflictingRetryCompleted = true;
    }));
  BOOST_CHECK(conflictingRetryCompleted);
  BOOST_CHECK(!conflictingRetryStatus);
  BOOST_CHECK_EQUAL(resolverCalls.load(std::memory_order_relaxed), 1U);

  // Close intake on this thread first. The test-only Core invoker shares the
  // same RegistrationState admission lease as the production callback path.
  registration.close();
  auto draining = std::async(std::launch::async, [&] {
    return registration.drain(std::chrono::seconds(2));
  });
  BOOST_CHECK(draining.wait_for(std::chrono::milliseconds(50)) ==
              std::future_status::timeout);
  {
    std::lock_guard<std::mutex> lock(resolverMutex);
    resolverReleased = true;
  }
  resolverChanged.notify_all();

  BOOST_CHECK(invocation.get());
  BOOST_CHECK(draining.get());
  BOOST_CHECK(completionCalled);
  BOOST_CHECK(!responseStatus);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerContextPreflightValidatesWithoutPublishing)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-context-preflight",
                                 "tpm-memory:spec212-context-preflight");
  const ndn::Name owner("/spec212/preflight-owner");
  const ndn::Name service(HOST_SERVICE_A);
  auto store = std::make_shared<Spec212CountingPacketStore>();
  unsigned grantCalls = 0;
  ndnsf::di::NativeConversationHandoffOwnerContextV1 context;
  context.ownerEncryptionCertificate = makeRsaIdentity(keyChain, owner);
  context.durableStorageOwner.ownerIdentity = owner.toUri(ndn::name::UriFormat::ALTERNATE);
  context.durableStorageOwner.resolveGrant = [&grantCalls](const ndn::Name&,
      const std::string&, const std::function<void()>&)
      -> ndn_service_framework::DurableStorageGrantSelector {
    ++grantCalls;
    throw std::runtime_error("preflight must not request a grant");
  };
  context.originalPacketStore = store;
  context.saveBatchLimits = {1, 1024};
  context.portableLimits = {{16U << 20, 20U << 20, {}}, 1U << 20, 24U << 20};
  context.protectedCheckpointLimits = {25U << 20, 32U << 20, {}};
  context.policy.enabled = true;
  context.policy.maxConcurrentEvents = 1;
  context.policy.maxRolesPerEvent = 1;
  context.policy.maxConcurrentRoleSlots = 1;
  context.policy.maxPacketCountPerRole = 1;
  context.policy.maxPacketSetWireBytesPerRole = 8192;
  context.policy.maxAggregateBytes = 8192;
  context.policy.maxScratchBytesPerEvent = 16384;
  context.policy.scratchQuotaBytes = 16384;
  context.policy.maxSaveDurationMs = 60000;
  context.policy.scratchRoot = "/spec212/preflight-scratch";
  context.repoTargetByRole.emplace("/Backbone",
    ndnsf::di::NativeDurableStorageGrantRepoTargetV1{
      "/spec212/repo", "/spec212/repo-provider", "/spec212/repo/service"});
  const auto original = context;
  const ndnsf::di::NativeConversationHandoffOwnerContextResolverV1 resolver =
    [&context](const ndn::Name&, const ndn::Name&, const std::string&) { return context; };
  const auto check = [&] {
    return ndnsf::di::resolveNativeConversationHandoffOwnerContextV1(
      resolver, owner, service, "startup-context-check");
  };
  const auto validated = check();
  BOOST_CHECK(validated.policy.effectiveDigest.find("sha256:") == 0);
  BOOST_CHECK_EQUAL(validated.portableLimits.maxMetadataBytes, 1U << 20);
  BOOST_CHECK_EQUAL(validated.portableLimits.maxCheckpointBytes, 24U << 20);
  BOOST_CHECK_EQUAL(validated.protectedCheckpointLimits.maxPlaintextBytes, 25U << 20);
  context.portableLimits.maxCheckpointBytes -= 1;
  BOOST_CHECK_NE(check().policy.effectiveDigest, validated.policy.effectiveDigest);
  context = original;
  context.portableLimits.maxMetadataBytes += 1;
  BOOST_CHECK_THROW(check(), std::invalid_argument);
  context = original;
  BOOST_CHECK_EXCEPTION(ndnsf::di::resolveNativeConversationHandoffOwnerContextV1(
    resolver, ndn::Name(), service, "startup-context-check"), std::invalid_argument,
    [](const auto& error) {
      return std::string(error.what()) == "HANDOFF_AUTHENTICATED_OWNER_SCOPE_INVALID";
    });
  for (const auto& invalid : std::vector<std::pair<const char*, std::function<void()>>>{
         {"HANDOFF_OWNER_CONTEXT_BINDING_INVALID", [&] {
           context.durableStorageOwner.ownerIdentity = "/spec212/wrong-owner"; }},
         {"HANDOFF_OWNER_CONTEXT_BINDING_INVALID", [&] { context.originalPacketStore.reset(); }},
         {"HANDOFF_OWNER_CONTEXT_BINDING_INVALID", [&] { context.saveBatchLimits.maxMetadataBytes = 0; }},
         {"HANDOFF_OWNER_CONTEXT_BINDING_INVALID", [&] { context.portableLimits.portable.maxEncodedBytes = 0; }},
         {"HANDOFF_OWNER_CONTEXT_BINDING_INVALID", [&] { context.portableLimits.maxCheckpointBytes = 26U << 20; }},
         {"HANDOFF_EFFECTIVE_POLICY_INVALID", [&] { context.policy.enabled = false; }},
         {"HANDOFF_EFFECTIVE_POLICY_INVALID", [&] { context.repoTargetByRole.clear(); }}}) {
    context = original;
    invalid.second();
    BOOST_CHECK_EXCEPTION(check(), std::invalid_argument, [&](const auto& error) {
      return std::string(error.what()) == invalid.first;
    });
  }
  BOOST_CHECK_EQUAL(grantCalls, 0U);
  BOOST_CHECK_EQUAL(store->commitCalls.load(), 0U);
}

BOOST_AUTO_TEST_CASE(Spec212ProductionHandoffRejectsAggregateAndScratchBudgetsBeforeDispatch)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-handoff-budget",
                                   "tpm-memory:spec212-handoff-budget");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name ownerName("/spec212/user-budget");
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-handoff-budget"));
  auto ownerCert = makeRsaIdentity(keyChain, ownerName);
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-handoff-budget"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto packetStore = std::make_shared<Spec212CountingPacketStore>();

  std::atomic<unsigned> resolverCalls{0};
  std::atomic<unsigned> grantResolverCalls{0};
  std::atomic<std::uint64_t> aggregateLimit{512};
  std::atomic<std::uint64_t> scratchLimit{4096};
  auto service = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  service.handoffOwnerContextResolver =
    [&, ownerCert, packetStore](const ndn::Name& authenticatedOwner,
                                const ndn::Name& serviceName,
                                const std::string&)
        -> ndnsf::di::NativeConversationHandoffOwnerContextV1 {
    BOOST_CHECK_EQUAL(authenticatedOwner, ownerName);
    BOOST_CHECK_EQUAL(serviceName, ndn::Name(HOST_SERVICE_A));
    ++resolverCalls;
    ndnsf::di::NativeConversationHandoffOwnerContextV1 context;
    context.ownerEncryptionCertificate = ownerCert;
    context.durableStorageOwner.ownerIdentity = ownerName.toUri();
    context.durableStorageOwner.resolveGrant = [&grantResolverCalls] (const ndn::Name&,
        const std::string&, const std::function<void()>&)
        -> ndn_service_framework::DurableStorageGrantSelector {
      ++grantResolverCalls;
      throw std::runtime_error("test Owner denies grant after admission");
    };
    context.originalPacketStore = packetStore;
    context.saveBatchLimits = {1, 1024};
    context.portableLimits = {{1024, 2048, {}}, 1024, 1024};
    context.protectedCheckpointLimits = {1024, 2048, {}};
    context.policy.enabled = true;
    context.policy.maxConcurrentEvents = 1;
    context.policy.maxRolesPerEvent = 1;
    context.policy.maxConcurrentRoleSlots = 1;
    context.policy.maxPacketCountPerRole = 1;
    context.policy.maxPacketSetWireBytesPerRole = 1024;
    // The aggregate cap is lower than the configured per-role ceiling. The
    // effective policy therefore cannot admit even one role and must reject
    // before taking the save fence or scheduling export work.
    context.policy.maxAggregateBytes = aggregateLimit.load();
    context.policy.maxScratchBytesPerEvent = scratchLimit.load();
    context.policy.scratchQuotaBytes = 4096;
    context.policy.maxSaveDurationMs = 300000;
    context.policy.scratchRoot = std::filesystem::temp_directory_path() /
      "ndnsf-spec212-handoff-budget";
    context.repoTargetByRole.emplace("/Backbone", ndnsf::di::NativeDurableStorageGrantRepoTargetV1{
      "/spec212/repo", "/spec212/repo-provider", "/spec212/repo/service"});
    return context;
  };

  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));
  ndnsf::di::NativeConversationHandoffControlV1 control;
  control.operationId = "spec212-budget-operation-0001";
  control.conversationId = "spec212-budget-conversation-0001";
  control.checkpointDigest = "sha256:" + std::string(64, '6');
  control.sourceRoleMapDigest = "sha256:" + std::string(64, '7');
  control.destinationRoleMapDigest = "sha256:" + std::string(64, '8');
  control.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60 * 1000;
  const auto makeRequest = [](const auto& value) {
    const auto wire = ndnsf::di::nativeConversationCanonicalJson(
      ndnsf::di::nativeConversationHandoffControlV1ToJson(value));
    ndn::Buffer payload(wire.begin(), wire.end());
    RequestMessage result;
    result.setPayload(payload, payload.size());
    return result;
  };

  ndnsf::di::NativeWithdrawalQueryV1 query;
  query.conversationId = control.conversationId;
  query.checkpointDigest = control.checkpointDigest;
  query.deadlineMs = control.deadlineMs;
  const auto queryWire = ndnsf::di::nativeConversationCanonicalJson(
    ndnsf::di::nativeWithdrawalQueryV1ToJson(query));
  ndn::Buffer queryPayload(queryWire.begin(), queryWire.end());
  RequestMessage queryRequest;
  queryRequest.setPayload(queryPayload, queryPayload.size());
  bool queryCompleted = false;
  bool queryAccepted = false;
  std::optional<ndnsf::di::NativeWithdrawalQueryReplyV1> queryReply;
  const bool queryRouted = provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ownerName,
    ndn::Name("/spec212/handoff/no-withdrawal-intent"), queryRequest,
    [&](ResponseMessage response) {
      queryCompleted = true;
      queryAccepted = response.getStatus();
      if (queryAccepted) {
        const auto payload = response.getPayload();
        queryReply = ndnsf::di::nativeWithdrawalQueryReplyV1FromJson(
          std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
      }
    });
  BOOST_REQUIRE(queryRouted);
  BOOST_REQUIRE(queryCompleted);
  BOOST_REQUIRE(queryAccepted);
  BOOST_REQUIRE(queryReply);
  BOOST_CHECK(queryReply->status == ndnsf::di::NativeWithdrawalQueryStatusV1::NoIntent);
  BOOST_CHECK_EQUAL(queryReply->query.conversationId, query.conversationId);
  BOOST_CHECK_EQUAL(queryReply->query.checkpointDigest, query.checkpointDigest);
  BOOST_CHECK_EQUAL(queryReply->query.deadlineMs, query.deadlineMs);
  BOOST_CHECK(!queryReply->intent);
  BOOST_CHECK(queryReply->originalProofWire.empty());
  BOOST_CHECK_EQUAL(resolverCalls.load(), 0U);
  BOOST_CHECK_EQUAL(grantResolverCalls.load(), 0U);
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);

  bool completed = false;
  bool responseStatus = true;
  std::string responseError;
  const bool routed = provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ownerName, ndn::Name("/spec212/handoff/budget"),
    makeRequest(control),
    [&] (ResponseMessage response) {
      responseStatus = response.getStatus();
      responseError = response.getErrorInfo();
      completed = true;
    });
  BOOST_REQUIRE(routed);
  BOOST_CHECK(completed);
  BOOST_CHECK_EQUAL(resolverCalls.load(), 1U);
  BOOST_CHECK(!responseStatus);
  BOOST_CHECK_EQUAL(responseError, "BUDGET_EXCEEDED");
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);
  BOOST_CHECK_EQUAL(grantResolverCalls.load(), 0U);

  // Failed operation IDs keep their original result without resolving again.
  completed = false;
  responseStatus = true;
  responseError.clear();
  BOOST_REQUIRE(provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ownerName, ndn::Name("/spec212/handoff/budget-replay"),
    makeRequest(control),
    [&] (ResponseMessage response) {
      responseStatus = response.getStatus();
      responseError = response.getErrorInfo();
      completed = true;
    }));
  BOOST_CHECK(completed);
  BOOST_CHECK(!responseStatus);
  BOOST_CHECK_EQUAL(responseError, "BUDGET_EXCEEDED");
  BOOST_CHECK_EQUAL(resolverCalls.load(), 1U);

  // Aggregate capacity is now sufficient, but the scratch reservation cannot
  // cover the configured portable plus protected per-role upper bounds. Retry
  // the same conversation immediately with a new operation ID.
  aggregateLimit.store(2048);
  scratchLimit.store(1024);
  control.operationId = "spec212-budget-operation-0002";
  completed = false;
  responseStatus = true;
  responseError.clear();
  const bool scratchRouted = provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ownerName, ndn::Name("/spec212/handoff/scratch-budget"),
    makeRequest(control),
    [&] (ResponseMessage response) {
      responseStatus = response.getStatus();
      responseError = response.getErrorInfo();
      completed = true;
    });
  BOOST_REQUIRE(scratchRouted);
  BOOST_CHECK(completed);
  BOOST_CHECK(!responseStatus);
  BOOST_CHECK_EQUAL(responseError, "BUDGET_EXCEEDED");
  BOOST_CHECK_EQUAL(resolverCalls.load(), 2U);
  BOOST_CHECK_EQUAL(grantResolverCalls.load(), 0U);
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);

  // A control with both ceilings sufficient is admitted and fenced; without
  // packet input it cannot reach Owner grant resolution or commit data.
  scratchLimit.store(4096);
  control.operationId = "spec212-budget-operation-0003";
  completed = false;
  responseStatus = false;
  responseError.clear();
  const bool admissibleRouted = provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ownerName, ndn::Name("/spec212/handoff/admissible"),
    makeRequest(control),
    [&] (ResponseMessage response) {
      responseStatus = response.getStatus();
      responseError = response.getErrorInfo();
      completed = true;
    });
  BOOST_REQUIRE(admissibleRouted);
  BOOST_CHECK(completed);
  BOOST_CHECK(responseStatus);
  BOOST_CHECK_EQUAL(resolverCalls.load(), 3U);
  BOOST_CHECK_EQUAL(grantResolverCalls.load(), 0U);
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);

  registration.close();
  BOOST_CHECK(registration.drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(Spec212V2WithdrawalDispatchValidatesOriginalProducerProof)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-v2-withdrawal-dispatch",
                                   "tpm-memory:spec212-v2-withdrawal-dispatch");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name ownerName("/spec212/user-withdrawal-dispatch");
  const ndn::Name withdrawingProvider("/spec212/provider-withdrawing");
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec212/aa-withdrawal-dispatch"));
  makeRsaIdentity(keyChain, withdrawingProvider);
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group-withdrawal-dispatch"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->useSigningKeyChainForTest(keyChain);
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  std::atomic<unsigned> resolverCalls{0};
  auto service = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  service.handoffOwnerContextResolver =
    [&resolverCalls](const ndn::Name&, const ndn::Name&, const std::string&)
      -> ndnsf::di::NativeConversationHandoffOwnerContextV1 {
    ++resolverCalls;
    throw std::runtime_error("fixture stops before save admission");
  };
  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));

  ndnsf::di::NativeConversationHandoffControlV2 control;
  control.control.action = ndnsf::di::NativeConversationHandoffActionV1::Begin;
  control.control.operationId = "spec212-v2-withdrawal-dispatch";
  control.control.conversationId = "spec212-v2-withdrawal-conversation";
  control.control.checkpointDigest = "sha256:" + std::string(64, 'a');
  control.control.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 5 * 1000;
  control.sourceProvidersByRole = {
    {"/Backbone", HOST_PROVIDER_NAME},
    {"/Other", withdrawingProvider.toUri(ndn::name::UriFormat::ALTERNATE)}};
  ndnsf::di::NativeJson sourceRoles = ndnsf::di::NativeJson::array();
  for (const auto& [role, source] : control.sourceProvidersByRole)
    sourceRoles.push_back(ndnsf::di::NativeJson::array({role, source}));
  control.control.sourceRoleMapDigest = ndnsf::di::nativePlanningDigest(
    ndnsf::di::nativeCanonicalJson(sourceRoles));
  control.control.destinationRoleMapDigest = ndnsf::di::nativePlanningDigest("[]");
  control.trigger = ndnsf::di::NativeConversationSaveTriggerV2::WithdrawalSave;
  control.withdrawingProviderIdentity = withdrawingProvider.toUri(
    ndn::name::UriFormat::ALTERNATE);
  ndnsf::di::NativeWithdrawalIntentV1 intent;
  intent.withdrawalId = "spec212-v2-withdrawal-intent";
  intent.ownerIdentity = ownerName.toUri(ndn::name::UriFormat::ALTERNATE);
  intent.serviceName = HOST_SERVICE_A;
  intent.providerBootId = "spec212-v2-withdrawal-boot";
  intent.registrationGeneration = 1;
  intent.deadlineMs = control.control.deadlineMs;
  intent.conversationId = control.control.conversationId;
  intent.checkpointDigest = control.control.checkpointDigest;
  intent.sourceRoleMapDigest = control.control.sourceRoleMapDigest;
  intent.localRoles = {"/Other"};
  intent.policyDigest = "sha256:" + std::string(64, 'b');
  control.withdrawalIntent = intent;
  const auto proof = ndnsf::di::nativeSignWithdrawalIntentProofV1(
    withdrawingProvider, intent, [&keyChain, &withdrawingProvider](ndn::Data& packet) {
      keyChain.sign(packet, ndn::security::signingByIdentity(withdrawingProvider));
    });
  const auto& encoded = proof.wireEncode();
  control.withdrawalProofWire = ndn::Buffer(encoded.data(), encoded.data() + encoded.size());

  const auto dispatch = [&](const auto& value, const ndn::Name& requester) {
    const auto wire = ndnsf::di::nativeConversationCanonicalJson(
      ndnsf::di::nativeConversationHandoffControlV2ToJson(value));
    RequestMessage request;
    ndn::Buffer payload(wire.begin(), wire.end());
    request.setPayload(payload, payload.size());
    bool completed = false;
    bool accepted = true;
    const bool routed = provider->invokeAsyncTargetedHandlerForTest(
      ndn::Name(HOST_SERVICE_A), requester,
      ndn::Name("/spec212/v2-withdrawal-dispatch"), request,
      [&](ResponseMessage response) {
        accepted = response.getStatus();
        completed = true;
      });
    BOOST_REQUIRE(routed);
    BOOST_REQUIRE(completed);
    return accepted;
  };

  BOOST_CHECK(!dispatch(control, ndn::Name("/spec212/other-user")));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 0U);
  auto wrongProof = control;
  const ndn::Name wrongSigner("/spec212/wrong-signer");
  makeRsaIdentity(keyChain, wrongSigner);
  const auto signedByOther = ndnsf::di::nativeSignWithdrawalIntentProofV1(
    withdrawingProvider, intent, [&keyChain, &wrongSigner](ndn::Data& packet) {
      keyChain.sign(packet, ndn::security::signingByIdentity(wrongSigner));
    });
  const auto& wrongWire = signedByOther.wireEncode();
  wrongProof.withdrawalProofWire = ndn::Buffer(
    wrongWire.data(), wrongWire.data() + wrongWire.size());
  BOOST_CHECK(!dispatch(wrongProof, ownerName));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 0U);
  auto absentSource = control;
  absentSource.sourceProvidersByRole.erase("/Backbone");
  ndnsf::di::NativeJson remainingRoles = ndnsf::di::NativeJson::array();
  remainingRoles.push_back(ndnsf::di::NativeJson::array(
    {"/Other", withdrawingProvider.toUri(ndn::name::UriFormat::ALTERNATE)}));
  absentSource.control.sourceRoleMapDigest = ndnsf::di::nativePlanningDigest(
    ndnsf::di::nativeCanonicalJson(remainingRoles));
  absentSource.withdrawalIntent->sourceRoleMapDigest =
    absentSource.control.sourceRoleMapDigest;
  BOOST_CHECK(!dispatch(absentSource, ownerName));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 0U);

  control.control.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 5 * 1000;
  intent.deadlineMs = control.control.deadlineMs;
  control.withdrawalIntent = intent;
  const auto renewedProof = ndnsf::di::nativeSignWithdrawalIntentProofV1(
    withdrawingProvider, intent, [&keyChain, &withdrawingProvider](ndn::Data& packet) {
      keyChain.sign(packet, ndn::security::signingByIdentity(withdrawingProvider));
    });
  const auto& renewedWire = renewedProof.wireEncode();
  control.withdrawalProofWire = ndn::Buffer(
    renewedWire.data(), renewedWire.data() + renewedWire.size());
  BOOST_CHECK(!dispatch(control, ownerName));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 1U);
  auto changedV2 = control;
  const auto changedProof = ndnsf::di::nativeSignWithdrawalIntentProofV1(
    withdrawingProvider, intent, [&keyChain, &wrongSigner](ndn::Data& packet) {
      keyChain.sign(packet, ndn::security::signingByIdentity(wrongSigner));
    });
  const auto& changedProofWire = changedProof.wireEncode();
  changedV2.withdrawalProofWire = ndn::Buffer(
    changedProofWire.data(), changedProofWire.data() + changedProofWire.size());
  BOOST_CHECK(!dispatch(changedV2, ownerName));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 1U);
  auto changedContext = control;
  changedContext.control.checkpointDigest = "sha256:" + std::string(64, 'c');
  changedContext.withdrawalIntent->checkpointDigest =
    changedContext.control.checkpointDigest;
  BOOST_CHECK(!dispatch(changedContext, ownerName));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 1U);
  registration.close();
  BOOST_CHECK(registration.drain(std::chrono::seconds(2)));
  face.shutdown();
  face.processEvents(ndn::time::milliseconds(20));
  host.reset();
  provider.reset();
}

BOOST_AUTO_TEST_CASE(Spec204TargetedControlSharesInferenceRegistrationLifetime)
{
  ndn::security::KeyChain keyChain("pib-memory:spec204-targeted-control",
                                   "tpm-memory:spec204-targeted-control");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/aa-targeted-control"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec204/group-targeted-control"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());

  std::size_t handlerCalls = 0;
  bool runtimeSaveSurfaceReady = false;
  bool providerKvPublisherReady = false;
  bool handoffOwnerContextResolverReady = false;
  bool handoffOwnerContextResolverInvoked = false;
  std::string handoffResolverOperationId;
  bool runtimeObserverHasNoPublisher = false;
  bool saveFenceAcquired = false;
  bool saveFenceQuiescent = false;
  std::optional<ndnsf::di::ConversationSaveBatchStatusV1> emptyStoreBatchStatus;
  NativeServiceDefinition service =
    makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  service.handoffOwnerContextResolver = [&handoffOwnerContextResolverInvoked,
                                         &handoffResolverOperationId](
      const ndn::Name& owner, const ndn::Name& serviceName,
      const std::string& operationId)
      -> ndnsf::di::NativeConversationHandoffOwnerContextV1 {
    BOOST_CHECK_EQUAL(owner, ndn::Name("/spec204/user"));
    BOOST_CHECK_EQUAL(serviceName, ndn::Name(HOST_SERVICE_A));
    handoffOwnerContextResolverInvoked = true;
    handoffResolverOperationId = operationId;
    return {};
  };
  service.runtimeObserver = [&runtimeObserverHasNoPublisher](
      const ndnsf::di::NativeProviderCollaborationRuntime& runtime) {
    runtimeObserverHasNoPublisher = !static_cast<bool>(runtime.publishProtectedKvCheckpoint);
  };
  service.asyncTargetedRequestHandlerFactory =
    [&handlerCalls, &runtimeSaveSurfaceReady, &saveFenceAcquired,
     &saveFenceQuiescent, &emptyStoreBatchStatus, &providerKvPublisherReady,
     &handoffOwnerContextResolverReady](
      const ndnsf::di::NativeProviderCollaborationRuntime& runtime,
      ndnsf::di::NativeConversationHandoffOwnerContextResolverV1 resolver) {
      providerKvPublisherReady = static_cast<bool>(runtime.publishProtectedKvCheckpoint);
      handoffOwnerContextResolverReady = static_cast<bool>(resolver);
      runtimeSaveSurfaceReady = static_cast<bool>(runtime.beginConversationSaveFence) &&
        static_cast<bool>(runtime.beginServiceWithdrawalSaveFence) &&
        static_cast<bool>(runtime.beginProviderWithdrawalSaveFence) &&
        static_cast<bool>(runtime.borrowConversationWithdrawalSaveFence) &&
        static_cast<bool>(runtime.isSaveFenceQuiescent) &&
        static_cast<bool>(runtime.tryAcquireCommittedSaveBatch) &&
        static_cast<bool>(runtime.exportCommittedSaveBatchRow);
      const auto beginFence = runtime.beginConversationSaveFence;
      const auto isQuiescent = runtime.isSaveFenceQuiescent;
      const auto acquireBatch = runtime.tryAcquireCommittedSaveBatch;
      return [&, resolver = std::move(resolver), beginFence, isQuiescent,
              acquireBatch](
          const ndn::Name& requester,
          const ndn::Name& providerName,
          const ndn::Name& serviceName,
          const ndn::Name& requestId,
          const RequestMessage&,
          ServiceProvider::AsyncTargetedCompletion complete) mutable {
        BOOST_CHECK_EQUAL(requester, ndn::Name("/spec204/user"));
        BOOST_CHECK_EQUAL(providerName, ndn::Name(HOST_PROVIDER_NAME));
        BOOST_CHECK_EQUAL(serviceName, ndn::Name(HOST_SERVICE_A));
        BOOST_CHECK_EQUAL(requestId, ndn::Name("/spec204/handoff/1"));
        ++handlerCalls;

        const std::string operationId = "spec204-handoff-operation-0001";
        const auto ownerContext = resolver(requester, serviceName, operationId);
        (void)ownerContext;

        auto fence = beginFence(requester.toUri(), serviceName.toUri(),
                                "conversation-a-0001");
        saveFenceAcquired = static_cast<bool>(fence);
        if (fence) {
          saveFenceQuiescent = isQuiescent(*fence);
          const auto result = acquireBatch(
            *fence, ndnsf::di::ConversationSaveBatchLimitsV1{4, 64 * 1024}, 1);
          emptyStoreBatchStatus = result.status;
        }

        // This fixture has no committed conversation state. A connected event
        // handler must report that condition rather than claim a saved handoff.
        ResponseMessage response;
        response.setStatus(false);
        response.setErrorInfo("KV_SAVE_STATE_UNAVAILABLE");
        complete(std::move(response));
      };
    };

  const auto validFactory = service.asyncTargetedRequestHandlerFactory;
  service.asyncTargetedRequestHandlerFactory = [] (const auto&, const auto&) {
    return ServiceProvider::AsyncTargetedRequestHandler{};
  };
  BOOST_CHECK_THROW(host->serve(service, makeHostConfig(HOST_SERVICE_A)),
                    std::invalid_argument);
  service.asyncTargetedRequestHandlerFactory = validFactory;

  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));
  BOOST_REQUIRE(registration.valid());
  service.handoffOwnerContextResolver = {};
  BOOST_CHECK(providerKvPublisherReady);
  BOOST_CHECK(handoffOwnerContextResolverReady);
  BOOST_CHECK(runtimeObserverHasNoPublisher);
  BOOST_CHECK(!registration.closed());

  RequestMessage request;
  ResponseMessage observed;
  bool completed = false;
  const bool routed = provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ndn::Name("/spec204/user"),
    ndn::Name("/spec204/handoff/1"), request,
    [&observed, &completed](ResponseMessage response) {
      observed = std::move(response);
      completed = true;
    });
  BOOST_REQUIRE(routed);
  BOOST_CHECK(completed);
  BOOST_CHECK(!observed.getStatus());
  BOOST_CHECK_EQUAL(observed.getErrorInfo(), "KV_SAVE_STATE_UNAVAILABLE");
  BOOST_CHECK_EQUAL(handlerCalls, 1U);
  BOOST_CHECK(runtimeSaveSurfaceReady);
  BOOST_CHECK(saveFenceAcquired);
  BOOST_CHECK(saveFenceQuiescent);
  BOOST_REQUIRE(emptyStoreBatchStatus);
  BOOST_CHECK(*emptyStoreBatchStatus ==
              ndnsf::di::ConversationSaveBatchStatusV1::Empty);
  BOOST_CHECK(handoffOwnerContextResolverInvoked);
  BOOST_CHECK_EQUAL(handoffResolverOperationId,
                    "spec204-handoff-operation-0001");

  // The native drain fence is raised before Core registration close. A late
  // dispatch through the retained callback cannot start a second handoff.
  registration.close();
  completed = false;
  const bool lateRouted = provider->invokeAsyncTargetedHandlerForTest(
    ndn::Name(HOST_SERVICE_A), ndn::Name("/spec204/user"),
    ndn::Name("/spec204/handoff/2"), request,
    [&observed, &completed](ResponseMessage response) {
      observed = std::move(response);
      completed = true;
    });
  BOOST_CHECK_EQUAL(handlerCalls, 1U);
  if (lateRouted) {
    BOOST_CHECK(completed);
    BOOST_CHECK(!observed.getStatus());
    BOOST_CHECK_EQUAL(observed.getErrorInfo(), "native service is closing");
  }
}

// host.serve 通过真实 addScopedService/addScopedCollaborationHandler 安装：
// 双 target 同 host 共存、各自 registration 有效；active 同名重复 serve 被
// host gate 拒绝且不波及其它 target。
BOOST_AUTO_TEST_CASE(Spec182ProviderHostDualTargetSharedHostFencesDuplicate)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-dual",
                                   "tpm-memory:spec182-host-dual");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-dual"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");

  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto serviceA = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  auto serviceB = makeHostService(HOST_SERVICE_B, makeAcceptingAckHandler());

  NativeServiceRegistration regA = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A));
  NativeServiceRegistration regB = host->serve(
    serviceB, makeHostConfig(HOST_SERVICE_B));
  BOOST_REQUIRE(regA.valid());
  BOOST_REQUIRE(regB.valid());
  BOOST_CHECK(!regA.closed());
  BOOST_CHECK(!regB.closed());
  BOOST_CHECK_EQUAL(regA.serviceName(), HOST_SERVICE_A);
  BOOST_CHECK_EQUAL(regB.serviceName(), HOST_SERVICE_B);
  BOOST_CHECK_GT(regA.generation(), 0);
  BOOST_CHECK_GT(regB.generation(), 0);

  // Active duplicate: refused by the host gate; sibling unaffected.
  BOOST_CHECK_THROW(
    host->serve(serviceA, makeHostConfig(HOST_SERVICE_A)), std::logic_error);
  BOOST_CHECK(!regB.closed());
}

BOOST_AUTO_TEST_CASE(Spec182ProviderHostFirstServeFailureRollsBackHost)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-rollback",
                                   "tpm-memory:spec182-host-rollback");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-rollback"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto service = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  service.runtimeObserver = [] (const auto&) {
    throw std::runtime_error("intentional runtime observer failure");
  };

  BOOST_CHECK_THROW(host->serve(service, makeHostConfig(HOST_SERVICE_A)),
                    std::runtime_error);

  // A failed first serve must not pin the failed identity or leave the fixed
  // lease entry registered. A clean retry with the normal host identity must
  // create a usable target.
  service.runtimeObserver = {};
  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));
  BOOST_REQUIRE(registration.valid());
  BOOST_CHECK(!registration.closed());
}

BOOST_AUTO_TEST_CASE(Spec182ProviderHostRejectsUnboundIdentityBeforeHostCreation)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-identity",
                                   "tpm-memory:spec182-host-identity");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-identity"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto service = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());

  auto missingIdentity = makeHostConfig(HOST_SERVICE_A);
  missingIdentity.localProviderName.clear();
  BOOST_CHECK_THROW(host->serve(service, missingIdentity), std::invalid_argument);

  auto foreignIdentity = makeHostConfig(HOST_SERVICE_A);
  foreignIdentity.localProviderName = "/provider/OtherHost";
  BOOST_CHECK_THROW(host->serve(service, foreignIdentity), std::invalid_argument);

  auto missingBoot = makeHostConfig(HOST_SERVICE_A);
  missingBoot.providerBootId.clear();
  BOOST_CHECK_THROW(host->serve(service, missingBoot), std::invalid_argument);

  // Rejections happen before host publication; a valid retry still creates a
  // usable registration and does not inherit a foreign identity or epoch.
  auto registration = host->serve(service, makeHostConfig(HOST_SERVICE_A));
  BOOST_REQUIRE(registration.valid());
  BOOST_CHECK(!registration.closed());
}

// Host 共享 boot 身份与 compute-slot 范围：后续 serve 不得悄悄重塑。
BOOST_AUTO_TEST_CASE(Spec182ProviderHostHostConfigConsistencyFences)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-config",
                                   "tpm-memory:spec182-host-config");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-config"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto serviceA = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  NativeServiceRegistration regA = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A));
  BOOST_REQUIRE(regA.valid());

  auto renamedConfig = makeHostConfig(HOST_SERVICE_A);
  renamedConfig.localProviderName = "/spec182/provider/other";
  BOOST_CHECK_THROW(host->serve(serviceA, renamedConfig),
                    std::invalid_argument);

  auto rebootingConfig = makeHostConfig(HOST_SERVICE_A);
  rebootingConfig.providerBootId = "other-boot-epoch";
  BOOST_CHECK_THROW(host->serve(serviceA, rebootingConfig),
                    std::invalid_argument);

  auto resizedConfig = makeHostConfig(HOST_SERVICE_A, /*workerCount=*/2);
  BOOST_CHECK_THROW(host->serve(serviceA, resizedConfig),
                    std::invalid_argument);

  // The active duplicate gate fires before config checks on the same name.
  BOOST_CHECK_THROW(host->serve(serviceA, makeHostConfig(HOST_SERVICE_A)),
                    std::logic_error);
}

// close 幂等且 fence 该 target；同 serviceName 立即可重新 serve（draining
// record 被替换，不继承旧 target 的 lease/fence/bindings）。
BOOST_AUTO_TEST_CASE(Spec182ProviderHostCloseAllowsSameNameReServe)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-reserve",
                                   "tpm-memory:spec182-host-reserve");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-reserve"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto serviceA = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());

  NativeServiceRegistration reg1 = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A));
  const auto generation1 = reg1.generation();
  reg1.close();
  BOOST_CHECK(reg1.closed());
  BOOST_CHECK_NO_THROW(reg1.close()); // idempotent

  // Draining record replaced: a fresh serve of the same name is accepted.
  NativeServiceRegistration reg2 = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A));
  BOOST_REQUIRE(reg2.valid());
  BOOST_CHECK(!reg2.closed());
  BOOST_CHECK_GT(reg2.generation(), generation1);
  BOOST_CHECK(reg1.closed()); // old handle stays closed

  // And a duplicate of the fresh registration is again refused.
  BOOST_CHECK_THROW(host->serve(serviceA, makeHostConfig(HOST_SERVICE_A)),
                    std::logic_error);
}

// close 一个 target 不影响 sibling；stop 幂等关闭全部并 fence 后续 serve；
// host 释放顺序（外部 provider owner 先 reset）不提前关闭 registration。
BOOST_AUTO_TEST_CASE(Spec182ProviderHostStopClosesAllAndFencesServe)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-stop",
                                   "tpm-memory:spec182-host-stop");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-stop"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto serviceA = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  auto serviceB = makeHostService(HOST_SERVICE_B, makeAcceptingAckHandler());

  NativeServiceRegistration regA = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A));
  NativeServiceRegistration regB = host->serve(
    serviceB, makeHostConfig(HOST_SERVICE_B));

  // Close A only: B stays open until host stop.
  regA.close();
  BOOST_CHECK(regA.closed());
  BOOST_CHECK(!regB.closed());

  // The host outlives the external provider owner: registration is held by
  // the host/closure chain, not by the caller's provider shared_ptr.
  provider.reset();
  BOOST_CHECK(!regB.closed());

  host->stop();
  BOOST_CHECK(regA.closed());
  BOOST_CHECK(regB.closed());
  BOOST_CHECK_NO_THROW(host->stop()); // idempotent
  BOOST_CHECK_THROW(host->serve(serviceA, makeHostConfig(HOST_SERVICE_A)),
                    std::runtime_error);

  // Host destruction also stops; remaining registration handle stays safe.
  host.reset();
  BOOST_CHECK(regB.closed());
  BOOST_CHECK_NO_THROW(regB.close());
}

// close 后晚到 request/ack 的真实 Core 边界（host 注册面冻结）：
// scoped close 的 fence 位于 dispatch/execution 层而非 decrypt->ack 决策
// 层——closed 后到达的新 request 仍经过 host-installed 注册的 ack 决策
// 并建立 pending（pending 保留到 cleanup 边界，同 Spec182Registration
// selector 1/2 冻结语义），execution 的拒绝由 Core closed/generation gate
// 保证（T009-A selector 1/2）。host 侧断言晚到 ack 不炸、注册链完好。
BOOST_AUTO_TEST_CASE(Spec182ProviderHostLateAckAfterCloseHitsCoreBoundary)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-fence",
                                   "tpm-memory:spec182-host-fence");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/alice");
  const ndn::Name serviceName(HOST_SERVICE_A);
  const ndn::Name requestId1("/request/host-fence-1");
  const ndn::Name requestId2("/request/host-fence-2");
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name(HOST_PROVIDER_NAME));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-fence"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->applyPermissionResponse(
    makePermissionResponse(ndn::Name(HOST_PROVIDER_NAME),
                           tlv::ProviderPermission,
                           ndn::Name(HOST_PROVIDER_NAME),
                           serviceName));

  std::atomic<int> ackCalls{0};
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto serviceA = makeHostService(
    HOST_SERVICE_A,
    ServiceProvider::AckStrategyHandler(
      [&ackCalls] (const RequestMessage&) {
        ++ackCalls;
        ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "accept";
        return decision;
      }));
  NativeServiceRegistration reg = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A));

  // Real accept R1 through the host-installed registration.
  const auto [request1, requestWire1] =
    makeAuthenticatedRequestWire("payload-before-close", "spec182-host-token-1");
  provider->addPendingRequestForTokenTest(
    requesterName, serviceName, requestId1, request1, "provider-token-1");
  injectRequest(*provider, requesterName, serviceName, requestId1, requestWire1);
  BOOST_REQUIRE(provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId1));
  BOOST_REQUIRE(waitUntil([&ackCalls] { return ackCalls.load() == 1; },
                          std::chrono::seconds(5)));

  reg.close();
  BOOST_CHECK(reg.closed());

  // Late R2 (new user token, distinct requestId) after close. The Core
  // accept boundary stays reachable through the closed entry (ack decision
  // is asked, pending is recorded for the cleanup boundary); execution is
  // fenced by the Core closed/generation gate per Spec182Registration.
  const auto [request2, requestWire2] =
    makeAuthenticatedRequestWire("payload-after-close", "spec182-host-token-2");
  provider->addPendingRequestForTokenTest(
    requesterName, serviceName, requestId2, request2, "provider-token-2");
  injectRequest(*provider, requesterName, serviceName, requestId2, requestWire2);
  BOOST_REQUIRE(waitUntil([&ackCalls] { return ackCalls.load() == 2; },
                          std::chrono::seconds(5)));
  BOOST_CHECK(provider->hasPendingRequestForTokenTest(
    requesterName, serviceName, requestId2));
  BOOST_CHECK(reg.closed());
  BOOST_CHECK_NO_THROW(reg.close()); // late-ack window never breaks the host
}

// 固定 lease 入口真实路由（Core request/selection 全链）：单入口服务双
// target；target close 后 sibling 继续可用（PO-014 shared service）；draining
// target 与 re-serve 后的新 target 都不破坏固定入口。
BOOST_AUTO_TEST_CASE(Spec182ProviderHostFixedLeaseEntryRoutesRealDispatch)
{
  ndn::security::KeyChain keyChain("pib-memory:spec182-host-lease-route",
                                   "tpm-memory:spec182-host-lease-route");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/spec182/user/alice");
  const ndn::Name providerName(HOST_PROVIDER_NAME);
  const ndn::Name leaseEntryName(ndnsf::di::EXECUTION_LEASE_SERVICE_NAME);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec182/aa-host-lease-route"));
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec182/group"), providerCert, aaCert,
    "examples/trust-any.conf");
  provider->applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           leaseEntryName));

  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto serviceA = makeHostService(HOST_SERVICE_A, makeAcceptingAckHandler());
  auto serviceB = makeHostService(HOST_SERVICE_B, makeAcceptingAckHandler());
  NativeServiceRegistration regA = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A, /*workerCount=*/1,
                             /*leaseOn=*/true));
  NativeServiceRegistration regB = host->serve(
    serviceB, makeHostConfig(HOST_SERVICE_B, /*workerCount=*/1,
                             /*leaseOn=*/true));

  // 真实投递并等待该 selection 的 dispatch 落定（成功 = Completed）。
  auto dispatchLeaseRequest = [&] (const std::string& tag,
                                   const std::string& userToken,
                                   const std::string& providerToken,
                                   const ndn::Name& requestIdName,
                                   const std::string& targetService) {
    ndnsf::di::LeaseOperationRequest operation;
    operation.operation = ndnsf::di::LeaseOperation::Prepare;
    operation.requestId = "lease-req-" + tag;
    operation.planDigest = "plan-" + tag;
    operation.idempotencyKey = "idem-" + tag;
    operation.targetServiceName = targetService;
    operation.resourceBindingProof = ndn::Buffer{1, 2, 3};
    operation.roles = {"/Backbone"};
    operation.expiresAtMs = 200000;
    const auto [request, requestWire] = makeAuthenticatedRequestWire(
      ndnsf::di::encodeLeaseOperationRequest(operation), userToken);
    provider->addPendingRequestForTokenTest(
      requesterName, leaseEntryName, requestIdName, request, providerToken);
    injectRequest(*provider, requesterName, leaseEntryName, requestIdName,
                  requestWire);
    BOOST_REQUIRE(provider->hasPendingRequestForTokenTest(
      requesterName, leaseEntryName, requestIdName));
    const auto selection = makeSelectionBuffer(requestIdName, providerToken);
    const auto digest = selectionDigestFor(selection);
    BOOST_REQUIRE(!digest.empty());
    provider->OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requesterName, providerName, leaseEntryName, requestIdName, selection);
    const auto status = provider->getSelectionExecutionStatus(digest);
    BOOST_REQUIRE_MESSAGE(
      status != std::nullopt,
      "lease dispatch for " << tag << " never reached a terminal state");
    return status;
  };

  // R_A: A 与 B 并存时,Prepare(A) 经单固定入口路由到 A 的 lease instance。
  auto statusA = dispatchLeaseRequest(
    "a", "spec182-lease-token-a", "provider-lease-token-a",
    ndn::Name("/request/lease-a"), HOST_SERVICE_A);
  BOOST_CHECK(statusA->state == SelectionExecutionState::Completed);
  BOOST_CHECK(statusA->message.find("exception") == std::string::npos);

  regA.close();

  // R_B: A close 后共享固定入口仍为 sibling B 路由（另一个共享服务可用）。
  auto statusB = dispatchLeaseRequest(
    "b", "spec182-lease-token-b", "provider-lease-token-b",
    ndn::Name("/request/lease-b"), HOST_SERVICE_B);
  BOOST_CHECK(statusB->state == SelectionExecutionState::Completed);
  BOOST_CHECK(statusB->message.find("exception") == std::string::npos);

  // R_A2: draining target 的晚到 Prepare 由 router 应答（不崩溃、不牵连
  // 固定入口）。
  auto statusA2 = dispatchLeaseRequest(
    "a2", "spec182-lease-token-a2", "provider-lease-token-a2",
    ndn::Name("/request/lease-a2"), HOST_SERVICE_A);
  BOOST_CHECK(statusA2->state == SelectionExecutionState::Completed);
  BOOST_CHECK(statusA2->message.find("exception") == std::string::npos);

  // Re-serve A: 旧 draining record 被替换;固定入口继续把 Prepare 路由到
  // 新 target 的 lease instance（旧行残留不影响新注册的 Core 面）。
  NativeServiceRegistration regA2 = host->serve(
    serviceA, makeHostConfig(HOST_SERVICE_A, /*workerCount=*/1,
                             /*leaseOn=*/true));
  BOOST_REQUIRE(regA2.valid());
  auto statusA3 = dispatchLeaseRequest(
    "a3", "spec182-lease-token-a3", "provider-lease-token-a3",
    ndn::Name("/request/lease-a3"), HOST_SERVICE_A);
  BOOST_CHECK(statusA3->state == SelectionExecutionState::Completed);
  BOOST_CHECK(statusA3->message.find("exception") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END() // Spec182ProviderHost

} // namespace ndn_service_framework::test
