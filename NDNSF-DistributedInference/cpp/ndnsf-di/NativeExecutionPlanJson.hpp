#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantTypes.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"

#include <boost/property_tree/ptree_fwd.hpp>

#include <atomic>
#include <chrono>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <variant>

namespace ndnsf::di {

struct NativeAssemblyTensorContractV3
{
  std::string name;
  std::string dtype;
  // Wire identity distinguishes numeric dimensions from symbolic strings.
  std::vector<std::variant<std::int64_t, std::string>> shape;
};

struct NativeSelectionRoleV3
{
  std::string role;
  std::string selectedRole;
  std::uint64_t rank = 0;
  std::uint64_t layerBegin = 0;
  std::uint64_t layerEnd = 0;
  std::string backend;
  std::vector<std::string> deviceSet;
  std::uint64_t requiredDeviceMemoryMb = 0;
  std::string artifactDigest;
  std::string recipeDigest;
  std::string roleKind;
  std::string adapterId;
  std::string adapterVersion;
  std::string modelManifestDigest;
  std::string artifactProfileDigest;
  std::string graphDigest;
  std::string canonicalInitializerDigest;
  std::string adapterDescriptorDigest;
  std::string assemblerDescriptorDigest;
  std::string backendAbi;
  std::vector<std::uint64_t> nodeIndices;
  std::vector<NativeAssemblyTensorContractV3> expectedInputs;
  std::vector<NativeAssemblyTensorContractV3> expectedOutputs;
  // Internal worker binding: the parent rebuilt this role from the
  // authenticated post-Selection material manifest, so the worker receives
  // a compact role graph rather than the complete canonical graph.
  bool materializedRole = false;
  std::string precision;
  std::string quantization;
  std::string layout;
  std::string padding;
  std::string protectionEpoch = "plaintext-v1";
  // Adapter-declared terminal postprocessing. NATIVE_POSTPROCESS has no
  // model-layer artifact; the Provider consumes dependency tensors directly.
  std::string mergeKind;
  std::string postprocessIdentity;
  std::string postprocessOutputName;
  double postprocessConfidenceThreshold = 0.0;
  std::string postprocessSort;
  std::uint64_t maxSourceBytes = 0;
  std::uint64_t maxAssembledBytes = 0;
  std::uint64_t maxNodes = 0;
};

struct NativeGenerationExecutionContractV1
{
  bool enabled = false;
  std::string mode;
  std::size_t maxGeneratedTokens = 0;
  std::string tokenInputName;
  std::vector<std::string> stateInputNames;
  std::vector<std::string> stateOutputNames;
  // Authenticated dynamic-past successor mapping. The value uses the same
  // canonical comma-separated input=output form consumed by the ONNX runner.
  // Empty preserves the legacy *_in/*_out contract for existing fixtures.
  std::string stateSuccessorMap;
  std::string positionInputPolicy;
  std::string attentionMaskInputName;
  std::string positionIdsInputName;
  std::string cachePositionInputName;
  std::vector<std::int64_t> eosTokenIds;
  std::string samplingDigest;
  std::string tokenizerDigest;
  std::string samplingMode = "Greedy";
  double samplingTemperature = 0.0;
  std::uint64_t samplingTopK = 1;
  double samplingTopP = 1.0;
  double samplingRepetitionPenalty = 1.0;
  std::uint64_t samplingSeed = 1'750'001;
  std::vector<std::string> stopStrings;
  std::string generationId;
  std::vector<std::int64_t> committedPrefixTokenIds;
  std::uint64_t streamingOperationStride = 0;
};

struct NativeFixedExecutionTemplateV1;
class NativeFixedPlacementCoreV1;

struct NativeSelectionProjectionV3
{
  std::string provider;
  std::string requestId;
  // The canonical root assigned by NDNSF Core. This is populated from the
  // authenticated CollaborationAssignment after Selection; it is deliberately
  // not requester-controlled JSON so a Provider assembly factory can fetch
  // only the root bound to its assignment.
  std::string canonicalArtifactName;
  std::uint64_t attempt = 0;
  std::string planCoreDigest;
  std::string planDigest;
  std::string ackClosedDigest;
  std::string offerDigest;
  std::string securityPolicySnapshotDigest;
  std::string requestContractDigest;
  std::uint64_t deadlineMs = 0;
  std::string groupCapabilityV1;
  bool hasGrantBinding = false;
  std::string grantName;
  std::string grantDigest;
  NativeSelectionRoleV3 selectedRole;
  NativeExecutionRoleV3 executionRole;
  NativeSelectionRoleV3 assembly;
  NativeRoleDataflowContractV3 dataflow;
  NativeDeviceBindingV3 deviceBinding;
  NativeGenerationExecutionContractV1 generationContract;
  std::optional<ConversationStateReferenceV1> conversationStateReference;
  std::optional<ConversationTurnBindingV1> conversationTurnBinding;
  NativeExecutionPlan plan;

