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
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include "ndn-service-framework/ControllerVersion.hpp"
#include "ndn-service-framework/InvocationStream.hpp"
#include "ndn-service-framework/OperationRuntime.hpp"

#include <ndn-cxx/encoding/block.hpp>
#include <pybind11/functional.h>
#include <pybind11/detail/type_caster_base.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>

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

py::object
callbackError(std::exception_ptr error)
{
  if (!error)
    return py::none();
  try {
    std::rethrow_exception(error);
  }
  catch (const di::DiError& value) {
    py::dict result;
    result["code"] = value.code();
    result["domain"] = value.domain();
    result["boundary"] = value.boundary();
    result["request_id"] = value.requestId();
    result["attempt"] = value.attempt();
    result["message"] = value.what();
    return std::move(result);
  }
  catch (const std::exception& value) {
    return py::str(value.what());
  }
  catch (...) {
    return py::str("native callback failed with an unknown exception");
  }
}

py::dict
eventValue(const di::Event& event)
{
  py::dict result;
  result["request_id"] = event.requestId;
  result["payload"] = py::bytes(
    reinterpret_cast<const char*>(event.payload.data()), event.payload.size());
  result["terminal"] = event.terminal;
  result["sequence"] = event.sequence;
  return result;
}

void
translateDiError(std::exception_ptr error)
{
  if (!error)
    return;
  try {
    std::rethrow_exception(error);
  }
  catch (const di::DiError& value) {
    // Resolve the exception type through the current interpreter's module on
    // each translation.  The translator is a non-capturing function pointer
    // (as required by pybind11), while the temporary py::object owns the type
    // for this call and cannot outlive module teardown.
    PyObject* module = PyImport_AddModule("ndnsf._ndnsf");
    if (module == nullptr) {
      PyErr_Clear();
      throw;
    }
    PyObject* type = PyObject_GetAttrString(module, "DiError");
    if (type == nullptr) {
      PyErr_Clear();
      throw;
    }
    py::object diErrorType = py::reinterpret_steal<py::object>(type);
    py::object instance = diErrorType(value.what());
    instance.attr("code") = value.code();
    instance.attr("domain") = value.domain();
    instance.attr("boundary") = value.boundary();
    instance.attr("request_id") = value.requestId();
    instance.attr("attempt") = value.attempt();
    PyErr_SetObject(diErrorType.ptr(), instance.ptr());
  }
  catch (...) {
    // Delegate all other exception types to pybind11's default translators.
    throw;
  }
}

std::chrono::milliseconds
timeoutMilliseconds(const py::object& value, std::uint64_t defaultValue)
{
  if (value.is_none())
    return std::chrono::milliseconds(
      static_cast<std::chrono::milliseconds::rep>(defaultValue));
  if (py::isinstance<py::bool_>(value))
    throw py::type_error("timeout_s must be a real number, not bool");
  const double seconds = value.cast<double>();
  if (!std::isfinite(seconds) || seconds < 0.0)
    throw py::value_error("timeout_s must be finite and nonnegative");
  const long double millis = static_cast<long double>(seconds) * 1000.0L;
  if (!std::isfinite(millis) ||
      millis > static_cast<long double>(std::numeric_limits<std::chrono::milliseconds::rep>::max()))
    throw py::value_error("timeout_s is too large");
  return std::chrono::milliseconds(
    static_cast<std::chrono::milliseconds::rep>(std::ceil(millis)));
}

py::object
optionalResultValue(const std::optional<di::Result>& result)
{
  if (!result)
    return py::none();
  return py::cast(*result);
}

di::PrepareOptions
prepareOptionsWithTimeout(di::PrepareOptions options, const py::object& timeout)
{
  if (!timeout.is_none()) {
    options.timeout = timeoutMilliseconds(timeout, 300000);
    if (options.timeout.count() <= 0)
      throw py::value_error("prepare timeout_s must be positive");
  }
  if (options.timeout.count() <= 0)
    throw py::value_error("prepare timeout must be positive");
  return options;
}

} // namespace

