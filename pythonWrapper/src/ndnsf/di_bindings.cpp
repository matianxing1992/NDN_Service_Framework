#include "di_bindings.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include "ndn-service-framework/ControllerVersion.hpp"
#include "ndn-service-framework/InvocationStream.hpp"

#include <ndn-cxx/encoding/block.hpp>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace di = ndnsf::di;

namespace {

py::object
blockWireOrNone(const std::optional<ndn::Block>& block)
{
  if (!block)
    return py::none();
  auto copy = *block;
  if (!copy.hasWire())
    copy.encode();
  return py::bytes(reinterpret_cast<const char*>(copy.data()), copy.size());
}

void
setBlockWire(std::optional<ndn::Block>& destination, const py::object& value)
{
  if (value.is_none()) {
    destination.reset();
    return;
  }
  const std::string bytes = value.cast<py::bytes>();
  if (bytes.empty())
    throw std::invalid_argument("event_key_grant_wire must contain one TLV block");
  ndn::Block block(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  block.parse();
  destination = std::move(block);
}

const char*
requestStatusName(di::NativeRequestStatus status)
{
  switch (status) {
    case di::NativeRequestStatus::Pending: return "PENDING";
    case di::NativeRequestStatus::Succeeded: return "SUCCEEDED";
    case di::NativeRequestStatus::Failed: return "FAILED";
    case di::NativeRequestStatus::Cancelled: return "CANCELLED";
  }
  return "UNKNOWN";
}

} // namespace

void
bindDistributedInference(py::module_& module)
{
  py::register_exception<di::NativeDiError>(module, "NativeDiError");

  py::enum_<di::NativeRequestStatus>(module, "NativeRequestStatus")
    .value("PENDING", di::NativeRequestStatus::Pending)
    .value("SUCCEEDED", di::NativeRequestStatus::Succeeded)
    .value("FAILED", di::NativeRequestStatus::Failed)
    .value("CANCELLED", di::NativeRequestStatus::Cancelled)
    .export_values();

  py::enum_<di::NativeInputTransportMode>(module, "NativeInputTransportMode")
    .value("INLINE", di::NativeInputTransportMode::Inline)
    .value("REPOSITORY_REFERENCE", di::NativeInputTransportMode::RepositoryReference)
    .export_values();

  py::enum_<ndn_service_framework::InvocationMode>(module, "NativeInvocationMode")
    .value("NORMAL", ndn_service_framework::InvocationMode::Normal)
    .value("TARGETED", ndn_service_framework::InvocationMode::Targeted)
    .export_values();

  py::class_<ndn_service_framework::ControllerVersion>(module, "NativeControllerVersion")
    .def(py::init<>())
    .def_readwrite("controller_generation_timestamp",
                   &ndn_service_framework::ControllerVersion::controllerGenerationTimestamp)
    .def_readwrite("controller_epoch",
                   &ndn_service_framework::ControllerVersion::controllerEpoch)
    .def("is_valid", &ndn_service_framework::ControllerVersion::isValid)
    .def("to_string", &ndn_service_framework::ControllerVersion::toString);

  py::class_<di::NativeGenerationExecutionContractV1>(
      module, "NativeGenerationExecutionContractV1")
    .def(py::init<>())
    .def_readwrite("enabled", &di::NativeGenerationExecutionContractV1::enabled)
    .def_readwrite("mode", &di::NativeGenerationExecutionContractV1::mode)
    .def_readwrite("max_generated_tokens",
                   &di::NativeGenerationExecutionContractV1::maxGeneratedTokens)
    .def_readwrite("token_input_name",
                   &di::NativeGenerationExecutionContractV1::tokenInputName)
    .def_readwrite("state_input_names",
                   &di::NativeGenerationExecutionContractV1::stateInputNames)
    .def_readwrite("state_output_names",
                   &di::NativeGenerationExecutionContractV1::stateOutputNames)
    .def_readwrite("eos_token_ids",
                   &di::NativeGenerationExecutionContractV1::eosTokenIds)
    .def_readwrite("sampling_digest",
                   &di::NativeGenerationExecutionContractV1::samplingDigest)
    .def_readwrite("tokenizer_digest",
                   &di::NativeGenerationExecutionContractV1::tokenizerDigest)
    .def_readwrite("sampling_mode",
                   &di::NativeGenerationExecutionContractV1::samplingMode)
    .def_readwrite("sampling_temperature",
                   &di::NativeGenerationExecutionContractV1::samplingTemperature)
    .def_readwrite("sampling_top_k",
                   &di::NativeGenerationExecutionContractV1::samplingTopK)
    .def_readwrite("sampling_top_p",
                   &di::NativeGenerationExecutionContractV1::samplingTopP)
    .def_readwrite("sampling_repetition_penalty",
                   &di::NativeGenerationExecutionContractV1::samplingRepetitionPenalty)
    .def_readwrite("sampling_seed",
                   &di::NativeGenerationExecutionContractV1::samplingSeed)
    .def_readwrite("stop_strings",
                   &di::NativeGenerationExecutionContractV1::stopStrings)
    .def_readwrite("generation_id",
                   &di::NativeGenerationExecutionContractV1::generationId)
    .def_readwrite("committed_prefix_token_ids",
                   &di::NativeGenerationExecutionContractV1::committedPrefixTokenIds)
    .def_readwrite("streaming_operation_stride",
                   &di::NativeGenerationExecutionContractV1::streamingOperationStride);

  py::class_<di::NativeConversationContinuation>(
      module, "NativeConversationContinuation")
    .def(py::init<>())
    .def_readwrite("conversation_id", &di::NativeConversationContinuation::conversationId)
    .def_readwrite("parent_context_epoch",
                   &di::NativeConversationContinuation::parentContextEpoch)
    .def_readwrite("service_name", &di::NativeConversationContinuation::serviceName)
    .def_readwrite("plan_role_map_digest",
                   &di::NativeConversationContinuation::planRoleMapDigest)
    .def_readwrite("parent_checkpoint_digest",
                   &di::NativeConversationContinuation::parentCheckpointDigest)
    .def_readwrite("request_contract_digest",
                   &di::NativeConversationContinuation::requestContractDigest)
    .def_readwrite("retention_deadline_ms",
                   &di::NativeConversationContinuation::retentionDeadlineMs)
    .def_readwrite("mode", &di::NativeConversationContinuation::mode)
    .def_readwrite("parent_checkpoint_wire",
                   &di::NativeConversationContinuation::parentCheckpointWire)
    .def_readwrite("generation_id", &di::NativeConversationContinuation::generationId)
    .def_readwrite("canonical_token_ids",
                   &di::NativeConversationContinuation::canonicalTokenIds)
    .def_readwrite("expected_roles", &di::NativeConversationContinuation::expectedRoles);

  py::class_<ndn_service_framework::StreamRequestOptions>(
      module, "NativeStreamRequestOptions")
    .def(py::init<>())
    .def_readwrite("version", &ndn_service_framework::StreamRequestOptions::version)
    .def_readwrite("mode", &ndn_service_framework::StreamRequestOptions::mode)
    .def_readwrite("generation_id", &ndn_service_framework::StreamRequestOptions::generationId)
    .def_readwrite("attempt_epoch", &ndn_service_framework::StreamRequestOptions::attemptEpoch)
    .def_readwrite("stream_epoch", &ndn_service_framework::StreamRequestOptions::streamEpoch)
    .def_readwrite("event_key_commitment",
                   &ndn_service_framework::StreamRequestOptions::eventKeyCommitment)
    .def_readwrite("deadline_epoch_ms",
                   &ndn_service_framework::StreamRequestOptions::deadlineEpochMs)
    .def_readwrite("controller_version",
                   &ndn_service_framework::StreamRequestOptions::controllerVersion)
    .def_property("event_key_grant_wire",
      [] (const ndn_service_framework::StreamRequestOptions& options) {
        return blockWireOrNone(options.eventKeyGrant);
      },
      [] (ndn_service_framework::StreamRequestOptions& options, const py::object& value) {
        setBlockWire(options.eventKeyGrant, value);
      })
    .def_readwrite("max_events", &ndn_service_framework::StreamRequestOptions::maxEvents)
    .def_readwrite("interest_window", &ndn_service_framework::StreamRequestOptions::interestWindow)
    .def_readwrite("interest_lifetime_ms",
                   &ndn_service_framework::StreamRequestOptions::interestLifetimeMs)
    .def_readwrite("max_event_retries",
                   &ndn_service_framework::StreamRequestOptions::maxEventRetries)
    .def_readwrite("publisher_queue_capacity",
                   &ndn_service_framework::StreamRequestOptions::publisherQueueCapacity)
    .def_readwrite("callback_queue_capacity",
                   &ndn_service_framework::StreamRequestOptions::callbackQueueCapacity)
    .def_readwrite("reorder_capacity",
                   &ndn_service_framework::StreamRequestOptions::reorderCapacity)
    .def_readwrite("retention_ms", &ndn_service_framework::StreamRequestOptions::retentionMs)
    .def_readwrite("completion_grace_ms",
                   &ndn_service_framework::StreamRequestOptions::completionGraceMs)
    .def_readwrite("max_event_wire_bytes",
                   &ndn_service_framework::StreamRequestOptions::maxEventWireBytes)
    .def_readwrite("allow_replacement",
                   &ndn_service_framework::StreamRequestOptions::allowReplacement)
    .def_readwrite("max_replacements",
                   &ndn_service_framework::StreamRequestOptions::maxReplacements)
    .def("validate", &ndn_service_framework::StreamRequestOptions::validate)
    .def("wire_encode", [] (const ndn_service_framework::StreamRequestOptions& options) {
      const auto wire = options.wireEncode();
      return py::bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
    })
    .def("wire_decode", [] (ndn_service_framework::StreamRequestOptions& options,
                              const py::bytes& wireBytes) {
      const std::string bytes = wireBytes;
      ndn::Block wire(ndn::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
      wire.parse();
      return options.wireDecode(wire);
    });

  py::class_<di::NativeAdapterDescriptor>(module, "NativeAdapterDescriptor")
    .def(py::init<>())
    .def_readwrite("name", &di::NativeAdapterDescriptor::name)
    .def_readwrite("version", &di::NativeAdapterDescriptor::version)
    .def_readwrite("state_digest", &di::NativeAdapterDescriptor::stateDigest)
    .def_readwrite("abi", &di::NativeAdapterDescriptor::abi)
    .def_readwrite("model_formats", &di::NativeAdapterDescriptor::modelFormats)
    .def_readwrite("tasks", &di::NativeAdapterDescriptor::tasks)
    .def_readwrite("backends", &di::NativeAdapterDescriptor::backends)
    .def_readwrite("precisions", &di::NativeAdapterDescriptor::precisions)
    .def_readwrite("input_schema_digest", &di::NativeAdapterDescriptor::inputSchemaDigest)
    .def_readwrite("options_schema_digest", &di::NativeAdapterDescriptor::optionsSchemaDigest)
    .def_readwrite("result_schema_digest", &di::NativeAdapterDescriptor::resultSchemaDigest)
    .def_readwrite("graph_schema_digest", &di::NativeAdapterDescriptor::graphSchemaDigest)
    .def_readwrite("split_schema_digest", &di::NativeAdapterDescriptor::splitSchemaDigest)
    .def_readwrite("state_schema_digest", &di::NativeAdapterDescriptor::stateSchemaDigest)
    .def_readwrite("graph_inspectable", &di::NativeAdapterDescriptor::graphInspectable)
    .def_readwrite("splittable", &di::NativeAdapterDescriptor::splittable)
    .def_readwrite("deterministic_analysis", &di::NativeAdapterDescriptor::deterministicAnalysis)
    .def("canonical_json", &di::NativeAdapterDescriptor::canonicalJson)
    .def_property_readonly("descriptor_digest", &di::NativeAdapterDescriptor::descriptorDigest);

  py::class_<di::NativeModelDescriptor>(module, "NativeModelDescriptor")
    .def(py::init<>())
    .def_readwrite("model_name", &di::NativeModelDescriptor::modelName)
    .def_readwrite("content_digest", &di::NativeModelDescriptor::contentDigest)
    .def_readwrite("semantics_digest", &di::NativeModelDescriptor::semanticsDigest)
    .def_readwrite("graph_digest", &di::NativeModelDescriptor::graphDigest)
    .def_readwrite("model_format", &di::NativeModelDescriptor::modelFormat)
    .def_readwrite("precision", &di::NativeModelDescriptor::precision)
    .def_readwrite("adapter_id", &di::NativeModelDescriptor::adapterId)
    .def_readwrite("adapter_version", &di::NativeModelDescriptor::adapterVersion)
    .def_readwrite("adapter", &di::NativeModelDescriptor::adapter)
    .def_readwrite("source_revision", &di::NativeModelDescriptor::sourceRevision)
    .def("canonical_json", &di::NativeModelDescriptor::canonicalJson)
    .def_property_readonly("model_digest", &di::NativeModelDescriptor::modelDigest);

  py::class_<di::NativeModelRef, di::NativeModelDescriptor>(module, "NativeModelRef")
    .def(py::init<>());

  py::class_<di::NativeApplicationInput>(module, "NativeApplicationInput")
    .def(py::init<>())
    .def_readwrite("task_name", &di::NativeApplicationInput::taskName)
    .def_readwrite("input_schema_digest", &di::NativeApplicationInput::inputSchemaDigest)
    .def_readwrite("options_schema_digest", &di::NativeApplicationInput::optionsSchemaDigest)
    .def_readwrite("payload", &di::NativeApplicationInput::payload)
    .def_readwrite("options", &di::NativeApplicationInput::options)
    .def_readwrite("transport_mode", &di::NativeApplicationInput::transportMode)
    .def_readwrite("repository_reference", &di::NativeApplicationInput::repositoryReference);

  py::class_<di::NativeRequestOptions>(module, "NativeRequestOptions")
    .def(py::init<>())
    .def_readwrite("timeout_ms", &di::NativeRequestOptions::timeoutMs)
    .def_readwrite("ack_timeout_ms", &di::NativeRequestOptions::ackTimeoutMs)
    .def_readwrite("task_name", &di::NativeRequestOptions::taskName)
    .def_readwrite("output_mode", &di::NativeRequestOptions::outputMode)
    .def_readwrite("generation", &di::NativeRequestOptions::generation)
    .def_readwrite("stream", &di::NativeRequestOptions::stream)
    .def_readwrite("conversation", &di::NativeRequestOptions::conversation);

  py::class_<di::NativeRequestContract>(module, "NativeRequestContract")
    .def(py::init<>())
    .def_readwrite("service_name", &di::NativeRequestContract::serviceName)
    .def_readwrite("task_name", &di::NativeRequestContract::taskName)
    .def_readwrite("adapter_name", &di::NativeRequestContract::adapterName)
    .def_readwrite("adapter_descriptor_digest", &di::NativeRequestContract::adapterDescriptorDigest)
    .def_readwrite("adapter_composition_digest", &di::NativeRequestContract::adapterCompositionDigest)
    .def_readwrite("task_descriptor_digest", &di::NativeRequestContract::taskDescriptorDigest)
    .def_readwrite("generation_mode", &di::NativeRequestContract::generationMode);

  py::class_<di::NativeSecurityPolicySnapshot>(module, "NativeSecurityPolicySnapshot")
    .def(py::init<>())
    .def_readwrite("policy_digest", &di::NativeSecurityPolicySnapshot::policyDigest)
    .def_readwrite("require_protected_artifacts", &di::NativeSecurityPolicySnapshot::requireProtectedArtifacts);

  py::class_<di::NativeCandidateBudget>(module, "NativeCandidateBudget")
    .def(py::init<>())
    .def_readwrite("max_candidates", &di::NativeCandidateBudget::maxCandidates)
    .def_readwrite("max_policy_ms", &di::NativeCandidateBudget::maxPolicyMs)
    .def_readwrite("max_reentries", &di::NativeCandidateBudget::maxReentries);

  py::class_<di::NativeStateTensorMapping>(module, "NativeStateTensorMapping")
    .def(py::init<>())
    .def_readwrite("inputs", &di::NativeStateTensorMapping::inputs)
    .def_readwrite("outputs", &di::NativeStateTensorMapping::outputs);

  py::class_<di::NativeRequestPreparation,
             std::shared_ptr<di::NativeRequestPreparation>>(
    module, "NativeRequestPreparation");

  py::class_<di::NativeCanonicalPreparationCatalog,
             std::shared_ptr<di::NativeCanonicalPreparationCatalog>>(
    module, "NativeCanonicalPreparationCatalog")
    .def_property_readonly("adapters", &di::NativeCanonicalPreparationCatalog::adapters);

  py::class_<di::NativeAuthenticatedGrantClient,
             std::shared_ptr<di::NativeAuthenticatedGrantClient>>(
    module, "NativeAuthenticatedGrantClient");

  py::class_<di::NativeOfferAdmission,
             std::shared_ptr<di::NativeOfferAdmission>>(
    module, "NativeOfferAdmission")
    .def(py::init<const std::string&, const std::map<std::string, std::string>&,
                  const std::string&>(),
         py::arg("policy_json"), py::arg("public_key_pem_by_id"),
         py::arg("candidate_digest"));

  py::class_<di::NativeRequestCatalog>(module, "NativeRequestCatalog")
    .def_readonly("model", &di::NativeRequestCatalog::model)
    // NativeInspectedModel is intentionally kept as a C++ inspection detail.
    // Callers need only the immutable model descriptor to submit a request;
    // exporting a copied NativeModelRef avoids leaking an unbound graph type
    // or asking Python to reconstruct descriptor identity.
    .def_property_readonly("model_ref", [] (const di::NativeRequestCatalog& catalog) {
      di::NativeModelRef ref;
      static_cast<di::NativeModelDescriptor&>(ref) = catalog.model.descriptor;
      return ref;
    })
    .def_property_readonly("model_manifest_digest", [] (const di::NativeRequestCatalog& catalog) {
      return catalog.model.modelManifestDigest;
    })
    .def_property_readonly("canonical_source_digest", [] (const di::NativeRequestCatalog& catalog) {
      return catalog.model.canonicalSourceDigest;
    })
    .def_property_readonly("canonical_initializer_object_digest", [] (const di::NativeRequestCatalog& catalog) {
      return catalog.model.canonicalInitializerObjectDigest;
    })
    .def_readonly("preparation", &di::NativeRequestCatalog::preparation)
    .def_readonly("splitter", &di::NativeRequestCatalog::splitter)
    .def_readonly("state_mapping", &di::NativeRequestCatalog::stateMapping)
    .def_static("load", [](const std::string& configuration_json,
                            const py::bytes& model_bytes,
                            const py::object& initializer_bytes,
                            std::uint64_t max_source_bytes,
                            std::uint64_t max_assembled_bytes) {
      if (!max_source_bytes || !max_assembled_bytes)
        throw std::invalid_argument("native catalog limits must be positive");
      di::NativeCanonicalSource source;
      const std::string model = model_bytes;
      source.modelBytes.assign(model.begin(), model.end());
      if (!initializer_bytes.is_none()) {
        const std::string initializer = initializer_bytes.cast<py::bytes>();
        source.initializerBytes.emplace(initializer.begin(), initializer.end());
      }
      const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::hours(1);
      di::NativeAssemblyControl control{
        deadline, [] {}, max_source_bytes, max_assembled_bytes};
      return di::NativeRequestCatalog::load(configuration_json,
                                            std::move(source), control);
    }, py::arg("configuration_json"), py::arg("model_bytes"),
       py::arg("initializer_bytes") = py::none(),
       py::arg("max_source_bytes") = 256ULL * 1024ULL * 1024ULL,
       py::arg("max_assembled_bytes") = 512ULL * 1024ULL * 1024ULL);

  py::class_<di::NativeRequestRuntime>(module, "NativeRequestRuntime")
    .def(py::init<>())
    .def_readwrite("contract", &di::NativeRequestRuntime::contract)
    .def_readwrite("requester_identity", &di::NativeRequestRuntime::requesterIdentity)
    .def_readwrite("protection_epoch", &di::NativeRequestRuntime::protectionEpoch)
    .def_readwrite("input_layout_digest", &di::NativeRequestRuntime::inputLayoutDigest)
    .def_readwrite("security", &di::NativeRequestRuntime::security)
    .def_readwrite("budget", &di::NativeRequestRuntime::budget)
    .def_readwrite("grants", &di::NativeRequestRuntime::grants)
    .def_readwrite("catalog", &di::NativeRequestRuntime::catalog)
    .def_readwrite("state_mapping", &di::NativeRequestRuntime::stateMapping)
    .def_readwrite("no_progress_ms", &di::NativeRequestRuntime::noProgressMs)
    .def_readwrite("max_segments", &di::NativeRequestRuntime::maxSegments);

  module.def("native_request_runtime_from_json",
             [](const std::string& configuration_json,
                const di::NativeRequestCatalog& catalog,
                std::shared_ptr<const di::NativeAuthenticatedGrantClient> grants) {
               return di::nativeRequestRuntimeFromJson(configuration_json, catalog,
                                                        std::move(grants));
             }, py::arg("configuration_json"), py::arg("catalog"), py::arg("grants"));

  py::class_<di::NativeInferenceResult>(module, "NativeInferenceResult")
    .def(py::init<>())
    .def_readonly("payload", &di::NativeInferenceResult::payload)
    .def_readonly("model_digest", &di::NativeInferenceResult::modelDigest)
    .def_readonly("plan_digest", &di::NativeInferenceResult::planDigest);

  py::class_<di::NativeInferenceEvent>(module, "NativeInferenceEvent")
    .def_readonly("request_id", &di::NativeInferenceEvent::requestId)
    .def_readonly("payload", &di::NativeInferenceEvent::payload)
    .def_readonly("terminal", &di::NativeInferenceEvent::terminal);

  py::class_<di::NativeAdapterRegistry,
             std::shared_ptr<di::NativeAdapterRegistry>>(module, "NativeAdapterRegistry")
    .def(py::init<>())
    .def("freeze", &di::NativeAdapterRegistry::freeze)
    .def("find", &di::NativeAdapterRegistry::find)
    .def_property_readonly("frozen", &di::NativeAdapterRegistry::frozen);

  py::class_<di::NativeModelAdapter,
             std::shared_ptr<di::NativeModelAdapter>>(module, "NativeModelAdapter");

  py::class_<di::NativePlacementStrategy,
             std::shared_ptr<di::NativePlacementStrategy>>(module,
                                                           "NativePlacementStrategy");
  py::class_<di::NativeModelSplitStrategy,
             std::shared_ptr<di::NativeModelSplitStrategy>>(module,
                                                           "NativeModelSplitStrategy");

  py::class_<di::qwen::NativeQwenLayerSplit, di::NativeModelSplitStrategy,
             std::shared_ptr<di::qwen::NativeQwenLayerSplit>>(
    module, "NativeQwenLayerSplit")
    .def(py::init<std::vector<di::qwen::NativeQwenLayerSplit::LayerRange>,
                  std::map<std::string, std::string>,
                  std::map<std::string, std::uint64_t>,
                  std::vector<std::string>, std::vector<std::uint64_t>>(),
         py::arg("layer_ranges"), py::arg("artifact_digests_by_role"),
         py::arg("weight_bytes_by_role"),
         py::arg("roles") = std::vector<std::string>{
           "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1",
           "/LLM/Pipeline/Stage/2"},
         py::arg("tensor_degrees") = std::vector<std::uint64_t>{1, 1, 1});

  py::class_<di::yolo::NativeYoloComponentSpec>(module, "NativeYoloComponentSpec")
    .def(py::init<>())
    .def_readwrite("candidate_id", &di::yolo::NativeYoloComponentSpec::candidateId)
    .def_readwrite("priority", &di::yolo::NativeYoloComponentSpec::priority)
    .def_readwrite("roles", &di::yolo::NativeYoloComponentSpec::roles)
    .def_readwrite("node_names_by_role",
                   &di::yolo::NativeYoloComponentSpec::nodeNamesByRole)
    .def_readwrite("input_ingress_role",
                   &di::yolo::NativeYoloComponentSpec::inputIngressRole)
    .def_readwrite("result_egress_role",
                   &di::yolo::NativeYoloComponentSpec::resultEgressRole)
    .def_readwrite("merge_kind", &di::yolo::NativeYoloComponentSpec::mergeKind)
    .def_readwrite("candidate_digest",
                   &di::yolo::NativeYoloComponentSpec::candidateDigest);

  py::class_<di::yolo::NativeYoloComponentSplit, di::NativeModelSplitStrategy,
             std::shared_ptr<di::yolo::NativeYoloComponentSplit>>(
    module, "NativeYoloComponentSplit")
    .def(py::init<std::vector<di::yolo::NativeYoloComponentSpec>>(),
         py::arg("candidates"));

  py::class_<di::NativePreSplitFirstPlacement, di::NativePlacementStrategy,
             std::shared_ptr<di::NativePreSplitFirstPlacement>>(
    module, "NativePreSplitFirstPlacement")
    .def(py::init<>());

  py::class_<di::NativeInferenceHandle,
             std::shared_ptr<di::NativeInferenceHandle>>(module, "NativeInferenceHandle")
    .def_property_readonly("request_id", &di::NativeInferenceHandle::requestId)
    .def_property_readonly("status", &di::NativeInferenceHandle::status)
    .def("result", [](const di::NativeInferenceHandle& handle,
                       std::uint64_t wait_timeout_ms) {
      return handle.result(std::chrono::milliseconds(wait_timeout_ms));
    }, py::arg("wait_timeout_ms") = 0)
    .def("cancel", &di::NativeInferenceHandle::cancel)
    .def("observe", [] (di::NativeInferenceHandle& handle,
                         py::function observer) {
      if (!observer)
        throw std::invalid_argument("native observer is empty");
      handle.observe([observer = std::move(observer)] (
          const di::NativeInferenceEvent& event) {
        py::gil_scoped_acquire gil;
        py::dict value;
        value["request_id"] = event.requestId;
        value["payload"] = py::bytes(
          reinterpret_cast<const char*>(event.payload.data()),
          event.payload.size());
        value["terminal"] = event.terminal;
        observer(value);
      });
    }, py::arg("observer"))
    .def_property_readonly("status_name", [](const di::NativeInferenceHandle& handle) {
      return requestStatusName(handle.status());
    });

  py::class_<di::NativeInferenceClient,
             std::shared_ptr<di::NativeInferenceClient>>(module, "NativeInferenceClient")
    .def("close", &di::NativeInferenceClient::close)
    .def("request", &di::NativeInferenceClient::request,
         py::arg("model"), py::arg("input"), py::arg("split_strategy"),
         py::arg("placement_strategy"), py::arg("options"));
}
