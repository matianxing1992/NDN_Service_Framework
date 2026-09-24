#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <future>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <string>
#include <vector>

namespace ndnsf::di::test {
namespace {

std::vector<std::uint8_t>
rawInt64(std::int64_t value)
{
  std::vector<std::uint8_t> payload(sizeof(value));
  std::memcpy(payload.data(), &value, sizeof(value));
  return payload;
}

class CapturingDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    std::promise<TensorBundle> promise;
    promise.set_exception(std::make_exception_ptr(
      std::runtime_error("unexpected stage-transfer prefetch")));
    return promise.get_future();
  }

  void
  publishOutput(const std::string&, const DependencyEdge& edge,
                const TensorBundle& bundle) override
  {
    if (bundle.transferObservation == nullptr) {
      m_valid.store(false);
      return;
    }
    auto& observation = *bundle.transferObservation;
    observation.actualDataName = "/actual/" + edge.scope;
    observation.transportPayloadBytes = observation.encodedPayloadBytes + 13;
    observation.metadataBytes = 11;
    observation.wireBytes = bundle.payload.size() + 97;
    observation.interestCount = 1;
    observation.retryCount = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    published[edge.scope] = bundle;
  }

  std::map<std::string, TensorBundle> snapshot() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return published;
  }

  bool valid() const
  {
    return m_valid.load();
  }

private:
  std::atomic<bool> m_valid{true};
  mutable std::mutex m_mutex;
  std::map<std::string, TensorBundle> published;
};

TensorBundle
runnerBundle(const std::string& scope)
{
  NamedTensor hidden;
  hidden.name = "hidden";
  hidden.elementType = TensorElementType::Float32;
  hidden.shape = {1, 2};
  hidden.payload = float32Payload({1.0f, 2.0f});

  NamedTensor kv;
  kv.name = "attention_kv_out";
  kv.elementType = TensorElementType::Int64;
  kv.shape = {1};
  kv.payload = rawInt64(3);

  NamedTensor weights;
  weights.name = "layer.0.weight";
  weights.elementType = TensorElementType::UInt8;
  weights.shape = {4};
  weights.payload = {1, 2, 3, 4};

  NamedTensor logits;
  logits.name = "logits";
  logits.elementType = TensorElementType::Float32;
  logits.shape = {1, 4};
  logits.payload = float32Payload({0.0f, 1.0f, 0.0f, 0.0f});

  return makeEncodedTensorBundle(
    scope, {std::move(hidden), std::move(kv), std::move(weights), std::move(logits)});
}

GenerationEpochLineageV1
fixtureLineage(const std::string& requestId)
{
  GenerationEpochLineageV1 lineage;
  const std::string digest = "sha256:" + std::string(64, 'a');
  lineage.requestId = requestId;
  lineage.attemptEpoch = 4;
  lineage.planDigest = digest;
  lineage.generationId = "generation-1";
  lineage.streamEpoch = 1;
  lineage.inferenceEpoch = 1;
  lineage.transitionKind = GenerationEpochLineageV1::DECODE;
  lineage.logicalPrefixDigest = digest;
  lineage.logicalPrefixTokenCount = 1;
  lineage.positionDigest = digest;
  lineage.producerRole = "/Stage/0";
  lineage.consumerRole = "/Stage/1";
  lineage.operationIndex = 1;
  return lineage;
}

} // namespace

