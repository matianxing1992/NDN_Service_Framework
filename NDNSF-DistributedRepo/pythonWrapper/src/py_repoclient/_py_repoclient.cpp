#include "ndnsf-distributed-repo/ArtifactManifest.hpp"
#include "ndnsf-distributed-repo/ArtifactTransfer.hpp"
#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <limits>
#include <sstream>
#include <set>

namespace py = pybind11;
namespace repo = ndnsf_distributed_repo;

namespace {

template<typename T>
T
requiredValue(const py::dict& values, const char* key, const char* errorCode)
{
  if (!values.contains(key)) {
    throw repo::ArtifactValidationError(
      errorCode, std::string("missing required field ") + key);
  }
  try {
    return py::cast<T>(values[key]);
  }
  catch (const py::cast_error&) {
    throw repo::ArtifactValidationError(
      errorCode, std::string("field has invalid type: ") + key);
  }
}

template<typename T>
T
optionalValue(const py::dict& values, const char* key, T fallback, const char* errorCode)
{
  if (!values.contains(key)) {
    return fallback;
  }
  try {
    return py::cast<T>(values[key]);
  }
  catch (const py::cast_error&) {
    throw repo::ArtifactValidationError(
      errorCode, std::string("field has invalid type: ") + key);
  }
}

void
rejectUnknownKeys(const py::dict& values, const std::set<std::string>& allowed,
                  const char* errorCode)
{
  for (const auto& item : values) {
    std::string key;
    try {
      key = py::cast<std::string>(item.first);
    }
    catch (const py::cast_error&) {
      throw repo::ArtifactValidationError(errorCode, "field names must be strings");
    }
    if (allowed.count(key) == 0) {
      throw repo::ArtifactValidationError(errorCode, "unknown field " + key);
    }
  }
}

repo::ArtifactReference
artifactReferenceFromDict(const py::dict& values, const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "logicalName", "digestAlgorithm", "contentDigest", "sizeBytes",
    "formatVersion", "rootManifestName", "publisherIdentity", "policyEpoch",
  }, repo::artifact_error::InvalidManifest);
  repo::ArtifactReference reference;
  reference.logicalName = requiredValue<std::string>(
    values, "logicalName", repo::artifact_error::InvalidManifest);
  reference.digestAlgorithm = optionalValue<std::string>(
    values, "digestAlgorithm", "sha256", repo::artifact_error::InvalidManifest);
  reference.contentDigest = requiredValue<std::string>(
    values, "contentDigest", repo::artifact_error::InvalidManifest);
  reference.sizeBytes = requiredValue<uint64_t>(
    values, "sizeBytes", repo::artifact_error::InvalidManifest);
  reference.formatVersion = optionalValue<std::string>(
    values, "formatVersion", "artifact-manifest-v2",
    repo::artifact_error::InvalidManifest);
  reference.rootManifestName = requiredValue<std::string>(
    values, "rootManifestName", repo::artifact_error::InvalidManifest);
  reference.publisherIdentity = requiredValue<std::string>(
    values, "publisherIdentity", repo::artifact_error::InvalidManifest);
  reference.policyEpoch = requiredValue<std::string>(
    values, "policyEpoch", repo::artifact_error::InvalidManifest);
  reference.validate(limits);
  return reference;
}

repo::ArtifactCapability
artifactCapabilityFromDict(const py::dict& values, const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "repoNode", "formatVersions", "digestAlgorithms", "signatureAlgorithms",
    "maxArtifactBytes", "maxChunkBytes", "maxRootEncodedBytes",
    "maxPageEncodedBytes", "maxPageEntries", "maxManifestDepth",
    "supportsResume", "supportsReplicaReceipts", "policyEpoch",
  }, repo::artifact_error::InvalidCapability);
  repo::ArtifactCapability capability;
  capability.repoNode = requiredValue<std::string>(
    values, "repoNode", repo::artifact_error::InvalidCapability);
  capability.formatVersions = requiredValue<std::vector<std::string>>(
    values, "formatVersions", repo::artifact_error::InvalidCapability);
  capability.digestAlgorithms = requiredValue<std::vector<std::string>>(
    values, "digestAlgorithms", repo::artifact_error::InvalidCapability);
  capability.signatureAlgorithms = requiredValue<std::vector<std::string>>(
    values, "signatureAlgorithms", repo::artifact_error::InvalidCapability);
  capability.maxArtifactBytes = requiredValue<uint64_t>(
    values, "maxArtifactBytes", repo::artifact_error::InvalidCapability);
  capability.maxChunkBytes = requiredValue<uint64_t>(
    values, "maxChunkBytes", repo::artifact_error::InvalidCapability);
  capability.maxRootEncodedBytes = requiredValue<uint64_t>(
    values, "maxRootEncodedBytes", repo::artifact_error::InvalidCapability);
  capability.maxPageEncodedBytes = requiredValue<uint64_t>(
    values, "maxPageEncodedBytes", repo::artifact_error::InvalidCapability);
  capability.maxPageEntries = requiredValue<uint32_t>(
    values, "maxPageEntries", repo::artifact_error::InvalidCapability);
  capability.maxManifestDepth = requiredValue<uint32_t>(
    values, "maxManifestDepth", repo::artifact_error::InvalidCapability);
  capability.supportsResume = optionalValue<bool>(
    values, "supportsResume", false, repo::artifact_error::InvalidCapability);
  capability.supportsReplicaReceipts = optionalValue<bool>(
    values, "supportsReplicaReceipts", false,
    repo::artifact_error::InvalidCapability);
  capability.policyEpoch = requiredValue<std::string>(
    values, "policyEpoch", repo::artifact_error::InvalidCapability);
  capability.validate(limits);
  return capability;
}

repo::ArtifactManifestChild
artifactManifestChildFromDict(const py::dict& values,
                              const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "kind", "index", "offsetBytes", "lengthBytes", "digestAlgorithm", "digest",
  }, repo::artifact_error::InvalidManifest);
  repo::ArtifactManifestChild child;
  child.kind = requiredValue<std::string>(
    values, "kind", repo::artifact_error::InvalidManifest);
  child.index = requiredValue<uint64_t>(
    values, "index", repo::artifact_error::InvalidManifest);
  child.offsetBytes = requiredValue<uint64_t>(
    values, "offsetBytes", repo::artifact_error::InvalidManifest);
  child.lengthBytes = requiredValue<uint64_t>(
    values, "lengthBytes", repo::artifact_error::InvalidManifest);
  child.digestAlgorithm = optionalValue<std::string>(
    values, "digestAlgorithm", "sha256", repo::artifact_error::InvalidManifest);
  child.digest = requiredValue<std::string>(
    values, "digest", repo::artifact_error::InvalidManifest);
  child.validate(limits);
  return child;
}

