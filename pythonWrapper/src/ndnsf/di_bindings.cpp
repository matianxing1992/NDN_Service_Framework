#include "di_bindings.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
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

#include <pybind11/stl.h>

namespace py = pybind11;
namespace di = ndnsf::di;

namespace {

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
    .def_readwrite("output_mode", &di::NativeRequestOptions::outputMode);

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
