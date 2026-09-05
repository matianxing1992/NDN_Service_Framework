"""Real YOLO26n model-neutral adapter and candidate splitter."""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
from typing import Any, Callable, Mapping

from ..base import (
    AdapterPortDescriptor, InferenceStateClass, InferenceStateContract,
    InferenceTaskDescriptor, JsonTaskAdapter, ModelFamilyAdapter,
)
from ..builtin import GuardedRunnerAdapter, StaticStateAdapter
from ...splitter import (
    AdapterDescriptor, GraphNodeView, ModelDescriptor, ModelGraphSnapshot,
    RoleDependency, RoleExecutionPlan, RoleResourceRequirement, SplitCandidate,
    SplitSource, SplitterDescriptor, TensorContract, TensorEdgeView,
    canonical_contract_digest,
)
from .candidates import (
    RegisteredYoloCandidate, verify_catalogue, verify_catalogue_signature,
)
from .graph import Yolo26GraphAdapter, load_yolo_graph


def _digest(value: Any) -> str:
    wire = json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")
    return "sha256:" + hashlib.sha256(wire).hexdigest()


def _model_descriptor() -> AdapterDescriptor:
    digest = lambda value: _digest({"spec180_yolo": value})
    return AdapterDescriptor(
        name="yolo26n-onnx", version="1.0.0", state_digest=digest("state"),
        abi="python-v1", model_formats=("onnx",),
        tasks=("object-detection",), backends=("onnxruntime-cpu", "onnxruntime-cuda"),
        precisions=("float32",), input_schema_digest=digest("input"),
        options_schema_digest=digest("options"), result_schema_digest=digest("result"),
        graph_schema_digest=digest("graph"), split_schema_digest=digest("split"),
        state_schema_digest=digest("state-schema"), graph_inspectable=True,
        splittable=True,
    )


