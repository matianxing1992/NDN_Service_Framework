#ifndef NDNSF_DI_NATIVE_PLANNING_HPP
#define NDNSF_DI_NATIVE_PLANNING_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <variant>

namespace ndnsf::di {

class NativeAdmittedOfferV3;
struct NativeOfferBindingContext;
struct NativeSelectionRoleV3;
struct NativeRolePlacementProposalV3;

struct NativeStrategyIdentity
{
  std::string name;
  std::string version;
  std::string configurationDigest;
  /** Splitter determinism is part of candidate identity, not placement wire metadata. */
  bool deterministic = true;

  void validate() const;
};

struct NativeAdapterDescriptor
{
  /** Nonempty registry identity supplied by the inspected adapter. */
  std::string name;
  /** Nonempty adapter implementation version, independent of the model revision. */
  std::string version;
  /** Canonical SHA-256 identity of the adapter's configured state. */
  std::string stateDigest;
  /** Nonempty adapter ABI contract identifier supplied by its implementation. */
  std::string abi;
  /** Nonempty ordered format capabilities; model validation requires membership. */
  std::vector<std::string> modelFormats;
  /** Nonempty ordered task capabilities; preserved verbatim in descriptor identity. */
  std::vector<std::string> tasks;
  /** Nonempty ordered execution backend capabilities; not an authorization grant. */
  std::vector<std::string> backends;
  /** Nonempty ordered precision capabilities; model validation requires membership. */
  std::vector<std::string> precisions;
  /** Canonical SHA-256 identity of accepted application input schemas. */
  std::string inputSchemaDigest;
  /** Canonical SHA-256 identity of accepted application option schemas. */
  std::string optionsSchemaDigest;
  /** Canonical SHA-256 identity of the adapter's result schema. */
  std::string resultSchemaDigest;
  /** Canonical SHA-256 identity of its inspected graph schema. */
  std::string graphSchemaDigest;
  /** Canonical SHA-256 identity of its split candidate schema. */
  std::string splitSchemaDigest;
  /** Canonical SHA-256 identity of its persistent/model state schema. */
  std::string stateSchemaDigest;
  /** Whether the adapter declares graph inspection support; part of its identity. */
  bool graphInspectable = false;
  /** Whether the adapter declares model splitting support; part of its identity. */
  bool splittable = false;
  /** Whether its analysis is deterministic; mirrors the maintained default. */
  bool deterministicAnalysis = true;

  /** Reject incomplete identities or malformed digests before serialization. */
  void validate() const;
  /** Return the maintained Python AdapterDescriptor schema's canonical bytes. */
  std::string canonicalJson() const;
  /** SHA-256 of canonicalJson(), including every declared capability/schema. */
  std::string descriptorDigest() const;
};

struct NativeModelDescriptor
{
  std::string modelName;
  std::string contentDigest;
  std::string semanticsDigest;
  std::string graphDigest;
  std::string modelFormat;
  std::string precision;
  std::string adapterId;
  std::string adapterVersion;
  /** Full source-owned adapter contract; flat identity aliases must agree. */
  NativeAdapterDescriptor adapter;
  /** Optional source revision retained when a request is copied into a base descriptor. */
  std::string sourceRevision;

  void validate() const;
  /** Canonical Python ModelDescriptor schema, including adapter and source_revision. */
  std::string canonicalJson() const;
  /** Descriptor identity; distinct from the model's contentDigest and wire aliases. */
  std::string modelDigest() const;
};

struct NativeGraphNode
{
  std::string id;
  std::string opType;
  std::uint64_t ordinal = 0;
};

struct NativeTensorContract
{
  std::string name;
  std::string dtype;
  std::vector<std::variant<std::int64_t, std::string>> shape;
  std::optional<std::uint64_t> estimatedBytes;

  void validate() const;
};

struct NativeGraphEdge
{
  std::string id;
  std::string producer;
  std::vector<std::string> consumers;
  NativeTensorContract tensor;
};

struct NativeGraphSnapshot
{
  std::string graphDigest;
  std::vector<NativeGraphNode> nodes;
  std::vector<std::string> topologicalOrder;
  std::vector<std::string> legalCutEdges;
  std::vector<NativeTensorContract> modelInputs;
  std::vector<NativeTensorContract> modelOutputs;
  std::vector<NativeGraphEdge> edges;

  void validate(const NativeModelDescriptor& model) const;
};

struct NativeCandidateBudget
{
  std::size_t maxCandidates = 1;
  std::uint64_t maxPolicyMs = 100;
  std::size_t maxReentries = 1;

  void validate() const;
};

struct NativeRoleResourceRequirement
{
  std::vector<std::string> backends;
  // Absent means unknown, not a zero-cost allocation. Adapters must explicitly
  // supply zero for budgets which do not apply to their execution profile.
  std::optional<std::uint64_t> weightBytes;
  std::optional<std::uint64_t> workspaceBytes;
  std::optional<std::uint64_t> kvBytes;
  std::optional<std::uint64_t> activationBytes;
  std::optional<std::uint64_t> transientBytes;
  double safetyMargin = 1.1;

  void validate() const;
  /** Complete maintained RoleResourceRequirement schema, preserving nulls. */
  std::string canonicalJson() const;
  /** Python-compatible integer peak; null if unknown, throws on uint64 overflow. */
  std::optional<std::uint64_t> estimatedPeakGpuMemoryBytes() const;
};

struct NativeProviderPlanningView
{
  std::string provider;
  std::string offerDigest;
  std::vector<std::string> acceptedRoles;
  std::vector<std::string> backends;
  std::vector<std::string> residencyDigests;
  std::uint64_t freeBytes = 0;
  std::uint64_t resourceSequence = 0;
  bool preparationAccepted = false;
  bool executionAllowed = false;