void
bindDistributedInference(py::module_& module)
{
  py::register_exception<di::NativeDiError>(module, "NativeDiError");
  auto diError = py::exception<di::DiError>(module, "DiError");
  (void)diError;
  py::register_local_exception_translator(&translateDiError);

  py::enum_<di::CachePolicy>(module, "CachePolicy")
    .value("REQUIRE_READY", di::CachePolicy::RequireReady)
    .value("USE_OR_WAIT", di::CachePolicy::UseOrWait)
    .value("USE_OR_FETCH", di::CachePolicy::UseOrFetch)
    .value("REFRESH", di::CachePolicy::Refresh);

  py::enum_<di::PreparationStatus>(module, "PreparationStatus")
    .value("PENDING", di::PreparationStatus::Pending)
    .value("READY", di::PreparationStatus::Ready)
    .value("FAILED", di::PreparationStatus::Failed)
    .value("CANCELLED", di::PreparationStatus::Cancelled);

  py::enum_<di::PreparationReceipt::Origin>(module, "PreparationOrigin")
    .value("CACHE_HIT", di::PreparationReceipt::Origin::CacheHit)
    .value("JOINED_IN_FLIGHT", di::PreparationReceipt::Origin::JoinedInFlight)
    .value("FETCHED", di::PreparationReceipt::Origin::Fetched)
    .value("REFRESHED", di::PreparationReceipt::Origin::Refreshed);

  py::enum_<di::RequestStatus>(module, "RequestStatus")
    .value("PENDING", di::RequestStatus::Pending)
    .value("SUCCEEDED", di::RequestStatus::Succeeded)
    .value("FAILED", di::RequestStatus::Failed)
    .value("CANCELLED", di::RequestStatus::Cancelled);

  py::class_<di::ModelRegistration>(module, "ModelRegistration")
    .def(py::init<>())
    .def_readwrite("key", &di::ModelRegistration::key)
    .def_readwrite("native_config_path", &di::ModelRegistration::nativeConfigPath);

  py::class_<di::RuntimeConfig>(module, "RuntimeConfig")
    .def(py::init<>())
    .def_readwrite("native_config_path", &di::RuntimeConfig::nativeConfigPath)
    .def_readwrite("models", &di::RuntimeConfig::models)
    .def_readwrite("max_prepared_bytes", &di::RuntimeConfig::maxPreparedBytes)
    .def_readwrite("max_prepared_entries", &di::RuntimeConfig::maxPreparedEntries)
    .def_property("preparation_job_timeout_s",
      [] (const di::RuntimeConfig& config) {
        return static_cast<double>(config.preparationJobTimeout.count()) / 1000.0;
      },
      [] (di::RuntimeConfig& config, const py::object& value) {
        config.preparationJobTimeout = timeoutMilliseconds(value, 300000);
      });

  py::class_<di::UserConfig>(module, "UserConfig")
    .def(py::init<>())
    .def_readwrite("profile_name", &di::UserConfig::profileName);

  py::class_<di::PrepareOptions>(module, "PrepareOptions")
    .def(py::init<>())
    .def_readwrite("cache", &di::PrepareOptions::cache)
    .def_property("timeout_s",
      [] (const di::PrepareOptions& options) {
        return static_cast<double>(options.timeout.count()) / 1000.0;
      },
      [] (di::PrepareOptions& options, const py::object& value) {
        const auto timeout = timeoutMilliseconds(value, 300000);
        if (timeout.count() <= 0)
          throw py::value_error("prepare timeout_s must be positive");
        options.timeout = timeout;
      });

  py::class_<di::ModelCapabilities>(module, "ModelCapabilities")
    .def(py::init<>())
    .def_readonly("input_schema_json", &di::ModelCapabilities::inputSchemaJson)
    .def_readonly("output_schema_json", &di::ModelCapabilities::outputSchemaJson)
    .def_readonly("input_kinds", &di::ModelCapabilities::inputKinds)
    .def_readonly("output_modes", &di::ModelCapabilities::outputModes)
    .def_readonly("streaming", &di::ModelCapabilities::streaming)
    .def_readonly("conversations", &di::ModelCapabilities::conversations);

  py::class_<di::ModelManifest>(module, "ModelManifest")
    .def(py::init<>())
    .def_readonly("model_name", &di::ModelManifest::modelName)
    .def_readonly("model_revision", &di::ModelManifest::modelRevision)
    .def_readonly("model_digest", &di::ModelManifest::modelDigest)
    .def_readonly("task_name", &di::ModelManifest::taskName)
    .def_readonly("canonical_graph_digest", &di::ModelManifest::canonicalGraphDigest)
    .def_readonly("planning_graph_digest", &di::ModelManifest::planningGraphDigest)
    .def_readonly("catalog_configuration_digest", &di::ModelManifest::catalogConfigurationDigest)
    .def_readonly("task_contract_digest", &di::ModelManifest::taskContractDigest)
    .def_readonly("preparation_key_digest", &di::ModelManifest::preparationKeyDigest);

  py::class_<di::PreparationReceipt>(module, "PreparationReceipt")
    .def_readonly("origin", &di::PreparationReceipt::origin)
    .def_readonly("preparation_key_digest", &di::PreparationReceipt::preparationKeyDigest)
    .def_readonly("manifest_digest", &di::PreparationReceipt::manifestDigest)
    .def_property_readonly("elapsed_s", [] (const di::PreparationReceipt& receipt) {
      return static_cast<double>(receipt.elapsed.count()) / 1000.0;
    });

  py::class_<di::GenerationOptions>(module, "GenerationOptions")
    .def(py::init<>())
    .def_readwrite("max_new_tokens", &di::GenerationOptions::maxNewTokens);

  py::class_<di::StreamOptions>(module, "StreamOptions")
    .def(py::init<>())
    .def_readwrite("enabled", &di::StreamOptions::enabled)
    .def_readwrite("allow_replacement", &di::StreamOptions::allowReplacement)
    .def_readwrite("max_replacements", &di::StreamOptions::maxReplacements);

  py::class_<di::PlacementStrategy, std::shared_ptr<di::PlacementStrategy>>(
    module, "PlacementStrategy");

  py::class_<di::RequestOptions>(module, "RequestOptions")
    .def(py::init<>())
    .def_property("timeout_s",
      [] (const di::RequestOptions& options) {
        return static_cast<double>(options.timeout.count()) / 1000.0;
      },
      [] (di::RequestOptions& options, const py::object& value) {
        options.timeout = timeoutMilliseconds(value, 30000);
      })
    .def_property("ack_timeout_s",
      [] (const di::RequestOptions& options) {
        return static_cast<double>(options.ackTimeout.count()) / 1000.0;
      },
      [] (di::RequestOptions& options, const py::object& value) {
        options.ackTimeout = timeoutMilliseconds(value, 5000);
      })
    .def_property("placement",
      [] (const di::RequestOptions& options) {
        return options.placement ?
          std::const_pointer_cast<di::PlacementStrategy>(options.placement) :
          std::shared_ptr<di::PlacementStrategy>{};
      },
      [] (di::RequestOptions& options, std::shared_ptr<di::PlacementStrategy> value) {
        options.placement = std::move(value);
      })
    .def_readwrite("provider_names", &di::RequestOptions::providerNames)
    .def_readwrite("application_request_id", &di::RequestOptions::applicationRequestId)
    .def_readwrite("output_mode", &di::RequestOptions::outputMode)
    .def_readwrite("generation", &di::RequestOptions::generation)
    .def_readwrite("stream", &di::RequestOptions::stream);

  py::class_<di::DataRef>(module, "DataRef")
    .def_static("from_published_metadata", &di::DataRef::fromPublishedMetadata,
                py::arg("canonical_reference_json"))
    .def("canonical_metadata", &di::DataRef::canonicalMetadata);

  py::class_<di::Input>(module, "Input")
    .def_static("inline_bytes", &di::Input::inlineBytes,
                py::arg("payload"), py::arg("application_options") = std::vector<std::uint8_t>{})
    .def_static("text", &di::Input::text, py::arg("utf8"))
    .def_static("repository", &di::Input::repository, py::arg("reference"));

  py::class_<di::Result>(module, "Result")
    .def_readonly("payload", &di::Result::payload)
    .def_readonly("request_id", &di::Result::requestId)
    .def_readonly("model_digest", &di::Result::modelDigest)
    .def_readonly("plan_digest", &di::Result::planDigest)
    .def("matches_float32_tensor", &di::Result::matchesFloat32Tensor,
         py::arg("tensor_name"), py::arg("expected"), py::arg("tolerance"));

  py::class_<di::Event>(module, "Event")
    .def_readonly("request_id", &di::Event::requestId)
    .def_readonly("payload", &di::Event::payload)
    .def_readonly("terminal", &di::Event::terminal)
    .def_readonly("sequence", &di::Event::sequence);

  py::class_<di::RequestDiagnostics>(module, "RequestDiagnostics")
    .def_readonly("observation_dropped", &di::RequestDiagnostics::observationDropped);

  py::class_<di::ConversationCheckpoint>(module, "ConversationCheckpoint")
    .def_static("from_bytes", &di::ConversationCheckpoint::fromBytes,
                py::arg("bytes"))
    .def("to_bytes", &di::ConversationCheckpoint::bytes);

  py::class_<di::ConversationOptions>(module, "ConversationOptions")
    .def(py::init<>())
    .def_readwrite("conversation_id", &di::ConversationOptions::conversationId)
    .def_readwrite("checkpoint", &di::ConversationOptions::checkpoint);

  py::class_<ndn_service_framework::OperationSubscription>(
      module, "Subscription")
    .def(py::init<>())
    .def("cancel", &ndn_service_framework::OperationSubscription::cancel)
    .def("unsubscribe", &ndn_service_framework::OperationSubscription::unsubscribe)
    .def("__enter__", [] (ndn_service_framework::OperationSubscription& subscription)
         -> ndn_service_framework::OperationSubscription& {
      return subscription;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (ndn_service_framework::OperationSubscription& subscription,
                           py::object, py::object, py::object) {
      subscription.unsubscribe();
      return false;
    });

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
    .def_readwrite("application_request_id", &di::NativeRequestOptions::applicationRequestId)
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
    .def_readwrite("generation_mode", &di::NativeRequestContract::generationMode)
    .def_readwrite("tokenizer_digest", &di::NativeRequestContract::tokenizerDigest);

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

  // The coordinator is an opaque C++ owner.  Python may retain and pass the
  // handle when composing a requester, but cannot construct or mutate its
  // journal, keys, checkpoints, or state machine.
  py::class_<di::NativeConversationCoordinator,
             std::shared_ptr<di::NativeConversationCoordinator>>(
    module, "NativeConversationCoordinator");

  py::class_<di::NativeOfferAdmission,
             std::shared_ptr<di::NativeOfferAdmission>>(
    module, "NativeOfferAdmission")
    .def(py::init<const std::string&, const std::map<std::string, std::string>&,
                  const std::string&>(),
         py::arg("policy_json"), py::arg("public_key_pem_by_id"),
         py::arg("candidate_digest"));

  py::class_<di::NativeRequestCatalog>(module, "NativeRequestCatalog")
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
    .def_property_readonly("application_request_id", &di::NativeInferenceHandle::applicationRequestId)
    .def_property_readonly("conversation_checkpoint", &di::NativeInferenceHandle::conversationCheckpoint)
    .def_property_readonly("status", &di::NativeInferenceHandle::status)
    .def("result", [](const di::NativeInferenceHandle& handle,
                       std::uint64_t wait_timeout_ms) {
      py::gil_scoped_release release;
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

  py::class_<di::EventReader>(module, "EventReader")
    .def("next", [] (di::EventReader& reader, const py::object& timeout) {
      // None uses the request deadline captured by the native reader; an
      // explicit zero remains a non-blocking poll.
      const auto timeoutMs = timeout.is_none() ? reader.remainingTimeout() :
        timeoutMilliseconds(timeout, 0);
      py::gil_scoped_release release;
      return reader.next(timeoutMs);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def("next_async", [] (const di::EventReader& reader,
                             const py::object& timeout,
                             const py::object& callback) -> py::object {
      if (callback.is_none()) {
        const auto* typeInfo = py::detail::get_type_info(typeid(di::EventReader));
        const auto selfHandle = typeInfo == nullptr ? py::handle() :
          py::detail::get_object_handle(&reader, typeInfo);
        if (!selfHandle)
          throw py::value_error("EventReader Python owner is unavailable");
        return py::module_::import(
          "ndnsf_distributed_inference.api._async").attr("next_event")(
            py::reinterpret_borrow<py::object>(selfHandle), timeout);
      }
      auto function = callback.cast<py::function>();
      // A missing local timeout follows the request deadline captured by the
      // native reader; an explicit zero remains a non-blocking poll.
      const auto timeoutMs = timeout.is_none() ? reader.remainingTimeout() :
        timeoutMilliseconds(timeout, 0);
      return py::cast(const_cast<di::EventReader&>(reader).nextAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error,
                                          std::optional<di::Event> event) {
          py::gil_scoped_acquire acquire;
          try {
            function(callbackError(error), event ? py::cast(*event) : py::none());
          }
          catch (...) {
            // Native callback delivery is isolated from the worker thread.
          }
        }));
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("close", &di::EventReader::close)
    .def("__enter__", [] (di::EventReader& reader) -> di::EventReader& {
      return reader;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::EventReader& reader, py::object, py::object, py::object) {
      reader.close();
      return false;
    });

  py::class_<di::RequestHandle>(module, "RequestHandle")
    .def_property_readonly("id", &di::RequestHandle::id)
    .def_property_readonly("status", &di::RequestHandle::status)
    .def_property_readonly("status_name", [] (const di::RequestHandle& handle) {
      switch (handle.status()) {
        case di::RequestStatus::Pending: return std::string("PENDING");
        case di::RequestStatus::Succeeded: return std::string("SUCCEEDED");
        case di::RequestStatus::Failed: return std::string("FAILED");
        case di::RequestStatus::Cancelled: return std::string("CANCELLED");
      }
      return std::string("UNKNOWN");
    })
    .def("result", [] (const di::RequestHandle& handle, const py::object& timeout) {
      const bool useDefault = timeout.is_none();
      const auto timeoutMs = useDefault ? std::chrono::milliseconds(0) :
        timeoutMilliseconds(timeout, 0);
      py::gil_scoped_release release;
      if (useDefault)
        return handle.result();
      return handle.result(timeoutMs);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def("wait", [] (const di::RequestHandle& handle, const py::object& timeout) {
      const bool useDefault = timeout.is_none();
      const auto timeoutMs = useDefault ? std::chrono::milliseconds(0) :
        timeoutMilliseconds(timeout, 0);
      py::gil_scoped_release release;
      if (useDefault)
        return handle.wait();
      return handle.wait(timeoutMs);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def("events", &di::RequestHandle::events)
    .def("events_async", [] (const di::RequestHandle& handle,
                               const py::object& timeout) {
      return py::module_::import(
        "ndnsf_distributed_inference.api._async").attr("events_async")(
          py::cast(handle), timeout);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def_property_readonly("diagnostics", &di::RequestHandle::diagnostics)
    .def("on_completion", [] (const di::RequestHandle& handle, py::function callback) {
      if (!callback)
        throw py::value_error("completion callback is empty");
      return handle.onCompletion(
        [callback = std::move(callback)] (std::exception_ptr error,
                                           std::optional<di::Result> result) {
          py::gil_scoped_acquire acquire;
          try {
            callback(callbackError(error), optionalResultValue(result));
          }
          catch (...) {
          }
        });
    }, py::arg("callback"))
    .def("result_async", [] (const di::RequestHandle& handle, const py::object& timeout,
                              const py::object& callback) -> py::object {
      if (callback.is_none()) {
        return py::module_::import(
          "ndnsf_distributed_inference.api._async").attr("request_result")(
            py::cast(handle), timeout);
      }
      auto function = callback.cast<py::function>();
      if (timeout.is_none()) {
        return py::cast(handle.onCompletion(
          [function = std::move(function)] (std::exception_ptr error,
                                             std::optional<di::Result> result) {
            py::gil_scoped_acquire acquire;
            try {
              function(callbackError(error), optionalResultValue(result));
            }
            catch (...) {
            }
          }));
      }
      const auto timeoutMs = timeoutMilliseconds(timeout, 0);
      return py::cast(handle.resultAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error,
                                           std::optional<di::Result> result) {
          py::gil_scoped_acquire acquire;
          try {
            function(callbackError(error), optionalResultValue(result));
          }
          catch (...) {
          }
        }));
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("observe", [] (const di::RequestHandle& handle, py::function callback) {
      if (!callback)
        throw py::value_error("observer is empty");
      return handle.observe(
        [callback = std::move(callback)] (const di::Event& event) {
          py::gil_scoped_acquire acquire;
          try {
            callback(eventValue(event));
          }
          catch (...) {
          }
        });
    }, py::arg("callback"))
    .def("cancel", &di::RequestHandle::cancel);

  py::class_<di::PreparedModel>(module, "PreparedModel")
    .def_property_readonly("manifest", &di::PreparedModel::manifest,
                           py::return_value_policy::reference_internal)
    .def_property_readonly("receipt", &di::PreparedModel::receipt,
                           py::return_value_policy::reference_internal)
    .def_property_readonly("capabilities", &di::PreparedModel::capabilities)
    .def("request", &di::PreparedModel::request,
         py::arg("input"), py::kw_only(),
         py::arg("options") = di::RequestOptions{})
    .def("run", [] (const di::PreparedModel& model, const di::Input& input,
                     const di::RequestOptions& options) {
      py::gil_scoped_release release;
      return model.run(input, options);
    }, py::arg("input"), py::kw_only(),
       py::arg("options") = di::RequestOptions{})
    .def("open_conversation", &di::PreparedModel::openConversation,
         py::kw_only(), py::arg("options") = di::ConversationOptions{});

  py::class_<di::PreparationHandle>(module, "PreparationHandle")
    .def_property_readonly("status", &di::PreparationHandle::status)
    .def_property_readonly("status_name", [] (const di::PreparationHandle& handle) {
      switch (handle.status()) {
        case di::PreparationStatus::Pending: return std::string("PENDING");
        case di::PreparationStatus::Ready: return std::string("READY");
        case di::PreparationStatus::Failed: return std::string("FAILED");
        case di::PreparationStatus::Cancelled: return std::string("CANCELLED");
      }
      return std::string("UNKNOWN");
    })
    .def("result", [] (const di::PreparationHandle& handle, const py::object& timeout) {
      const bool useDefault = timeout.is_none();
      const auto timeoutMs = useDefault ? std::chrono::milliseconds(0) :
        timeoutMilliseconds(timeout, 0);
      py::gil_scoped_release release;
      if (useDefault)
        return handle.result();
      return handle.result(timeoutMs);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def("result_async", [] (const di::PreparationHandle& handle, const py::object& timeout,
                              const py::object& callback) -> py::object {
      if (callback.is_none()) {
        return py::module_::import(
          "ndnsf_distributed_inference.api._async").attr("preparation_result")(
            py::cast(handle), timeout);
      }
      auto function = callback.cast<py::function>();
      if (timeout.is_none()) {
        return py::cast(handle.onCompletion(
          [function = std::move(function)] (std::exception_ptr error,
                                             std::optional<di::PreparedModel> model) {
            py::gil_scoped_acquire acquire;
            try {
              function(callbackError(error), model ? py::cast(*model) : py::none());
            }
            catch (...) {
            }
          }));
      }
      const auto timeoutMs = timeoutMilliseconds(timeout, 0);
      return py::cast(handle.resultAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error,
                                           std::optional<di::PreparedModel> model) {
          py::gil_scoped_acquire acquire;
          try {
            function(callbackError(error), model ? py::cast(*model) : py::none());
          }
          catch (...) {
          }
        }));
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("on_completion", [] (const di::PreparationHandle& handle, py::function callback) {
      if (!callback)
        throw py::value_error("preparation callback is empty");
      return handle.onCompletion(
        [callback = std::move(callback)] (std::exception_ptr error,
                                           std::optional<di::PreparedModel> model) {
          py::gil_scoped_acquire acquire;
          try {
            callback(callbackError(error), model ? py::cast(*model) : py::none());
          }
          catch (...) {
          }
        });
    }, py::arg("callback"))
    .def("cancel", &di::PreparationHandle::cancel);

  py::class_<di::Conversation>(module, "Conversation")
    .def("request", &di::Conversation::request,
         py::arg("input"), py::kw_only(),
         py::arg("options") = di::RequestOptions{})
    .def("checkpoint", &di::Conversation::checkpoint)
    .def("export_checkpoint", [] (const di::Conversation& conversation,
                                   const std::string& destination) {
      py::gil_scoped_release release;
      conversation.exportCheckpoint(std::filesystem::path(destination));
    }, py::arg("destination"))
    .def("close", &di::Conversation::close)
    .def("__enter__", [] (di::Conversation& conversation) -> di::Conversation& {
      return conversation;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::Conversation& conversation, py::object, py::object, py::object) {
      conversation.close();
      return false;
    });

  py::class_<di::ProviderConfig>(module, "NativeProviderConfig")
    .def(py::init<>())
    .def_static("from_file", &di::ProviderConfig::fromFile, py::arg("path"))
    .def("valid", &di::ProviderConfig::valid)
    .def("equivalent", &di::ProviderConfig::equivalent);

  py::class_<di::ServiceDefinition>(module, "ServiceDefinition")
    .def(py::init<>())
    .def_readwrite("service_name", &di::ServiceDefinition::serviceName)
    .def_readwrite("allowed_roles", &di::ServiceDefinition::allowedRoles);

  py::class_<di::ProviderCounters>(module, "ProviderCounters")
    .def_readonly("source_fetches", &di::ProviderCounters::sourceFetches)
    .def_readonly("assemblies", &di::ProviderCounters::assemblies)
    .def_readonly("template_hits", &di::ProviderCounters::templateHits)
    .def_readonly("runners_created", &di::ProviderCounters::runnersCreated)
    .def_readonly("active_leases", &di::ProviderCounters::activeLeases);

  py::class_<di::ProviderRegistration>(module, "ProviderRegistration")
    .def("close", &di::ProviderRegistration::close)
    .def("closed", &di::ProviderRegistration::closed)
    .def("valid", &di::ProviderRegistration::valid)
    .def_property_readonly("service_name", &di::ProviderRegistration::serviceName)
    .def("__enter__", [] (di::ProviderRegistration& registration) -> di::ProviderRegistration& {
      return registration;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::ProviderRegistration& registration, py::object, py::object, py::object) {
      registration.close();
      return false;
    });

  py::class_<di::Provider>(module, "Provider")
    .def(py::init<>())
    .def("serve", [] (di::Provider& provider, const di::ServiceDefinition& definition) {
      py::gil_scoped_release release;
      return provider.serve(definition);
    }, py::arg("definition"))
    .def("stop", &di::Provider::stop)
    .def("drain", [] (const di::Provider& provider, const py::object& timeout) {
      const auto timeoutMs = timeoutMilliseconds(timeout, 5000);
      py::gil_scoped_release release;
      return provider.drain(timeoutMs);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def("drain_async", [] (const di::Provider& provider, const py::object& timeout,
                             const py::object& callback) -> py::object {
      if (callback.is_none()) {
        return py::module_::import(
          "ndnsf_distributed_inference.api._async").attr("drain_result")(
            py::cast(provider), timeout);
      }
      auto function = callback.cast<py::function>();
      return py::cast(provider.drainAsync(timeoutMilliseconds(timeout, 5000),
        [function = std::move(function)] (std::exception_ptr error, bool drained) {
          py::gil_scoped_acquire acquire;
          try {
            function(callbackError(error), drained);
          }
          catch (...) {
          }
        }));
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("valid", &di::Provider::valid)
    .def_property_readonly("counters", &di::Provider::counters);

  py::class_<di::User>(module, "User")
    .def("prepare", [] (const di::User& user, const std::string& modelKey,
                         di::PrepareOptions options, const py::object& timeout) {
      const auto normalized = prepareOptionsWithTimeout(std::move(options), timeout);
      py::gil_scoped_release release;
      return user.prepare(modelKey, normalized);
    }, py::arg("model_key") = "default", py::kw_only(),
       py::arg("options") = di::PrepareOptions{},
       py::arg("timeout_s") = py::none())
    .def("start_prepare", [] (const di::User& user, const std::string& modelKey,
                                di::PrepareOptions options, const py::object& timeout) {
      const auto normalized = prepareOptionsWithTimeout(std::move(options), timeout);
      return user.prepareAsync(modelKey, normalized);
    },
         py::arg("model_key") = "default", py::kw_only(),
         py::arg("options") = di::PrepareOptions{},
         py::arg("timeout_s") = py::none())
    .def("prepare_async", [] (const di::User& user, const std::string& modelKey,
                                const di::PrepareOptions& options,
                                const py::object& timeout) {
      return py::module_::import(
        "ndnsf_distributed_inference.api._async").attr("_prepare_positional")(
          py::cast(user), modelKey, py::cast(options), timeout);
    }, py::arg("model_key") = "default", py::kw_only(),
       py::arg("options") = di::PrepareOptions{},
       py::arg("timeout_s") = py::none());

  py::class_<di::Runtime, std::shared_ptr<di::Runtime>>(module, "Runtime")
    .def_static("open", [] (di::RuntimeConfig config) {
      py::gil_scoped_release release;
      return di::Runtime::open(std::move(config));
    }, py::arg("config"))
    .def_static("open", [] (di::ProviderConfig config) {
      py::gil_scoped_release release;
      return di::Runtime::open(config);
    }, py::arg("provider_config"))
    .def("user", &di::Runtime::user, py::arg("config") = di::UserConfig{})
    .def("placement_strategy", [] (const di::Runtime& runtime, const std::string& id) {
      auto value = runtime.placementStrategy(id);
      return value ? std::const_pointer_cast<di::PlacementStrategy>(value) :
        std::shared_ptr<di::PlacementStrategy>{};
    }, py::arg("id"))
    .def("provider", [] (di::Runtime& runtime, const di::ProviderConfig& config) {
      return runtime.provider(config);
    }, py::arg("config"))
    .def("provider", [] (di::Runtime& runtime) {
      return runtime.provider();
    })
    .def("close", &di::Runtime::close)
    .def("drain", [] (const di::Runtime& runtime, const py::object& timeout) {
      const auto timeoutMs = timeoutMilliseconds(timeout, 5000);
      py::gil_scoped_release release;
      return runtime.drain(timeoutMs);
    }, py::kw_only(), py::arg("timeout_s") = py::none())
    .def("drain_async", [] (std::shared_ptr<di::Runtime> runtime,
                             const py::object& timeout,
                             const py::object& callback) -> py::object {
      if (callback.is_none()) {
        return py::module_::import(
          "ndnsf_distributed_inference.api._async").attr("drain_result")(
            runtime, timeout);
      }
      auto function = callback.cast<py::function>();
      return py::cast(runtime->drainAsync(timeoutMilliseconds(timeout, 5000),
        [function = std::move(function)] (std::exception_ptr error, bool drained) {
          py::gil_scoped_acquire acquire;
          try {
            function(callbackError(error), drained);
          }
          catch (...) {
          }
        }));
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("__enter__", [] (di::Runtime& runtime) -> di::Runtime& {
      return runtime;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::Runtime& runtime, py::object excType,
                           py::object, py::object) {
      runtime.close();
      bool drained = false;
      {
        py::gil_scoped_release release;
        drained = runtime.drain(std::chrono::milliseconds(5000));
      }
      if (!drained && !excType.is_none()) {
        std::clog << "async Runtime shutdown failed while preserving body exception: "
                  << "SHUTDOWN_TIMEOUT" << std::endl;
        return false;
      }
      if (!drained)
        throw di::DiError("SHUTDOWN_TIMEOUT", "local", "runtime",
                          "native Runtime did not drain before context exit");
      return false;
    })
    .def("__aenter__", [] (std::shared_ptr<di::Runtime> runtime) {
      return py::module_::import(
        "ndnsf_distributed_inference.api._async").attr("runtime_enter")(runtime);
    })
    .def("__aexit__", [] (std::shared_ptr<di::Runtime> runtime,
                            py::object excType, py::object exc, py::object traceback) {
      return py::module_::import(
        "ndnsf_distributed_inference.api._async").attr("runtime_exit")(
          runtime, excType, exc, traceback);
    });
}