repo::ArtifactRootManifest
artifactRootManifestFromDict(const py::dict& values, uint64_t encodedBytes,
                             const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "artifact", "packetPayloadBytes", "chunkBytes", "namingTemplate",
    "manifestRootDigestAlgorithm", "manifestRootDigest", "signatureAlgorithm",
    "publisherKeyLocator", "createdAtMs", "expiresAtMs", "criticalExtensions",
  }, repo::artifact_error::InvalidManifest);
  repo::ArtifactRootManifest manifest;
  manifest.artifact = artifactReferenceFromDict(
    requiredValue<py::dict>(values, "artifact", repo::artifact_error::InvalidManifest),
    limits);
  manifest.packetPayloadBytes = requiredValue<uint32_t>(
    values, "packetPayloadBytes", repo::artifact_error::InvalidManifest);
  manifest.chunkBytes = requiredValue<uint64_t>(
    values, "chunkBytes", repo::artifact_error::InvalidManifest);
  manifest.namingTemplate = requiredValue<std::string>(
    values, "namingTemplate", repo::artifact_error::InvalidManifest);
  manifest.manifestRootDigestAlgorithm = optionalValue<std::string>(
    values, "manifestRootDigestAlgorithm", "sha256",
    repo::artifact_error::InvalidManifest);
  manifest.manifestRootDigest = requiredValue<std::string>(
    values, "manifestRootDigest", repo::artifact_error::InvalidManifest);
  manifest.signatureAlgorithm = requiredValue<std::string>(
    values, "signatureAlgorithm", repo::artifact_error::InvalidManifest);
  manifest.publisherKeyLocator = requiredValue<std::string>(
    values, "publisherKeyLocator", repo::artifact_error::InvalidManifest);
  manifest.createdAtMs = requiredValue<uint64_t>(
    values, "createdAtMs", repo::artifact_error::InvalidManifest);
  manifest.expiresAtMs = optionalValue<uint64_t>(
    values, "expiresAtMs", 0, repo::artifact_error::InvalidManifest);
  manifest.criticalExtensions = optionalValue<std::vector<std::string>>(
    values, "criticalExtensions", {}, repo::artifact_error::InvalidManifest);
  manifest.validate(encodedBytes, limits);
  return manifest;
}

repo::ArtifactManifestPage
artifactManifestPageFromDict(const py::dict& values, uint64_t encodedBytes,
                             const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "pageVersion", "depth", "offsetBytes", "lengthBytes",
    "pageDigestAlgorithm", "pageDigest", "children",
  }, repo::artifact_error::InvalidManifest);
  repo::ArtifactManifestPage page;
  page.pageVersion = optionalValue<std::string>(
    values, "pageVersion", "artifact-manifest-page-v2",
    repo::artifact_error::InvalidManifest);
  page.depth = requiredValue<uint32_t>(
    values, "depth", repo::artifact_error::InvalidManifest);
  page.offsetBytes = requiredValue<uint64_t>(
    values, "offsetBytes", repo::artifact_error::InvalidManifest);
  page.lengthBytes = requiredValue<uint64_t>(
    values, "lengthBytes", repo::artifact_error::InvalidManifest);
  page.pageDigestAlgorithm = optionalValue<std::string>(
    values, "pageDigestAlgorithm", "sha256", repo::artifact_error::InvalidManifest);
  page.pageDigest = requiredValue<std::string>(
    values, "pageDigest", repo::artifact_error::InvalidManifest);
  const auto children = requiredValue<py::list>(
    values, "children", repo::artifact_error::InvalidManifest);
  if (children.size() > limits.maxPageEntries) {
    throw repo::ArtifactValidationError(
      repo::artifact_error::LimitExceeded,
      "manifest page entry count exceeds policy before child decoding");
  }
  page.children.reserve(children.size());
  for (const auto& value : children) {
    try {
      page.children.push_back(
        artifactManifestChildFromDict(py::cast<py::dict>(value), limits));
    }
    catch (const py::cast_error&) {
      throw repo::ArtifactValidationError(
        repo::artifact_error::InvalidManifest,
        "manifest page children must be dictionaries");
    }
  }
  page.validate(encodedBytes, limits);
  return page;
}

repo::ArtifactChunk
artifactChunkFromDict(const py::dict& values, const repo::ArtifactReference& artifact,
                      const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "index", "offsetBytes", "lengthBytes", "digestAlgorithm", "digest",
    "firstSegment", "finalSegment",
  }, repo::artifact_error::InvalidRange);
  repo::ArtifactChunk chunk;
  chunk.index = requiredValue<uint64_t>(
    values, "index", repo::artifact_error::InvalidRange);
  chunk.offsetBytes = requiredValue<uint64_t>(
    values, "offsetBytes", repo::artifact_error::InvalidRange);
  chunk.lengthBytes = requiredValue<uint64_t>(
    values, "lengthBytes", repo::artifact_error::InvalidRange);
  chunk.digestAlgorithm = optionalValue<std::string>(
    values, "digestAlgorithm", "sha256", repo::artifact_error::InvalidRange);
  chunk.digest = requiredValue<std::string>(
    values, "digest", repo::artifact_error::InvalidRange);
  chunk.firstSegment = requiredValue<uint64_t>(
    values, "firstSegment", repo::artifact_error::InvalidRange);
  chunk.finalSegment = requiredValue<uint64_t>(
    values, "finalSegment", repo::artifact_error::InvalidRange);
  chunk.validate(artifact, limits);
  return chunk;
}