  void validate() const;
};

struct NativePlanningSnapshot
{
  NativeModelDescriptor model;
  NativeGraphSnapshot graph;
  std::vector<NativeProviderPlanningView> offers;
  std::string requestId;
  std::uint64_t attempt = 1;
  std::string ackClosedDigest;
  std::chrono::steady_clock::time_point deadline;

  void validate() const;
};

/** Candidate-stage rank topology; redistribution values are shared with dataflow. */
struct NativeHybridPlan
{
  std::uint64_t stages = 0;
  std::vector<std::uint64_t> tensorDegrees;
  std::vector<std::string> rankLabels;
  std::vector<RedistributionSpec> redistributions;

  /** Validate canonical rank labels and complete adjacent-stage redistribution cover. */
  void validate() const;
  std::string canonicalJson() const;
};

using NativeEstimatedCost = std::variant<std::monostate, std::int64_t, std::uint64_t, double>;

struct NativeSplitCandidate
{
  std::string source;
  NativeStrategyIdentity splitter;
  NativeModelDescriptor model;
  std::string graphDigest;
  NativeExecutionPlan executionPlan;
  std::map<std::string, std::string> fragmentsByRole;
  std::map<std::string, std::vector<std::string>> artifactsByRole;
  std::map<std::string, NativeRoleResourceRequirement> requirementsByRole;
  std::vector<std::string> crossPartitionTensors;
  std::map<std::string, std::uint64_t> tensorDegreesByRole;
  std::map<std::string, std::vector<std::string>> rankArtifactDigestsByRole;
  int selectionPriority = 0;
  std::string inputIngressRole;
  std::string resultEgressRole;
  std::string mergeKind;
  /** Owned JSON object; parsed strictly and canonicalized into the complete identity. */
  std::string postprocessingJson = "{}";
  std::map<std::string, NativeEstimatedCost> estimatedCosts;
  std::optional<NativeHybridPlan> hybridPlan;
  /** Caller-supplied claim, verified against computedDigest() at every candidate boundary. */
  std::string candidateDigest;

  // Planning node IDs, not canonical ONNX assembly indices. The adapter owns
  // the conversion between these two graph identity spaces.
  std::map<std::string, std::string> nodeRoles;
  std::map<std::string, std::vector<NativeTensorContract>> roleStateInputsByRole;
  std::map<std::string, std::vector<NativeTensorContract>> roleStateOutputsByRole;

  void validate(const NativeGraphSnapshot& graph) const;
  /** Complete maintained SplitCandidate schema; excludes its derived candidateDigest. */
  std::string canonicalJson() const;
  std::string computedDigest() const;
};

struct NativePlacementProposal
{
  std::string requestId;
  std::uint64_t attempt = 1;
  std::string modelDigest;
  std::string graphDigest;
  std::string candidateDigest;
  NativeStrategyIdentity strategy;
  NativeExecutionPlan executionPlan;
  NativeProviderAssignment assignment;

  void validate(const NativePlanningSnapshot& snapshot,
                const NativeSplitCandidate& candidate) const;
};

class NativeModelSplitStrategy
{
public:
  virtual ~NativeModelSplitStrategy() = default;
  virtual NativeStrategyIdentity identity() const = 0;
  virtual std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor& model,
    const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget) const = 0;
};

class NativePlacementStrategy
{
public:
  virtual ~NativePlacementStrategy() = default;
  virtual NativeStrategyIdentity identity() const = 0;
  virtual NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
    const std::vector<NativeSelectionRoleV3>& roles,
    const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs) const = 0;
};

class NativePreSplitFirstPlacement final : public NativePlacementStrategy
{
public:
  explicit NativePreSplitFirstPlacement(NativeStrategyIdentity identity = {
    "native-pre-split-first", "1",
    "sha256:34a7cfcdea48eecebb78f1118ce89cf0b546e4cf08ca99da943daeff28a0dac9"});

  NativeStrategyIdentity identity() const override;
  // Legacy concrete entry for migration fixtures; not the injectable strategy contract.
  NativePlacementProposal propose(const NativePlanningSnapshot& snapshot,
                                  const NativeSplitCandidate& candidate) const;

  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
    const std::vector<NativeSelectionRoleV3>& roles,
    const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs) const override;

private:
  NativeStrategyIdentity m_identity;
};

class NativeModelAdapter
{
public:
  virtual ~NativeModelAdapter() = default;
  virtual std::string adapterId() const = 0;
  virtual std::string adapterVersion() const = 0;
  virtual NativeModelDescriptor inspect(const std::string& modelName,
                                        const std::string& modelDigest) const = 0;
  virtual std::vector<std::uint8_t> encodeInput(
    const std::vector<std::uint8_t>& applicationInput) const = 0;
  virtual std::vector<std::uint8_t> decodeResult(
    const std::vector<std::uint8_t>& nativeResult) const = 0;
};

class NativeAdapterRegistry
{
public:
  void registerAdapter(std::shared_ptr<const NativeModelAdapter> adapter);
  void freeze();
  bool frozen() const noexcept { return m_frozen; }
  std::shared_ptr<const NativeModelAdapter> find(const std::string& adapterId) const;

private:
  std::map<std::string, std::shared_ptr<const NativeModelAdapter>> m_adapters;
  bool m_frozen = false;
};

std::string nativePlanningDigest(const std::string& canonical);
// Hash owned binary sources without allocating an intermediate string.
std::string nativePlanningDigest(const std::uint8_t* data, std::size_t size);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_PLANNING_HPP
