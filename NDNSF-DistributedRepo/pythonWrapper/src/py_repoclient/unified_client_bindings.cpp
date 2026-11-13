#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/repo-client-types.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace py = pybind11;
namespace repo = ndnsf_distributed_repo;

namespace {

repo::FileReference
fileReferenceFromDict(const py::dict& value)
{
  repo::FileReference reference;
  if (value.contains("logical_name"))
    reference.logicalName = py::cast<std::string>(value["logical_name"]);
  if (value.contains("content_digest"))
    reference.contentDigest = py::cast<std::string>(value["content_digest"]);
  if (value.contains("size_bytes"))
    reference.sizeBytes = py::cast<uint64_t>(value["size_bytes"]);
  if (value.contains("format_version"))
    reference.formatVersion = py::cast<std::string>(value["format_version"]);
  if (value.contains("publisher_identity"))
    reference.publisherIdentity = py::cast<std::string>(value["publisher_identity"]);
  if (value.contains("policy_epoch"))
    reference.policyEpoch = py::cast<std::string>(value["policy_epoch"]);
  return reference;
}

py::dict
fileReferenceToDict(const repo::FileReference& reference)
{
  py::dict value;
  value["logical_name"] = reference.logicalName;
  value["digest_algorithm"] = reference.digestAlgorithm;
  value["content_digest"] = reference.contentDigest;
  value["size_bytes"] = reference.sizeBytes;
  value["format_version"] = reference.formatVersion;
  value["publisher_identity"] = reference.publisherIdentity;
  value["policy_epoch"] = reference.policyEpoch;
  return value;
}

repo::RepoOperationOptions
makeOptions(int timeoutMs)
{
  repo::RepoOperationOptions options;
  options.deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(std::max(1, timeoutMs));
  return options;
}

py::dict
publishResultToDict(const repo::RepoPublishResult& result)
{
  py::dict value;
  const auto* file = std::get_if<repo::FileReference>(&result.reference);
  if (file != nullptr)
    value["reference"] = fileReferenceToDict(*file);
  value["state"] = result.state == repo::RepoPublishState::COMMITTED
    ? "COMMITTED" : "ACCEPTED";
  value["deduplicated"] = result.deduplicated;
  value["resumed"] = result.resumed;
  value["receipt_ids"] = result.receiptIds;
  if (result.operationReference)
    value["operation_reference"] = *result.operationReference;
  py::list replicas;
  for (const auto& replica : result.replicas) {
    py::dict entry;
    entry["target_id"] = replica.targetId;
    entry["receipt_id"] = replica.receiptId;
    entry["message"] = replica.message;
    replicas.append(entry);
  }
  value["replicas"] = replicas;
  return value;
}

py::dict
fetchResultToDict(const repo::RepoFetchResult& result)
{
  py::dict value;
  value["bytes_written"] = result.bytesWritten;
  value["digest_verified"] = result.digestVerified;
  value["digest_algorithm"] = result.digestAlgorithm;
  value["content_digest"] = result.contentDigest;
  value["attempted_sources"] = result.attemptedSources;
  return value;
}

} // namespace

void
bindUnifiedRepoClient(py::module_& m)
{
  // The unified client is the only Repo client surface a host-side caller needs:
  // these entry points construct one facade per call and return plain dictionaries
  // so the native types stay authoritative without duplicating them in Python.
  m.def("unified_publish_file",
        [](const std::string& durableRoot, const std::string& ownerId,
           const std::string& sourcePath, const py::dict& reference,
           int timeoutMs) {
          repo::InAppRepoConfig config;
          config.durableRoot = durableRoot;
          config.ownerId = ownerId;
          auto client = repo::RepoClient::makeInApp(std::move(config));
          repo::RepoFilePublication publication;
          publication.sourcePath = sourcePath;
          publication.reference = fileReferenceFromDict(reference);
          return publishResultToDict(client.publishFile(publication, makeOptions(timeoutMs)));
        },
        py::arg("durable_root"), py::arg("owner_id"), py::arg("source_path"),
        py::arg("reference"), py::arg("timeout_ms") = 60000);

  m.def("unified_fetch_file",
        [](const std::string& durableRoot, const std::string& ownerId,
           const py::dict& reference, const std::string& targetPath, int timeoutMs) {
          repo::InAppRepoConfig config;
          config.durableRoot = durableRoot;
          config.ownerId = ownerId;
          auto client = repo::RepoClient::makeInApp(std::move(config));
          repo::RepoFileFetch request;
          request.reference = fileReferenceFromDict(reference);
          request.targetPath = targetPath;
          return fetchResultToDict(client.fetchFile(request, makeOptions(timeoutMs)));
        },
        py::arg("durable_root"), py::arg("owner_id"), py::arg("reference"),
        py::arg("target_path"), py::arg("timeout_ms") = 60000);

  m.def("unified_inspect_object",
        [](const std::string& durableRoot, const std::string& ownerId,
           const py::dict& reference, int timeoutMs) {
          repo::InAppRepoConfig config;
          config.durableRoot = durableRoot;
          config.ownerId = ownerId;
          auto client = repo::RepoClient::makeInApp(std::move(config));
          repo::RepoHandle handle;
          handle.reference = fileReferenceFromDict(reference);
          repo::RepoInspectionRequest request;
          request.kind = repo::RepoInspectionRequest::Kind::OBJECT;
          request.object = handle;
          const auto inspection = client.inspect(request, makeOptions(timeoutMs));
          py::dict value;
          value["availability"] =
            inspection.availability == repo::RepoInspection::Availability::AVAILABLE
              ? "AVAILABLE"
              : inspection.availability == repo::RepoInspection::Availability::UNAVAILABLE
                  ? "UNAVAILABLE" : "UNKNOWN";
          value["stored_bytes"] = inspection.storedBytes.value_or(0);
          value["lifecycle_state"] = inspection.lifecycleState;
          return value;
        },
        py::arg("durable_root"), py::arg("owner_id"), py::arg("reference"),
        py::arg("timeout_ms") = 60000);
}
