#ifndef NDNSF_DI_NATIVE_PLANNING_HPP
#define NDNSF_DI_NATIVE_PLANNING_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeStrategyIdentity
{
  std::string name;
  std::string version;
  std::string configurationDigest;

  void validate() const;
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

  void validate() const;
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
  std::vector<std::int64_t> shape;
};

struct NativeGraphSnapshot
{
  std::string graphDigest;
  std::vector<NativeGraphNode> nodes;
  std::vector<std::string> topologicalOrder;
  std::vector<std::string> legalCutEdges;
  std::vector<NativeTensorContract> modelInputs;
  std::vector<NativeTensorContract> modelOutputs;

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
  std::uint64_t weightBytes = 0;
  std::uint64_t workspaceBytes = 0;
  std::uint64_t activationBytes = 0;
  std::uint64_t transientBytes = 0;
  double safetyMargin = 1.0;
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
  std::string postprocessIdentity;
  std::string candidateDigest;

  void validate(const NativeGraphSnapshot& graph) const;
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
  virtual NativePlacementProposal propose(
    const NativePlanningSnapshot& snapshot,
    const NativeSplitCandidate& candidate) const = 0;
};

class NativePreSplitFirstPlacement final : public NativePlacementStrategy
{
public:
  explicit NativePreSplitFirstPlacement(NativeStrategyIdentity identity = {
    "native-pre-split-first", "1",
    "sha256:34a7cfcdea48eecebb78f1118ce89cf0b546e4cf08ca99da943daeff28a0dac9"});

  NativeStrategyIdentity identity() const override;
  NativePlacementProposal propose(const NativePlanningSnapshot& snapshot,
                                  const NativeSplitCandidate& candidate) const override;

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
  std::shared_ptr<const NativeModelAdapter> find(const std::string& adapterId) const;

private:
  std::map<std::string, std::shared_ptr<const NativeModelAdapter>> m_adapters;
};

std::string nativePlanningDigest(const std::string& canonical);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_PLANNING_HPP
