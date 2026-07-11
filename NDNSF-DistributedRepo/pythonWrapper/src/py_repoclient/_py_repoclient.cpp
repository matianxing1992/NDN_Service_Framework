#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <sstream>

namespace py = pybind11;
namespace repo = ndnsf_distributed_repo;

namespace {

std::vector<uint8_t>
bytesToVector(const py::bytes& value)
{
  std::string text = value;
  return std::vector<uint8_t>(text.begin(), text.end());
}

py::bytes
vectorToBytes(const std::vector<uint8_t>& value)
{
  return py::bytes(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string
manifestRepr(const repo::RepoObjectManifest& manifest)
{
  std::ostringstream os;
  os << "RepoObjectManifest(object_name='" << manifest.objectName
     << "', object_type='" << manifest.objectType
     << "', size=" << manifest.size
     << ", segment_count=" << manifest.segmentCount
     << ", replication_factor=" << manifest.replicationFactor << ")";
  return os.str();
}

std::string
capabilityRepr(const repo::StorageCapability& capability)
{
  std::ostringstream os;
  os << "StorageCapability(repo_node='" << capability.repoNode
     << "', repo_mode='" << capability.repoMode
     << "', accepts_backup_replica=" << capability.acceptsBackupReplica
     << ", free_bytes=" << capability.freeBytes
     << ", used_bytes=" << capability.usedBytes
     << ", recent_load=" << capability.recentLoad
     << ", availability_score=" << capability.availabilityScore << ")";
  return os.str();
}

} // namespace

PYBIND11_MODULE(_py_repoclient, m)
{
  m.doc() = "NDNSF-DistributedRepo RepoClient bindings";

  py::class_<repo::RepoObjectManifest>(m, "RepoObjectManifest")
    .def(py::init<>())
    .def_readwrite("object_name", &repo::RepoObjectManifest::objectName)
    .def_readwrite("object_type", &repo::RepoObjectManifest::objectType)
    .def_readwrite("sha256", &repo::RepoObjectManifest::sha256)
    .def_readwrite("size", &repo::RepoObjectManifest::size)
    .def_readwrite("segment_count", &repo::RepoObjectManifest::segmentCount)
    .def_readwrite("replication_factor", &repo::RepoObjectManifest::replicationFactor)
    .def_readwrite("replica_nodes", &repo::RepoObjectManifest::replicaNodes)
    .def_readwrite("packet_names", &repo::RepoObjectManifest::packetNames)
    .def_readwrite("policy_epoch", &repo::RepoObjectManifest::policyEpoch)
    .def_readwrite("generation", &repo::RepoObjectManifest::generation)
    .def_readwrite("parent_generation", &repo::RepoObjectManifest::parentGeneration)
    .def_readwrite("write_consistency", &repo::RepoObjectManifest::writeConsistency)
    .def_readwrite("required_write_acks", &repo::RepoObjectManifest::requiredWriteAcks)
    .def_readwrite("confirmed_replica_nodes", &repo::RepoObjectManifest::confirmedReplicaNodes)
    .def_readwrite("operation_id", &repo::RepoObjectManifest::operationId)
    .def_readwrite("lifecycle_state", &repo::RepoObjectManifest::lifecycleState)
    .def("to_json", &repo::RepoObjectManifest::toJson)
    .def("__repr__", &manifestRepr);

  py::class_<repo::RepoDataReference>(m, "RepoDataReference")
    .def(py::init<>())
    .def_readwrite("object_name", &repo::RepoDataReference::objectName)
    .def_readwrite("data_prefix", &repo::RepoDataReference::dataPrefix)
    .def_readwrite("first_segment", &repo::RepoDataReference::firstSegment)
    .def_readwrite("final_segment", &repo::RepoDataReference::finalSegment)
    .def_readwrite("has_final_segment", &repo::RepoDataReference::hasFinalSegment)
    .def_readwrite("forwarding_hint", &repo::RepoDataReference::forwardingHint)
    .def_readwrite("expected_sha256", &repo::RepoDataReference::expectedSha256)
    .def_readwrite("expected_size", &repo::RepoDataReference::expectedSize)
    .def_readwrite("store_wire_packets", &repo::RepoDataReference::storeWirePackets)
    .def_readwrite("object_type", &repo::RepoDataReference::objectType)
    .def("to_json", &repo::RepoDataReference::toJson);

  py::class_<repo::RepoOperationStatus>(m, "RepoOperationStatus")
    .def(py::init<>())
    .def_readwrite("operation_id", &repo::RepoOperationStatus::operationId)
    .def_readwrite("operation", &repo::RepoOperationStatus::operation)
    .def_readwrite("state", &repo::RepoOperationStatus::state)
    .def_readwrite("object_name", &repo::RepoOperationStatus::objectName)
    .def_readwrite("message", &repo::RepoOperationStatus::message)
    .def_readwrite("completed_segments", &repo::RepoOperationStatus::completedSegments)
    .def_readwrite("total_segments", &repo::RepoOperationStatus::totalSegments)
    .def_readwrite("created_at_ms", &repo::RepoOperationStatus::createdAtMs)
    .def_readwrite("updated_at_ms", &repo::RepoOperationStatus::updatedAtMs)
    .def_readwrite("expires_at_ms", &repo::RepoOperationStatus::expiresAtMs)
    .def("to_json", &repo::RepoOperationStatus::toJson);

  py::class_<repo::StorageCapability>(m, "StorageCapability")
    .def(py::init<>())
    .def_readwrite("repo_node", &repo::StorageCapability::repoNode)
    .def_readwrite("free_bytes", &repo::StorageCapability::freeBytes)
    .def_readwrite("used_bytes", &repo::StorageCapability::usedBytes)
    .def_readwrite("recent_load", &repo::StorageCapability::recentLoad)
    .def_readwrite("availability_score", &repo::StorageCapability::availabilityScore)
    .def_readwrite("failure_domain", &repo::StorageCapability::failureDomain)
    .def_readwrite("storage_classes", &repo::StorageCapability::storageClasses)
    .def_readwrite("repo_mode", &repo::StorageCapability::repoMode)
    .def_readwrite("accepts_backup_replica", &repo::StorageCapability::acceptsBackupReplica)
    .def("to_json", &repo::StorageCapability::toJson)
    .def("__repr__", &capabilityRepr);

  py::class_<repo::PlacementPolicy>(m, "PlacementPolicy")
    .def(py::init<>())
    .def_readwrite("replication_factor", &repo::PlacementPolicy::replicationFactor)
    .def_readwrite("avoid_same_failure_domain", &repo::PlacementPolicy::avoidSameFailureDomain)
    .def_readwrite("prefer_low_load", &repo::PlacementPolicy::preferLowLoad)
    .def_readwrite("prefer_high_availability", &repo::PlacementPolicy::preferHighAvailability);

  py::class_<repo::RepoCatalogEntry>(m, "RepoCatalogEntry")
    .def(py::init<>())
    .def_readwrite("manifest", &repo::RepoCatalogEntry::manifest)
    .def_readwrite("source_repo", &repo::RepoCatalogEntry::sourceRepo)
    .def_readwrite("repo_mode", &repo::RepoCatalogEntry::repoMode)
    .def_readwrite("state", &repo::RepoCatalogEntry::state)
    .def_readwrite("catalog_epoch", &repo::RepoCatalogEntry::catalogEpoch)
    .def("to_json", &repo::RepoCatalogEntry::toJson);

  py::class_<repo::RepoCatalogStatus>(m, "RepoCatalogStatus")
    .def(py::init<>())
    .def_readwrite("repo_node", &repo::RepoCatalogStatus::repoNode)
    .def_readwrite("repo_mode", &repo::RepoCatalogStatus::repoMode)
    .def_readwrite("catalog_epoch", &repo::RepoCatalogStatus::catalogEpoch)
    .def_readwrite("object_count", &repo::RepoCatalogStatus::objectCount)
    .def_readwrite("accepts_backup_replica", &repo::RepoCatalogStatus::acceptsBackupReplica)
    .def("to_json", &repo::RepoCatalogStatus::toJson);

  py::class_<repo::RepoCatalogDelta>(m, "RepoCatalogDelta")
    .def(py::init<>())
    .def_readwrite("repo_node", &repo::RepoCatalogDelta::repoNode)
    .def_readwrite("repo_mode", &repo::RepoCatalogDelta::repoMode)
    .def_readwrite("since_epoch", &repo::RepoCatalogDelta::sinceEpoch)
    .def_readwrite("catalog_epoch", &repo::RepoCatalogDelta::catalogEpoch)
    .def_readwrite("entries", &repo::RepoCatalogDelta::entries)
    .def("to_json", &repo::RepoCatalogDelta::toJson);

  py::class_<repo::RepoCacheStatus>(m, "RepoCacheStatus")
    .def(py::init<>())
    .def_readwrite("storage_backend", &repo::RepoCacheStatus::storageBackend)
    .def_readwrite("authoritative_backend", &repo::RepoCacheStatus::authoritativeBackend)
    .def_readwrite("cache_policy", &repo::RepoCacheStatus::cachePolicy)
    .def_readwrite("budget_bytes", &repo::RepoCacheStatus::budgetBytes)
    .def_readwrite("used_bytes", &repo::RepoCacheStatus::usedBytes)
    .def_readwrite("entry_count", &repo::RepoCacheStatus::entryCount)
    .def_readwrite("hits", &repo::RepoCacheStatus::hits)
    .def_readwrite("misses", &repo::RepoCacheStatus::misses)
    .def_readwrite("admissions", &repo::RepoCacheStatus::admissions)
    .def_readwrite("evictions", &repo::RepoCacheStatus::evictions)
    .def_readwrite("invalidations", &repo::RepoCacheStatus::invalidations)
    .def_readwrite("oversized_bypasses", &repo::RepoCacheStatus::oversizedBypasses)
    .def_readwrite("backing_reads", &repo::RepoCacheStatus::backingReads)
    .def_readwrite("backing_writes", &repo::RepoCacheStatus::backingWrites)
    .def("to_json", &repo::RepoCacheStatus::toJson);

  m.def("sha256_hex",
        [](const py::bytes& payload) {
          return repo::sha256Hex(bytesToVector(payload));
        },
        py::arg("payload"));

  m.def("make_repo_service_name",
        [](const std::string& prefix, const std::string& operation) {
          return repo::makeRepoServiceName(ndn::Name(prefix), operation).toUri();
        },
        py::arg("prefix"),
        py::arg("operation"));

  m.def("make_manifest",
        [](const std::string& objectName,
           const std::string& objectType,
           const py::bytes& payload,
           uint32_t replicationFactor,
           std::vector<std::string> replicaNodes,
           const std::string& policyEpoch) {
          return repo::RepoClient::makeManifest(objectName, objectType,
                                                bytesToVector(payload),
                                                replicationFactor,
                                                std::move(replicaNodes),
                                                policyEpoch);
        },
        py::arg("object_name"),
        py::arg("object_type"),
        py::arg("payload"),
        py::arg("replication_factor") = 1,
        py::arg("replica_nodes") = std::vector<std::string>{},
        py::arg("policy_epoch") = "");

  m.def("parse_manifest_json",
        &repo::parseManifestJson,
        py::arg("manifest_json"));

  m.def("parse_data_reference_json", &repo::parseDataReferenceJson,
        py::arg("reference_json"));
  m.def("parse_operation_status_json", &repo::parseOperationStatusJson,
        py::arg("status_json"));
  m.def("parse_catalog_entry_json", &repo::parseCatalogEntryJson,
        py::arg("entry_json"));
  m.def("parse_catalog_status_json", &repo::parseCatalogStatusJson,
        py::arg("status_json"));
  m.def("parse_catalog_delta_json", &repo::parseCatalogDeltaJson,
        py::arg("delta_json"));
  m.def("parse_cache_status_json", &repo::parseCacheStatusJson,
        py::arg("status_json"));
  m.def("parse_inventory_json", &repo::parseInventoryJson,
        py::arg("inventory_json"));

  m.def("encode_inventory",
        &repo::encodeInventory,
        py::arg("manifests"));

  m.def("encode_store_request",
        [](const repo::RepoObjectManifest& manifest, const py::bytes& payload) {
          return vectorToBytes(repo::encodeStoreRequest(manifest, bytesToVector(payload)));
        },
        py::arg("manifest"),
        py::arg("payload"));

  m.def("decode_store_request",
        [](const py::bytes& request) {
          repo::RepoObjectManifest manifest;
          std::vector<uint8_t> payload;
          repo::decodeStoreRequest(bytesToVector(request), manifest, payload);
          return py::make_tuple(manifest, vectorToBytes(payload));
        },
        py::arg("request"));

  m.def("select_replicas",
        &repo::selectReplicas,
        py::arg("candidates"),
        py::arg("policy"),
        py::arg("object_size"));
}