  // A protected grant may be bound to a process-local conversation lease.
  // The surrounding projection remains turn-scoped.
  std::optional<NativeGrantLeaseScope> grantLeaseScope;

  // Runtime-only state. Excluded from canonical JSON; all runner rebuilds for
  // this authenticated Selection share the same operation sequence.
  std::shared_ptr<std::atomic<std::uint64_t>> assemblyProgressSequence;

  // Cold transport opt-in: encode the identical assembly contract once in
  // roles[0]. Legacy complete projections remain readable and encodable.
  bool compactAssembly = false;

  // Immutable storage shared by invocation-local projection copies. This is
  // storage only: the current invocation must still pass authorization checks.
  std::shared_ptr<const NativeSelectionRoleV3> fixedRoleOwner;
  // Pinned by this request, independently of shared cache eviction/replacement.
  std::shared_ptr<const NativeFixedExecutionTemplateV1> fixedExecutionTemplateOwner;
  // Filled by the sealer or the validated fixed source, never inferred from
  // request-local endpoint names.
  std::map<std::string, std::string> fixedProviderByRole;
  const std::map<std::string, std::string>& providerOwnership() const;
  // Runtime identity set only by the cached invocation decoder.
  std::string fixedRoleReference;

  const NativeSelectionRoleV3& selectedRoleContract() const
  {
    return fixedRoleOwner ? *fixedRoleOwner : selectedRole;
  }

  const NativeSelectionRoleV3& assemblyContract() const
  {
    return fixedRoleOwner ? *fixedRoleOwner : assembly;
  }

