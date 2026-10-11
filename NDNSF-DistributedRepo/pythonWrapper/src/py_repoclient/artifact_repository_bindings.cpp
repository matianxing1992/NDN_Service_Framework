#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"

#include <ndn-service-framework/ServiceUser.hpp>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;
namespace repo = ndnsf_distributed_repo;

namespace {

constexpr const char* SERVICE_USER_CAPSULE_NAME = "ndnsf.ServiceUser.v1";
PyObject* g_artifactFetchErrorType = nullptr;

struct BoundArtifactRepositoryApi
{
  // Members are destroyed in reverse declaration order: destroy the borrowing
  // native API before releasing the NativeServiceUser that owns ServiceUser.
  py::object serviceUserOwner;
  repo::ArtifactRepositoryApi api;

  BoundArtifactRepositoryApi(repo::ArtifactRepositoryApi apiValue,
                             py::object ownerValue)
    : serviceUserOwner(std::move(ownerValue))
    , api(std::move(apiValue))
  {
  }
};

template<typename Function>
auto
withoutGil(Function&& function) -> decltype(function())
{
  py::gil_scoped_release release;
  return function();
}

ndn_service_framework::ServiceUser&
serviceUserFromOwner(const py::object& owner)
{
  if (owner.is_none() || !py::hasattr(owner, "_repo_service_user_capsule")) {
    throw std::invalid_argument(
      "Persistent Repo API requires an NDNSF NativeServiceUser owner");
  }
  const py::object value = owner.attr("_repo_service_user_capsule")();
  if (!PyCapsule_CheckExact(value.ptr()) ||
      !PyCapsule_IsValid(value.ptr(), SERVICE_USER_CAPSULE_NAME)) {
    throw std::invalid_argument(
      "Persistent Repo API received an invalid ServiceUser capsule");
  }
  auto* serviceUser = static_cast<ndn_service_framework::ServiceUser*>(
    PyCapsule_GetPointer(value.ptr(), SERVICE_USER_CAPSULE_NAME));
  if (serviceUser == nullptr) {
    throw std::invalid_argument(
      "Persistent Repo API received an empty ServiceUser capsule");
  }
  return *serviceUser;
}

void
translateArtifactFetchError(std::exception_ptr error)
{
  try {
    if (error) {
      std::rethrow_exception(error);
    }
  }
  catch (const repo::ArtifactFetchError& caught) {
    if (g_artifactFetchErrorType == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, caught.what());
      return;
    }
    py::object instance = py::reinterpret_steal<py::object>(
      PyObject_CallFunction(g_artifactFetchErrorType, "s", caught.what()));
    if (!instance) {
      return;
    }
    instance.attr("result") = py::cast(caught.result());
    PyErr_SetObject(g_artifactFetchErrorType, instance.ptr());
  }
  catch (...) {
    // This translator is first in pybind11's chain; rethrow so the standard
    // translator can preserve unrelated native validation and runtime errors.
    throw;
  }
}