BOOST_AUTO_TEST_CASE(StageTransferBudgetUsesActualSelectedTensorAndTransportBytes)
{
  const std::vector<std::pair<std::string, std::string>> phases = {
    {"prompt", "APPLICATION_INPUT"},
    {"delta", "ACTIVATION"},
    {"decode", "TOKEN_FEEDBACK"},
    {"finalize", "FINALIZE"},
  };
  auto io = std::make_shared<CapturingDependencyIo>();
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));

  RoleSpec role;
  role.role = "/Stage/0";
  role.requestId = "/request/stage-transfer";
  role.attemptEpoch = 4;
  role.inferenceEpoch = 1;
  role.generationLineage = fixtureLineage(role.requestId);
  role.stateOutputNames = {"attention_kv_out"};
  for (const auto& [scope, operation] : phases) {
    DependencyEdge edge;
    edge.scope = scope;
    edge.producerRole = "/Stage/0";
    edge.consumerRole = "/Stage/1";
    edge.plannedDataName = "/stage-transfer/" + scope;
    edge.tensors = {"hidden"};
    edge.operationKind = operation;
    edge.requestId = role.requestId;
    edge.attemptEpoch = role.attemptEpoch;
    role.outputs.push_back(std::move(edge));
  }

  auto future = worker.executeAsync(
    "stage-session", role, io,
    [] (const RoleExecutionContext&) {
      std::map<std::string, TensorBundle> outputs;
      for (const auto& scope : {"prompt", "delta", "decode", "finalize"}) {
        outputs.emplace(scope, runnerBundle(scope));
      }
      return outputs;
    });
  const auto result = future.get();
  BOOST_REQUIRE(io->valid());
  BOOST_REQUIRE_EQUAL(result.outputTimings.size(), phases.size());

  StageTransferBudget cumulative;
  StageTransferBudget previous;
  previous.snapshotIdentity = "snapshot-initial";
  for (std::size_t index = 0; index < result.outputTimings.size(); ++index) {
    const auto& timing = result.outputTimings[index];
    BOOST_REQUIRE(timing.transferObservation != nullptr);
    const auto& observation = *timing.transferObservation;
    BOOST_CHECK_EQUAL(observation.phase, phases[index].first);
    BOOST_CHECK_EQUAL(observation.direction, "send");
    BOOST_CHECK_EQUAL(observation.tensorBytes, sizeof(float) * 2);
    BOOST_REQUIRE_EQUAL(observation.tensorNames.size(), 1U);
    BOOST_CHECK_EQUAL(observation.tensorNames.front(), "hidden");
    BOOST_CHECK(observation.encodedPayloadBytes > observation.tensorBytes);
    BOOST_REQUIRE(observation.localCopyBytes.has_value());
    BOOST_CHECK(*observation.localCopyBytes > 0);
    BOOST_REQUIRE(observation.transportPayloadBytes.has_value());
    BOOST_CHECK_EQUAL(observation.actualDataName, "/actual/" + phases[index].first);
    BOOST_CHECK(observation.lineagePresent);
    BOOST_CHECK_EQUAL(observation.positionDigest.size(), 71U);
    BOOST_REQUIRE(observation.metadataBytes.has_value());
    BOOST_REQUIRE(observation.wireBytes.has_value());
    BOOST_REQUIRE(observation.interestCount.has_value());
    BOOST_REQUIRE(observation.retryCount.has_value());
    BOOST_CHECK_EQUAL(*observation.metadataBytes, 11U);
    BOOST_CHECK_EQUAL(*observation.wireBytes,
                      observation.encodedPayloadBytes + 97U);
    BOOST_CHECK_EQUAL(*observation.interestCount, 1U);
    BOOST_CHECK_EQUAL(*observation.retryCount, 0U);
    BOOST_CHECK_EQUAL(*observation.transportPayloadBytes,
                      observation.encodedPayloadBytes + 13U);

    cumulative.add(observation);
    cumulative.snapshotIdentity = "snapshot-" + std::to_string(index);
    const auto delta = cumulative.deltaFrom(previous);
    BOOST_CHECK_EQUAL(delta.tensorBytes, observation.tensorBytes);
    BOOST_CHECK_EQUAL(delta.encodedPayloadBytes, observation.encodedPayloadBytes);
    BOOST_CHECK_EQUAL(delta.transportPayloadBytes,
                      *observation.transportPayloadBytes);
    BOOST_CHECK(delta.transportPayloadObserved);
    BOOST_REQUIRE(observation.localCopyBytes.has_value());
    BOOST_CHECK(delta.localCopyObserved);
    BOOST_CHECK_EQUAL(delta.localCopyBytes, *observation.localCopyBytes);
    previous = cumulative;
  }
  BOOST_CHECK_EQUAL(cumulative.tensorBytes, sizeof(float) * 2 * phases.size());
  BOOST_CHECK_EQUAL(cumulative.interestCount, phases.size());
  BOOST_CHECK_EQUAL(cumulative.retryCount, 0U);

  const auto published = io->snapshot();
  BOOST_REQUIRE_EQUAL(published.size(), phases.size());
  for (const auto& item : published) {
    const auto tensors = decodeTensorBundle(
      stripGenerationEpochLineage(item.second).payload);
    BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
    BOOST_CHECK_EQUAL(tensors.front().name, "hidden");
    BOOST_CHECK_EQUAL(tensors.front().payload.size(), sizeof(float) * 2);
  }
}

BOOST_AUTO_TEST_CASE(StageTransferRejectsProviderLocalKvInSealedEdge)
{
  auto io = std::make_shared<CapturingDependencyIo>();
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  RoleSpec role;
  role.role = "/Stage/0";
  role.requestId = "/request/stage-transfer-negative";
  role.attemptEpoch = 1;
  role.stateOutputNames = {"attention_kv_out"};
  DependencyEdge edge;
  edge.scope = "activation";
  edge.producerRole = "/Stage/0";
  edge.consumerRole = "/Stage/1";
  edge.plannedDataName = "/stage-transfer/activation";
  edge.tensors = {"hidden", "attention_kv_out"};
  edge.operationKind = "ACTIVATION";
  role.outputs.push_back(edge);

  auto future = worker.executeAsync(
    "negative-session", role, io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{
        {"activation", runnerBundle("activation")},
      };
    });
  BOOST_CHECK_THROW(future.get(), std::logic_error);
  BOOST_CHECK(io->snapshot().empty());
}

