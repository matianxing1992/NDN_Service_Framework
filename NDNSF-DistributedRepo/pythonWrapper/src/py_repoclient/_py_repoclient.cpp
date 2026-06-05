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
    .def_readwrite("policy_epoch", &repo::RepoObjectManifest::policyEpoch)
    .def("to_json", &repo::RepoObjectManifest::toJson)
    .def("__repr__", &manifestRepr);

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