@dataclass(frozen=True)
class Yolo26Splitter:
    descriptor: AdapterPortDescriptor
    splitter_descriptor: SplitterDescriptor
    registered: tuple[RegisteredYoloCandidate, ...]
    graph_node_names: tuple[str, ...] = ()
    graph_metadata: Mapping[str, Any] = field(default_factory=dict)
    postprocessing: Mapping[str, Any] = field(default_factory=dict)

    def _role_assignment(self, graph: ModelGraphSnapshot, candidate: RegisteredYoloCandidate) -> dict[str, str]:
        nodes = tuple(graph.topological_order)
        roles = tuple(str(item["role"]) for item in candidate.roles)
        if candidate.candidate_id == "atomic-v1":
            return {node: roles[0] for node in nodes}
        partition = candidate.semantic_partition
        role_sets = partition.get("roleNodeSets")
        if not isinstance(role_sets, Mapping) or set(role_sets) != set(roles):
            raise ValueError("YOLO semantic role partition is incomplete")
        if len(self.graph_node_names) != len(nodes):
            raise ValueError("YOLO graph node names are not bound to the graph")
        name_by_node = dict(zip(nodes, self.graph_node_names))
        graph_names = set(name_by_node.values())
        owner_by_name: dict[str, str] = {}
        for role in roles:
            names = role_sets.get(role)
            if (not isinstance(names, list) or not names
                    or any(not isinstance(name, str) or not name for name in names)):
                raise ValueError("YOLO semantic role node set is invalid")
            for name in names:
                if name in owner_by_name:
                    raise ValueError("YOLO semantic role assigns a node twice")
                owner_by_name[name] = role
        if set(owner_by_name) != graph_names:
            raise ValueError("YOLO semantic role partition does not cover graph")
        node_roles = {node: owner_by_name[name] for node, name in name_by_node.items()}

        graph_edges = {edge.edge_id: edge for edge in graph.edges}
        actual_interfaces: dict[str, tuple[str, str, set[str], set[str]]] = {}
        actual_dependencies: dict[tuple[str, str], set[str]] = {}
        for edge in graph.edges:
            producer_role = node_roles[edge.producer]
            crossing_consumers = {
                consumer for consumer in edge.consumers
                if node_roles[consumer] != producer_role
            }
            if not crossing_consumers:
                continue
            consumer_roles = {node_roles[item] for item in crossing_consumers}
            actual_interfaces[edge.edge_id] = (
                name_by_node[edge.producer], producer_role,
                {name_by_node[item] for item in crossing_consumers}, consumer_roles)
            for consumer_role in consumer_roles:
                actual_dependencies.setdefault((producer_role, consumer_role), set()).add(
                    edge.edge_id)
        interfaces = partition.get("tensorInterfaces")
        if not isinstance(interfaces, list):
            raise ValueError("YOLO semantic tensor interfaces are missing")
        declared_interfaces = {str(item.get("edgeId", "")): item
                               for item in interfaces if isinstance(item, Mapping)}
        if set(declared_interfaces) != set(actual_interfaces):
            raise ValueError("YOLO semantic tensor interface coverage differs from graph")
        for edge_id, actual in actual_interfaces.items():
            item = declared_interfaces[edge_id]
            if (item.get("producerNode") != actual[0]
                    or item.get("producerRole") != actual[1]
                    or set(item.get("consumerNodes", ())) != actual[2]
                    or set(item.get("consumerRoles", ())) != actual[3]):
                raise ValueError("YOLO semantic tensor interface does not match graph")
            edge = graph_edges[edge_id]
            declared_shape = tuple(item.get("shape", ()))
            if (str(item.get("dtype", "unknown")) != edge.dtype
                    or declared_shape != tuple(edge.shape)):
                raise ValueError("YOLO semantic tensor contract does not match graph")
        declared_dependencies = {
            (str(item.get("fromRole", "")), str(item.get("toRole", ""))):
            set(str(edge) for edge in item.get("tensorEdges", ()))
            for item in partition.get("dependencyEdges", ())
            if isinstance(item, Mapping)
        }
        if declared_dependencies != actual_dependencies:
            raise ValueError("YOLO semantic dependency graph does not match graph")
        declared_cuts = {
            (str(item.get("fromRole", "")), str(item.get("toRole", ""))):
            set(str(edge) for edge in item.get("boundaryTensors", ()))
            for item in partition.get("safeCuts", ())
            if isinstance(item, Mapping)
        }
        if declared_cuts != actual_dependencies:
            raise ValueError("YOLO semantic safe cuts do not match graph")
        metadata_nodes = self.graph_metadata.get("nodes")
        tensor_meta = self.graph_metadata.get("tensors", {})
        if (not isinstance(metadata_nodes, list) or not isinstance(tensor_meta, Mapping)
                or {str(item.get("name", "")) for item in metadata_nodes
                    if isinstance(item, Mapping)} != graph_names):
            raise ValueError("YOLO semantic graph metadata is not bound")
        metadata_inputs = {str(item) for item in self.graph_metadata.get("inputs", ())}
        metadata_outputs = {str(item) for item in self.graph_metadata.get("outputs", ())}
        metadata_producers: dict[str, str] = {}
        metadata_consumers: dict[str, list[str]] = {}
        for item in metadata_nodes:
            name = str(item["name"])
            for tensor in item.get("outputs", ()):
                metadata_producers[str(tensor)] = name
            for tensor in item.get("inputs", ()):
                metadata_consumers.setdefault(str(tensor), []).append(name)
        actual_role_inputs: dict[str, set[str]] = {role: set() for role in roles}
        actual_role_outputs: dict[str, set[str]] = {role: set() for role in roles}
        for item in metadata_nodes:
            name = str(item["name"])
            role = owner_by_name[name]
            for tensor in item.get("inputs", ()):
                tensor = str(tensor)
                producer = metadata_producers.get(tensor)
                if tensor in metadata_inputs or (
                        producer is not None and owner_by_name[producer] != role):
                    actual_role_inputs[role].add(tensor)
            for tensor in item.get("outputs", ()):
                tensor = str(tensor)
                consumers = metadata_consumers.get(tensor, ())
                if tensor in metadata_outputs or any(
                        owner_by_name[consumer] != role for consumer in consumers):
                    actual_role_outputs[role].add(tensor)

        def endpoint(tensor: str) -> tuple[str, str, tuple[Any, ...]]:
            info = tensor_meta.get(tensor, {})
            return (tensor, str(info.get("dtype", "unknown")),
                    tuple(info.get("shape", ())))

        declared_role_interfaces = partition.get("roleInterfaces")
        if not isinstance(declared_role_interfaces, Mapping):
            raise ValueError("YOLO semantic role interfaces are missing")
        for role in roles:
            declared = declared_role_interfaces.get(role)
            if not isinstance(declared, Mapping):
                raise ValueError("YOLO semantic role interface is invalid")
            declared_inputs = {
                (str(item.get("name", "")), str(item.get("dtype", "unknown")),
                 tuple(item.get("shape", ())))
                for item in declared.get("inputs", ()) if isinstance(item, Mapping)
            }
            declared_outputs = {
                (str(item.get("name", "")), str(item.get("dtype", "unknown")),
                 tuple(item.get("shape", ())))
                for item in declared.get("outputs", ()) if isinstance(item, Mapping)
            }
            if (declared_inputs != {endpoint(name) for name in actual_role_inputs[role]}
                    or declared_outputs != {endpoint(name) for name in actual_role_outputs[role]}):
                raise ValueError("YOLO semantic role interface does not match graph")
        return node_roles

    def _candidate(self, model: ModelDescriptor, graph: ModelGraphSnapshot,
                   registered: RegisteredYoloCandidate) -> SplitCandidate:
        node_roles = self._role_assignment(graph, registered)
        roles = tuple(str(item["role"]) for item in registered.roles)
        graph_edges = {edge.edge_id: edge for edge in graph.edges}
        crossed = tuple(
            edge.edge_id for edge in graph.edges
            if any(node_roles[edge.producer] != node_roles[consumer]
                   for consumer in edge.consumers)
        )
        dependencies = []
        for edge_id in crossed:
            edge = graph_edges[edge_id]
            producer = node_roles[edge.producer]
            consumer_roles = {node_roles[item] for item in edge.consumers}
            for consumer in sorted(consumer_roles):
                if consumer != producer:
                    dependencies.append(RoleDependency(producer, consumer, (edge_id,)))
        fragments = {
            role: canonical_contract_digest({"candidate": registered.candidate_digest,
                                             "graph": graph.graph_digest,
                                             "role": role,
                                             "nodes": [node for node, owner in node_roles.items() if owner == role]})
            for role in roles
        }
        requirements = {
            role: RoleResourceRequirement(
                backends=("onnxruntime-cpu", "onnxruntime-cuda"),
                weight_bytes=max(1, sum(edge.estimated_bytes or 0 for edge in graph.edges) // len(roles)),
                workspace_bytes=256 * 1024 * 1024,
                kv_bytes=0, activation_bytes=256 * 1024 * 1024,
                transient_bytes=64 * 1024 * 1024,
            ) for role in roles
        }
        candidate = SplitCandidate(
            source=SplitSource.PRE_SPLIT,
            splitter=self.splitter_descriptor,
            model=model, graph_digest=graph.graph_digest,
            execution_plan=RoleExecutionPlan(roles=roles,
                                              dependencies=tuple(dependencies),
                                              node_roles=node_roles),
            fragments_by_role=fragments,
            artifacts_by_role={role: (digest,) for role, digest in fragments.items()},
            requirements_by_role=requirements,
            cross_partition_tensors=crossed,
            estimated_costs={"role_count": len(roles),
                             "known_transfer_bytes": sum(graph_edges[e].estimated_bytes or 0 for e in crossed)},
            selection_priority=registered.priority,
            input_ingress_role=registered.input_ingress_role,
            result_egress_role=registered.result_egress_role,
            # The atomic candidate's FullModel already includes the complete
            # graph output. Only the explicit dependency-consumer Merge role
            # receives the native postprocessing contract.
            merge_kind=(registered.merge_kind if "Merge" in roles else ""),
            postprocessing=(self.postprocessing if "Merge" in roles else {}),
        )
        candidate.validate_against(graph)
        return candidate

    def enumerate_candidates(self, model: ModelDescriptor,
                             graph: ModelGraphSnapshot) -> tuple[SplitCandidate, ...]:
        model.validate_graph(graph)
        return tuple(self._candidate(model, graph, item) for item in self.registered)


class YoloCanonicalArtifactBinding:
    """Describe/ensure port that certifies Provider-local YOLO recipes.

    The local qualification slice binds every role to the same validated
    canonical package.  ``describe()`` returns the digest facts the
    coordinator seals into each ``RoleAssemblySpec`` recipe.  When a native
    encrypted publisher is configured, ``ensure()`` publishes the canonical
    graph, external initializer, and root in that order, then returns stable
    ARTIFACT identities separately from their request-scoped transport names.
    Every Provider assembles its certified subgraph from that root after
    Selection.
    """

    ASSEMBLER_DESCRIPTOR_DIGEST = (
        "sha256:" + hashlib.sha256(
            b"ndnsf-di-certified-onnx-assembler-v1").hexdigest())
    BACKEND_ABI = "onnxruntime-cpu-v1"

    def __init__(
        self,
        *,
        package_dir: str | Path,
        adapter: ModelFamilyAdapter,
        model: ModelDescriptor,
        graph: ModelGraphSnapshot,
        artifact_root: str,
        publish_encrypted_artifact: Callable[..., Mapping[str, Any]] | None = None,
    ) -> None:
        package = Path(package_dir)
        manifest_path = package / "manifest.json"
        if not manifest_path.is_file():
            raise ValueError("YOLO canonical manifest is missing")
        manifest_payload = manifest_path.read_bytes()
        manifest = json.loads(manifest_payload.decode("utf-8"))
        if manifest.get("schema") != "spec180-yolo26n-canonical-v1":
            raise ValueError("YOLO canonical manifest schema is unsupported")
        graph_info = manifest.get("graph")
        weights_info = manifest.get("weights")
        if not isinstance(graph_info, Mapping) or not isinstance(
                weights_info, Mapping):
            raise ValueError("YOLO canonical manifest identity is incomplete")
        if graph.graph_digest != model.graph_digest:
            raise ValueError("YOLO graph identity does not match the model")
        graph_path = package / "canonical" / "yolo26n.onnx"
        if not graph_path.is_file():
            raise ValueError("YOLO canonical graph file is missing")
        from ...adapters.onnx.graph import canonical_onnx_identity
        identity = canonical_onnx_identity(graph_path)
        if not str(artifact_root).startswith("/"):
            raise ValueError("YOLO artifact root must be an absolute NDN name")
        self._adapter = adapter
        self._graph = graph
        self._model = model
        self._artifact_root = str(artifact_root).rstrip("/")
        self._manifest_payload = manifest_payload
        self._graph_path = graph_path
        self._publish_encrypted_artifact = publish_encrypted_artifact
        self._published_root_fetch_name = ""
        self._binding = {
            "model_manifest_digest": "sha256:" + hashlib.sha256(
                manifest_payload).hexdigest(),
            "artifact_profile_digest": _digest({
                "graph": str(graph_info.get("graphDigest", "")),
                "weights": str(weights_info.get("digest", "")),
                "graphRevision": str(manifest.get("graphRevision", "")),
            }),
            # Planning-space graph-port digest (satisfies the certification
            # gate) vs the canonical ONNX identity the assembler verifies.
            "graph_digest": graph.graph_digest,
            "canonical_graph_digest": identity.graph_digest,
            "canonical_initializer_digest": (
                identity.normalized_initializer_content_digest),
            "adapter_descriptor_digest": adapter.descriptor.descriptor_digest,
            "assembler_descriptor_digest": self.ASSEMBLER_DESCRIPTOR_DIGEST,
            "backend_abi": self.BACKEND_ABI,
        }
        weights_path = package / str(weights_info.get("path", ""))
        if not weights_path.is_file():
            raise ValueError("YOLO canonical initializer file is missing")
        self._weights_path = weights_path
        # The certified-assembly source limit applies independently to the
        # canonical graph and separately addressed initializer object.  The
        # planning-time binding does not expose the post-publication NDN
        # initializer reference, so placement derives a separate finite
        # assembled-output limit from this per-object source bound.
        self._canonical_source_bytes = max(
            graph_path.stat().st_size,
            weights_path.stat().st_size if weights_path.is_file() else 0,
        )
        self._canonical_source_digest = "sha256:" + hashlib.sha256(
            graph_path.read_bytes()).hexdigest()

    @property
    def model_manifest_digest(self) -> str:
        """Read-only digest of the canonical model manifest this binding
        certifies (used by the protected grant view before certification)."""
        return str(self._binding.get("model_manifest_digest", ""))

    def describe(self, candidate: SplitCandidate | None = None):
        """Return the immutable digest facts for one candidate root."""
        if candidate is not None and candidate.graph_digest != (
                self._binding["graph_digest"]):
            raise ValueError("YOLO candidate graph does not match the root")
        from ...app_sdk.canonical_artifacts import CanonicalArtifactBinding
        return CanonicalArtifactBinding(
            model_manifest_digest=self._binding["model_manifest_digest"],
            artifact_profile_digest=self._binding["artifact_profile_digest"],
            graph_digest=self._binding["graph_digest"],
            canonical_initializer_digest=(
                self._binding["canonical_initializer_digest"]),
            adapter_descriptor_digest=self._binding["adapter_descriptor_digest"],
            assembler_descriptor_digest=self._binding["assembler_descriptor_digest"],
            backend_abi=self._binding["backend_abi"],
            canonical_source_bytes=self._canonical_source_bytes,
            canonical_graph_digest=self._binding["canonical_graph_digest"],
        )

    def ensure(self, candidate, role_specs, *, deadline_ms: int):
        """Confirm the expected per-role artifact digests against the ARTIFACT
        identities published by the controller's runtime batch."""
        from time import time
        if int(time() * 1000) >= int(deadline_ms):
            raise TimeoutError("YOLO canonical ensure deadline expired")
        from ...app_sdk.placement import PublishedSplit
        specs = tuple(role_specs)
        if not specs:
            raise ValueError("YOLO canonical ensure requires role specs")
        digests: dict[str, str] = {}
        names: dict[str, str] = {}
        for spec in specs:
            key = spec.role if sum(
                other.role == spec.role for other in specs) == 1 else (
                f"{spec.role}#{spec.rank}")
            expected = candidate.fragments_by_role.get(spec.role, "")
            if not expected or spec.artifact_digest != expected:
                raise ValueError(
                    f"YOLO role {spec.role} artifact digest is not certified")
            if key in digests:
                raise ValueError("YOLO canonical ensure role key is duplicated")
            digests[key] = expected
            names[key] = (
                f"{self._artifact_root}/{candidate.candidate_digest[7:]}/"
                f"{spec.role}")
        if self._publish_encrypted_artifact is None:
            return PublishedSplit(
                candidate_digest=candidate.candidate_digest,
                artifact_digests_by_role=digests,
                artifact_data_names_by_role=names,
            )

        def publish(payload: bytes, *, label: str, object_type: str) -> Mapping[str, Any]:
            if int(time() * 1000) >= int(deadline_ms):
                raise TimeoutError("YOLO canonical publication deadline expired")
            reference = self._publish_encrypted_artifact(
                payload, object_label=label, object_type=object_type)
            if not isinstance(reference, Mapping):
                raise TypeError("YOLO canonical publisher returned an invalid reference")
            data_name = str(reference.get("dataName", ""))
            content_digest = str(reference.get(
                "ciphertextDigest", reference.get("contentDigest", "")))
            expected_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
            if (not data_name.startswith("/")
                    or int(reference.get("plaintextSize", 0)) != len(payload)
                    or content_digest != expected_digest
                    or reference.get("encrypted") is not True):
                raise ValueError(
                    "YOLO canonical encrypted publication is not source-bound")
            return reference

        if not self._published_root_fetch_name:
            source_payload = self._graph_path.read_bytes()
            initializer_payload = self._weights_path.read_bytes()
            source = publish(
                source_payload,
                label="spec180-yolo-canonical-source",
                object_type="application/onnx",
            )
            initializer = publish(
                initializer_payload,
                label="spec180-yolo-canonical-initializer",
                object_type="application/octet-stream",
            )
            root = {
                "artifactProfileDigest": self._binding["artifact_profile_digest"],
                "metadata": {
                    "canonicalInitializerBytes": len(initializer_payload),
                    "canonicalInitializerDataName": str(initializer["dataName"]),
                    "canonicalInitializerObjectDigest": (
                        "sha256:" + hashlib.sha256(initializer_payload).hexdigest()),
                    "canonicalSourceBytes": len(source_payload),
                    "canonicalSourceDataName": str(source["dataName"]),
                    "canonicalSourceDigest": (
                        "sha256:" + hashlib.sha256(source_payload).hexdigest()),
                    "packageManifestDigest": (
                        "sha256:" + hashlib.sha256(
                            self._manifest_payload).hexdigest()),
                },
                "modelIdentityDigest": self._model.model_digest,
                "modelName": self._model.model_name,
                "schema": "ndnsf-di-canonical-model-manifest-v1",
                "state": "ACTIVE",
            }
            root_payload = json.dumps(
                root, sort_keys=True, separators=(",", ":"),
                ensure_ascii=False).encode("utf-8")
            root_reference = publish(
                root_payload,
                label="spec180-yolo-canonical-root",
                object_type="application/vnd.ndnsf.di.canonical-model-manifest-v1",
            )
            self._binding["model_manifest_digest"] = (
                "sha256:" + hashlib.sha256(root_payload).hexdigest())
            self._published_root_fetch_name = str(root_reference["dataName"])
        return PublishedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role=digests,
            artifact_data_names_by_role=names,
            artifact_fetch_data_names_by_role={
                key: self._published_root_fetch_name for key in digests
            },
        )