BOOST_AUTO_TEST_CASE(StageTransferBudgetRejectsDuplicateAndOutOfOrderSnapshots)
{
  StageTransferObservation observation;
  observation.edgeScope = "activation";
  observation.plannedDataName = "/stage-transfer/activation";
  observation.direction = "send";
  observation.phase = "delta";
  observation.identity = "request|1|activation|/stage-transfer/activation";
  observation.tensorBytes = 8;
  observation.encodedPayloadBytes = 32;
  observation.localCopyBytes = 32;

  StageTransferBudget cumulative;
  cumulative.add(observation);
  cumulative.snapshotIdentity = "snapshot-1";
  BOOST_CHECK_THROW(cumulative.add(observation), std::invalid_argument);

  StageTransferBudget previous;
  previous.snapshotIdentity = "snapshot-1";
  BOOST_CHECK_THROW(cumulative.deltaFrom(previous), std::invalid_argument);
  cumulative.snapshotIdentity = "snapshot-2";
  previous.snapshotIdentity = "snapshot-previous";
  previous.seenIdentities.insert("not-in-current");
  previous.tensorBytes = 1;
  BOOST_CHECK_THROW(cumulative.deltaFrom(previous), std::invalid_argument);
  previous.seenIdentities.clear();
  previous.tensorBytes = 16;
  BOOST_CHECK_THROW(cumulative.deltaFrom(previous), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StrippingGenerationLineageRetainsTransferObservation)
{
  auto bundle = runnerBundle("decode");
  auto observation = std::make_shared<StageTransferObservation>();
  observation->edgeScope = "decode";
  observation->direction = "receive";
  observation->phase = "decode";
  observation->identity = "decode-observation";
  observation->lineagePresent = true;
  observation->positionDigest = "sha256:" + std::string(64, 'a');

  const auto withLineage = attachGenerationEpochLineage(
    bundle, fixtureLineage("/request/strip-observation"));
  auto observed = withLineage;
  observed.transferObservation = observation;
  const auto stripped = stripGenerationEpochLineage(observed);

  BOOST_CHECK(!extractGenerationEpochLineage(stripped).has_value());
  BOOST_REQUIRE(stripped.transferObservation != nullptr);
  BOOST_CHECK_EQUAL(stripped.transferObservation.get(), observation.get());
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPreservesStrippedDecodeObservation)
{
  const auto requestId = "/request/worker-stripped-decode";
  auto bundle = attachGenerationEpochLineage(
    runnerBundle("feedback"), fixtureLineage(requestId));
  auto observation = std::make_shared<StageTransferObservation>();
  observation->lineagePresent = true;
  observation->positionDigest = fixtureLineage(requestId).positionDigest;
  observation->lineageIdentity = "preserved-lineage";
  bundle.transferObservation = observation;
  bundle = stripGenerationEpochLineage(bundle);

  DependencyEdge edge("feedback", "/Stage/0", "/Stage/1",
                      "/stage-transfer/feedback", 1, 0,
                      {"hidden"});
  edge.requestId = requestId;
  edge.attemptEpoch = 4;
  edge.operationKind = "TOKEN_FEEDBACK";
  RoleSpec role("/Stage/1", {edge}, {}, requestId, 4);
  auto io = std::make_shared<CapturingDependencyIo>();
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));

  auto future = worker.executeAsync(
    "stripped-decode-session", role, io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{};
    },
    {{"feedback", std::move(bundle)}});
  const auto result = future.get();

  BOOST_REQUIRE_EQUAL(result.inputTimings.size(), 1U);
  BOOST_REQUIRE(result.inputTimings.front().transferObservation != nullptr);
  BOOST_CHECK(result.inputTimings.front().transferObservation->lineagePresent);
  BOOST_CHECK_EQUAL(result.inputTimings.front().transferObservation->positionDigest,
                    fixtureLineage(requestId).positionDigest);
}

BOOST_AUTO_TEST_CASE(StageTransferBudgetRejectsMissingGenerationPosition)
{
  StageTransferObservation observation;
  observation.edgeScope = "decode";
  observation.direction = "send";
  observation.phase = "decode";
  observation.identity = "decode-without-lineage";
  observation.tensorBytes = 8;
  observation.encodedPayloadBytes = 32;
  StageTransferBudget budget;
  BOOST_CHECK_THROW(budget.add(observation), std::invalid_argument);
}

} // namespace ndnsf::di::test