repo::ArtifactUploadLease
artifactUploadLeaseFromDict(const py::dict& values, uint64_t nowMs,
                            const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "leaseId", "operationId", "repoNode", "artifact", "reservedBytes",
    "issuedAtMs", "expiresAtMs", "replayId",
  }, repo::artifact_error::InvalidLease);
  repo::ArtifactUploadLease lease;
  lease.leaseId = requiredValue<std::string>(
    values, "leaseId", repo::artifact_error::InvalidLease);
  lease.operationId = requiredValue<std::string>(
    values, "operationId", repo::artifact_error::InvalidLease);
  lease.repoNode = requiredValue<std::string>(
    values, "repoNode", repo::artifact_error::InvalidLease);
  lease.artifact = artifactReferenceFromDict(
    requiredValue<py::dict>(values, "artifact", repo::artifact_error::InvalidLease),
    limits);
  lease.reservedBytes = requiredValue<uint64_t>(
    values, "reservedBytes", repo::artifact_error::InvalidLease);
  lease.issuedAtMs = requiredValue<uint64_t>(
    values, "issuedAtMs", repo::artifact_error::InvalidLease);
  lease.expiresAtMs = requiredValue<uint64_t>(
    values, "expiresAtMs", repo::artifact_error::InvalidLease);
  lease.replayId = requiredValue<std::string>(
    values, "replayId", repo::artifact_error::InvalidLease);
  lease.validate(nowMs, limits);
  return lease;
}

repo::ArtifactResumeIdentity
artifactResumeIdentityFromDict(const py::dict& values,
                               const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "artifact", "manifestRootDigest", "packetPayloadBytes", "chunkBytes",
  }, repo::artifact_error::InvalidManifest);
  repo::ArtifactResumeIdentity identity;
  identity.artifact = artifactReferenceFromDict(
    requiredValue<py::dict>(
      values, "artifact", repo::artifact_error::InvalidManifest),
    limits);
  identity.manifestRootDigest = requiredValue<std::string>(
    values, "manifestRootDigest", repo::artifact_error::InvalidManifest);
  identity.packetPayloadBytes = requiredValue<uint64_t>(
    values, "packetPayloadBytes", repo::artifact_error::InvalidManifest);
  identity.chunkBytes = requiredValue<uint64_t>(
    values, "chunkBytes", repo::artifact_error::InvalidManifest);
  identity.validate(limits);
  return identity;
}