def build_yolo26n_adapter(package_dir: str | Path, *, require_signature: bool = True,
                          registry_path: str | Path | None = None) -> ModelFamilyAdapter:
    """Build the registered YOLO adapter from one canonical package."""

    package = Path(package_dir)
    manifest_path = package / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError("YOLO canonical manifest is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "spec180-yolo26n-canonical-v1":
        raise ValueError("YOLO canonical manifest schema is unsupported")
    if manifest.get("modelFamily") != "YOLO26n":
        raise ValueError("YOLO canonical manifest family is unsupported")
    if manifest.get("graphRevision") != "spec180-yolo26n-graph-v1":
        raise ValueError("YOLO canonical graph revision is unsupported")
    descriptor = _model_descriptor()
    catalogue = manifest.get("catalogue")
    if not isinstance(catalogue, Mapping):
        raise ValueError("YOLO canonical catalogue is missing")
    if require_signature:
        if registry_path is None:
            raise ValueError("registry_path is required for signed YOLO catalogue")
        verify_catalogue_signature(catalogue, registry_path)
    elif "signature" in catalogue and registry_path is not None:
        verify_catalogue_signature(catalogue, registry_path)
    registered = verify_catalogue(catalogue)
    graph_path = package / "canonical" / "yolo26n.onnx"
    graph_info = manifest.get("graph")
    if not isinstance(graph_info, Mapping) or not graph_path.is_file() or graph_info.get("graphDigest") != (
            "sha256:" + hashlib.sha256(graph_path.read_bytes()).hexdigest()):
        raise ValueError("YOLO canonical graph digest does not match manifest")
    source_info = manifest.get("source")
    expected_model_digest = (
        "sha256:" + str(source_info.get("checkpointSha256", ""))
        if isinstance(source_info, Mapping) else ""
    )
    if catalogue.get("modelDigest") != expected_model_digest:
        raise ValueError("YOLO catalogue model digest does not match manifest")
    if catalogue.get("graphDigest") != graph_info.get("graphDigest"):
        raise ValueError("YOLO catalogue graph digest does not match manifest")
    weights_info = manifest.get("weights")
    if not isinstance(weights_info, Mapping):
        raise ValueError("YOLO canonical weights manifest is missing")
    weights_path = package / str(weights_info.get("path", ""))
    if not weights_path.is_file() or ("sha256:" + hashlib.sha256(weights_path.read_bytes()).hexdigest()
                                      != weights_info.get("digest")):
        raise ValueError("YOLO canonical initializer file is missing or altered")
    graph = load_yolo_graph(graph_path, descriptor, manifest=manifest)
    splitter_port = AdapterPortDescriptor("yolo26n-splitter", "1", descriptor.split_schema_digest)
    splitter = Yolo26Splitter(
        splitter_port,
        SplitterDescriptor(splitter_port.name, splitter_port.version, splitter_port.state_digest),
        registered,
        tuple(str(item) for item in graph_info.get("nodeNames", ())),
        graph_info,
        manifest.get("postprocessing", {}),
    )
    task = JsonTaskAdapter(
        AdapterPortDescriptor("yolo26n-task", "1", descriptor.input_schema_digest),
        InferenceTaskDescriptor("object-detection", descriptor.input_schema_digest,
                                descriptor.options_schema_digest, descriptor.result_schema_digest),
    )
    state = StaticStateAdapter(
        AdapterPortDescriptor("yolo26n-state", "1", descriptor.state_schema_digest),
        (InferenceStateContract(
            profile="YOLO_STATELESS_V1", state_class=InferenceStateClass.STATELESS,
            identity_schema_digest=_digest("yolo-state-identity"),
            estimator_schema_digest=_digest("yolo-state-estimator"),
            allowed_tiers=("REQUEST_LOCAL",), owner_scope="requester-security-domain",
            role_scope="assigned-role", confidentiality="recipient-confidential",
            maximum_retention_ms=0, eviction_policy="terminal-destroy",
            boot_epoch_bound=False, cache_epoch_bound=False, pin_required_for_reuse=False,
            migration_supported=False, revalidation_rule="exact-digest-and-access-domain",
            cleanup_rule="release-request-scoped-state",
        ),),
    )
    runner = GuardedRunnerAdapter(
        AdapterPortDescriptor("yolo26n-runner", "1", descriptor.state_digest))
    return ModelFamilyAdapter(descriptor, graph, splitter, task, state, runner)
