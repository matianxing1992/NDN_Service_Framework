/* spec182 T009-A: Spec182Registration frozen selectors.
 *
 * 全部通过真实注册（addScopedService/addScopedCollaborationHandler）与
 * V2 request/selection 投递驱动：真实 inline/worker dispatch、真实 Face
 * 完成路径、真实 cleanup，不以独立 bool 代替。六 selector 对应
 * specs/182-native-di-python-bindings/contracts/native-provider-lifecycle-design.md
 * 六路径 gate 表（Selection/ack/finish/cleanup）。
 */

#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

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

} // namespace ndn_service_framework::test