repo::ArtifactReplicaReceipt
artifactReplicaReceiptFromDict(const py::dict& values,
                               const repo::ArtifactLimits& limits)
{
  rejectUnknownKeys(values, {
    "receiptId", "operationId", "repoNode", "artifact", "committedAtMs",
    "storageGeneration", "policyEpoch", "state",
  }, repo::artifact_error::InvalidReceipt);
  repo::ArtifactReplicaReceipt receipt;
  receipt.receiptId = requiredValue<std::string>(
    values, "receiptId", repo::artifact_error::InvalidReceipt);
  receipt.operationId = requiredValue<std::string>(
    values, "operationId", repo::artifact_error::InvalidReceipt);
  receipt.repoNode = requiredValue<std::string>(
    values, "repoNode", repo::artifact_error::InvalidReceipt);
  receipt.artifact = artifactReferenceFromDict(
    requiredValue<py::dict>(values, "artifact", repo::artifact_error::InvalidReceipt),
    limits);
  receipt.committedAtMs = requiredValue<uint64_t>(
    values, "committedAtMs", repo::artifact_error::InvalidReceipt);
  receipt.storageGeneration = optionalValue<uint64_t>(
    values, "storageGeneration", 0, repo::artifact_error::InvalidReceipt);
  receipt.policyEpoch = requiredValue<std::string>(
    values, "policyEpoch", repo::artifact_error::InvalidReceipt);
  receipt.state = optionalValue<std::string>(
    values, "state", "COMMITTED", repo::artifact_error::InvalidReceipt);
  receipt.validate(limits);
  return receipt;
}

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

  py::register_exception<repo::ArtifactValidationError>(
    m, "ArtifactValidationError", PyExc_ValueError);

  py::class_<repo::ArtifactLimits>(m, "ArtifactLimits")
    .def(py::init<>())
    .def_readwrite("max_artifact_bytes", &repo::ArtifactLimits::maxArtifactBytes)
    .def_readwrite("max_chunk_bytes", &repo::ArtifactLimits::maxChunkBytes)
    .def_readwrite("max_root_encoded_bytes", &repo::ArtifactLimits::maxRootEncodedBytes)
    .def_readwrite("max_page_encoded_bytes", &repo::ArtifactLimits::maxPageEncodedBytes)
    .def_readwrite("max_page_entries", &repo::ArtifactLimits::maxPageEntries)
    .def_readwrite("max_manifest_depth", &repo::ArtifactLimits::maxManifestDepth)
    .def_readwrite("max_critical_extensions",
                   &repo::ArtifactLimits::maxCriticalExtensions)
    .def_readwrite("max_name_bytes", &repo::ArtifactLimits::maxNameBytes)
    .def_readwrite("max_packet_payload_bytes",
                   &repo::ArtifactLimits::maxPacketPayloadBytes)
    .def_readwrite("max_signature_bytes",
                   &repo::ArtifactLimits::maxSignatureBytes)
    .def_readwrite("max_manifest_pages",
                   &repo::ArtifactLimits::maxManifestPages)
    .def_readwrite("max_manifest_chunks",
                   &repo::ArtifactLimits::maxManifestChunks)
    .def_readwrite("max_cryptographic_operations",
                   &repo::ArtifactLimits::maxCryptographicOperations);

  py::class_<repo::ArtifactReference>(m, "ArtifactReference")
    .def(py::init([] (std::string logicalName,
                      std::string contentDigest,
                      uint64_t sizeBytes,
                      std::string rootManifestName,
                      std::string publisherIdentity,
                      std::string policyEpoch,
                      std::string digestAlgorithm,
                      std::string formatVersion) {
      repo::ArtifactReference reference;
      reference.logicalName = std::move(logicalName);
      reference.digestAlgorithm = std::move(digestAlgorithm);
      reference.contentDigest = std::move(contentDigest);
      reference.sizeBytes = sizeBytes;
      reference.formatVersion = std::move(formatVersion);
      reference.rootManifestName = std::move(rootManifestName);
      reference.publisherIdentity = std::move(publisherIdentity);
      reference.policyEpoch = std::move(policyEpoch);
      reference.validate();
      return reference;
    }),
    py::arg("logical_name"),
    py::arg("content_digest"),
    py::arg("size_bytes"),
    py::arg("root_manifest_name"),
    py::arg("publisher_identity"),
    py::arg("policy_epoch"),
    py::arg("digest_algorithm") = "sha256",
    py::arg("format_version") = "artifact-manifest-v2")
    .def_readonly("logical_name", &repo::ArtifactReference::logicalName)
    .def_readonly("digest_algorithm", &repo::ArtifactReference::digestAlgorithm)
    .def_readonly("content_digest", &repo::ArtifactReference::contentDigest)
    .def_readonly("size_bytes", &repo::ArtifactReference::sizeBytes)
    .def_readonly("format_version", &repo::ArtifactReference::formatVersion)
    .def_readonly("root_manifest_name", &repo::ArtifactReference::rootManifestName)
    .def_readonly("publisher_identity", &repo::ArtifactReference::publisherIdentity)
    .def_readonly("policy_epoch", &repo::ArtifactReference::policyEpoch)
    .def("same_bytes", &repo::ArtifactReference::sameBytes, py::arg("other"))
    .def("to_dict", [] (const repo::ArtifactReference& reference) {
      py::dict result;
      result["logicalName"] = reference.logicalName;
      result["digestAlgorithm"] = reference.digestAlgorithm;
      result["contentDigest"] = reference.contentDigest;
      result["sizeBytes"] = reference.sizeBytes;
      result["formatVersion"] = reference.formatVersion;
      result["rootManifestName"] = reference.rootManifestName;
      result["publisherIdentity"] = reference.publisherIdentity;
      result["policyEpoch"] = reference.policyEpoch;
      return result;
    });

  py::class_<repo::ArtifactCapability>(m, "ArtifactCapability")
    .def_readonly("repo_node", &repo::ArtifactCapability::repoNode)
    .def_readonly("format_versions", &repo::ArtifactCapability::formatVersions)
    .def_readonly("digest_algorithms", &repo::ArtifactCapability::digestAlgorithms)
    .def_readonly("signature_algorithms", &repo::ArtifactCapability::signatureAlgorithms)
    .def_readonly("max_artifact_bytes", &repo::ArtifactCapability::maxArtifactBytes)
    .def_readonly("max_chunk_bytes", &repo::ArtifactCapability::maxChunkBytes)
    .def_readonly("max_root_encoded_bytes",
                  &repo::ArtifactCapability::maxRootEncodedBytes)
    .def_readonly("max_page_encoded_bytes",
                  &repo::ArtifactCapability::maxPageEncodedBytes)
    .def_readonly("max_page_entries", &repo::ArtifactCapability::maxPageEntries)
    .def_readonly("max_manifest_depth", &repo::ArtifactCapability::maxManifestDepth)
    .def_readonly("supports_resume", &repo::ArtifactCapability::supportsResume)
    .def_readonly("supports_replica_receipts",
                  &repo::ArtifactCapability::supportsReplicaReceipts)
    .def_readonly("policy_epoch", &repo::ArtifactCapability::policyEpoch)
    .def("supports", &repo::ArtifactCapability::supports,
         py::arg("artifact"), py::arg("root_signature_algorithm"))
    .def("incompatibilities",
         [](const repo::ArtifactCapability& capability,
            const repo::ArtifactReference& artifact,
            const std::string& rootSignatureAlgorithm,
            uint64_t chunkBytes, uint64_t rootEncodedBytes,
            uint64_t pageEncodedBytes, uint32_t pageEntries,
            uint32_t manifestDepth, bool requireResume,
            bool requireReplicaReceipts) {
           repo::ArtifactCapabilityRequirements requirements;
           requirements.artifact = artifact;
           requirements.rootSignatureAlgorithm = rootSignatureAlgorithm;
           requirements.chunkBytes = chunkBytes;
           requirements.rootEncodedBytes = rootEncodedBytes;
           requirements.pageEncodedBytes = pageEncodedBytes;
           requirements.pageEntries = pageEntries;
           requirements.manifestDepth = manifestDepth;
           requirements.requireResume = requireResume;
           requirements.requireReplicaReceipts = requireReplicaReceipts;
           return capability.incompatibilities(requirements);
         },
         py::arg("artifact"), py::arg("root_signature_algorithm") = "ed25519",
         py::arg("chunk_bytes") = 1024 * 1024,
         py::arg("root_encoded_bytes") = 64 * 1024,
         py::arg("page_encoded_bytes") = 1024 * 1024,
         py::arg("page_entries") = 4096,
         py::arg("manifest_depth") = 8,
         py::arg("require_resume") = true,
         py::arg("require_replica_receipts") = true)
    .def("require_support",
         [](const repo::ArtifactCapability& capability,
            const repo::ArtifactReference& artifact,
            const std::string& rootSignatureAlgorithm,
            uint64_t chunkBytes, uint64_t rootEncodedBytes,
            uint64_t pageEncodedBytes, uint32_t pageEntries,
            uint32_t manifestDepth, bool requireResume,
            bool requireReplicaReceipts) {
           repo::ArtifactCapabilityRequirements requirements;
           requirements.artifact = artifact;
           requirements.rootSignatureAlgorithm = rootSignatureAlgorithm;
           requirements.chunkBytes = chunkBytes;
           requirements.rootEncodedBytes = rootEncodedBytes;
           requirements.pageEncodedBytes = pageEncodedBytes;
           requirements.pageEntries = pageEntries;
           requirements.manifestDepth = manifestDepth;
           requirements.requireResume = requireResume;
           requirements.requireReplicaReceipts = requireReplicaReceipts;
           capability.requireSupport(requirements);
         },
         py::arg("artifact"), py::arg("root_signature_algorithm") = "ed25519",
         py::arg("chunk_bytes") = 1024 * 1024,
         py::arg("root_encoded_bytes") = 64 * 1024,
         py::arg("page_encoded_bytes") = 1024 * 1024,
         py::arg("page_entries") = 4096,
         py::arg("manifest_depth") = 8,
         py::arg("require_resume") = true,
         py::arg("require_replica_receipts") = true);

  py::class_<repo::ArtifactManifestChild>(m, "ArtifactManifestChild")
    .def_readonly("kind", &repo::ArtifactManifestChild::kind)
    .def_readonly("index", &repo::ArtifactManifestChild::index)
    .def_readonly("offset_bytes", &repo::ArtifactManifestChild::offsetBytes)
    .def_readonly("length_bytes", &repo::ArtifactManifestChild::lengthBytes)
    .def_readonly("digest_algorithm",
                  &repo::ArtifactManifestChild::digestAlgorithm)
    .def_readonly("digest", &repo::ArtifactManifestChild::digest);

  py::class_<repo::ArtifactRootManifest>(m, "ArtifactRootManifest")
    .def_readonly("artifact", &repo::ArtifactRootManifest::artifact)
    .def_readonly("packet_payload_bytes",
                  &repo::ArtifactRootManifest::packetPayloadBytes)
    .def_readonly("chunk_bytes", &repo::ArtifactRootManifest::chunkBytes)
    .def_readonly("naming_template", &repo::ArtifactRootManifest::namingTemplate)
    .def_readonly("manifest_root_digest_algorithm",
                  &repo::ArtifactRootManifest::manifestRootDigestAlgorithm)
    .def_readonly("manifest_root_digest",
                  &repo::ArtifactRootManifest::manifestRootDigest)
    .def_readonly("signature_algorithm",
                  &repo::ArtifactRootManifest::signatureAlgorithm)
    .def_readonly("publisher_key_locator",
                  &repo::ArtifactRootManifest::publisherKeyLocator)
    .def_readonly("created_at_ms", &repo::ArtifactRootManifest::createdAtMs)
    .def_readonly("expires_at_ms", &repo::ArtifactRootManifest::expiresAtMs)
    .def_readonly("critical_extensions",
                  &repo::ArtifactRootManifest::criticalExtensions);

  py::class_<repo::ArtifactManifestPage>(m, "ArtifactManifestPage")
    .def_readonly("page_version", &repo::ArtifactManifestPage::pageVersion)
    .def_readonly("depth", &repo::ArtifactManifestPage::depth)
    .def_readonly("offset_bytes", &repo::ArtifactManifestPage::offsetBytes)
    .def_readonly("length_bytes", &repo::ArtifactManifestPage::lengthBytes)
    .def_readonly("page_digest_algorithm",
                  &repo::ArtifactManifestPage::pageDigestAlgorithm)
    .def_readonly("page_digest", &repo::ArtifactManifestPage::pageDigest)
    .def_readonly("children", &repo::ArtifactManifestPage::children);

  py::class_<repo::ArtifactChunk>(m, "ArtifactChunk")
    .def_readonly("index", &repo::ArtifactChunk::index)
    .def_readonly("offset_bytes", &repo::ArtifactChunk::offsetBytes)
    .def_readonly("length_bytes", &repo::ArtifactChunk::lengthBytes)
    .def_readonly("digest_algorithm", &repo::ArtifactChunk::digestAlgorithm)
    .def_readonly("digest", &repo::ArtifactChunk::digest)
    .def_readonly("first_segment", &repo::ArtifactChunk::firstSegment)
    .def_readonly("final_segment", &repo::ArtifactChunk::finalSegment);

  py::class_<repo::SignedArtifactRoot>(m, "SignedArtifactRoot")
    .def(py::init<>())
    .def_readwrite("root", &repo::SignedArtifactRoot::root)
    .def_property(
      "signature_value",
      [] (const repo::SignedArtifactRoot& value) {
        return vectorToBytes(value.signatureValue);
      },
      [] (repo::SignedArtifactRoot& value, const py::bytes& signature) {
        value.signatureValue = bytesToVector(signature);
      });

  py::class_<repo::ArtifactManifestTrustPolicy>(
      m, "ArtifactManifestTrustPolicy")
    .def(py::init<>())
    .def_readwrite("trusted_publisher_identity",
                   &repo::ArtifactManifestTrustPolicy::trustedPublisherIdentity)
    .def_readwrite("trusted_key_locator",
                   &repo::ArtifactManifestTrustPolicy::trustedKeyLocator)
    .def_readwrite("public_key_pem",
                   &repo::ArtifactManifestTrustPolicy::publicKeyPem)
    .def_readwrite("policy_epoch",
                   &repo::ArtifactManifestTrustPolicy::policyEpoch)
    .def_readwrite("evaluation_time_ms",
                   &repo::ArtifactManifestTrustPolicy::evaluationTimeMs)
    .def_readwrite("allowed_digest_algorithms",
                   &repo::ArtifactManifestTrustPolicy::allowedDigestAlgorithms)
    .def_readwrite("allowed_signature_algorithms",
                   &repo::ArtifactManifestTrustPolicy::allowedSignatureAlgorithms)
    .def_readwrite("supported_critical_extensions",
                   &repo::ArtifactManifestTrustPolicy::supportedCriticalExtensions)
    .def_readwrite("revoked_key_locators",
                   &repo::ArtifactManifestTrustPolicy::revokedKeyLocators)
    .def("validate", &repo::ArtifactManifestTrustPolicy::validate,
         py::arg("limits") = repo::ArtifactLimits{});

  py::class_<repo::ArtifactManifestVerificationResult>(
      m, "ArtifactManifestVerificationResult")
    .def_readonly("artifact",
                  &repo::ArtifactManifestVerificationResult::artifact)
    .def_readonly("verified_page_count",
                  &repo::ArtifactManifestVerificationResult::verifiedPageCount)
    .def_readonly("verified_chunk_count",
                  &repo::ArtifactManifestVerificationResult::verifiedChunkCount)
    .def_readonly("asymmetric_verification_count",
                  &repo::ArtifactManifestVerificationResult::
                    asymmetricVerificationCount)
    .def_readonly("digest_verification_count",
                  &repo::ArtifactManifestVerificationResult::
                    digestVerificationCount)
    .def_readonly("derived_page_names",
                  &repo::ArtifactManifestVerificationResult::derivedPageNames);

  py::class_<repo::ArtifactUploadLease>(m, "ArtifactUploadLease")
    .def_readonly("lease_id", &repo::ArtifactUploadLease::leaseId)
    .def_readonly("operation_id", &repo::ArtifactUploadLease::operationId)
    .def_readonly("repo_node", &repo::ArtifactUploadLease::repoNode)
    .def_readonly("artifact", &repo::ArtifactUploadLease::artifact)
    .def_readonly("reserved_bytes", &repo::ArtifactUploadLease::reservedBytes)
    .def_readonly("issued_at_ms", &repo::ArtifactUploadLease::issuedAtMs)
    .def_readonly("expires_at_ms", &repo::ArtifactUploadLease::expiresAtMs)
    .def_readonly("replay_id", &repo::ArtifactUploadLease::replayId);

  py::class_<repo::ArtifactReplicaReceipt>(m, "ArtifactReplicaReceipt")
    .def_readonly("receipt_id", &repo::ArtifactReplicaReceipt::receiptId)
    .def_readonly("operation_id", &repo::ArtifactReplicaReceipt::operationId)
    .def_readonly("repo_node", &repo::ArtifactReplicaReceipt::repoNode)
    .def_readonly("artifact", &repo::ArtifactReplicaReceipt::artifact)
    .def_readonly("committed_at_ms", &repo::ArtifactReplicaReceipt::committedAtMs)
    .def_readonly("storage_generation",
                  &repo::ArtifactReplicaReceipt::storageGeneration)
    .def_readonly("policy_epoch", &repo::ArtifactReplicaReceipt::policyEpoch)
    .def_readonly("state", &repo::ArtifactReplicaReceipt::state);

  m.def("artifact_reference_from_dict", &artifactReferenceFromDict,
        py::arg("values"), py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_capability_from_dict", &artifactCapabilityFromDict,
        py::arg("values"), py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_manifest_child_from_dict", &artifactManifestChildFromDict,
        py::arg("values"), py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_root_manifest_from_dict", &artifactRootManifestFromDict,
        py::arg("values"), py::arg("encoded_bytes"),
        py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_manifest_page_from_dict", &artifactManifestPageFromDict,
        py::arg("values"), py::arg("encoded_bytes"),
        py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_chunk_from_dict", &artifactChunkFromDict,
        py::arg("values"), py::arg("artifact"),
        py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_upload_lease_from_dict", &artifactUploadLeaseFromDict,
        py::arg("values"), py::arg("now_ms"),
        py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_resume_identity_from_dict", &artifactResumeIdentityFromDict,
        py::arg("values"), py::arg("limits") = repo::ArtifactLimits{});
  m.def("artifact_replica_receipt_from_dict", &artifactReplicaReceiptFromDict,
        py::arg("values"), py::arg("limits") = repo::ArtifactLimits{});

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

  py::class_<repo::RepoOperationMetrics>(m, "RepoOperationMetrics")
    .def(py::init<>())
    .def_readwrite("operation_id", &repo::RepoOperationMetrics::operationId)
    .def_readwrite("started_at_ms", &repo::RepoOperationMetrics::startedAtMs)
    .def_readwrite("completed_at_ms", &repo::RepoOperationMetrics::completedAtMs)
    .def_readwrite("phase_timings_ms", &repo::RepoOperationMetrics::phaseTimingsMs)
    .def_readwrite("logical_payload_bytes", &repo::RepoOperationMetrics::logicalPayloadBytes)
    .def_readwrite("data_wire_bytes", &repo::RepoOperationMetrics::dataWireBytes)
    .def_readwrite("interest_wire_bytes", &repo::RepoOperationMetrics::interestWireBytes)
    .def_readwrite("wire_bytes", &repo::RepoOperationMetrics::wireBytes)
    .def_readwrite("retransmitted_bytes", &repo::RepoOperationMetrics::retransmittedBytes)
    .def_readwrite("payload_store_bytes_read",
                   &repo::RepoOperationMetrics::payloadStoreBytesRead)
    .def_readwrite("payload_store_bytes_written",
                   &repo::RepoOperationMetrics::payloadStoreBytesWritten)
    .def_readwrite("metadata_store_bytes_read",
                   &repo::RepoOperationMetrics::metadataStoreBytesRead)
    .def_readwrite("metadata_store_bytes_written",
                   &repo::RepoOperationMetrics::metadataStoreBytesWritten)
    .def_readwrite("storage_bytes_read", &repo::RepoOperationMetrics::storageBytesRead)
    .def_readwrite("storage_bytes_written", &repo::RepoOperationMetrics::storageBytesWritten)
    .def_readwrite("asymmetric_verifications",
                   &repo::RepoOperationMetrics::asymmetricVerifications)
    .def_readwrite("digest_verifications", &repo::RepoOperationMetrics::digestVerifications)
    .def_readwrite("asymmetric_verification_ms",
                   &repo::RepoOperationMetrics::asymmetricVerificationMs)
    .def_readwrite("digest_verification_ms",
                   &repo::RepoOperationMetrics::digestVerificationMs)
    .def_readwrite("control_operations", &repo::RepoOperationMetrics::controlOperations)
    .def_readwrite("metadata_operations", &repo::RepoOperationMetrics::metadataOperations)
    .def_readwrite("metadata_record_count", &repo::RepoOperationMetrics::metadataRecordCount)
    .def_readwrite("requested_replica_count",
                   &repo::RepoOperationMetrics::requestedReplicaCount)
    .def_readwrite("selected_replica_count",
                   &repo::RepoOperationMetrics::selectedReplicaCount)
    .def_readwrite("committed_replica_count",
                   &repo::RepoOperationMetrics::committedReplicaCount)
    .def_readwrite("rejected_replica_receipt_count",
                   &repo::RepoOperationMetrics::rejectedReplicaReceiptCount)
    .def("validate", &repo::RepoOperationMetrics::validate)
    .def("to_json", &repo::RepoOperationMetrics::toJson);

  py::enum_<repo::ArtifactSegmentDisposition>(m, "ArtifactSegmentDisposition")
    .value("ACCEPTED", repo::ArtifactSegmentDisposition::Accepted)
    .value("DUPLICATE", repo::ArtifactSegmentDisposition::Duplicate)
    .value("UNSOLICITED", repo::ArtifactSegmentDisposition::Unsolicited);

  py::class_<repo::AdaptiveTransferOptions>(m, "AdaptiveTransferOptions")
    .def(py::init<>())
    .def_readwrite("initial_window", &repo::AdaptiveTransferOptions::initialWindow)
    .def_readwrite("minimum_window", &repo::AdaptiveTransferOptions::minimumWindow)
    .def_readwrite("maximum_window", &repo::AdaptiveTransferOptions::maximumWindow)
    .def_readwrite("verification_backlog_limit",
                   &repo::AdaptiveTransferOptions::verificationBacklogLimit)
    .def_readwrite("maximum_retries", &repo::AdaptiveTransferOptions::maximumRetries)
    .def_readwrite("segment_timeout_ms",
                   &repo::AdaptiveTransferOptions::segmentTimeoutMs)
    .def("validate", &repo::AdaptiveTransferOptions::validate);

  py::class_<repo::ArtifactSegmentRequest>(m, "ArtifactSegmentRequest")
    .def_readonly("segment_no", &repo::ArtifactSegmentRequest::segmentNo)
    .def_readonly("attempt", &repo::ArtifactSegmentRequest::attempt)
    .def_readonly("retransmission", &repo::ArtifactSegmentRequest::retransmission);

  py::class_<repo::ArtifactTransferSnapshot>(m, "ArtifactTransferSnapshot")
    .def_readonly("total_segments", &repo::ArtifactTransferSnapshot::totalSegments)
    .def_readonly("verified_segments", &repo::ArtifactTransferSnapshot::verifiedSegments)
    .def_readonly("in_flight_segments", &repo::ArtifactTransferSnapshot::inFlightSegments)
    .def_readonly("verification_backlog",
                  &repo::ArtifactTransferSnapshot::verificationBacklog)
    .def_readonly("logical_bytes", &repo::ArtifactTransferSnapshot::logicalBytes)
    .def_readonly("wire_bytes", &repo::ArtifactTransferSnapshot::wireBytes)
    .def_readonly("retransmitted_bytes",
                  &repo::ArtifactTransferSnapshot::retransmittedBytes)
    .def_readonly("interest_count", &repo::ArtifactTransferSnapshot::interestCount)
    .def_readonly("retransmission_count",
                  &repo::ArtifactTransferSnapshot::retransmissionCount)
    .def_readonly("duplicate_count", &repo::ArtifactTransferSnapshot::duplicateCount)
    .def_readonly("timeout_count", &repo::ArtifactTransferSnapshot::timeoutCount)
    .def_readonly("rejected_count", &repo::ArtifactTransferSnapshot::rejectedCount)
    .def_readonly("congestion_window",
                  &repo::ArtifactTransferSnapshot::congestionWindow)
    .def_readonly("complete", &repo::ArtifactTransferSnapshot::complete)
    .def_readonly("failed", &repo::ArtifactTransferSnapshot::failed)
    .def_readonly("failure_reason", &repo::ArtifactTransferSnapshot::failureReason);

  py::class_<repo::AdaptiveArtifactTransfer>(m, "AdaptiveArtifactTransfer")
    .def(py::init<uint64_t, repo::AdaptiveTransferOptions>(),
         py::arg("total_segments"),
         py::arg("options") = repo::AdaptiveTransferOptions{})
    .def("poll", &repo::AdaptiveArtifactTransfer::poll,
         py::arg("now_ms"),
         py::arg("maximum_requests") = std::numeric_limits<size_t>::max())
    .def("receive", &repo::AdaptiveArtifactTransfer::receive,
         py::arg("segment_no"), py::arg("logical_bytes"),
         py::arg("wire_bytes"), py::arg("now_ms"))
    .def("mark_verified", &repo::AdaptiveArtifactTransfer::markVerified)
    .def("reject", &repo::AdaptiveArtifactTransfer::reject)
    .def("expire", &repo::AdaptiveArtifactTransfer::expire)
    .def("fail", &repo::AdaptiveArtifactTransfer::fail)
    .def("snapshot", &repo::AdaptiveArtifactTransfer::snapshot)
    .def("missing_segments", &repo::AdaptiveArtifactTransfer::missingSegments);

  py::enum_<repo::ArtifactResumeState>(m, "ArtifactResumeState")
    .value("OPEN", repo::ArtifactResumeState::Open)
    .value("CANCELLED", repo::ArtifactResumeState::Cancelled)
    .value("EXPIRED", repo::ArtifactResumeState::Expired)
    .value("COMPLETED", repo::ArtifactResumeState::Completed)
    .value("FAILED", repo::ArtifactResumeState::Failed);

  py::class_<repo::ArtifactResumeIdentity>(m, "ArtifactResumeIdentity")
    .def_readonly("artifact", &repo::ArtifactResumeIdentity::artifact)
    .def_readonly("manifest_root_digest",
                  &repo::ArtifactResumeIdentity::manifestRootDigest)
    .def_readonly("packet_payload_bytes",
                  &repo::ArtifactResumeIdentity::packetPayloadBytes)
    .def_readonly("chunk_bytes", &repo::ArtifactResumeIdentity::chunkBytes)
    .def("validate", &repo::ArtifactResumeIdentity::validate,
         py::arg("limits") = repo::ArtifactLimits{});

  py::class_<repo::ArtifactResumeSnapshot>(m, "ArtifactResumeSnapshot")
    .def_readonly("state", &repo::ArtifactResumeSnapshot::state)
    .def_readonly("operation_id", &repo::ArtifactResumeSnapshot::operationId)
    .def_readonly("lease_id", &repo::ArtifactResumeSnapshot::leaseId)
    .def_readonly("expires_at_ms", &repo::ArtifactResumeSnapshot::expiresAtMs)
    .def_readonly("total_chunks", &repo::ArtifactResumeSnapshot::totalChunks)
    .def_readonly("verified_chunks",
                  &repo::ArtifactResumeSnapshot::verifiedChunks)
    .def_readonly("newly_verified_bytes",
                  &repo::ArtifactResumeSnapshot::newlyVerifiedBytes)
    .def_readonly("avoided_retransmission_bytes",
                  &repo::ArtifactResumeSnapshot::avoidedRetransmissionBytes)
    .def_readonly("preserves_progress",
                  &repo::ArtifactResumeSnapshot::preservesProgress);

  py::class_<repo::ArtifactResumeSession>(m, "ArtifactResumeSession")
    .def(py::init<repo::ArtifactResumeIdentity, repo::ArtifactUploadLease,
                  std::vector<repo::ArtifactChunk>, uint64_t>(),
         py::arg("identity"), py::arg("lease"), py::arg("chunks"),
         py::arg("now_ms"))
    .def("restore_verified", &repo::ArtifactResumeSession::restoreVerified)
    .def("mark_verified", &repo::ArtifactResumeSession::markVerified,
         py::arg("chunk_index"), py::arg("now_ms"))
    .def("missing_chunks", &repo::ArtifactResumeSession::missingChunks)
    .def("renew_lease", &repo::ArtifactResumeSession::renewLease,
         py::arg("lease"), py::arg("now_ms"))
    .def("resume", &repo::ArtifactResumeSession::resume,
         py::arg("identity"), py::arg("lease"), py::arg("now_ms"))
    .def("cancel", &repo::ArtifactResumeSession::cancel,
         py::arg("preserve_progress"))
    .def("expire", &repo::ArtifactResumeSession::expire)
    .def("complete", &repo::ArtifactResumeSession::complete)
    .def("fail", &repo::ArtifactResumeSession::fail)
    .def("snapshot", &repo::ArtifactResumeSession::snapshot)
    .def_property_readonly(
      "identity",
      [] (const repo::ArtifactResumeSession& value) {
        return value.identity();
      });

  py::enum_<repo::ReplicaLeaseControlState>(m, "ReplicaLeaseControlState")
    .value("IDLE", repo::ReplicaLeaseControlState::Idle)
    .value("COLLABORATION_OPEN", repo::ReplicaLeaseControlState::CollaborationOpen)
    .value("ACK_CLOSED", repo::ReplicaLeaseControlState::AckClosed)
    .value("PLAN_COMMITTED", repo::ReplicaLeaseControlState::PlanCommitted)
    .value("FAILED", repo::ReplicaLeaseControlState::Failed);

  py::class_<repo::ReplicaLeaseControlSnapshot>(m, "ReplicaLeaseControlSnapshot")
    .def_readonly("state", &repo::ReplicaLeaseControlSnapshot::state)
    .def_readonly("request_id", &repo::ReplicaLeaseControlSnapshot::requestId)
    .def_readonly("candidate_count",
                  &repo::ReplicaLeaseControlSnapshot::candidateCount)
    .def_readonly("selected_replica_count",
                  &repo::ReplicaLeaseControlSnapshot::selectedReplicaCount)
    .def_readonly("control_operation_count",
                  &repo::ReplicaLeaseControlSnapshot::controlOperationCount)
    .def_readonly("leases", &repo::ReplicaLeaseControlSnapshot::leases);

  py::class_<repo::ReplicaLeaseControlFlow>(m, "ReplicaLeaseControlFlow")
    .def(py::init<>())
    .def("begin_collaboration",
         &repo::ReplicaLeaseControlFlow::beginCollaboration)
    .def("close_acks", &repo::ReplicaLeaseControlFlow::closeAcks)
    .def("commit_plan", &repo::ReplicaLeaseControlFlow::commitPlan,
         py::arg("selected_leases"), py::arg("now_ms"))
    .def("fail", &repo::ReplicaLeaseControlFlow::fail)
    .def("snapshot", &repo::ReplicaLeaseControlFlow::snapshot);

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

  m.def("artifact_sha256_hex",
        [] (const py::bytes& payload) {
          return repo::artifactSha256Hex(bytesToVector(payload));
        },
        py::arg("payload"));

  m.def("canonical_root_manifest_bytes",
        [] (const repo::ArtifactRootManifest& root,
            const repo::ArtifactLimits& limits) {
          return vectorToBytes(repo::canonicalRootManifestBytes(root, limits));
        },
        py::arg("root"), py::arg("limits") = repo::ArtifactLimits{});

  m.def("encode_signed_artifact_root",
        [] (const repo::SignedArtifactRoot& root,
            const repo::ArtifactLimits& limits) {
          return vectorToBytes(repo::encodeSignedArtifactRoot(root, limits));
        },
        py::arg("signed_root"), py::arg("limits") = repo::ArtifactLimits{});

  m.def("decode_signed_artifact_root",
        [] (const py::bytes& wire, const repo::ArtifactLimits& limits) {
          return repo::decodeSignedArtifactRoot(bytesToVector(wire), limits);
        },
        py::arg("wire"), py::arg("limits") = repo::ArtifactLimits{});

  m.def("canonical_manifest_page_bytes",
        [] (const repo::ArtifactManifestPage& page,
            const repo::ArtifactLimits& limits) {
          return vectorToBytes(repo::canonicalManifestPageBytes(page, limits));
        },
        py::arg("page"), py::arg("limits") = repo::ArtifactLimits{});

  m.def("encode_artifact_manifest_page",
        [] (const repo::ArtifactManifestPage& page,
            const repo::ArtifactLimits& limits) {
          return vectorToBytes(repo::encodeArtifactManifestPage(page, limits));
        },
        py::arg("page"), py::arg("limits") = repo::ArtifactLimits{});

  m.def("decode_artifact_manifest_page",
        [] (const py::bytes& wire, const repo::ArtifactLimits& limits) {
          return repo::decodeArtifactManifestPage(bytesToVector(wire), limits);
        },
        py::arg("wire"), py::arg("limits") = repo::ArtifactLimits{});

  m.def("derive_manifest_page_name", &repo::deriveManifestPageName,
        py::arg("root"), py::arg("page_digest"));
  m.def("derive_artifact_data_name", &repo::deriveArtifactDataName,
        py::arg("root"), py::arg("chunk_index"), py::arg("segment"));
  m.def("verify_signed_artifact_root", &repo::verifySignedArtifactRoot,
        py::arg("signed_root"), py::arg("expected_artifact"),
        py::arg("capability"), py::arg("policy"),
        py::arg("limits") = repo::ArtifactLimits{});
  m.def("verify_artifact_manifest_graph", &repo::verifyArtifactManifestGraph,
        py::arg("signed_root"), py::arg("expected_artifact"),
        py::arg("pages"), py::arg("chunks"), py::arg("capability"),
        py::arg("policy"), py::arg("limits") = repo::ArtifactLimits{});
  m.def("verify_artifact_chunk_payload",
        [] (const repo::ArtifactChunk& chunk, const py::bytes& payload) {
          repo::verifyArtifactChunkPayload(chunk, bytesToVector(payload));
        },
        py::arg("chunk"), py::arg("payload"));
  m.def("verify_artifact_payload",
        [] (const repo::ArtifactReference& artifact, const py::bytes& payload) {
          repo::verifyArtifactPayload(artifact, bytesToVector(payload));
        },
        py::arg("artifact"), py::arg("payload"));
  m.def("validate_artifact_resume_identity",
        &repo::validateArtifactResumeIdentity,
        py::arg("expected_artifact"), py::arg("expected_root"),
        py::arg("resumed_artifact"), py::arg("resumed_root"));

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
