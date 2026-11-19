#!/usr/bin/env python3
"""P06 actual native-facade compatibility rejection peer."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from ndnsf import ServiceUser
import py_repoclient as repo
from py_repoclient import orchestration, persistence


ROOT_TYPES = """ArtifactApiBackend ArtifactCancellationToken ArtifactCapabilityNegotiation
ArtifactCapabilityRejection ArtifactCapacityStatus ArtifactControlMetrics ArtifactDescriptor
ArtifactErrorCode ArtifactFetchDriver ArtifactFetchSession ArtifactFinalizationRecord
ArtifactProgress ArtifactPublishDriver ArtifactReplicaSession ArtifactSessionStatus
ArtifactStorageIdentity ArtifactStoreAssignment ArtifactStoreOffer ArtifactTransferSessionRecord
ArtifactUploadSession AtomicArtifactDestination AuthenticatedReplicaReceipt
CollaborationArtifactApiBackend FilesystemArtifactApiBackend FilesystemCasPayloadStore
HmacReceiptAuthenticator MetadataStore NetworkDistributedRepoClient PayloadStore
PendingReplicaLeaseCollaboration PendingReplicaTaskCollaboration ReplicaLeaseCollaborationClient
ReplicaTaskCollaborationClient ReplicaTaskControlSnapshot RepoDataPlaneProducer RepoLifecycleEvent
RepoNodeApp SqliteRepositoryPersistence artifact_capability_from_ack capability_from_ack
discovery_record_from_ack ready_capability_from_ack""".split()
ROOT_FUNCTIONS = """negotiate_artifact_capabilities resolve_active_artifact
retrieve_to_atomic_destination decode_store_assignment decode_store_offer_ack
decode_upload_lease_assignment encode_store_offer_ack encode_upload_lease_ack
install_artifact_collaboration_service""".split()
ORCHESTRATION_CALLS = """DistributedRepo ExactPacketRepositoryApi LocalDistributedRepo
NetworkDistributedRepoClient PlacementPolicy RepoIncompleteWriteError RepoNodeApp
RepoObjectManifest RepoOperationMetrics RepoPlacement RepoRepairAction RepoWriteIntent
RepoWriteReceipt StorageCapability configure_repo_object_class_policies decode_repo_request
encode_repo_request large_data_reference_from_repo_manifest normalize_repo_mode
normalize_repo_operation_state normalize_write_consistency repo_artifact_reference
repo_manifest_from_artifact_reference repo_manifest_from_large_data_reference
repo_object_class_policy required_write_acks select_replicas validate_write_receipts""".split()
ORCHESTRATION_CONSTANTS = """CATALOG_MERGE_MAX_PULL_BYTES REPO_METRIC_PHASES
REPO_OPERATION_ID_MAX_BYTES REPO_OPERATION_STATES REPO_REASON_GENERATION_CONFLICT
REPO_REASON_INTEGRITY_FAILURE REPO_REASON_OPERATION_CONFLICT REPO_REASON_OVERLOADED
REPO_REASON_REPAIR_UNAVAILABLE REPO_REASON_WRITE_INCOMPLETE REPO_REJECTION_REASONS""".split()
PERSISTENCE_CONSTANTS = """ARTIFACT_LIFECYCLE_STATES ARTIFACT_LIFECYCLE_TRANSITIONS
REPO_PERSISTENCE_OWNED REPO_PERSISTENCE_CLOSED REPO_LIFECYCLE_INVALID_STATE
REPO_LIFECYCLE_STATE_CONFLICT REPO_LIFECYCLE_IDENTITY_CONFLICT REPO_LIFECYCLE_EVENT_CONFLICT
REPO_LIFECYCLE_ILLEGAL_TRANSITION REPO_ARTIFACT_INVALID_IDENTITY
REPO_ARTIFACT_RANGE_OUT_OF_BOUNDS REPO_ARTIFACT_INCOMPLETE REPO_ARTIFACT_DIGEST_MISMATCH
REPO_ARTIFACT_WRITES_DISABLED""".split()
FACADE_ATTRIBUTES = """artifact_capabilities capability delete fetch fetch_file_async
fetch_object get get_object insert inventory list lookup_local_commit publisher_namespace
publisher_object_name publish_file_async remove store _require_publisher_object_name _service_for
begin_upload begin_fetch""".split()
API_METHODS = """configure_artifact_backend begin_upload begin_fetch publish_file_async fetch_file_async""".split()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ("group", "controller", "user", "trust-schema", "service-prefix", "source", "forbidden-root", "report-out"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    source = Path(args.source)
    forbidden = Path(args.forbidden_root)
    if forbidden.exists():
        raise RuntimeError("P06 forbidden root already exists")
    source_digest = sha256(source)
    user = ServiceUser(group=args.group, controller=args.controller, user=args.user,
                       trust_schema=args.trust_schema, permission_wait_ms=20000)
    user.start()
    client = api = None
    records = []
    try:
        client = repo.RepoClient(user=user, repo_service_name=args.service_prefix)
        api = client.artifact_api
        if not isinstance(api._native, repo._native.NativeArtifactRepositoryApi):
            raise RuntimeError("P06 facade is not backed by native C++ API")
        def rejected(label, operation, call):
            try:
                call()
            except repo.RepoCompatibilityError as error:
                if (error.code != "SPEC192_COMPATIBILITY_V1" or error.operation != operation or
                        not error.replacement or not str(error).startswith(error.code + ": " + operation)):
                    raise RuntimeError("P06 compatibility error contract mismatch: " + label) from error
                records.append({"case": label, "operation": error.operation, "code": error.code,
                                "replacement": error.replacement})
            else:
                raise RuntimeError("retired interface unexpectedly returned: " + label)
        for name in ROOT_TYPES + ROOT_FUNCTIONS:
            rejected("root." + name, name, lambda name=name: getattr(repo, name)(str(forbidden)))
        for name in ORCHESTRATION_CALLS:
            rejected("orchestration." + name, name, lambda name=name: getattr(orchestration, name)(str(forbidden)))
        for module, names in ((orchestration, ORCHESTRATION_CONSTANTS), (persistence, PERSISTENCE_CONSTANTS)):
            for name in names:
                rejected(module.__name__ + "." + name, name, lambda name=name, module=module: getattr(module, name))
        for name in FACADE_ATTRIBUTES:
            rejected("facade." + name, name, lambda name=name: getattr(client, name)())
        rejected("facade.configure", "configure_artifact_backend", lambda: client.configure_artifact_backend(object()))
        rejected("facade.manifest", "make_manifest", lambda: client.make_manifest())
        rejected("facade.backend", "artifact_backend", lambda: repo.RepoClient(user=user, artifact_backend=object()))
        rejected("facade.publish.reference", "publish_file legacy options", lambda: client.publish_file(source))
        rejected("facade.publish.options", "publish_file legacy options", lambda: client.publish_file(source, legacy=True))
        rejected("facade.fetch.reference", "fetch_file by reference", lambda: client.fetch_file("/legacy/object", forbidden / "fetch.bin"))
        rejected("facade.fetch.options", "fetch_file legacy options", lambda: client.fetch_file(None, forbidden / "fetch.bin", legacy=True))
        for name in API_METHODS:
            rejected("api." + name, name, lambda name=name: getattr(api, name)())
        rejected("api.backend", "ArtifactRepositoryApi(backend=...)", lambda: repo.ArtifactRepositoryApi(object()))
        rejected("api.publish.reference", "publish_file legacy options", lambda: api.publish_file(source))
        rejected("api.publish.options", "publish_file legacy options", lambda: api.publish_file(source, legacy=True))
        rejected("api.fetch.options", "fetch_file legacy options", lambda: api.fetch_file(None, forbidden / "fetch.bin", legacy=True))
        if len(records) != 141 or len({record["case"] for record in records}) != 141:
            raise RuntimeError("P06 frozen interface inventory is incomplete")
        if forbidden.exists() or sha256(source) != source_digest:
            raise RuntimeError("P06 retired call changed local storage")
        modules = {}
        for name, module in list(sys.modules.items()):
            path = getattr(module, "__file__", None)
            if path and path.endswith(".so") and ("ndnsf" in name or "repoclient" in name):
                modules[name] = {"path": str(Path(path).resolve()), "sha256": sha256(path)}
        if not any("repoclient" in name for name in modules) or not any("ndnsf" in name for name in modules):
            raise RuntimeError("P06 native modules not loaded")
        libraries = {}
        for line in Path("/proc/self/maps").read_text().splitlines():
            fields = line.split(None, 5)
            if len(fields) == 6 and any(token in fields[5] for token in ("libndn", "_ndnsf", "_py_repoclient")):
                path = Path(fields[5])
                if path.is_file():
                    libraries[str(path.resolve())] = sha256(path)
        Path(args.report_out).write_text(json.dumps({"status": "PASS", "cases": records,
            "source_sha256": source_digest, "native_type": type(api._native).__module__ + "." + type(api._native).__name__,
            "native_modules": modules, "loaded_libraries": libraries}, sort_keys=True, indent=2) + "\n")
        print("SPEC193_RETIRED_PYTHON_PASS count=141", flush=True)
        return 0
    finally:
        api = client = None
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