  // Call only after the complete role contract and invocation bindings have
  // been validated. Copies keep the same physically const role allocation.
  std::shared_ptr<const NativeSelectionRoleV3> shareFixedRoleStorage();
};

/** Immutable recovery source; servingLease is local and never encoded. */
struct NativeFixedRoleSourceV1
{
  std::string dataName;
  std::string plaintextDigest;
  std::uint64_t plaintextSize = 0;
  std::shared_ptr<void> servingLease;
};

/** Admit the original assignment DATA prefix before fetch or cache mutation. */
void nativeValidateFixedRoleSourceProducer(const NativeFixedRoleSourceV1& source,
  const std::string& requester, const std::string& role);

bool nativeSelectionRoleContractsEqualV3(
  const NativeSelectionRoleV3& left, const NativeSelectionRoleV3& right);

std::vector<std::string>
stringArrayFromJson(const boost::property_tree::ptree& node, const std::string& key);

/** Encode a complete V3 contract and validate it through the production parser.
 * Provider-only canonicalArtifactName and legacy plan metadata are not wire fields.
 */
std::string nativeSelectionProjectionV3ToJson(const NativeSelectionProjectionV3& projection);

/** Immutable execution configuration. Names, input and deadlines use fixed
 * placeholders here and are rebound by each authenticated invocation. */
struct NativeFixedExecutionTemplateV1
{
  std::map<std::string, std::string> providerByRole;
  std::vector<NativeDependencySpec> dependencies;
  NativeRoleDataflowContractV3 dataflow;
  NativeGenerationExecutionContractV1 generation;
  // Complete repeated ROLE epochs are stored once; invocation supplies budget.
  bool expandRoleEpochs = false;
};

/** Fixed storage is a decoded contract, never invocation authority. */
class NativeFixedRoleAssignmentV1 final
{
public:
  const std::string& provider() const { return m_provider; }
  const std::string& digest() const { return m_digest; }
  const std::shared_ptr<const NativeSelectionRoleV3>& roleOwner() const { return m_role; }
  // Derived once from the validated fixed role; it carries no call authority.
  const NativeExecutionRoleV3& executionRole() const { return m_executionRole; }
  const std::shared_ptr<const NativeFixedExecutionTemplateV1>& executionTemplate() const { return m_template; }
  bool matchesExecutionTemplate(const NativeSelectionProjectionV3& projection) const;

private:
  friend std::shared_ptr<const NativeFixedRoleAssignmentV1>
  nativeFixedRoleAssignmentV1FromProjection(NativeSelectionProjectionV3 projection);
  friend std::shared_ptr<const NativeFixedRoleAssignmentV1>
  nativeFixedRoleAssignmentV1FromJson(std::istream& input);
  NativeFixedRoleAssignmentV1(std::string provider, std::string digest,
                             std::shared_ptr<const NativeSelectionRoleV3> role,
                             std::shared_ptr<const NativeFixedExecutionTemplateV1> executionTemplate);
  std::string m_provider;
  std::string m_digest;
  std::shared_ptr<const NativeSelectionRoleV3> m_role;
  NativeExecutionRoleV3 m_executionRole;
  std::shared_ptr<const NativeFixedExecutionTemplateV1> m_template;
};

/** Publication metadata from an already validated fixed role/model owner.
 * Used only on a source cache miss; never infers a model from a file name. */
ndn_service_framework::PublishedContentName nativeFixedRoleSourceContentName(
  const NativeSelectionProjectionV3& projection, const NativeFixedRoleAssignmentV1& fixed);

/** Cold-only: validates the complete projection before fixing its role. */
std::shared_ptr<const NativeFixedRoleAssignmentV1>
nativeFixedRoleAssignmentV1FromProjection(NativeSelectionProjectionV3 projection);

std::string nativeFixedRoleAssignmentV1ToJson(const NativeFixedRoleAssignmentV1& assignment);
std::shared_ptr<const NativeFixedRoleAssignmentV1> nativeFixedRoleAssignmentV1FromJson(std::istream& input);

/** Current invocation bindings with a reference to an already fixed role. */
std::string nativeSelectionInvocationV1ToJson(
  const NativeSelectionProjectionV3& projection,
  const NativeFixedRoleAssignmentV1& fixedAssignment,
  const NativeFixedRoleSourceV1* source = nullptr);

NativeSelectionProjectionV3 nativeSelectionInvocationV1FromJson(
  std::istream& input, const std::string& selectedRole,
  const NativeFixedRoleAssignmentV1& fixedAssignment);

// Checks current binding shape before any fixed-source I/O. This is not an
// authorization check and does not substitute for the cached semantic decoder.
void validateNativeSelectionInvocationBindingShapeV1(const NativeJson& value);

/** Process-local storage only. Every invocation still requires fresh validation. */
class NativeFixedRoleAssignmentCache final
{
public:
  explicit NativeFixedRoleAssignmentCache(
    std::size_t capacity = 32,
    std::chrono::milliseconds retention = std::chrono::minutes(10));

  std::shared_ptr<const NativeFixedRoleAssignmentV1> find(
    const std::string& requester, const std::string& service,
    const std::string& provider, const std::string& role,
    const std::string& digest);

  std::shared_ptr<const NativeFixedRoleAssignmentV1> findMatching(
    const std::string& requester, const std::string& service,
    const std::string& provider, const NativeSelectionRoleV3& role);
  std::shared_ptr<const NativeFixedRoleAssignmentV1> findMatching(
    const std::string& requester, const std::string& service,
    const NativeSelectionProjectionV3& projection);

