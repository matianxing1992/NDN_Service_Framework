#include "di_bindings.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-app-request-events.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-journal-file.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"

#include "ndn-service-framework/InvocationStream.hpp"
#include "ndn-service-framework/OperationRuntime.hpp"

#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/name.hpp>
#include <openssl/crypto.h>
#include <pybind11/functional.h>
#include <pybind11/detail/type_caster_base.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

namespace py = pybind11;
namespace di = ndnsf::di;

namespace {

struct JournalEnvelopeKeyBuffer
{
  ~JournalEnvelopeKeyBuffer()
  {
    for (auto& key : keys) {
      OPENSSL_cleanse(key.bytes.data(), key.bytes.size());
    }
  }

  void
  add(std::string keyId, py::handle value)
  {
    if (!PyBytes_Check(value.ptr()) || PyBytes_GET_SIZE(value.ptr()) != 32) {
      throw py::value_error("request-envelope key must contain exactly 32 bytes");
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(PyBytes_AS_STRING(value.ptr()));
    keys.push_back({std::move(keyId), {bytes, bytes + 32}});
  }

  std::vector<di::NativeConversationJournalKey> keys;
};

template<typename Operation>
void
runJournalFileOperation(Operation&& operation)
{
  try {
    py::gil_scoped_release release;
    operation();
  }
  catch (const std::system_error& error) {
    PyErr_SetString(PyExc_OSError, error.what());
    throw py::error_already_set();
  }
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

template<typename Subscribe>
py::object
subscribeNative(Subscribe&& subscribe)
{
  ndn_service_framework::OperationSubscription subscription;
  {
    py::gil_scoped_release release;
    subscription = subscribe();
  }
  return py::cast(std::move(subscription));
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
  module.def("_di_tensor_object_digest", [] (py::bytes wire) {
    return di::computeTensorExecutionObjectDigest(
      di::detail::decodeStandaloneTensorEndpoint(std::string(wire)));
  });
  module.def("_di_tensor_name_prefix", [] (py::bytes wire) {
    return di::tensorObjectNamePrefix(di::detail::decodeStandaloneTensorEndpoint(std::string(wire)));
  });
  module.def("_di_tensor_manifest_name", [] (py::bytes wire) {
    return di::tensorObjectManifestName(
      di::detail::decodeStandaloneTensorEndpoint(std::string(wire)));
  });
  module.def("_di_tensor_segment_name", [] (py::bytes wire, std::size_t segment) {
    return di::tensorObjectSegmentName(
      di::detail::decodeStandaloneTensorEndpoint(std::string(wire)), segment);
  });
  py::register_exception<di::NativeDiError>(module, "NativeDiError");
  auto diError = py::exception<di::DiError>(module, "DiError");
  (void)diError;
  py::register_local_exception_translator(&translateDiError);
  py::register_exception<di::NativeJournalEnvelopeKeyUnavailable>(
    module, "_JournalEnvelopeKeyUnavailable", PyExc_ValueError);
  py::register_exception<di::detail::NativeJournalRollbackError>(
    module, "_JournalRollbackError", PyExc_OSError);
  module.def("_drive_di_coordinator_request", [] (py::object context,
                                                   const std::string& outerRequestId,
                                                   const std::string& coordinatorService,
                                                   const std::string& deploymentOwner,
                                                   const std::string& requestWire,
                                                   std::int64_t applicationBudgetMs,
                                                   py::function start, py::function report)
    -> py::object {
    const auto stringAttr = [] (py::handle object, const char* field) {
      return py::cast<std::string>(object.attr(field));
    };
    if (outerRequestId.empty() ||
        ndn::Name(outerRequestId) != ndn::Name(stringAttr(context, "session_id")) ||
        ndn::Name(coordinatorService) !=
          ndn::Name(stringAttr(context.attr("assignment"), "service")) ||
        ndn::Name(deploymentOwner) != ndn::Name(stringAttr(context, "local_provider")) ||
        stringAttr(context, "role") != "coordinator") {
      PyErr_SetString(PyExc_PermissionError, "coordinator request identity or assignment mismatch");
      throw py::error_already_set();
    }
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::milliseconds;
    const auto now = Clock::now();
    auto workBudget = context.attr("_coordinator_work_budget_ms");
    const auto budget = std::min({applicationBudgetMs, py::cast<std::int64_t>(workBudget()),
      std::chrono::duration_cast<Milliseconds>(Clock::time_point::max() - now).count()});
    if (budget <= 0) {
      return py::none();
    }
    const auto deadline = now + Milliseconds(budget);
    const auto remaining = [&] {
      return std::min(py::cast<std::int64_t>(workBudget()),
                      std::chrono::duration_cast<Milliseconds>(deadline - Clock::now()).count());
    };
    const auto initial = remaining();
    if (initial <= 0) {
      return py::none();
    }
    context.attr("_arm_coordinator_cancellation")(di::nativePlanningDigest(requestWire));
    const auto cancelled = context.attr("_coordinator_cancellation_received");
    const auto notifyCancellation = [&] {
      if (remaining() > 0) {
        context.attr("publish_final_failure_response")(py::bytes(),
                                                        "COORDINATOR_CANCEL_REQUESTED");
      }
    };
    if (cancelled().cast<bool>()) {
      notifyCancellation();
      return py::none();
    }
    auto request = start(initial);
    const auto cancel = [&] (const char* reason) { request.attr("cancel")(reason); };
    bool requesterCancelled = false;
    try {
      for (auto left = remaining(); left > 0; left = remaining()) {
        report(request);
        if (remaining() <= 0) {
          break;
        }
        const auto state = stringAttr(request.attr("status")(), "value");
        if (state == "COMPLETED" || state == "FAILED" || state == "CANCELLED" || state == "EXPIRED") {
          return request;
        }
        if (cancelled().cast<bool>()) {
          requesterCancelled = true;
          break;
        }
        const auto pause = std::min<std::int64_t>(250, remaining());
        if (pause > 0) {
          py::gil_scoped_release release;
          std::this_thread::sleep_for(Milliseconds(pause));
        }
      }
    }
    catch (...) {
      try { cancel("COORDINATOR_WORK_ENDED"); } catch (...) {}
      throw;
    }
    cancel(requesterCancelled ? "COORDINATOR_REQUESTER_CANCELLED" : "COORDINATOR_WORK_ENDED");
    if (requesterCancelled) {
      notifyCancellation();
    }
    return py::none();
  });
  using AppRequestEvents = di::detail::NativeAppRequestEvents;
  py::class_<AppRequestEvents>(module, "_AppRequestEvents")
    .def(py::init<>())
    .def("restore", &AppRequestEvents::restore, py::call_guard<py::gil_scoped_release>())
    .def("events", &AppRequestEvents::events, py::call_guard<py::gil_scoped_release>())
    .def("count", &AppRequestEvents::count, py::call_guard<py::gil_scoped_release>())
    .def("status", &AppRequestEvents::status, py::call_guard<py::gil_scoped_release>())
    .def("append", [] (AppRequestEvents& self, const std::string& requestId,
                       const std::vector<AppRequestEvents::Transition>& transitions,
                       py::function persist, bool allowTerminalWinner) {
      py::gil_scoped_release release;
      return self.append(requestId, transitions, [&persist] (const std::vector<std::string>& rows) {
        py::gil_scoped_acquire acquire;
        persist(rows);
      }, allowTerminalWinner);
    }, py::arg("request_id"), py::arg("transitions"), py::arg("persist"),
       py::arg("allow_terminal_winner") = false);
  module.def("_validate_di_journal_record", [] (const std::string& body,
                                               const std::string& checksum,
                                               const std::string& schemaJson) {
    py::gil_scoped_release release;
    using Validation = di::detail::JournalRecordValidation;
    switch (di::detail::validateJournalRecord(body, checksum, schemaJson)) {
      case Validation::CHECKSUM_MISMATCH: return "checksum";
      case Validation::SCHEMA_MISMATCH: return "schema";
      case Validation::VALID: return "";
    }
    throw std::logic_error("unknown journal validation result");
  });
  module.def("_frame_di_journal_records", [] (
      std::vector<std::tuple<std::string, std::string, std::uint64_t>> entries,
      std::uint64_t timestampMs) {
    di::detail::JournalFrame frame;
    {
      py::gil_scoped_release release;
      std::vector<di::detail::JournalRecord> records;
      records.reserve(entries.size());
      for (auto& [kindJson, payloadJson, time] : entries) {
        records.push_back({std::move(kindJson), std::move(payloadJson), time});
      }
      frame = di::detail::frameJournalRecords(records, timestampMs);
    }
    return py::make_tuple(py::bytes(frame.wire), std::move(frame.checksums));
  });
  module.def("_append_di_journal_wire", [] (int fd, std::uint64_t offset, py::bytes encoded) {
    const std::string wire(encoded);
    runJournalFileOperation([&] {
      di::detail::appendJournalWire(fd, offset, wire);
    });
  });
  module.def("_truncate_di_journal_tail", [] (int fd, std::uint64_t observedSize,
                                              std::uint64_t validSize) {
    runJournalFileOperation([&] {
      di::detail::truncateJournalTail(fd, observedSize, validSize);
    });
  });
  module.def("_seal_di_journal_envelope", [] (py::bytes payload, const std::string& requestId,
                                             std::uint64_t expiresAtMs, std::string keyId,
                                             py::bytes key) {
    JournalEnvelopeKeyBuffer buffer;
    buffer.keys.reserve(1);
    buffer.add(std::move(keyId), key);
    const std::string plaintext(payload);
    std::string encoded;
    {
      py::gil_scoped_release release;
      encoded = di::nativeSealRequestJournalEnvelope(
        plaintext, requestId, expiresAtMs, buffer.keys.front());
    }
    return py::bytes(encoded);
  });
  module.def("_read_di_journal_envelope", [] (py::bytes encoded, const std::string& requestId,
                                             const py::dict& keyRing, std::uint64_t nowMs) {
    JournalEnvelopeKeyBuffer buffer;
    buffer.keys.reserve(keyRing.size());
    for (const auto& [keyId, value] : keyRing) {
      buffer.add(py::cast<std::string>(keyId), value);
    }
    try {
      const std::string wire(encoded);
      std::string plaintext;
      {
        py::gil_scoped_release release;
        plaintext = di::nativeReadRequestJournalEnvelope(wire, requestId, buffer.keys, nowMs);
      }
      return py::bytes(plaintext);
    }
    catch (const di::NativeJson::exception&) {
      throw py::value_error("request envelope integrity failure");
    }
  });

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
    .def_static("repository", &di::Input::repository, py::arg("reference"),
                py::arg("application_options") = std::vector<std::uint8_t>{});

  py::class_<di::Result>(module, "Result")
    .def_readonly("payload", &di::Result::payload)
    .def_readonly("request_id", &di::Result::requestId)
    .def_readonly("model_digest", &di::Result::modelDigest)
    .def_readonly("plan_digest", &di::Result::planDigest)
    .def("matches_float32_tensor",
         py::overload_cast<const std::string&, const std::vector<float>&, double>(
           &di::Result::matchesFloat32Tensor, py::const_),
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
    .def("cancel", &ndn_service_framework::OperationSubscription::cancel,
         py::call_guard<py::gil_scoped_release>())
    .def("unsubscribe", &ndn_service_framework::OperationSubscription::unsubscribe,
         py::call_guard<py::gil_scoped_release>())
    .def("__enter__", [] (ndn_service_framework::OperationSubscription& subscription)
         -> ndn_service_framework::OperationSubscription& {
      return subscription;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (ndn_service_framework::OperationSubscription& subscription,
                           py::object, py::object, py::object) {
      py::gil_scoped_release release;
      subscription.unsubscribe();
      return false;
    });

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
      auto function = keepPyFunction(callback.cast<py::function>());
      // A missing local timeout follows the request deadline captured by the
      // native reader; an explicit zero remains a non-blocking poll.
      const auto timeoutMs = timeout.is_none() ? reader.remainingTimeout() :
        timeoutMilliseconds(timeout, 0);
      return subscribeNative([&] {
        return const_cast<di::EventReader&>(reader).nextAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error,
                                          std::optional<di::Event> event) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), event ? py::cast(*event) : py::none());
          }
          catch (...) {
            // Native callback delivery is isolated from the worker thread.
          }
        });
      });
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("close", &di::EventReader::close, py::call_guard<py::gil_scoped_release>())
    .def("__enter__", [] (di::EventReader& reader) -> di::EventReader& {
      return reader;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::EventReader& reader, py::object, py::object, py::object) {
      py::gil_scoped_release release;
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
      auto function = keepPyFunction(std::move(callback));
      py::gil_scoped_release release;
      return handle.onCompletion(
        [function = std::move(function)] (std::exception_ptr error,
                                           std::optional<di::Result> result) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), optionalResultValue(result));
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
      auto function = keepPyFunction(callback.cast<py::function>());
      if (timeout.is_none()) {
        return subscribeNative([&] {
          return handle.onCompletion(
          [function = std::move(function)] (std::exception_ptr error,
                                             std::optional<di::Result> result) {
            py::gil_scoped_acquire acquire;
            try {
              (*function)(callbackError(error), optionalResultValue(result));
            }
            catch (...) {
            }
          });
        });
      }
      const auto timeoutMs = timeoutMilliseconds(timeout, 0);
      return subscribeNative([&] {
        return handle.resultAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error,
                                           std::optional<di::Result> result) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), optionalResultValue(result));
          }
          catch (...) {
          }
        });
      });
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("observe", [] (const di::RequestHandle& handle, py::function callback) {
      if (!callback)
        throw py::value_error("observer is empty");
      auto function = keepPyFunction(std::move(callback));
      py::gil_scoped_release release;
      return handle.observe(
        [function = std::move(function)] (const di::Event& event) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(eventValue(event));
          }
          catch (...) {
          }
        });
    }, py::arg("callback"))
    .def("cancel", &di::RequestHandle::cancel, py::call_guard<py::gil_scoped_release>());

  py::class_<di::PreparedModel>(module, "PreparedModel")
    .def_property_readonly("manifest", &di::PreparedModel::manifest,
                           py::return_value_policy::reference_internal)
    .def_property_readonly("receipt", &di::PreparedModel::receipt,
                           py::return_value_policy::reference_internal)
    .def_property_readonly("capabilities", &di::PreparedModel::capabilities);

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
      auto function = keepPyFunction(callback.cast<py::function>());
      if (timeout.is_none()) {
        return subscribeNative([&] {
          return handle.onCompletion(
          [function = std::move(function)] (std::exception_ptr error,
                                             std::optional<di::PreparedModel> model) {
            py::gil_scoped_acquire acquire;
            try {
              (*function)(callbackError(error), model ? py::cast(*model) : py::none());
            }
            catch (...) {
            }
          });
        });
      }
      const auto timeoutMs = timeoutMilliseconds(timeout, 0);
      return subscribeNative([&] {
        return handle.resultAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error,
                                           std::optional<di::PreparedModel> model) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), model ? py::cast(*model) : py::none());
          }
          catch (...) {
          }
        });
      });
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("on_completion", [] (const di::PreparationHandle& handle, py::function callback) {
      if (!callback)
        throw py::value_error("preparation callback is empty");
      auto function = keepPyFunction(std::move(callback));
      py::gil_scoped_release release;
      return handle.onCompletion(
        [function = std::move(function)] (std::exception_ptr error,
                                           std::optional<di::PreparedModel> model) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), model ? py::cast(*model) : py::none());
          }
          catch (...) {
          }
        });
    }, py::arg("callback"))
    .def("cancel", &di::PreparationHandle::cancel, py::call_guard<py::gil_scoped_release>());

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
    .def("close", &di::Conversation::close, py::call_guard<py::gil_scoped_release>())
    .def("__enter__", [] (di::Conversation& conversation) -> di::Conversation& {
      return conversation;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::Conversation& conversation, py::object, py::object, py::object) {
      py::gil_scoped_release release;
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
    .def("close", &di::ProviderRegistration::close, py::call_guard<py::gil_scoped_release>())
    .def("closed", &di::ProviderRegistration::closed)
    .def("valid", &di::ProviderRegistration::valid)
    .def_property_readonly("service_name", &di::ProviderRegistration::serviceName)
    .def("__enter__", [] (di::ProviderRegistration& registration) -> di::ProviderRegistration& {
      return registration;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::ProviderRegistration& registration, py::object, py::object, py::object) {
      py::gil_scoped_release release;
      registration.close();
      return false;
    });

  py::class_<di::Provider>(module, "Provider")
    .def(py::init<>())
    .def("serve", [] (di::Provider& provider, const di::ServiceDefinition& definition) {
      py::gil_scoped_release release;
      return provider.serve(definition);
    }, py::arg("definition"))
    .def("stop", &di::Provider::stop, py::call_guard<py::gil_scoped_release>())
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
      auto function = keepPyFunction(callback.cast<py::function>());
      const auto timeoutMs = timeoutMilliseconds(timeout, 5000);
      return subscribeNative([&] {
        return provider.drainAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error, bool drained) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), drained);
          }
          catch (...) {
          }
        });
      });
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("valid", &di::Provider::valid)
    .def_property_readonly("counters", &di::Provider::counters);

  py::class_<di::User>(module, "User")
    .def("publish_input", [] (const di::User& user, const di::PreparedModel& model,
                              const std::vector<std::uint8_t>& payload,
                              const py::object& timeout) {
      const auto budget = timeoutMilliseconds(timeout, 30000);
      py::gil_scoped_release release;
      return user.publishInput(model, payload, budget);
    }, py::arg("model"), py::arg("payload"), py::kw_only(),
       py::arg("timeout_s") = py::none())
    .def("prepare", [] (const di::User& user, const std::string& modelKey,
                         di::PrepareOptions options, const py::object& timeout) {
      const auto normalized = prepareOptionsWithTimeout(std::move(options), timeout);
      py::gil_scoped_release release;
      return user.prepare(modelKey, normalized);
    }, py::arg("model_key") = "default", py::kw_only(),
       py::arg("options") = di::PrepareOptions{},
       py::arg("timeout_s") = py::none())
    .def("request", [] (const di::User& user, const di::PreparedModel& model,
                          const di::Input& input, const di::RequestOptions& options) {
      py::gil_scoped_release release;
      return user.request(model, input, options);
    }, py::arg("model"), py::arg("input"), py::kw_only(),
       py::arg("options") = di::RequestOptions{})
    .def("run", [] (const di::User& user, const di::PreparedModel& model,
                     const di::Input& input, const di::RequestOptions& options) {
      py::gil_scoped_release release;
      return user.run(model, input, options);
    }, py::arg("model"), py::arg("input"), py::kw_only(),
       py::arg("options") = di::RequestOptions{})
    .def("open_conversation", [] (const di::User& user,
                                    const di::PreparedModel& model,
                                    const di::ConversationOptions& options) {
      py::gil_scoped_release release;
      return user.openConversation(model, options);
    }, py::arg("model"), py::kw_only(),
       py::arg("options") = di::ConversationOptions{})
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
    .def("close", &di::Runtime::close, py::call_guard<py::gil_scoped_release>())
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
      auto function = keepPyFunction(callback.cast<py::function>());
      const auto timeoutMs = timeoutMilliseconds(timeout, 5000);
      return subscribeNative([&] {
        return runtime->drainAsync(timeoutMs,
        [function = std::move(function)] (std::exception_ptr error, bool drained) {
          py::gil_scoped_acquire acquire;
          try {
            (*function)(callbackError(error), drained);
          }
          catch (...) {
          }
        });
      });
    }, py::kw_only(), py::arg("timeout_s") = py::none(),
       py::arg("callback") = py::none())
    .def("__enter__", [] (di::Runtime& runtime) -> di::Runtime& {
      return runtime;
    }, py::return_value_policy::reference_internal)
    .def("__exit__", [] (di::Runtime& runtime, py::object excType,
                           py::object, py::object) {
      bool drained = false;
      {
        py::gil_scoped_release release;
        runtime.close();
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