void
bindArtifactRepositoryTypes(py::module_& m)
{
  py::enum_<repo::RepoWriteConsistency>(m, "RepoWriteConsistency")
    .value("ONE", repo::RepoWriteConsistency::One)
    .value("QUORUM", repo::RepoWriteConsistency::Quorum)
    .value("ALL", repo::RepoWriteConsistency::All);

  py::class_<repo::InAppRepoConfig>(m, "InAppRepoConfig")
    .def(py::init<>())
    .def_property("durable_root",
      [] (const repo::InAppRepoConfig& value) {
        return value.durableRoot.string();
      },
      [] (repo::InAppRepoConfig& value, const std::string& path) {
        value.durableRoot = path;
      })
    .def_readwrite("owner_id", &repo::InAppRepoConfig::ownerId)
    .def_readwrite("limits", &repo::InAppRepoConfig::limits)
    .def_readwrite("availability_freshness_ms",
                   &repo::InAppRepoConfig::availabilityFreshnessMs);

  py::class_<repo::PersistentRepoClientConfig>(m, "PersistentRepoClientConfig")
    .def(py::init<>())
    .def_readwrite("limits", &repo::PersistentRepoClientConfig::limits)
    .def_property("service_prefix",
      [] (const repo::PersistentRepoClientConfig& value) {
        return value.servicePrefix.toUri();
      },
      [] (repo::PersistentRepoClientConfig& value, const std::string& name) {
        value.servicePrefix = ndn::Name(name);
      })
    .def_property("provider_scope",
      [] (const repo::PersistentRepoClientConfig& value) {
        std::vector<std::string> names;
        names.reserve(value.providerScope.size());
        for (const auto& name : value.providerScope) {
          names.push_back(name.toUri());
        }
        return names;
      },
      [] (repo::PersistentRepoClientConfig& value,
          const std::vector<std::string>& names) {
        value.providerScope.clear();
        value.providerScope.reserve(names.size());
        for (const auto& name : names) {
          value.providerScope.emplace_back(name);
        }
      })
    .def_readwrite("ack_collection_time_ms",
                   &repo::PersistentRepoClientConfig::ackCollectionTimeMs)
    .def_readwrite("request_timeout_ms",
                   &repo::PersistentRepoClientConfig::requestTimeoutMs)
    .def_readwrite("operation_wait_timeout_ms",
                   &repo::PersistentRepoClientConfig::operationWaitTimeoutMs)
    .def_readwrite("fetch_failure_cooldown_ms",
                   &repo::PersistentRepoClientConfig::fetchFailureCooldownMs)
    .def_readwrite("max_fetch_health_entries",
                   &repo::PersistentRepoClientConfig::maxFetchHealthEntries);

  py::class_<repo::ArtifactCapabilityRequirements>(m,
                                                    "ArtifactCapabilityRequirements")
    .def(py::init<>())
    .def_readwrite("artifact", &repo::ArtifactCapabilityRequirements::artifact)
    .def_readwrite("root_signature_algorithm",
                   &repo::ArtifactCapabilityRequirements::rootSignatureAlgorithm)
    .def_readwrite("chunk_bytes", &repo::ArtifactCapabilityRequirements::chunkBytes)
    .def_readwrite("root_encoded_bytes",
                   &repo::ArtifactCapabilityRequirements::rootEncodedBytes)
    .def_readwrite("page_encoded_bytes",
                   &repo::ArtifactCapabilityRequirements::pageEncodedBytes)
    .def_readwrite("page_entries", &repo::ArtifactCapabilityRequirements::pageEntries)
    .def_readwrite("manifest_depth",
                   &repo::ArtifactCapabilityRequirements::manifestDepth)
    .def_readwrite("require_resume",
                   &repo::ArtifactCapabilityRequirements::requireResume)
    .def_readwrite("require_replica_receipts",
                   &repo::ArtifactCapabilityRequirements::requireReplicaReceipts);

  py::class_<repo::RepoCandidate>(m, "RepoCandidate")
    .def_readonly("repo_node", &repo::RepoCandidate::repoNode)
    .def_property_readonly("provider_identity", [] (const repo::RepoCandidate& value) {
      return value.providerIdentity.toUri();
    })
    .def_property_readonly("service_prefix", [] (const repo::RepoCandidate& value) {
      return value.servicePrefix.toUri();
    })
    .def_readonly("capability", &repo::RepoCandidate::capability)
    .def_readonly("observed_at_ms", &repo::RepoCandidate::observedAtMs)
    .def_readonly("ack_wire_digest", &repo::RepoCandidate::ackWireDigest)
    .def_readonly("signer_key_locator", &repo::RepoCandidate::signerKeyLocator);

  py::class_<repo::RepoCandidateSet>(m, "RepoCandidateSet")
    .def_readonly("candidates", &repo::RepoCandidateSet::candidates)
    .def_readonly("observed_at_ms", &repo::RepoCandidateSet::observedAtMs)
    .def_property_readonly("request_id", [] (const repo::RepoCandidateSet& value) {
      return value.requestId.toUri();
    });

  py::class_<repo::SelectedRepoTarget>(m, "SelectedRepoTarget")
    .def(py::init<>())
    .def_readwrite("repo_node", &repo::SelectedRepoTarget::repoNode)
    .def_property("provider_identity",
      [] (const repo::SelectedRepoTarget& value) {
        return value.providerIdentity.toUri();
      },
      [] (repo::SelectedRepoTarget& value, const std::string& name) {
        value.providerIdentity = ndn::Name(name);
      })
    .def_property("service_prefix",
      [] (const repo::SelectedRepoTarget& value) {
        return value.servicePrefix.toUri();
      },
      [] (repo::SelectedRepoTarget& value, const std::string& name) {
        value.servicePrefix = ndn::Name(name);
      });

  py::class_<repo::SelectedRepoSet>(m, "SelectedRepoSet")
    .def(py::init<>())
    .def_readwrite("targets", &repo::SelectedRepoSet::targets)
    .def_readwrite("seed_repo_id", &repo::SelectedRepoSet::seedRepoId)
    .def_readwrite("consistency", &repo::SelectedRepoSet::consistency);

  py::class_<repo::RepoArtifactTarget>(m, "RepoArtifactTarget")
    .def(py::init<>())
    .def_readwrite("repo_node", &repo::RepoArtifactTarget::repoNode)
    .def_readwrite("provider_identity", &repo::RepoArtifactTarget::providerIdentity)
    .def_readwrite("service_prefix", &repo::RepoArtifactTarget::servicePrefix);

  py::class_<repo::RepoArtifactCommitReceipt>(m, "RepoArtifactCommitReceipt")
    .def_readonly("schema_version", &repo::RepoArtifactCommitReceipt::schemaVersion)
    .def_readonly("repo_node", &repo::RepoArtifactCommitReceipt::repoNode)
    .def_readonly("provider_identity",
                  &repo::RepoArtifactCommitReceipt::providerIdentity)
    .def_readonly("operation_id", &repo::RepoArtifactCommitReceipt::operationId)
    .def_readonly("receipt_id", &repo::RepoArtifactCommitReceipt::receiptId)
    .def_readonly("reference", &repo::RepoArtifactCommitReceipt::reference)
    .def_readonly("generation", &repo::RepoArtifactCommitReceipt::generation)
    .def_readonly("persisted_bytes", &repo::RepoArtifactCommitReceipt::persistedBytes)
    .def_readonly("completed_at_ms", &repo::RepoArtifactCommitReceipt::completedAtMs)
    .def_readonly("location_version", &repo::RepoArtifactCommitReceipt::locationVersion)
    .def_readonly("route_state", &repo::RepoArtifactCommitReceipt::routeState)
    .def_readonly("forwarding_hint", &repo::RepoArtifactCommitReceipt::forwardingHint)
    .def("to_json", &repo::RepoArtifactCommitReceipt::toJson);

  py::class_<repo::PublishOperationTargetStatus>(m,
                                                  "PublishOperationTargetStatus")
    .def_readonly("target", &repo::PublishOperationTargetStatus::target)
    .def_readonly("state", &repo::PublishOperationTargetStatus::state)
    .def_readonly("attempt_count", &repo::PublishOperationTargetStatus::attemptCount)
    .def_readonly("updated_at_ms", &repo::PublishOperationTargetStatus::updatedAtMs)
    .def_readonly("receipt", &repo::PublishOperationTargetStatus::receipt)
    .def_readonly("failure_code", &repo::PublishOperationTargetStatus::failureCode);

  py::class_<repo::PublishOperationStatus>(m, "PublishOperationStatus")
    .def_readonly("schema_version", &repo::PublishOperationStatus::schemaVersion)
    .def_readonly("operation_id", &repo::PublishOperationStatus::operationId)
    .def_readonly("reference", &repo::PublishOperationStatus::reference)
    .def_readonly("generation", &repo::PublishOperationStatus::generation)
    .def_readonly("seed", &repo::PublishOperationStatus::seed)
    .def_readonly("consistency", &repo::PublishOperationStatus::consistency)
    .def_readonly("targets", &repo::PublishOperationStatus::targets)
    .def_readonly("state", &repo::PublishOperationStatus::state)
    .def_readonly("updated_at_ms", &repo::PublishOperationStatus::updatedAtMs)
    .def_readonly("receipt_id", &repo::PublishOperationStatus::receiptId)
    .def("to_json", &repo::PublishOperationStatus::toJson)
    .def_static("from_json", &repo::PublishOperationStatus::fromJson,
                py::arg("wire"), py::arg("limits") = repo::ArtifactLimits{});

  py::class_<repo::PublishOperationHandle>(m, "PublishOperationHandle")
    .def_readonly("schema_version", &repo::PublishOperationHandle::schemaVersion)
    .def_readonly("operation_id", &repo::PublishOperationHandle::operationId)
    .def_readonly("seed", &repo::PublishOperationHandle::seed)
    .def("to_json", &repo::PublishOperationHandle::toJson)
    .def_static("from_json", &repo::PublishOperationHandle::fromJson,
                py::arg("wire"), py::arg("limits") = repo::ArtifactLimits{});

  py::class_<repo::ArtifactLocalCommitRecord>(m, "ArtifactLocalCommitRecord")
    .def_readonly("local_commit_id", &repo::ArtifactLocalCommitRecord::localCommitId)
    .def_readonly("reference", &repo::ArtifactLocalCommitRecord::reference)
    .def_readonly("committed_at_ms", &repo::ArtifactLocalCommitRecord::committedAtMs);

  py::class_<repo::ArtifactReplicaCommit>(m, "ArtifactReplicaCommit")
    .def_readonly("repo_node", &repo::ArtifactReplicaCommit::repoNode)
    .def_readonly("provider_identity", &repo::ArtifactReplicaCommit::providerIdentity)
    .def_readonly("receipt_id", &repo::ArtifactReplicaCommit::receiptId)
    .def_readonly("operation_id", &repo::ArtifactReplicaCommit::operationId)
    .def_readonly("reference", &repo::ArtifactReplicaCommit::reference)
    .def_readonly("generation", &repo::ArtifactReplicaCommit::generation)
    .def_readonly("content_digest", &repo::ArtifactReplicaCommit::contentDigest)
    .def_readonly("persisted_bytes", &repo::ArtifactReplicaCommit::persistedBytes)
    .def_readonly("completed_at_ms", &repo::ArtifactReplicaCommit::completedAtMs)
    .def_readonly("location_version", &repo::ArtifactReplicaCommit::locationVersion)
    .def_readonly("route_state", &repo::ArtifactReplicaCommit::routeState)
    .def_readonly("state", &repo::ArtifactReplicaCommit::state)
    .def_readonly("failure_reason", &repo::ArtifactReplicaCommit::failureReason)
    .def_readonly("forwarding_hint", &repo::ArtifactReplicaCommit::forwardingHint);

  py::class_<repo::ArtifactHandle>(m, "ArtifactHandle")
    .def_readonly("schema_version", &repo::ArtifactHandle::schemaVersion)
    .def_readonly("reference", &repo::ArtifactHandle::reference)
    .def_readonly("commit_receipt_ids", &repo::ArtifactHandle::commitReceiptIds)
    .def_readonly("selected_targets", &repo::ArtifactHandle::selectedTargets)
    .def_readonly("replica_commits", &repo::ArtifactHandle::replicaCommits)
    .def_readonly("seed_repo_id", &repo::ArtifactHandle::seedRepoId)
    .def_readonly("write_consistency", &repo::ArtifactHandle::writeConsistency)
    .def_readonly("lifecycle_mode", &repo::ArtifactHandle::lifecycleMode)
    .def("to_json", &repo::ArtifactHandle::toJson)
    .def_static("from_json", &repo::ArtifactHandle::fromJson,
                py::arg("wire"), py::arg("limits") = repo::ArtifactLimits{});

  py::class_<repo::ArtifactCommitReceipt>(m, "ArtifactCommitReceipt")
    .def_readonly("receipt_id", &repo::ArtifactCommitReceipt::receiptId)
    .def_readonly("operation_id", &repo::ArtifactCommitReceipt::operationId)
    .def_readonly("reference", &repo::ArtifactCommitReceipt::reference)
    .def_readonly("generation", &repo::ArtifactCommitReceipt::generation)
    .def_readonly("lifecycle_mode", &repo::ArtifactCommitReceipt::lifecycleMode)
    .def_readonly("placement_state", &repo::ArtifactCommitReceipt::placementState)
    .def_readonly("requested_replicas", &repo::ArtifactCommitReceipt::requestedReplicas)
    .def_readonly("required_acks", &repo::ArtifactCommitReceipt::requiredAcks)
    .def_readonly("achieved_replicas", &repo::ArtifactCommitReceipt::achievedReplicas)
    .def_readonly("replica_commits", &repo::ArtifactCommitReceipt::replicaCommits)
    .def_readonly("local_commit", &repo::ArtifactCommitReceipt::localCommit)
    .def_readonly("write_consistency", &repo::ArtifactCommitReceipt::writeConsistency)
    .def_readonly("state", &repo::ArtifactCommitReceipt::state)
    .def_readonly("completed_at_ms", &repo::ArtifactCommitReceipt::completedAtMs)
    .def_readonly("schema_version", &repo::ArtifactCommitReceipt::schemaVersion)
    .def("to_json", &repo::ArtifactCommitReceipt::toJson)
    .def_static("from_json", &repo::ArtifactCommitReceipt::fromJson,
                py::arg("wire"), py::arg("limits") = repo::ArtifactLimits{});

  py::class_<repo::ArtifactReplicaResult>(m, "ArtifactReplicaResult")
    .def_readonly("repo_node", &repo::ArtifactReplicaResult::repoNode)
    .def_readonly("provider_identity", &repo::ArtifactReplicaResult::providerIdentity)
    .def_readonly("state", &repo::ArtifactReplicaResult::state)
    .def_readonly("receipt_id", &repo::ArtifactReplicaResult::receiptId)
    .def_readonly("error_code", &repo::ArtifactReplicaResult::errorCode);

  py::class_<repo::ArtifactPublishResult>(m, "ArtifactPublishResult")
    .def_readonly("reference", &repo::ArtifactPublishResult::reference)
    .def_readonly("operation_id", &repo::ArtifactPublishResult::operationId)
    .def_readonly("requested_replicas", &repo::ArtifactPublishResult::requestedReplicas)
    .def_readonly("achieved_replicas", &repo::ArtifactPublishResult::achievedReplicas)
    .def_readonly("replicas", &repo::ArtifactPublishResult::replicas)
    .def_readonly("deduplicated", &repo::ArtifactPublishResult::deduplicated)
    .def_readonly("resumed", &repo::ArtifactPublishResult::resumed)
    .def_readonly("total_duration_ms", &repo::ArtifactPublishResult::totalDurationMs)
    .def_readonly("phase_durations_ms", &repo::ArtifactPublishResult::phaseDurationsMs)
    .def_readonly("handle", &repo::ArtifactPublishResult::handle)
    .def_readonly("receipt", &repo::ArtifactPublishResult::receipt)
    .def_readonly("operation_handle", &repo::ArtifactPublishResult::operationHandle)
    .def_readonly("operation_status", &repo::ArtifactPublishResult::operationStatus);

  py::class_<repo::ArtifactFetchAttempt>(m, "ArtifactFetchAttempt")
    .def_readonly("repo_node", &repo::ArtifactFetchAttempt::repoNode)
    .def_readonly("provider_identity", &repo::ArtifactFetchAttempt::providerIdentity)
    .def_readonly("was_cooling_down_at_selection",
                  &repo::ArtifactFetchAttempt::wasCoolingDownAtSelection)
    .def_readonly("location_version", &repo::ArtifactFetchAttempt::locationVersion)
    .def_readonly("request_timeout_budget_ms",
                  &repo::ArtifactFetchAttempt::requestTimeoutBudgetMs)
    .def_readonly("elapsed_ms", &repo::ArtifactFetchAttempt::elapsedMs)
    .def_readonly("bytes_transferred", &repo::ArtifactFetchAttempt::bytesTransferred)
    .def_readonly("bytes_verified", &repo::ArtifactFetchAttempt::bytesVerified)
    .def_readonly("outcome", &repo::ArtifactFetchAttempt::outcome)
    .def_readonly("failure_class", &repo::ArtifactFetchAttempt::failureClass);

  py::class_<repo::ArtifactFetchResult>(m, "ArtifactFetchResult")
    .def_readonly("reference", &repo::ArtifactFetchResult::reference)
    .def_readonly("repo_node", &repo::ArtifactFetchResult::repoNode)
    .def_readonly("provider_identity", &repo::ArtifactFetchResult::providerIdentity)
    .def_readonly("bytes_transferred", &repo::ArtifactFetchResult::bytesTransferred)
    .def_readonly("bytes_verified", &repo::ArtifactFetchResult::bytesVerified)
    .def_readonly("total_elapsed_ms", &repo::ArtifactFetchResult::totalElapsedMs)
    .def_readonly("attempts", &repo::ArtifactFetchResult::attempts);

  py::class_<repo::AvailabilityLocationReport>(m, "AvailabilityLocationReport")
    .def_readonly("schema_version", &repo::AvailabilityLocationReport::schemaVersion)
    .def_readonly("location_kind", &repo::AvailabilityLocationReport::locationKind)
    .def_readonly("repo_node", &repo::AvailabilityLocationReport::repoNode)
    .def_readonly("provider_identity", &repo::AvailabilityLocationReport::providerIdentity)
    .def_readonly("generation", &repo::AvailabilityLocationReport::generation)
    .def_readonly("operation_id", &repo::AvailabilityLocationReport::operationId)
    .def_readonly("receipt_id", &repo::AvailabilityLocationReport::receiptId)
    .def_readonly("location_version", &repo::AvailabilityLocationReport::locationVersion)
    .def_readonly("reachability", &repo::AvailabilityLocationReport::reachability)
    .def_readonly("object_presence", &repo::AvailabilityLocationReport::objectPresence)
    .def_readonly("integrity", &repo::AvailabilityLocationReport::integrity)
    .def_readonly("observed_at_ms", &repo::AvailabilityLocationReport::observedAtMs)
    .def_readonly("valid_until_ms", &repo::AvailabilityLocationReport::validUntilMs)
    .def_readonly("probe_kind", &repo::AvailabilityLocationReport::probeKind)
    .def_readonly("reason", &repo::AvailabilityLocationReport::reason)
    .def("to_json", &repo::AvailabilityLocationReport::toJson);

  py::class_<repo::AvailabilityReport>(m, "AvailabilityReport")
    .def_readonly("schema_version", &repo::AvailabilityReport::schemaVersion)
    .def_readonly("reference", &repo::AvailabilityReport::reference)
    .def_readonly("probe_kind", &repo::AvailabilityReport::probeKind)
    .def_readonly("locations", &repo::AvailabilityReport::locations)
    .def("to_json", &repo::AvailabilityReport::toJson);

  py::object fetchErrorType = py::reinterpret_steal<py::object>(
    PyErr_NewException("py_repoclient._py_repoclient.ArtifactFetchError",
                       PyExc_RuntimeError, nullptr));
  if (!fetchErrorType) {
    throw py::error_already_set();
  }
  m.attr("ArtifactFetchError") = fetchErrorType;
  g_artifactFetchErrorType = fetchErrorType.ptr();
  Py_INCREF(g_artifactFetchErrorType);
  py::register_exception_translator(&translateArtifactFetchError);

  py::class_<BoundArtifactRepositoryApi>(m, "NativeArtifactRepositoryApi")
    .def_static("make_in_app", [] (repo::InAppRepoConfig config) {
      return std::make_unique<BoundArtifactRepositoryApi>(
        repo::ArtifactRepositoryApi::makeInApp(std::move(config)), py::none());
    }, py::arg("config"))
    .def_static("make_persistent", [] (py::object nativeServiceUser,
                                       repo::PersistentRepoClientConfig config) {
      auto& serviceUser = serviceUserFromOwner(nativeServiceUser);
      auto api = repo::ArtifactRepositoryApi::makePersistent(
        serviceUser, std::move(config));
      return std::make_unique<BoundArtifactRepositoryApi>(
        std::move(api), std::move(nativeServiceUser));
    }, py::arg("native_service_user"),
       py::arg("config") = repo::PersistentRepoClientConfig{})
    .def("publish_file", [] (BoundArtifactRepositoryApi& self,
                             const std::string& path,
                             const repo::ArtifactReference& reference) {
      return withoutGil([&] { return self.api.publishFile(path, reference); });
    }, py::arg("source_path"), py::arg("reference"))
    .def("publish_file", [] (BoundArtifactRepositoryApi& self,
                             const std::string& path,
                             const repo::ArtifactReference& reference,
                             const repo::SelectedRepoSet& selected,
                             int timeoutMs) {
      return withoutGil([&] {
        return self.api.publishFile(path, reference, selected, timeoutMs);
      });
    }, py::arg("source_path"), py::arg("reference"),
       py::arg("selected"), py::arg("timeout_ms"))
    .def("begin_publish_file", [] (BoundArtifactRepositoryApi& self,
                                    const std::string& path,
                                    const repo::ArtifactReference& reference,
                                    const repo::SelectedRepoSet& selected,
                                    const std::string& idempotencyKey,
                                    int timeoutMs) {
      return withoutGil([&] {
        return self.api.beginPublishFile(path, reference, selected,
                                         idempotencyKey, timeoutMs);
      });
    }, py::arg("source_path"), py::arg("reference"), py::arg("selected"),
       py::arg("idempotency_key"), py::arg("timeout_ms"))
    .def("begin_archive", [] (BoundArtifactRepositoryApi& self,
                              const BoundArtifactRepositoryApi& inAppSource,
                              const repo::ArtifactHandle& handle,
                              const repo::SelectedRepoSet& selected,
                              const std::string& idempotencyKey,
                              int timeoutMs) {
      return withoutGil([&] {
        return self.api.beginArchive(inAppSource.api, handle, selected,
                                     idempotencyKey, timeoutMs);
      });
    }, py::arg("in_app_source"), py::arg("handle"), py::arg("selected"),
       py::arg("idempotency_key"), py::arg("timeout_ms"))
    .def("get_publish_status", [] (const BoundArtifactRepositoryApi& self,
                                   const repo::PublishOperationHandle& handle,
                                   int timeoutMs) {
      return withoutGil([&] { return self.api.getPublishStatus(handle, timeoutMs); });
    }, py::arg("handle"), py::arg("timeout_ms") = 0)
    .def("discover_repo_candidates", [] (const BoundArtifactRepositoryApi& self,
                                         const repo::ArtifactCapabilityRequirements& requirements,
                                         int timeoutMs) {
      return withoutGil([&] {
        return self.api.discoverRepoCandidates(requirements, timeoutMs);
      });
    }, py::arg("requirements"), py::arg("timeout_ms"))
    .def("archive", [] (BoundArtifactRepositoryApi& self,
                        const BoundArtifactRepositoryApi& inAppSource,
                        const repo::ArtifactHandle& handle,
                        const repo::SelectedRepoSet& selected,
                        int timeoutMs) {
      return withoutGil([&] {
        return self.api.archive(inAppSource.api, handle, selected, timeoutMs);
      });
    }, py::arg("in_app_source"), py::arg("handle"), py::arg("selected"),
       py::arg("timeout_ms"))
    .def("fetch_file", [] (const BoundArtifactRepositoryApi& self,
                           const repo::ArtifactReference& reference,
                           const std::string& destination) {
      withoutGil([&] { self.api.fetchFile(reference, destination); });
    }, py::arg("reference"), py::arg("destination_path"))
    .def("fetch_file", [] (const BoundArtifactRepositoryApi& self,
                           const repo::ArtifactHandle& handle,
                           const std::string& destination,
                           int timeoutMs) {
      return withoutGil([&] {
        return self.api.fetchFile(handle, destination, timeoutMs);
      });
    }, py::arg("handle"), py::arg("destination_path"), py::arg("timeout_ms"))
    .def("lookup_local_commit", [] (const BoundArtifactRepositoryApi& self,
                                    const repo::ArtifactReference& reference) {
      return withoutGil([&] { return self.api.lookupLocalCommit(reference); });
    }, py::arg("reference"))
    .def("get_receipt", [] (const BoundArtifactRepositoryApi& self,
                            const std::string& receiptId,
                            std::optional<repo::ArtifactHandle> handle,
                            int timeoutMs) {
      return withoutGil([&] {
        return self.api.getReceipt(receiptId, std::move(handle), timeoutMs);
      });
    }, py::arg("receipt_id"), py::arg("handle") = std::nullopt,
       py::arg("timeout_ms") = 0)
    .def("check_availability", [] (const BoundArtifactRepositoryApi& self,
                                   const repo::ArtifactHandle& handle,
                                   const std::string& probeKind,
                                   int timeoutMs,
                                   uint64_t maxAgeMs) {
      return withoutGil([&] {
        return self.api.checkAvailability(handle, probeKind, timeoutMs, maxAgeMs);
      });
    }, py::arg("handle"), py::arg("probe_kind") = "metadata",
       py::arg("timeout_ms") = 0, py::arg("max_age_ms") = 0);
}

} // namespace

void
bindArtifactRepositoryApi(py::module_& m)
{
  bindArtifactRepositoryTypes(m);
}
