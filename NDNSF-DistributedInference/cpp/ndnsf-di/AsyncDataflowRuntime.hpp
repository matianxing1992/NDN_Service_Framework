#ifndef NDNSF_DISTRIBUTED_INFERENCE_ASYNC_DATAFLOW_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_ASYNC_DATAFLOW_RUNTIME_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ndnsf::di {

struct TensorBundle
{
  std::string name;
  std::vector<uint8_t> payload;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
};

/** Adapter-certified tensor-layout transition at one pipeline boundary. */
struct RedistributionSpec
{
  std::vector<std::uint64_t> producerRanks;
  std::vector<std::uint64_t> consumerRanks;
  std::string tensor;
  std::string operation;
  std::string epoch;
  std::string integrityDigest;
  std::string sourceLayoutDigest;
  std::string targetLayoutDigest;
  std::int64_t axis = -1;
  std::size_t temporaryMemoryBytes = 0;
  bool completeOutput = false;
};

struct DependencyEdge
{
  DependencyEdge() = default;

  DependencyEdge(std::string scope,
                 std::string producerRole,
                 std::string consumerRole,
                 std::string plannedDataName,
                 std::size_t expectedSegments = 0,
                 std::size_t expectedBytes = 0,
                 std::vector<std::string> tensors = {});

  std::string scope;
  std::string producerRole;
  std::string consumerRole;
  std::vector<std::string> consumerRoles;
  std::string plannedDataName;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::vector<std::string> tensors;
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  // Cross-Provider edges opt into the authenticated NDNSF_DATA_V1 profile.
  // Ordinary pipeline dependencies remain on the existing COLLAB-LARGE path.
  bool useNdnsfDataV1 = false;
  std::uint64_t collectiveOperationIndex = 0;
  std::string collectiveProducerRank;
  std::string collectiveSourceLayoutDigest;
  std::string collectiveTargetLayoutDigest;
  std::string collectiveTensorDigest;
  std::vector<RedistributionSpec> redistributions;
  // Rank identifiers from the adapter-certified redistribution edge for this
  // exact producer/consumer role projection.
  std::optional<std::uint64_t> redistributionProducerRank;
  std::optional<std::uint64_t> redistributionConsumerRank;
  // Request-scoped V3 projections use the ordinary Collaboration naming
  // contract when no explicit plannedDataName is sealed.
  std::string transportScope;
  std::string producerProvider;
  std::string topicPrefix;
  // Exact Placement V3 tensor-object authority. `expectedSegments` is the
  // concrete count only after the signed manifest is verified; maxSegments is
  // the plan-time allocation/fetch bound.
  bool declaredByV3 = false;
  std::string manifestDataName;
  std::size_t maxSegments = 0;
  std::string endpointDigest;
  std::string planDigest;
  std::string manifestContractDigest;
  std::string tensorDigest;
  std::string layoutDigest;
  std::string securityProfile;
  std::string operationKind;
  std::uint64_t round = 0;
  std::uint64_t microbatch = 0;
  std::uint64_t noProgressDeadlineMs = 0;
  std::uint64_t hardDeadlineMs = 0;
};

/** Fail closed unless a dependency bundle is complete for its sealed edge.
 * V3 uses the runtime manifest's concrete segment/byte counts, while legacy
 * edges may retain zero as an unspecified count. */
void
validateTensorBundleForEdge(const DependencyEdge& edge,
                            const TensorBundle& bundle,
                            bool requireReconstructed = true);

struct RoleSpec
{
  RoleSpec() = default;

  RoleSpec(std::string role,
           std::vector<DependencyEdge> inputs,
           std::vector<DependencyEdge> outputs,
           std::string requestId = {},
           std::uint64_t attemptEpoch = 0)
    : role(std::move(role))
    , inputs(std::move(inputs))
    , outputs(std::move(outputs))
    , requestId(std::move(requestId))
    , attemptEpoch(attemptEpoch)
  {
  }

  std::string role;
  std::vector<DependencyEdge> inputs;
  std::vector<DependencyEdge> outputs;
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
};

struct RoleExecutionContext
{
  std::string sessionId;
  std::string role;
  std::map<std::string, TensorBundle> inputsByScope;
  // Exact request-scoped dependency and redistribution contracts corresponding
  // to inputsByScope. Adapter runners use these to apply GATHER/SCATTER/RESHARD
  // rather than guessing layout semantics from tensor names.
  std::map<std::string, DependencyEdge> inputEdgesByScope;
};

using RoleRunner = std::function<std::map<std::string, TensorBundle>(
  const RoleExecutionContext&)>;

struct RoleTiming
{
  std::string role;
  std::chrono::steady_clock::time_point queuedAt;
  std::chrono::steady_clock::time_point workerStartedAt;
  std::chrono::steady_clock::time_point startedAt;
  std::chrono::steady_clock::time_point finishedAt;
};

struct DataflowResult
{
  std::map<std::string, TensorBundle> outputsByScope;
  std::vector<RoleTiming> roleTimings;
};

class AsyncDataflowRuntime
{
public:
  explicit AsyncDataflowRuntime(std::size_t workerCount = std::thread::hardware_concurrency());

  ~AsyncDataflowRuntime();

  DataflowResult
  run(const std::string& sessionId,
      const std::vector<RoleSpec>& roles,
      const std::map<std::string, TensorBundle>& initialInputsByScope,
      const RoleRunner& runner);

private:
  struct RunState
  {
    std::string sessionId;
    RoleRunner runner;
    std::map<std::string, RoleSpec> roles;
    std::map<std::string, std::set<std::string>> consumersByScope;
    std::map<std::string, TensorBundle> initialInputsByScope;
    std::map<std::string, TensorBundle> availableByScope;
    std::map<std::string, TensorBundle> outputsByScope;
    std::set<std::string> scheduledRoles;
    std::size_t remainingRoles = 0;
    std::vector<RoleTiming> roleTimings;
    std::optional<std::exception_ptr> failure;
    std::mutex mutex;
    std::condition_variable doneCv;
  };

  struct WorkItem
  {
    std::shared_ptr<RunState> state;
    std::string role;
    std::chrono::steady_clock::time_point queuedAt;
  };

  static bool
  readyToRun(const RunState& state, const RoleSpec& role);

  static RoleExecutionContext
  makeContext(const RunState& state, const RoleSpec& role);

  static void
  publishToRun(RunState& state, const std::string& scope, const TensorBundle& bundle);

  void
  scheduleRole(const std::shared_ptr<RunState>& state, const std::string& role);

  void
  workerLoop();

  void
  execute(const WorkItem& item);

  static void
  failRun(RunState& state, std::exception_ptr failure);

private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<WorkItem> m_queue;
  std::vector<std::thread> m_workers;
  bool m_stopping = false;
};

double
durationMs(std::chrono::steady_clock::time_point start,
           std::chrono::steady_clock::time_point end);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_ASYNC_DATAFLOW_RUNTIME_HPP
