#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di::tests {

void spec185RunRealProviderUnary();
void spec185RunRealProviderCancellationRace();
void spec185RunRealProviderCommitFailure();

namespace {

class CoreDelegationAdapter final : public NativeModelAdapter
{
public:
  std::string adapterId() const override { return "spec185-core-delegation"; }
  std::string adapterVersion() const override { return "1"; }

  NativeModelDescriptor inspect(const std::string& modelName,
                                const std::string& modelDigest) const override
  {
    NativeModelDescriptor descriptor;
    descriptor.modelName = modelName;
    descriptor.contentDigest = modelDigest;
    descriptor.semanticsDigest = nativePlanningDigest("spec185-semantics");
    descriptor.graphDigest = nativePlanningDigest("spec185-graph");
    descriptor.modelFormat = "onnx";
    descriptor.precision = "float32";
    descriptor.adapterId = adapterId();
    descriptor.adapterVersion = adapterVersion();
    descriptor.adapter = fixture::modelAdapter(adapterId(), adapterVersion(),
                                                descriptor.modelFormat,
                                                descriptor.precision);
    return descriptor;
  }

  std::vector<std::uint8_t> encodeInput(
    const std::vector<std::uint8_t>& input) const override
  {
    return input;
  }

  std::vector<std::uint8_t> decodeResult(
    const std::vector<std::uint8_t>& result) const override
  {
    return result;
  }
};

class CoreDelegationSplit final : public NativeModelSplitStrategy
{
public:
  NativeStrategyIdentity identity() const override
  {
    return {"spec185-core-delegation-split", "1", nativePlanningDigest("{}")};
  }

  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor&, const NativeGraphSnapshot&,
    const NativeCandidateBudget&) const override
  {
    return {};
  }
};

struct ClientFixture
{
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  std::shared_ptr<ndn_service_framework::test::LocalServiceUser> user;
  std::shared_ptr<NativeAdapterRegistry> adapters =
    std::make_shared<NativeAdapterRegistry>();
  std::shared_ptr<CoreDelegationAdapter> adapter =
    std::make_shared<CoreDelegationAdapter>();
  NativeModelRef model;

  ClientFixture()
  {
    using namespace ndn_service_framework::test;
    const auto cert = makeRsaIdentity(keyChain, ndn::Name("/spec185/user"));
    const auto aa = makeRsaIdentity(keyChain, ndn::Name("/spec185/aa"));
    user = std::make_shared<LocalServiceUser>(face, ndn::Name("/spec185"), cert, aa,
                                               "examples/trust-any.conf");
    adapters->registerAdapter(adapter);
    adapters->freeze();
    model.modelName = "spec185-model";
    model.contentDigest = nativePlanningDigest("spec185-model");
    model.semanticsDigest = nativePlanningDigest("spec185-semantics");
    model.graphDigest = nativePlanningDigest("spec185-graph");
    model.modelFormat = "onnx";
    model.precision = "float32";
    model.adapterId = adapter->adapterId();
    model.adapterVersion = adapter->adapterVersion();
    model.adapter = fixture::modelAdapter(model.adapterId, model.adapterVersion,
                                           model.modelFormat, model.precision);
  }
};

NativeInferenceHandle
submitUnlinkedRequest(NativeInferenceClient& client, const NativeModelRef& model)
{
  NativeApplicationInput input;
  input.taskName = "task";
  input.payload = {0x01, 0x02, 0x03};
  NativeRequestOptions options;
  options.timeoutMs = 2'000;
  options.ackTimeoutMs = 100;
  return client.request(model, input,
                        std::make_shared<CoreDelegationSplit>(),
                        std::make_shared<NativePreSplitFirstPlacement>(), options);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec185DiCoreOperation)

BOOST_AUTO_TEST_CASE(ProductionClientUsesCoreCompletionAndObservation)
{
  ClientFixture fixture;
  NativeInferenceClient client(fixture.user, fixture.adapters);
  auto handle = submitUnlinkedRequest(client, fixture.model);

  std::promise<void> terminalObserved;
  auto observed = terminalObserved.get_future();
  handle.observe([&terminalObserved] (const NativeInferenceEvent& event) {
    if (event.terminal)
      terminalObserved.set_value();
  });

  try {
    (void)handle.result(std::chrono::seconds(2));
    BOOST_FAIL("an unlinked client must not synthesize a successful result");
  }
  catch (const NativeDiError& error) {
    BOOST_CHECK_EQUAL(error.code(), "NATIVE_REQUEST_PIPELINE_NOT_READY");
    BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
  }
  BOOST_REQUIRE(observed.wait_for(std::chrono::seconds(1)) ==
                std::future_status::ready);
  client.close();
}

BOOST_AUTO_TEST_CASE(TerminalFailureRemainsStableAfterCancel)
{
  ClientFixture fixture;
  NativeInferenceClient client(fixture.user, fixture.adapters);
  auto handle = submitUnlinkedRequest(client, fixture.model);
  try {
    (void)handle.result(std::chrono::seconds(2));
    BOOST_FAIL("an unlinked client must fail closed");
  }
  catch (const NativeDiError& error) {
    BOOST_CHECK_EQUAL(error.code(), "NATIVE_REQUEST_PIPELINE_NOT_READY");
  }
  handle.cancel();
  BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
  client.close();
}

BOOST_AUTO_TEST_CASE(ProductionClientDelegatesAckCommitResponseToCore)
{
  // This production-backed fixture drives ACK closure, native planning,
  // CommitCollaborationPlan, Provider response, and the Core-owned result.
  spec185RunRealProviderUnary();
}

BOOST_AUTO_TEST_CASE(ProductionClientPreservesTerminalStateAcrossLateCancel)
{
  // The Provider blocks during FINALIZE after durable commit.  A concurrent
  // cancel is a late callback race and must not downgrade the committed turn.
  spec185RunRealProviderCancellationRace();
}

BOOST_AUTO_TEST_CASE(ProductionClientRejectsConversationCommitFailure)
{
  // A Provider commit ACK with committed=false must fail the DI operation and
  // leave no conversation checkpoint, while Core owns the terminal result.
  spec185RunRealProviderCommitFailure();
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
