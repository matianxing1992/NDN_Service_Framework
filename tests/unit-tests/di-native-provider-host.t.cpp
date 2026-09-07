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
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

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

constexpr char NATIVE_HOST_PROVIDER_NAME[] = "/spec182/provider/native-host";
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
