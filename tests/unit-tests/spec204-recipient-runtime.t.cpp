#include "tests/fixtures/spec204/runtime-config.hpp"
#include "tests/unit-tests/spec204-protected-kv-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/RuntimeTestAccess.hpp"

#include <future>
#include <thread>

BOOST_AUTO_TEST_SUITE(Spec204RecipientRuntime)

BOOST_AUTO_TEST_CASE(RuntimeKvWrappingRunsOnOwnedIoAndDrains)
{
  using namespace ndnsf::di;
  RuntimeFixture configuration;
  const auto runtime = Runtime::open(runtimeConfig(configuration));
  ndn::security::Certificate ownerCertificate;
  const auto wrap = detail::RuntimeTestAccess::startProtectedKvKeyOwner(runtime, ownerCertificate);
  auto threadPromise = std::make_shared<std::promise<std::thread::id>>();
  auto thread = threadPromise->get_future();
  detail::RuntimeTestAccess::postProtectedKvOwner(runtime,
    [threadPromise] { threadPromise->set_value(std::this_thread::get_id()); });
  BOOST_REQUIRE(thread.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  const auto ioThread = thread.get(); BOOST_CHECK(ioThread != std::this_thread::get_id());
  BOOST_CHECK_THROW(runtime->user().prepare(), DiError);

  ndnsf::di::test::protectedKv::Fixture kv; kv.certificate = ownerCertificate;
  const auto reference = kv.protect();
  ndn::security::KeyChain recipientKeys{"pib-memory:runtime-kv-recipient", "tpm-memory:runtime-kv-recipient"};
  const auto certificate = recipientKeys.createIdentity(ndn::Name("/fresh/provider/b"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto key = certificate.getPublicKey(); const ndn::Buffer publicKey(key.data(), key.size());
  NativeProtectedKvRecipientBindingV1 binding;
  binding.requesterIdentity = ownerCertificate.getIdentity(); binding.recipientIdentity = certificate.getIdentity();
  binding.recipientCertificateName = certificate.getName(); binding.serviceName = ndn::Name("/Inference");
  binding.requestId = ndn::Name("/opaque-runtime-key-request");
  binding.authorizationBinding = ndnsf::di::test::protectedKv::hash("actual fixture grant binding");
  binding.planDigest = ndnsf::di::test::protectedKv::hash("sealed runtime plan"); binding.roleName = "/decoder";
  binding.recipientPublicKeyDigest = ndnsf::di::test::protectedKv::hash(
    std::string(reinterpret_cast<const char*>(key.data()), key.size()));
  binding.expiresAtMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  NativeRequestControl control{binding.requestId.toUri(), 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [cancelled] { return cancelled->load(); }};
  auto sawIo = std::make_shared<std::atomic<bool>>(false);
  const auto current = [ioThread, sawIo] {
    if (std::this_thread::get_id() == ioThread) sawIo->store(true);
  };
  const auto capsule = wrap(reference, publicKey, binding, control, current);
  BOOST_CHECK(sawIo->load());
  nativeRecoverProtectedPortableKvCheckpointForRecipient(kv.cipher, kv.restored, reference, capsule,
    certificate, recipientKeys, binding, kv.limits, [] {});
  BOOST_CHECK_EQUAL(ndnsf::di::test::protectedKv::read(kv.restored), ndnsf::di::test::protectedKv::read(kv.plain));

  auto release = std::make_shared<std::promise<void>>();
  const auto released = release->get_future().share();
  struct Release {
    std::shared_ptr<std::promise<void>> promise;
    ~Release() { try { promise->set_value(); } catch (const std::future_error&) {} }
  } releaseOnFailure{release};
  auto entered = std::make_shared<std::promise<void>>(); auto enteredFuture = entered->get_future();
  auto queueTimeout = std::make_shared<std::atomic<bool>>(false);
  detail::RuntimeTestAccess::postProtectedKvOwner(runtime, [released, entered, queueTimeout] {
    entered->set_value();
    if (released.wait_for(std::chrono::seconds(5)) != std::future_status::ready) queueTimeout->store(true);
  });
  BOOST_REQUIRE(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  auto probes = std::make_shared<std::atomic<unsigned>>(0);
  auto queued = std::make_shared<std::promise<void>>(); auto queuedFuture = queued->get_future();
  auto pending = std::async(std::launch::async, [wrap, reference, publicKey, binding, control, probes, queued] {
    return wrap(reference, publicKey, binding, control, [probes, queued] {
      if (probes->fetch_add(1) == 1) queued->set_value();
    });
  });
  BOOST_REQUIRE(queuedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  cancelled->store(true);
  BOOST_REQUIRE(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_THROW(pending.get(), std::runtime_error);
  release->set_value();
  auto drained = std::make_shared<std::promise<void>>(); auto drainedFuture = drained->get_future();
  detail::RuntimeTestAccess::postProtectedKvOwner(runtime, [drained] { drained->set_value(); });
  BOOST_REQUIRE(drainedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK(!queueTimeout->load());
  cancelled->store(false); runtime->close();
  BOOST_CHECK_THROW(wrap(reference, publicKey, binding, control, current), std::runtime_error);
  BOOST_CHECK(runtime->drain(std::chrono::seconds(3)));
}

BOOST_AUTO_TEST_SUITE_END()