  // Installation follows the caller's current authorization checks.
  std::shared_ptr<const NativeFixedRoleAssignmentV1> remember(
    const std::string& requester, const std::string& service,
    std::shared_ptr<const NativeFixedRoleAssignmentV1> assignment);

  void clear();
  std::shared_ptr<const NativeFixedPlacementCoreV1> placementCore(
    const std::string& requester, const std::string& service);
  void rememberPlacementCore(const std::string& requester, const std::string& service,
    std::shared_ptr<const NativeFixedPlacementCoreV1> owner);

  std::shared_ptr<const NativeFixedRoleSourceV1> source(
    const std::string& requester, const std::string& service,
    const NativeFixedRoleAssignmentV1& assignment);
  std::shared_ptr<const NativeFixedRoleSourceV1> rememberSource(
    const std::string& requester, const std::string& service,
    const NativeFixedRoleAssignmentV1& assignment, NativeFixedRoleSourceV1 source);

private:
  using Key = std::tuple<std::string, std::string, std::string, std::string>;
  struct Entry {
    std::shared_ptr<const NativeFixedRoleAssignmentV1> assignment;
    std::chrono::steady_clock::time_point expires;
    std::shared_ptr<const NativeFixedRoleSourceV1> source;
    std::shared_ptr<const NativeFixedPlacementCoreV1> placementCore;
  };
  void pruneExpired(std::chrono::steady_clock::time_point now);
  static void validateScope(const std::string& requester, const std::string& service);
  std::size_t m_capacity;
  std::chrono::milliseconds m_retention;
  std::mutex m_mutex;
  std::map<Key, Entry> m_entries;
};

std::map<std::string, NativeExecutionPlan>
nativeExecutionPlansByServiceFromJson(std::istream& input);

bool nativeFixedDependencyContractsEqualV1(const NativeDependencySpec& left,
                                         const NativeDependencySpec& right);
bool nativeFixedGenerationInterfacesEqualV1(const NativeGenerationExecutionContractV1& left,
                                          const NativeGenerationExecutionContractV1& right);
bool nativeSelectionRoleContractsEqualExceptCertificatesV3(const NativeSelectionRoleV3& left,
  const NativeSelectionRoleV3& right, bool ignoreManifest, bool ignoreRecipe);

NativeExecutionPlan
nativeExecutionPlanForServiceFromJson(std::istream& input, const std::string& serviceName);

NativeSelectionProjectionV3
nativeSelectionProjectionV3FromJson(std::istream& input,
                                    const std::string& selectedRole);

/** Validate the global one-projection-per-role cover and complete named tensor
 * graph after all per-Provider projections have been decoded. Multiple roles
 * may be projected to one Provider when placement assigns them there.
 */
void
validateNativeSelectionProjectionSetV3(
  const std::vector<NativeSelectionProjectionV3>& projections);

/**
 * Project only the local role's sealed mayPublish/mustFetch tensor endpoints
 * into executable dependency edges. The legacy plan.dependencies graph is not
 * authoritative for a V3 Selection.
 */
RoleSpec
roleSpecFromSelectionProjectionV3(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider = "");

/** Select one bounded generation epoch from the exact sealed endpoint set. */
RoleSpec
roleSpecFromSelectionProjectionV3(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider,
  std::uint64_t sequence);

/**
 * Request-local role factory for an authenticated Selection projection.
 *
 * The first call performs the complete generation-endpoint validation. Later
 * calls retain the exact projection and only materialize the requested epoch;
 * callers must not mutate the projection after constructing this factory.
 */
class NativeSelectionProjectionRoleFactory final
{
public:
  NativeSelectionProjectionRoleFactory(
    NativeSelectionProjectionV3 projection,
    std::string localProvider);

  RoleSpec
  operator()(std::uint64_t sequence) const;

private:
  struct State;
  std::shared_ptr<State> m_state;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
